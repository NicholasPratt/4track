#include "AudioEngine.h"

namespace
{
    constexpr juce::uint32 kPalette[kNumTracks] = {
        0xff4f9dff, 0xff34c79a, 0xfff2b84b, 0xffef6f6c, 0xffb084f5, 0xff4fd1e0, 0xfff58fc7, 0xffa3d65c
    };

    const juce::Identifier kMidiTrackProperty { "eightTrackMidi" };   // on a track's state: it's a MIDI track

    constexpr float kMinDb = -60.0f;   // the UI treats this as -inf
    constexpr float kOffDb = -100.0f;  // Tracktion's silence

    float toUiDb(float db) { return db <= kMinDb ? kMinDb : db; }
    float toEngineDb(float db) { return db <= kMinDb ? kOffDb : db; }
    float levelToGain(const te::DbTimePair& p) { return p.dB <= -99.0f ? 0.0f : juce::Decibels::decibelsToGain(p.dB); }

    bool differs(float a, float b) { return std::abs(a - b) > 1.0e-3f * juce::jmax(1.0f, std::abs(b)); }

    /** Keeps one value in step between the UI and the Edit. Whichever side changed since
        the last exchange wins: a UI change is pushed, otherwise an Edit change (undo,
        loading) is pulled. */
    template <typename T, typename Push>
    void twoWay(std::atomic<T>& ui, T& last, T engineValue, Push&& push)
    {
        const T uiValue = ui.load();
        if constexpr (std::is_floating_point_v<T>)
        {
            if (differs(uiValue, last)) { push(uiValue); last = uiValue; }
            else if (differs(engineValue, last)) { ui = engineValue; last = engineValue; }
        }
        else
        {
            if (uiValue != last) { push(uiValue); last = uiValue; }
            else if (engineValue != last) { ui = engineValue; last = engineValue; }
        }
    }

    template <typename PluginType>
    PluginType* insertPlugin(te::Edit& edit, te::PluginList& list, int index)
    {
        auto plugin = edit.getPluginCache().createNewPlugin(PluginType::xmlTypeName, {});
        list.insertPlugin(plugin, index, nullptr);
        return dynamic_cast<PluginType*>(plugin.get());
    }

    struct EngineBehaviour : te::EngineBehaviour
    {
        // A plugin that crashes while being scanned takes down a helper process, not 8track.
        bool canScanPluginsOutOfProcess() override { return true; }
    };
}

AudioEngine::AudioEngine(std::unique_ptr<te::UIBehaviour> ui)
    : engine("8track", ui != nullptr ? std::move(ui) : std::make_unique<te::UIBehaviour>(),
             std::make_unique<EngineBehaviour>())
{
    setUpInputLayout();
    getDeviceManager().addChangeListener(this);
    engine.getDeviceManager().addChangeListener(this);   // the input layout (stereo pairs) changed
    lastSampleRate = getSampleRate();

    for (int t = 0; t < kNumTracks; ++t)
    {
        tracks[(size_t) t].name = "Track " + juce::String(t + 1);
        tracks[(size_t) t].colour = kPalette[t];
    }

    startTimerHz(30);
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    getDeviceManager().removeChangeListener(this);
    engine.getDeviceManager().removeChangeListener(this);
    jassert(edit == nullptr);   // Project detaches its Edit before it is destroyed
}

double AudioEngine::getSampleRate() const
{
    const auto sr = engine.getDeviceManager().getSampleRate();
    return sr > 0.0 ? sr : 44100.0;
}

int AudioEngine::firstChannelOf(const te::WaveInputDevice& device)
{
    return (int) device.deviceDescription.getDeviceChannelRange().first;
}

te::WaveInputDevice* AudioEngine::inputForChannel(int channel) const
{
    // The enabled input that carries this channel: a mono input, or a pair containing it.
    for (auto* device : engine.getDeviceManager().getWaveInputDevices())
    {
        if (device->getDeviceType() != te::InputDevice::waveDevice || !device->isEnabled())
            continue;
        const auto range = device->deviceDescription.getDeviceChannelRange();
        if ((juce::uint32) channel >= range.first && (juce::uint32) channel < range.second)
            return device;
    }
    return nullptr;
}

std::vector<AudioEngine::Input> AudioEngine::getInputs() const
{
    std::vector<Input> inputs;
    for (auto* device : engine.getDeviceManager().getWaveInputDevices())
    {
        if (device->getDeviceType() != te::InputDevice::waveDevice || !device->isEnabled())
            continue;
        Input in;
        in.channel = firstChannelOf(*device);
        in.numChannels = (int) device->deviceDescription.getNumChannels();
        in.name = "In " + juce::String(in.channel + 1);
        for (int c = 1; c < in.numChannels; ++c)
            in.name << "/" << (in.channel + c + 1);
        in.label = device->getName();
        inputs.push_back(in);
    }
    std::sort(inputs.begin(), inputs.end(), [](auto& a, auto& b) { return a.channel < b.channel; });
    return inputs;
}

bool AudioEngine::isInputPaired(int firstChannel) const
{
    auto* device = inputForChannel(firstChannel);
    return device != nullptr && firstChannelOf(*device) == firstChannel && device->deviceDescription.getNumChannels() == 2;
}

bool AudioEngine::canPairInputs(int firstChannel) const
{
    if (firstChannel % 2 != 0)
        return false;
    if (isInputPaired(firstChannel))
        return true;
    auto* left = inputForChannel(firstChannel);
    auto* right = inputForChannel(firstChannel + 1);
    return left != nullptr && right != nullptr && left != right
        && left->deviceDescription.getNumChannels() == 1 && right->deviceDescription.getNumChannels() == 1;
}

void AudioEngine::setInputPaired(int firstChannel, bool stereo)
{
    if (!canPairInputs(firstChannel) || isInputPaired(firstChannel) == stereo)
        return;
    if (auto* device = inputForChannel(firstChannel))
        engine.getDeviceManager().setDeviceNumChannels(*device, stereo ? 2u : 1u);
    // Tracktion rebuilds its inputs and tells us through changeListenerCallback.
}

void AudioEngine::setUpInputLayout()
{
    // Tracktion's default groups inputs into stereo pairs. 8track starts each audio
    // interface as mono channels the first time it's used; after that, whatever pairs
    // the user sets up in Audio Settings are kept (Tracktion saves them per device).
    auto* device = getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr)
        return;
    auto& props = engine.getPropertyStorage().getPropertiesFile();
    auto seen = juce::StringArray::fromLines(props.getValue("8trackInputLayoutDevices"));
    seen.removeEmptyStrings();
    const auto key = device->getTypeName() + ":" + device->getName();
    if (seen.contains(key))
        return;
    seen.add(key);
    props.setValue("8trackInputLayoutDevices", seen.joinIntoString("\n"));
    engine.getDeviceManager().setAllWaveInputsToNumChannels(1);
}

void AudioEngine::normaliseInputChoices()
{
    // After pairing or unpairing, point each track at the first channel of the input that
    // now carries its old channel (a track on In 2 moves to In 1/2, and back to In 1).
    for (auto& p : tracks)
        if (auto* device = inputForChannel(p.inputChannel.load()))
        {
            p.inputChannel = firstChannelOf(*device);
            p.inputType = (int) (device->deviceDescription.getNumChannels() > 1 ? InputType::Stereo : InputType::Mono);
        }
}

te::AudioTrack* AudioEngine::getAudioTrack(int index) const
{
    if (edit == nullptr)
        return nullptr;
    return te::getAudioTracks(*edit)[index];
}

//==============================================================================
void AudioEngine::attachEdit(te::Edit* newEdit, bool freshEdit)
{
    detachMeters();
    edit = newEdit;
    for (auto& s : strips)
    {
        s.track = nullptr;
        s.lowCut = s.highCut = nullptr;
        s.eq = nullptr;
        s.volume = nullptr;
        s.send = nullptr;
    }
    reverb = nullptr;
    if (edit == nullptr)
        return;

    edit->playInStopEnabled = true;   // keeps monitoring and input meters running while stopped
    edit->getTransport().ensureContextAllocated();
    buildStrips();
    attachMeters();
    pullAll();
    if (freshEdit)
    {
        for (int t = 0; t < kNumTracks; ++t)
        {
            tracks[(size_t) t].name = "Track " + juce::String(t + 1);
            tracks[(size_t) t].colour = kPalette[t];
            tracks[(size_t) t].monitor = (int) MonitorMode::Off;
            tracks[(size_t) t].inputType = (int) InputType::Mono;
            tracks[(size_t) t].inputChannel = 0;
        }
        master.volumeDb = 0.0f;
        master.reverbRoom = 0.5f;
        master.reverbDamping = 0.5f;
        master.reverbWet = 0.33f;
        countInOn = true;
        punchOn = false;
        punchIn = 0;
        punchOut = 0;
    }
    inputsDirty = true;
    sync();
    edit->getUndoManager().clearUndoHistory();   // building the mixer isn't an undoable edit
}

void AudioEngine::buildStrips()
{
    edit->ensureNumberOfAudioTracks(kNumTracks + 1);
    auto audioTracks = te::getAudioTracks(*edit);

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = strips[(size_t) t];
        s.track = audioTracks[t];
        auto& list = s.track->pluginList;

        for (auto* p : list.getPlugins())
        {
            if (auto* lp = dynamic_cast<te::LowPassPlugin*>(p))
                (lp->isLowPass() ? s.highCut : s.lowCut) = lp;
            else if (auto* eq = dynamic_cast<te::EqualiserPlugin*>(p))
                s.eq = eq;
            else if (auto* send = dynamic_cast<te::AuxSendPlugin*>(p))
                s.send = send;
        }
        s.volume = s.track->getVolumePlugin();

        if (s.lowCut == nullptr)
        {
            s.lowCut = insertPlugin<te::LowPassPlugin>(*edit, list, list.indexOf(s.volume));
            s.lowCut->mode = "highpass";
            s.lowCut->frequency->setParameter(20.0f, juce::sendNotification);
            s.lowCut->setEnabled(false);
        }
        if (s.eq == nullptr)
        {
            s.eq = insertPlugin<te::EqualiserPlugin>(*edit, list, list.indexOf(s.volume));
            s.eq->setLowGain(0.0f);
            s.eq->setHighGain(0.0f);
            s.eq->setMidGain2(0.0f);
            s.eq->setMidGain1(0.0f);
            s.eq->setMidFreq1(1000.0f);
        }
        if (s.highCut == nullptr)
        {
            s.highCut = insertPlugin<te::LowPassPlugin>(*edit, list, list.indexOf(s.volume));
            s.highCut->mode = "lowpass";
            s.highCut->frequency->setParameter(20000.0f, juce::sendNotification);
            s.highCut->setEnabled(false);
        }
        if (s.send == nullptr)
        {
            s.send = insertPlugin<te::AuxSendPlugin>(*edit, list, list.indexOf(s.volume) + 1);
            s.send->busNumber = 0;
            s.send->setGainDb(kOffDb);
        }
    }

    // The reverb return: a track with no clips that the strips' sends feed.
    auto* ret = audioTracks[kReverbTrack];
    ret->setName("Reverb Return");
    te::AuxReturnPlugin* auxReturn = nullptr;
    for (auto* p : ret->pluginList.getPlugins())
    {
        if (auto* r = dynamic_cast<te::AuxReturnPlugin*>(p)) auxReturn = r;
        if (auto* r = dynamic_cast<te::ReverbPlugin*>(p))    reverb = r;
    }
    auto& retList = ret->pluginList;
    if (auxReturn == nullptr)
    {
        auxReturn = insertPlugin<te::AuxReturnPlugin>(*edit, retList, 0);
        auxReturn->busNumber = 0;
    }
    if (reverb == nullptr)
    {
        reverb = insertPlugin<te::ReverbPlugin>(*edit, retList, retList.indexOf(auxReturn) + 1);
        reverb->setDry(0.0f);
        reverb->setWet(0.33f);
    }
}

void AudioEngine::attachMeters()
{
    for (auto& s : strips)
        if (s.track != nullptr)
            if (auto* meter = s.track->getLevelMeterPlugin())
            {
                s.measurer = &meter->measurer;
                s.measurer->addClient(s.meter);
            }

    edit->getTransport().ensureContextAllocated();
    if (auto* context = edit->getCurrentPlaybackContext())
    {
        meteredContext = context;
        masterMeasurer = &context->masterLevels;
        masterMeasurer->addClient(masterMeter);
    }
}

void AudioEngine::detachMeters()
{
    for (auto& s : strips)
    {
        if (s.measurer != nullptr)
            s.measurer->removeClient(s.meter);
        s.measurer = nullptr;
    }
    // Only detach from the master meter if its playback context is still the Edit's.
    if (masterMeasurer != nullptr && edit != nullptr && edit->getCurrentPlaybackContext() == meteredContext)
        masterMeasurer->removeClient(masterMeter);
    masterMeasurer = nullptr;
    meteredContext = nullptr;

    // After a device change some of these inputs may have been deleted, so only detach
    // from the ones that still exist. Nothing refers to the clients of deleted inputs.
    const auto current = engine.getDeviceManager().getWaveInputDevices();
    for (auto& [device, client] : inputMeters)
        if (std::find(current.begin(), current.end(), device) != current.end())
            device->levelMeasurer.removeClient(*client);
    inputMeters.clear();
}

//==============================================================================
void AudioEngine::pullAll()
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = strips[(size_t) t];
        auto& p = tracks[(size_t) t];
        auto& last = synced[(size_t) t];

        p.name = last.name = s.track->getName().isNotEmpty() ? s.track->getName() : "Track " + juce::String(t + 1);
        const auto colour = s.track->getColour();
        p.colour = last.colour = colour.isTransparent() ? kPalette[t] : colour.getARGB();
        p.volumeDb = last.volumeDb = toUiDb(s.volume->getVolumeDb());
        p.pan = last.pan = s.volume->getPan();
        p.mute = last.mute = s.track->isMuted(false);
        p.midi = last.midi = (bool) s.track->state.getProperty(kMidiTrackProperty, false);
        p.solo = last.solo = s.track->isSolo(false);
        p.lowCut = last.lowCut = s.lowCut->isEnabled() ? s.lowCut->frequency->getCurrentValue() : 20.0f;
        p.highCut = last.highCut = s.highCut->isEnabled() ? s.highCut->frequency->getCurrentValue() : 20000.0f;
        p.midFreq = last.midFreq = s.eq->midFreq1->getCurrentValue();
        p.midGainDb = last.midGainDb = s.eq->midGain1->getCurrentValue();
        const float sendDb = s.send->getGainDb();
        p.send = last.send = sendDb <= kMinDb ? 0.0f : juce::Decibels::decibelsToGain(sendDb);
        p.armed = false;

        // The input this track records from, if the Edit already has one assigned.
        for (auto* instance : edit->getAllInputDevices())
            if (instance->getTargets().contains(s.track->itemID))
                if (auto* device = dynamic_cast<te::WaveInputDevice*>(&instance->getInputDevice()))
                {
                    p.inputType = (int) (device->deviceDescription.getNumChannels() > 1 ? InputType::Stereo : InputType::Mono);
                    p.inputChannel = firstChannelOf(*device);
                }
    }

    master.volumeDb = syncedMaster.volumeDb = toUiDb(edit->getMasterVolumePlugin()->getVolumeDb());
    master.reverbRoom = syncedMaster.room = reverb->roomSizeValue.get();
    master.reverbDamping = syncedMaster.damping = reverb->dampValue.get();
    master.reverbWet = syncedMaster.wet = reverb->wetValue.get();

    bpm = syncedBpm = edit->tempoSequence.getTempo(0)->getBpm();
    beatsPerBar = syncedBeatsPerBar = edit->tempoSequence.getTimeSig(0)->numerator.get();
    metronomeOn = syncedClick = edit->clickTrackEnabled.get();
    countInOn = edit->getCountInMode() != te::Edit::CountIn::none;
    punchOn = syncedPunchOn = edit->recordingPunchInOut.get();
    const auto loop = edit->getTransport().getLoopRange();
    punchIn = syncedPunchIn = toSamples(loop.getStart());
    punchOut = syncedPunchOut = toSamples(loop.getEnd());
}

void AudioEngine::sync()
{
    if (edit == nullptr)
        return;

    for (int t = 0; t < kNumTracks; ++t)
        syncTrack(t);
    syncMaster();
    syncTransportSettings();

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = tracks[(size_t) t];
        auto& last = syncedInputs[(size_t) t];
        const InputState now { p.armed.load(), p.monitor.load(), p.inputType.load(), p.inputChannel.load() };
        if (now.armed != last.armed || now.monitor != last.monitor || now.type != last.type || now.channel != last.channel)
        {
            last = now;
            inputsDirty = true;
        }
    }
    if (inputsDirty)
        applyInputs();
}

void AudioEngine::syncTrack(int t)
{
    auto& s = strips[(size_t) t];
    auto& p = tracks[(size_t) t];
    auto& last = synced[(size_t) t];

    twoWay(p.volumeDb, last.volumeDb, toUiDb(s.volume->getVolumeDb()), [&](float v) { s.volume->setVolumeDb(toEngineDb(v)); });
    twoWay(p.pan, last.pan, s.volume->getPan(), [&](float v) { s.volume->setPan(v); });
    twoWay(p.mute, last.mute, s.track->isMuted(false), [&](bool v) { s.track->setMute(v); });
    twoWay(p.solo, last.solo, s.track->isSolo(false), [&](bool v) { s.track->setSolo(v); });
    const bool wasMidi = last.midi;
    twoWay(p.midi, last.midi, (bool) s.track->state.getProperty(kMidiTrackProperty, false), [&](bool v)
    {
        s.track->state.setProperty(kMidiTrackProperty, v, &edit->getUndoManager());
    });
    if (wasMidi != last.midi)
        inputsDirty = true;

    // A filter only runs while it's switched on, so automating it switches it on.
    if (s.lowCut->frequency->hasAutomationPoints() && !s.lowCut->isEnabled())
        s.lowCut->setEnabled(true);
    if (s.highCut->frequency->hasAutomationPoints() && !s.highCut->isEnabled())
        s.highCut->setEnabled(true);

    const float lowCut = s.lowCut->isEnabled() ? s.lowCut->frequency->getCurrentValue() : 20.0f;
    twoWay(p.lowCut, last.lowCut, lowCut, [&](float v)
    {
        s.lowCut->setEnabled(v > 20.5f);
        s.lowCut->frequency->setParameter(v, juce::sendNotification);
    });
    const float highCut = s.highCut->isEnabled() ? s.highCut->frequency->getCurrentValue() : 20000.0f;
    twoWay(p.highCut, last.highCut, highCut, [&](float v)
    {
        s.highCut->setEnabled(v < 19999.0f);
        s.highCut->frequency->setParameter(v, juce::sendNotification);
    });
    twoWay(p.midFreq, last.midFreq, s.eq->midFreq1->getCurrentValue(), [&](float v) { s.eq->setMidFreq1(v); });
    twoWay(p.midGainDb, last.midGainDb, s.eq->midGain1->getCurrentValue(), [&](float v) { s.eq->setMidGain1(v); });

    const float sendDb = s.send->getGainDb();
    twoWay(p.send, last.send, sendDb <= kMinDb ? 0.0f : juce::Decibels::decibelsToGain(sendDb), [&](float v)
    {
        s.send->setGainDb(v <= 0.001f ? kOffDb : juce::Decibels::gainToDecibels(v));
    });

    // Name and colour are plain members, only ever changed by the UI.
    if (p.name != last.name)
    {
        s.track->setName(p.name);
        last.name = p.name;
    }
    if (p.colour != last.colour)
    {
        s.track->setColour(juce::Colour(p.colour));
        last.colour = p.colour;
    }
}

void AudioEngine::syncMaster()
{
    auto vol = edit->getMasterVolumePlugin();
    twoWay(master.volumeDb, syncedMaster.volumeDb, toUiDb(vol->getVolumeDb()), [&](float v) { vol->setVolumeDb(toEngineDb(v)); });
    twoWay(master.reverbRoom, syncedMaster.room, reverb->roomSizeValue.get(), [&](float v) { reverb->setRoomSize(v); });
    twoWay(master.reverbDamping, syncedMaster.damping, reverb->dampValue.get(), [&](float v) { reverb->setDamp(v); });
    twoWay(master.reverbWet, syncedMaster.wet, reverb->wetValue.get(), [&](float v) { reverb->setWet(v); });
}

void AudioEngine::syncTransportSettings()
{
    auto* tempo = edit->tempoSequence.getTempo(0);
    if (std::abs(bpm.load() - syncedBpm) > 1.0e-6)
    {
        syncedBpm = bpm.load();
        tempo->setBpm(syncedBpm);
    }
    else if (std::abs(tempo->getBpm() - syncedBpm) > 1.0e-6)
    {
        bpm = syncedBpm = tempo->getBpm();
    }

    auto* sig = edit->tempoSequence.getTimeSig(0);
    if (beatsPerBar.load() != syncedBeatsPerBar)
    {
        syncedBeatsPerBar = beatsPerBar.load();
        sig->numerator = syncedBeatsPerBar;
    }
    else if (sig->numerator.get() != syncedBeatsPerBar)
    {
        beatsPerBar = syncedBeatsPerBar = sig->numerator.get();
    }

    if (metronomeOn.load() != syncedClick)
    {
        syncedClick = metronomeOn.load();
        edit->clickTrackEnabled = syncedClick;
        edit->clickTrackEmphasiseBars = true;
    }
    if (punchOn.load() != syncedPunchOn)
    {
        syncedPunchOn = punchOn.load();
        edit->recordingPunchInOut = syncedPunchOn;
    }
    if (punchIn.load() != syncedPunchIn || punchOut.load() != syncedPunchOut)
    {
        syncedPunchIn = punchIn.load();
        syncedPunchOut = punchOut.load();
        if (syncedPunchOut > syncedPunchIn)
            edit->getTransport().setLoopRange({ toTime(syncedPunchIn), toTime(syncedPunchOut) });
    }
}

void AudioEngine::applyInputs()
{
    inputsDirty = false;
    edit->getTransport().ensureContextAllocated();

    for (auto* instance : edit->getAllInputDevices())
    {
        auto& device = instance->getInputDevice();
        const bool isWave = device.getDeviceType() == te::InputDevice::waveDevice;
        const bool isMidi = device.isMidi();
        if (!isWave && !isMidi)
            continue;

        juce::Array<int> wanted;
        bool anyOn = false, anyAuto = false;
        for (int t = 0; t < kNumTracks; ++t)
        {
            auto& p = tracks[(size_t) t];
            // MIDI tracks take every MIDI input; audio tracks take the input carrying their channel.
            const bool wants = p.midi.load()
                                   ? isMidi
                                   : isWave && p.inputType.load() != (int) InputType::Mix
                                         && inputForChannel(p.inputChannel.load()) == &device;
            if (!wants)
                continue;
            wanted.add(t);
            const auto mode = (MonitorMode) p.monitor.load();
            anyOn = anyOn || mode == MonitorMode::On;
            anyAuto = anyAuto || mode == MonitorMode::Auto;
        }

        for (auto id : instance->getTargets())
        {
            bool keep = false;
            for (int t : wanted)
                keep = keep || strips[(size_t) t].track->itemID == id;
            if (!keep)
                [[maybe_unused]] auto r = instance->removeTarget(id, nullptr);
        }

        for (int t : wanted)
        {
            const auto id = strips[(size_t) t].track->itemID;
            if (!instance->getTargets().contains(id))
                [[maybe_unused]] auto r = instance->setTarget(id, false, nullptr);
            instance->setRecordingEnabled(id, tracks[(size_t) t].armed.load());
        }

        device.setMonitorMode(anyOn ? te::InputDevice::MonitorMode::on
                              : anyAuto ? te::InputDevice::MonitorMode::automatic
                                        : te::InputDevice::MonitorMode::off);
    }
}

//==============================================================================
std::vector<te::Plugin*> AudioEngine::getInserts(int track) const
{
    std::vector<te::Plugin*> inserts;
    if (track == kMasterBus)
    {
        if (edit != nullptr)
            for (auto* p : edit->getMasterPluginList().getPlugins())
                if (p != edit->getMasterVolumePlugin().get() && dynamic_cast<te::LevelMeterPlugin*>(p) == nullptr)
                    inserts.push_back(p);
        return inserts;
    }
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return inserts;

    auto* instrument = getInstrument(track);
    for (auto* p : s.track->pluginList.getPlugins())
        if (p != s.lowCut && p != s.eq && p != s.highCut && p != s.volume && p != s.send && p != instrument
            && dynamic_cast<te::LevelMeterPlugin*>(p) == nullptr)
            inserts.push_back(p);
    return inserts;
}

te::Plugin::Ptr AudioEngine::loadPlugin(const juce::PluginDescription& description, juce::String& error)
{
    // Tracktion finds a plugin's binary through the scanned list, so make sure it's there.
    if (getKnownPlugins().getTypeForIdentifierString(description.createIdentifierString()) == nullptr)
        getKnownPlugins().addType(description);

    auto plugin = edit->getPluginCache().createNewPlugin(te::ExternalPlugin::xmlTypeName, description);
    auto* external = dynamic_cast<te::ExternalPlugin*>(plugin.get());
    if (external == nullptr || external->getAudioPluginInstance() == nullptr)
    {
        error = description.name + " couldn't be loaded";
        if (external != nullptr && external->getLoadError().isNotEmpty())
            error << ": " << external->getLoadError();
        return {};
    }
    return plugin;
}

te::Plugin* AudioEngine::getInstrument(int track) const
{
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return nullptr;
    for (auto* p : s.track->pluginList.getPlugins())
        if (auto* external = dynamic_cast<te::ExternalPlugin*>(p); external != nullptr && external->isSynth())
            return p;
    return nullptr;
}

te::Plugin* AudioEngine::setInstrument(int track, const juce::PluginDescription& description, juce::String& error)
{
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return nullptr;
    }
    auto plugin = loadPlugin(description, error);
    if (plugin == nullptr)
        return nullptr;

    edit->getUndoManager().beginNewTransaction("Set instrument " + description.name);
    if (auto* old = getInstrument(track))
        old->deleteFromParent();
    strips[(size_t) track].track->pluginList.insertPlugin(plugin, 0, nullptr);
    return plugin.get();
}

void AudioEngine::removeInstrument(int track)
{
    if (auto* instrument = getInstrument(track))
        removeInsert(*instrument);
}

std::vector<AudioEngine::AutomationTarget> AudioEngine::getAutomationTargets(int track) const
{
    std::vector<AutomationTarget> targets;
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return targets;

    auto add = [&targets](te::AutomatableParameter* param, const juce::String& name, const juce::String& group)
    {
        if (param == nullptr)
            return;
        targets.push_back({ param, name, group,
                            juce::String((juce::int64) param->getOwnerID().getRawID()) + "/" + param->paramID });
    };

    add(s.volume->volParam.get(), "Volume", "Mixer");
    add(s.volume->panParam.get(), "Pan", "Mixer");
    add(s.send->gain.get(), "Reverb Send", "Mixer");
    add(s.lowCut->frequency.get(), "Low Cut", "Mixer");
    add(s.highCut->frequency.get(), "High Cut", "Mixer");
    add(s.eq->midFreq1.get(), "EQ Freq", "Mixer");
    add(s.eq->midGain1.get(), "EQ Gain", "Mixer");

    std::vector<te::Plugin*> plugins;
    if (auto* instrument = getInstrument(track))
        plugins.push_back(instrument);
    for (auto* p : getInserts(track))
        plugins.push_back(p);
    for (auto* plugin : plugins)
        for (auto* param : plugin->getAutomatableParameters())
            add(param, param->getParameterName(), plugin->getName());
    return targets;
}

te::AutomatableParameter* AudioEngine::findAutomationTarget(int track, const juce::String& key) const
{
    if (key.isEmpty())
        return nullptr;
    for (auto& target : getAutomationTargets(track))
        if (target.key == key)
            return target.param;
    return nullptr;
}

juce::StringArray AudioEngine::getMidiInputNames() const
{
    juce::StringArray names;
    for (auto& device : engine.getDeviceManager().getMidiInDevices())
        if (device->isEnabled())
            names.add(device->getName());
    return names;
}

te::Plugin* AudioEngine::addInsert(int track, const juce::PluginDescription& description, juce::String& error)
{
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return nullptr;
    }
    if ((int) getInserts(track).size() >= kMaxInserts)
    {
        error = (track == kMasterBus ? "The master" : "A track") + juce::String(" can have up to ")
                + juce::String(kMaxInserts) + " plugins.";
        return nullptr;
    }

    auto plugin = loadPlugin(description, error);
    if (plugin == nullptr)
        return nullptr;

    edit->getUndoManager().beginNewTransaction("Add " + description.name);
    if (track == kMasterBus)
    {
        auto& list = edit->getMasterPluginList();
        list.insertPlugin(plugin, list.size(), nullptr);   // the master fader isn't in this list: it comes after
        return plugin.get();
    }
    auto& s = strips[(size_t) track];
    s.track->pluginList.insertPlugin(plugin, s.track->pluginList.indexOf(s.volume), nullptr);
    return plugin.get();
}

void AudioEngine::removeInsert(te::Plugin& plugin)
{
    if (edit == nullptr)
        return;
    edit->getUndoManager().beginNewTransaction("Remove " + plugin.getName());
    plugin.deleteFromParent();
}

//==============================================================================
void AudioEngine::drainMeters()
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = strips[(size_t) t];
        auto& p = tracks[(size_t) t];
        if (s.measurer == nullptr)
            continue;
        atomicMax(p.peakL, levelToGain(s.meter.getAndClearAudioLevel(0)));
        atomicMax(p.peakR, levelToGain(s.meter.getAndClearAudioLevel(1)));

        // An armed track shows its input level even when it isn't monitored.
        if (p.armed.load() && !p.midi.load() && p.inputType.load() != (int) InputType::Mix)
        {
            if (auto* device = inputForChannel(p.inputChannel.load()))
            {
                auto& client = inputMeters[device];
                if (client == nullptr)
                {
                    client = std::make_unique<te::LevelMeasurer::Client>();
                    device->levelMeasurer.addClient(*client);
                }
                const float left = levelToGain(client->getAndClearAudioLevel(0));
                const float right = device->deviceDescription.getNumChannels() > 1
                                        ? levelToGain(client->getAndClearAudioLevel(1)) : left;
                atomicMax(p.peakL, left);
                atomicMax(p.peakR, right);
            }
        }
    }

    if (masterMeasurer != nullptr)
    {
        atomicMax(master.peakL, levelToGain(masterMeter.getAndClearAudioLevel(0)));
        atomicMax(master.peakR, levelToGain(masterMeter.getAndClearAudioLevel(1)));
    }
}

void AudioEngine::timerCallback()
{
    if (edit == nullptr)
        return;
    if (edit->getCurrentPlaybackContext() != meteredContext)
    {
        // Tracktion made a new playback context (after rendering, or a device change).
        detachMeters();
        attachMeters();
        inputsDirty = true;
    }
    sync();
    drainMeters();
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The audio device or the input layout changed: inputs and the playback context may
    // be new objects.
    setUpInputLayout();
    normaliseInputChoices();
    if (edit != nullptr)
    {
        detachMeters();
        attachMeters();
        inputsDirty = true;
    }
    lastSampleRate = getSampleRate();
    sendChangeMessage();
}

//==============================================================================
AudioEngine::State AudioEngine::getState() const
{
    if (edit == nullptr)
        return State::Stopped;
    auto& transport = edit->getTransport();
    if (transport.isRecording())
        return State::Recording;
    return transport.isPlaying() ? State::Playing : State::Stopped;
}

juce::int64 AudioEngine::getPosition() const
{
    return edit == nullptr ? 0 : toSamples(edit->getTransport().getPosition());
}

void AudioEngine::setPosition(juce::int64 newPosition)
{
    if (edit != nullptr)
        edit->getTransport().setPosition(toTime(juce::jmax<juce::int64>(0, newPosition)));
}

void AudioEngine::play()
{
    if (edit == nullptr || getState() != State::Stopped)
        return;
    returnPosition = getPosition();
    edit->getTransport().play(false);
}

void AudioEngine::stop()
{
    if (edit == nullptr)
        return;
    const bool wasRolling = getState() != State::Stopped;
    edit->getTransport().stop(false, false);
    if (wasRolling)
        setPosition(returnPosition);
}

bool AudioEngine::record(const juce::File& takeFolder, juce::String& error)
{
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return false;
    }

    bool anyArmed = false;
    for (auto& p : tracks)
        anyArmed = anyArmed || p.armed.load();
    if (!anyArmed)
    {
        error = "Arm a track (R) before recording.";
        return false;
    }
    if (!takeFolder.createDirectory())
    {
        error = "Couldn't create " + takeFolder.getFullPathName();
        return false;
    }

    // Takes go in the project's Audio folder, and replace (non-destructively trim) whatever
    // they're recorded over, like the rest of 8track's editing.
    const auto replaceMode = te::WaveInputDevice::getMergeModes()[1];
    for (auto* device : engine.getDeviceManager().getWaveInputDevices())
    {
        device->setFilenameMask(takeFolder.getChildFile("%track%_%date%_%time%").getFullPathName());
        device->setMergeMode(replaceMode);
    }
    for (auto& midiIn : engine.getDeviceManager().getMidiInDevices())
    {
        midiIn->mergeRecordings = false;
        midiIn->replaceExistingClips = true;
    }
    sync();

    auto& transport = edit->getTransport();
    if (!transport.isPlaying())
    {
        returnPosition = getPosition();
        const bool punch = punchOn.load() && punchOut.load() > punchIn.load();
        if (punch)
            setPosition(punchIn.load() - getSamplesPerBar());   // one bar of pre-roll
        edit->setCountInMode(countInOn.load() && !punch ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none);
    }
    transport.record(false);
    return true;
}

std::vector<AudioEngine::LiveTake> AudioEngine::getLiveTakes() const
{
    std::vector<LiveTake> takes;
    if (edit == nullptr || !edit->getTransport().isRecording())
        return takes;

    for (auto* instance : edit->getAllInputDevices())
        for (int t = 0; t < kNumTracks; ++t)
        {
            const auto id = strips[(size_t) t].track->itemID;
            if (instance->isRecording(id) && instance->getInputDevice().getDeviceType() == te::InputDevice::waveDevice)
                takes.push_back({ t, instance->getRecordingFile(id), toSamples(instance->getPunchInTime(id)) });
        }
    return takes;
}

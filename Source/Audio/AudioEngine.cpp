#include "AudioEngine.h"
#include "RecordingThumbnail.h"
#include "Effects.h"
#include "Dynamics.h"
#include "DrumSampler.h"

// The direct output sits after both sends. Fading this path leaves the sends and meters intact.
class MainOutputGate final : public te::Plugin
{
public:
    static constexpr auto xmlTypeName = "8trackMainOutput";
    static const char* getPluginName() { return "Main Output"; }

    explicit MainOutputGate(te::PluginCreationInfo info)
        : Plugin(info), open(static_cast<bool>(state.getProperty(openProperty, true))) {}

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    bool canBeDisabled() override { return false; }
    bool takesMidiInput() override { return true; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    int getNumOutputChannelsGivenInputs(int channels) override { return channels; }

    bool isOpen() const { return open.load(std::memory_order_relaxed); }
    void setOpen(bool shouldOpen) { state.setProperty(openProperty, shouldOpen, getUndoManager()); }

    void initialise(const te::PluginInitialisationInfo& info) override
    {
        gain.reset(info.sampleRate, 0.005);
        gain.setCurrentAndTargetValue(isOpen() ? 1.0f : 0.0f);
    }

    void deinitialise() override {}

    void applyToBuffer(const te::PluginRenderContext& context) override
    {
        auto* buffer = context.destBuffer;
        if (buffer == nullptr)
            return;
        const bool shouldOpen = isOpen();
        gain.setTargetValue(shouldOpen ? 1.0f : 0.0f);
        if (!gain.isSmoothing())
        {
            if (!shouldOpen)
                buffer->clear(context.bufferStartSample, context.bufferNumSamples);
            return;
        }
        for (int i = 0; i < context.bufferNumSamples; ++i)
        {
            const auto level = gain.getNextValue();
            for (int channel = 0; channel < buffer->getNumChannels(); ++channel)
                buffer->getWritePointer(channel, context.bufferStartSample)[i] *= level;
        }
    }

private:
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree == state && property == openProperty)
            open.store(static_cast<bool>(state.getProperty(openProperty, true)), std::memory_order_relaxed);
        Plugin::valueTreePropertyChanged(tree, property);
    }

    static inline const juce::Identifier openProperty { "mainEnabled" };
    std::atomic<bool> open;
    juce::SmoothedValue<float> gain;
};

namespace
{
    constexpr juce::uint32 kPalette[kNumTracks] = {
        0xff4f9dff, 0xff34c79a, 0xfff2b84b, 0xffef6f6c, 0xffb084f5, 0xff4fd1e0, 0xfff58fc7, 0xffa3d65c
    };

    const juce::Identifier kRecordCountIn { "eightTrackRecordCountIn" };
    const juce::Identifier kPlaybackCountIn { "eightTrackPlaybackCountIn" };

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

namespace
{
    /** With no UI (tests), Tracktion's long tasks (rendering) just run to the end. */
    struct HeadlessBehaviour : AudioUIBehaviour
    {
        void runTaskWithProgressBar(te::ThreadPoolJobWithProgress& task) override
        {
            while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain) {}
        }
    };
}

AudioEngine::AudioEngine(std::unique_ptr<te::UIBehaviour> ui)
    : engine("8track", ui != nullptr ? std::move(ui) : std::make_unique<HeadlessBehaviour>(),
             std::make_unique<EngineBehaviour>())
{
    engine.getPluginManager().createBuiltInType<MidiOutPlugin>();   // before any Edit is loaded
    engine.getPluginManager().createBuiltInType<MainOutputGate>();
    EffectPlugin::registerTypes(engine.getPluginManager());
    setUpInputLayout();
    ensureTrackMidiInputs();
    // Tracktion's start-up asks for a quick MIDI scan and then replaces it with its regular
    // 4-second check, so MIDI devices would only appear after a few seconds. Scan now.
    engine.getDeviceManager().rescanMidiDeviceList();
    refreshMidiActivity();
    getDeviceManager().addChangeListener(this);
    engine.getDeviceManager().addChangeListener(this);   // the input layout (stereo pairs) changed
    lastSampleRate = getSampleRate();

    for (int t = 0; t < kNumTracks; ++t)
    {
        tracks[(size_t) t].name = "Track " + juce::String(t + 1);
        tracks[(size_t) t].colour = kPalette[t];
    }

    loopWatcher.tick = [this] { checkLoopWrap(); };
    startTimerHz(30);
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    loopWatcher.stopTimer();
    for (auto& a : midiActivity)
        if (auto device = a->device.lock())
            if (auto* physical = dynamic_cast<te::PhysicalMidiInputDevice*>(device.get()))
                physical->removeListener(a.get());
    midiActivity.clear();
    cancelPendingUpdate();
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

std::vector<AudioEngine::ArmedInputChannel> AudioEngine::getArmedInputChannels() const
{
    std::vector<ArmedInputChannel> result;
    for (const auto& track : tracks)
    {
        if (!track.armed.load() || track.midi.load() || track.inputType.load() == (int) InputType::Mix)
            continue;
        if (auto* input = inputForChannel(track.inputChannel.load()))
            for (const auto& channel : input->getChannels())
            {
                const int index = static_cast<int>(channel.indexInDevice);
                if (std::none_of(result.begin(), result.end(), [index](const auto& entry) { return entry.channel == index; }))
                    result.push_back({ index, track.name + "  ·  In " + juce::String(index + 1) });
            }
    }
    return result;
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
    chordPreview.stop();
    detachMeters();
    edit = newEdit;
    for (auto& s : strips)
    {
        s.track = nullptr;
        s.lowCut = s.highCut = nullptr;
        s.eq = nullptr;
        s.volume = nullptr;
        s.sends.fill(nullptr);
        s.mainGate = nullptr;
    }
    for (auto& bus : buses)
    {
        bus.track = nullptr;
        bus.volume = nullptr;
        bus.input = nullptr;
    }
    reverb = nullptr;
    if (edit == nullptr)
        return;

    edit->playInStopEnabled = true;   // keeps monitoring and input meters running while stopped
    edit->getTransport().addListener(this);   // the Edit's transport goes with the Edit, so it's never removed
    loopRecord = LoopRecord::Off;
    loopWatcher.stopTimer();
    edit->getTransport().ensureContextAllocated();
    buildStrips();
    attachMeters();
    pullAll();
    connectMidiOutPlugins();
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
        countInOn = true;
        playbackCountInOn = false;
        metronomeRecordOnly = false;
        punchOn = false;
        loopOn = false;
        punchIn = 0;
        punchOut = 0;
    }
    inputsDirty = true;
    sync();
    updateEffectTempo();
    edit->getUndoManager().clearUndoHistory();   // building the mixer isn't an undoable edit
}

void AudioEngine::buildStrips()
{
    edit->ensureNumberOfAudioTracks(kNumTracks + kStereoBusCount);
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
            {
                if (juce::isPositiveAndBelow(send->getBusNumber(), kStereoBusCount))
                    s.sends[(size_t) send->getBusNumber()] = send;
            }
            else if (auto* gate = dynamic_cast<MainOutputGate*>(p))
                s.mainGate = gate;
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
        for (int bus = 0; bus < kStereoBusCount; ++bus)
        {
            auto*& send = s.sends[(size_t) bus];
            if (send == nullptr)
            {
                const auto index = s.mainGate != nullptr ? list.indexOf(s.mainGate) : list.size();
                send = insertPlugin<te::AuxSendPlugin>(*edit, list, index);
                send->busNumber = bus;
                send->setGainDb(0.0f);
                send->setEnabled(false);
            }
        }
        if (s.mainGate == nullptr)
            s.mainGate = insertPlugin<MainOutputGate>(*edit, list, list.size());
    }

    for (int i = 0; i < kStereoBusCount; ++i)
    {
        auto& bus = buses[(size_t) i];
        bus.track = audioTracks[kFirstStereoBus + i];
        bus.volume = bus.track->getVolumePlugin();
        const auto name = "Bus " + juce::String(i + 1);
        bus.track->setName(name);
        edit->setAuxBusName(i, name);
        bus.track->setSoloIsolate(true);
        auto& list = bus.track->pluginList;
        for (auto* plugin : list.getPlugins())
        {
            if (auto* input = dynamic_cast<te::AuxReturnPlugin*>(plugin))
                bus.input = input;
            if (i == 0)
                if (auto* room = dynamic_cast<te::ReverbPlugin*>(plugin))
                    reverb = room;
        }
        if (bus.input == nullptr)
        {
            bus.input = insertPlugin<te::AuxReturnPlugin>(*edit, list, 0);
            bus.input->busNumber = i;
        }
        if (i == 0 && reverb == nullptr)
        {
            reverb = insertPlugin<te::ReverbPlugin>(*edit, list, list.indexOf(bus.input) + 1);
            reverb->setDry(0.0f);
            reverb->setWet(0.33f);
        }
    }
}

void AudioEngine::attachMeters()
{
    const auto attach = [](auto& strip)
    {
        if (strip.track != nullptr)
            if (auto* meter = strip.track->getLevelMeterPlugin())
            {
                strip.measurer = &meter->measurer;
                strip.measurer->addClient(strip.meter);
            }
    };
    for (auto& strip : strips) attach(strip);
    for (auto& bus : buses) attach(bus);

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
    const auto detach = [](auto& strip)
    {
        if (strip.measurer != nullptr)
            strip.measurer->removeClient(strip.meter);
        strip.measurer = nullptr;
    };
    for (auto& strip : strips) detach(strip);
    for (auto& bus : buses) detach(bus);
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
        for (size_t bus = 0; bus < s.sends.size(); ++bus)
            p.sendLevels[bus] = last.sendLevels[bus] = dbToGain(s.sends[bus]->getGainDb());
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

    master.volumeDb = syncedMasterVolumeDb = toUiDb(edit->getMasterVolumePlugin()->getVolumeDb());
    for (auto& bus : buses)
        bus.params.volumeDb = bus.syncedVolumeDb = toUiDb(bus.volume->getVolumeDb());
    busReverb.room = syncedReverb.room = reverb->roomSizeValue.get();
    busReverb.damping = syncedReverb.damping = reverb->dampValue.get();
    busReverb.wet = syncedReverb.wet = reverb->wetValue.get();

    bpm = syncedBpm = edit->tempoSequence.getTempo(0)->getBpm();
    beatsPerBar = syncedBeatsPerBar = edit->tempoSequence.getTimeSig(0)->numerator.get();
    metronomeOn = syncedClick = edit->clickTrackEnabled.get();
    countInOn = static_cast<bool>(edit->state.getProperty(kRecordCountIn, edit->getCountInMode() != te::Edit::CountIn::none));
    playbackCountInOn = static_cast<bool>(edit->state.getProperty(kPlaybackCountIn, false));
    metronomeRecordOnly = edit->clickTrackRecordingOnly.get();
    punchOn = syncedPunchOn = edit->recordingPunchInOut.get();
    loopOn = syncedLoopOn = edit->getTransport().looping.get() && !punchOn.load();
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

void AudioEngine::setTrackMidi(int index, bool midi)
{
    auto& params = getTrack(index);
    if (getState() == State::Recording || params.midi.load() == midi)
        return;
    if (!midi)
        removeInstrument(index);
    params.armed = false;
    params.midi = midi;
    params.monitor = (int) (midi ? MonitorMode::Auto : MonitorMode::Off);
    sync();
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

    for (size_t bus = 0; bus < s.sends.size(); ++bus)
        twoWay(p.sendLevels[bus], last.sendLevels[bus], dbToGain(s.sends[bus]->getGainDb()), [send = s.sends[bus]](float v)
        {
            send->setGainDb(v <= 0.001f ? kOffDb : juce::Decibels::gainToDecibels(v));
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
    twoWay(master.volumeDb, syncedMasterVolumeDb, toUiDb(vol->getVolumeDb()), [&](float v) { vol->setVolumeDb(toEngineDb(v)); });
    for (auto& bus : buses)
        twoWay(bus.params.volumeDb, bus.syncedVolumeDb, toUiDb(bus.volume->getVolumeDb()), [&](float v) { bus.volume->setVolumeDb(toEngineDb(v)); });
    twoWay(busReverb.room, syncedReverb.room, reverb->roomSizeValue.get(), [&](float v) { reverb->setRoomSize(v); });
    twoWay(busReverb.damping, syncedReverb.damping, reverb->dampValue.get(), [&](float v) { reverb->setDamp(v); });
    twoWay(busReverb.wet, syncedReverb.wet, reverb->wetValue.get(), [&](float v) { reverb->setWet(v); });
}

bool AudioEngine::hasTrackRoute(int track, Route route) const
{
    if (edit == nullptr || !juce::isPositiveAndBelow(track, kNumTracks))
        return false;
    const auto& strip = strips[(size_t) track];
    if (route == Route::Main)
        return strip.mainGate->isOpen();
    return strip.sends[(size_t) route - 1]->isEnabled();
}

void AudioEngine::setTrackRoute(int track, Route route, bool enabled)
{
    if (edit == nullptr || !juce::isPositiveAndBelow(track, kNumTracks)
        || getState() == State::Recording || hasTrackRoute(track, route) == enabled)
        return;

    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction("Change channel routing");
    auto& strip = strips[(size_t) track];
    if (route == Route::Main)
        strip.mainGate->setOpen(enabled);
    else
        strip.sends[(size_t) route - 1]->setEnabled(enabled);
    KeyedEffect::removeLoopingKeys(*edit);
    undo.beginNewTransaction();
}

bool AudioEngine::bus1FeedsBus2() const
{
    return edit != nullptr && buses[0].track->getOutput().getDestinationTrack() == buses[1].track;
}

void AudioEngine::setBus1FeedsBus2(bool toBus2)
{
    if (edit == nullptr || getState() == State::Recording || bus1FeedsBus2() == toBus2)
        return;
    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction("Route Bus 1");
    auto& output = buses[0].track->getOutput();
    if (toBus2)
        output.setOutputToTrack(buses[1].track);
    else
        output.setOutputToDefaultDevice(false);
    KeyedEffect::removeLoopingKeys(*edit);
    undo.beginNewTransaction();
}

bool AudioEngine::isBusReverbEnabled() const
{
    return reverb != nullptr && reverb->isEnabled();
}

void AudioEngine::setBusReverbEnabled(bool enabled)
{
    if (reverb == nullptr || isBusReverbEnabled() == enabled)
        return;
    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction(enabled ? "Enable Bus 1 reverb" : "Bypass Bus 1 reverb");
    reverb->setEnabled(enabled);
    undo.beginNewTransaction();
}

void AudioEngine::syncTransportSettings()
{
    auto* tempo = edit->tempoSequence.getTempo(0);
    if (std::abs(bpm.load() - syncedBpm) > 1.0e-6)
    {
        syncedBpm = bpm.load();
        tempo->setBpm(syncedBpm);
        updateEffectTempo();
    }
    else if (std::abs(tempo->getBpm() - syncedBpm) > 1.0e-6)
    {
        bpm = syncedBpm = tempo->getBpm();
        updateEffectTempo();
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

    if (!edit->state.hasProperty(kRecordCountIn) || static_cast<bool>(edit->state[kRecordCountIn]) != countInOn.load())
        edit->state.setProperty(kRecordCountIn, countInOn.load(), nullptr);
    if (static_cast<bool>(edit->state.getProperty(kPlaybackCountIn, false)) != playbackCountInOn.load())
        edit->state.setProperty(kPlaybackCountIn, playbackCountInOn.load(), nullptr);
    if (edit->clickTrackRecordingOnly.get() != metronomeRecordOnly.load())
        edit->clickTrackRecordingOnly = metronomeRecordOnly.load();

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
    auto& transport = edit->getTransport();
    if (loopOn.load() != syncedLoopOn)
    {
        syncedLoopOn = loopOn.load();
        transport.looping = syncedLoopOn && hasRange();
    }
    else if (transport.looping.get() != syncedLoopOn)
    {
        loopOn = syncedLoopOn = transport.looping.get();
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

    for (int t = 0; t < kNumTracks; ++t)
        if (tracks[(size_t) t].midi.load())
            configureTrackMidiInput(t);

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
            // A MIDI track takes its own virtual input (or, until Tracktion has made that, every
            // hardware MIDI input); an audio track takes the input carrying its channel.
            auto* own = p.midi.load() ? trackMidiInput(t) : nullptr;
            const bool wants = p.midi.load()
                                   ? (own != nullptr ? &device == own
                                                     : device.getDeviceType() == te::InputDevice::physicalMidiDevice)
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
    if (edit == nullptr)
        return inserts;
    if (track == kMasterBus)
    {
        for (auto* p : edit->getMasterPluginList().getPlugins())
            if (p != edit->getMasterVolumePlugin().get() && dynamic_cast<te::LevelMeterPlugin*>(p) == nullptr)
                inserts.push_back(p);
        return inserts;
    }
    if (juce::isPositiveAndBelow(track - kFirstStereoBus, kStereoBusCount))
    {
        const auto& bus = buses[(size_t) (track - kFirstStereoBus)];
        for (auto* p : bus.track->pluginList.getPlugins())
            if (p != bus.input && p != reverb && p != bus.volume && dynamic_cast<te::LevelMeterPlugin*>(p) == nullptr)
                inserts.push_back(p);
        return inserts;
    }
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return inserts;

    auto* instrument = getInstrument(track);
    for (auto* p : s.track->pluginList.getPlugins())
        if (p != s.lowCut && p != s.eq && p != s.highCut && p != s.volume && p != s.mainGate && p != instrument
            && std::find(s.sends.begin(), s.sends.end(), p) == s.sends.end()
            && dynamic_cast<te::LevelMeterPlugin*>(p) == nullptr && dynamic_cast<MidiOutPlugin*>(p) == nullptr)
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
    if (external == nullptr || (external->getAudioPluginInstance() == nullptr && !external->isInitialisingAsync()))
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
        if (p->isSynth())
            return p;
    return nullptr;
}

int AudioEngine::midiEffectsEnd(int track) const
{
    auto& list = strips[(size_t) track].track->pluginList;
    int i = 0;
    while (i < list.size())
        if (auto* e = dynamic_cast<EffectPlugin*>(list[i]); e != nullptr && e->getKind() == EffectPlugin::Kind::MidiEffect)
            ++i;
        else
            break;
    return i;
}

te::Plugin* AudioEngine::setInstrument(int track, const juce::String& instrumentType, juce::String& error)
{
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return nullptr;
    }
    auto type = EffectPlugin::findType(instrumentType);
    if (!type || type->kind != EffectPlugin::Kind::Instrument)
    {
        error = "8track doesn't have an instrument called " + instrumentType.quoted() + ".";
        return nullptr;
    }
    auto plugin = edit->getPluginCache().createNewPlugin(instrumentType, {});
    if (auto* effect = dynamic_cast<EffectPlugin*>(plugin.get()))
        effect->setTempo(bpm.load());
    if (auto* drums = dynamic_cast<DrumSampler*>(plugin.get()))
        drums->loadFactoryKit(DrumSampler::FactoryKit::acoustic);
    return placeInstrument(track, plugin);
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
    return placeInstrument(track, plugin);
}

te::Plugin* AudioEngine::placeInstrument(int track, te::Plugin::Ptr plugin)
{
    edit->getUndoManager().beginNewTransaction("Set instrument " + plugin->getName());
    if (auto* old = getInstrument(track))
        old->deleteFromParent();
    // After the MIDI effects and the MIDI Out plugin, if there is one: they see the notes first.
    auto& list = strips[(size_t) track].track->pluginList;
    auto* midiOut = getMidiOutPlugin(track);
    list.insertPlugin(plugin, midiOut != nullptr ? list.indexOf(midiOut) + 1 : midiEffectsEnd(track), nullptr);
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
    add(s.sends[0]->gain.get(), "Bus 1 Send", "Mixer");
    add(s.sends[1]->gain.get(), "Bus 2 Send", "Mixer");
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

//==============================================================================
// MIDI assign
namespace
{
    const juce::Identifier kAssignments { "EIGHTTRACK_MIDI_ASSIGN" }, kAssignment { "ASSIGN" },
                           kParamProp { "param" }, kChannelProp { "channel" }, kControllerProp { "cc" };
}

juce::String AudioEngine::parameterKey(te::AutomatableParameter& param)
{
    return juce::String((juce::int64) param.getOwnerID().getRawID()) + "/" + param.paramID;
}

te::AutomatableParameter* AudioEngine::findParameter(const juce::String& key) const
{
    if (edit == nullptr || key.isEmpty())
        return nullptr;
    for (int t = 0; t < kNumTracks; ++t)
        if (auto* param = findAutomationTarget(t, key))
            return param;
    for (auto* plugin : getInserts(kMasterBus))
        for (auto* param : plugin->getAutomatableParameters())
            if (parameterKey(*param) == key)
                return param;
    return nullptr;
}

std::vector<AudioEngine::MidiAssignment> AudioEngine::getMidiAssignments() const
{
    std::vector<MidiAssignment> out;
    if (edit == nullptr)
        return out;
    for (const auto& v : edit->state.getChildWithName(kAssignments))
        out.push_back({ v[kParamProp].toString(), juce::jlimit(1, 16, (int) v[kChannelProp]),
                        juce::jlimit(0, 127, (int) v[kControllerProp]) });
    return out;
}

std::optional<AudioEngine::MidiAssignment> AudioEngine::getMidiAssignment(const juce::String& key) const
{
    for (auto& a : getMidiAssignments())
        if (a.key == key)
            return a;
    return std::nullopt;
}

void AudioEngine::assignMidi(const juce::String& key, int channel, int controller)
{
    if (edit == nullptr || key.isEmpty())
        return;
    auto* um = &edit->getUndoManager();
    um->beginNewTransaction("MIDI assign");
    auto list = edit->state.getOrCreateChildWithName(kAssignments, um);
    // One controller per parameter, one parameter per controller.
    for (int i = list.getNumChildren(); --i >= 0;)
    {
        auto v = list.getChild(i);
        if (v[kParamProp].toString() == key || ((int) v[kChannelProp] == channel && (int) v[kControllerProp] == controller))
            list.removeChild(i, um);
    }
    juce::ValueTree v(kAssignment);
    v.setProperty(kParamProp, key, nullptr);
    v.setProperty(kChannelProp, channel, nullptr);
    v.setProperty(kControllerProp, controller, nullptr);
    list.appendChild(v, um);
    um->beginNewTransaction();
}

void AudioEngine::removeMidiAssignment(const juce::String& key)
{
    if (edit == nullptr)
        return;
    auto list = edit->state.getChildWithName(kAssignments);
    auto* um = &edit->getUndoManager();
    um->beginNewTransaction("Remove MIDI assign");
    for (int i = list.getNumChildren(); --i >= 0;)
        if (list.getChild(i)[kParamProp].toString() == key)
            list.removeChild(i, um);
    um->beginNewTransaction();
}

void AudioEngine::queueController(int channel, int controller, int value)
{
    if (controllerQueue.try_push(std::array<int, 3> { channel, controller, value }))
        triggerAsyncUpdate();
}

void AudioEngine::handleAsyncUpdate()
{
    std::array<int, 3> event;
    for (int i = 0; i < 4096 && controllerQueue.try_pop(event); ++i)
    {
        const auto [channel, controller, value] = event;
        if (onMidiLearn)
            onMidiLearn(channel, controller);
        else
            handleController(channel, controller, value);
    }
    if (!controllerQueue.empty())
        triggerAsyncUpdate();
}

void AudioEngine::handleController(int channel, int controller, int value)
{
    for (auto& a : getMidiAssignments())
        if (a.channel == channel && a.controller == controller)
            if (auto* param = findParameter(a.key))
                param->setNormalisedParameter(juce::jlimit(0.0f, 1.0f, (float) value / 127.0f), juce::sendNotification);
}

//==============================================================================
// MIDI devices
namespace
{
    const juce::Identifier kMidiInProperty { "eightTrackMidiIn" };
    const juce::Identifier kMidiChannelProperty { "eightTrackMidiChannel" };

    /** Each MIDI track records from its own virtual input, which picks the device and channel. */
    juce::String trackMidiInputName(int track) { return "8track Track " + juce::String(track + 1); }
}

std::vector<std::shared_ptr<te::MidiInputDevice>> AudioEngine::physicalMidiInputs() const
{
    std::vector<std::shared_ptr<te::MidiInputDevice>> inputs;
    for (auto& device : engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceType() == te::InputDevice::physicalMidiDevice)
            inputs.push_back(device);
    return inputs;
}

std::vector<AudioEngine::MidiInput> AudioEngine::getMidiInputs() const
{
    std::vector<MidiInput> inputs;
    for (auto& device : physicalMidiInputs())
    {
        MidiInput in { device->getName(), device->isEnabled(), 0.0 };
        for (auto& a : midiActivity)
            if (a->name == device->getName())
                in.lastActivityMs = a->lastMs.load();
        inputs.push_back(in);
    }
    return inputs;
}

void AudioEngine::setMidiInputEnabled(int inputIndex, bool enabled)
{
    const auto inputs = physicalMidiInputs();
    if (inputIndex >= 0 && inputIndex < (int) inputs.size())
        inputs[(size_t) inputIndex]->setEnabled(enabled);   // Tracktion rescans and tells us
}

void AudioEngine::refreshMidiActivity()
{
    std::vector<std::unique_ptr<MidiActivity>> next;
    for (auto& device : physicalMidiInputs())
    {
        auto* physical = dynamic_cast<te::PhysicalMidiInputDevice*>(device.get());
        if (physical == nullptr)
            continue;
        auto a = std::make_unique<MidiActivity>();
        a->owner = this;
        a->device = device;
        a->name = device->getName();
        for (auto& old : midiActivity)
            if (old->name == a->name)
                a->lastMs = old->lastMs.load();
        physical->addListener(a.get());
        next.push_back(std::move(a));
    }
    for (auto& old : midiActivity)
        if (auto device = old->device.lock())
            if (auto* physical = dynamic_cast<te::PhysicalMidiInputDevice*>(device.get()))
                physical->removeListener(old.get());
    midiActivity = std::move(next);
}

te::MidiOutputDevice* AudioEngine::midiOutput(int index) const
{
    return engine.getDeviceManager().getMidiOutDevice(index);
}

std::vector<AudioEngine::MidiOutput> AudioEngine::getMidiOutputs()
{
    std::vector<MidiOutput> outputs;
    auto& dm = engine.getDeviceManager();
    for (int i = 0; i < dm.getNumMidiOutDevices(); ++i)
        if (auto* d = dm.getMidiOutDevice(i))
            outputs.push_back({ d->getName(), d->isEnabled(), d->isEnabled() && d->isSendingClock(),
                                d->isSendingMMC(), d->isSendingTimecode(), d->getPreDelayMs() });
    return outputs;
}

void AudioEngine::setMidiOutputEnabled(int outputIndex, bool enabled)
{
    if (auto* d = midiOutput(outputIndex))
    {
        d->setEnabled(enabled);   // Tracktion rescans its MIDI devices and reallocates playback
        if (edit != nullptr)
            edit->restartPlayback();
    }
}

void AudioEngine::setSendingMidiClock(int outputIndex, bool shouldSend)
{
    auto* device = midiOutput(outputIndex);
    if (device == nullptr)
        return;
    if (shouldSend && !device->isEnabled())
        device->setEnabled(true);
    device->setSendingClock(shouldSend);
    // The playback graph only includes MIDI outputs that are in use, so rebuild it.
    if (edit != nullptr)
        edit->restartPlayback();
}

void AudioEngine::setSendingMMC(int outputIndex, bool shouldSend)
{
    if (auto* d = midiOutput(outputIndex))
    {
        if (shouldSend && !d->isEnabled())
            d->setEnabled(true);
        d->setSendingMMC(shouldSend);
    }
}

void AudioEngine::setSendingTimecode(int outputIndex, bool shouldSend)
{
    if (auto* d = midiOutput(outputIndex); d != nullptr && d->isSendingTimecode() != shouldSend)
    {
        if (shouldSend && !d->isEnabled())
            d->setEnabled(true);
        d->flipSendingTimecode();
        if (edit != nullptr)
            edit->restartPlayback();
    }
}

void AudioEngine::setMidiOutputDelayMs(int outputIndex, int ms)
{
    if (auto* d = midiOutput(outputIndex))
    {
        d->setPreDelayMs(ms);
        connectMidiOutPlugins();   // the notes tracks send there take the same delay
    }
}

//==============================================================================
// MIDI tracks' routing
te::MidiInputDevice* AudioEngine::trackMidiInput(int track) const
{
    const auto name = trackMidiInputName(track);
    for (auto& device : engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceType() == te::InputDevice::virtualMidiDevice && device->getName() == name)
            return device.get();
    return nullptr;
}

void AudioEngine::ensureTrackMidiInputs()
{
    // Created once per machine (Tracktion keeps them); they appear after its next device scan.
    for (int t = 0; t < kNumTracks; ++t)
        if (trackMidiInput(t) == nullptr)
            [[maybe_unused]] auto r = engine.getDeviceManager().createVirtualMidiDevice(trackMidiInputName(t));
}

void AudioEngine::configureTrackMidiInput(int track)
{
    auto* input = dynamic_cast<te::VirtualMidiInputDevice*>(trackMidiInput(track));
    if (input == nullptr)
        return;
    if (!input->isEnabled())
        input->setEnabled(true);

    const auto routing = getMidiRouting(track);
    juce::StringArray sources;
    for (auto& device : physicalMidiInputs())
        if (routing.input.isEmpty() || device->getName() == routing.input)
            sources.add(device->getDeviceID());
    if (input->getMIDIInputSourceDevices() != sources)
        input->setMIDIInputSourceDevices(sources);
    for (int ch = 1; ch <= 16; ++ch)
        input->setChannelAllowed(ch, routing.channel == 0 || routing.channel == ch);
}

MidiOutPlugin* AudioEngine::getMidiOutPlugin(int track) const
{
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return nullptr;
    for (auto* p : s.track->pluginList.getPlugins())
        if (auto* out = dynamic_cast<MidiOutPlugin*>(p))
            return out;
    return nullptr;
}

AudioEngine::MidiRouting AudioEngine::getMidiRouting(int track) const
{
    MidiRouting r;
    auto& s = strips[(size_t) track];
    if (s.track == nullptr)
        return r;
    r.input = s.track->state.getProperty(kMidiInProperty).toString();
    r.channel = juce::jlimit(0, 16, (int) s.track->state.getProperty(kMidiChannelProperty, 0));
    if (auto* out = getMidiOutPlugin(track))
    {
        r.output = out->deviceName.get();
        r.replaceInstrument = out->replaceInstrument.get();
    }
    return r;
}

void AudioEngine::setMidiRouting(int track, const MidiRouting& r)
{
    auto& s = strips[(size_t) track];
    if (edit == nullptr || s.track == nullptr || getMidiRouting(track) == r)
        return;

    auto* um = &edit->getUndoManager();
    um->beginNewTransaction("MIDI routing");
    s.track->state.setProperty(kMidiInProperty, r.input, um);
    s.track->state.setProperty(kMidiChannelProperty, juce::jlimit(0, 16, r.channel), um);

    auto* out = getMidiOutPlugin(track);
    if (r.output.isEmpty())
    {
        if (out != nullptr)
            out->deleteFromParent();
    }
    else
    {
        if (out == nullptr)
        {
            // After the MIDI effects and before the instrument, so it sees the track's notes
            // as the instrument will play them.
            auto plugin = edit->getPluginCache().createNewPlugin(MidiOutPlugin::create());
            s.track->pluginList.insertPlugin(plugin, midiEffectsEnd(track), nullptr);
            out = dynamic_cast<MidiOutPlugin*>(plugin.get());
        }
        if (out != nullptr)
        {
            out->deviceName = r.output;
            out->replaceInstrument = r.replaceInstrument;
        }
    }
    um->beginNewTransaction();

    connectMidiOutPlugins();
    applyInputs();
}

void AudioEngine::connectMidiOutPlugins()
{
    updateMidiOutLatency();
    if (edit == nullptr)
        return;
    const auto available = juce::MidiOutput::getAvailableDevices();
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto* out = getMidiOutPlugin(t);
        if (out == nullptr)
            continue;
        int port = -1;
        for (auto& info : available)
            if (info.name == out->deviceName.get())
                port = midiOutSender.openPort(info.identifier);

        // The port's delay setting (also used for its clock) applies to the notes too.
        auto& dm = engine.getDeviceManager();
        for (int i = 0; i < dm.getNumMidiOutDevices(); ++i)
            if (auto* d = dm.getMidiOutDevice(i); d != nullptr && d->getName() == out->deviceName.get())
                midiOutSender.setPortDelayMs(port, d->getPreDelayMs());
        out->connect(midiOutSender, port);
    }
}

void AudioEngine::updateMidiOutLatency()
{
    // CoreAudio's reported output latency already includes its I/O buffer.
    if (auto* device = getDeviceManager().getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();
        if (rate > 0)
            midiOutSender.setOutputLatencyMs(1000.0 * device->getOutputLatencyInSamples() / rate);
    }
}

juce::StringArray AudioEngine::getMidiInputNames() const
{
    juce::StringArray names;
    for (auto& device : engine.getDeviceManager().getMidiInDevices())
        if (device->isEnabled())
            names.add(device->getName());
    return names;
}

bool AudioEngine::canAddInsert(int track, juce::String& error) const
{
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return false;
    }
    if ((int) getInserts(track).size() >= kMaxInserts)
    {
        error = (track == kMasterBus ? "The master" : "A track") + juce::String(" can have up to ")
                + juce::String(kMaxInserts) + " plugins.";
        return false;
    }
    return true;
}

te::Plugin* AudioEngine::addInsert(int track, const juce::PluginDescription& description, juce::String& error)
{
    if (!canAddInsert(track, error))
        return nullptr;
    auto plugin = loadPlugin(description, error);
    if (plugin == nullptr)
        return nullptr;
    return placeInsert(track, plugin);
}

te::Plugin* AudioEngine::addEffect(int track, const juce::String& effectType, juce::String& error)
{
    if (!canAddInsert(track, error))
        return nullptr;
    const auto type = EffectPlugin::findType(effectType);
    if (!type || type->kind == EffectPlugin::Kind::Instrument)
    {
        error = "8track doesn't have an effect called " + effectType.quoted() + ".";
        return nullptr;
    }
    const bool midiEffect = type->kind == EffectPlugin::Kind::MidiEffect;
    if (midiEffect && (!juce::isPositiveAndBelow(track, kNumTracks) || !tracks[(size_t) track].midi.load()))
    {
        error = type->name + " works on MIDI tracks.";
        return nullptr;
    }
    auto plugin = edit->getPluginCache().createNewPlugin(effectType, {});
    auto* effect = dynamic_cast<EffectPlugin*>(plugin.get());
    if (effect == nullptr)
    {
        error = type->name + " couldn't be created.";
        return nullptr;
    }
    effect->setTempo(bpm.load());
    if (midiEffect)
    {
        // First in the chain, after any other MIDI effects: before MIDI Out and the instrument.
        edit->getUndoManager().beginNewTransaction("Add " + plugin->getName());
        strips[(size_t) track].track->pluginList.insertPlugin(plugin, midiEffectsEnd(track), nullptr);
        return plugin.get();
    }
    return placeInsert(track, plugin);
}

void AudioEngine::updateEffectTempo()
{
    if (edit == nullptr)
        return;
    for (auto* p : te::getAllPlugins(*edit, false))
        if (auto* effect = dynamic_cast<EffectPlugin*>(p))
            effect->setTempo(syncedBpm);
}

te::Plugin* AudioEngine::placeInsert(int track, te::Plugin::Ptr plugin)
{
    edit->getUndoManager().beginNewTransaction("Add " + plugin->getName());
    if (track == kMasterBus)
    {
        auto& list = edit->getMasterPluginList();
        list.insertPlugin(plugin, list.size(), nullptr);   // the master fader isn't in this list: it comes after
        return plugin.get();
    }
    if (juce::isPositiveAndBelow(track - kFirstStereoBus, kStereoBusCount))
    {
        auto& bus = buses[(size_t) (track - kFirstStereoBus)];
        bus.track->pluginList.insertPlugin(plugin, bus.track->pluginList.indexOf(bus.volume), nullptr);
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
    for (auto& bus : buses)
        if (bus.measurer != nullptr)
        {
            atomicMax(bus.params.peakL, levelToGain(bus.meter.getAndClearAudioLevel(0)));
            atomicMax(bus.params.peakR, levelToGain(bus.meter.getAndClearAudioLevel(1)));
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

    // The looper ends when the transport stops, however that happens.
    if (loopRecord != LoopRecord::Off && !edit->getTransport().isPlaying())
    {
        loopRecord = LoopRecord::Off;
        loopWatcher.stopTimer();
    }
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The audio device or the input layout changed: inputs and the playback context may
    // be new objects.
    setUpInputLayout();
    normaliseInputChoices();
    ensureTrackMidiInputs();   // MIDI devices were plugged in, unplugged, or switched on or off
    refreshMidiActivity();
    if (edit != nullptr)
    {
        detachMeters();
        attachMeters();
        connectMidiOutPlugins();
        inputsDirty = true;
    }
    const auto rate = getSampleRate();
    if (lastSampleRate > 0 && !juce::approximatelyEqual(rate, lastSampleRate))
    {
        auto rescale = [&](juce::int64 samples)
        {
            return (juce::int64) std::llround((double) samples * rate / lastSampleRate);
        };
        punchIn = rescale(punchIn.load());
        punchOut = rescale(punchOut.load());
        syncedPunchIn = rescale(syncedPunchIn);
        syncedPunchOut = rescale(syncedPunchOut);
        returnPosition = rescale(returnPosition);
        armedLoopOffset = rescale(armedLoopOffset);
    }
    lastSampleRate = rate;
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

void AudioEngine::setRangeMode(RangeMode mode)
{
    punchOn = mode == RangeMode::Punch;
    loopOn = mode == RangeMode::Loop;
}

void AudioEngine::play()
{
    if (edit == nullptr || getState() != State::Stopped)
        return;
    syncTransportSettings();
    returnPosition = getPosition();
    if (loopOn.load() && hasRange() && (getPosition() < punchIn.load() || getPosition() >= punchOut.load()))
        setPosition(punchIn.load());   // looping starts at the loop
    edit->getTransport().play(false, playbackCountInOn.load() ? beatsPerBar.load() : 0);
}

void AudioEngine::stop()
{
    if (edit == nullptr)
        return;
    const bool wasRolling = getState() != State::Stopped;
    loopRecord = LoopRecord::Off;
    loopWatcher.stopTimer();
    edit->getTransport().stop(false, false);
    if (wasRolling)
        setPosition(returnPosition);
}

bool AudioEngine::record(const juce::File& takeFolder, juce::String& error)
{
    chordPreview.stop();
    if (edit == nullptr)
    {
        error = "There's no project open.";
        return false;
    }

    auto& transport = edit->getTransport();
    if (auto* context = transport.getCurrentPlaybackContext())
        if (transport.isPlaying() && context->getUnloopedPosition().inSeconds() < transport.getPlaybackCountInEndSeconds())
        {
            error = "Wait for the playback count-in to finish before recording.";
            return false;
        }
    const bool looper = loopOn.load() && hasRange();

    // A looper that's already going: record toggles finishing at the end of this pass,
    // and cancels a start that hasn't happened yet.
    switch (loopRecord)
    {
        case LoopRecord::Recording: loopRecord = LoopRecord::Finishing; armedLoopOffset = loopPassOffset(); loopWatcher.startTimer(1); return true;
        case LoopRecord::Finishing: loopRecord = LoopRecord::Recording; loopWatcher.stopTimer(); return true;
        case LoopRecord::Waiting:   loopRecord = LoopRecord::Off; loopWatcher.stopTimer(); return true;
        case LoopRecord::Off:       break;
    }
    if (looper && toTime(punchOut.load() - punchIn.load()).inSeconds() < kMinLoopSeconds)
    {
        error = "To record in a loop, make the loop at least " + juce::String(kMinLoopSeconds, 0) + " seconds long.";
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
        // Looping MIDI overdubs: each pass is merged into what's there.
        midiIn->mergeRecordings = looper;
        midiIn->replaceExistingClips = !looper;
    }
    sync();

    if (looper)
    {
        if (transport.isPlaying())
        {
            // Wait for the next pass, so the takes line up with the loop.
            loopRecord = LoopRecord::Waiting;
            armedLoopOffset = loopPassOffset();
            loopWatcher.startTimer(1);
            return true;
        }
        returnPosition = getPosition();
        setPosition(punchIn.load());
        edit->setCountInMode(countInOn.load() ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none);
        transport.record(false);
        loopRecord = LoopRecord::Recording;
        return true;
    }

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

juce::int64 AudioEngine::loopPassOffset() const
{
    if (auto* context = edit->getCurrentPlaybackContext())
        if (auto point = context->getSyncPoint())
            return toSamples(point->unloopedTime - point->time);
    return 0;
}

void AudioEngine::checkLoopWrap()
{
    if (edit == nullptr || loopRecord == LoopRecord::Off || loopRecord == LoopRecord::Recording)
    {
        loopWatcher.stopTimer();
        return;
    }
    auto& transport = edit->getTransport();
    if (!transport.isPlaying())
    {
        loopRecord = LoopRecord::Off;
        loopWatcher.stopTimer();
        return;
    }

    // Both positions come from one audio block. A busy message thread can
    // observe the crossing even after playback has passed its previous position.
    if (loopPassOffset() <= armedLoopOffset)
        return;

    loopWatcher.stopTimer();
    if (loopRecord == LoopRecord::Waiting)
    {
        transport.record(false);
        loopRecord = LoopRecord::Recording;
    }
    else
    {
        // A few milliseconds of the next pass get recorded too: Tracktion drops a take that
        // short, and ends the clip at the loop end.
        transport.stopRecording(false);
        loopRecord = LoopRecord::Off;
    }
}

void AudioEngine::recordingFinished(te::InputDeviceInstance&, te::EditItemID, const juce::ReferenceCountedArray<te::Clip>& clips)
{
    for (auto& clip : clips)
        if (auto* wave = dynamic_cast<te::WaveAudioClip*>(clip))
            keepLastFullPass(*wave);
}

void AudioEngine::storeTakes(te::WaveAudioClip& clip, const juce::Array<juce::File>& passes)
{
    if (passes.size() < 2)
        return;
    juce::ValueTree alternatives(takeListID);
    for (const auto& file : passes)
    {
        juce::ValueTree take(te::IDs::TAKE);
        te::SourceFileReference(*edit, take, takePathID).setToFile(file, te::SourceFileReference::PathStyle::chooseBest, false);
        alternatives.appendChild(take, nullptr);
    }
    clip.state.appendChild(alternatives, &edit->getUndoManager());
}

void AudioEngine::keepLastFullPass(te::WaveAudioClip& clip)
{
    auto takes = clip.state.getChildWithName(te::IDs::TAKES);
    if (!takes.isValid() || edit == nullptr)
        return;

    // Tracktion lists the first pass, the later passes in order, then the first pass again.
    juce::Array<juce::File> passes;
    for (auto take : takes)
    {
        const auto file = te::SourceFileReference(*edit, take, te::IDs::source).getFile();
        if (file.existsAsFile())
            passes.addIfNotAlreadyThere(file);
    }

    const double loopLength = edit->getTransport().getLoopRange().getLength().inSeconds();
    juce::File keep = passes.isEmpty() ? juce::File() : passes.getFirst();
    for (auto& file : passes)
    {
        const te::AudioFile recording(engine, file);
        const double rate = recording.getSampleRate();
        // Rounding the two loop boundaries to file samples can cost one frame.
        if (rate > 0 && recording.getLengthInSamples() + 1 >= std::llround(loopLength * rate))
            keep = file;
    }

    auto* um = &edit->getUndoManager();
    storeTakes(clip, passes);
    clip.state.removeChild(takes, um);
    if (keep != juce::File() && keep != clip.getSourceFileReference().getFile())
        clip.getSourceFileReference().setToFile(keep, te::SourceFileReference::PathStyle::chooseBest, false);
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

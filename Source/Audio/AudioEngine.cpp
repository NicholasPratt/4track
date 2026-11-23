#include "AudioEngine.h"

namespace
{
    constexpr juce::uint32 kPalette[kNumTracks] = {
        0xff4f9dff, 0xff34c79a, 0xfff2b84b, 0xffef6f6c, 0xffb084f5, 0xff4fd1e0, 0xfff58fc7, 0xffa3d65c
    };

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
}

AudioEngine::AudioEngine()
{
    // One input per hardware channel: each track picks the channel it records.
    engine.getDeviceManager().setAllWaveInputsToNumChannels(1);
    getDeviceManager().addChangeListener(this);
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
    jassert(edit == nullptr);   // Project detaches its Edit before it is destroyed
}

double AudioEngine::getSampleRate() const
{
    const auto sr = engine.getDeviceManager().getSampleRate();
    return sr > 0.0 ? sr : 44100.0;
}

juce::StringArray AudioEngine::getActiveInputNames() const
{
    juce::StringArray names;
    if (auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
    {
        const auto all = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        for (int ch = 0; ch < all.size(); ++ch)
            if (active[ch])
                names.add(all[ch]);
    }
    return names;
}

te::WaveInputDevice* AudioEngine::inputForChannel(int channel) const
{
    // Inputs are listed in channel order; the n-th enabled mono input is the n-th active channel.
    int n = 0;
    for (auto* device : engine.getDeviceManager().getWaveInputDevices())
    {
        if (device->getDeviceType() != te::InputDevice::waveDevice || !device->isEnabled())
            continue;
        if (n++ == channel)
            return device;
    }
    return nullptr;
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
    if (masterMeasurer != nullptr)
        masterMeasurer->removeClient(masterMeter);
    masterMeasurer = nullptr;

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
                for (int ch = 0; auto* device = inputForChannel(ch); ++ch)
                    if (&instance->getInputDevice() == device)
                    {
                        p.inputType = (int) InputType::Mono;
                        p.inputChannel = ch;
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
        if (device.getDeviceType() != te::InputDevice::waveDevice)
            continue;

        int channel = -1;
        for (int ch = 0; auto* d = inputForChannel(ch); ++ch)
            if (d == &device)
                channel = ch;

        juce::Array<int> wanted;
        bool anyOn = false, anyAuto = false;
        for (int t = 0; t < kNumTracks; ++t)
        {
            auto& p = tracks[(size_t) t];
            if (channel < 0 || p.inputType.load() == (int) InputType::Mix || p.inputChannel.load() != channel)
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
        if (p.armed.load() && p.inputType.load() != (int) InputType::Mix)
        {
            if (auto* device = inputForChannel(p.inputChannel.load()))
            {
                auto& client = inputMeters[device];
                if (client == nullptr)
                {
                    client = std::make_unique<te::LevelMeasurer::Client>();
                    device->levelMeasurer.addClient(*client);
                }
                const float level = levelToGain(client->getAndClearAudioLevel(0));
                atomicMax(p.peakL, level);
                atomicMax(p.peakR, level);
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
    sync();
    drainMeters();
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The audio device changed: its inputs and the playback context may be new objects.
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
            if (instance->isRecording(id))
                takes.push_back({ t, instance->getRecordingFile(id), toSamples(instance->getPunchInTime(id)) });
        }
    return takes;
}

#include "AudioEngine.h"
#include "../Project/AudioFileIO.h"

namespace
{
    juce::File deviceStateFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("8track").getChildFile("audioDevice.xml");
    }

    /** Where the app saved its device settings when it was called 4track. */
    juce::File legacyDeviceStateFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("4track").getChildFile("audioDevice.xml");
    }

    constexpr int kMaxPrepared = 4096;
}

AudioEngine::AudioEngine(bool openDevice)
{
    const juce::uint32 palette[kNumTracks] = {
        0xff4f9dff, 0xff34c79a, 0xfff2b84b, 0xffef6f6c, 0xffb084f5, 0xff4fd1e0, 0xfff58fc7, 0xffa3d65c
    };
    for (int t = 0; t < kNumTracks; ++t)
    {
        paramArray[(size_t) t] = &tracks[(size_t) t];
        tracks[(size_t) t].name = "Track " + juce::String(t + 1);
        tracks[(size_t) t].colour = palette[t];
        tracks[(size_t) t].inputChannel = t % 2;   // alternate Ch 1 / Ch 2 by default
    }

    audioArrangement = std::make_shared<Arrangement>();
    mixer.prepare(sampleRate.load(), kMaxPrepared);
    scratch.setSize(3, kMaxPrepared);
    scratch.clear();
    scratchSize = kMaxPrepared;

    diskThread.startThread(juce::Thread::Priority::high);
    deviceManager.addChangeListener(this);

    if (!openDevice)
        return;

    if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio)
        || !juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio))
    {
        initialiseDevice();
    }
    else
    {
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](bool granted)
        {
            if (!granted)
                DBG("Microphone permission denied - recording will be silent");
            juce::MessageManager::callAsync([this] { initialiseDevice(); });
        });
    }
}

AudioEngine::~AudioEngine()
{
    cancelPendingUpdate();
    if (getState() == State::Recording)
        stop();
    deviceManager.removeChangeListener(this);
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
    diskThread.stopThread(2000);
}

//==============================================================================
void AudioEngine::initialiseDevice()
{
    if (deviceInitialised)
        return;

    juce::String error;
    std::unique_ptr<juce::XmlElement> saved;
    if (deviceStateFile().existsAsFile())
        saved = juce::XmlDocument::parse(deviceStateFile());
    else if (legacyDeviceStateFile().existsAsFile())
        saved = juce::XmlDocument::parse(legacyDeviceStateFile());

    if (saved != nullptr)
    {
        error = deviceManager.initialise(8, 2, saved.get(), true);
    }
    else
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        setup.inputChannels.setRange(0, 8, true);
        setup.outputChannels.setRange(0, 2, true);
        setup.useDefaultInputChannels = false;
        setup.useDefaultOutputChannels = true;
        error = deviceManager.initialise(8, 2, nullptr, true, {}, &setup);
    }

    if (error.isNotEmpty())
        error = deviceManager.initialiseWithDefaultDevices(2, 2);

    if (error.isNotEmpty())
    {
        DBG("Audio device error: " << error);
        return;
    }

    deviceManager.addAudioCallback(this);
    deviceInitialised = true;
    sendChangeMessage();
}

void AudioEngine::saveDeviceState()
{
    if (auto xml = deviceManager.createStateXml())
    {
        deviceStateFile().getParentDirectory().createDirectory();
        deviceStateFile().replaceWithText(xml->toString());
    }
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    saveDeviceState();
    sendChangeMessage();
}

juce::StringArray AudioEngine::getActiveInputNames() const
{
    juce::StringArray names;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        auto all = device->getInputChannelNames();
        auto active = device->getActiveInputChannels();
        for (int i = 0; i < all.size(); ++i)
            if (active[i])
                names.add(all[i]);
    }
    return names;
}

juce::int64 AudioEngine::getRoundTripLatency() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    return 0;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // The callback isn't running here, so it's safe to (re)allocate.
    const double sr = device->getCurrentSampleRate();
    const int block = juce::jmax(kMaxPrepared, device->getCurrentBufferSizeSamples());
    sampleRate = sr;
    scratch.setSize(3, block);
    scratch.clear();
    scratchSize = block;
    mixer.prepare(sr, block);
    clickRemaining = 0;

    if (getState() == State::Recording)
        triggerAsyncUpdate();   // device restarted mid-take: finish it safely
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::handleAsyncUpdate()
{
    if (getState() == State::Recording)
        stop();
}

//==============================================================================
void AudioEngine::setArrangement(std::shared_ptr<const Arrangement> snapshot)
{
    {
        const juce::ScopedLock sl(deviceManager.getAudioCallbackLock());
        std::swap(audioArrangement, snapshot);
    }
    // `snapshot` now holds the previous arrangement and is released here, on the message thread.
}

juce::int64 AudioEngine::getPosition() const
{
    const auto pending = pendingLocate.load();
    return pending != kNoLocate ? pending : position.load();
}

void AudioEngine::setPosition(juce::int64 newPosition)
{
    if (getState() == State::Recording)
        return;
    pendingLocate = newPosition;
    returnPosition = newPosition;
}

void AudioEngine::play()
{
    if (getState() != State::Stopped)
        return;
    returnPosition = getPosition();
    state = (int) State::Playing;
}

bool AudioEngine::record(const juce::File& takeFolder, juce::String& error)
{
    if (getState() == State::Recording)
        return false;

    const double sr = getSampleRate();
    auto newSession = std::make_unique<RecordSession>();
    newSession->latency = getRoundTripLatency();
    newSession->maxSamples = (juce::int64) (sr * 60.0 * 60.0);   // one hour per take

    if (!takeFolder.createDirectory())
    {
        error = "Couldn't create the folder for recordings:\n" + takeFolder.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = tracks[(size_t) t];
        if (!p.armed.load())
            continue;

        auto take = std::make_unique<LiveTake>();
        take->track = t;
        take->inputType = (InputType) p.inputType.load();
        take->channel = p.inputChannel.load();
        take->file = takeFolder.getChildFile("T" + juce::String(t + 1) + "_" + stamp + "_" + juce::String(++takeCounter) + ".wav");

        auto fail = [&](const juce::String& message)
        {
            error = message;
            for (auto& made : newSession->takes)
            {
                made->writer.reset();
                made->file.deleteFile();
            }
            take->file.deleteFile();
            return false;
        };

        auto stream = take->file.createOutputStream();
        if (stream == nullptr)
            return fail("Couldn't create " + take->file.getFullPathName());

        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(), sr, 2, 24, {}, 0));
        if (writer == nullptr)
            return fail("Couldn't create a WAV writer for " + take->file.getFileName());
        stream.release();   // the writer owns it now

        take->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), diskThread,
                                                                                 (int) (sr * 10.0));
        const auto peakBlocks = (size_t) (newSession->maxSamples / AudioSourceData::kPeakBlock + 1);
        take->peakMin.assign(peakBlocks, 0.0f);
        take->peakMax.assign(peakBlocks, 0.0f);
        newSession->takes.push_back(std::move(take));
    }

    if (newSession->takes.empty())
    {
        error = "Arm at least one track (the R button) before recording.";
        return false;
    }

    // Work out the record window and where the transport starts.
    const bool alreadyPlaying = getState() == State::Playing;
    const auto here = getPosition();
    const auto bar = getSamplesPerBar();
    juce::int64 startAt = here;

    const bool punch = punchOn.load() && punchOut.load() > punchIn.load();
    newSession->punch = punch;
    if (punch)
    {
        newSession->windowStart = punchIn.load();
        newSession->windowEnd = punchOut.load();
        // Pre-roll: one bar before the punch-in, or from the playhead if it's already earlier.
        startAt = here < newSession->windowStart ? here : newSession->windowStart - bar;
    }
    else
    {
        newSession->windowStart = here;
        newSession->windowEnd = std::numeric_limits<juce::int64>::max() / 2;
        if (countInOn.load() && !alreadyPlaying)
            startAt = here - bar;
    }

    newSession->captureStart = newSession->windowStart + newSession->latency;
    newSession->captureEnd = newSession->windowEnd + newSession->latency;

    {
        const juce::ScopedLock sl(deviceManager.getAudioCallbackLock());
        session = std::move(newSession);
        audioSession = session.get();
        if (!alreadyPlaying)
        {
            // Pressing record during playback punches in on the fly without moving the playhead.
            pendingLocate = startAt;
            returnPosition = here;
        }
        state = (int) State::Recording;
    }
    return true;
}

void AudioEngine::stop()
{
    std::unique_ptr<RecordSession> finished;
    {
        const juce::ScopedLock sl(deviceManager.getAudioCallbackLock());
        state = (int) State::Stopped;
        audioSession = nullptr;
        finished = std::move(session);
        pendingLocate = returnPosition;
    }

    if (finished != nullptr)
    {
        auto takes = finishSession(std::move(finished));
        if (onTakesFinished)
            onTakesFinished(std::move(takes));
    }
}

std::vector<AudioEngine::FinishedTake> AudioEngine::finishSession(std::unique_ptr<RecordSession> finished)
{
    std::vector<FinishedTake> result;
    const double sr = getSampleRate();

    // The first captured sample was performed `latency` samples earlier on the timeline.
    const auto first = finished->firstCapture.load();
    const auto takeStart = first == std::numeric_limits<juce::int64>::min()
                               ? finished->windowStart
                               : first - finished->latency;

    for (auto& take : finished->takes)
    {
        take->writer.reset();   // flushes everything still buffered and closes the file

        const auto written = take->written.load();
        if (written < Arrangement::kMinClipLength)
        {
            take->file.deleteFile();
            continue;
        }

        juce::String error;
        auto source = AudioFileIO::loadSource(take->file, sr, error);
        if (source == nullptr)
        {
            DBG("Failed to load take: " << error);
            continue;
        }

        FinishedTake ft;
        ft.track = take->track;
        ft.start = takeStart;
        ft.length = juce::jmin(written, source->getLength());
        ft.source = std::move(source);
        result.push_back(std::move(ft));
    }
    return result;
}

//==============================================================================
void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                                   float* const* outputChannelData, int numOutputChannels,
                                                   int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    // Process in chunks no bigger than the scratch buffers.
    const float* inputs[64] = {};
    const int numIn = juce::jmin(numInputChannels, 64);

    for (int offset = 0; offset < numSamples; offset += scratchSize)
    {
        const int n = juce::jmin(scratchSize, numSamples - offset);
        for (int ch = 0; ch < numIn; ++ch)
            inputs[ch] = inputChannelData[ch] != nullptr ? inputChannelData[ch] + offset : nullptr;

        float* mixL = scratch.getWritePointer(0);
        float* mixR = scratch.getWritePointer(1);
        processChunk(inputs, numIn, mixL, mixR, n);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;
            float* out = outputChannelData[ch] + offset;
            if (ch == 0)
                juce::FloatVectorOperations::copy(out, mixL, n);
            else if (ch == 1)
                juce::FloatVectorOperations::copy(out, mixR, n);
            else
                juce::FloatVectorOperations::clear(out, n);
        }
        if (numOutputChannels == 1 && outputChannelData[0] != nullptr)
            juce::FloatVectorOperations::addWithMultiply(outputChannelData[0] + offset, mixR, 1.0f, n);
    }
}

void AudioEngine::processChunk(const float* const* inputs, int numInputs, float* outL, float* outR, int n)
{
    const auto locate = pendingLocate.exchange(kNoLocate);
    if (locate != kNoLocate)
    {
        position = locate;
        mixer.reset();   // drop filter/reverb state so a jump doesn't click or smear
    }

    const auto st = (State) state.load();
    const bool running = st != State::Stopped;
    const auto pos = position.load(std::memory_order_relaxed);
    auto* rec = st == State::Recording ? audioSession : nullptr;
    const float* silence = scratch.getReadPointer(2);

    auto inputFor = [&](int ch) -> const float*
    {
        return (ch >= 0 && ch < numInputs && inputs[ch] != nullptr) ? inputs[ch] : silence;
    };

    // inWindow: the old audio on a recording track is replaced (silenced).
    // monitorWindow: Auto monitoring hears the input. For a plain recording that
    // includes the count-in; for a punch, the pre-roll plays the old take instead.
    const bool inWindow = rec != nullptr && pos + n > rec->windowStart && pos < rec->windowEnd;
    const bool monitorWindow = rec != nullptr && (inWindow || !rec->punch);

    // Decide what each track feeds into the mix.
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = tracks[(size_t) t];
        auto& feed = feeds[(size_t) t];
        feed = {};

        const bool armed = p.armed.load(std::memory_order_relaxed);
        const auto type = (InputType) p.inputType.load(std::memory_order_relaxed);
        const auto mode = (MonitorMode) p.monitor.load(std::memory_order_relaxed);

        bool inSession = false;
        if (rec != nullptr)
            for (auto& take : rec->takes)
                inSession = inSession || take->track == t;
        const bool recordingHere = inSession && inWindow;

        const bool monitorInput = armed && type != InputType::Mix
            && (mode == MonitorMode::On
                || (mode == MonitorMode::Auto && (st == State::Stopped || (inSession && monitorWindow))));

        if (monitorInput)
        {
            const int ch = p.inputChannel.load(std::memory_order_relaxed);
            feed.kind = MixEngine::TrackFeed::Kind::LiveInput;
            feed.left = inputFor(ch);
            feed.right = type == InputType::Stereo ? inputFor(ch + 1) : feed.left;
        }
        else if (recordingHere)
        {
            feed.kind = MixEngine::TrackFeed::Kind::Silence;   // the new take replaces what was there
        }

        // Armed tracks always show their input level, even when it isn't being monitored,
        // so you can set levels and see that signal is arriving without risking feedback.
        if (armed && !monitorInput && type != InputType::Mix)
        {
            const int ch = p.inputChannel.load(std::memory_order_relaxed);
            const float* l = inputFor(ch);
            const float* r = type == InputType::Stereo ? inputFor(ch + 1) : l;
            auto peak = [n](const float* d)
            {
                auto range = juce::FloatVectorOperations::findMinAndMax(d, n);
                return juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
            };
            atomicMax(p.peakL, peak(l));
            atomicMax(p.peakR, peak(r));
        }
    }

    mixer.process(audioArrangement.get(), paramArray, master, pos, n, running, feeds.data(), outL, outR, true);

    if (rec != nullptr)
        captureInput(inputs, numInputs, outL, outR, n);

    const bool countIn = rec != nullptr && pos < rec->windowStart && countInOn.load(std::memory_order_relaxed);
    if (running && (metronomeOn.load(std::memory_order_relaxed) || countIn))
        addMetronome(outL, outR, n, pos, countIn);

    if (running)
        position.store(pos + n, std::memory_order_relaxed);
}

void AudioEngine::captureInput(const float* const* inputs, int numInputs, const float* mixL, const float* mixR, int n)
{
    auto* rec = audioSession;
    const auto pos = position.load(std::memory_order_relaxed);

    // Overlap of this block with the (latency-shifted) capture window.
    const auto s0 = juce::jmax(pos, rec->captureStart);
    const auto s1 = juce::jmin(pos + n, rec->captureEnd);
    if (s1 <= s0)
        return;

    const int offset = (int) (s0 - pos);
    const int count = (int) (s1 - s0);
    if (rec->firstCapture.load(std::memory_order_relaxed) == std::numeric_limits<juce::int64>::min())
        rec->firstCapture.store(s0, std::memory_order_relaxed);
    const float* silence = scratch.getReadPointer(2);
    auto inputFor = [&](int ch) -> const float*
    {
        return (ch >= 0 && ch < numInputs && inputs[ch] != nullptr) ? inputs[ch] : silence;
    };

    for (auto& take : rec->takes)
    {
        const auto written = take->written.load(std::memory_order_relaxed);
        if (take->overflowed.load(std::memory_order_relaxed) || written + count > rec->maxSamples)
        {
            take->overflowed = true;
            continue;
        }

        const float* l;
        const float* r;
        if (take->inputType == InputType::Mix)
        {
            l = mixL;
            r = mixR;
        }
        else
        {
            l = inputFor(take->channel);
            r = take->inputType == InputType::Stereo ? inputFor(take->channel + 1) : l;
        }

        const float* chans[2] = { l + offset, r + offset };
        if (!take->writer->write(chans, count))
            take->overflowed = true;   // disk couldn't keep up; keep what we have

        // Live waveform overview for the UI
        for (int i = 0; i < count; ++i)
        {
            const auto block = (size_t) ((written + i) / AudioSourceData::kPeakBlock);
            const float v = 0.5f * (chans[0][i] + chans[1][i]);
            take->peakMin[block] = juce::jmin(take->peakMin[block], v);
            take->peakMax[block] = juce::jmax(take->peakMax[block], v);
        }
        take->written.store(written + count, std::memory_order_release);
    }
}

void AudioEngine::addMetronome(float* outL, float* outR, int n, juce::int64 pos, bool countInClick)
{
    const double sr = getSampleRate();
    const double spb = sr * 60.0 / juce::jmax(20.0, bpm.load(std::memory_order_relaxed));
    const int bpb = juce::jmax(1, beatsPerBar.load(std::memory_order_relaxed));
    const float level = dbToGain(clickLevelDb.load(std::memory_order_relaxed)) * (countInClick ? 1.0f : 0.8f);

    // Beat boundaries that fall inside this block.
    auto beat = (juce::int64) std::ceil((double) pos / spb);
    int nextTrigger = (int) std::ceil((double) beat * spb - (double) pos);

    for (int i = 0; i < n; ++i)
    {
        if (i == nextTrigger && nextTrigger < n)
        {
            const bool accent = ((beat % bpb) + bpb) % bpb == 0;
            clickPhase = 0.0;
            clickPhaseInc = juce::MathConstants<double>::twoPi * (accent ? 1760.0 : 1320.0) / sr;
            clickRemaining = (int) (sr * 0.035);
            clickAmp = level * (accent ? 1.0f : 0.6f);
            ++beat;
            nextTrigger = (int) std::ceil((double) beat * spb - (double) pos);
        }

        if (clickRemaining > 0)
        {
            const float env = (float) clickRemaining / (float) (sr * 0.035);
            const float s = clickAmp * env * env * (float) std::sin(clickPhase);
            clickPhase += clickPhaseInc;
            --clickRemaining;
            outL[i] += s;
            outR[i] += s;
        }
    }
}

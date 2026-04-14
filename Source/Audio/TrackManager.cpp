#include "TrackManager.h"

TrackManager::TrackManager()
{
    // Initialize 4 tracks
    for (int i = 0; i < NUM_TRACKS; ++i)
    {
        tracks[i] = std::make_unique<Track>(i);
    }
}

TrackManager::~TrackManager()
{
}

void TrackManager::prepareToPlay(double sampleRate, int samplesPerBlockExpected)
{
    currentSampleRate = sampleRate;

    // Prepare reverb
    reverb.setSampleRate(sampleRate);
    reverb.reset();

    // Allocate reverb buffers
    reverbSendBuffer.setSize(2, samplesPerBlockExpected);
    reverbReturnBuffer.setSize(2, samplesPerBlockExpected);

    // Initialize reverb parameters
    updateReverbParameters();

    // Prepare each track
    for (auto& track : tracks)
    {
        if (track)
        {
            // Set sample rate for EQ filters
            track->setSampleRate(sampleRate);

            // Ensure tracks have sufficient buffer space
            // Allocate 5 minutes at current sample rate
            int bufferSize = static_cast<int>(sampleRate * 60.0 * 5.0);
            track->ensureBufferSize(bufferSize);
        }
    }
}

void TrackManager::processBlock(juce::AudioBuffer<float>& outputBuffer,
                                const juce::AudioBuffer<float>& inputBuffer,
                                bool isRecording)
{
    const int numSamples = outputBuffer.getNumSamples();

    // Clear output and reverb send buffers
    outputBuffer.clear();
    reverbSendBuffer.clear();

    // Check if any track is soloed
    const bool soloActive = anySoloEnabled();

    // Detect bounce-recording tracks (armed + BounceFromMix input mode)
    bool anyBounceRecording = false;
    if (isRecording)
    {
        for (auto& track : tracks)
        {
            if (track && track->isRecordArmed() &&
                track->getInputMode() == Track::InputMode::BounceFromMix)
            {
                anyBounceRecording = true;
                break;
            }
        }
    }

    // Debug recording
    static int recordDebugCounter = 0;
    if (isRecording && ++recordDebugCounter % 100 == 0)
    {
        float maxInput = 0.0f;
        for (int ch = 0; ch < inputBuffer.getNumChannels(); ++ch)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                maxInput = juce::jmax(maxInput, std::abs(inputBuffer.getSample(ch, i)));
            }
        }
        juce::Logger::writeToLog("Recording: pos=" + juce::String(currentPosition) +
                                 " maxInput=" + juce::String(maxInput, 4));
    }

    // Process each track
    for (auto& track : tracks)
    {
        if (!track)
            continue;

        // Update track playback position
        track->setPlaybackPosition(currentPosition);

        const bool isBounceTrack = (track->getInputMode() == Track::InputMode::BounceFromMix);

        // Record from hardware input (non-bounce tracks only)
        if (isRecording && track->isRecordArmed() && !isBounceTrack)
        {
            const int numInputChannels = inputBuffer.getNumChannels();

            for (int sample = 0; sample < numSamples; ++sample)
            {
                float leftSample, rightSample;

                if (track->getInputMode() == Track::InputMode::Mono)
                {
                    // Mono mode: read single channel and duplicate to both L/R
                    int ch = track->getInputChannelLeft();
                    float monoSample = (ch < numInputChannels)
                        ? inputBuffer.getSample(ch, sample)
                        : 0.0f;
                    leftSample = rightSample = monoSample;
                }
                else // Stereo mode
                {
                    // Stereo mode: read separate L/R channels
                    int chL = track->getInputChannelLeft();
                    int chR = track->getInputChannelRight();
                    leftSample = (chL < numInputChannels)
                        ? inputBuffer.getSample(chL, sample)
                        : 0.0f;
                    rightSample = (chR < numInputChannels)
                        ? inputBuffer.getSample(chR, sample)
                        : 0.0f;
                }

                track->addInputSample(leftSample, rightSample, currentPosition + sample);
            }
        }

        // Bounce-armed tracks are excluded from the mix during bounce recording
        // so we only hear the source tracks, not a feedback loop
        if (anyBounceRecording && isRecording && track->isRecordArmed() && isBounceTrack)
        {
            // Still process for metering, but discard output
            juce::AudioBuffer<float> dummyBuffer(2, numSamples);
            dummyBuffer.clear();
            track->processBlock(dummyBuffer, 0, numSamples);
            continue;
        }

        // Always call processBlock to update VU meters
        // The Track itself will handle mute logic for output
        // TrackManager only needs to enforce solo logic
        bool shouldOutput = !soloActive || track->isSolo();

        if (shouldOutput)
        {
            track->processBlock(outputBuffer, 0, numSamples);

            // Collect reverb send (post-fader)
            float sendLevel = track->getReverbSend();
            if (sendLevel > 0.001f)
            {
                track->addToReverbSend(reverbSendBuffer, 0, numSamples, sendLevel);
            }
        }
        else
        {
            // Track is soloed out, but still process for metering
            // Just don't output to the mix (pass a dummy buffer)
            juce::AudioBuffer<float> dummyBuffer(2, numSamples);
            dummyBuffer.clear();
            track->processBlock(dummyBuffer, 0, numSamples);
        }
    }

    // Process reverb bus if there's signal
    if (reverbSendBuffer.getMagnitude(0, numSamples) > 0.0001f)
    {
        // Copy send buffer to return buffer for processing
        reverbReturnBuffer.makeCopyOf(reverbSendBuffer);

        // Process through reverb (stereo)
        reverb.processStereo(reverbReturnBuffer.getWritePointer(0),
                            reverbReturnBuffer.getWritePointer(1),
                            numSamples);

        // Add reverb return to output mix
        for (int ch = 0; ch < juce::jmin(2, outputBuffer.getNumChannels()); ++ch)
        {
            outputBuffer.addFrom(ch, 0, reverbReturnBuffer, ch, 0, numSamples);
        }
    }

    // Feed the final mix into any bounce-armed tracks
    if (anyBounceRecording)
    {
        const int numOutChannels = outputBuffer.getNumChannels();
        for (auto& track : tracks)
        {
            if (!track || !track->isRecordArmed() ||
                track->getInputMode() != Track::InputMode::BounceFromMix)
                continue;

            for (int sample = 0; sample < numSamples; ++sample)
            {
                float left  = (numOutChannels > 0) ? outputBuffer.getSample(0, sample) : 0.0f;
                float right = (numOutChannels > 1) ? outputBuffer.getSample(1, sample) : left;
                track->addInputSample(left, right, currentPosition + sample);
            }
        }
    }

    // Update master peak level metering with decay
    static constexpr float masterDecay = 0.95f;
    float peakL = (outputBuffer.getNumChannels() > 0) ? outputBuffer.getMagnitude(0, 0, numSamples) : 0.0f;
    float peakR = (outputBuffer.getNumChannels() > 1) ? outputBuffer.getMagnitude(1, 0, numSamples) : peakL;
    masterPeakLeft  = std::max(masterPeakLeft.load()  * masterDecay, peakL);
    masterPeakRight = std::max(masterPeakRight.load() * masterDecay, peakR);
}

void TrackManager::releaseResources()
{
    // Nothing specific to release for now
}

Track* TrackManager::getTrack(int index)
{
    if (index >= 0 && index < NUM_TRACKS)
        return tracks[index].get();

    return nullptr;
}

const Track* TrackManager::getTrack(int index) const
{
    if (index >= 0 && index < NUM_TRACKS)
        return tracks[index].get();

    return nullptr;
}

void TrackManager::setPlaybackPosition(int position)
{
    currentPosition = position;

    for (auto& track : tracks)
    {
        if (track)
            track->setPlaybackPosition(position);
    }
}

bool TrackManager::anySoloEnabled() const
{
    for (const auto& track : tracks)
    {
        if (track && track->isSolo())
            return true;
    }

    return false;
}

void TrackManager::startRecordingToFiles(const juce::File& projectFolder)
{
    if (!projectFolder.exists())
    {
        projectFolder.createDirectory();
    }

    for (int i = 0; i < NUM_TRACKS; ++i)
    {
        if (tracks[i] && tracks[i]->isRecordArmed())
        {
            juce::File trackFile = projectFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");
            tracks[i]->startRecordingToFile(trackFile, currentSampleRate);
        }
    }

    juce::Logger::writeToLog("TrackManager: Started recording to files in " + projectFolder.getFullPathName());
}

void TrackManager::stopRecordingToFiles()
{
    for (auto& track : tracks)
    {
        if (track)
        {
            track->stopRecordingToFile();
        }
    }

    juce::Logger::writeToLog("TrackManager: Stopped recording to files");
}

void TrackManager::stopRecordingToFilesForPunch()
{
    for (auto& track : tracks)
    {
        if (track)
            track->stopRecordingToFileForPunch();
    }

    juce::Logger::writeToLog("TrackManager: Punch out - file writers flushed, memory buffer preserved");
}

void TrackManager::setReverbRoomSize(float value)
{
    reverbRoomSize = juce::jlimit(0.0f, 1.0f, value);
    updateReverbParameters();
}

void TrackManager::setReverbDamping(float value)
{
    reverbDamping = juce::jlimit(0.0f, 1.0f, value);
    updateReverbParameters();
}

void TrackManager::setReverbWetLevel(float value)
{
    reverbWetLevel = juce::jlimit(0.0f, 1.0f, value);
    updateReverbParameters();
}

void TrackManager::updateReverbParameters()
{
    juce::Reverb::Parameters params;
    params.roomSize = reverbRoomSize.load();
    params.damping = reverbDamping.load();
    params.wetLevel = reverbWetLevel.load();
    params.dryLevel = 0.0f;  // Dry signal is already in direct mix
    params.width = 1.0f;
    params.freezeMode = 0.0f;
    reverb.setParameters(params);
}

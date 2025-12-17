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

    // Prepare each track
    for (auto& track : tracks)
    {
        if (track)
        {
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

    // Clear output buffer first
    outputBuffer.clear();

    // Check if any track is soloed
    const bool soloActive = anySoloEnabled();

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

        // Handle recording
        if (isRecording && track->isRecordArmed())
        {
            const int numInputChannels = inputBuffer.getNumChannels();

            for (int sample = 0; sample < numSamples; ++sample)
            {
                float leftSample = numInputChannels >= 1 ? inputBuffer.getSample(0, sample) : 0.0f;
                float rightSample = numInputChannels >= 2 ? inputBuffer.getSample(1, sample) : leftSample;

                track->addInputSample(leftSample, rightSample, currentPosition + sample);
            }
        }

        // Always call processBlock to update VU meters
        // The Track itself will handle mute logic for output
        // TrackManager only needs to enforce solo logic
        bool shouldOutput = !soloActive || track->isSolo();

        if (shouldOutput)
        {
            track->processBlock(outputBuffer, 0, numSamples);
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

    // Advance playback position
    currentPosition += numSamples;
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

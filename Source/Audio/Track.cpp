#include "Track.h"

Track::Track(int trackNum)
    : trackNumber(trackNum),
      trackName("Track " + juce::String(trackNum + 1))
{
    // Initialize with 2 channels (stereo), 10 seconds at 44.1kHz
    recordedAudio.setSize(2, 441000);
    recordedAudio.clear();
}

Track::~Track()
{
}

void Track::processBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    float currentPeak = 0.0f;

    // If muted, don't output anything but still calculate peak for meters
    const int numChannels = juce::jmin(outputBuffer.getNumChannels(), recordedAudio.getNumChannels());
    const float vol = volume.load();
    const float panValue = pan.load();

    // Calculate left and right gains based on pan
    // Pan: -1.0 = full left, 0.0 = center, 1.0 = full right
    const float leftGain = vol * (1.0f - juce::jmax(0.0f, panValue));
    const float rightGain = vol * (1.0f + juce::jmin(0.0f, panValue));

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const int readPos = playbackPosition + sample;

        // Bounds check
        if (readPos >= 0 && readPos < recordedAudio.getNumSamples())
        {
            float leftSample = 0.0f;
            float rightSample = 0.0f;

            // Read samples and calculate peak
            if (numChannels >= 1 && recordedAudio.getNumChannels() >= 1)
            {
                leftSample = recordedAudio.getSample(0, readPos);
                currentPeak = juce::jmax(currentPeak, std::abs(leftSample * leftGain));
            }

            if (numChannels >= 2 && recordedAudio.getNumChannels() >= 2)
            {
                rightSample = recordedAudio.getSample(1, readPos);
                currentPeak = juce::jmax(currentPeak, std::abs(rightSample * rightGain));
            }

            // Add to output (mixing with other tracks) if not muted
            if (!muted)
            {
                if (numChannels >= 1 && recordedAudio.getNumChannels() >= 1)
                {
                    outputBuffer.addSample(0, startSample + sample, leftSample * leftGain);
                }

                if (numChannels >= 2 && recordedAudio.getNumChannels() >= 2)
                {
                    outputBuffer.addSample(1, startSample + sample, rightSample * rightGain);
                }
            }
        }
    }

    // Update peak level (with decay)
    float oldPeak = peakLevel.load();
    peakLevel.store(juce::jmax(currentPeak, oldPeak * 0.95f));
}

void Track::addInputSample(float leftSample, float rightSample, int position)
{
    // Only record if armed
    if (!recordArmed)
        return;

    // Clamp input to safe range (-1.0 to 1.0)
    leftSample = juce::jlimit(-1.0f, 1.0f, leftSample);
    rightSample = juce::jlimit(-1.0f, 1.0f, rightSample);

    // Update peak level for input monitoring
    float inputPeak = juce::jmax(std::abs(leftSample), std::abs(rightSample));
    float oldPeak = peakLevel.load();
    peakLevel.store(juce::jmax(inputPeak, oldPeak * 0.95f));

    // Ensure buffer is large enough
    if (position >= recordedAudio.getNumSamples())
    {
        ensureBufferSize(position + 44100); // Add 1 second buffer
    }

    // Write to buffer
    if (position >= 0 && position < recordedAudio.getNumSamples())
    {
        if (recordedAudio.getNumChannels() >= 1)
            recordedAudio.setSample(0, position, leftSample);

        if (recordedAudio.getNumChannels() >= 2)
            recordedAudio.setSample(1, position, rightSample);
    }
}

void Track::clearBuffer()
{
    recordedAudio.clear();
    playbackPosition = 0;
}

void Track::ensureBufferSize(int numSamples)
{
    if (numSamples > recordedAudio.getNumSamples())
    {
        juce::AudioBuffer<float> newBuffer(recordedAudio.getNumChannels(), numSamples);
        newBuffer.clear();

        // Copy existing data
        for (int ch = 0; ch < recordedAudio.getNumChannels(); ++ch)
        {
            newBuffer.copyFrom(ch, 0, recordedAudio, ch, 0, recordedAudio.getNumSamples());
        }

        recordedAudio = std::move(newBuffer);
    }
}

void Track::loadAudioData(const juce::AudioBuffer<float>& sourceBuffer)
{
    const int numSamples = sourceBuffer.getNumSamples();
    const int numChannels = juce::jmin(2, sourceBuffer.getNumChannels());

    // Ensure our buffer is large enough
    ensureBufferSize(numSamples);

    // Copy audio data directly to our buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        recordedAudio.copyFrom(ch, 0, sourceBuffer, ch, 0, numSamples);
    }

    // If source is mono and we have stereo, copy to both channels
    if (sourceBuffer.getNumChannels() == 1 && recordedAudio.getNumChannels() >= 2)
    {
        recordedAudio.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }

    playbackPosition = 0;
}

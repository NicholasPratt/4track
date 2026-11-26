#pragma once

#include "Effects.h"
#include <tracktion_engine/tracktion_engine.h>

/**
    What 8track's sample players share: a loaded sample, a slot that swaps samples while the
    audio thread plays them, and a recorder that captures an audio input.
*/
struct Sample : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<Sample>;

    juce::AudioBuffer<float> audio;   // one or two channels
    double rate = 48000.0;
    juce::String name;

    double lengthSeconds() const { return audio.getNumSamples() / rate; }

    /** Reads an audio file (up to maxSeconds long). */
    static Ptr load(const juce::File&, juce::String& error, double maxSeconds = 120.0);
    static Ptr load(std::unique_ptr<juce::InputStream>, const juce::String& name,
                    juce::String& error, double maxSeconds = 120.0);
    static Ptr fromBuffer(juce::AudioBuffer<float>&&, double rate, const juce::String& name);

    /** A sample between samples (cubic), 0 outside the sample. */
    float read(int channel, double position) const
    {
        const int n = audio.getNumSamples();
        const int i = (int) std::floor(position);
        if (i < -1 || i >= n)
            return 0.0f;
        const float t = (float) (position - i);
        const float* d = audio.getReadPointer(juce::jmin(channel, audio.getNumChannels() - 1));
        auto at = [&](int k) { return k >= 0 && k < n ? d[k] : 0.0f; };
        const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    /** Peak levels in `count` equal slices, for drawing. */
    std::vector<float> peaks(int count) const;
};

/**
    One sample the audio thread plays while the message thread may replace it. Replaced
    samples are freed later on the message thread, once no voice still holds them, so the
    audio thread never frees memory.
*/
class SampleSlot
{
public:
    Sample::Ptr get() const   // either thread
    {
        const juce::SpinLock::ScopedLockType lock(spin);
        return current;
    }
    void set(Sample::Ptr);   // message thread

private:
    mutable juce::SpinLock spin;
    Sample::Ptr current;
};

/** Records one hardware input (or a stereo pair) into memory, up to a time limit. */
class InputRecorder : private te::DeviceManager::AudioInputObserver, private juce::Timer
{
public:
    explicit InputRecorder(te::DeviceManager&);
    ~InputRecorder() override;

    void start(int firstChannel, int numChannels, double maxSeconds = 60.0);
    /** Stops and hands over what was recorded, with leading silence trimmed. */
    Sample::Ptr stop(const juce::String& name);
    bool isRecording() const { return recording.load(); }
    double getSeconds() const;
    float getLevel() const { return level.load(); }
    std::function<void()> onLimitReached;

private:
    void onAudioInput(const float* const*, int channels, int frames, double sampleRate) override;
    void timerCallback() override;
    void drain();

    te::DeviceManager& devices;
    std::atomic<bool> recording { false };
    std::atomic<float> level { 0.0f };
    std::atomic<double> deviceRate { 48000.0 };
    int first = 0, count = 1;   // set under the callback lock
    static constexpr int kFifo = 1 << 17;
    juce::AbstractFifo fifo { kFifo };
    juce::AudioBuffer<float> ring { 2, kFifo };
    juce::AudioBuffer<float> taken;
    int takenLength = 0, maxLength = 0;
};

/** A two-band tilt: dark (-1) to bright (+1) around 1 kHz. */
struct Tilt
{
    float low = 0, coeff = 0, lowGain = 1, highGain = 1;
    void set(double sampleRate, float tone)
    {
        coeff = (float) (1.0 - std::exp(-juce::MathConstants<double>::twoPi * 1000.0 / sampleRate));
        lowGain = juce::Decibels::decibelsToGain(-9.0f * tone);
        highGain = juce::Decibels::decibelsToGain(9.0f * tone);
    }
    float process(float x)
    {
        low += coeff * (x - low);
        return low * lowGain + (x - low) * highGain;
    }
};

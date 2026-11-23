#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

enum class MonitorMode { Off = 0, Auto = 1, On = 2 };   // Auto = hear input while armed
enum class InputType   { Mono = 0, Stereo = 1, Mix = 2 }; // Mix = bounce the master bus

/** Relaxed-atomic float max: lets the audio thread accumulate meter peaks that the UI drains. */
inline void atomicMax(std::atomic<float>& a, float v)
{
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

inline float dbToGain(float db) { return db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }

/**
    Mixer settings for one track. Written by the UI, read by the audio thread.
    Lives for the whole session (never reallocated), so raw pointers are safe.
*/
struct TrackParams
{
    std::atomic<float> volumeDb { 0.0f };    // -60 (= -inf) .. +6
    std::atomic<float> pan { 0.0f };         // -1 .. 1
    std::atomic<float> send { 0.0f };        // reverb send, 0 .. 1 (post-fader)
    std::atomic<float> lowCut { 20.0f };     // 20 = off
    std::atomic<float> highCut { 20000.0f }; // 20000 = off
    std::atomic<float> midFreq { 1000.0f };
    std::atomic<float> midGainDb { 0.0f };

    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<bool> armed { false };
    std::atomic<int>  monitor { (int) MonitorMode::Off };
    std::atomic<int>  inputType { (int) InputType::Mono };
    std::atomic<int>  inputChannel { 0 };    // mono channel, or left of a stereo pair
    std::atomic<bool> midi { false };        // a MIDI track: records from MIDI inputs, plays an instrument

    // Meters: max peak since the UI last read them (UI calls exchange(0)).
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    // Message-thread only
    juce::String name;
    juce::uint32 colour = 0xff4f9dff;   // ARGB (the audio layer has no juce_graphics)

    void resetMixer()
    {
        volumeDb = 0.0f; pan = 0.0f; send = 0.0f;
        lowCut = 20.0f; highCut = 20000.0f; midFreq = 1000.0f; midGainDb = 0.0f;
        mute = false; solo = false; armed = false;
        monitor = (int) MonitorMode::Off;
    }
};

struct MasterParams
{
    std::atomic<float> volumeDb { 0.0f };
    std::atomic<float> reverbRoom { 0.5f };
    std::atomic<float> reverbDamping { 0.5f };
    std::atomic<float> reverbWet { 0.33f };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
};

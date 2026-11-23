#pragma once

#include "../Model/Arrangement.h"
#include "Parameters.h"
#include <array>

/**
    Renders a range of the arrangement through the per-track EQ, fader, pan and
    reverb send, then the master reverb and fader.

    The live engine and the offline mixdown use this same code, so an export sounds
    the way playback does. Each instance owns its own DSP state; nothing is shared
    with other instances. process() never allocates, locks or does I/O, and it can
    take any block size (blocks bigger than the prepared size are split up).
*/
class MixEngine
{
public:
    using ParamArray = std::array<TrackParams*, kNumTracks>;

    /** What a track feeds into the mixer for this block. */
    struct TrackFeed
    {
        enum class Kind { Playback, LiveInput, Silence };
        Kind kind = Kind::Playback;
        const float* left = nullptr;   // for LiveInput: numSamples of input
        const float* right = nullptr;
    };

    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    /**
        Renders numSamples starting at timeline position `position` into outL/outR,
        overwriting them. `feeds` may be null, which means every track plays back.
        When playClips is false nothing from the arrangement is heard, but live input
        and the reverb tail still are.
    */
    void process(const Arrangement* arrangement, const ParamArray& params, MasterParams& master,
                 juce::int64 position, int numSamples, bool playClips,
                 const TrackFeed* feeds, float* outL, float* outR, bool updateMeters);

    /** Offline mixdown of [0, arrangement end + tail). Runs on the calling thread. */
    static juce::AudioBuffer<float> renderOffline(const Arrangement& arrangement, const ParamArray& params,
                                                  MasterParams& master, double sampleRate, double tailSeconds);

private:
    struct TrackDsp
    {
        juce::IIRFilter lowCut[2], highCut[2], mid[2];
        float appliedLc = -1.0f, appliedHc = -1.0f, appliedMf = -1.0f, appliedMg = -999.0f;
        bool lcOn = false, hcOn = false, midOn = false;
        juce::LinearSmoothedValue<float> gainL, gainR;
    };

    void processChunk(const Arrangement* arrangement, const ParamArray& params, MasterParams& master,
                      juce::int64 position, int n, bool playClips, const TrackFeed* feeds, int feedOffset,
                      float* outL, float* outR, bool updateMeters);

    void updateFilters(TrackDsp& dsp, const TrackParams& p);
    static void renderClips(const std::vector<Clip>& lane, juce::int64 position, int n, float* l, float* r);

    double sampleRate = 44100.0;
    int maxBlock = 0;

    std::array<TrackDsp, kNumTracks> dsp;
    juce::AudioBuffer<float> trackBuffer;   // 2 x maxBlock scratch
    juce::AudioBuffer<float> sendBuffer;    // 2 x maxBlock reverb bus

    juce::Reverb reverb;
    float appliedRoom = -1.0f, appliedDamp = -1.0f, appliedWet = -1.0f;
    juce::LinearSmoothedValue<float> masterGain;
};

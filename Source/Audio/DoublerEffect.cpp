#include "DoublerEffect.h"

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    juce::String hzText(float v) { return v >= 1000.0f ? juce::String(v / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz"; }

    /** How each voice differs from the others: its delay (times Delay), its detune (times
        Detune), its pan (times Width), how fast it wanders, and how loud it is. */
    struct VoiceShape { double delay, detune, pan, driftHz; };
    constexpr VoiceShape kOne[] = { { 1.0, 1.0, 0.0, 1.1 } };
    constexpr VoiceShape kTwo[] = { { 1.0, 1.0, -1.0, 1.1 }, { 1.37, -1.0, 1.0, 0.83 } };
    constexpr VoiceShape kFour[] = { { 1.0, 1.0, -1.0, 1.1 }, { 1.37, -1.0, 1.0, 0.83 },
                                     { 1.18, -0.6, -0.45, 1.27 }, { 1.61, 0.6, 0.45, 0.71 } };

    const VoiceShape* shapesFor(int choice)
    {
        return choice == DoublerEffect::One ? kOne : choice == DoublerEffect::Two ? kTwo : kFour;
    }

    constexpr double kWindowMs = 16.0;      // the pitch shifter's sweep: its taps are this far apart at most
    constexpr double kMaxDriftMs = 2.5;     // how far a voice wanders at 100% Variation
}

const char* DoublerEffect::xmlTypeName = "eightTrackDoubler";

DoublerEffect::DoublerEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    voices    = addChoice("voices", "Voices", voiceNames(), Two, "Doubles", 2);
    delay     = addKnob("delay", "Delay", { 12.0f, 60.0f, 0.1f }, 20.0f,
                        [](float v) { return juce::String(v, 1) + " ms"; }, "Doubles");
    detune    = addKnob("detune", "Detune", { 0.0f, 40.0f, 0.1f }, 9.0f,
                        [](float v) { return juce::String(v, 1) + " ct"; }, "Doubles");
    variation = addKnob("variation", "Variation", { 0.0f, 1.0f, 0.01f }, 0.5f, percentText, "Doubles");
    width     = addKnob("width", "Width", { 0.0f, 1.0f, 0.01f }, 0.8f, percentText, "Tone");
    lowCut    = addKnob("lowCut", "Low Cut", { 20.0f, 1000.0f, 1.0f, 0.4f }, 150.0f, hzText, "Tone");
    highCut   = addKnob("highCut", "High Cut", { 1000.0f, 20000.0f, 1.0f, 0.4f }, 9000.0f, hzText, "Tone");
    mix       = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 0.5f, percentText, "Output");

    for (int v = 0; v < kMaxVoices; ++v)
        voiceState[(size_t) v].random.setSeed(0x8d0b1e + 7919 * v);
}

DoublerEffect::~DoublerEffect() { notifyListenersOfDeletion(); }

bool DoublerEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == width)
        return choiceOf(*voices) != One;
    return true;
}

void DoublerEffect::initialise(const te::PluginInitialisationInfo& info)
{
    line.assign((size_t) std::ceil(kMaxSeconds * info.sampleRate) + 8, 0.0f);
    reset();
}

void DoublerEffect::reset()
{
    std::fill(line.begin(), line.end(), 0.0f);
    writePos = 0;
    smoothedDelayMs = smoothedDetune = smoothedVariation = -1;
    for (int v = 0; v < kMaxVoices; ++v)
    {
        auto& s = voiceState[(size_t) v];
        s.random.setSeed(0x8d0b1e + 7919 * v);
        s.shifterPhase = 0.5;   // tap A at full level, at the voice's delay
        s.driftFrom = s.driftTo = 0;
        s.driftPos = 1;
    }
    for (auto* f : { lowCutFilter, highCutFilter })
        f[0].reset(), f[1].reset();
}

/** The line `delaySamples` behind the sample just written, with cubic interpolation. */
float DoublerEffect::read(double delaySamples) const
{
    const int size = (int) line.size();
    const double pos = writePos - juce::jlimit(2.0, (double) size - 3.0, delaySamples);
    const int i1 = (int) std::floor(pos);
    const float t = (float) (pos - i1);
    auto at = [&](int i) { return line[(size_t) (((i % size) + size) % size)]; };
    const float y0 = at(i1 - 1), y1 = at(i1), y2 = at(i1 + 1), y3 = at(i1 + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void DoublerEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || line.empty())
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* l = b.getWritePointer(0, fc.bufferStartSample);
    float* r = b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample);

    const int choice = choiceOf(*voices);
    const int n = voiceCount(choice);
    const VoiceShape* shapes = shapesFor(choice);
    const auto [dryGain, wetGain] = mixGains(mix->getCurrentValue());
    const double spread = width->getCurrentValue();
    const double msToSamples = sampleRate / 1000.0;
    const double window = kWindowMs * msToSamples;

    // Each double a little under the lead (-3 dB in all), panned with equal power so that
    // Width doesn't change how loud they are.
    const float voiceLevel = std::sqrt(0.5f / (float) n);
    std::array<float, kMaxVoices> panL {}, panR {};
    for (int v = 0; v < n; ++v)
    {
        const double theta = (shapes[v].pan * spread + 1.0) * juce::MathConstants<double>::pi * 0.25;
        panL[(size_t) v] = voiceLevel * (float) (std::sqrt(2.0) * std::cos(theta));
        panR[(size_t) v] = voiceLevel * (float) (std::sqrt(2.0) * std::sin(theta));
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        lowCutFilter[ch].set(sampleRate, lowCut->getCurrentValue(), 0.707);
        highCutFilter[ch].set(sampleRate, highCut->getCurrentValue(), 0.707);
    }

    // The knobs glide, so turning them doesn't click or zip.
    const float targetDelay = delay->getCurrentValue(), targetDetune = detune->getCurrentValue(),
                targetVariation = variation->getCurrentValue();
    if (smoothedDelayMs < 0)
    {
        smoothedDelayMs = targetDelay;
        smoothedDetune = targetDetune;
        smoothedVariation = targetVariation;
    }
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));
    const int size = (int) line.size();

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        smoothedDelayMs += glide * (targetDelay - smoothedDelayMs);
        smoothedDetune += glide * (targetDetune - smoothedDetune);
        smoothedVariation += glide * (targetVariation - smoothedVariation);

        const float in = 0.5f * (l[i] + r[i]);
        line[(size_t) writePos] = in;

        float wetL = 0.0f, wetR = 0.0f;
        for (int v = 0; v < n; ++v)
        {
            auto& s = voiceState[(size_t) v];
            const auto& shape = shapes[v];

            // The wander: a smooth (raised-cosine) walk between random points.
            s.driftPos += shape.driftHz / sampleRate;
            if (s.driftPos >= 1.0)
            {
                s.driftPos -= 1.0;
                s.driftFrom = s.driftTo;
                s.driftTo = s.random.nextDouble() * 2.0 - 1.0;
            }
            const double ease = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * s.driftPos);
            const double drift = s.driftFrom + (s.driftTo - s.driftFrom) * ease;

            // The pitch shifter: two taps a half-sweep apart, each fading in and out as it
            // moves, so one is always at full level while the other jumps back.
            const double ratio = std::pow(2.0, shape.detune * smoothedDetune / 1200.0);
            const double step = (1.0 - ratio) / window;   // the delay moves (1 - ratio) samples a sample
            if (std::abs(step) > 1.0e-9)
            {
                s.shifterPhase += step;
                s.shifterPhase -= std::floor(s.shifterPhase);
            }
            else
            {
                // No detune: settle on one tap, rather than leave two copies combing.
                const double target = s.shifterPhase < 0.25 ? 0.0 : s.shifterPhase < 0.75 ? 0.5 : 1.0;
                s.shifterPhase += 0.0005 * (target - s.shifterPhase);
                if (s.shifterPhase >= 1.0)
                    s.shifterPhase -= 1.0;
            }

            const double centre = (smoothedDelayMs * shape.delay + smoothedVariation * kMaxDriftMs * drift) * msToSamples;
            const double phaseB = s.shifterPhase + 0.5 - std::floor(s.shifterPhase + 0.5);
            const double gainA = std::sin(juce::MathConstants<double>::pi * s.shifterPhase);
            const double gainB = std::sin(juce::MathConstants<double>::pi * phaseB);
            const float out = (float) (gainA * gainA * read(centre + (s.shifterPhase - 0.5) * window)
                                       + gainB * gainB * read(centre + (phaseB - 0.5) * window));

            wetL += panL[(size_t) v] * out;
            wetR += panR[(size_t) v] * out;
        }

        if (++writePos >= size)
            writePos = 0;

        wetL = highCutFilter[0].process(lowCutFilter[0].process(wetL, EffectSvf::HighPass), EffectSvf::LowPass);
        wetR = highCutFilter[1].process(lowCutFilter[1].process(wetR, EffectSvf::HighPass), EffectSvf::LowPass);

        const float dl = l[i], dr = r[i];
        if (stereo)
        {
            l[i] = dryGain * dl + wetGain * wetL;
            r[i] = dryGain * dr + wetGain * wetR;
        }
        else
        {
            l[i] = dryGain * dl + wetGain * 0.7071f * (wetL + wetR);
        }
    }
}

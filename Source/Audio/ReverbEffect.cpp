#include "ReverbEffect.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow", "-Wshadow-field-in-constructor", "-Wsign-conversion", "-Wconversion",
                                    "-Wfloat-conversion", "-Wimplicit-float-conversion", "-Wshorten-64-to-32",
                                    "-Wunused-parameter", "-Wfloat-equal", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                    "-Wimplicit-int-conversion", "-Wcast-align", "-Wunused-variable", "-Wdeprecated",
                                    "-Wmissing-field-initializers", "-Wold-style-cast", "-Wunused-private-field",
                                    "-Wreorder-ctor", "-Wsign-compare", "-Wunused-function", "-Wmacro-redefined")
#include "../../ThirdParty/cloudseed/CloudSeed.h"
#include "../../tracktion_engine/modules/tracktion_engine/3rd_party/signalsmith-stretch/signalsmith-stretch.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace P = Cloudseed::Parameter;

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    juce::String msText(float v) { return juce::String(juce::roundToInt(v)) + " ms"; }
    juce::String hzText(float v) { return v >= 1000.0f ? juce::String(v / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz"; }

    // CloudSeed's parameters are 0..1, bent onto real ranges by its "decades" and "octaves"
    // curves; these undo the curves, so 8track's knobs can be in ms, seconds and Hz.
    double fromDecades(double y, int decades)   // inverse of (10^(n x) - 1) / (10^n - 1)
    {
        const double top = std::pow(10.0, decades) - 1.0;
        return juce::jlimit(0.0, 1.0, std::log10(juce::jmax(0.0, y) * top + 1.0) / decades);
    }
    double fromOctaves(double y, int octaves)   // inverse of (2^(n x) - 1) / (2^n - 1)
    {
        const double top = std::pow(2.0, octaves) - 1.0;
        return juce::jlimit(0.0, 1.0, std::log2(juce::jmax(0.0, y) * top + 1.0) / octaves);
    }

    /** The structure of each space: how many delay lines, how much diffusion, the early taps. */
    struct SpaceShape
    {
        int lines, lateStages, earlyStages;
        double lateDelayMs, earlyDelayMs;
        bool taps;
        int tapCount;
        double tapLengthMs;
        bool postDiffuse;   // CloudSeed's "late mode": tap the lines after their diffusers
        double lateOutDb;
    };

    SpaceShape shapeOf(int space)
    {
        switch (space)
        {
            case ReverbEffect::Room:      return { 4, 3, 4, 20, 20, true, 40, 80, false, -3 };
            case ReverbEffect::Hall:      return { 8, 5, 6, 40, 35, true, 60, 250, false, -6 };
            case ReverbEffect::Plate:     return { 1, 4, 1, 34, 10, false, 1, 10, true, -4 };
            case ReverbEffect::Chamber:   return { 6, 4, 5, 28, 25, true, 50, 150, false, -5 };
            case ReverbEffect::Cathedral: return { 12, 7, 8, 70, 60, true, 120, 600, false, -8 };
            case ReverbEffect::Ambience:  return { 3, 1, 6, 15, 15, true, 30, 40, false, -4 };
            default:                      return shapeOf(ReverbEffect::Hall);
        }
    }
}

float ReverbEffect::intervalSemitones(int index)
{
    static const float semis[] = { 12.0f, 7.0f, 5.0f, 19.0f, 24.0f, -12.0f };
    return semis[juce::jlimit(0, 5, index)];
}

//==============================================================================
struct ReverbEffect::Engine
{
    explicit Engine(int rate) : reverb(rate) {}

    Cloudseed::ReverbController reverb;
    double sent[P::COUNT];   // what CloudSeed has, so only changes are sent
    bool sentAny = false;

    signalsmith::stretch::SignalsmithStretch<float> shifter;
    float shiftedBy = 0.0f;

    // The shimmer loop: the tail, a block late, through the shifter and back in.
    juce::AudioBuffer<float> tail;            // ring of the reverb's output
    int tailWrite = 0, tailLag = 512;
    juce::AudioBuffer<float> loopIn, loopOut, revIn, revOut;
    EffectSvf loopHigh[2], loopLow[2];
    int maxBlock = 512;

    void set(int id, double value)
    {
        if (!sentAny || std::abs(sent[id] - value) > 1.0e-9)
        {
            sent[id] = value;
            reverb.SetParameter(id, value);
        }
    }
};

const char* ReverbEffect::xmlTypeName = "eightTrackReverb";

ReverbEffect::ReverbEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    space     = addChoice("space", "Space", spaceNames(), Hall, "Space");
    preDelay  = addKnob("preDelay", "Pre-delay", { 0.0f, 500.0f, 1.0f, 0.5f }, 20.0f, msText, "Space");
    size      = addKnob("size", "Size", { 20.0f, 1000.0f, 1.0f, 0.5f }, 120.0f, msText, "Space");
    decay     = addKnob("decay", "Decay", { 0.1f, 30.0f, 0.01f, 0.4f }, 2.5f,
                        [](float v) { return juce::String(v, v < 10.0f ? 2 : 1) + " s"; }, "Space");
    diffusion = addKnob("diffusion", "Diffusion", { 0.0f, 1.0f, 0.01f }, 0.75f, percentText, "Space");
    early     = addKnob("early", "Early", { 0.0f, 1.0f, 0.01f }, 0.3f, percentText, "Space");

    lowCut    = addKnob("lowCut", "Low Cut", { 20.0f, 1000.0f, 1.0f, 0.4f }, 120.0f, hzText, "Tone");
    highCut   = addKnob("highCut", "High Cut", { 400.0f, 20000.0f, 1.0f, 0.35f }, 9000.0f, hzText, "Tone");
    damping   = addKnob("damping", "Damping", { 0.0f, 1.0f, 0.01f }, 0.4f, percentText, "Tone");
    modDepth  = addKnob("modDepth", "Mod Depth", { 0.0f, 1.0f, 0.01f }, 0.3f, percentText, "Tone");
    modRate   = addKnob("modRate", "Mod Rate", { 0.05f, 5.0f, 0.01f, 0.5f }, 0.4f,
                        [](float v) { return juce::String(v, 2) + " Hz"; }, "Tone");
    width     = addKnob("width", "Width", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Tone");

    shimmer   = addKnob("shimmer", "Shimmer", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "Shimmer");
    interval  = addChoice("interval", "Interval", intervalNames(), 0, "Shimmer");

    mix       = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 0.3f, percentText, "Output");

    // CloudSeed's delay lines are large; allocate them here, not on the audio thread.
    cloud = std::make_unique<Engine>(48000);
}

ReverbEffect::~ReverbEffect() { notifyListenersOfDeletion(); }

double ReverbEffect::getTailLength() const
{
    const double t = decay->getCurrentValue() * (shimmer->getCurrentValue() > 0.0f ? 3.0 : 1.5);
    return juce::jmin(90.0, t + preDelay->getCurrentValue() / 1000.0);
}

bool ReverbEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == interval)
        return shimmer->getCurrentValue() > 0.0f;
    return true;
}

void ReverbEffect::initialise(const te::PluginInitialisationInfo& info)
{
    auto& e = *cloud;
    if (e.reverb.GetSamplerate() != (int) info.sampleRate)
        e.reverb.SetSamplerate((int) info.sampleRate);

    // Fixed parts: seeds (changing them allocates), interpolation, the shelving EQ off.
    e.sentAny = false;
    e.set(P::Interpolation, 1.0);
    e.set(P::SeedTap, 0.334);
    e.set(P::SeedDiffusion, 0.185);
    e.set(P::SeedDelay, 0.218);
    e.set(P::SeedPostDiffusion, 0.365);
    e.set(P::EqCrossSeed, 0.5);
    e.set(P::EqLowShelfEnabled, 0.0);
    e.set(P::EqHighShelfEnabled, 0.0);
    e.set(P::DryOut, 0.0);
    e.set(P::InputMix, 0.25);
    e.set(P::TapDecay, 1.0);
    e.sentAny = true;
    for (int i = 0; i < P::COUNT; ++i)   // mark the rest unsent
        if (i != P::Interpolation && i != P::SeedTap && i != P::SeedDiffusion && i != P::SeedDelay
            && i != P::SeedPostDiffusion && i != P::EqCrossSeed && i != P::EqLowShelfEnabled
            && i != P::EqHighShelfEnabled && i != P::DryOut && i != P::InputMix && i != P::TapDecay)
            e.sent[i] = -1.0;

    e.maxBlock = juce::jlimit(64, BUFFER_SIZE, info.blockSizeSamples);
    e.tailLag = e.maxBlock;
    e.tail.setSize(2, 2 * e.maxBlock);
    for (auto* b : { &e.loopIn, &e.loopOut, &e.revIn, &e.revOut })
        b->setSize(2, e.maxBlock);
    e.shifter.configure(2, (int) (info.sampleRate * 0.05), (int) (info.sampleRate * 0.0125), true);
    e.shiftedBy = 0.0f;
    applySettings();
    reset();
}

void ReverbEffect::reset()
{
    auto& e = *cloud;
    e.reverb.ClearBuffers();
    e.shifter.reset();
    e.tail.clear();
    e.tailWrite = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        e.loopHigh[ch].reset();
        e.loopLow[ch].reset();
    }
}

void ReverbEffect::applySettings()
{
    auto& e = *cloud;
    const auto shape = shapeOf(choiceOf(*space));
    const double diff = diffusion->getCurrentValue();
    const double depth = modDepth->getCurrentValue();
    const double rate = fromDecades(modRate->getCurrentValue() / 5.0, 2);

    e.set(P::LateLineCount, (shape.lines - 1) / 11.0);
    e.set(P::LateDiffuseEnabled, shape.lateStages > 0 ? 1.0 : 0.0);
    e.set(P::LateDiffuseCount, (juce::jmax(1, shape.lateStages) - 1) / 7.0);
    e.set(P::LateDiffuseDelay, (shape.lateDelayMs - 10.0) / 90.0);
    e.set(P::LateDiffuseFeedback, 0.35 + 0.55 * diff);
    e.set(P::LateMode, shape.postDiffuse ? 1.0 : 0.0);
    e.set(P::EarlyDiffuseEnabled, diff > 0.02 ? 1.0 : 0.0);
    e.set(P::EarlyDiffuseCount, (shape.earlyStages - 1) / 11.0);
    e.set(P::EarlyDiffuseDelay, (shape.earlyDelayMs - 10.0) / 90.0);
    e.set(P::EarlyDiffuseFeedback, 0.8 * diff);
    e.set(P::TapEnabled, shape.taps ? 1.0 : 0.0);
    e.set(P::TapCount, (shape.tapCount - 1) / 255.0);
    e.set(P::TapLength, (shape.tapLengthMs - 10.0) / 990.0);

    e.set(P::TapPredelay, fromDecades(preDelay->getCurrentValue() / 500.0, 1));
    e.set(P::LateLineSize, fromDecades((size->getCurrentValue() - 20.0) / 980.0, 2));
    e.set(P::LateLineDecay, fromDecades((decay->getCurrentValue() - 0.05) / 59.95, 3));

    const double earlyAmount = early->getCurrentValue();
    e.set(P::EarlyOut, earlyAmount <= 0.0 ? 0.0 : (1.0 + 20.0 * std::log10(earlyAmount) / 30.0));   // 0..1 = -30..0 dB
    e.set(P::LateOut, 1.0 + shape.lateOutDb / 30.0);

    e.set(P::LowCutEnabled, 1.0);
    e.set(P::LowCut, fromOctaves((lowCut->getCurrentValue() - 20.0) / 980.0, 4));
    e.set(P::HighCutEnabled, 1.0);
    e.set(P::HighCut, fromOctaves((highCut->getCurrentValue() - 400.0) / 19600.0, 4));
    // Damping: the tail darkens as it decays, from no damping (20 kHz) to heavy (1 kHz).
    const double dampHz = 20000.0 * std::pow(1000.0 / 20000.0, (double) damping->getCurrentValue());
    e.set(P::EqLowpassEnabled, 1.0);
    e.set(P::EqCutoff, fromOctaves((dampHz - 400.0) / 19600.0, 4));

    e.set(P::LateLineModAmount, depth);
    e.set(P::LateLineModRate, rate);
    e.set(P::LateDiffuseModAmount, 0.6 * depth);
    e.set(P::LateDiffuseModRate, rate);
    e.set(P::EarlyDiffuseModAmount, 0.5 * depth);
    e.set(P::EarlyDiffuseModRate, rate);
}

void ReverbEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || cloud->tail.getNumSamples() == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& e = *cloud;
    applySettings();

    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* io[2] = { b.getWritePointer(0, fc.bufferStartSample), b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample) };

    const float shimmerAmount = shimmer->getCurrentValue();
    const float semis = intervalSemitones(choiceOf(*interval));
    if (std::abs(semis - e.shiftedBy) > 1.0e-3f)
    {
        e.shifter.setTransposeSemitones(semis);
        e.shiftedBy = semis;
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        e.loopHigh[ch].set(sampleRate, semis < 0.0f ? 120.0 : 250.0, 0.7);
        e.loopLow[ch].set(sampleRate, 7000.0, 0.7);
    }
    const auto [dryGain, wetGain] = mixGains(mix->getCurrentValue());
    const float w = width->getCurrentValue();
    const int ringLength = e.tail.getNumSamples();

    for (int start = 0; start < fc.bufferNumSamples; start += e.maxBlock)
    {
        const int n = juce::jmin(e.maxBlock, fc.bufferNumSamples - start);

        // The shimmer: the tail from a block ago, shifted, filtered, softly limited.
        if (shimmerAmount > 0.0f)
        {
            int read = (e.tailWrite - e.tailLag + ringLength) % ringLength;
            for (int i = 0; i < n; ++i)
            {
                e.loopIn.setSample(0, i, e.tail.getSample(0, read));
                e.loopIn.setSample(1, i, e.tail.getSample(1, read));
                if (++read >= ringLength)
                    read = 0;
            }
            float* in[2] = { e.loopIn.getWritePointer(0), e.loopIn.getWritePointer(1) };
            float* out[2] = { e.loopOut.getWritePointer(0), e.loopOut.getWritePointer(1) };
            e.shifter.process(in, n, out, n);
        }
        // The shimmer's gain: up to just under unity round the loop, with tanh keeping it bounded.
        const float loopGain = 0.95f * shimmerAmount;
        for (int ch = 0; ch < 2; ++ch)
        {
            float* rin = e.revIn.getWritePointer(ch);
            const float* fb = e.loopOut.getReadPointer(ch);
            for (int i = 0; i < n; ++i)
            {
                float s = 0.0f;
                if (shimmerAmount > 0.0f)
                {
                    s = e.loopHigh[ch].process(fb[i], EffectSvf::HighPass);
                    s = e.loopLow[ch].process(s, EffectSvf::LowPass);
                    s = std::tanh(loopGain * s);
                }
                rin[i] = io[ch][start + i] + s;
            }
        }

        e.reverb.Process(e.revIn.getWritePointer(0), e.revIn.getWritePointer(1),
                         e.revOut.getWritePointer(0), e.revOut.getWritePointer(1), n);

        for (int i = 0; i < n; ++i)
        {
            float l = e.revOut.getSample(0, i), r = e.revOut.getSample(1, i);
            e.tail.setSample(0, e.tailWrite, l);
            e.tail.setSample(1, e.tailWrite, r);
            if (++e.tailWrite >= ringLength)
                e.tailWrite = 0;

            // Width: mid/side, from mono (0) to as recorded (1)
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * w;
            l = mid + side;
            r = mid - side;
            if (stereo)
            {
                io[0][start + i] = dryGain * io[0][start + i] + wetGain * l;
                io[1][start + i] = dryGain * io[1][start + i] + wetGain * r;
            }
            else
            {
                io[0][start + i] = dryGain * io[0][start + i] + wetGain * mid;
            }
        }
    }
}

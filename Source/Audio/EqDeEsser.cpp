#include "EqDeEsser.h"
#include <complex>

namespace
{
    juce::String dbText(float v) { return (v > 0.0f ? "+" : "") + juce::String(v, 1) + " dB"; }

    juce::String hzText(float v)
    {
        return v >= 1000.0f ? juce::String(v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String(juce::roundToInt(v)) + " Hz";
    }

    juce::String msText(float v) { return juce::String(v, v < 10.0f ? 1 : 0) + " ms"; }

    juce::NormalisableRange<float> skewed(float lo, float hi, float centre, float interval = 0.0f)
    {
        juce::NormalisableRange<float> r(lo, hi, interval);
        r.setSkewForCentre(centre);
        return r;
    }

    float coefficient(double ms, double sampleRate) { return (float) std::exp(-1.0 / juce::jmax(1.0, ms * 0.001 * sampleRate)); }
}

//==============================================================================
// EQ
const char* EqEffect::xmlTypeName = "eightTrackEq";

EqEffect::EqEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    struct Default { Type type; float hz, q; bool on; };
    static constexpr Default defaults[kBands] {
        { LowCut, 80.0f, 0.707f, false },
        { LowShelf, 200.0f, 0.707f, true },
        { Bell, 800.0f, 1.0f, true },
        { Bell, 3000.0f, 1.0f, true },
        { HighShelf, 10000.0f, 0.707f, true },
    };

    for (int i = 0; i < kBands; ++i)
    {
        const auto id = "band" + juce::String(i + 1);
        const auto name = "Band " + juce::String(i + 1) + " ";
        const auto row = rowFor(i);
        const auto& d = defaults[i];
        auto& b = bands[(size_t) i];
        b.on   = addToggle(id + "On", name + "On", d.on, row);
        b.type = addChoice(id + "Type", name + "Type", typeNames(), (int) d.type, row, 2);
        b.freq = addKnob(id + "Freq", name + "Freq", skewed(kMinHz, kMaxHz, 632.0f), d.hz, hzText, row);
        b.gain = addKnob(id + "Gain", name + "Gain", { -kMaxGainDb, kMaxGainDb, 0.1f }, 0.0f, dbText, row);
        b.q    = addKnob(id + "Q", name + "Q", skewed(0.1f, 18.0f, 1.0f, 0.01f), d.q,
                         [](float v) { return juce::String(v, 2); }, row);
        setLabel(b.on, "On");
        setLabel(b.type, "Type");
        setLabel(b.freq, "Freq");
        setLabel(b.gain, "Gain");
        setLabel(b.q, "Q");
    }
    output = addKnob("output", "Output", { -kMaxGainDb, kMaxGainDb, 0.1f }, 0.0f, dbText, "Output");
}

EqEffect::~EqEffect() { notifyListenersOfDeletion(); }

bool EqEffect::isControlActive(const te::AutomatableParameter& p) const
{
    for (auto& b : bands)
    {
        if (&p == b.on)
            return true;
        if (&p == b.type || &p == b.freq || &p == b.gain || &p == b.q)
            return b.isOn() && (&p != b.gain || hasGain(b.getType()));
    }
    return true;
}

void EqEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void EqEffect::reset()
{
    for (auto& f : filters)
        for (int ch = 0; ch < 2; ++ch)
            f.z1[ch] = f.z2[ch] = 0.0;
    applied = {};
}

EqEffect::Coefficients EqEffect::design(Type type, double fs, double hz, double gainDb, double q)
{
    const double w0 = juce::MathConstants<double>::twoPi * juce::jlimit(10.0, fs * 0.49, hz) / fs;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * juce::jmax(0.05, q));
    const double A = std::pow(10.0, gainDb / 40.0), sa = 2.0 * std::sqrt(A) * alpha;

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type)
    {
        case LowCut:
            b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = b0;
            a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            break;
        case HighCut:
            b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw; b2 = b0;
            a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            break;
        case Bell:
            b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
            break;
        case Notch:
            b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0;
            a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            break;
        case LowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + sa);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - sa);
            a0 = (A + 1.0) + (A - 1.0) * cw + sa;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - sa;
            break;
        case HighShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + sa);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - sa);
            a0 = (A + 1.0) - (A - 1.0) * cw + sa;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - sa;
            break;
    }
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

double EqEffect::Coefficients::magnitudeDb(double hz, double fs) const
{
    const double w = juce::MathConstants<double>::twoPi * hz / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    return 20.0 * std::log10(juce::jmax(1.0e-9, std::abs(h)));
}

double EqEffect::bandResponseDb(int index, double hz) const
{
    const auto& b = bands[(size_t) index];
    if (!b.isOn())
        return 0.0;
    return design(b.getType(), rate(), b.freq->getCurrentValue(), b.gain->getCurrentValue(), b.q->getCurrentValue())
        .magnitudeDb(hz, rate());
}

double EqEffect::responseDb(double hz) const
{
    double db = output->getCurrentValue();
    for (int i = 0; i < kBands; ++i)
        db += bandResponseDb(i, hz);
    return db;
}

void EqEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& buf = *fc.destBuffer;
    const int n = fc.bufferNumSamples;
    const int channels = juce::jmin(2, buf.getNumChannels());
    float* io[2] = { buf.getWritePointer(0, fc.bufferStartSample), buf.getWritePointer(channels - 1, fc.bufferStartSample) };
    if (before.active.load(std::memory_order_relaxed))
        before.push(io[0], io[1], n);

    for (int i = 0; i < kBands; ++i)
    {
        const auto& b = bands[(size_t) i];
        const Settings now { b.isOn(), (int) b.getType(), b.freq->getCurrentValue(), b.gain->getCurrentValue(), b.q->getCurrentValue() };
        auto& f = filters[(size_t) i];
        if (now != applied[(size_t) i])
        {
            if (now.on && !applied[(size_t) i].on)   // coming back on: start from silence, not old state
                for (int ch = 0; ch < 2; ++ch)
                    f.z1[ch] = f.z2[ch] = 0.0;
            applied[(size_t) i] = now;
            if (now.on)
                f.c = design((Type) now.type, rate(), now.freq, now.gain, now.q);
        }
        if (!now.on)
            continue;
        for (int ch = 0; ch < channels; ++ch)
            for (int s = 0; s < n; ++s)
                io[ch][s] = f.process(io[ch][s], ch);
    }

    // Output gain, ramped so moving it doesn't click
    const float target = juce::Decibels::decibelsToGain(output->getCurrentValue());
    if (std::abs(target - outputGain) > 1.0e-6f)
    {
        for (int ch = 0; ch < channels; ++ch)
            buf.applyGainRamp(ch, fc.bufferStartSample, n, outputGain, target);
        outputGain = target;
    }
    else if (std::abs(outputGain - 1.0f) > 1.0e-6f)
    {
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::multiply(io[ch], outputGain, n);
    }

    if (after.active.load(std::memory_order_relaxed))
        after.push(io[0], io[1], n);
}

//==============================================================================
// De-Esser
const char* DeEsserEffect::xmlTypeName = "eightTrackDeEsser";

DeEsserEffect::DeEsserEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    frequency = addKnob("frequency", "Frequency", skewed(2000.0f, 16000.0f, 6000.0f, 10.0f), 6000.0f, hzText, "Detect");
    threshold = addKnob("threshold", "Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f,
                        [](float v) { return juce::String(v, 1) + " dB"; }, "Detect");
    detect    = addChoice("detect", "Detect", { "High Pass", "Band Pass" }, HighPass, "Detect");
    listen    = addToggle("listen", "Listen", false, "Detect");
    range     = addKnob("range", "Range", { 0.0f, 24.0f, 0.1f }, 10.0f,
                        [](float v) { return juce::String(v, 1) + " dB"; }, "Reduce");
    release   = addKnob("release", "Release", skewed(10.0f, 300.0f, 60.0f, 1.0f), 60.0f, msText, "Reduce");
    mode      = addChoice("mode", "Mode", { "Split", "Wide" }, Split, "Reduce");
}

DeEsserEffect::~DeEsserEffect() { notifyListenersOfDeletion(); }

void DeEsserEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void DeEsserEffect::reset()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        detector[ch].reset();
        for (auto& f : splitter[ch])
            f.reset();
    }
    envelope = 0.0f;
    reduction = 0.0f;
}

void DeEsserEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& buf = *fc.destBuffer;
    const int channels = juce::jmin(2, buf.getNumChannels());
    float* io[2] = { buf.getWritePointer(0, fc.bufferStartSample), buf.getWritePointer(channels - 1, fc.bufferStartSample) };

    const double hz = frequency->getCurrentValue();
    const bool bandPass = choiceOf(*detect) == BandPass;
    for (int ch = 0; ch < 2; ++ch)
    {
        detector[ch].set(sampleRate, hz, bandPass ? 2.0 : 0.707);
        for (auto& f : splitter[ch])
            f.set(sampleRate, hz, juce::MathConstants<double>::sqrt2 * 0.5);
    }
    const auto detectMode = bandPass ? EffectSvf::BandPass : EffectSvf::HighPass;
    const float thr = threshold->getCurrentValue();
    const float maxDb = range->getCurrentValue();
    const float attackCoef = coefficient(0.3, sampleRate);
    const float releaseCoef = coefficient(release->getCurrentValue(), sampleRate);
    const bool wide = choiceOf(*mode) == Wide;
    const bool listening = isOn(*listen);
    constexpr float slope = 0.8f;   // 5:1 above the threshold
    float most = 0.0f;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        float side[2] {}, level = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            side[ch] = detector[ch].process(io[ch][i], detectMode);
            level = juce::jmax(level, std::abs(side[ch]));
        }
        envelope = level + (envelope - level) * (level > envelope ? attackCoef : releaseCoef);
        const float over = juce::Decibels::gainToDecibels(envelope, -120.0f) - thr;
        reduction = juce::jlimit(0.0f, maxDb, over * slope);
        most = juce::jmax(most, reduction);
        const float g = juce::Decibels::decibelsToGain(-reduction);

        for (int ch = 0; ch < channels; ++ch)
        {
            const float x = io[ch][i];
            if (listening)
                io[ch][i] = side[ch];
            else if (wide)
                io[ch][i] = x * g;
            else
            {
                // A Linkwitz-Riley crossover: the lows and highs add back up flat, so only the
                // highs move when g does
                auto& f = splitter[ch];
                const float lows = f[1].process(f[0].process(x, EffectSvf::LowPass), EffectSvf::LowPass);
                const float highs = f[3].process(f[2].process(x, EffectSvf::HighPass), EffectSvf::HighPass);
                io[ch][i] = lows + highs * g;
            }
        }
    }
    reductionNow.store(most, std::memory_order_relaxed);
}

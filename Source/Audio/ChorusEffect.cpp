#include "ChorusEffect.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow", "-Wshadow-field-in-constructor", "-Wsign-conversion", "-Wconversion",
                                    "-Wfloat-conversion", "-Wimplicit-float-conversion", "-Wshorten-64-to-32",
                                    "-Wunused-parameter", "-Wfloat-equal", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                    "-Wimplicit-int-conversion", "-Wcast-align", "-Wunused-variable", "-Wdeprecated",
                                    "-Wmissing-field-initializers", "-Wold-style-cast", "-Wunused-private-field")
#include "../../ThirdParty/kr106/BBDFilter.h"
#include "../../ThirdParty/kr106/KR106Chorus.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    double triangle(double phase) { return phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase; }   // -1..1
}

/** One bucket-brigade line: band-limited going in and coming out, a little soft at the top. */
struct ChorusEffect::Bbd
{
    static constexpr double kMaxSeconds = 0.03;
    std::vector<float> line;
    int write = 0;
    BBDFilter pre, post;

    void init(double sampleRate)
    {
        line.assign((size_t) std::ceil(kMaxSeconds * sampleRate) + 4, 0.0f);
        write = 0;
        pre.Init((float) sampleRate);
        post.Init((float) sampleRate);
        clear();
    }

    void clear()
    {
        std::fill(line.begin(), line.end(), 0.0f);
        pre.Reset();
        post.Reset();
    }

    float process(float input, double delaySamples)
    {
        const int size = (int) line.size();
        line[(size_t) write] = std::tanh(pre.Process(input));   // the BBD's input stage clips softly
        const double pos = write - juce::jlimit(2.0, (double) size - 3.0, delaySamples);
        const int i1 = (int) std::floor(pos);
        const float t = (float) (pos - i1);
        auto at = [&](int i) { return line[(size_t) (((i % size) + size) % size)]; };
        const float y0 = at(i1 - 1), y1 = at(i1), y2 = at(i1 + 1), y3 = at(i1 + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        if (++write >= size)
            write = 0;
        return post.Process(((c3 * t + c2) * t + c1) * t + y1);
    }
};

struct ChorusEffect::Juno
{
    kr106::Chorus chorus;
};

const char* ChorusEffect::xmlTypeName = "eightTrackChorus";

ChorusEffect::ChorusEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    mode  = addChoice("mode", "Mode", modeNames(), JunoI, "Chorus");
    rate  = addKnob("rate", "Rate", { 0.1f, 8.0f, 0.01f }, 0.8f,
                    [](float v) { return juce::String(v, v < 1.0f ? 2 : 1) + " Hz"; }, "Chorus");
    depth = addKnob("depth", "Depth", { 0.0f, 1.0f, 0.01f }, 0.5f, percentText, "Chorus");
    hiss  = addKnob("hiss", "Hiss", { 0.0f, 1.0f, 0.01f }, 0.25f, percentText, "Chorus");
    mix   = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Output");
    lineA = std::make_unique<Bbd>();
    lineB = std::make_unique<Bbd>();
    juno = std::make_unique<Juno>();
}

ChorusEffect::~ChorusEffect() { notifyListenersOfDeletion(); }

bool ChorusEffect::isControlActive(const te::AutomatableParameter& p) const
{
    const int m = choiceOf(*mode);
    if (&p == rate || &p == depth) return m == CE2;
    if (&p == hiss) return m >= JunoI && m <= JunoI_II;
    return true;
}

void ChorusEffect::initialise(const te::PluginInitialisationInfo& info)
{
    lineA->init(info.sampleRate);
    lineB->init(info.sampleRate);
    juno->chorus.Init((float) info.sampleRate);
    junoMode = -1;
    reset();
}

void ChorusEffect::reset()
{
    lineA->clear();
    lineB->clear();
    juno->chorus.Clear();
    lfoPhase = 0;
}

void ChorusEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || lineA->line.empty())
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* l = b.getWritePointer(0, fc.bufferStartSample);
    float* r = b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample);

    const int m = choiceOf(*mode);
    const float wet = mix->getCurrentValue(), dry = 1.0f - wet;
    const double msToSamples = sampleRate / 1000.0;

    if (m >= JunoI && m <= JunoI_II)
    {
        const int want = m - JunoI + 1;   // KR-106: 1 = I, 2 = II, 3 = I+II
        if (want != junoMode)
        {
            juno->chorus.SetMode(want);
            junoMode = want;
        }
        juno->chorus.mClockMul = hiss->getCurrentValue();
        juno->chorus.mAnalogMul = hiss->getCurrentValue();
        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            const float in = 0.5f * (l[i] + r[i]);
            float outL = 0.0f, outR = 0.0f;
            juno->chorus.Process(in, outL, outR);
            // The Juno sums dry and wet at about +6 dB; bring it back to the input's level.
            outL *= 0.5f;
            outR *= 0.5f;
            const float dl = l[i], dr = r[i];
            l[i] = dry * dl + wet * outL;
            if (stereo)
                r[i] = dry * dr + wet * outR;
        }
        return;
    }

    junoMode = -1;
    double hz, centreMs, swingMs;
    if (m == CE2)
    {
        hz = rate->getCurrentValue();
        centreMs = 6.0;
        swingMs = 0.3 + 3.2 * depth->getCurrentValue();
    }
    else
    {
        const int button = m - Dimension1;   // 0..3: deeper, and the fourth faster
        hz = button == 3 ? 0.5 : 0.25;
        centreMs = 5.0;
        swingMs = 0.25 * (button + 1);
    }
    const double inc = hz / sampleRate;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        const double lfo = triangle(lfoPhase);
        lfoPhase += inc;
        if (lfoPhase >= 1.0)
            lfoPhase -= 1.0;
        const float in = 0.5f * (l[i] + r[i]);
        const float dl = l[i], dr = r[i];
        float outL, outR;
        if (m == CE2)
        {
            const float w = lineA->process(in, (centreMs + swingMs * lfo) * msToSamples);
            outL = outR = 0.5f * (in + w);   // the pedal mixes dry and wet equally
        }
        else
        {
            // Two lines swept in opposite directions, each side getting one minus some of the
            // other: in mono the sweeps cancel, in stereo they spread.
            const float a = lineA->process(in, (centreMs + swingMs * lfo) * msToSamples);
            const float c = lineB->process(in, (centreMs - swingMs * lfo) * msToSamples);
            outL = 0.6f * in + 0.5f * a - 0.25f * c;
            outR = 0.6f * in + 0.5f * c - 0.25f * a;
        }
        l[i] = dry * dl + wet * outL;
        if (stereo)
            r[i] = dry * dr + wet * outR;
    }
}

#include "Effects.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow", "-Wshadow-field-in-constructor", "-Wsign-conversion", "-Wconversion",
                                    "-Wfloat-conversion", "-Wimplicit-float-conversion", "-Wshorten-64-to-32",
                                    "-Wunused-parameter", "-Wfloat-equal", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                    "-Wimplicit-int-conversion", "-Wcast-align", "-Wunused-variable", "-Wdeprecated")
#include "../../tracktion_engine/modules/tracktion_engine/3rd_party/signalsmith-stretch/signalsmith-stretch.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }

    juce::String hzText(float v)
    {
        return v >= 1000.0f ? juce::String(v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String(juce::roundToInt(v)) + " Hz";
    }

    juce::String msText(float v)
    {
        if (v >= 1000.0f) return juce::String(v / 1000.0f, 2) + " s";
        return juce::String(v, v < 10.0f ? 1 : 0) + " ms";
    }

    juce::String dbText(float v) { return juce::String(v, 1) + " dB"; }


    juce::NormalisableRange<float> skewed(float lo, float hi, float centre, float interval = 0.0f)
    {
        juce::NormalisableRange<float> r(lo, hi, interval);
        r.setSkewForCentre(centre);
        return r;
    }

    float coefficient(double ms, double sampleRate)
    {
        return (float) std::exp(-1.0 / juce::jmax(1.0, ms * 0.001 * sampleRate));
    }

    double frac(double x) { return x - std::floor(x); }

    int indexOfDivision(const char* name) { return juce::jmax(0, EffectPlugin::divisionNames().indexOf(name)); }

    // Channel pointers for a block: a mono buffer feeds both sides of a stereo effect.
    struct Channels
    {
        float* l = nullptr;
        float* r = nullptr;
        bool stereo = false;
        explicit Channels(const te::PluginRenderContext& fc)
        {
            auto& b = *fc.destBuffer;
            l = b.getWritePointer(0, fc.bufferStartSample);
            stereo = b.getNumChannels() > 1;
            r = stereo ? b.getWritePointer(1, fc.bufferStartSample) : l;
        }
    };
}

//==============================================================================
EffectPlugin::EffectPlugin(te::PluginCreationInfo info) : te::Plugin(info)
{
    if (auto* tempo = edit.tempoSequence.getTempo(0))
        setTempo(tempo->getBpm());
}

EffectPlugin::~EffectPlugin()
{
    for (auto& c : controls)
        c.param->detachFromCurrentValue();
}

te::AutomatableParameter* EffectPlugin::addKnob(const juce::String& id, const juce::String& name,
                                                juce::NormalisableRange<float> range, float defaultValue,
                                                std::function<juce::String(float)> text, const juce::String& row)
{
    Control c;
    c.value = std::make_unique<juce::CachedValue<float>>();
    c.value->referTo(state, juce::Identifier(id), getUndoManager(), defaultValue);
    c.param = addParam(id, name, range, std::move(text), [](const juce::String& s) { return s.getFloatValue(); });
    c.param->attachToCurrentValue(*c.value);
    c.row = row;
    controls.push_back(std::move(c));
    return controls.back().param;
}

te::AutomatableParameter* EffectPlugin::addChoice(const juce::String& id, const juce::String& name,
                                                  const juce::StringArray& choices, int defaultIndex, const juce::String& row)
{
    auto* p = addKnob(id, name, { 0.0f, (float) (choices.size() - 1), 1.0f }, (float) defaultIndex,
                      [choices](float v) { return choices[juce::jlimit(0, choices.size() - 1, juce::roundToInt(v))]; }, row);
    p->stringToValueFunction = [choices](const juce::String& s) { return (float) juce::jmax(0, choices.indexOf(s)); };
    controls.back().kind = Control::Kind::Choice;
    controls.back().choices = choices;
    return p;
}

te::AutomatableParameter* EffectPlugin::addToggle(const juce::String& id, const juce::String& name, bool defaultOn,
                                                  const juce::String& row)
{
    auto* p = addChoice(id, name, { "Off", "On" }, defaultOn ? 1 : 0, row);
    controls.back().kind = Control::Kind::Toggle;
    return p;
}

void EffectPlugin::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    for (auto& c : controls)
    {
        if (auto* p = v.getPropertyPointer(c.value->getPropertyID()))
            *c.value = (float) *p;
        else
            c.value->resetToDefault();
    }
    for (auto* p : getAutomatableParameters())
        p->updateFromAttachedValue();
}

juce::String EffectPlugin::signedText(float v, int decimals, const char* unit)
{
    return (v > 0.0f ? "+" : "") + juce::String(v, decimals) + unit;
}

juce::File EffectPlugin::getFileProperty(const juce::Identifier& id) const
{
    const auto path = state[id].toString();
    return path.isEmpty() ? juce::File() : te::SourceFileReference::findFileFromString(edit, path);
}

void EffectPlugin::setFileProperty(const juce::Identifier& id, const juce::File& file)
{
    state.setProperty(id, file == juce::File() ? juce::String() : te::SourceFileReference::findPathFromFile(edit, file, true),
                      getUndoManager());
}

juce::File EffectPlugin::adoptFile(const juce::File& file) const
{
    const auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File();
    if (editFile == juce::File() || !file.existsAsFile())
        return file;
    const auto folder = editFile.getParentDirectory();
    if (file.isAChildOf(folder))
        return file;
    auto audio = folder.getChildFile("Audio");
    audio.createDirectory();
    const auto copy = audio.getNonexistentChildFile(file.getFileNameWithoutExtension(), file.getFileExtension(), false);
    return file.copyFileTo(copy) ? copy : file;
}

std::optional<double> EffectPlugin::getBeatPosition(const te::PluginRenderContext& fc) const
{
    if (!fc.isPlaying)
        return {};
    return fc.editTime.getStart().inSeconds() * getTempo() / 60.0;
}

const juce::StringArray& EffectPlugin::divisionNames()
{
    static const juce::StringArray names { "4/1", "2/1", "1/1", "1/2 D", "1/2", "1/2 T", "1/4 D", "1/4", "1/4 T",
                                           "1/8 D", "1/8", "1/8 T", "1/16 D", "1/16", "1/16 T", "1/32" };
    return names;
}

double EffectPlugin::divisionBeats(int index)
{
    static constexpr double beats[] { 16.0, 8.0, 4.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0,
                                      0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0, 0.125 };
    return beats[juce::jlimit(0, (int) std::size(beats) - 1, index)];
}

//==============================================================================
double EffectLfo::startBlock(double sampleRate, bool synced, double hz, double bpm, double divisionBeats,
                             std::optional<double> beatPosition)
{
    if (!synced)
        return hz / sampleRate;

    if (beatPosition.has_value())
    {
        const double locked = frac(*beatPosition / divisionBeats);
        if (locked < phase - 0.5)
            nextHeld = random.nextFloat() * 2.0f - 1.0f, held = nextHeld;
        phase = locked;
    }
    return bpm / 60.0 / sampleRate / divisionBeats;
}

float EffectLfo::valueAt(double offset, Wave wave) const
{
    const double p = frac(phase + offset);
    switch (wave)
    {
        case Sine:     return (float) std::sin(juce::MathConstants<double>::twoPi * p);
        case Triangle: return (float) (p < 0.25 ? 4.0 * p : p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
        case Square:   return p < 0.5 ? 1.0f : -1.0f;
        case SawUp:    return (float) (2.0 * p - 1.0);
        case SawDown:  return (float) (1.0 - 2.0 * p);
        case Random:   return held;
    }
    return 0.0f;
}

void EffectLfo::advance(double amount)
{
    phase += amount;
    if (phase >= 1.0)
    {
        phase = frac(phase);
        held = random.nextFloat() * 2.0f - 1.0f;
    }
}

float EffectLfo::next(double increment, Wave wave, double offset)
{
    const float v = valueAt(offset, wave);
    advance(increment);
    return v;
}

//==============================================================================
void EffectSvf::set(double sampleRate, double hz, double q)
{
    const double g = std::tan(juce::MathConstants<double>::pi * juce::jlimit(5.0, sampleRate * 0.49, hz) / sampleRate);
    k = (float) (1.0 / q);
    a1 = (float) (1.0 / (1.0 + g * (g + k)));
    a2 = (float) (g * a1);
    a3 = (float) (g * a2);
}

float EffectSvf::process(float x, Mode mode)
{
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    switch (mode)
    {
        case LowPass:  return v2;
        case HighPass: return x - k * v1 - v2;
        case BandPass: return k * v1;
        case Notch:    return x - k * v1;
    }
    return x;
}

//==============================================================================
// Noise Gate
const char* GateEffect::xmlTypeName = "eightTrackGate";

GateEffect::GateEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    threshold  = addKnob("threshold", "Threshold", { -80.0f, 0.0f, 0.5f }, -40.0f, dbText, "Detect");
    hysteresis = addKnob("hysteresis", "Hysteresis", { 0.0f, 12.0f, 0.5f }, 4.0f, dbText, "Detect");
    keyFilter  = addKnob("keyFilter", "Key Filter", skewed(20.0f, 2000.0f, 200.0f, 1.0f), 20.0f,
                         [](float v) { return v < 20.5f ? juce::String("Off") : hzText(v); }, "Detect");
    attack     = addKnob("attack", "Attack", skewed(0.1f, 50.0f, 5.0f), 0.5f, msText, "Shape");
    hold       = addKnob("hold", "Hold", skewed(0.0f, 500.0f, 60.0f, 1.0f), 30.0f, msText, "Shape");
    release    = addKnob("release", "Release", skewed(5.0f, 2000.0f, 200.0f, 1.0f), 120.0f, msText, "Shape");
    range      = addKnob("floor", "Floor", { -80.0f, 0.0f, 0.5f }, -80.0f,
                         [](float v) { return v <= -79.9f ? juce::String("-inf") : dbText(v); }, "Shape");
}

GateEffect::~GateEffect() { notifyListenersOfDeletion(); }

void GateEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void GateEffect::reset()
{
    for (auto& f : key)
        f.reset();
    envelope = 0.0f;
    gain = 0.0f;   // starts closed, so playback doesn't begin with a burst of noise
    open = false;
    holdLeft = 0;
}

void GateEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;

    auto& b = *fc.destBuffer;
    const int channels = juce::jmin(2, b.getNumChannels());
    const float openLevel = juce::Decibels::decibelsToGain(threshold->getCurrentValue());
    const float closeLevel = juce::Decibels::decibelsToGain(threshold->getCurrentValue() - hysteresis->getCurrentValue());
    const float floorGain = range->getCurrentValue() <= -79.9f ? 0.0f : juce::Decibels::decibelsToGain(range->getCurrentValue());
    const bool filtered = keyFilter->getCurrentValue() > 20.5f;
    for (auto& f : key)
        f.set(sampleRate, keyFilter->getCurrentValue(), 0.707);

    const float envelopeFall = coefficient(10.0, sampleRate);
    const float attackCoef = coefficient(attack->getCurrentValue(), sampleRate);
    const float releaseCoef = coefficient(release->getCurrentValue() * 0.5, sampleRate);
    const int holdSamples = (int) (hold->getCurrentValue() * 0.001 * sampleRate);

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        float level = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            float x = b.getSample(ch, fc.bufferStartSample + i);
            if (filtered)
                x = key[ch].process(x, EffectSvf::HighPass);
            level = juce::jmax(level, std::abs(x));
        }
        envelope = level > envelope ? level : level + (envelope - level) * envelopeFall;

        if (envelope >= openLevel)
        {
            open = true;
            holdLeft = holdSamples;
        }
        else if (open)
        {
            if (envelope >= closeLevel)
                holdLeft = holdSamples;
            else if (holdLeft > 0)
                --holdLeft;
            else
                open = false;
        }

        const float target = open ? 1.0f : floorGain;
        gain = target + (gain - target) * (target > gain ? attackCoef : releaseCoef);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.getWritePointer(ch, fc.bufferStartSample)[i] *= gain;
    }
    gainNow.store(gain, std::memory_order_relaxed);
}

//==============================================================================
// Delay
const char* DelayEffect::xmlTypeName = "eightTrackDelay";

DelayEffect::DelayEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    sync     = addToggle("sync", "Sync", true, "Time");
    time     = addKnob("time", "Time", skewed(1.0f, 2000.0f, 250.0f, 1.0f), 350.0f, msText, "Time");
    division = addChoice("division", "Division", divisionNames(), indexOfDivision("1/4"), "Time");
    feedback = addKnob("feedback", "Feedback", { 0.0f, 0.95f, 0.01f }, 0.35f, percentText, "Repeats");
    lowCut   = addKnob("lowCut", "Low Cut", skewed(20.0f, 2000.0f, 200.0f, 1.0f), 100.0f,
                       [](float v) { return v < 20.5f ? juce::String("Off") : hzText(v); }, "Repeats");
    highCut  = addKnob("highCut", "High Cut", skewed(1000.0f, 20000.0f, 5000.0f, 10.0f), 8000.0f,
                       [](float v) { return v > 19990.0f ? juce::String("Off") : hzText(v); }, "Repeats");
    pingPong = addToggle("pingPong", "Ping-Pong", false, "Repeats");
    mix      = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 0.3f, percentText, "Output");
}

DelayEffect::~DelayEffect() { notifyListenersOfDeletion(); }

bool DelayEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == time) return !isOn(*sync);
    if (&p == division) return isOn(*sync);
    return true;
}

double DelayEffect::getDelaySeconds() const
{
    const double seconds = isOn(*sync) ? divisionBeats(choiceOf(*division)) * 60.0 / getTempo()
                                       : time->getCurrentValue() / 1000.0;
    return juce::jlimit(0.001, kMaxSeconds, seconds);
}

double DelayEffect::getTailLength() const
{
    const double delay = getDelaySeconds();
    const double fb = feedback->getCurrentValue();
    if (fb < 0.01)
        return delay;
    const double repeats = std::log(0.001) / std::log(fb);   // until the echoes are 60 dB down
    return juce::jmin(30.0, delay * (repeats + 1.0));
}

void DelayEffect::initialise(const te::PluginInitialisationInfo& info)
{
    buffer.setSize(2, (int) std::ceil(kMaxSeconds * info.sampleRate) + 8);
    reset();
}

void DelayEffect::deinitialise() { buffer.setSize(0, 0); }

void DelayEffect::reset()
{
    buffer.clear();
    writePos = 0;
    smoothedDelay = -1;
    for (auto* filters : { lowCutFilter, highCutFilter })
        for (int ch = 0; ch < 2; ++ch)
            filters[ch].reset();
}

void DelayEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || buffer.getNumSamples() == 0)
        return;
    juce::ScopedNoDenormals noDenormals;

    const int length = buffer.getNumSamples();
    const double target = juce::jmin((double) length - 4.0, getDelaySeconds() * sampleRate);
    if (smoothedDelay < 0)
        smoothedDelay = target;
    const double glide = std::exp(-1.0 / (0.08 * sampleRate));   // time changes glide, like tape

    const float fb = feedback->getCurrentValue();
    const bool useLowCut = lowCut->getCurrentValue() > 20.5f, useHighCut = highCut->getCurrentValue() < 19990.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        lowCutFilter[ch].set(sampleRate, lowCut->getCurrentValue(), 0.707);
        highCutFilter[ch].set(sampleRate, highCut->getCurrentValue(), 0.707);
    }
    const auto [dry, wet] = mixGains(mix->getCurrentValue());

    Channels io(fc);
    const bool cross = isOn(*pingPong) && io.stereo;
    float* lines[2] = { buffer.getWritePointer(0), buffer.getWritePointer(1) };

    auto read = [&](int ch, double pos)
    {
        // 4-point Hermite interpolation
        const int i1 = (int) std::floor(pos);
        const float t = (float) (pos - i1);
        auto at = [&](int i) { return lines[ch][((i % length) + length) % length]; };
        const float y0 = at(i1 - 1), y1 = at(i1), y2 = at(i1 + 1), y3 = at(i1 + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    };

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        smoothedDelay = target + (smoothedDelay - target) * glide;
        const double pos = writePos - smoothedDelay;

        float echo[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            float e = read(ch, pos);
            if (useLowCut) e = lowCutFilter[ch].process(e, EffectSvf::HighPass);
            if (useHighCut) e = highCutFilter[ch].process(e, EffectSvf::LowPass);
            echo[ch] = e;
        }

        const float inL = io.l[i], inR = io.r[i];
        if (cross)
        {
            lines[0][writePos] = 0.5f * (inL + inR) + fb * echo[1];
            lines[1][writePos] = fb * echo[0];
        }
        else
        {
            lines[0][writePos] = inL + fb * echo[0];
            lines[1][writePos] = inR + fb * echo[1];
        }
        if (++writePos >= length)
            writePos = 0;

        io.l[i] = dry * inL + wet * echo[0];
        if (io.stereo)
            io.r[i] = dry * inR + wet * echo[1];
    }
}

//==============================================================================
// Auto Pan
const char* AutoPanEffect::xmlTypeName = "eightTrackAutoPan";

AutoPanEffect::AutoPanEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    sync     = addToggle("sync", "Sync", true, "Rate");
    rate     = addKnob("rate", "Rate", skewed(0.05f, 20.0f, 1.0f, 0.01f), 1.0f,
                       [](float v) { return juce::String(v, v < 1.0f ? 2 : 1) + " Hz"; }, "Rate");
    division = addChoice("division", "Division", divisionNames(), indexOfDivision("1/2"), "Rate");
    wave     = addChoice("wave", "Wave", EffectLfo::waveNames(), EffectLfo::Sine, "Shape");
    depth    = addKnob("depth", "Depth", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Shape");
    phase    = addKnob("phase", "Phase", { 0.0f, 360.0f, 1.0f }, 0.0f,
                       [](float v) { return juce::String(juce::roundToInt(v)) + juce::String::fromUTF8("°"); }, "Shape");
}

AutoPanEffect::~AutoPanEffect() { notifyListenersOfDeletion(); }

bool AutoPanEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == rate) return !isOn(*sync);
    if (&p == division) return isOn(*sync);
    return true;
}

void AutoPanEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void AutoPanEffect::reset()
{
    lfo.reset();
    smoothedPan = 0.0f;
}

void AutoPanEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;

    const double inc = lfo.startBlock(sampleRate, isOn(*sync), rate->getCurrentValue(), getTempo(),
                                      divisionBeats(choiceOf(*division)), getBeatPosition(fc));
    const auto shape = (EffectLfo::Wave) choiceOf(*wave);
    const float amount = depth->getCurrentValue();
    const double offset = phase->getCurrentValue() / 360.0;
    const float smoothing = 1.0f - coefficient(2.0, sampleRate);   // softens the square wave's jumps

    Channels io(fc);
    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        const float target = amount * lfo.next(inc, shape, offset);
        smoothedPan += (target - smoothedPan) * smoothing;
        if (!io.stereo)
            continue;
        // Constant power: centred, both sides are untouched.
        const float angle = (smoothedPan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        io.l[i] *= std::cos(angle) * juce::MathConstants<float>::sqrt2;
        io.r[i] *= std::sin(angle) * juce::MathConstants<float>::sqrt2;
    }
    panNow.store(smoothedPan, std::memory_order_relaxed);
}

//==============================================================================
// Filter
const char* FilterEffect::xmlTypeName = "eightTrackFilter";

FilterEffect::FilterEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    mode      = addChoice("mode", "Type", { "Low Pass", "High Pass", "Band Pass", "Notch" }, 0, "Filter");
    slope     = addChoice("slope", "Slope", { "12 dB", "24 dB" }, 0, "Filter");
    cutoff    = addKnob("cutoff", "Cutoff", skewed(kMinHz, kMaxHz, 632.0f, 1.0f), 1000.0f, hzText, "Filter");
    resonance = addKnob("resonance", "Resonance", { 0.0f, 1.0f, 0.01f }, 0.1f, percentText, "Filter");
    drive     = addKnob("drive", "Drive", { 0.0f, 24.0f, 0.1f }, 0.0f, dbText, "Filter");
    mix       = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Filter");

    lfoDepth    = addKnob("lfoDepth", "LFO Depth", { -4.0f, 4.0f, 0.01f }, 0.0f,
                          [](float v) { return signedText(v, 2, " oct"); }, "LFO");
    lfoSync     = addToggle("lfoSync", "LFO Sync", true, "LFO");
    lfoRate     = addKnob("lfoRate", "LFO Rate", skewed(0.05f, 20.0f, 1.0f, 0.01f), 1.0f,
                          [](float v) { return juce::String(v, v < 1.0f ? 2 : 1) + " Hz"; }, "LFO");
    lfoDivision = addChoice("lfoDivision", "LFO Division", divisionNames(), indexOfDivision("1/4"), "LFO");
    lfoWave     = addChoice("lfoWave", "LFO Wave", EffectLfo::waveNames(), EffectLfo::Sine, "LFO");
    lfoSpread   = addKnob("lfoSpread", "Stereo Spread", { 0.0f, 180.0f, 1.0f }, 0.0f,
                          [](float v) { return juce::String(juce::roundToInt(v)) + juce::String::fromUTF8("°"); }, "LFO");
}

FilterEffect::~FilterEffect() { notifyListenersOfDeletion(); }

bool FilterEffect::isControlActive(const te::AutomatableParameter& p) const
{
    const bool modulating = std::abs(lfoDepth->getCurrentValue()) > 0.001f;
    if (&p == lfoRate) return modulating && !isOn(*lfoSync);
    if (&p == lfoDivision) return modulating && isOn(*lfoSync);
    if (&p == lfoSync || &p == lfoWave || &p == lfoSpread) return modulating;
    return true;
}

EffectPlugin::Display FilterEffect::getDisplay() const
{
    const float hz = cutoffNow.load(std::memory_order_relaxed);
    return { "Cutoff", (float) (std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz)) };
}

void FilterEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void FilterEffect::reset()
{
    for (auto& channel : stages)
        for (auto& s : channel)
            s.reset();
    lfo.reset();
    smoothedLogHz = -1;
}

void FilterEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;

    constexpr int kChunk = 8;   // the cutoff moves every few samples
    const auto type = (EffectSvf::Mode) choiceOf(*mode);
    const bool steep = choiceOf(*slope) == 1;
    const double q = 0.5 * std::pow(40.0, (double) resonance->getCurrentValue());
    // A 24 dB low or high pass resonates in its first stage only: the second is a flat Butterworth half.
    const double q2 = type == EffectSvf::LowPass || type == EffectSvf::HighPass ? 0.54 : q;
    const float driveGain = juce::Decibels::decibelsToGain(drive->getCurrentValue());
    const bool driving = driveGain > 1.001f;
    const float driveTrim = 1.0f / std::sqrt(driveGain);
    const float wet = mix->getCurrentValue(), dry = 1.0f - wet;

    const double inc = lfo.startBlock(sampleRate, isOn(*lfoSync), lfoRate->getCurrentValue(), getTempo(),
                                      divisionBeats(choiceOf(*lfoDivision)), getBeatPosition(fc));
    const auto shape = (EffectLfo::Wave) choiceOf(*lfoWave);
    const double octaves = lfoDepth->getCurrentValue();
    const double spread = lfoSpread->getCurrentValue() / 360.0;

    const double targetLogHz = std::log((double) cutoff->getCurrentValue());
    if (smoothedLogHz < 0)
        smoothedLogHz = targetLogHz;
    const double smoothing = 1.0 - std::exp(-kChunk / (0.02 * sampleRate));
    const double topHz = juce::jmin((double) kMaxHz, sampleRate * 0.45);

    Channels io(fc);
    float* channels[2] = { io.l, io.r };
    const int numChannels = io.stereo ? 2 : 1;
    double hzNow = 0;

    for (int start = 0; start < fc.bufferNumSamples; start += kChunk)
    {
        const int n = juce::jmin(kChunk, fc.bufferNumSamples - start);
        smoothedLogHz += (targetLogHz - smoothedLogHz) * smoothing;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const double mod = octaves * lfo.valueAt(ch == 0 ? 0.0 : spread, shape);
            const double hz = juce::jlimit((double) kMinHz, topHz, std::exp(smoothedLogHz) * std::exp2(mod));
            if (ch == 0)
                hzNow = hz;
            stages[ch][0].set(sampleRate, hz, q);
            stages[ch][1].set(sampleRate, hz, q2);

            float* d = channels[ch] + start;
            for (int i = 0; i < n; ++i)
            {
                const float in = d[i];
                float x = driving ? std::tanh(in * driveGain) * driveTrim : in;
                x = stages[ch][0].process(x, type);
                if (steep)
                    x = stages[ch][1].process(x, type);
                d[i] = dry * in + wet * x;
            }
        }
        lfo.advance(inc * n);
    }
    cutoffNow.store((float) hzNow, std::memory_order_relaxed);
}

//==============================================================================
// Pitch Shifter: Signalsmith Stretch (MIT licence, shipped with Tracktion) does the shifting.
struct PitchEffect::Stretcher
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    juce::AudioBuffer<float> wet;   // the stretcher's output, one chunk at a time
    juce::AudioBuffer<float> dry;   // the input, delayed to line up with the shifted sound
    int dryPos = 0;
};

const char* PitchEffect::xmlTypeName = "eightTrackPitch";

PitchEffect::PitchEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    pitch        = addKnob("pitch", "Pitch", { -24.0f, 24.0f, 1.0f }, 0.0f,
                           [](float v) { return signedText(v, 0, " st"); }, "Pitch");
    fine         = addKnob("fine", "Fine", { -100.0f, 100.0f, 1.0f }, 0.0f,
                           [](float v) { return signedText(v, 0, " ct"); }, "Pitch");
    formant      = addKnob("formant", "Formant", { -12.0f, 12.0f, 0.1f }, 0.0f,
                           [](float v) { return signedText(v, 1, " st"); }, "Formant");
    keepFormants = addToggle("keepFormants", "Keep Formants", true, "Formant");
    mix          = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Output");
}

PitchEffect::~PitchEffect() { notifyListenersOfDeletion(); }

double PitchEffect::getLatencySeconds() { return latencySamples / sampleRate; }

void PitchEffect::initialise(const te::PluginInitialisationInfo& info)
{
    if (stretcher == nullptr)
        stretcher = std::make_unique<Stretcher>();
    auto& s = *stretcher;
    // 80 ms blocks: good on voices and most instruments, with about 100 ms of latency, which
    // Tracktion makes up for on playback.
    s.stretch.configure(2, (int) (info.sampleRate * 0.08), (int) (info.sampleRate * 0.02), true);
    latencySamples = s.stretch.inputLatency() + s.stretch.outputLatency();
    s.wet.setSize(2, juce::jmax(512, info.blockSizeSamples));
    s.dry.setSize(2, juce::jmax(1, latencySamples));   // read-then-write: delays by exactly its length
    reset();
}

void PitchEffect::deinitialise() {}

void PitchEffect::reset()
{
    if (stretcher == nullptr)
        return;
    stretcher->stretch.reset();
    stretcher->dry.clear();
    stretcher->dryPos = 0;
}

void PitchEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || stretcher == nullptr)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& s = *stretcher;

    s.stretch.setTransposeSemitones(pitch->getCurrentValue() + fine->getCurrentValue() / 100.0f,
                                    (float) (8000.0 / sampleRate));
    s.stretch.setFormantSemitones(formant->getCurrentValue(), isOn(*keepFormants));
    const auto [dryGain, wetGain] = mixGains(mix->getCurrentValue());

    Channels io(fc);
    const int dryLength = s.dry.getNumSamples();
    for (int start = 0; start < fc.bufferNumSamples; start += s.wet.getNumSamples())
    {
        const int n = juce::jmin(s.wet.getNumSamples(), fc.bufferNumSamples - start);
        float* in[2] = { io.l + start, io.r + start };
        float* out[2] = { s.wet.getWritePointer(0), s.wet.getWritePointer(1) };
        s.stretch.process(in, n, out, n);

        for (int i = 0; i < n; ++i)
        {
            float* d[2] = { s.dry.getWritePointer(0), s.dry.getWritePointer(1) };
            const float delayedL = d[0][s.dryPos], delayedR = d[1][s.dryPos];
            d[0][s.dryPos] = in[0][i];
            d[1][s.dryPos] = in[1][i];
            if (++s.dryPos >= dryLength)
                s.dryPos = 0;

            in[0][i] = dryGain * delayedL + wetGain * out[0][i];
            if (io.stereo)
                in[1][i] = dryGain * delayedR + wetGain * out[1][i];
        }
    }
}

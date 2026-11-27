#include "AnalogSynth.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow", "-Wshadow-field-in-constructor", "-Wsign-conversion", "-Wconversion",
                                    "-Wfloat-conversion", "-Wimplicit-float-conversion", "-Wshorten-64-to-32",
                                    "-Wunused-parameter", "-Wfloat-equal", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                    "-Wimplicit-int-conversion", "-Wcast-align", "-Wunused-variable", "-Wdeprecated",
                                    "-Wmissing-field-initializers", "-Wold-style-cast", "-Wunused-private-field",
                                    "-Wswitch-enum", "-Wsign-compare", "-Wunused-function", "-Wreorder-ctor",
                                    "-Wshadow-uncaptured-local", "-Wshadow-field", "-Wcomma", "-Wunused-but-set-variable",
                                    "-Wimplicit-fallthrough", "-Wmissing-prototypes", "-Wundef", "-Wnon-virtual-dtor")
#include "../../ThirdParty/kr106/KR106_DSP.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }

    // KR106DSP::SetParam's indices (KR106_DSP_SetParam.h)
    enum
    {
        kBenderDco = 0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
        kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
        kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
        kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
        kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
        kChorusOff, kChorusI, kChorusII,
        kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
        kVcfEnvInv, kVcaMode,
        kBender, kTuning, kPower,
        kPortaMode, kPortaRate,
        kTransposeOffset, kBenderLfo,
        kAdsrMode,
        kNumSent
    };
}

struct AnalogSynth::Dsp
{
    KR106DSP<float> synth;
    double sent[kNumSent];
    bool any = false;

    void set(int id, double value, bool force)
    {
        if (force || !any || std::abs(sent[id] - value) > 1.0e-7)
        {
            sent[id] = value;
            synth.SetParam(id, value);
        }
    }
};

const char* AnalogSynth::xmlTypeName = "eightTrackAnalogSynth";

AnalogSynth::AnalogSynth(te::PluginCreationInfo info) : EffectPlugin(info)
{
    saw       = addToggle("saw", "Saw", true, "Oscillator");
    pulse     = addToggle("pulse", "Pulse", false, "Oscillator");
    pwm       = addKnob("pwm", "PWM", { 0.0f, 1.0f, 0.01f }, 0.4f, percentText, "Oscillator");
    pwmSource = addChoice("pwmSource", "PWM Source", { "LFO", "Manual", "Envelope" }, 0, "Oscillator");
    sub       = addKnob("sub", "Sub", { 0.0f, 1.0f, 0.01f }, 0.4f, percentText, "Oscillator");
    noise     = addKnob("noise", "Noise", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "Oscillator");
    range     = addChoice("range", "Range", { "16'", "8'", "4'" }, 1, "Oscillator");

    hpf       = addChoice("hpf", "HPF", { "Bass Boost", "Flat", "236 Hz", "754 Hz" }, 1, "Filter");
    cutoff    = addKnob("cutoff", "Cutoff", { 0.0f, 1.0f, 0.005f }, 0.55f, percentText, "Filter");
    resonance = addKnob("resonance", "Resonance", { 0.0f, 1.0f, 0.01f }, 0.2f, percentText, "Filter");
    envAmount = addKnob("envAmount", "Env", { 0.0f, 1.0f, 0.01f }, 0.35f, percentText, "Filter");
    envInvert = addToggle("envInvert", "Invert Env", false, "Filter");
    lfoAmount = addKnob("lfoAmount", "LFO", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "Filter");
    keyFollow = addKnob("keyFollow", "Key", { 0.0f, 1.0f, 0.01f }, 0.5f, percentText, "Filter");

    attack    = addKnob("attack", "Attack", { 0.0f, 1.0f, 0.01f }, 0.05f, percentText, "Envelope");
    decay     = addKnob("decay", "Decay", { 0.0f, 1.0f, 0.01f }, 0.4f, percentText, "Envelope");
    sustain   = addKnob("sustain", "Sustain", { 0.0f, 1.0f, 0.01f }, 0.6f, percentText, "Envelope");
    release   = addKnob("release", "Release", { 0.0f, 1.0f, 0.01f }, 0.3f, percentText, "Envelope");
    vcaMode   = addChoice("vcaMode", "VCA", { "Envelope", "Gate" }, 0, "Envelope");
    envModel  = addChoice("envModel", "Env Type", { "Analog", "Digital" }, 1, "Envelope");

    lfoRate   = addKnob("lfoRate", "Rate", { 0.0f, 1.0f, 0.01f }, 0.4f, percentText, "LFO");
    lfoDelay  = addKnob("lfoDelay", "Delay", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "LFO");
    lfoPitch  = addKnob("lfoPitch", "Vibrato", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "LFO");

    voiceMode = addChoice("voiceMode", "Voices", voiceModeNames(), PolyI, "Voice");
    glide     = addKnob("glide", "Glide", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "Voice");
    chorus    = addChoice("chorus", "Chorus", { "Off", "I", "II", "I+II" }, 1, "Voice");
    analog    = addKnob("analog", "Hiss & Hum", { 0.0f, 1.0f, 0.01f }, 0.0f, percentText, "Voice");
    volume    = addKnob("volume", "Volume", { -36.0f, 6.0f, 0.1f }, -6.0f,
                        [](float v) { return juce::String(v, 1) + " dB"; }, "Voice");

    dsp = std::make_unique<Dsp>();
}

AnalogSynth::~AnalogSynth() { notifyListenersOfDeletion(); }

bool AnalogSynth::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == pwm || &p == pwmSource)
        return isOn(*pulse);
    if (&p == glide)
        return choiceOf(*voiceMode) == Mono;
    if (&p == attack || &p == decay || &p == sustain || &p == release)
        return choiceOf(*vcaMode) == 0 || envAmount->getCurrentValue() > 0.0f || choiceOf(*pwmSource) == 2;
    return true;
}

void AnalogSynth::initialise(const te::PluginInitialisationInfo& info)
{
    maxBlock = juce::jmax(64, info.blockSizeSamples);
    auto& d = *dsp;
    d.synth.Reset(info.sampleRate, maxBlock);
    d.any = false;
    applySettings(true);
    d.any = true;
}

void AnalogSynth::reset()
{
    dsp->synth.AllNotesOff();
}

void AnalogSynth::applySettings(bool force)
{
    auto& d = *dsp;
    auto set = [&](int id, double v) { d.set(id, v, force); };
    // The envelope type first: it picks which hardware's curves the other values go through.
    set(kAdsrMode, choiceOf(*envModel));
    set(kDcoSaw, isOn(*saw) ? 1.0 : 0.0);
    set(kDcoPulse, isOn(*pulse) ? 1.0 : 0.0);
    set(kDcoPwm, pwm->getCurrentValue());
    set(kPwmMode, choiceOf(*pwmSource));
    set(kDcoSub, sub->getCurrentValue());
    set(kDcoSubSw, sub->getCurrentValue() > 0.0f ? 1.0 : 0.0);
    set(kDcoNoise, noise->getCurrentValue());
    set(kOctTranspose, choiceOf(*range));

    set(kHpfFreq, choiceOf(*hpf));
    set(kVcfFreq, cutoff->getCurrentValue());
    set(kVcfRes, resonance->getCurrentValue());
    set(kVcfEnv, envAmount->getCurrentValue());
    set(kVcfEnvInv, isOn(*envInvert) ? 1.0 : 0.0);
    set(kVcfLfo, lfoAmount->getCurrentValue());
    set(kVcfKbd, keyFollow->getCurrentValue());

    set(kEnvA, attack->getCurrentValue());
    set(kEnvD, decay->getCurrentValue());
    set(kEnvS, sustain->getCurrentValue());
    set(kEnvR, release->getCurrentValue());
    set(kVcaMode, choiceOf(*vcaMode));
    set(kVcaLevel, 0.5);

    set(kLfoRate, lfoRate->getCurrentValue());
    set(kLfoDelay, lfoDelay->getCurrentValue());
    set(kDcoLfo, lfoPitch->getCurrentValue());
    set(kLfoMode, 0.0);   // auto: the LFO runs on its own

    set(kPortaMode, choiceOf(*voiceMode));
    set(kPortaRate, glide->getCurrentValue());
    const int ch = choiceOf(*chorus);
    set(kChorusI, (ch & 1) ? 1.0 : 0.0);
    set(kChorusII, (ch & 2) ? 1.0 : 0.0);

    set(kBenderDco, 2.0 / 12.0);   // the pitch wheel bends a tone each way
    set(kBenderVcf, 0.0);
    set(kBenderLfo, 0.0);
    set(kTuning, 0.0);
    set(kArpeggio, 0.0);           // 8track's Arpeggiator does this, in time with the song
    set(kHold, 0.0);
    set(kTranspose, 0.0);

    const float a = analog->getCurrentValue();
    d.synth.mNoiseFloorMul = a;
    d.synth.mMainsMul = a;
    d.synth.mChorus.mClockMul = a;
}

void AnalogSynth::applyToBuffer(const te::PluginRenderContext& fc)
{
    applyPanic(fc);
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& d = *dsp;
    applySettings(false);

    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* out[2] = { b.getWritePointer(0, fc.bufferStartSample), b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample) };
    const float gain = juce::Decibels::decibelsToGain(volume->getCurrentValue());

    auto handle = [&](const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
            d.synth.NoteOn(m.getNoteNumber(), m.getVelocity());
        else if (m.isNoteOff())
            d.synth.NoteOff(m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            d.synth.AllNotesOff();
        else if (m.isPitchWheel())
            d.set(kBender, (m.getPitchWheelValue() - 8192) / 8192.0, false);
        else if (m.isController())
            d.synth.ControlChange(m.getControllerNumber(), (float) m.getControllerValue() / 127.0f);
    };

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        d.synth.AllNotesOff();

    // Render between MIDI events, so each note starts on its own sample.
    int done = 0;
    int next = 0;
    auto* midi = fc.bufferForMidiMessages;
    if (midi != nullptr)
        midi->sortByTimestamp();
    const int numEvents = midi != nullptr ? midi->size() : 0;
    while (done < fc.bufferNumSamples)
    {
        while (next < numEvents)
        {
            const auto& m = (*midi)[next];
            const int at = juce::jlimit(0, fc.bufferNumSamples - 1,
                                        (int) std::floor((m.getTimeStamp() + fc.midiBufferOffset) * sampleRate));
            if (at > done)
                break;
            handle(m);
            ++next;
        }
        int until = fc.bufferNumSamples;
        if (next < numEvents)
            until = juce::jlimit(done + 1, fc.bufferNumSamples,
                                 (int) std::floor(((*midi)[next].getTimeStamp() + fc.midiBufferOffset) * sampleRate));
        until = juce::jmin(until, done + maxBlock);
        float* o[2] = { out[0] + done, (stereo ? out[1] : out[0]) + done };
        if (stereo)
            d.synth.ProcessBlock(nullptr, o, 2, until - done);
        else
        {
            // Mono track: render stereo into the channel and a scratch buffer, then sum.
            float right[512];
            for (int s = done; s < until; s += 512)
            {
                const int n = juce::jmin(512, until - s);
                float* pair[2] = { out[0] + s, right };
                d.synth.ProcessBlock(nullptr, pair, 2, n);
                for (int i = 0; i < n; ++i)
                    pair[0][i] = 0.5f * (pair[0][i] + right[i]);
            }
        }
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
            juce::FloatVectorOperations::multiply(o[ch], gain, until - done);
        done = until;
    }
}

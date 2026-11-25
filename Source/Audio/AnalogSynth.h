#pragma once

#include "Effects.h"
#include <memory>

/**
    A Juno-style polysynth that can also play SH-101-style mono leads and basses: one DCO
    per voice (saw, pulse with PWM, sub, noise), the high-pass switch, a resonant 24 dB
    filter, one envelope, the LFO, and the bucket-brigade chorus. In Mono, every voice plays
    the one note (unison, with drift) and Glide slides between notes.

    The voice engine is Ultramaster KR-106 (GPL3), a circuit-level emulation of the
    Juno-6, 60 and 106; ThirdParty/kr106 has its licence and source.
*/
class AnalogSynth : public EffectPlugin
{
public:
    explicit AnalogSynth(te::PluginCreationInfo);
    ~AnalogSynth() override;

    static const char* getPluginName() { return "Analog Synth"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    Kind getKind() const override { return Kind::Instrument; }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return 4.0; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    enum VoiceMode { Mono, PolyI, PolyII };
    static juce::StringArray voiceModeNames() { return { "Mono", "Poly I", "Poly II" }; }

    // Oscillator
    te::AutomatableParameter *saw, *pulse, *pwm, *pwmSource, *sub, *noise, *range;
    // Filter
    te::AutomatableParameter *hpf, *cutoff, *resonance, *envAmount, *envInvert, *lfoAmount, *keyFollow;
    // Envelope
    te::AutomatableParameter *attack, *decay, *sustain, *release, *vcaMode, *envModel;
    // LFO
    te::AutomatableParameter *lfoRate, *lfoDelay, *lfoPitch;
    // Voice
    te::AutomatableParameter *voiceMode, *glide, *chorus, *analog, *volume;

private:
    struct Dsp;
    std::unique_ptr<Dsp> dsp;
    void applySettings(bool force);
    int maxBlock = 512;
};

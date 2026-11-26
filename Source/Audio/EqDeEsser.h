#pragma once

#include "Effects.h"
#include <array>

/**
    The last few thousand samples an effect has played, for a spectrum analyser. The audio
    thread writes, the UI reads, with no locks; the UI may see a block half written, which
    an analyser doesn't mind. Nothing is written unless a window is showing it.
*/
class SpectrumTap
{
public:
    static constexpr int kSize = 4096;

    /** The window sets this while it's open. */
    std::atomic<bool> active { false };

    /** Audio thread: adds a block of mono samples. */
    void push(const float* l, const float* r, int n)
    {
        int w = writePos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            samples[(size_t) w].store(0.5f * (l[i] + r[i]), std::memory_order_relaxed);
            w = (w + 1) & (kSize - 1);
        }
        writePos.store(w, std::memory_order_release);
    }

    /** UI: copies the latest `n` samples (n <= kSize), oldest first. */
    void read(float* dest, int n) const
    {
        const int end = writePos.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dest[i] = samples[(size_t) ((end - n + i) & (kSize - 1))].load(std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, kSize> samples {};
    std::atomic<int> writePos { 0 };
};

//==============================================================================
/**
    A five-band parametric EQ. Each band can be a low cut, low shelf, bell, high shelf,
    high cut or notch, and switched off on its own. The window draws the curve over a
    spectrum of what goes in and what comes out, and the bands can be dragged on it.
*/
class EqEffect : public EffectPlugin
{
public:
    explicit EqEffect(te::PluginCreationInfo);
    ~EqEffect() override;

    static const char* getPluginName() { return "EQ"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    static constexpr int kBands = 5;
    static constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f, kMaxGainDb = 18.0f;

    enum Type { LowCut, LowShelf, Bell, HighShelf, HighCut, Notch };
    static juce::StringArray typeNames() { return { "Low Cut", "Low Shelf", "Bell", "High Shelf", "High Cut", "Notch" }; }
    /** False for the cuts and the notch, which have no gain. */
    static bool hasGain(Type t) { return t == LowShelf || t == Bell || t == HighShelf; }

    /** The row that holds band `index` (0-based)'s controls: "Band 1"... */
    static juce::String rowFor(int index) { return "Band " + juce::String(index + 1); }

    struct Band
    {
        te::AutomatableParameter *on, *type, *freq, *gain, *q;
        Type getType() const { return (Type) choiceOf(*type); }
        bool isOn() const { return EffectPlugin::isOn(*on); }
    };
    std::array<Band, kBands> bands;
    te::AutomatableParameter* output;

    /** A biquad's coefficients, normalised so a0 = 1. */
    struct Coefficients
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        /** The gain at `hz`, in dB. */
        double magnitudeDb(double hz, double sampleRate) const;
    };
    static Coefficients design(Type, double sampleRate, double hz, double gainDb, double q);

    /** The response of one band (0 if it's off), or of all of them plus Output, in dB, at
        the current settings. Any thread. */
    double bandResponseDb(int index, double hz) const;
    double responseDb(double hz) const;

    SpectrumTap before, after;

    /** The sample rate it runs at (48 kHz until it's been initialised). */
    double rate() const { return sampleRate > 0 ? sampleRate : 48000.0; }

private:
    struct Biquad
    {
        Coefficients c;
        double z1[2] {}, z2[2] {};
        float process(float x, int ch)
        {
            const double y = c.b0 * x + z1[ch];
            z1[ch] = c.b1 * x - c.a1 * y + z2[ch];
            z2[ch] = c.b2 * x - c.a2 * y;
            return (float) y;
        }
    };
    struct Settings { bool on = false; int type = -1; float freq = 0, gain = 0, q = 0;
        bool operator==(const Settings& o) const
        {
            return on == o.on && type == o.type && juce::exactlyEqual(freq, o.freq) && juce::exactlyEqual(gain, o.gain)
                && juce::exactlyEqual(q, o.q);
        }
    };

    std::array<Biquad, kBands> filters;
    std::array<Settings, kBands> applied;
    float outputGain = 1.0f;
};

//==============================================================================
/**
    A de-esser: listens above (or around) a frequency for sibilance and turns it down when
    it goes over the threshold. Split turns down only the highs; Wide turns down everything,
    which can sound more natural on a voice. Listen plays what the detector hears, to find
    the right frequency.
*/
class DeEsserEffect : public EffectPlugin
{
public:
    explicit DeEsserEffect(te::PluginCreationInfo);
    ~DeEsserEffect() override;

    static const char* getPluginName() { return "De-Esser"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    Display getDisplay() const override { return { "Reduction", reductionNow.load(std::memory_order_relaxed) / 24.0f }; }

    enum Mode { Split, Wide };
    enum Detect { HighPass, BandPass };

    te::AutomatableParameter *frequency, *threshold, *range, *release, *mode, *detect, *listen;

private:
    EffectSvf detector[2];
    EffectSvf splitter[2][4];   // [channel]: low pass twice, high pass twice
    float envelope = 0, reduction = 0;   // reduction in dB, >= 0
    std::atomic<float> reductionNow { 0.0f };
};

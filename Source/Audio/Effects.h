#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace te = tracktion;

/**
    8track's own effects: Noise Gate, Delay, Auto Pan, Filter and Pitch Shifter.

    They're Tracktion plugins, so they go in the same insert slots as VST3/AU effects and
    get the same things for free: every control can be automated and MIDI-assigned, and
    their settings are saved with the project and undone like any other edit.

    Each effect lists its controls (getControls()); the UI builds the effect's window from
    that list. Effects that follow the tempo (Delay, Auto Pan and the Filter's LFO) are
    told the song tempo by the AudioEngine, and lock to the playhead while playing.
*/
class EffectPlugin : public te::Plugin
{
public:
    explicit EffectPlugin(te::PluginCreationInfo);
    ~EffectPlugin() override;

    /** One control in the effect's window. */
    struct Control
    {
        enum class Kind { Knob, Choice, Toggle };
        te::AutomatableParameter* param = nullptr;
        Kind kind = Kind::Knob;
        juce::StringArray choices;   // Choice: the names of its values 0, 1, 2...
        juce::String row;            // controls are laid out in rows, in the order added
        juce::String label;          // its name in the window, if shorter than the parameter's
        std::unique_ptr<juce::CachedValue<float>> value;
    };
    const std::vector<Control>& getControls() const { return controls; }

    /** False when a control does nothing in the current settings (e.g. Time while synced). */
    virtual bool isControlActive(const te::AutomatableParameter&) const { return true; }

    /** A live reading for the effect's window: the gate's gain, the pan position... */
    struct Display { juce::String name; float value = 0; bool bipolar = false; };
    virtual Display getDisplay() const { return {}; }

    /** The song tempo. The AudioEngine keeps it up to date; any thread may read it. */
    void setTempo(double bpm) { tempoBpm.store(juce::jlimit(20.0, 999.0, bpm), std::memory_order_relaxed); }
    double getTempo() const { return tempoBpm.load(std::memory_order_relaxed); }

    /** Note lengths for tempo sync, longest first. */
    static const juce::StringArray& divisionNames();
    static double divisionBeats(int index);   // in quarter notes

    /** The UI sets this to give the effects their windows. Tests leave it empty. */
    static inline std::function<std::unique_ptr<te::Plugin::EditorComponent>(EffectPlugin&)> editorFactory;
    std::unique_ptr<EditorComponent> createEditor() override { return editorFactory ? editorFactory(*this) : nullptr; }

    bool canBeDisabled() override { return true; }
    bool takesAudioInput() override { return getKind() == Kind::Effect; }
    bool takesMidiInput() override { return getKind() != Kind::Effect; }
    bool isSynth() override { return getKind() == Kind::Instrument; }
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    /** Effects process audio; instruments play MIDI; MIDI effects change the notes on their
        way to the instrument, so they go first in a MIDI track's chain. */
    enum class Kind { Effect, Instrument, MidiEffect };
    virtual Kind getKind() const { return Kind::Effect; }

    /** A menu in the effect's window that isn't a parameter (e.g. the sidechain source).
        Message thread only. */
    struct Selector
    {
        juce::String name, row;
        std::function<juce::StringArray()> options;
        std::function<int()> get;              // -1 for none of them
        std::function<void(int)> set;
    };
    virtual std::vector<Selector> getSelectors() { return {}; }

    /** A file the effect loads (an impulse response, a sample). Message thread only. */
    struct FileSlot
    {
        juce::String name, row, wildcard;
        std::function<juce::File()> get;
        std::function<bool(const juce::File&, juce::String& error)> set;   // an empty file clears it
    };
    virtual std::vector<FileSlot> getFileSlots() { return {}; }

    /** Properties in the effect's state that name audio files, so that Save As can copy them
        into the project like recordings. Resolve them with te::SourceFileReference. */
    struct FileReference { juce::ValueTree state; juce::Identifier property; };
    virtual void getFileReferences(std::vector<FileReference>&) {}
    /** Every file reference of 8track's effects in an Edit. */
    static std::vector<FileReference> findFileReferences(te::Edit&);

    /** The built-in effects, for the insert and instrument menus. */
    struct Type { juce::String name, xmlType, description; Kind kind = Kind::Effect; };
    static std::vector<Type> getTypes();
    static std::optional<Type> findType(const juce::String& xmlType);
    /** Registers the effects with Tracktion. Call once, before any Edit is loaded. */
    static void registerTypes(te::PluginManager&);

protected:
    te::AutomatableParameter* addKnob(const juce::String& id, const juce::String& name, juce::NormalisableRange<float>,
                                      float defaultValue, std::function<juce::String(float)> text, const juce::String& row);
    te::AutomatableParameter* addChoice(const juce::String& id, const juce::String& name, const juce::StringArray& choices,
                                        int defaultIndex, const juce::String& row);
    te::AutomatableParameter* addToggle(const juce::String& id, const juce::String& name, bool defaultOn, const juce::String& row);

    /** A shorter name for a control in its window (the parameter keeps its full name, for
        automation lists): "Tune" under "Pad 3" rather than "3 Tune". */
    void setLabel(const te::AutomatableParameter* p, const juce::String& label)
    {
        for (auto& c : controls)
            if (c.param == p)
                c.label = label;
    }

    static int choiceOf(const te::AutomatableParameter& p) { return juce::roundToInt(p.getCurrentValue()); }
    static bool isOn(const te::AutomatableParameter& p) { return p.getCurrentValue() >= 0.5f; }

    /** "+3.0 st", "-12 ct": a value with its sign. */
    static juce::String signedText(float v, int decimals, const char* unit);

    /** A file named by a property of the state, resolved like clips' audio files. */
    juce::File getFileProperty(const juce::Identifier&) const;
    void setFileProperty(const juce::Identifier&, const juce::File&);
    /** Copies a file from outside the project into its Audio folder, so the project keeps
        working if the original moves. Returns the copy (or the file, if already inside). */
    juce::File adoptFile(const juce::File&) const;

    /** Position in quarter notes at the start of this block, if the transport is playing. */
    std::optional<double> getBeatPosition(const te::PluginRenderContext&) const;

    /** Mix 0..1 to dry and wet gains: both are at full level in the middle. */
    static std::pair<float, float> mixGains(float mix) { return { juce::jmin(1.0f, 2.0f * (1.0f - mix)), juce::jmin(1.0f, 2.0f * mix) }; }

private:
    std::vector<Control> controls;
    std::atomic<double> tempoBpm { 120.0 };
};

//==============================================================================
/** A sine/triangle/square/saw/random LFO that can lock to the song position. */
class EffectLfo
{
public:
    enum Wave { Sine, Triangle, Square, SawUp, SawDown, Random };
    static juce::StringArray waveNames() { return { "Sine", "Triangle", "Square", "Saw Up", "Saw Down", "Random" }; }

    /** Sets the phase for this block: locked to the song position when given (and synced),
        otherwise it carries on from where it was. Returns the phase increment per sample. */
    double startBlock(double sampleRate, bool synced, double hz, double bpm, double divisionBeats,
                      std::optional<double> beatPosition);
    /** The LFO's value, -1..1, at the current phase plus offset (0..1); then moves on one sample. */
    float next(double increment, Wave, double offset = 0.0);
    /** The value at the current phase plus offset (0..1), without moving on. */
    float valueAt(double offset, Wave) const;
    void advance(double amount);
    void reset() { phase = 0.0; held = 0.0f; nextHeld = 0.0f; }

private:
    double phase = 0.0;
    float held = 0.0f, nextHeld = 0.0f;
    juce::Random random;
};

/** A state-variable filter (Simper's): low, high, band and notch from one structure, and
    smooth when its cutoff moves every few samples. */
struct EffectSvf
{
    enum Mode { LowPass, HighPass, BandPass, Notch };
    void set(double sampleRate, double hz, double q);
    float process(float x, Mode);
    void reset() { ic1 = ic2 = 0.0f; }

    float a1 = 1, a2 = 0, a3 = 0, k = 1.4142f;
    float ic1 = 0, ic2 = 0;
};

//==============================================================================
class GateEffect : public EffectPlugin
{
public:
    explicit GateEffect(te::PluginCreationInfo);
    ~GateEffect() override;

    static const char* getPluginName() { return "Noise Gate"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    int getNumOutputChannelsGivenInputs(int numInputs) override { return numInputs; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    Display getDisplay() const override { return { "Gate", gainNow.load(std::memory_order_relaxed) }; }

    te::AutomatableParameter *threshold, *range, *attack, *hold, *release, *hysteresis, *keyFilter;

private:
    EffectSvf key[2];
    float envelope = 0, gain = 1;
    bool open = false;
    int holdLeft = 0;
    std::atomic<float> gainNow { 1.0f };
};

//==============================================================================
class DelayEffect : public EffectPlugin
{
public:
    explicit DelayEffect(te::PluginCreationInfo);
    ~DelayEffect() override;

    static const char* getPluginName() { return "Delay"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override;

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    /** The delay time the settings ask for now, in seconds. */
    double getDelaySeconds() const;
    static constexpr double kMaxSeconds = 4.0;

    te::AutomatableParameter *sync, *time, *division, *feedback, *pingPong, *lowCut, *highCut, *mix;

private:
    juce::AudioBuffer<float> buffer;
    int writePos = 0;
    double smoothedDelay = -1;   // in samples
    EffectSvf lowCutFilter[2], highCutFilter[2];
};

//==============================================================================
class AutoPanEffect : public EffectPlugin
{
public:
    explicit AutoPanEffect(te::PluginCreationInfo);
    ~AutoPanEffect() override;

    static const char* getPluginName() { return "Auto Pan"; }
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
    Display getDisplay() const override { return { "Pan", panNow.load(std::memory_order_relaxed), true }; }

    te::AutomatableParameter *sync, *rate, *division, *depth, *wave, *phase;

private:
    EffectLfo lfo;
    float smoothedPan = 0;
    std::atomic<float> panNow { 0.0f };
};

//==============================================================================
class FilterEffect : public EffectPlugin
{
public:
    explicit FilterEffect(te::PluginCreationInfo);
    ~FilterEffect() override;

    static const char* getPluginName() { return "Filter"; }
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
    Display getDisplay() const override;

    static constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;

    te::AutomatableParameter *mode, *slope, *cutoff, *resonance, *drive, *mix;
    te::AutomatableParameter *lfoDepth, *lfoSync, *lfoRate, *lfoDivision, *lfoWave, *lfoSpread;

private:
    EffectSvf stages[2][2];   // [channel][stage]
    EffectLfo lfo;
    double smoothedLogHz = -1;
    std::atomic<float> cutoffNow { 1000.0f };
};

//==============================================================================
class PitchEffect : public EffectPlugin
{
public:
    explicit PitchEffect(te::PluginCreationInfo);
    ~PitchEffect() override;

    static const char* getPluginName() { return "Pitch Shifter"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getLatencySeconds() override;

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;

    te::AutomatableParameter *pitch, *fine, *formant, *keepFormants, *mix;

private:
    struct Stretcher;
    std::unique_ptr<Stretcher> stretcher;
    int latencySamples = 0;
};

#pragma once

#include "Samples.h"
#include "../Model/DrumPattern.h"
#include <array>

/**
    Sixteen pads, one sample each, played from MIDI notes 48 (C2) up: drop a sound on a pad
    and shape it with pitch, attack, decay, tone, level and pan. Pads in the same choke group
    cut each other off (open and closed hi-hats). Like Sitala, it keeps the controls few.
*/
class DrumSampler : public EffectPlugin
{
public:
    explicit DrumSampler(te::PluginCreationInfo);
    ~DrumSampler() override;

    static const char* getPluginName() { return "Drum Sampler"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    Kind getKind() const override { return Kind::Instrument; }
    BusLayout getBusses() const override;
    int getNumOutputChannelsGivenInputs(int) override { return kOutputChannels; }
    std::optional<AuxOutputRoute> getAuxOutputRoute(int outputBus) const override;
    double getTailLength() const override { return 1.0; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    void getFileReferences(std::vector<FileReference>&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    static constexpr int kPads = 16, kFirstNote = 48, kVoices = 32, kSendCount = 2;
    static juce::String rowFor(int pad) { return "Pad " + juce::String(pad + 1); }
    static juce::String noteNameForPad(int pad) { return juce::MidiMessage::getMidiNoteName(kFirstNote + pad, true, true, 3); }

    enum class FactoryKit { empty, acoustic, electronic };
    static const char* kitName(FactoryKit);
    FactoryKit getFactoryKit() const;
    /** Loads the full kit. Replaces sample assignments in one undo step. */
    void loadFactoryKit(FactoryKit);

    /** Loads a sound onto a pad (copied into the project); an empty file clears the pad. */
    bool loadPad(int pad, const juce::File&, juce::String& error);
    juce::File getPadFile(int pad) const;
    Sample::Ptr getPadSample(int pad) const { return slots[(size_t) pad].get(); }

    /** Plays a pad now (clicking it in the window), at this velocity. Any thread. */
    void audition(int pad, int velocity = 100);
    /** Loops a pattern through this sampler while the song transport is stopped. Message thread. */
    void previewPattern(const DrumPattern&, bool restart);
    void stopPatternPreview();
    int getPreviewStep() const { return playingStep.load(std::memory_order_relaxed); }
    /** How recently each pad played (1 = just now), for lighting the pads. */
    float getActivity(int pad) const { return activity[(size_t) pad].load(); }

    struct Pad
    {
        te::AutomatableParameter *tune, *attack, *decay, *tone, *level, *pan, *choke;
        std::array<te::AutomatableParameter*, kSendCount> sends;
    };
    std::array<Pad, kPads> pads;
    te::AutomatableParameter *velocity, *volume;

private:
    static constexpr int kOutputChannels = 2 * (1 + kSendCount);
    using Outputs = std::array<float*, kOutputChannels>;
    struct Voice
    {
        Sample::Ptr sample;
        int pad = -1;
        double pos = 0, step = 1;
        float pitch = 0;
        float gain = 0, panL = 1, panR = 1;
        juce::ADSR env;
        Tilt tilt[2];
        bool active = false;
        bool preview = false;
        juce::uint32 started = 0;
    };
    void start(int pad, int velocity, bool preview = false);
    void setVoicePitch(Voice&, float semitones);
    void render(Voice&, const Outputs&, int n);
    static const juce::Identifier& padProperty(int pad);
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void reloadPad(int pad);
    void applyPreviewRequest(bool transportPlaying);

    std::array<SampleSlot, kPads> slots;
    std::array<Voice, kVoices> voices;
    std::array<std::atomic<float>, kPads> activity {};
    std::atomic<int> pendingAudition { -1 }, pendingVelocity { 100 };
    struct PreviewRequest { std::optional<DrumPattern> pattern; bool restart = false; };
    juce::SpinLock previewLock;
    std::optional<PreviewRequest> pendingPreview;
    std::optional<DrumPattern> activePreview;
    int nextPreviewStep = 0;
    double nextPreviewSample = 0.0;
    struct PreviewNote { double at = 0; int pad = 0, velocity = 0; };   // at: samples from this block's start
    std::array<PreviewNote, kPads * 9> previewNotes {};   // a step's notes: up to 8 roll hits and a flam's grace a pad
    int previewNoteCount = 0;
    std::atomic<int> playingStep { -1 };
    juce::uint32 counter = 0;
    bool suppressPadReload = false;
};

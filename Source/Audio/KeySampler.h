#pragma once

#include "Samples.h"
#include <array>

/**
    Plays one sound up and down the keyboard: load a file, or record one straight from an
    input (sing a note, play a guitar chord) and play it from MIDI. Set the note it was
    played at (Root), where it starts, whether it loops while held, and its envelope.
*/
class KeySampler : public EffectPlugin
{
public:
    explicit KeySampler(te::PluginCreationInfo);
    ~KeySampler() override;

    static const char* getPluginName() { return "Sampler"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    Kind getKind() const override { return Kind::Instrument; }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return release->getCurrentValue() + 0.1; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;
    std::vector<FileSlot> getFileSlots() override;
    void getFileReferences(std::vector<FileReference>&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    static constexpr int kVoices = 16;

    bool loadFile(const juce::File&, juce::String& error);
    /** Keeps a recording: writes it into the project's Audio folder and plays it. */
    bool useRecording(Sample::Ptr, juce::String& error);
    juce::File getFile() const { return getFileProperty(fileProperty); }
    Sample::Ptr getSample() const { return slot.get(); }
    /** Guesses the recording's note from its pitch, and sets Root to it. */
    bool detectRoot();

    /** Plays a key now (from the window's keyboard). Any thread. */
    void playKey(int note, bool on);

    te::AutomatableParameter *root, *fine, *start, *loop, *loopStart;
    te::AutomatableParameter *attack, *decay, *sustain, *release;
    te::AutomatableParameter *velocity, *tone, *volume;

private:
    struct Voice
    {
        Sample::Ptr sample;
        int note = -1;
        double pos = 0, step = 1;
        float gain = 0;
        juce::ADSR env;
        Tilt tilt[2];
        bool active = false;
        juce::uint32 started = 0;
    };
    void noteOn(int note, int vel);
    void noteOff(int note);
    void render(Voice&, float* l, float* r, int n);
    void reload();

    static inline const juce::Identifier fileProperty { "sample" };
    SampleSlot slot;
    std::array<Voice, kVoices> voices;
    juce::uint32 counter = 0;
    static constexpr int kKeyQueue = 64;
    juce::AbstractFifo keyFifo { kKeyQueue };
    std::array<int, kKeyQueue> keyQueue {};
};

#pragma once

#include "Effects.h"
#include <array>

/**
    Plays the notes you hold one at a time, in time with the song: up, down, up and down,
    random, in the order played, or as repeated chords, over one to four octaves, with gate
    length, swing and latch. It sits in front of a MIDI track's instrument. With the transport
    stopped it keeps its own clock at the song's tempo, starting the moment you play.
*/
class ArpeggiatorEffect : public EffectPlugin
{
public:
    explicit ArpeggiatorEffect(te::PluginCreationInfo);
    ~ArpeggiatorEffect() override;

    static const char* getPluginName() { return "Arpeggiator"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    Kind getKind() const override { return Kind::MidiEffect; }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }   // audio passes untouched
    int getNumOutputChannelsGivenInputs(int n) override { return n; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    enum Mode { Up, Down, UpDown, DownUp, UpAndDown, Random, AsPlayed, Chord };
    static juce::StringArray modeNames()
    {
        return { "Up", "Down", "Up/Down", "Down/Up", "Up & Down", "Random", "As Played", "Chord" };
    }

    /** The notes one cycle plays, for these held notes (in the order pressed), into `out`
        (which doesn't allocate if it has the room reserved). */
    static void pattern(Mode, const int* heldInOrder, int numHeld, int octaves, std::vector<int>& out);
    static std::vector<int> pattern(Mode m, const std::vector<int>& held, int octaves)
    {
        std::vector<int> out;
        pattern(m, held.data(), (int) held.size(), octaves, out);
        return out;
    }

    te::AutomatableParameter *mode, *rate, *octaves, *gate, *swing, *latch, *velocity;

private:
    struct Held { int note; int velocity; };
    void noteOn(int note, int vel);
    void noteOff(int note);
    void rebuild();
    void stopSounding(te::MidiMessageArray&, double time);

    static constexpr int kMaxHeld = 32;
    std::array<Held, kMaxHeld> held {};
    int numHeld = 0;
    std::array<bool, 128> down {};   // physically held keys (with latch on, held may be more)
    int numDown = 0;
    std::vector<int> cycle;          // reserved in initialise
    int position = 0;

    std::array<int, 16> sounding {};
    int numSounding = 0;
    double offBeat = 0;              // when the sounding notes stop
    long long lastStep = -1;
    double clockBeats = 0;           // the arp's own clock, when the transport is stopped
    double lastBlockEndBeat = -1;
    bool wasPlaying = false;
    juce::Random random;
    int channel = 1;
};

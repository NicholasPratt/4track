#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <3rd_party/rigtorp/MPMCQueue.h>
#include <array>
#include <atomic>
#include <mutex>

namespace te = tracktion;

/**
    Sends tracks' MIDI to hardware MIDI outputs, on time.

    Tracktion gives each track one output, so a MIDI track that plays its instrument (audio
    to the master) can't also send its notes to a hardware synth that way. MidiOutPlugin sits
    first in such a track's chain and copies the notes it sees (from clips and from the live
    input) to this sender, which plays them out of the chosen MIDI port at the moment they'll
    be heard: the audio output latency later, plus the port's own delay setting.

    The audio threads only push into a fixed-size queue; a timer thread sends the messages.
*/
class MidiOutSender : private juce::HighResolutionTimer
{
public:
    MidiOutSender();
    ~MidiOutSender() override;

    static constexpr int kMaxPorts = 32;

    /** Opens (or finds) a MIDI output by its device identifier; returns its port number, or -1. Message thread. */
    int openPort(const juce::String& identifier);
    /** Extra delay for a port, e.g. to line a slow synth up with the audio. Message thread. */
    void setPortDelayMs(int port, double ms);
    /** Time from now until audio being rendered now is heard. Message thread. */
    void setOutputLatencyMs(double ms) { latencyMs = ms; }

    /** Queues a message to go out `offsetSeconds` after the current block starts. Audio thread. */
    void send(int port, const juce::MidiMessage&, double offsetSeconds);
    /** Note-offs and sustain-off on every channel, straight away. Message thread. */
    void allNotesOff(int port);
    /** The same, queued to go out next, for the audio thread. */
    void queueAllNotesOff(int port);

private:
    void hiResTimerCallback() override;

    struct Event { double timeMs = 0; unsigned generation = 0; int port = -1; juce::uint8 data[3] {}; int size = 0; };
    static constexpr int kQueueSize = 8192;
    rigtorp::MPMCQueue<Event> queue { kQueueSize };
    // A reset must reach the port even when the event queue is full.
    std::array<std::atomic<unsigned>, kMaxPorts> generation {};
    std::array<unsigned, kMaxPorts> appliedGeneration {};   // protected by portLock
    void resetPortLocked(int port);
    std::vector<Event> pending;   // timer thread only

    std::mutex portLock;
    std::array<std::unique_ptr<juce::MidiOutput>, kMaxPorts> ports;
    std::array<juce::String, kMaxPorts> portIds;
    std::array<std::atomic<double>, kMaxPorts> portDelayMs {};
    std::atomic<double> latencyMs { 0.0 };
};

/**
    First plugin on a MIDI track that sends to hardware: copies the track's MIDI to a port of
    the MidiOutSender. With replaceInstrument on, the notes go only to the hardware: the
    plugin clears them so the instrument after it stays quiet.
*/
class MidiOutPlugin : public te::Plugin
{
public:
    explicit MidiOutPlugin(te::PluginCreationInfo);
    ~MidiOutPlugin() override;

    static const char* getPluginName() { return "MIDI Out"; }
    static const char* xmlTypeName;
    static juce::ValueTree create();

    juce::String getName() const override { return "MIDI Out"; }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return "MIDI Out"; }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return false; }
    bool canBeDisabled() override { return false; }
    int getNumOutputChannelsGivenInputs(int numInputs) override { return numInputs; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer(const te::PluginRenderContext&) override;

    /** The MIDI output's name (as Tracktion lists it), saved with the project. */
    juce::CachedValue<juce::String> deviceName;
    /** True: the hardware plays instead of the instrument. False: both play. */
    juce::CachedValue<bool> replaceInstrument;

    /** Points the plugin at the sender port for deviceName. Message thread. */
    void connect(MidiOutSender&, int port);

private:
    std::atomic<MidiOutSender*> sender { nullptr };
    std::atomic<int> port { -1 };
    std::atomic<bool> replace { false };
    bool wasPlaying = false;
};

#include "Arpeggiator.h"

namespace
{
    int sixteenth()
    {
        for (int i = 0; i < EffectPlugin::divisionNames().size(); ++i)
            if (std::abs(EffectPlugin::divisionBeats(i) - 0.25) < 1.0e-9)
                return i;
        return 0;
    }
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
}

const char* ArpeggiatorEffect::xmlTypeName = "eightTrackArpeggiator";

ArpeggiatorEffect::ArpeggiatorEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    mode     = addChoice("mode", "Mode", modeNames(), Up, "Arpeggio");
    rate     = addChoice("rate", "Rate", divisionNames(), sixteenth(), "Arpeggio");
    octaves  = addKnob("octaves", "Octaves", { 1.0f, 4.0f, 1.0f }, 1.0f, [](float v) { return juce::String(juce::roundToInt(v)); }, "Arpeggio");
    gate     = addKnob("gate", "Gate", { 0.05f, 1.0f, 0.01f }, 0.5f, percentText, "Arpeggio");
    swing    = addKnob("swing", "Swing", { 0.0f, 0.75f, 0.01f }, 0.0f, percentText, "Feel");
    velocity = addKnob("velocity", "Velocity", { 0.0f, 127.0f, 1.0f }, 0.0f,
                       [](float v) { return v < 0.5f ? juce::String("As Played") : juce::String(juce::roundToInt(v)); }, "Feel");
    latch    = addToggle("latch", "Latch", false, "Feel");
}

ArpeggiatorEffect::~ArpeggiatorEffect() { notifyListenersOfDeletion(); }

bool ArpeggiatorEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == octaves)
        return choiceOf(*mode) != Chord;
    return true;
}

void ArpeggiatorEffect::initialise(const te::PluginInitialisationInfo&)
{
    cycle.reserve((size_t) kMaxHeld * 4 * 2 + 2);
    reset();
}

void ArpeggiatorEffect::reset()
{
    numHeld = 0;
    down.fill(false);
    numDown = 0;
    numSounding = 0;
    cycle.clear();
    position = 0;
    lastStep = -1;
    clockBeats = 0;
    lastBlockEndBeat = -1;
}

void ArpeggiatorEffect::pattern(Mode m, const int* heldInOrder, int count, int octaveCount, std::vector<int>& out)
{
    out.clear();
    count = juce::jmin(count, kMaxHeld);
    if (count <= 0)
        return;
    if (m == Chord)
    {
        out.assign(heldInOrder, heldInOrder + count);
        return;
    }
    int notes[kMaxHeld];
    std::copy(heldInOrder, heldInOrder + count, notes);
    if (m != AsPlayed)
        std::sort(notes, notes + count);
    for (int o = 0; o < juce::jlimit(1, 4, octaveCount); ++o)
        for (int i = 0; i < count; ++i)
            if (notes[i] + 12 * o < 128)
                out.push_back(notes[i] + 12 * o);

    // `out` holds the notes going up; add the way back for the two-way modes.
    const int n = (int) out.size();
    switch (m)
    {
        case Down:
            std::reverse(out.begin(), out.end());
            break;
        case UpDown:      // C E G E: the top and bottom once each
            for (int i = n - 2; i >= 1; --i)
                out.push_back(out[(size_t) i]);
            break;
        case DownUp:      // G E C E
            std::reverse(out.begin(), out.end());
            for (int i = n - 2; i >= 1; --i)
                out.push_back(out[(size_t) i]);
            break;
        case UpAndDown:   // C E G G E C: the top and bottom twice
            for (int i = n - 1; i >= 0; --i)
                out.push_back(out[(size_t) i]);
            break;
        case Up: case Random: case AsPlayed: case Chord:
            break;
    }
}

void ArpeggiatorEffect::noteOn(int note, int vel)
{
    if (!down[(size_t) note])
    {
        down[(size_t) note] = true;
        ++numDown;
    }
    // With latch on, the first key after letting go of them all starts a new chord.
    if (isOn(*latch) && numDown == 1)
        numHeld = 0;
    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i].note == note)
            return;
    if (numHeld < kMaxHeld)
        held[(size_t) numHeld++] = { note, vel };
}

void ArpeggiatorEffect::noteOff(int note)
{
    if (down[(size_t) note])
    {
        down[(size_t) note] = false;
        --numDown;
    }
    if (isOn(*latch))
        return;
    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i].note == note)
        {
            for (int j = i; j < numHeld - 1; ++j)
                held[(size_t) j] = held[(size_t) j + 1];
            --numHeld;
            return;
        }
}

void ArpeggiatorEffect::rebuild()
{
    int notes[kMaxHeld];
    for (int i = 0; i < numHeld; ++i)
        notes[i] = held[(size_t) i].note;
    pattern((Mode) choiceOf(*mode), notes, numHeld, juce::roundToInt(octaves->getCurrentValue()), cycle);
}

void ArpeggiatorEffect::stopSounding(te::MidiMessageArray& midi, double time)
{
    for (int i = 0; i < numSounding; ++i)
        midi.addMidiMessage(juce::MidiMessage::noteOff(channel, sounding[(size_t) i]), time, {});
    numSounding = 0;
}

void ArpeggiatorEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.bufferForMidiMessages == nullptr)
        return;
    auto& midi = *fc.bufferForMidiMessages;
    const double blockSeconds = fc.bufferNumSamples / sampleRate;
    const double beatsPerSecond = getTempo() / 60.0;

    // Where this block is, in beats: the song's position when playing, else the arp's clock.
    double startBeat;
    const auto songBeat = getBeatPosition(fc);
    if (fc.isPlaying && songBeat)
        startBeat = *songBeat;
    else
    {
        if (fc.isPlaying != wasPlaying)
            clockBeats = 0;
        startBeat = clockBeats;
    }
    if (fc.isPlaying != wasPlaying || midi.isAllNotesOff
        || (lastBlockEndBeat >= 0 && std::abs(startBeat - lastBlockEndBeat) > 1.0e-6))
    {
        // A jump (a loop, a locate, start or stop): stop what's sounding, start the step count afresh.
        stopSounding(midi, 0.0);
        lastStep = -1;
        if (midi.isAllNotesOff)
            reset();
    }
    wasPlaying = fc.isPlaying;

    // Take in the keys played; let everything else (controllers, pitch bend) through.
    bool freshChord = false;
    midi.sortByTimestamp();
    for (int i = 0; i < midi.size(); ++i)
    {
        const auto& m = midi[i];
        if (m.isNoteOn())
        {
            if (numHeld == 0 || (isOn(*latch) && numDown == 0))
                freshChord = true;
            channel = m.getChannel();
            noteOn(m.getNoteNumber(), m.getVelocity());
        }
        else if (m.isNoteOff())
            noteOff(m.getNoteNumber());
    }
    midi.removeNoteOnsAndOffs();

    const double stepBeats = divisionBeats(choiceOf(*rate));
    const double endBeat = startBeat + blockSeconds * beatsPerSecond;
    if (!fc.isPlaying)
    {
        // Stopped: start the arp's clock from the first key, so the first note comes at once.
        if (freshChord && numSounding == 0)
        {
            clockBeats = 0;
            startBeat = 0;
            lastStep = -1;
        }
        clockBeats = startBeat + blockSeconds * beatsPerSecond;
    }
    const double blockEnd = fc.isPlaying ? endBeat : clockBeats;
    lastBlockEndBeat = blockEnd;
    auto toSeconds = [&](double beat) { return juce::jlimit(0.0, blockSeconds, (beat - startBeat) / beatsPerSecond); };

    // Rebuilt every block (it's a few notes, and allocates nothing), so Mode and Octaves
    // changes take effect on the next step.
    rebuild();
    if (position >= (int) cycle.size())
        position = 0;

    // Note-offs due in this block
    if (numSounding > 0 && offBeat < blockEnd)
        stopSounding(midi, toSeconds(offBeat));

    const double swingAmount = swing->getCurrentValue();
    auto stepStart = [&](long long k)
    {
        // Every second step is late by the swing (75% swing = a dotted feel).
        return (double) k * stepBeats + ((k & 1) ? swingAmount * stepBeats * 0.5 : 0.0);
    };

    long long k = (long long) std::floor(startBeat / stepBeats) - 1;
    for (; ; ++k)
    {
        const double at = stepStart(k);
        if (at >= blockEnd)
            break;
        if (at < startBeat - 1.0e-9 || k <= lastStep)
            continue;
        lastStep = k;
        if (numSounding > 0)
            stopSounding(midi, toSeconds(at));
        if (cycle.empty())
            continue;

        const auto m = (Mode) choiceOf(*mode);
        const int fixedVel = juce::roundToInt(velocity->getCurrentValue());
        auto velocityOf = [&](int note)
        {
            if (fixedVel > 0)
                return fixedVel;
            for (int i = 0; i < numHeld; ++i)
                if ((note - held[(size_t) i].note) % 12 == 0)
                    return held[(size_t) i].velocity;
            return 100;
        };
        const double t = toSeconds(at);
        if (m == Chord)
        {
            for (int n : cycle)
                if (numSounding < (int) sounding.size())
                {
                    midi.addMidiMessage(juce::MidiMessage::noteOn(channel, n, (juce::uint8) velocityOf(n)), t, {});
                    sounding[(size_t) numSounding++] = n;
                }
        }
        else
        {
            const int index = m == Random ? random.nextInt((int) cycle.size()) : position % (int) cycle.size();
            position = (position + 1) % juce::jmax(1, (int) cycle.size());
            const int n = cycle[(size_t) index];
            midi.addMidiMessage(juce::MidiMessage::noteOn(channel, n, (juce::uint8) velocityOf(n)), t, {});
            sounding[(size_t) numSounding++] = n;
        }
        const double length = (stepStart(k + 1) - at) * gate->getCurrentValue();
        offBeat = at + juce::jmax(length, 0.01);
        if (offBeat < blockEnd)
            stopSounding(midi, toSeconds(offBeat));
    }

    if (numHeld == 0)
    {
        position = 0;
        if (numSounding > 0)
            stopSounding(midi, 0.0);
    }
    midi.sortByTimestamp();
}

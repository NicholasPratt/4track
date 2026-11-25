#include "ChordPreview.h"

namespace
{
    int pitchClass(juce::String::CharPointerType& text)
    {
        constexpr int pitches[] = { 9, 11, 0, 2, 4, 5, 7 };
        const auto letter = juce::CharacterFunctions::toUpperCase(text.getAndAdvance());
        if (letter < 'A' || letter > 'G')
            return -1;
        int pitch = pitches[letter - 'A'];
        if (*text == '#' || *text == 0x266f)
        {
            ++pitch;
            ++text;
        }
        else if (*text == 'b' || *text == 0x266d)
        {
            --pitch;
            ++text;
        }
        return (pitch + 12) % 12;
    }
}

juce::Array<int> ChordPreview::notesFor(const juce::String& input)
{
    const auto chord = input.trim();
    if (chord.isEmpty())
        return {};
    auto text = chord.getCharPointer();
    const int root = pitchClass(text);
    if (root < 0)
        return {};

    const auto tail = juce::String(text).removeCharacters(" ");
    auto symbol = tail.upToFirstOccurrenceOf("/", false, false);
    if (symbol.isEmpty() || symbol == "maj") symbol = "M";
    else if (symbol == "min" || symbol == "-") symbol = "m";
    else if (symbol == "dim") symbol = "o";
    else if (symbol == "aug") symbol = "+";
    else if (symbol == "sus") symbol = "sus4";

    symbol = symbol.replace("maj", "M").replace("min", "m").replace("dim", "o").replace("aug", "+");
    if (symbol == "m7b5") symbol = tracktion::Chord(tracktion::Chord::halfDiminishedSeventhChord).getSymbol();

    juce::Array<int> notes;
    for (auto type : tracktion::Chord::getAllChordType())
    {
        const tracktion::Chord quality(type);
        if (quality.getSymbol() == symbol)
        {
            for (int interval : quality.getSteps())
                notes.add(60 + root + interval);
            break;
        }
    }
    if (notes.isEmpty())
        return {};

    if (tail.containsChar('/'))
    {
        const auto bass = tail.fromFirstOccurrenceOf("/", false, false);
        if (bass.isEmpty())
            return {};
        auto bassText = bass.getCharPointer();
        const int pitch = pitchClass(bassText);
        if (pitch < 0 || !bassText.isEmpty())
            return {};
        notes.insert(0, 48 + pitch);
    }
    return notes;
}

ChordPreview::~ChordPreview()
{
    if (deviceManager)
        deviceManager->removeAudioCallback(this);
}

void ChordPreview::setEnabled(juce::AudioDeviceManager& manager, bool enabled)
{
    if (enabled == isEnabled())
        return;
    if (enabled)
    {
        deviceManager = &manager;
        manager.addAudioCallback(this);
    }
    else
    {
        manager.removeAudioCallback(this);
        deviceManager = nullptr;
        command.store(0);
    }
}

bool ChordPreview::play(const juce::String& chord)
{
    if (!isEnabled() || deviceManager->getCurrentAudioDevice() == nullptr)
        return false;
    const auto notes = notesFor(chord);
    std::uint64_t next = 0;
    for (int i = 0; i < juce::jmin(maxNotes, notes.size()); ++i)
        next |= static_cast<std::uint64_t>(notes[i]) << (8 * i);
    if (next != 0)
        command.store(next);
    return next != 0;
}

void ChordPreview::stop()
{
    command.store(stopCommand);
}

void ChordPreview::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    rate = device->getCurrentSampleRate();
    noteCount = 0;
    command.store(0);
}

void ChordPreview::audioDeviceStopped()
{
    noteCount = 0;
    command.store(0);
}

void ChordPreview::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* output,
                                                     int channels, int frames, const juce::AudioIODeviceCallbackContext&)
{
    const auto next = command.exchange(0);
    if (next != 0)
    {
        elapsed = 0;
        noteCount = 0;
        for (int i = 0; next != stopCommand && i < maxNotes; ++i)
        {
            const int note = static_cast<int>((next >> (8 * i)) & 0xff);
            if (note == 0)
                break;
            const double angle = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz(note) / rate;
            voices[static_cast<size_t>(noteCount++)] = { 0, 1, std::sin(angle), std::cos(angle) };
        }
    }

    const int attack = juce::jmax(1, static_cast<int>(rate * 0.015));
    const int duration = static_cast<int>(rate * 0.55);
    for (int i = 0; i < frames; ++i)
    {
        float sample = 0;
        if (noteCount > 0)
        {
            double sum = 0;
            for (int n = 0; n < noteCount; ++n)
            {
                auto& v = voices[static_cast<size_t>(n)];
                sum += v.sine;
                const auto sine = v.sine * v.stepCos + v.cosine * v.stepSin;
                v.cosine = v.cosine * v.stepCos - v.sine * v.stepSin;
                v.sine = sine;
            }
            const double envelope = juce::jmin(1.0, (double) elapsed / attack) * (1.0 - (double) elapsed / duration);
            sample = static_cast<float>(sum * envelope * 0.12 / noteCount);
            if (++elapsed >= duration)
                noteCount = 0;
        }

        for (int ch = 0; ch < channels; ++ch)
            if (output[ch] != nullptr)
                output[ch][i] = sample;
    }
}

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

/**
    View of one clip (audio or MIDI) on the timeline, in samples at the current device rate.
    Built from the Tracktion Edit (see Project) and edited by the Arrangement
    operations; the result is written back to the Edit. Cheap to copy.
*/
struct Clip
{
    juce::int64 id = 0;            // the Tracktion clip's EditItemID, or a new id for a clip not in the Edit yet
    juce::File file;               // the audio file the clip plays
    juce::int64 sourceLength = 0;  // length of that file
    juce::int64 start = 0;         // timeline position
    juce::int64 sourceOffset = 0;  // first sample of the file that is heard
    juce::int64 length = 0;
    juce::int64 fadeIn = 0;
    juce::int64 fadeOut = 0;
    float gain = 1.0f;

    // MIDI clips have no file: they play the notes stored in the Edit. A MIDI clip made by
    // an edit (e.g. the right half of a split) is created from a copy of midiState, the
    // state of the clip it came from.
    juce::Array<juce::File> takes;
    int activeTake = 0;
    bool midi = false;
    juce::ValueTree midiState;
    static constexpr juce::int64 kMidiSourceLength = (juce::int64) 1 << 50;   // MIDI clips can be extended freely

    juce::int64 end() const { return start + length; }

    /** Gain from fades at a sample index relative to the clip start. */
    float fadeGainAt(juce::int64 rel) const
    {
        float g = 1.0f;
        if (fadeIn > 0 && rel < fadeIn)
            g = (float) rel / (float) fadeIn;
        const auto fromEnd = length - rel;
        if (fadeOut > 0 && fromEnd < fadeOut)
            g = juce::jmin(g, (float) fromEnd / (float) fadeOut);
        return g;
    }
};

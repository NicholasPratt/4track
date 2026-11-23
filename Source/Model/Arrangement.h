#pragma once

#include "Clip.h"
#include <array>
#include <vector>

static constexpr int kNumTracks = 8;

/**
    The editable timeline: kNumTracks lanes, each a list of non-overlapping clips
    sorted by start time. A value type: copying it is cheap (clips share their
    audio), which is what undo/redo and the audio-thread snapshot rely on.

    Every edit keeps the lanes non-overlapping. Anything placed on top of existing
    clips (a new take, a moved clip) trims or splits what is underneath. The
    underlying audio is never modified, so undo restores everything.
*/
struct Arrangement
{
    std::array<std::vector<Clip>, kNumTracks> tracks;
    juce::int64 nextClipId = 1;

    static constexpr juce::int64 kMinClipLength = 64;
    static constexpr juce::int64 kEdgeFade = 128;   // anti-click fade for new edges

    // Queries
    const Clip* findClip(juce::int64 id, int* trackOut = nullptr) const;
    juce::int64 getEnd() const;   // end of the last clip on any track
    bool isEmpty() const;

    // Edits. Each returns true if something changed.
    juce::int64 insertClip(int track, Clip clip);   // returns the new clip id
    bool removeClip(juce::int64 id);
    bool moveClip(juce::int64 id, int newTrack, juce::int64 newStart);
    bool trimStart(juce::int64 id, juce::int64 newStart);
    bool trimEnd(juce::int64 id, juce::int64 newEnd);
    bool splitClip(juce::int64 id, juce::int64 position);
    bool setFades(juce::int64 id, juce::int64 fadeIn, juce::int64 fadeOut);
    bool setGain(juce::int64 id, float gain);

    /** Removes the given range from a lane, trimming or splitting clips that overlap it. */
    void clearRange(int track, juce::int64 rangeStart, juce::int64 rangeEnd);

    /** Converts all clip positions after the engine sample rate changes. */
    void rescalePositions(double ratio);

private:
    Clip* findMutable(juce::int64 id, int* trackOut = nullptr);
    void sortTrack(int track);
    static void clampFades(Clip& c);
};

/** Linear undo/redo over whole Arrangement snapshots. */
class EditHistory
{
public:
    void push(const Arrangement& before)
    {
        undoStack.push_back(before);
        if (undoStack.size() > kMaxDepth)
            undoStack.erase(undoStack.begin());
        redoStack.clear();
    }

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    bool undo(Arrangement& current)
    {
        if (undoStack.empty()) return false;
        redoStack.push_back(current);
        current = undoStack.back();
        undoStack.pop_back();
        return true;
    }

    bool redo(Arrangement& current)
    {
        if (redoStack.empty()) return false;
        undoStack.push_back(current);
        current = redoStack.back();
        redoStack.pop_back();
        return true;
    }

    void clear() { undoStack.clear(); redoStack.clear(); }

private:
    static constexpr size_t kMaxDepth = 200;
    std::vector<Arrangement> undoStack, redoStack;
};

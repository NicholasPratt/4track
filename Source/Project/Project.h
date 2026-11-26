#pragma once

#include "../Audio/AudioEngine.h"
#include "../Model/Arrangement.h"
#include "../Model/DrumPattern.h"
#include <functional>
#include <map>
#include <optional>
#include <set>

/**
    The open project: a Tracktion Edit plus the folder it lives in.

    On disk a project is a folder:
        MySong/
          project.8track   (a Tracktion Edit file: clips, mixer, tempo, punch range)
          Audio/           (one file per recorded take or imported file)

    Older project.8track / project.4track files (8track's own XML format, from before
    Tracktion) are imported when opened and saved in the new format.

    The UI sees the clips as an Arrangement (samples, one lane per track). Edits are
    made on a copy of that Arrangement with its tested operations (move, trim, split,
    fade, gain), then written back to the Edit as one undo step. The Edit is the
    single source of truth: the Arrangement is rebuilt from it after every change,
    including undo, redo and recording.

    Audio files are never modified or deleted by edits, so undo can always restore anything.
*/
class Project : private juce::ValueTree::Listener,
                private juce::AsyncUpdater,
                private juce::ChangeListener
{
public:
    explicit Project(AudioEngine& engine);
    ~Project() override;

    const Arrangement& getArrangement() const { return arrangement; }
    te::Edit& getEdit() { return *edit; }

    /** Runs an edit on the arrangement as a single undo step. The edit returns false if it changed nothing. */
    bool perform(const std::function<bool(Arrangement&)>& edit);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

    /** Makes sure the project folder and its Edit file exist on disk. Tracktion stores
        clip paths relative to the Edit file, and can only do that correctly once the file
        exists, so call this before audio is recorded into the project. */
    bool prepareFolder(juce::String& error);

    /** Adds an empty MIDI clip to a MIDI track, as one undo step. Returns its Tracktion clip, or nullptr. */
    te::MidiClip* createMidiClip(int track, juce::int64 start, juce::int64 length);

    /** The Tracktion MIDI clip for an Arrangement clip id, for editing its notes. */
    te::MidiClip* getMidiClip(juce::int64 clipId) const;

    DrumPattern getDrumPattern(int slot) const;
    void setDrumPattern(int slot, DrumPattern);
    std::optional<te::TimeRange> drumPatternDropRange(int track, int slot, te::BeatPosition start) const;
    te::MidiClip* placeDrumPattern(int track, int slot, te::BeatPosition start);
    static juce::String drumPatternName(int slot) { return "Drum Pattern " + juce::String(slot + 1); }

    // Clip operations on a selection, each one undo step.
    /** True if some track has two or more of these clips, so joinClips would do something. */
    bool canJoin(const std::set<juce::int64>& ids) const;
    /** Joins the clips on each track into one clip, from the first one's start to the last
        one's end. Audio clips are rendered (with their gain and fades, silence in any gaps)
        into a new file in the project's Audio folder; MIDI clips are merged. Returns the
        joined clips' ids, or nothing (with error set) if it couldn't. */
    std::set<juce::int64> joinClips(const std::set<juce::int64>& ids, juce::String& error);
    /** Sets each audio clip's gain so its loudest sample peaks at 0 dBFS (up to +40 dB).
        The audio file isn't changed. */
    bool normaliseClips(const std::set<juce::int64>& ids);
    /** Moves every note in these MIDI clips by this many semitones (kept within 0..127). */
    bool transposeClips(const std::set<juce::int64>& ids, int semitones);

    /** Copies an audio file into the project and places it as a clip. */
    bool importAudio(const juce::File& file, int track, juce::int64 position, juce::String& error);

    void chooseTake(juce::int64 clipID, int index);
    bool canRecall(int track) const;
    bool recall(int track, juce::String& error);
    juce::Result addSection(const juce::String&, te::TimeRange);
    void duplicateSection(te::ArrangerClip&);

    enum class SongEntry { Chord, Lyric };
    struct SongNote { te::BeatPosition beat; juce::String text; juce::ValueTree entry; };
    std::vector<SongNote> songNotes(SongEntry) const;
    // An existing lyric is edited by identity, so other lines at the same beat stay intact.
    void setSongNote(SongEntry, te::BeatPosition, const juce::String&, juce::ValueTree entry = {});
    void moveLyric(juce::ValueTree lyric, te::BeatPosition);
    juce::ValueTree adjacentLyric(juce::ValueTree lyric, int direction) const;
    void reorderLyric(juce::ValueTree lyric, int direction);
    void mergeLyric(juce::ValueTree lyric, int direction);
    juce::String getLyricDraft() const;
    void setLyricDraft(const juce::String&);
    juce::String timelineLyricText() const;
    juce::String lyricSheetText() const;
    juce::int64 songNotesEnd() const;
    te::BeatDuration chordDuration(te::BeatPosition source) const;
    bool canPlaceChord(int track, te::TimeRange) const;
    te::MidiClip* placeChord(int track, te::TimeRange, const juce::String& symbol, bool allowConversion);

    // Files
    bool isUntitled() const { return untitled; }
    bool isDirty() const { return dirty; }
    juce::File getFolder() const { return folder; }
    juce::String getName() const { return untitled ? juce::String("Untitled") : folder.getFileName(); }
    juce::File getAudioFolder() const { return folder.getChildFile("Audio"); }

    void newProject();
    bool save(juce::String& error);                    // to the current folder (must not be untitled)
    bool saveAs(const juce::File& newFolder, juce::String& error);
    bool load(const juce::File& folderOrFile, juce::String& error);

    struct ExportSettings
    {
        enum class Format { wav, mp3 };
        Format format = Format::wav;
        int wavBitDepth = 24;
        int mp3BitrateKbps = 192;

        static constexpr int wavBitDepths[] { 16, 24, 32 };
        static constexpr int mp3Bitrates[] { 64, 96, 128, 160, 192, 224, 256, 320 };
        const char* extension() const { return format == Format::mp3 ? "mp3" : "wav"; }
    };

    /** Renders the whole arrangement, with a reverb tail. */
    bool exportMix(const juce::File& file, juce::String& error);
    bool exportMix(const juce::File& file, const ExportSettings&, juce::String& error);

    /** Call when the device sample rate changes: clip positions are re-read at the new rate. */
    void handleSampleRateChange() { rebuild(); }

    /** Runs slow file work (rendering, copying, scanning audio) that doesn't touch the Edit.
        The UI sets this to run it on a background thread behind a busy indicator; by
        default it just runs the work. */
    std::function<void(const juce::String& message, const std::function<void()>& work)> runInBackground;

    /** Called after anything changes that the UI should redraw. */
    std::function<void()> onChanged;

    /** Marks the project as changed. */
    void markDirty();

    static constexpr const char* kProjectFileName = "project.8track";
    static constexpr const char* kLegacyProjectFileName = "project.4track";

private:
    bool canPlaceMidiRegion(int track, te::TimeRange, bool allowEmptyAudioTrack) const;
    void setEdit(std::unique_ptr<te::Edit> newEdit, bool fresh);
    void rebuild();
    void writeToEdit(const Arrangement& target);
    bool importLegacy(const juce::XmlElement& xml, const juce::File& projectFolder, juce::String& error);
    juce::File makeUntitledFolder() const;
    juce::File editFile() const { return folder.getChildFile(kProjectFileName); }
    juce::int64 fileLength(const juce::File&) const;
    static bool renderClips(const std::vector<Clip>& clips, const juce::File& dest, double sampleRate,
                            te::AudioFileFormatManager&, juce::String& error);
    static float peakOf(const Clip&, juce::AudioFormatManager&, double sampleRate);
    void background(const juce::String& message, const std::function<void()>& work);

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void editTreeChanged(const juce::ValueTree&);
    void handleAsyncUpdate() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    AudioEngine& engine;
    std::unique_ptr<te::Edit> edit;
    juce::ValueTree editState;
    Arrangement arrangement;
    juce::File folder;
    bool untitled = true;
    bool dirty = false;
    bool settingUp = false;   // ignore Edit changes made while attaching a new Edit
    mutable std::map<juce::String, juce::int64> lengthCache;   // file path @ rate -> length in samples
};

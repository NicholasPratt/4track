#pragma once

#include "../Audio/AudioEngine.h"
#include "../Model/Arrangement.h"
#include <functional>
#include <map>

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

    /** Copies an audio file into the project and places it as a clip. */
    bool importAudio(const juce::File& file, int track, juce::int64 position, juce::String& error);

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

    /** Renders the whole arrangement, with a reverb tail, to a WAV file. */
    bool exportMix(const juce::File& file, juce::String& error);

    /** Call when the device sample rate changes: clip positions are re-read at the new rate. */
    void handleSampleRateChange() { rebuild(); }

    /** Called after anything changes that the UI should redraw. */
    std::function<void()> onChanged;

    /** Marks the project as changed. */
    void markDirty();

    static constexpr const char* kProjectFileName = "project.8track";
    static constexpr const char* kLegacyProjectFileName = "project.4track";

private:
    void setEdit(std::unique_ptr<te::Edit> newEdit, bool fresh);
    void rebuild();
    void writeToEdit(const Arrangement& target);
    bool importLegacy(const juce::XmlElement& xml, const juce::File& projectFolder, juce::String& error);
    juce::File makeUntitledFolder() const;
    juce::File editFile() const { return folder.getChildFile(kProjectFileName); }
    juce::int64 fileLength(const juce::File&) const;

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

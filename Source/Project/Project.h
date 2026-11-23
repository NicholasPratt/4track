#pragma once

#include "../Audio/AudioEngine.h"
#include "../Model/Arrangement.h"
#include <functional>

/**
    Message-thread owner of the session: the arrangement and its undo history, the
    project folder on disk, and saving/loading.

    On disk a project is a folder:
        MySong/
          project.8track   (XML: tempo, mixer, clips; project.4track from older versions also loads)
          Audio/           (one WAV per recorded take or imported file)

    Audio files are never modified or deleted by edits. Clips only point into them,
    so undo can always restore anything.
*/
class Project
{
public:
    explicit Project(AudioEngine& engine);

    const Arrangement& getArrangement() const { return arrangement; }

    /** Runs an edit as a single undo step. The edit returns false if it changed nothing. */
    bool perform(const std::function<bool(Arrangement&)>& edit);
    bool undo();
    bool redo();
    bool canUndo() const { return history.canUndo(); }
    bool canRedo() const { return history.canRedo(); }

    /** Adds finished recordings as clips (one undo step). */
    void addTakes(const std::vector<AudioEngine::FinishedTake>& takes);

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

    /** Re-renders a mixdown of the whole arrangement to a WAV file. */
    bool exportMix(const juce::File& file, juce::String& error);

    /** Call when the device sample rate changes: resamples audio and rescales positions. */
    void handleSampleRateChange();

    /** Called after anything changes that the UI should redraw. */
    std::function<void()> onChanged;

    /** Marks the project as changed (for mixer/tempo changes, which aren't undoable). */
    void markDirty();

    static constexpr const char* kProjectFileName = "project.8track";
    static constexpr const char* kLegacyProjectFileName = "project.4track";

private:
    void publish();
    void changed();
    std::unique_ptr<juce::XmlElement> toXml() const;
    juce::File makeUntitledFolder() const;

    AudioEngine& engine;
    Arrangement arrangement;
    EditHistory history;
    juce::File folder;
    bool untitled = true;
    bool dirty = false;
    double positionsRate = 0.0;   // sample rate that clip positions are expressed in
};

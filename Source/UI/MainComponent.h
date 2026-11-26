#pragma once

#include "ArrangeView.h"
#include "MixerPanel.h"
#include "PianoRoll.h"
#include "PluginWindows.h"
#include "TransportBar.h"

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::KeyListener,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::FocusChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void parentHierarchyChanged() override;

    /** Asks to save unsaved changes, then calls `quit` unless the user cancels. */
    void requestQuit(std::function<void()> quit);

   #if JUCE_DEBUG
    /** Dev aid: `8track --snapshot out.png [--import file.wav ...] [--mixer]` renders the UI to a PNG and quits. */
    void runDebugSnapshot(const juce::StringArray& args);
    /** Dev aid: `8track --diagnose` prints the audio device setup and input levels, then quits. */
    void runDiagnostics();
   #endif

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int topLevelMenuIndex) override;

    // KeyListener for keys that reach the main editor.
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    using juce::Component::keyPressed;

private:
    void perform(int command);
    void perform(int command, Shortcuts::Context);
    Shortcuts::Context shortcutContext() const;
    void globalFocusChanged(juce::Component*) override { menuItemsChanged(); }
    void movePlayhead(int direction, bool byBar);
    void showShortcutHelp();
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void togglePlay();
    void toggleRecord();
    void splitSelection();
    void deleteSelection();
    void copySelection();
    void paste();
    /** Copies the selection straight after itself, `times` times in a row, as one undo step. */
    void duplicateSelection(int times = 1);
    /** Asks how many copies to make, then duplicates. */
    void askToDuplicate();
    int lastDuplicateCount = 2;
    /** Pastes the clips at position; times > 1 pastes them that many times end to end. */
    void pasteClips(const std::vector<ClipboardClip>&, int track, juce::int64 position, bool movePlayhead, int times = 1);
    void setPunchPoint(bool in);
    void setRangeMode(AudioEngine::RangeMode);
    void toggleMixer();
    DrumSampler* selectedDrumSampler() const;
    void openDrumPatterns();
    void openPianoRoll(juce::int64 clipId);
    void closePianoRoll();

    void confirmDiscard(std::function<void()> proceed);
    void saveProject(bool chooseLocation, std::function<void(bool saved)> then = {});
    void openProject();
    void importAudio();
    void exportMix();
    void chooseExportFile(Project::ExportSettings);
    void showAudioSettings();
    void showMidiSettings();
    void projectChanged();
    void showError(const juce::String& title, const juce::String& message);

    AudioEngine engine { std::make_unique<PluginWindowBehaviour>() };
    Project project { engine };

    std::unique_ptr<ArrangeView> arrange;
    std::unique_ptr<TransportBar> transport;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<PianoRoll> pianoRoll;   // replaces the mixer in the bottom panel while open
    std::unique_ptr<juce::MenuBarComponent> menuBar;   // non-mac only
    juce::TooltipWindow tooltips { this, 600 };
    std::shared_ptr<juce::FileChooser> chooser;

    std::vector<ClipboardClip> clipboard;

    bool mixerVisible = false;
    bool pianoRollVisible = false;
    juce::Component::SafePointer<juce::Component> shortcutHost;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

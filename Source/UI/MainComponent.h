#pragma once

#include "ArrangeView.h"
#include "MixerPanel.h"
#include "PluginWindows.h"
#include "TransportBar.h"

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::KeyListener,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
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

    // KeyListener (on the top-level window, so shortcuts work wherever focus is)
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    using juce::Component::keyPressed;

private:
    enum Command
    {
        cmdNew = 1, cmdOpen, cmdSave, cmdSaveAs, cmdImport, cmdExport, cmdAudioSettings, cmdPlugins,
        cmdUndo, cmdRedo, cmdSplit, cmdDelete, cmdSelectAll,
        cmdPlayStop, cmdRecord, cmdToStart, cmdPunch, cmdPunchIn, cmdPunchOut, cmdClick, cmdCountIn,
        cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdMixer
    };

    void perform(int command);
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void togglePlay();
    void toggleRecord();
    void splitSelection();
    void deleteSelection();
    void setPunchPoint(bool in);
    void toggleMixer();

    void confirmDiscard(std::function<void()> proceed);
    void saveProject(bool chooseLocation, std::function<void(bool saved)> then = {});
    void openProject();
    void importAudio();
    void exportMix();
    void showAudioSettings();
    void projectChanged();
    void showError(const juce::String& title, const juce::String& message);

    AudioEngine engine { std::make_unique<PluginWindowBehaviour>() };
    Project project { engine };

    std::unique_ptr<ArrangeView> arrange;
    std::unique_ptr<TransportBar> transport;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<juce::MenuBarComponent> menuBar;   // non-mac only
    juce::TooltipWindow tooltips { this, 600 };
    std::shared_ptr<juce::FileChooser> chooser;

    bool mixerVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

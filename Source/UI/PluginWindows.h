#pragma once

#include "../Audio/AudioEngine.h"
#include "../Audio/RecordingThumbnail.h"
#include <juce_gui_extra/juce_gui_extra.h>

class Project;
class DrumSampler;

/** Tells Tracktion how to open a plugin's editor: in its own window, like other DAWs. */
class PluginWindowBehaviour : public AudioUIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow(te::PluginWindowState&) override;
    void recreatePluginWindowContentAsync(te::Plugin&) override;
    /** The engine plugin windows assign MIDI controllers through. Set once at start-up. */
    static void setEngine(AudioEngine*, Project*);
    static void showDrumPatterns(DrumSampler&);

    /** Tracktion's long tasks (rendering an export) run behind the busy indicator. */
    void runTaskWithProgressBar(te::ThreadPoolJobWithProgress&) override;
};

/** Opens the window that scans for, lists and removes installed VST3/AU plugins. */
void showPluginManager(AudioEngine&);

/** Whether the plugin menus list third-party (VST3/AU) plugins as well as 8track's own.
    A machine setting, kept between launches. */
bool showsThirdPartyPlugins(AudioEngine&);
void setShowsThirdPartyPlugins(AudioEngine&, bool);

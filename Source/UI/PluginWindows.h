#pragma once

#include "../Audio/AudioEngine.h"
#include <juce_gui_extra/juce_gui_extra.h>

/** Tells Tracktion how to open a plugin's editor: in its own window, like other DAWs. */
class PluginWindowBehaviour : public te::UIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow(te::PluginWindowState&) override;
    void recreatePluginWindowContentAsync(te::Plugin&) override;
};

/** Opens the window that scans for, lists and removes installed VST3/AU plugins. */
void showPluginManager(AudioEngine&);

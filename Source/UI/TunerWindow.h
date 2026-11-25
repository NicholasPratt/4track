#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class AudioEngine;
std::unique_ptr<juce::DocumentWindow> openTunerWindow(AudioEngine&, juce::Rectangle<int> anchor);

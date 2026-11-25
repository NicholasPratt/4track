#pragma once

#include "../Project/Project.h"

struct LyricLineDrag final : juce::ReferenceCountedObject
{
    LyricLineDrag(juce::ValueTree editState, juce::String line) : edit(std::move(editState)), text(std::move(line)) {}
    const juce::ValueTree edit;
    const juce::String text;
};

class LyricEditor;

class LyricEditorWindow final : public juce::DocumentWindow
{
public:
    LyricEditorWindow(Project&, juce::Rectangle<int> anchor);
    void refreshProject();
    void closeButtonPressed() override;
    void visibilityChanged() override;

private:
    LyricEditor* editor = nullptr; // owned by the window's content
};

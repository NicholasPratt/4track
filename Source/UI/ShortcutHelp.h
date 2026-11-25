#pragma once

#include "Shortcuts.h"
#include "Theme.h"

class ShortcutHelp final : public juce::Component, private juce::ListBoxModel
{
public:
    ShortcutHelp();

    void resized() override;

private:
    void updateResults();
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;

    struct Row
    {
        juce::String action;
        juce::String keys;
        bool heading = false;
    };
    std::vector<Row> rows;
    juce::Label hint, empty;
    juce::TextEditor search;
    juce::ComboBox scope;
    juce::ListBox list { "Keyboard shortcuts", this };
};

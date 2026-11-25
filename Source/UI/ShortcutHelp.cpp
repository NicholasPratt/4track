#include "ShortcutHelp.h"

namespace
{
    using Shortcuts::Context;

    constexpr std::pair<Context, const char*> sections[] {
        { Context::Global, "General" },
        { Context::Arrangement, "Arrangement" },
        { Context::PianoRoll, "Piano Roll" },
        { Context::Songwriting, "Songwriting" }
    };

    juce::String normaliseKeys(const juce::String& text)
    {
        return text.toLowerCase().replace("command", "cmd").replace("control", "ctrl").removeCharacters(" ");
    }
}

ShortcutHelp::ShortcutHelp()
{
    hint.setText("Click an editor or lane to give it keyboard focus.", juce::dontSendNotification);
    hint.setColour(juce::Label::textColourId, Theme::textDim);
    hint.setFont(Theme::font(13));
    addAndMakeVisible(hint);

    search.setTitle("Search shortcuts");
    search.setComponentID("shortcut-search");
    search.setTextToShowWhenEmpty("Search shortcuts", Theme::textDim);
    search.onTextChange = [this] { updateResults(); };
    search.onEscapeKey = [this]
    {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState(0);
    };
    addAndMakeVisible(search);

    scope.setTitle("Shortcut context");
    scope.addItem("All contexts", 1);
    for (size_t i = 0; i < std::size(sections); ++i)
        scope.addItem(sections[i].second, static_cast<int>(i) + 2);
    scope.setSelectedId(1, juce::dontSendNotification);
    scope.onChange = [this] { updateResults(); };
    addAndMakeVisible(scope);

    list.setRowHeight(32);
    list.setColour(juce::ListBox::backgroundColourId, Theme::bg);
    addAndMakeVisible(list);
    empty.setText("No matching shortcuts", juce::dontSendNotification);
    empty.setColour(juce::Label::textColourId, Theme::textDim);
    empty.setJustificationType(juce::Justification::centred);
    empty.setInterceptsMouseClicks(false, false);
    addChildComponent(empty);
    setSize(580, 610);
    updateResults();
}

void ShortcutHelp::resized()
{
    auto area = getLocalBounds().reduced(18);
    hint.setBounds(area.removeFromTop(24));
    auto controls = area.removeFromTop(32);
    scope.setBounds(controls.removeFromRight(155));
    search.setBounds(controls.withTrimmedRight(10));
    area.removeFromTop(12);
    list.setBounds(area);
    empty.setBounds(area.removeFromTop(58));
}

void ShortcutHelp::updateResults()
{
    rows.clear();
    const auto filter = search.getText().trim();
    const auto keyFilter = normaliseKeys(filter);
    for (size_t i = 0; i < std::size(sections); ++i)
    {
        if (scope.getSelectedId() > 1 && scope.getSelectedItemIndex() != static_cast<int>(i) + 1)
            continue;

        const auto [context, section] = sections[i];
        const auto heading = rows.size();
        for (const auto& binding : Shortcuts::bindings)
        {
            if (binding.context != context)
                continue;
            const auto keystrokes = Shortcuts::keys(binding);
            const juce::String action(binding.description);
            auto plainKeys = juce::KeyPress(binding.key, juce::ModifierKeys(binding.modifiers), 0).getTextDescription();
            if (binding.alternate != 0)
                plainKeys += " " + juce::KeyPress(binding.alternate, juce::ModifierKeys(binding.alternateModifiers), 0).getTextDescription();
            if (filter.isNotEmpty() && !action.containsIgnoreCase(filter) && !keystrokes.containsIgnoreCase(filter)
                && !normaliseKeys(plainKeys).contains(keyFilter) && !juce::String(section).containsIgnoreCase(filter))
                continue;
            if (rows.size() == heading)
                rows.push_back({ section, {}, true });
            if (rows.back().action == action)
                rows.back().keys += "  /  " + keystrokes;
            else
                rows.push_back({ action, keystrokes });
        }
    }

    list.deselectAllRows();
    list.updateContent();
    list.scrollToEnsureRowIsOnscreen(0);
    list.repaint();
    empty.setVisible(rows.empty());
}

int ShortcutHelp::getNumRows()
{
    return static_cast<int>(rows.size());
}

juce::String ShortcutHelp::getNameForRow(int row)
{
    if (!juce::isPositiveAndBelow(row, getNumRows()))
        return {};
    const auto& item = rows[static_cast<size_t>(row)];
    return item.heading ? item.action : item.action + "   " + item.keys;
}

void ShortcutHelp::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (!juce::isPositiveAndBelow(row, getNumRows()))
        return;

    const auto& item = rows[static_cast<size_t>(row)];
    auto area = juce::Rectangle<int>(0, 0, width, height).reduced(12, 0);
    if (item.heading)
    {
        g.fillAll(Theme::panelLight);
        g.setColour(Theme::text);
        g.setFont(Theme::font(13, true));
        g.drawText(item.action, area, juce::Justification::centredLeft);
        return;
    }

    if (selected)
        g.fillAll(Theme::accent.withAlpha(0.2f));
    else if (row % 2 != 0)
        g.fillAll(Theme::laneA);
    auto keyArea = area.removeFromRight(176).reduced(3, 5);
    g.setColour(Theme::panelLight);
    g.fillRoundedRectangle(keyArea.toFloat(), 4);
    g.setColour(Theme::text);
    g.setFont(Theme::font(13));
    g.drawFittedText(item.action, area.withTrimmedRight(8), juce::Justification::centredLeft, 1);
    g.drawFittedText(item.keys, keyArea.reduced(6, 0), juce::Justification::centred, 1);
}

#pragma once

#include "../Audio/Effects.h"
#include <juce_gui_basics/juce_gui_basics.h>

class AudioEngine;

/**
    An effect's controls, in titled rows: knobs, menus and switches for its parameters, plus
    its selectors (e.g. the sidechain source) and file slots (e.g. an impulse response).
    Controls follow automation and undo, and dim when the current settings make them do
    nothing. Long rows wrap. Custom editors use it for some of the rows (`include`).
*/
class EffectControls : public juce::Component, private juce::Timer
{
public:
    EffectControls(EffectPlugin&, std::function<bool(const juce::String& row)> include = {});
    ~EffectControls() override;

    /** The height for this width, after wrapping. */
    int heightForWidth(int width) const;
    /** The width that fits the widest row without wrapping (capped at kMaxColumns). */
    int naturalWidth() const;

    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kCellWidth = 86, kRowTitle = 18, kRowHeight = 96, kMaxColumns = 8;

    class Item : public juce::Component
    {
    public:
        virtual void update() = 0;
        int cells = 1;
    };

private:
    struct Row { juce::String name; std::vector<Item*> items; };
    void timerCallback() override;
    int linesFor(const Row&, int columns) const;
    int columnsFor(int width) const;

    EffectPlugin& effect;
    juce::OwnedArray<Item> items;
    std::vector<Row> rows;
};

/**
    The window for one of 8track's own effects: a live reading (the gate opening, the pan
    moving, the cutoff sweeping) when the effect has one, above all its controls.
*/
class EffectEditor : public te::Plugin::EditorComponent, private juce::Timer
{
public:
    explicit EffectEditor(EffectPlugin&);
    ~EffectEditor() override;

    bool allowWindowResizing() override { return false; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Gives every EffectPlugin its window. Call once at start-up; the engine is for the
        instruments that record from an input. */
    static void install(AudioEngine*);

    static constexpr int kMargin = 14, kDisplayHeight = 34;

    /** Draws an effect's live reading as a bar (or a dot, for a bipolar one). */
    static void paintDisplay(juce::Graphics&, juce::Rectangle<int>, const EffectPlugin::Display&);

private:
    void timerCallback() override;
    juce::Rectangle<int> displayBounds() const;

    EffectPlugin& effect;
    EffectControls controls;
    EffectPlugin::Display display;
};

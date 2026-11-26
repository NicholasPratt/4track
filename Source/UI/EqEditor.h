#pragma once

#include "EffectEditor.h"
#include "../Audio/EqDeEsser.h"
#include <juce_dsp/juce_dsp.h>

/**
    The EQ's window: the curve over a live spectrum (what comes in, dim, and what goes out),
    with a dot for each band to drag: across for frequency, up and down for gain, the mouse
    wheel for Q, and a double-click to switch the band on or off. The buttons 1 to 5 (or
    clicking a dot) choose the band whose controls show below.
*/
class EqEditor : public te::Plugin::EditorComponent, private juce::Timer
{
public:
    explicit EqEditor(EqEffect&);
    ~EqEditor() override;

    bool allowWindowResizing() override { return false; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }
    void paint(juce::Graphics&) override;
    void resized() override;

    void select(int band);
    int getSelected() const { return selected; }

    static juce::Colour bandColour(int band);
    static constexpr int kGraphHeight = 260, kMargin = 14, kButtons = 26;

private:
    class Graph;
    void timerCallback() override;
    void layout();

    EqEffect& eq;
    std::unique_ptr<Graph> graph;
    juce::OwnedArray<juce::TextButton> bandButtons;
    std::unique_ptr<EffectControls> bandControls;
    EffectControls outputControls;
    int selected = 2;
};

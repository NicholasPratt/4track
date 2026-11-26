#pragma once

#include "../Project/Project.h"

class DrumSampler;

/** A bank of 16-pad step patterns. Dragging a slot places a copy on a MIDI lane. */
class DrumSequencer : public juce::Component, public juce::DragAndDropContainer, private juce::Timer
{
public:
    DrumSequencer(AudioEngine&, Project&, DrumSampler&);
    ~DrumSequencer() override;

    static constexpr int kHeight = 380;
    static std::optional<int> draggedSlot(const juce::DragAndDropTarget::SourceDetails&);

    void refresh();
    void stopPreview();
    bool isPreviewing() const { return previewing; }
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    class SlotButton;

    static constexpr int kPageSteps = 16;
    int firstStep() const { return page * kPageSteps; }
    int stepsPerBeat() const { return juce::jmax(1, pattern.stepsPerBar / 4); }
    int visibleSteps() const { return juce::jmin(kPageSteps, pattern.steps - firstStep()); }
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> cell(int pad, int column) const;
    void timerCallback() override;
    void editStep(int pad, int step, std::function<DrumPattern::Step(DrumPattern::Step)>);
    void showStepMenu(int pad, int step);
    static void paintStep(juce::Graphics&, juce::Rectangle<float>, const DrumPattern::Step&);

    AudioEngine& engine;
    Project& project;
    DrumSampler& sampler;
    int selectedSlot = 0;
    int page = 0;
    int playingStep = -1;
    bool previewing = false;
    DrumPattern pattern;
    std::array<std::unique_ptr<SlotButton>, DrumPattern::kSlots> slots;
    juce::Slider length { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ComboBox pages, division;
    juce::TextButton clear { "Clear" }, preview { "Preview" };
};

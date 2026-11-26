#include "DrumSequencer.h"
#include "../Audio/DrumSampler.h"
#include "Theme.h"

namespace
{
    constexpr int margin = 12, labelWidth = 124, headerHeight = 43, stepLabelHeight = 18;
}

class DrumSequencer::SlotButton : public juce::TextButton
{
public:
    SlotButton(DrumSequencer& panel, int index)
        : juce::TextButton(juce::String(index + 1)), owner(panel), slot(index)
    {
        setTitle(Project::drumPatternName(slot));
        setTooltip("Select this slot, or drag it onto a MIDI track to place a copy.");
        onClick = [this]
        {
            const bool changed = owner.selectedSlot != slot;
            owner.selectedSlot = slot;
            if (changed)
                owner.page = 0;
            owner.refresh();
            if (changed && owner.previewing)
                owner.sampler.previewPattern(owner.pattern, true);
        };
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseDrag(e);
        if (e.getDistanceFromDragStart() >= 5 && !owner.isDragAndDropActive()
            && owner.engine.getState() != AudioEngine::State::Recording && owner.project.getDrumPattern(slot).hasHits())
        {
            owner.stopPreview();
            owner.startDragging(juce::var(slot), this, juce::ScaledImage(createComponentSnapshot(getLocalBounds())),
                                true, nullptr, &e.source);
        }
    }

    DrumSequencer& owner;
    const int slot;
};

DrumSequencer::DrumSequencer(AudioEngine& e, Project& p, DrumSampler& drums)
    : engine(e), project(p), sampler(drums)
{
    for (int i = 0; i < DrumPattern::kSlots; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        slot = std::make_unique<SlotButton>(*this, i);
        slot->setColour(juce::TextButton::buttonOnColourId, Theme::accent);
        slot->setColour(juce::TextButton::textColourOnId, Theme::bg);
        addAndMakeVisible(*slot);
    }

    length.setComponentID("pattern-length");
    length.setRange(DrumPattern::kMinSteps, DrumPattern::kMaxSteps, 1);
    length.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 68, 26);
    length.setTextValueSuffix(" steps");
    length.setTooltip("Pattern length in steps, 16 to 64. Click the value to type one.");
    length.textFromValueFunction = [](double steps) { return juce::String(juce::roundToInt(steps)); };
    length.onValueChange = [this]
    {
        auto next = project.getDrumPattern(selectedSlot);
        next.steps = juce::roundToInt(length.getValue());
        project.setDrumPattern(selectedSlot, next);
        refresh();
    };
    pages.setComponentID("pattern-page");
    pages.setTooltip("Choose which 16-step page to edit.");
    pages.onChange = [this]
    {
        if (const int selected = pages.getSelectedItemIndex(); selected >= 0)
            page = selected;
        repaint();
    };
    clear.setTooltip("Clear all hits in the selected pattern.");
    clear.onClick = [this]
    {
        auto next = project.getDrumPattern(selectedSlot);
        next.clear();
        project.setDrumPattern(selectedSlot, next);
        refresh();
    };
    addAndMakeVisible(length);
    addAndMakeVisible(pages);
    division.setComponentID("pattern-division");
    division.setTooltip("Steps per bar: eighth, sixteenth or thirty-second notes.");
    for (int perBar : DrumPattern::kStepsPerBarChoices)
        division.addItem(juce::String(perBar) + " / bar", perBar);
    division.onChange = [this]
    {
        auto next = project.getDrumPattern(selectedSlot);
        if (const int perBar = division.getSelectedId(); DrumPattern::isStepsPerBar(perBar) && perBar != next.stepsPerBar)
        {
            next.stepsPerBar = perBar;
            project.setDrumPattern(selectedSlot, next);
            refresh();
        }
    };
    addAndMakeVisible(division);
    addAndMakeVisible(clear);
    addAndMakeVisible(preview);
    preview.setTooltip("Loop the selected pattern through this kit at the project tempo.");
    preview.onClick = [this]
    {
        if (previewing)
            stopPreview();
        else if (engine.getState() == AudioEngine::State::Stopped && pattern.hasHits())
        {
            previewing = true;
            preview.setButtonText("Stop");
            sampler.previewPattern(pattern, true);
        }
    };
    startTimerHz(25);
    refresh();
}

DrumSequencer::~DrumSequencer()
{
    stopTimer();
    stopPreview();
}

std::optional<int> DrumSequencer::draggedSlot(const juce::DragAndDropTarget::SourceDetails& source)
{
    if (const auto* button = dynamic_cast<const SlotButton*>(source.sourceComponent.get()))
        return button->slot;
    return {};
}

void DrumSequencer::refresh()
{
    const auto next = project.getDrumPattern(selectedSlot);
    if (previewing && next != pattern)
        sampler.previewPattern(next, false);
    if (next.steps != pattern.steps || pages.getNumItems() == 0)
    {
        pages.clear(juce::dontSendNotification);
        for (int start = 0; start < next.steps; start += kPageSteps)
            pages.addItem("Steps " + juce::String(start + 1) + "-" + juce::String(juce::jmin(start + kPageSteps, next.steps)),
                          start / kPageSteps + 1);
        page = juce::jmin(page, pages.getNumItems() - 1);
        pages.setEnabled(pages.getNumItems() > 1);
    }
    pages.setSelectedItemIndex(page, juce::dontSendNotification);
    pattern = next;
    const bool editing = engine.getState() != AudioEngine::State::Recording;
    length.setValue(pattern.steps, juce::dontSendNotification);
    division.setSelectedId(pattern.stepsPerBar, juce::dontSendNotification);
    division.setEnabled(editing);
    length.setEnabled(editing);
    clear.setEnabled(editing && std::any_of(pattern.rows.begin(), pattern.rows.end(), [](const auto& row) { return row.any(); }));
    preview.setEnabled(previewing || (pattern.hasHits() && engine.getState() == AudioEngine::State::Stopped));
    for (int i = 0; i < DrumPattern::kSlots; ++i)
    {
        auto& slot = *slots[static_cast<size_t>(i)];
        const bool filled = project.getDrumPattern(i).hasHits();
        slot.setToggleState(i == selectedSlot, juce::dontSendNotification);
        slot.setColour(juce::TextButton::buttonColourId, filled ? Theme::accent.withMultipliedBrightness(0.36f) : Theme::control);
        slot.setColour(juce::TextButton::textColourOffId, filled ? Theme::text : Theme::textDim);
    }
    repaint();
}

void DrumSequencer::stopPreview()
{
    if (!previewing)
        return;
    sampler.stopPatternPreview();
    previewing = false;
    playingStep = -1;
    preview.setButtonText("Preview");
    repaint();
}

void DrumSequencer::timerCallback()
{
    // The sampler window can stay open while its Patterns tab is hidden.
    const auto* editor = getParentComponent();
    if (editor == nullptr || !editor->isShowing())
    {
        stopPreview();
        return;
    }
    const bool stopped = engine.getState() == AudioEngine::State::Stopped;
    if (!stopped)
        stopPreview();
    if (project.getDrumPattern(selectedSlot) != pattern)
        refresh();
    preview.setEnabled(previewing || (stopped && pattern.hasHits()));
    const int step = previewing ? sampler.getPreviewStep() : -1;
    if (step != playingStep)
    {
        playingStep = step;
        repaint();
    }
}

juce::Rectangle<int> DrumSequencer::gridArea() const
{
    return getLocalBounds().withTrimmedLeft(margin + labelWidth).withTrimmedRight(margin)
                           .withTrimmedTop(headerHeight + stepLabelHeight).withTrimmedBottom(margin);
}

juce::Rectangle<int> DrumSequencer::cell(int pad, int column) const
{
    const auto grid = gridArea();
    const auto x = [&grid](int n) { return grid.getX() + n * grid.getWidth() / kPageSteps; };
    const auto y = [&grid](int n) { return grid.getY() + n * grid.getHeight() / DrumPattern::kPads; };
    return { x(column), y(pad), x(column + 1) - x(column), y(pad + 1) - y(pad) };
}

void DrumSequencer::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);
    g.setColour(Theme::outline);
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
    g.setFont(Theme::font(14.0f, true));
    g.setColour(Theme::text);
    g.drawText("Drum Patterns", juce::Rectangle<int>(margin, 0, labelWidth, headerHeight), juce::Justification::centredLeft);

    g.setFont(Theme::font(12.0f));
    g.setColour(Theme::textDim);
    const int hintLeft = clear.getRight() + 16;
    const int hintWidth = juce::jmax(0, getWidth() - margin - hintLeft);
    const auto hint = playingStep >= 0 ? "Playing step " + juce::String(playingStep + 1)
                                      : juce::String("Right-click a step: rolls");
    g.drawFittedText(hint, { hintLeft, 0, hintWidth, headerHeight },
                     juce::Justification::centredLeft, 1);

    for (int column = 0; column < visibleSteps(); ++column)
    {
        const int step = firstStep() + column;
        const auto top = cell(0, column).withY(headerHeight).withHeight(stepLabelHeight);
        g.setColour(step == playingStep ? Theme::monitor : step % stepsPerBeat() == 0 ? Theme::text : Theme::textDim);
        g.drawText(juce::String(step + 1), top, juce::Justification::centred);
    }

    for (int pad = 0; pad < DrumPattern::kPads; ++pad)
    {
        const auto first = cell(pad, 0);
        g.setColour(Theme::textDim);
        auto label = juce::Rectangle<int>(margin, first.getY(), labelWidth - 4, first.getHeight());
        g.drawText(DrumSampler::noteNameForPad(pad), label.removeFromRight(32), juce::Justification::centredRight);
        const auto sample = sampler.getPadSample(pad);
        g.drawFittedText(sample != nullptr ? sample->name : DrumSampler::rowFor(pad), label, juce::Justification::centredLeft, 1);
        for (int column = 0; column < visibleSteps(); ++column)
        {
            const int step = firstStep() + column;
            const auto bounds = cell(pad, column).reduced(2, 2).toFloat();
            g.setColour(step / stepsPerBeat() % 2 == 0 ? Theme::control : Theme::panelLight);   // alternate beats
            g.fillRoundedRectangle(bounds, 3.0f);
            paintStep(g, bounds, pattern.getStep(pad, step));
            if (step == playingStep)
            {
                g.setColour(Theme::monitor);
                g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
            }
        }
    }
}

void DrumSequencer::paintStep(juce::Graphics& g, juce::Rectangle<float> bounds, const DrumPattern::Step& s)
{
    if (!s.on)
        return;
    using Hit = DrumPattern::Hit;
    const auto colour = s.hit == Hit::Accent ? Theme::text : s.hit == Hit::Ghost ? Theme::accent.withAlpha(0.55f) : Theme::accent;
    auto area = bounds;
    if (s.hit == Hit::Ghost)
        area = area.withSizeKeepingCentre(area.getWidth() * 0.5f, area.getHeight() * 0.5f);
    if (s.hit == Hit::Flam)   // the grace note: a thin bar ahead of the hit
    {
        g.setColour(colour.withAlpha(0.6f));
        g.fillRoundedRectangle(area.removeFromLeft(juce::jmax(2.0f, area.getWidth() * 0.18f)), 1.5f);
        area.removeFromLeft(1.5f);
    }
    const int n = s.hits();
    if (n == 1)
    {
        g.setColour(colour);
        g.fillRoundedRectangle(area, 3.0f);
        return;
    }
    // A roll: one bar per hit, as tall as it plays
    const float gap = 1.0f, w = (area.getWidth() - gap * (float) (n - 1)) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        float scale = 1.0f;
        const float t = (float) i / (float) (n - 1);
        if (s.ramp == DrumPattern::Ramp::Fade)  scale = 1.0f - (1.0f - DrumPattern::kRampFloor) * t;
        if (s.ramp == DrumPattern::Ramp::Swell) scale = DrumPattern::kRampFloor + (1.0f - DrumPattern::kRampFloor) * t;
        auto bar = juce::Rectangle<float>(area.getX() + (float) i * (w + gap), area.getY(), w, area.getHeight());
        g.setColour(colour);
        g.fillRoundedRectangle(bar.withTrimmedTop(bar.getHeight() * (1.0f - scale)), 1.0f);
    }
}

void DrumSequencer::editStep(int pad, int step, std::function<DrumPattern::Step(DrumPattern::Step)> change)
{
    auto next = project.getDrumPattern(selectedSlot);
    next.setStep(pad, step, change(next.getStep(pad, step)));
    project.setDrumPattern(selectedSlot, next);
    refresh();
}

void DrumSequencer::showStepMenu(int pad, int step)
{
    using Step = DrumPattern::Step;
    using Hit = DrumPattern::Hit;
    using Ramp = DrumPattern::Ramp;
    const auto current = pattern.getStep(pad, step);
    // The window may close while the menu is open
    auto edit = [safe = SafePointer<DrumSequencer>(this), pad, step](std::function<Step(Step)> change)
    {
        if (safe != nullptr)
            safe->editStep(pad, step, std::move(change));
    };
    juce::PopupMenu menu;
    menu.addSectionHeader(DrumSampler::rowFor(pad) + ", step " + juce::String(step + 1));
    const std::pair<Hit, const char*> hits[] { { Hit::Hit, "Hit" }, { Hit::Accent, "Accent" }, { Hit::Flam, "Flam" }, { Hit::Ghost, "Ghost" } };
    for (const auto& [type, name] : hits)
        menu.addItem(name, true, current.on && current.hit == type, [edit, hit = type] { edit([hit](Step s) { s.on = true; s.hit = hit; return s; }); });
    menu.addItem("None", current.on, !current.on, [edit] { edit([](Step) { return Step {}; }); });

    menu.addSeparator();
    juce::PopupMenu roll;
    for (int i = 0; i < (int) DrumPattern::kRollHits.size(); ++i)
        roll.addItem(i == 0 ? juce::String("Off") : juce::String(DrumPattern::kRollHits[(size_t) i]) + " hits", true,
                     current.on && current.roll == i, [edit, i] { edit([i](Step s) { s.on = true; s.roll = i; return s; }); });
    menu.addSubMenu("Roll", roll);
    juce::PopupMenu ramp;
    const std::pair<Ramp, const char*> ramps[] { { Ramp::Even, "Even" }, { Ramp::Fade, "Fade (down)" }, { Ramp::Swell, "Swell (up)" } };
    for (const auto& [shape, name] : ramps)
        ramp.addItem(name, current.on && current.roll > 0, current.on && current.ramp == shape, [edit, r = shape] { edit([r](Step s) { s.ramp = r; return s; }); });
    menu.addSubMenu("Roll Ramp", ramp, current.on && current.roll > 0);

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void DrumSequencer::resized()
{
    auto toolbar = getLocalBounds().removeFromTop(headerHeight).reduced(margin, 7);
    toolbar.removeFromLeft(labelWidth);
    for (auto& slot : slots)
        slot->setBounds(toolbar.removeFromLeft(32).withTrimmedRight(4));
    toolbar.removeFromLeft(12);
    length.setBounds(toolbar.removeFromLeft(112));
    toolbar.removeFromLeft(8);
    division.setBounds(toolbar.removeFromLeft(86));
    toolbar.removeFromLeft(8);
    pages.setBounds(toolbar.removeFromLeft(106));
    toolbar.removeFromLeft(8);
    preview.setBounds(toolbar.removeFromLeft(74));
    toolbar.removeFromLeft(8);
    clear.setBounds(toolbar.removeFromLeft(58));
}

void DrumSequencer::mouseDown(const juce::MouseEvent& e)
{
    if (engine.getState() == AudioEngine::State::Recording)
        return;
    const bool menu = e.mods.isPopupMenu();
    if (!menu && !e.mods.isLeftButtonDown())
        return;

    for (int pad = 0; pad < DrumPattern::kPads; ++pad)
    {
        const auto row = cell(pad, 0);
        if (e.y < row.getY() || e.y >= row.getBottom())
            continue;
        if (e.x < row.getX() && !menu)
            sampler.audition(pad);
        for (int column = 0; column < visibleSteps(); ++column)
            if (cell(pad, column).contains(e.getPosition()))
            {
                const int step = firstStep() + column;
                if (menu)
                    showStepMenu(pad, step);
                else
                    editStep(pad, step, DrumPattern::nextHit);
                break;
            }
        break;
    }
}

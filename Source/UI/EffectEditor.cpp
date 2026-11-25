#include "EffectEditor.h"
#include "SamplerEditors.h"
#include "Theme.h"

namespace
{
/** One control: a knob, a menu or a switch, with its name and value. */
class ControlView : public EffectControls::Item
{
public:
    ControlView(EffectPlugin& e, const EffectPlugin::Control& c) : effect(e), param(*c.param), kind(c.kind)
    {
        using Kind = EffectPlugin::Control::Kind;
        name.setText(c.label.isNotEmpty() ? c.label : param.getParameterName(), juce::dontSendNotification);
        name.setFont(Theme::font(11.0f, true));
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, Theme::textDim);
        name.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(name);

        if (kind == Kind::Knob)
        {
            const auto& r = param.valueRange;
            slider.setNormalisableRange({ (double) r.start, (double) r.end, (double) r.interval, (double) r.skew });
            slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            slider.setColour(juce::Slider::rotarySliderFillColourId, Theme::accent);
            slider.setScrollWheelEnabled(false);
            slider.textFromValueFunction = [this](double v) { return param.valueToString((float) v); };
            if (auto def = param.getDefaultValue())
                slider.setDoubleClickReturnValue(true, *def);
            slider.onDragStart = [this] { begin(); };
            slider.onDragEnd = [this] { param.parameterChangeGestureEnd(); };
            slider.onValueChange = [this] { param.setParameter((float) slider.getValue(), juce::sendNotification); };
            slider.setTooltip(param.getParameterName());
            addAndMakeVisible(slider);

            value.setFont(Theme::font(11.0f));
            value.setJustificationType(juce::Justification::centred);
            value.setColour(juce::Label::textColourId, Theme::text);
            value.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(value);
        }
        else if (kind == Kind::Choice)
        {
            for (int i = 0; i < c.choices.size(); ++i)
                menu.addItem(c.choices[i], i + 1);
            menu.onChange = [this]
            {
                if (menu.getSelectedId() > 0 && menu.getSelectedId() - 1 != juce::roundToInt(param.getCurrentValue()))
                    change((float) (menu.getSelectedId() - 1));
            };
            addAndMakeVisible(menu);
        }
        else
        {
            toggle.setClickingTogglesState(false);
            toggle.setColour(juce::TextButton::buttonOnColourId, Theme::accent);
            toggle.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            toggle.onClick = [this] { change(param.getCurrentValue() >= 0.5f ? 0.0f : 1.0f); };
            addAndMakeVisible(toggle);
        }
        for (auto* child : getChildren())
            child->setWantsKeyboardFocus(false);
        update();
    }

    void resized() override
    {
        using Kind = EffectPlugin::Control::Kind;
        auto r = getLocalBounds();
        if (kind == Kind::Knob)
        {
            name.setBounds(r.removeFromTop(14));
            value.setBounds(r.removeFromBottom(14));
            slider.setBounds(r.reduced(4, 0));
        }
        else
        {
            name.setBounds(r.removeFromTop(14));
            r = r.withSizeKeepingCentre(r.getWidth() - 8, 24);
            (kind == Kind::Choice ? (juce::Component&) menu : (juce::Component&) toggle).setBounds(r);
        }
    }

    /** Follows the parameter: automation, undo and MIDI move it from elsewhere. */
    void update() override
    {
        using Kind = EffectPlugin::Control::Kind;
        const float v = param.getCurrentValue();
        if (kind == Kind::Knob)
        {
            if (!slider.isMouseButtonDown())
                slider.setValue(v, juce::dontSendNotification);
            const auto text = param.valueToString(v);
            if (value.getText() != text)
                value.setText(text, juce::dontSendNotification);
        }
        else if (kind == Kind::Choice)
        {
            if (menu.getSelectedId() != juce::roundToInt(v) + 1)
                menu.setSelectedId(juce::roundToInt(v) + 1, juce::dontSendNotification);
        }
        else
        {
            toggle.setToggleState(v >= 0.5f, juce::dontSendNotification);
            toggle.setButtonText(v >= 0.5f ? "On" : "Off");
        }

        const bool active = effect.isControlActive(param);
        if (active != isEnabled())
        {
            setEnabled(active);
            setAlpha(active ? 1.0f : 0.35f);
        }
    }

private:
    void begin()
    {
        effect.edit.getUndoManager().beginNewTransaction("Change " + param.getParameterName());
        param.parameterChangeGestureBegin();
    }

    void change(float v)
    {
        begin();
        param.setParameter(v, juce::sendNotification);
        param.parameterChangeGestureEnd();
        update();
    }

    EffectPlugin& effect;
    te::AutomatableParameter& param;
    const EffectPlugin::Control::Kind kind;
    juce::Label name, value;
    juce::Slider slider;
    juce::ComboBox menu;
    juce::TextButton toggle;
};


/** A menu that isn't a parameter: e.g. which track keys the compressor. */
class SelectorView : public EffectControls::Item
{
public:
    explicit SelectorView(EffectPlugin::Selector s) : selector(std::move(s))
    {
        cells = 2;
        name.setText(selector.name, juce::dontSendNotification);
        name.setFont(Theme::font(11.0f, true));
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible(name);
        menu.onChange = [this]
        {
            const int chosen = menu.getSelectedId() - 1;
            if (chosen >= 0 && chosen != selector.get())
                selector.set(chosen);
        };
        menu.setWantsKeyboardFocus(false);
        addAndMakeVisible(menu);
        update();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        name.setBounds(r.removeFromTop(14));
        menu.setBounds(r.withSizeKeepingCentre(r.getWidth() - 8, 24));
    }

    void update() override
    {
        const auto now = selector.options();
        if (now != options)
        {
            options = now;
            menu.clear(juce::dontSendNotification);
            for (int i = 0; i < options.size(); ++i)
                menu.addItem(options[i], i + 1);
        }
        const int current = selector.get();
        if (menu.getSelectedId() != current + 1)
        {
            if (current < 0)
                menu.setText("(missing)", juce::dontSendNotification);
            else
                menu.setSelectedId(current + 1, juce::dontSendNotification);
        }
    }

private:
    EffectPlugin::Selector selector;
    juce::StringArray options;
    juce::Label name;
    juce::ComboBox menu;
};

/** A file the effect loads: Load... and Clear buttons, or drop a file on it. */
class FileView : public EffectControls::Item, public juce::FileDragAndDropTarget
{
public:
    explicit FileView(EffectPlugin::FileSlot s) : slot(std::move(s))
    {
        cells = 3;
        name.setText(slot.name, juce::dontSendNotification);
        name.setFont(Theme::font(11.0f, true));
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible(name);
        file.setFont(Theme::font(11.0f));
        file.setJustificationType(juce::Justification::centred);
        file.setColour(juce::Label::textColourId, Theme::text);
        addAndMakeVisible(file);
        load.onClick = [this] { choose(); };
        clear.onClick = [this] { set({}); };
        for (auto* b : { &load, &clear })
        {
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        update();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        name.setBounds(r.removeFromTop(14));
        file.setBounds(r.removeFromTop(24).reduced(4, 0));
        auto buttons = r.withSizeKeepingCentre(r.getWidth() - 8, 24);
        clear.setBounds(buttons.removeFromRight(64));
        buttons.removeFromRight(6);
        load.setBounds(buttons);
    }

    void paint(juce::Graphics& g) override
    {
        if (dragging)
        {
            g.setColour(Theme::accent);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
        }
    }

    void update() override
    {
        const auto f = slot.get();
        const auto text = f == juce::File() ? juce::String("None") : f.getFileName();
        if (file.getText() != text)
        {
            file.setText(text, juce::dontSendNotification);
            file.setTooltip(f.getFullPathName());
        }
        clear.setEnabled(f != juce::File());
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return files.size() == 1 && juce::File(files[0]).hasFileExtension(slot.wildcard.removeCharacters("*"));
    }
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dragging = false;
        repaint();
        set(juce::File(files[0]));
    }

private:
    void choose()
    {
        chooser = std::make_unique<juce::FileChooser>("Load " + slot.name, slot.get(), slot.wildcard);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = SafePointer<FileView>(this)](const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File())
                safe->set(fc.getResult());
        });
    }

    void set(const juce::File& f)
    {
        juce::String error;
        if (!slot.set(f, error))
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(juce::MessageBoxIconType::WarningIcon)
                                             .withTitle("Couldn't load " + f.getFileName())
                                             .withMessage(error)
                                             .withButton("OK", 0),
                                         nullptr);
        update();
    }

    EffectPlugin::FileSlot slot;
    juce::Label name, file;
    juce::TextButton load { "Load..." }, clear { "Clear" };
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragging = false;
};
}

//==============================================================================
EffectControls::EffectControls(EffectPlugin& e, std::function<bool(const juce::String&)> include) : effect(e)
{
    auto place = [&](Item* item, const juce::String& rowName)
    {
        items.add(item);
        addAndMakeVisible(item);
        auto row = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.name == rowName; });
        if (row == rows.end())
            row = rows.insert(rows.end(), Row { rowName, {} });
        row->items.push_back(item);
    };
    auto wanted = [&](const juce::String& row) { return include == nullptr || include(row); };

    for (auto& control : effect.getControls())
        if (wanted(control.row))
            place(new ControlView(effect, control), control.row);
    for (auto& selector : effect.getSelectors())
        if (wanted(selector.row))
            place(new SelectorView(selector), selector.row);
    for (auto& slot : effect.getFileSlots())
        if (wanted(slot.row))
            place(new FileView(slot), slot.row);

    setSize(naturalWidth(), heightForWidth(naturalWidth()));
    startTimerHz(20);
}

EffectControls::~EffectControls() { stopTimer(); }

int EffectControls::columnsFor(int width) const { return juce::jmax(1, width / kCellWidth); }

int EffectControls::linesFor(const Row& row, int columns) const
{
    int lines = 1, used = 0;
    for (auto* item : row.items)
    {
        if (used > 0 && used + item->cells > columns)
            ++lines, used = 0;
        used += item->cells;
    }
    return lines;
}

int EffectControls::naturalWidth() const
{
    int widest = 1;
    for (auto& row : rows)
    {
        int cells = 0;
        for (auto* item : row.items)
            cells += item->cells;
        widest = juce::jmax(widest, cells);
    }
    return juce::jmin(widest, kMaxColumns) * kCellWidth;
}

int EffectControls::heightForWidth(int width) const
{
    int height = 0;
    for (auto& row : rows)
        height += kRowTitle + linesFor(row, columnsFor(width)) * kRowHeight;
    return height;
}

void EffectControls::resized()
{
    const int columns = columnsFor(getWidth());
    int y = 0;
    for (auto& row : rows)
    {
        y += kRowTitle;
        int used = 0;
        for (auto* item : row.items)
        {
            if (used > 0 && used + item->cells > columns)
                y += kRowHeight, used = 0;
            item->setBounds(juce::Rectangle<int>(used * kCellWidth, y, item->cells * kCellWidth, kRowHeight).reduced(3, 4));
            used += item->cells;
        }
        y += kRowHeight;
    }
}

void EffectControls::paint(juce::Graphics& g)
{
    const int columns = columnsFor(getWidth());
    const auto font = Theme::font(10.0f, true);
    g.setFont(font);
    int y = 0;
    for (auto& row : rows)
    {
        const auto title = juce::Rectangle<int>(0, y, getWidth(), kRowTitle);
        const auto text = row.name.toUpperCase();
        g.setColour(Theme::textDim);
        g.drawText(text, title, juce::Justification::centredLeft, false);
        g.setColour(Theme::outline);
        g.drawHorizontalLine(title.getCentreY(), (float) (juce::GlyphArrangement::getStringWidthInt(font, text) + 8), (float) getWidth());
        y += kRowTitle + linesFor(row, columns) * kRowHeight;
    }
}

void EffectControls::timerCallback()
{
    for (auto* item : items)
        item->update();
}

//==============================================================================
EffectEditor::EffectEditor(EffectPlugin& e) : effect(e), controls(e)
{
    display = effect.getDisplay();
    addAndMakeVisible(controls);
    const int width = juce::jmax(380 - kMargin * 2, controls.naturalWidth());   // at least as wide as the MIDI Learn bar
    setSize(width + kMargin * 2,
            kMargin * 2 + (display.name.isEmpty() ? 0 : kDisplayHeight + 8) + controls.heightForWidth(width));
    startTimerHz(30);
}

EffectEditor::~EffectEditor() { stopTimer(); }

void EffectEditor::install(AudioEngine* engine)
{
    EffectPlugin::editorFactory = [engine](EffectPlugin& e) -> std::unique_ptr<te::Plugin::EditorComponent>
    {
        if (auto custom = createSamplerEditor(e, engine))
            return custom;
        return std::make_unique<EffectEditor>(e);
    };
}

juce::Rectangle<int> EffectEditor::displayBounds() const
{
    if (display.name.isEmpty())
        return {};
    return getLocalBounds().reduced(kMargin).removeFromTop(kDisplayHeight);
}

void EffectEditor::resized()
{
    auto r = getLocalBounds().reduced(kMargin);
    if (!display.name.isEmpty())
        r.removeFromTop(kDisplayHeight + 8);
    controls.setBounds(r);
}

void EffectEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);
    if (!display.name.isEmpty())
        paintDisplay(g, displayBounds(), display);
}

void EffectEditor::paintDisplay(juce::Graphics& g, juce::Rectangle<int> box, const EffectPlugin::Display& display)
{
    g.setColour(Theme::bg);
    g.fillRoundedRectangle(box.toFloat(), 4.0f);
    auto bar = box.reduced(10, 0).withTrimmedLeft(84).toFloat();
    bar = bar.withSizeKeepingCentre(bar.getWidth(), 8.0f);
    g.setColour(Theme::control);
    g.fillRoundedRectangle(bar, 4.0f);

    const float v = juce::jlimit(display.bipolar ? -1.0f : 0.0f, 1.0f, display.value);
    g.setColour(Theme::accent);
    if (display.bipolar)
    {
        const float x = bar.getCentreX() + v * bar.getWidth() * 0.5f;
        g.fillRect(juce::Rectangle<float>(bar.getCentreX() - 0.5f, bar.getY() - 3.0f, 1.0f, bar.getHeight() + 6.0f));
        g.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre({ x, bar.getCentreY() }));
    }
    else
    {
        g.fillRoundedRectangle(bar.withWidth(juce::jmax(8.0f, bar.getWidth() * v)), 4.0f);
    }

    g.setColour(Theme::textDim);
    g.setFont(Theme::font(11.0f, true));
    g.drawText(display.name.toUpperCase(), box.reduced(10, 0).withWidth(80), juce::Justification::centredLeft, false);
    if (display.bipolar)
    {
        g.setFont(Theme::font(9.0f));
        g.drawText("L", bar.toNearestInt().translated(0, 10).withHeight(10), juce::Justification::left, false);
        g.drawText("R", bar.toNearestInt().translated(0, 10).withHeight(10), juce::Justification::right, false);
    }
}

void EffectEditor::timerCallback()
{
    const auto now = effect.getDisplay();
    if (std::abs(now.value - display.value) > 0.002f)
    {
        display = now;
        repaint(displayBounds());
    }
}

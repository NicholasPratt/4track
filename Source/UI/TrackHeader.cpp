#include "TrackHeader.h"

namespace
{
    juce::String dbText(double db)
    {
        return db <= -59.9 ? juce::String("-inf dB") : juce::String(db, 1) + " dB";
    }

    juce::String panText(double v)
    {
        if (std::abs(v) < 0.005) return "C";
        return juce::String(juce::roundToInt(std::abs(v) * 100.0)) + (v < 0 ? " L" : " R");
    }
}

// Each track records one input channel. (Stereo pairs and recording the master mix
// aren't wired to the Tracktion engine yet.)
void fillInputSelector(juce::ComboBox& box, const juce::StringArray& names)
{
    box.clear(juce::dontSendNotification);
    for (int ch = 0; ch < names.size(); ++ch)
        box.addItem("In " + juce::String(ch + 1) + (names[ch].isNotEmpty() ? "  " + names[ch] : juce::String()), ch + 1);
}

void showTrackInput(juce::ComboBox& box, const TrackParams& p)
{
    const int id = p.inputChannel.load() + 1;
    if (box.indexOfItemId(id) >= 0)
        box.setSelectedId(id, juce::dontSendNotification);
    else if (box.getNumItems() > 0)
        box.setSelectedItemIndex(0, juce::dontSendNotification);
}

void applyInputSelection(const juce::ComboBox& box, TrackParams& p)
{
    if (const int id = box.getSelectedId(); id > 0)
    {
        p.inputType = (int) InputType::Mono;
        p.inputChannel = id - 1;
    }
}

//==============================================================================
TrackHeader::TrackHeader(AudioEngine& e, Project& pr, int trackIndex)
    : engine(e), project(pr), index(trackIndex), params(e.getTrack(trackIndex))
{
    name.setFont(Theme::font(13.0f, true));
    name.setEditable(false, true, false);
    name.setTooltip("Double-click to rename");
    name.onTextChange = [this]
    {
        params.name = name.getText().trim().isEmpty() ? "Track " + juce::String(index + 1) : name.getText().trim();
        edited();
    };
    addAndMakeVisible(name);

    arm.setClickingTogglesState(true);
    arm.setColour(juce::TextButton::buttonOnColourId, Theme::record);
    arm.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    arm.setTooltip("Arm for recording");
    arm.onClick = [this] { params.armed = arm.getToggleState(); edited(); };

    mute.setClickingTogglesState(true);
    mute.setColour(juce::TextButton::buttonOnColourId, Theme::mute);
    mute.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    mute.setTooltip("Mute");
    mute.onClick = [this] { params.mute = mute.getToggleState(); edited(); };

    solo.setClickingTogglesState(true);
    solo.setColour(juce::TextButton::buttonOnColourId, Theme::solo);
    solo.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    solo.setTooltip("Solo");
    solo.onClick = [this] { params.solo = solo.getToggleState(); edited(); };

    monitor.setTooltip("Input monitoring: off / auto (hear the input while armed) / always on.\n"
                       "Use headphones, or you may get feedback.");
    monitor.onClick = [this]
    {
        params.monitor = (params.monitor.load() + 1) % 3;
        styleMonitorButton();
        edited();
    };

    for (auto* b : { &arm, &mute, &solo, &monitor })
        addAndMakeVisible(b);

    input.setTooltip("Recording input");
    input.onChange = [this] { applyInputSelection(input, params); edited(); };
    addAndMakeVisible(input);

    volume.setSliderStyle(juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange(-60.0, 6.0, 0.1);
    volume.setSkewFactorFromMidPoint(-12.0);
    volume.setDoubleClickReturnValue(true, 0.0);
    volume.setScrollWheelEnabled(false);
    volume.setPopupDisplayEnabled(true, true, nullptr);
    volume.textFromValueFunction = dbText;
    volume.setColour(juce::Slider::trackColourId, juce::Colour(params.colour));
    volume.onValueChange = [this] { params.volumeDb = (float) volume.getValue(); };
    volume.onDragEnd = [this] { project.markDirty(); };
    addAndMakeVisible(volume);

    pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pan.setRange(-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue(true, 0.0);
    pan.setScrollWheelEnabled(false);
    pan.setPopupDisplayEnabled(true, true, nullptr);
    pan.textFromValueFunction = panText;
    pan.onValueChange = [this] { params.pan = (float) pan.getValue(); };
    pan.onDragEnd = [this] { project.markDirty(); };
    addAndMakeVisible(pan);

    addAndMakeVisible(meter);
    refresh();
}

void TrackHeader::edited()
{
    project.markDirty();
    if (onChanged)
        onChanged();
}

void TrackHeader::styleMonitorButton()
{
    const auto mode = (MonitorMode) params.monitor.load();
    monitor.setButtonText(mode == MonitorMode::Auto ? "A" : "I");
    monitor.setColour(juce::TextButton::buttonColourId,
                      mode == MonitorMode::On ? Theme::monitor
                      : mode == MonitorMode::Auto ? Theme::monitor.withAlpha(0.35f) : Theme::control);
    monitor.setColour(juce::TextButton::textColourOffId, mode == MonitorMode::On ? juce::Colours::black : Theme::text);
}

void TrackHeader::setInputNames(const juce::StringArray& names)
{
    fillInputSelector(input, names);
    showTrackInput(input, params);
    applyInputSelection(input, params);   // in case the old channel no longer exists
}

void TrackHeader::refresh()
{
    if (!name.isBeingEdited() && name.getText() != params.name)
        name.setText(params.name, juce::dontSendNotification);

    arm.setToggleState(params.armed.load(), juce::dontSendNotification);
    arm.setEnabled(engine.getState() != AudioEngine::State::Recording);
    mute.setToggleState(params.mute.load(), juce::dontSendNotification);
    solo.setToggleState(params.solo.load(), juce::dontSendNotification);
    styleMonitorButton();

    if (!input.isPopupActive())
        showTrackInput(input, params);
    input.setEnabled(engine.getState() != AudioEngine::State::Recording);

    if (!volume.isMouseButtonDown())
        volume.setValue(params.volumeDb.load(), juce::dontSendNotification);
    if (!pan.isMouseButtonDown())
        pan.setValue(params.pan.load(), juce::dontSendNotification);
}

void TrackHeader::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour(selected ? Theme::panelLight : Theme::panel);
    g.fillRect(bounds);
    g.setColour(juce::Colour(params.colour));
    g.fillRect(bounds.removeFromLeft(4));
    g.setColour(Theme::bg);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine(getWidth() - 1, 0.0f, (float) getHeight());
}

void TrackHeader::resized()
{
    auto r = getLocalBounds().withTrimmedLeft(10).withTrimmedRight(8).withTrimmedTop(5).withTrimmedBottom(4);

    auto row1 = r.removeFromTop(22);
    const int bw = 24;
    for (auto* b : { &monitor, &solo, &mute, &arm })
    {
        b->setBounds(row1.removeFromRight(bw));
        row1.removeFromRight(3);
    }
    name.setBounds(row1.withTrimmedLeft(-4));

    r.removeFromTop(4);
    input.setBounds(r.removeFromTop(20));

    r.removeFromTop(4);
    auto row3 = r.removeFromTop(18);
    pan.setBounds(row3.removeFromRight(20));
    row3.removeFromRight(4);
    volume.setBounds(row3);

    meter.setBounds(r.withTrimmedTop(2).withHeight(4));
}

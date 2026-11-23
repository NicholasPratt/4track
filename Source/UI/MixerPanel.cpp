#include "MixerPanel.h"
#include "PluginWindows.h"

namespace
{
    juce::String dbText(double db) { return db <= -59.9 ? juce::String("-inf") : juce::String(db, 1); }

    juce::String freqText(double hz)
    {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
    }

    void setupFader(juce::Slider& fader, juce::Label& value)
    {
        fader.setSliderStyle(juce::Slider::LinearVertical);
        fader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange(-60.0, 6.0, 0.1);
        fader.setSkewFactorFromMidPoint(-12.0);
        fader.setDoubleClickReturnValue(true, 0.0);
        fader.setScrollWheelEnabled(false);
        value.setJustificationType(juce::Justification::centred);
        value.setFont(Theme::font(10.0f));
        value.setColour(juce::Label::textColourId, Theme::textDim);
    }

    template <typename Fn>
    void bind(juce::Slider& s, Project& project, Fn&& onValue)
    {
        s.onValueChange = [&s, fn = std::forward<Fn>(onValue)] { fn((float) s.getValue()); };
        s.onDragEnd = [&project] { project.markDirty(); };
    }

    void sync(juce::Slider& s, float value)
    {
        if (!s.isMouseButtonDown())
            s.setValue(value, juce::dontSendNotification);
    }
}

Knob::Knob(const juce::String& label, juce::Colour colour)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setColour(juce::Slider::rotarySliderFillColourId, colour);
    slider.setPopupDisplayEnabled(true, true, nullptr);
    slider.setScrollWheelEnabled(false);
    addAndMakeVisible(slider);

    caption.setText(label, juce::dontSendNotification);
    caption.setFont(Theme::font(9.0f, true));
    caption.setJustificationType(juce::Justification::centred);
    caption.setColour(juce::Label::textColourId, Theme::textDim);
    caption.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(caption);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    caption.setBounds(r.removeFromBottom(11));
    slider.setBounds(r);
}

//==============================================================================
InsertSlots::InsertSlots(AudioEngine& e, Project& p, int trackIndex) : engine(e), project(p), index(trackIndex)
{
    refresh();
}

void InsertSlots::refresh()
{
    std::vector<Slot> now;
    for (auto* plugin : engine.getInserts(index))
        now.push_back({ plugin, plugin->getName(), plugin->isEnabled() });
    if (now != slots)
    {
        slots = std::move(now);
        repaint();
    }
}

juce::Rectangle<int> InsertSlots::slotBounds(int i) const
{
    return { 0, i * (kSlotHeight + kGap), getWidth(), kSlotHeight };
}

void InsertSlots::paint(juce::Graphics& g)
{
    g.setFont(Theme::font(10.0f));
    for (int i = 0; i < AudioEngine::kMaxInserts; ++i)
    {
        const auto r = slotBounds(i).toFloat();
        if (i < (int) slots.size())
        {
            auto& slot = slots[(size_t) i];
            g.setColour(slot.enabled ? Theme::panelLight : Theme::bg);
            g.fillRoundedRectangle(r, 3.0f);
            g.setColour(slot.enabled ? Theme::accent : Theme::textDim.withAlpha(0.5f));
            g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
            g.setColour(slot.enabled ? Theme::text : Theme::textDim);
            g.drawText(slot.name, r.toNearestInt().withTrimmedLeft(7).withTrimmedRight(3), juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour(Theme::outline);
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
            if (i == (int) slots.size())
            {
                g.setColour(Theme::textDim);
                g.drawText("+", r.toNearestInt(), juce::Justification::centred, false);
            }
        }
    }
}

void InsertSlots::mouseUp(const juce::MouseEvent& e)
{
    if (!getLocalBounds().contains(e.getPosition()))
        return;
    const int i = e.y / (kSlotHeight + kGap);
    if (i < (int) slots.size())
    {
        auto* plugin = slots[(size_t) i].plugin;
        if (e.mods.isPopupMenu())
            showPluginMenu(*plugin);
        else
            plugin->showWindowExplicitly();
    }
    else if (i < AudioEngine::kMaxInserts)
    {
        showAddMenu();
    }
}

void InsertSlots::showAddMenu()
{
    juce::Array<juce::PluginDescription> effects;
    for (auto& type : engine.getKnownPlugins().getTypes())
        if (!type.isInstrument)
            effects.add(type);

    juce::PopupMenu menu;
    if (effects.isEmpty())
        menu.addItem(-1, "No plugins found yet", false);
    else
        juce::KnownPluginList::addToMenu(menu, effects, juce::KnownPluginList::sortByManufacturer);
    menu.addSeparator();
    menu.addItem(-2, "Scan for Plugins...");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [safe = SafePointer<InsertSlots>(this), effects](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        if (result == -2)
        {
            showPluginManager(safe->engine);
            return;
        }
        const int chosen = juce::KnownPluginList::getIndexChosenByMenu(effects, result);
        if (chosen < 0)
            return;

        juce::String error;
        if (auto* plugin = safe->engine.addInsert(safe->index, effects.getReference(chosen), error))
        {
            safe->project.markDirty();
            safe->refresh();
            plugin->showWindowExplicitly();
        }
        else
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(juce::MessageBoxIconType::WarningIcon)
                                             .withTitle("Couldn't add plugin")
                                             .withMessage(error)
                                             .withButton("OK", 0),
                                         nullptr);
        }
    });
}

void InsertSlots::showPluginMenu(te::Plugin& plugin)
{
    juce::PopupMenu menu;
    menu.addSectionHeader(plugin.getName());
    menu.addItem(1, "Open Editor");
    menu.addItem(2, "Bypass", true, !plugin.isEnabled());
    menu.addSeparator();
    menu.addItem(3, "Remove");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [safe = SafePointer<InsertSlots>(this), ref = te::Plugin::Ptr(&plugin)](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        // The plugin may have gone (undo) while the menu was open.
        const auto current = safe->engine.getInserts(safe->index);
        if (std::find(current.begin(), current.end(), ref.get()) == current.end())
            return;

        if (result == 1)
            ref->showWindowExplicitly();
        else if (result == 2)
            ref->setEnabled(!ref->isEnabled());
        else if (result == 3)
            safe->engine.removeInsert(*ref);
        safe->project.markDirty();
        safe->refresh();
    });
}

//==============================================================================
ChannelStrip::ChannelStrip(AudioEngine& e, Project& p, int trackIndex)
    : engine(e), project(p), params(e.getTrack(trackIndex)), index(trackIndex), inserts(e, p, trackIndex)
{
    lowCut.slider.setRange(20.0, 500.0, 1.0);
    lowCut.slider.setSkewFactorFromMidPoint(100.0);
    lowCut.slider.setDoubleClickReturnValue(true, 20.0);
    lowCut.slider.textFromValueFunction = [](double v) { return v < 20.5 ? juce::String("Off") : freqText(v); };
    bind(lowCut.slider, project, [this](float v) { params.lowCut = v; });

    highCut.slider.setRange(1000.0, 20000.0, 10.0);
    highCut.slider.setSkewFactorFromMidPoint(5000.0);
    highCut.slider.setDoubleClickReturnValue(true, 20000.0);
    highCut.slider.textFromValueFunction = [](double v) { return v > 19990.0 ? juce::String("Off") : freqText(v); };
    bind(highCut.slider, project, [this](float v) { params.highCut = v; });

    midFreq.slider.setRange(200.0, 8000.0, 10.0);
    midFreq.slider.setSkewFactorFromMidPoint(1000.0);
    midFreq.slider.setDoubleClickReturnValue(true, 1000.0);
    midFreq.slider.textFromValueFunction = freqText;
    bind(midFreq.slider, project, [this](float v) { params.midFreq = v; });

    midGain.slider.setRange(-12.0, 12.0, 0.5);
    midGain.slider.setDoubleClickReturnValue(true, 0.0);
    midGain.slider.textFromValueFunction = [](double v) { return juce::String(v, 1) + " dB"; };
    bind(midGain.slider, project, [this](float v) { params.midGainDb = v; });

    send.slider.setRange(0.0, 1.0, 0.01);
    send.slider.setDoubleClickReturnValue(true, 0.0);
    send.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    bind(send.slider, project, [this](float v) { params.send = v; });

    pan.slider.setRange(-1.0, 1.0, 0.01);
    pan.slider.setDoubleClickReturnValue(true, 0.0);
    pan.slider.textFromValueFunction = [](double v)
    {
        return std::abs(v) < 0.005 ? juce::String("C") : juce::String(juce::roundToInt(std::abs(v) * 100.0)) + (v < 0 ? " L" : " R");
    };
    bind(pan.slider, project, [this](float v) { params.pan = v; });

    for (auto* k : { &lowCut, &highCut, &midFreq, &midGain, &send, &pan })
        addAndMakeVisible(k);
    addAndMakeVisible(inserts);

    mute.setClickingTogglesState(true);
    mute.setColour(juce::TextButton::buttonOnColourId, Theme::mute);
    mute.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    mute.onClick = [this] { params.mute = mute.getToggleState(); project.markDirty(); };
    solo.setClickingTogglesState(true);
    solo.setColour(juce::TextButton::buttonOnColourId, Theme::solo);
    solo.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    solo.onClick = [this] { params.solo = solo.getToggleState(); project.markDirty(); };
    addAndMakeVisible(mute);
    addAndMakeVisible(solo);

    setupFader(fader, faderValue);
    fader.setColour(juce::Slider::trackColourId, juce::Colour(params.colour));
    bind(fader, project, [this](float v) { params.volumeDb = v; });
    addAndMakeVisible(fader);
    addAndMakeVisible(faderValue);
    addAndMakeVisible(meter);
    refresh();
}

void ChannelStrip::refresh()
{
    sync(lowCut.slider, params.lowCut.load());
    sync(highCut.slider, params.highCut.load());
    sync(midFreq.slider, params.midFreq.load());
    sync(midGain.slider, params.midGainDb.load());
    sync(send.slider, params.send.load());
    sync(pan.slider, params.pan.load());
    sync(fader, params.volumeDb.load());
    inserts.refresh();
    mute.setToggleState(params.mute.load(), juce::dontSendNotification);
    solo.setToggleState(params.solo.load(), juce::dontSendNotification);

    const auto text = dbText(fader.getValue());
    if (faderValue.getText() != text)
        faderValue.setText(text, juce::dontSendNotification);
    repaint(0, 0, getWidth(), 22);
}

void ChannelStrip::paint(juce::Graphics& g)
{
    g.setColour(Theme::panel);
    g.fillRect(getLocalBounds().reduced(1, 0));
    g.setColour(juce::Colour(params.colour));
    g.fillRect(2, 0, getWidth() - 4, 3);

    g.setColour(Theme::text);
    g.setFont(Theme::font(11.0f, true));
    g.drawText(params.name, getLocalBounds().withHeight(22).withTrimmedTop(4).reduced(4, 0),
               juce::Justification::centred, true);
    juce::ignoreUnused(engine, index);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced(5, 0).withTrimmedTop(24).withTrimmedBottom(6);

    auto knobRow = [&r](Knob& a, Knob& b)
    {
        auto row = r.removeFromTop(46);
        a.setBounds(row.removeFromLeft(row.getWidth() / 2));
        b.setBounds(row);
        r.removeFromTop(2);
    };
    knobRow(lowCut, highCut);
    knobRow(midFreq, midGain);
    knobRow(send, pan);

    r.removeFromTop(4);
    inserts.setBounds(r.removeFromTop(InsertSlots::preferredHeight()).reduced(2, 0));
    r.removeFromTop(4);

    auto buttons = r.removeFromTop(20);
    mute.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(2, 0));
    solo.setBounds(buttons.reduced(2, 0));
    r.removeFromTop(6);

    faderValue.setBounds(r.removeFromBottom(14));
    meter.setBounds(r.removeFromRight(10));
    r.removeFromRight(4);
    fader.setBounds(r);
}

//==============================================================================
MasterStrip::MasterStrip(AudioEngine& e, Project& p) : engine(e), project(p)
{
    auto& m = engine.getMaster();
    juce::ignoreUnused(m);

    for (auto* k : { &room, &damping, &wet })
    {
        k->slider.setRange(0.0, 1.0, 0.01);
        k->slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        addAndMakeVisible(k);
    }
    room.slider.setDoubleClickReturnValue(true, 0.5);
    damping.slider.setDoubleClickReturnValue(true, 0.5);
    wet.slider.setDoubleClickReturnValue(true, 0.33);
    bind(room.slider, project, [this](float v) { engine.getMaster().reverbRoom = v; });
    bind(damping.slider, project, [this](float v) { engine.getMaster().reverbDamping = v; });
    bind(wet.slider, project, [this](float v) { engine.getMaster().reverbWet = v; });

    setupFader(fader, faderValue);
    bind(fader, project, [this](float v) { engine.getMaster().volumeDb = v; });
    addAndMakeVisible(fader);
    addAndMakeVisible(faderValue);
    addAndMakeVisible(meter);
    refresh();
}

void MasterStrip::refresh()
{
    auto& m = engine.getMaster();
    sync(room.slider, m.reverbRoom.load());
    sync(damping.slider, m.reverbDamping.load());
    sync(wet.slider, m.reverbWet.load());
    sync(fader, m.volumeDb.load());
    const auto text = dbText(fader.getValue());
    if (faderValue.getText() != text)
        faderValue.setText(text, juce::dontSendNotification);
}

void MasterStrip::paint(juce::Graphics& g)
{
    g.setColour(Theme::panelLight);
    g.fillRect(getLocalBounds().reduced(1, 0));
    g.setColour(Theme::text);
    g.setFont(Theme::font(11.0f, true));
    g.drawText("MASTER", getLocalBounds().withHeight(22).withTrimmedTop(4), juce::Justification::centred, false);
}

void MasterStrip::resized()
{
    auto r = getLocalBounds().reduced(6, 0).withTrimmedTop(24).withTrimmedBottom(6);
    auto row = r.removeFromTop(46);
    const int w = row.getWidth() / 3;
    room.setBounds(row.removeFromLeft(w));
    damping.setBounds(row.removeFromLeft(w));
    wet.setBounds(row);
    r.removeFromTop(10);

    faderValue.setBounds(r.removeFromBottom(14));
    meter.setBounds(r.removeFromRight(36));
    r.removeFromRight(4);
    fader.setBounds(r);
}

//==============================================================================
MixerPanel::MixerPanel(AudioEngine& e, Project& p) : master(e, p)
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        strips[(size_t) t] = std::make_unique<ChannelStrip>(e, p, t);
        addAndMakeVisible(*strips[(size_t) t]);
    }
    addAndMakeVisible(master);
}

void MixerPanel::refresh()
{
    for (auto& s : strips)
        s->refresh();
    master.refresh();
}

void MixerPanel::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
    g.setColour(Theme::outline);
    g.drawHorizontalLine(0, 0.0f, (float) getWidth());
}

void MixerPanel::resized()
{
    auto r = getLocalBounds().withTrimmedTop(2);
    master.setBounds(r.removeFromRight(150));
    const int w = juce::jmin(110, r.getWidth() / kNumTracks);
    for (auto& s : strips)
        s->setBounds(r.removeFromLeft(w));
}

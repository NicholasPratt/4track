#include "MixerPanel.h"
#include "Busy.h"
#include "PluginWindows.h"
#include "../Audio/Effects.h"

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

    void styleRoute(juce::TextButton& button, juce::Colour colour)
    {
        button.setColour(juce::TextButton::buttonOnColourId, colour);
        button.setColour(juce::TextButton::textColourOnId, Theme::bg);
    }

    constexpr int knobRowHeight = 48;
    void layoutRouteButtons(juce::Rectangle<int> bank, std::initializer_list<juce::TextButton*> buttons)
    {
        for (auto* button : buttons)
        {
            button->setBounds(bank.removeFromTop(20));
            bank.removeFromTop(4);
        }
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

    constexpr int kFirstEffect = 1000;   // 8track's own effects; KnownPluginList's ids are far above
    // 8track's own: effects everywhere; on the 8 tracks, its instruments and MIDI effects too.
    // Those need a MIDI track, so on an empty audio track picking one makes it one.
    const bool isTrack = juce::isPositiveAndBelow(index, kNumTracks);
    const bool midiTrack = engine.isTrackMidi(index);
    const bool canBecomeMidi = isTrack && !midiTrack && project.getArrangement().tracks[(size_t) index].empty();
    const auto builtIn = EffectPlugin::getTypes();
    juce::PopupMenu menu;
    menu.addSectionHeader("8track");
    for (size_t i = 0; i < builtIn.size(); ++i)
        if (builtIn[i].kind == EffectPlugin::Kind::Effect)
            menu.addItem(juce::PopupMenu::Item(builtIn[i].name).setID(kFirstEffect + (int) i));
    if (isTrack)
    {
        const bool usable = midiTrack || canBecomeMidi;
        const juce::String why = midiTrack ? juce::String() : canBecomeMidi ? " (makes this a MIDI track)" : " (MIDI tracks only)";
        menu.addSectionHeader("8track Instruments" + why);
        auto* current = engine.getInstrument(index);
        for (size_t i = 0; i < builtIn.size(); ++i)
            if (builtIn[i].kind == EffectPlugin::Kind::Instrument)
                menu.addItem(juce::PopupMenu::Item(builtIn[i].name)
                                 .setID(kFirstEffect + (int) i)
                                 .setEnabled(usable)
                                 .setTicked(current != nullptr && current->getPluginType() == builtIn[i].xmlType));
        menu.addSectionHeader("8track MIDI Effects" + why);
        for (size_t i = 0; i < builtIn.size(); ++i)
            if (builtIn[i].kind == EffectPlugin::Kind::MidiEffect)
                menu.addItem(juce::PopupMenu::Item(builtIn[i].name).setID(kFirstEffect + (int) i).setEnabled(usable));
        if (!usable)
            menu.addItem(-4, "(A track with clips on it can't change to MIDI)", false);
    }
    const bool thirdParty = showsThirdPartyPlugins(engine);
    if (thirdParty)
    {
        menu.addSectionHeader("Plugins");
        if (effects.isEmpty())
            menu.addItem(-1, "No plugins found yet", false);
        else
            juce::KnownPluginList::addToMenu(menu, effects, juce::KnownPluginList::sortByManufacturer);
    }
    else
    {
        effects.clear();   // so no menu id can pick one
    }
    menu.addSeparator();
    menu.addItem(-3, "Show Third-Party Plugins", true, thirdParty);
    if (thirdParty)
        menu.addItem(-2, "Scan for Plugins...");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [safe = SafePointer<InsertSlots>(this), effects, builtIn](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        if (result == -2)
        {
            showPluginManager(safe->engine);
            return;
        }
        if (result == -3)
        {
            setShowsThirdPartyPlugins(safe->engine, !showsThirdPartyPlugins(safe->engine));
            safe->showAddMenu();   // open it again, so the change shows straight away
            return;
        }
        juce::String error;
        te::Plugin* plugin = nullptr;
        if (juce::isPositiveAndBelow(result - kFirstEffect, (int) builtIn.size()))
        {
            const auto& type = builtIn[(size_t) (result - kFirstEffect)];
            if (type.kind != EffectPlugin::Kind::Effect && !safe->engine.isTrackMidi(safe->index))
            {
                if (!safe->project.getArrangement().tracks[(size_t) safe->index].empty())
                    return;
                safe->engine.setTrackMidi(safe->index, true);   // the header follows on its next refresh
            }
            if (type.kind == EffectPlugin::Kind::Instrument)
                plugin = safe->engine.setInstrument(safe->index, type.xmlType, error);
            else
                plugin = safe->engine.addEffect(safe->index, type.xmlType, error);
        }
        else
        {
            const int chosen = juce::KnownPluginList::getIndexChosenByMenu(effects, result);
            if (chosen < 0)
                return;
            const auto& description = effects.getReference(chosen);
            Busy::runHere("Loading " + description.name + "...",
                          [&] { plugin = safe->engine.addInsert(safe->index, description, error); });
        }
        if (plugin != nullptr)
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

    const std::array sends { &send1, &send2 };
    for (size_t bus = 0; bus < sends.size(); ++bus)
    {
        auto& slider = sends[bus]->slider;
        slider.setRange(0.0, 1.0, 0.01);
        slider.setDoubleClickReturnValue(true, 1.0);
        slider.setTooltip("Post-fader send level to Bus " + juce::String(static_cast<int>(bus) + 1));
        slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        bind(slider, project, [this, bus](float v) { params.sendLevels[bus] = v; });
    }

    pan.slider.setRange(-1.0, 1.0, 0.01);
    pan.slider.setDoubleClickReturnValue(true, 0.0);
    pan.slider.textFromValueFunction = [](double v)
    {
        return std::abs(v) < 0.005 ? juce::String("C") : juce::String(juce::roundToInt(std::abs(v) * 100.0)) + (v < 0 ? " L" : " R");
    };
    bind(pan.slider, project, [this](float v) { params.pan = v; });

    for (auto* k : { &lowCut, &highCut, &midFreq, &midGain, &send1, &send2, &pan })
        addAndMakeVisible(k);
    addAndMakeVisible(inserts);

    const auto addRoute = [this](juce::TextButton& button, AudioEngine::Route route, juce::Colour colour, const char* tooltip)
    {
        styleRoute(button, colour);
        button.setTooltip(tooltip);
        button.onClick = [this, route]
        {
            engine.setTrackRoute(index, route, !engine.hasTrackRoute(index, route));
            refresh();
        };
        addAndMakeVisible(button);
    };
    addRoute(mainRoute, AudioEngine::Route::Main, Theme::accent, "Send directly to Main");
    addRoute(bus1Route, AudioEngine::Route::Bus1, Theme::monitor, "Send to Bus 1");
    addRoute(bus2Route, AudioEngine::Route::Bus2, Theme::solo, "Send to Bus 2");

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
    sync(send1.slider, params.sendLevels[0].load());
    sync(send2.slider, params.sendLevels[1].load());
    sync(pan.slider, params.pan.load());
    sync(fader, params.volumeDb.load());
    inserts.refresh();
    mute.setToggleState(params.mute.load(), juce::dontSendNotification);
    solo.setToggleState(params.solo.load(), juce::dontSendNotification);
    mainRoute.setToggleState(engine.hasTrackRoute(index, AudioEngine::Route::Main), juce::dontSendNotification);
    bus1Route.setToggleState(engine.hasTrackRoute(index, AudioEngine::Route::Bus1), juce::dontSendNotification);
    bus2Route.setToggleState(engine.hasTrackRoute(index, AudioEngine::Route::Bus2), juce::dontSendNotification);
    const bool routingAvailable = engine.getState() != AudioEngine::State::Recording;
    for (auto* button : { &mainRoute, &bus1Route, &bus2Route })
        button->setEnabled(routingAvailable);

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
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced(5, 0).withTrimmedTop(24).withTrimmedBottom(6);

    auto knobRow = [&r](Knob& a, Knob& b)
    {
        auto row = r.removeFromTop(knobRowHeight).withTrimmedBottom(2);
        a.setBounds(row.removeFromLeft(row.getWidth() / 2));
        b.setBounds(row);
    };
    knobRow(lowCut, highCut);
    knobRow(midFreq, midGain);
    auto sends = r.removeFromTop(knobRowHeight).withTrimmedBottom(2);
    const int columnWidth = sends.getWidth() / 3;
    pan.setBounds(sends.removeFromLeft(columnWidth));
    send1.setBounds(sends.removeFromLeft(columnWidth));
    send2.setBounds(sends);
    r.removeFromTop(4);
    inserts.setBounds(r.removeFromTop(InsertSlots::preferredHeight()).reduced(2, 0));
    r.removeFromTop(4);

    auto buttons = r.removeFromTop(20);
    mute.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(2, 0));
    solo.setBounds(buttons.reduced(2, 0));
    r.removeFromTop(6);

    const auto valueRow = r.removeFromBottom(14);
    layoutRouteButtons(r.removeFromLeft(32).withTrimmedTop(2), { &mainRoute, &bus1Route, &bus2Route });
    r.removeFromLeft(4);
    meter.setBounds(r.removeFromRight(10));
    r.removeFromRight(4);
    faderValue.setBounds(valueRow.withWidth(40).withCentre({ r.getCentreX(), valueRow.getCentreY() }));
    fader.setBounds(r);
}

//==============================================================================
OutputStrip::OutputStrip(AudioEngine& e, Project& p, int mixerIndex)
    : engine(e),
      params(mixerIndex == AudioEngine::kMasterBus ? e.getMaster() : e.getBus(mixerIndex - AudioEngine::kFirstStereoBus)),
      bus1(mixerIndex == AudioEngine::kFirstStereoBus),
      name(mixerIndex == AudioEngine::kMasterBus ? "MAIN" : bus1 ? "BUS 1" : "BUS 2"),
      colour(mixerIndex == AudioEngine::kMasterBus ? Theme::accent : bus1 ? Theme::monitor : Theme::solo),
      inserts(e, p, mixerIndex)
{
    addAndMakeVisible(inserts);
    if (bus1)
    {
        for (auto* knob : { &room, &damping, &wet })
        {
            knob->slider.setRange(0.0, 1.0, 0.01);
            knob->slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
            addAndMakeVisible(knob);
        }
        room.slider.setDoubleClickReturnValue(true, 0.5);
        damping.slider.setDoubleClickReturnValue(true, 0.5);
        wet.slider.setDoubleClickReturnValue(true, 0.33);
        bind(room.slider, p, [this](float v) { engine.getBusReverb().room = v; });
        bind(damping.slider, p, [this](float v) { engine.getBusReverb().damping = v; });
        bind(wet.slider, p, [this](float v) { engine.getBusReverb().wet = v; });

        styleRoute(reverb, Theme::monitor);
        styleRoute(toMain, Theme::accent);
        styleRoute(toBus2, Theme::solo);
        reverb.setTooltip("Enable or bypass the Bus 1 reverb");
        toMain.setTooltip("Send Bus 1 directly to Main");
        toBus2.setTooltip("Send Bus 1 through Bus 2");
        reverb.onClick = [this] { engine.setBusReverbEnabled(!engine.isBusReverbEnabled()); refresh(); };
        toMain.onClick = [this] { engine.setBus1FeedsBus2(false); refresh(); };
        toBus2.onClick = [this] { engine.setBus1FeedsBus2(true); refresh(); };
        for (auto* button : { &reverb, &toMain, &toBus2 })
            addAndMakeVisible(button);
    }

    setupFader(fader, faderValue);
    fader.setColour(juce::Slider::trackColourId, colour);
    bind(fader, p, [this](float v) { params.volumeDb = v; });
    addAndMakeVisible(fader);
    addAndMakeVisible(faderValue);
    addAndMakeVisible(meter);
    refresh();
}

void OutputStrip::refresh()
{
    if (bus1)
    {
        auto& effect = engine.getBusReverb();
        sync(room.slider, effect.room.load());
        sync(damping.slider, effect.damping.load());
        sync(wet.slider, effect.wet.load());
        const bool active = engine.isBusReverbEnabled();
        reverb.setToggleState(active, juce::dontSendNotification);
        for (auto* knob : { &room, &damping, &wet })
            knob->setEnabled(active);
        const bool cascaded = engine.bus1FeedsBus2();
        toMain.setToggleState(!cascaded, juce::dontSendNotification);
        toBus2.setToggleState(cascaded, juce::dontSendNotification);
        const bool routingAvailable = engine.getState() != AudioEngine::State::Recording;
        toMain.setEnabled(routingAvailable);
        toBus2.setEnabled(routingAvailable);
    }
    inserts.refresh();
    sync(fader, params.volumeDb.load());
    const auto text = dbText(fader.getValue());
    if (faderValue.getText() != text)
        faderValue.setText(text, juce::dontSendNotification);
}

void OutputStrip::paint(juce::Graphics& g)
{
    g.setColour(Theme::panelLight);
    g.fillRect(getLocalBounds().reduced(1, 0));
    g.setColour(colour);
    g.fillRect(2, 0, getWidth() - 4, 3);
    g.setColour(Theme::text);
    g.setFont(Theme::font(11.0f, true));
    g.drawText(name, getLocalBounds().withHeight(22).withTrimmedTop(4),
               juce::Justification::centred, false);
}

void OutputStrip::resized()
{
    auto r = getLocalBounds().reduced(6, 0).withTrimmedTop(24).withTrimmedBottom(6);
    if (bus1)
    {
        reverb.setBounds(r.removeFromTop(24).reduced(0, 2));
        auto row = r.removeFromTop(knobRowHeight);
        const int width = row.getWidth() / 3;
        room.setBounds(row.removeFromLeft(width));
        damping.setBounds(row.removeFromLeft(width));
        wet.setBounds(row);
    }
    r.removeFromTop(4);
    inserts.setBounds(r.removeFromTop(InsertSlots::preferredHeight()).reduced(2, 0));
    r.removeFromTop(8);

    const auto valueRow = r.removeFromBottom(14);
    if (bus1)
    {
        layoutRouteButtons(r.removeFromLeft(36).withTrimmedTop(2), { &toMain, &toBus2 });
        r.removeFromLeft(4);
    }
    meter.setBounds(r.removeFromRight(34));
    r.removeFromRight(4);
    faderValue.setBounds(valueRow.withWidth(40).withCentre({ r.getCentreX(), valueRow.getCentreY() }));
    fader.setBounds(r);
}

//==============================================================================
MixerPanel::MixerPanel(AudioEngine& e, Project& p)
    : bus1(e, p, AudioEngine::kFirstStereoBus),
      bus2(e, p, AudioEngine::kFirstStereoBus + 1),
      master(e, p, AudioEngine::kMasterBus)
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        strips[(size_t) t] = std::make_unique<ChannelStrip>(e, p, t);
        addAndMakeVisible(*strips[(size_t) t]);
    }
    for (auto* output : { &bus1, &bus2, &master })
        addAndMakeVisible(output);
}

void MixerPanel::refresh()
{
    for (auto& s : strips)
        s->refresh();
    bus1.refresh();
    bus2.refresh();
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
    const int outputWidth = juce::jmin(124, r.getWidth() / (kNumTracks + AudioEngine::kStereoBusCount + 1));
    master.setBounds(r.removeFromRight(outputWidth));
    bus2.setBounds(r.removeFromRight(outputWidth));
    bus1.setBounds(r.removeFromRight(outputWidth));
    const int w = r.getWidth() / kNumTracks;
    for (auto& s : strips)
        s->setBounds(r.removeFromLeft(w));
}

#include "TrackHeader.h"
#include "PluginWindows.h"
#include <map>

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

// Items are identified by the input's first channel + 1. (Recording the master mix
// isn't wired to the Tracktion engine yet.)
void fillInputSelector(juce::ComboBox& box, const std::vector<AudioEngine::Input>& inputs)
{
    box.clear(juce::dontSendNotification);
    for (auto& in : inputs)
        box.addItem(in.name + "  " + in.label, in.channel + 1);
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
        p.inputChannel = id - 1;   // the engine works out whether it's a pair
        if (p.inputType.load() == (int) InputType::Mix)
            p.inputType = (int) InputType::Mono;
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

    automation.setTooltip("Automation: choose a control to draw its changes over time on this track");
    automation.onClick = [this] { showAutomationMenu(); };

    for (auto* b : { &arm, &mute, &solo, &monitor, &automation })
        addAndMakeVisible(b);

    input.setTooltip("Recording input");
    input.onChange = [this] { applyInputSelection(input, params); edited(); };
    addAndMakeVisible(input);

    instrument.setTooltip("The instrument this MIDI track plays. It records from every MIDI input.");
    instrument.onClick = [this] { showInstrumentMenu(); };
    addChildComponent(instrument);

    name.addMouseListener(this, false);   // right-click the name for the track menu
    name.setTooltip("Double-click to rename, right-click for audio/MIDI");

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

void TrackHeader::mouseDown(const juce::MouseEvent& e)
{
    if (onClicked)
        onClicked();
    if (e.mods.isPopupMenu())
        showTrackMenu();
}

void TrackHeader::showTrackMenu()
{
    const bool hasClips = !project.getArrangement().tracks[(size_t) index].empty();
    const bool midi = params.midi.load();
    const bool recording = engine.getState() == AudioEngine::State::Recording;

    juce::PopupMenu menu;
    menu.addSectionHeader(params.name);
    menu.addItem(1, "Audio Track", !recording && (midi ? !hasClips : true), !midi);
    menu.addItem(2, "MIDI Track", !recording && (midi ? true : !hasClips), midi);
    if (hasClips)
        menu.addItem(3, "(A track can change type only when it has no clips)", false);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&name),
                       [safe = SafePointer<TrackHeader>(this)](int result)
    {
        if (safe != nullptr && (result == 1 || result == 2))
            safe->setMidi(result == 2);
    });
}

void TrackHeader::setMidi(bool midi)
{
    if (params.midi.load() == midi || !project.getArrangement().tracks[(size_t) index].empty())
        return;
    if (!midi)
        engine.removeInstrument(index);   // an instrument would replace an audio track's sound
    params.armed = false;
    params.midi = midi;
    params.monitor = (int) (midi ? MonitorMode::Auto : MonitorMode::Off);   // hear the keyboard while armed
    engine.syncNow();
    updateKind();
    edited();
}

void TrackHeader::showInstrumentMenu()
{
    juce::Array<juce::PluginDescription> instruments;
    for (auto& type : engine.getKnownPlugins().getTypes())
        if (type.isInstrument)
            instruments.add(type);

    auto* current = engine.getInstrument(index);
    juce::PopupMenu choose;
    if (instruments.isEmpty())
        choose.addItem(-1, "No instruments found yet", false);
    else
        juce::KnownPluginList::addToMenu(choose, instruments, juce::KnownPluginList::sortByManufacturer,
                                         current != nullptr ? current->getIdentifierString() : juce::String());

    juce::PopupMenu menu;
    if (current != nullptr)
    {
        menu.addItem(-3, "Open " + current->getName());
        menu.addItem(-4, "Remove Instrument");
        menu.addSeparator();
        menu.addSubMenu("Change Instrument", choose);
    }
    else
    {
        menu = choose;
    }
    menu.addSeparator();
    menu.addItem(-2, "Scan for Plugins...");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&instrument),
                       [safe = SafePointer<TrackHeader>(this), instruments](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        auto& eng = safe->engine;
        const int track = safe->index;
        if (result == -2)
            showPluginManager(eng);
        else if (result == -3)
        {
            if (auto* p = eng.getInstrument(track))
                p->showWindowExplicitly();
        }
        else if (result == -4)
        {
            eng.removeInstrument(track);
            safe->edited();
        }
        else if (const int chosen = juce::KnownPluginList::getIndexChosenByMenu(instruments, result); chosen >= 0)
        {
            juce::String error;
            if (auto* p = eng.setInstrument(track, instruments.getReference(chosen), error))
            {
                p->showWindowExplicitly();
                safe->edited();
            }
            else
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                                 .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                 .withTitle("Couldn't load instrument")
                                                 .withMessage(error)
                                                 .withButton("OK", 0),
                                             nullptr);
            }
        }
        safe->updateKind();
    });
}

void TrackHeader::showAutomationMenu()
{
    const auto targets = engine.getAutomationTargets(index);
    const auto current = params.automationShown;

    juce::PopupMenu menu;
    menu.addItem(1, "Clips (no automation shown)", true, current.isEmpty());
    menu.addSeparator();

    std::map<juce::String, juce::PopupMenu> pluginMenus;
    juce::StringArray pluginOrder;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        auto& target = targets[i];
        const bool automated = target.param->hasAutomationPoints();
        juce::PopupMenu::Item item(target.name + (automated ? juce::String(" (automated)") : juce::String()));
        item.itemID = (int) i + 10;
        item.isTicked = target.key == current;
        if (target.group == "Mixer")
        {
            menu.addItem(item);
        }
        else
        {
            if (!pluginOrder.contains(target.group))
                pluginOrder.add(target.group);
            pluginMenus[target.group].addItem(item);
        }
    }
    if (!pluginOrder.isEmpty())
        menu.addSeparator();
    for (auto& group : pluginOrder)
        menu.addSubMenu(group, pluginMenus[group]);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&automation),
                       [safe = SafePointer<TrackHeader>(this), targets](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        safe->params.automationShown = result == 1 ? juce::String() : targets[(size_t) (result - 10)].key;
        safe->styleAutomationButton();
        if (safe->onChanged)
            safe->onChanged();
    });
}

void TrackHeader::styleAutomationButton()
{
    // Off when nothing is shown, or the shown control has gone (its plugin was removed).
    if (params.automationShown.isNotEmpty() && engine.findAutomationTarget(index, params.automationShown) == nullptr)
        params.automationShown = {};
    const bool on = params.automationShown.isNotEmpty();
    automation.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xfff2b84b) : Theme::control);
    automation.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : Theme::text);
}

void TrackHeader::updateKind()
{
    const bool midi = params.midi.load();
    auto* inst = engine.getInstrument(index);
    const auto instName = inst != nullptr ? inst->getName() : juce::String();
    if (midi == shownAsMidi && instName == shownInstrument)
        return;
    shownAsMidi = midi;
    shownInstrument = instName;

    input.setVisible(!midi);
    instrument.setVisible(midi);
    instrument.setButtonText(instName.isNotEmpty() ? instName : juce::String("Choose instrument..."));
    instrument.setColour(juce::TextButton::textColourOffId, instName.isNotEmpty() ? Theme::text : Theme::textDim);
    repaint();
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

void TrackHeader::setInputs(const std::vector<AudioEngine::Input>& inputs)
{
    fillInputSelector(input, inputs);
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
    styleAutomationButton();
    updateKind();
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
    for (auto* b : { &automation, &monitor, &solo, &mute, &arm })
    {
        b->setBounds(row1.removeFromRight(bw));
        row1.removeFromRight(3);
    }
    name.setBounds(row1.withTrimmedLeft(-4));

    r.removeFromTop(4);
    input.setBounds(r.removeFromTop(20));
    instrument.setBounds(input.getBounds());

    r.removeFromTop(4);
    auto row3 = r.removeFromTop(18);
    pan.setBounds(row3.removeFromRight(20));
    row3.removeFromRight(4);
    volume.setBounds(row3);

    meter.setBounds(r.withTrimmedTop(2).withHeight(4));
}

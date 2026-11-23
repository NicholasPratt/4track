#include "MainComponent.h"
#include "AudioSettingsDialog.h"

namespace
{
    juce::File projectsRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("8track-projects");
    }

    /** Buttons and sliders shouldn't take keyboard focus, so Space etc. always reach the shortcuts. */
    void disableFocus(juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (dynamic_cast<juce::TextEditor*>(child) == nullptr)
            {
                child->setWantsKeyboardFocus(false);
                child->setMouseClickGrabsKeyboardFocus(false);
            }
            disableFocus(*child);
        }
    }
}

MainComponent::MainComponent()
{
    arrange = std::make_unique<ArrangeView>(engine, project);
    transport = std::make_unique<TransportBar>(engine, project, arrange->getView());
    mixer = std::make_unique<MixerPanel>(engine, project);

    addAndMakeVisible(*transport);
    addAndMakeVisible(*arrange);
    addChildComponent(*mixer);
    pianoRoll = std::make_unique<PianoRoll>(engine, project);
    pianoRoll->onClose = [this] { closePianoRoll(); };
    addChildComponent(*pianoRoll);
    arrange->getLanes().onEditMidiClip = [this](juce::int64 id) { openPianoRoll(id); };
    arrange->getLanes().onCut = [this] { perform(cmdCut); };
    arrange->getLanes().onCopy = [this] { perform(cmdCopy); };
    arrange->getLanes().onDuplicate = [this] { perform(cmdDuplicate); };
    arrange->getLanes().onPaste = [this](int track, juce::int64 position) { pasteClips(clipboard, track, position, false); };
    arrange->getLanes().canPaste = [this] { return !clipboard.empty(); };

    transport->onPlay = [this] { if (engine.getState() == AudioEngine::State::Stopped) engine.play(); };
    transport->onStop = [this] { engine.stop(); };
    transport->onRecord = [this] { toggleRecord(); };
    transport->onToStart = [this] { perform(cmdToStart); };
    transport->onZoomIn = [this] { arrange->zoom(0.6); };
    transport->onZoomOut = [this] { arrange->zoom(1.0 / 0.6); };
    transport->onToggleMixer = [this] { toggleMixer(); };
    transport->onGridChanged = [this] { arrange->viewChanged(); };

    project.onChanged = [this] { projectChanged(); };
    engine.addChangeListener(this);

#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#else
    menuBar = std::make_unique<juce::MenuBarComponent>(this);
    addAndMakeVisible(*menuBar);
#endif

    disableFocus(*this);
    setWantsKeyboardFocus(true);
    pianoRoll->setWantsKeyboardFocus(true);   // Delete removes the selected note while it has focus

    arrange->setInputs(engine.getInputs());
    setSize(1320, 820);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    if (auto* top = getTopLevelComponent())
        top->removeKeyListener(this);
    engine.removeChangeListener(this);
    if (engine.getState() != AudioEngine::State::Stopped)
        engine.stop();
}

void MainComponent::parentHierarchyChanged()
{
    if (auto* top = getTopLevelComponent())
        top->addKeyListener(this);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    if (menuBar != nullptr)
        menuBar->setBounds(r.removeFromTop(24));
    transport->setBounds(r.removeFromTop(52));
    if (pianoRollVisible)
        pianoRoll->setBounds(r.removeFromBottom(MixerPanel::kHeight));
    else if (mixerVisible)
        mixer->setBounds(r.removeFromBottom(MixerPanel::kHeight));
    arrange->setBounds(r);
}

//==============================================================================
void MainComponent::timerCallback()
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = engine.getTrack(t);
        const float l = p.peakL.exchange(0.0f), r = p.peakR.exchange(0.0f);
        arrange->setMeter(t, l, r);
        if (mixerVisible)
            mixer->setMeter(t, l, r);
    }

    auto& m = engine.getMaster();
    const float ml = m.peakL.exchange(0.0f), mr = m.peakR.exchange(0.0f);
    transport->setMasterPeaks(ml, mr);
    if (mixerVisible)
    {
        mixer->setMasterMeter(ml, mr);
        mixer->refresh();
    }

    transport->tick();
    arrange->tick();
    if (pianoRollVisible && engine.getState() != AudioEngine::State::Stopped)
        pianoRoll->repaint();   // playhead

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        const auto title = project.getName() + (project.isDirty() ? " (edited)" : "") + " - 8track";
        if (window->getName() != title)
            window->setName(title);
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Audio device changed (or started)
    arrange->setInputs(engine.getInputs());
    project.handleSampleRateChange();
    arrange->viewChanged();
}

void MainComponent::projectChanged()
{
    arrange->projectChanged();
    transport->refresh();
    mixer->refresh();
    if (pianoRollVisible)
        pianoRoll->refresh();
    menuItemsChanged();
}

void MainComponent::showError(const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message);
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (engine.getState() == AudioEngine::State::Stopped)
        engine.play();
    else
        engine.stop();
}

void MainComponent::toggleRecord()
{
    if (engine.getState() == AudioEngine::State::Recording)
    {
        engine.stop();
        return;
    }

    juce::String error;
    if (!project.prepareFolder(error) || !engine.record(project.getAudioFolder(), error))
        showError("Can't record", error);
}

void MainComponent::splitSelection()
{
    const auto playhead = engine.getPosition();
    auto ids = arrange->getLanes().getSelection();

    // Nothing selected: split whatever is under the playhead on the selected track.
    if (ids.empty())
        for (auto& c : project.getArrangement().tracks[(size_t) arrange->getSelectedTrack()])
            if (playhead > c.start && playhead < c.end())
                ids.insert(c.id);

    project.perform([ids, playhead](Arrangement& a)
    {
        bool any = false;
        for (auto id : ids)
            any = a.splitClip(id, playhead) || any;
        return any;
    });
}

void MainComponent::deleteSelection()
{
    auto ids = arrange->getLanes().getSelection();
    project.perform([ids](Arrangement& a)
    {
        bool any = false;
        for (auto id : ids)
            any = a.removeClip(id) || any;
        return any;
    });
    arrange->getLanes().clearSelection();
}

void MainComponent::copySelection()
{
    auto copied = project.getArrangement().copyClips(arrange->getLanes().getSelection());
    if (!copied.empty())
        clipboard = std::move(copied);
}

void MainComponent::paste()
{
    // The topmost copied clip goes on the selected track, the earliest at the playhead.
    pasteClips(clipboard, arrange->getSelectedTrack(), engine.getPosition(), true);
}

void MainComponent::duplicateSelection()
{
    // A copy straight after the selection, on the same tracks. Repeating it repeats the copy.
    const auto& arr = project.getArrangement();
    const auto& ids = arrange->getLanes().getSelection();
    auto copied = arr.copyClips(ids);
    if (copied.empty())
        return;

    juce::int64 end = 0;
    int top = kNumTracks;
    for (int t = 0; t < kNumTracks; ++t)
        for (auto& c : arr.tracks[(size_t) t])
            if (ids.count(c.id) != 0)
            {
                end = std::max(end, c.end());
                top = std::min(top, t);
            }
    pasteClips(copied, top, end, false);
}

void MainComponent::pasteClips(const std::vector<ClipboardClip>& clips, int track, juce::int64 position, bool movePlayhead)
{
    if (clips.empty())
        return;

    std::array<bool, kNumTracks> midiTracks {};
    for (int t = 0; t < kNumTracks; ++t)
        midiTracks[(size_t) t] = engine.getTrack(t).midi.load();

    // Where each pasted clip will be, to select them afterwards: their ids change once
    // they are in the Edit.
    std::vector<std::pair<int, juce::int64>> placed;
    if (!project.perform([&](Arrangement& a)
        {
            const auto ids = a.pasteClips(clips, track, position, midiTracks);
            for (auto id : ids)
            {
                int t = -1;
                if (auto* c = a.findClip(id, &t))
                    placed.emplace_back(t, c->start);
            }
            return !ids.empty();
        }))
        return;

    std::set<juce::int64> selection;
    for (auto [t, start] : placed)
        for (auto& c : project.getArrangement().tracks[(size_t) t])
            if (c.start == start)
                selection.insert(c.id);
    arrange->getLanes().setSelection(std::move(selection));

    // Move the playhead past the paste, so pasting again carries on from there.
    if (movePlayhead && engine.getState() == AudioEngine::State::Stopped)
        engine.setPosition(std::max<juce::int64>(0, position) + Arrangement::lengthOf(clips));
    arrange->viewChanged();
}

void MainComponent::setPunchPoint(bool in)
{
    auto pos = engine.getPosition();
    if (arrange->getView().snapOn)
        pos = Grid::make(engine, arrange->getView()).snap(pos);

    if (in)
    {
        engine.punchIn = pos;
        if (engine.punchOut.load() <= pos)
            engine.punchOut = pos + engine.getSamplesPerBar() * 2;
    }
    else
    {
        engine.punchOut = pos;
        if (engine.punchIn.load() >= pos)
            engine.punchIn = juce::jmax<juce::int64>(0, pos - engine.getSamplesPerBar() * 2);
    }
    engine.punchOn = true;
    project.markDirty();
    arrange->viewChanged();
}

void MainComponent::openPianoRoll(juce::int64 clipId)
{
    pianoRoll->setVisible(true);
    pianoRollVisible = true;
    mixer->setVisible(false);
    resized();
    pianoRoll->setClip(clipId);
    pianoRoll->grabKeyboardFocus();
}

void MainComponent::closePianoRoll()
{
    pianoRollVisible = false;
    pianoRoll->setVisible(false);
    mixer->setVisible(mixerVisible);
    resized();
    grabKeyboardFocus();
}

void MainComponent::toggleMixer()
{
    if (pianoRollVisible)
        closePianoRoll();
    mixerVisible = !mixerVisible;
    mixer->setVisible(mixerVisible);
    transport->setMixerVisible(mixerVisible);
    resized();
}

//==============================================================================
void MainComponent::perform(int command)
{
    const bool stopped = engine.getState() == AudioEngine::State::Stopped;

    switch (command)
    {
        case cmdNew:      confirmDiscard([this] { project.newProject(); arrange->zoomToFit(); }); break;
        case cmdOpen:     confirmDiscard([this] { openProject(); }); break;
        case cmdSave:     saveProject(project.isUntitled()); break;
        case cmdSaveAs:   saveProject(true); break;
        case cmdImport:   importAudio(); break;
        case cmdExport:   exportMix(); break;
        case cmdAudioSettings: showAudioSettings(); break;
        case cmdPlugins: showPluginManager(engine); break;

        case cmdUndo:     if (stopped) project.undo(); break;
        case cmdRedo:     if (stopped) project.redo(); break;
        case cmdCut:      copySelection(); deleteSelection(); break;
        case cmdCopy:     copySelection(); break;
        case cmdPaste:    paste(); break;
        case cmdDuplicate: duplicateSelection(); break;
        case cmdSplit:    splitSelection(); break;
        case cmdDelete:   deleteSelection(); break;
        case cmdSelectAll: arrange->getLanes().selectAll(); break;

        case cmdPlayStop: togglePlay(); break;
        case cmdRecord:   toggleRecord(); break;
        case cmdToStart:
            if (engine.getState() == AudioEngine::State::Recording) break;
            if (!stopped) engine.stop();
            engine.setPosition(0);
            arrange->getView().viewStart = 0.0;
            arrange->viewChanged();
            break;
        case cmdPunch:
            if (engine.punchOut.load() > engine.punchIn.load())
                engine.punchOn = !engine.punchOn.load();
            else
                setPunchPoint(true);
            project.markDirty();
            arrange->viewChanged();
            break;
        case cmdPunchIn:  setPunchPoint(true); break;
        case cmdPunchOut: setPunchPoint(false); break;
        case cmdClick:    engine.metronomeOn = !engine.metronomeOn.load(); project.markDirty(); break;
        case cmdCountIn:  engine.countInOn = !engine.countInOn.load(); project.markDirty(); transport->refresh(); break;

        case cmdZoomIn:   arrange->zoom(0.6); break;
        case cmdZoomOut:  arrange->zoom(1.0 / 0.6); break;
        case cmdZoomFit:  arrange->zoomToFit(); break;
        case cmdMixer:    toggleMixer(); break;
        default: break;
    }
    menuItemsChanged();
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component* origin)
{
    // Let text fields have their keys.
    if (dynamic_cast<juce::TextEditor*>(origin) != nullptr)
        return false;

    // The note editor handles Delete and Escape itself.
    if (pianoRollVisible && pianoRoll->hasKeyboardFocus(true)
        && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey || key == juce::KeyPress::escapeKey))
        return pianoRoll->keyPressed(key);

    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown();
    const bool shift = mods.isShiftDown();
    const int code = key.getKeyCode();
    const auto ch = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());

    if (cmd)
    {
        switch (ch)
        {
            case 'z': perform(shift ? cmdRedo : cmdUndo); return true;
            case 'y': perform(cmdRedo); return true;
            case 's': perform(shift ? cmdSaveAs : cmdSave); return true;
            case 'o': perform(cmdOpen); return true;
            case 'n': perform(cmdNew); return true;
            case 'a': perform(cmdSelectAll); return true;
            case 'x': perform(cmdCut); return true;
            case 'c': perform(cmdCopy); return true;
            case 'v': perform(cmdPaste); return true;
            case 'd': perform(cmdDuplicate); return true;
            case 'i': perform(cmdImport); return true;
            case 'e': perform(cmdExport); return true;
            case ',': perform(cmdAudioSettings); return true;
            default: break;
        }
        return false;
    }

    if (code == juce::KeyPress::spaceKey)                 { perform(cmdPlayStop); return true; }
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::homeKey) { perform(cmdToStart); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { perform(cmdDelete); return true; }

    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const auto grid = Grid::make(engine, arrange->getView());
        const auto step = (juce::int64) (shift ? grid.samplesPerBar() : grid.step);
        const auto pos = grid.snap(engine.getPosition()) + (code == juce::KeyPress::leftKey ? -step : step);
        engine.setPosition(juce::jmax<juce::int64>(0, pos));
        arrange->viewChanged();
        return true;
    }

    switch (ch)
    {
        case 'r': perform(cmdRecord); return true;
        case 's': perform(cmdSplit); return true;
        case 'p': perform(cmdPunch); return true;
        case 'i': perform(cmdPunchIn); return true;
        case 'o': perform(cmdPunchOut); return true;
        case 'c': perform(cmdClick); return true;
        case 'x': perform(cmdMixer); return true;
        case 'z': perform(cmdZoomFit); return true;
        case '=': case '+': perform(cmdZoomIn); return true;
        case '-': perform(cmdZoomOut); return true;
        default: break;
    }
    return false;
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Transport", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    auto item = [](int id, const juce::String& text, const juce::String& shortcut, bool enabled = true, bool ticked = false)
    {
        juce::PopupMenu::Item i(text);
        i.itemID = id;
        i.shortcutKeyDescription = shortcut;
        i.isEnabled = enabled;
        i.isTicked = ticked;
        return i;
    };

    const bool stopped = engine.getState() == AudioEngine::State::Stopped;
    juce::PopupMenu m;
    switch (index)
    {
        case 0:
            m.addItem(item(cmdNew, "New Project", "Cmd+N", stopped));
            m.addItem(item(cmdOpen, "Open Project...", "Cmd+O", stopped));
            m.addSeparator();
            m.addItem(item(cmdSave, "Save", "Cmd+S", stopped));
            m.addItem(item(cmdSaveAs, "Save As...", "Shift+Cmd+S", stopped));
            m.addSeparator();
            m.addItem(item(cmdImport, "Import Audio...", "Cmd+I", stopped));
            m.addItem(item(cmdExport, "Export Mix...", "Cmd+E", stopped));
            m.addSeparator();
            m.addItem(item(cmdAudioSettings, "Audio Settings...", "Cmd+,"));
            m.addItem(item(cmdPlugins, "Plugins...", ""));
            break;
        case 1:
            m.addItem(item(cmdUndo, "Undo", "Cmd+Z", stopped && project.canUndo()));
            m.addItem(item(cmdRedo, "Redo", "Shift+Cmd+Z", stopped && project.canRedo()));
            m.addSeparator();
            m.addItem(item(cmdCut, "Cut", "Cmd+X", !arrange->getLanes().getSelection().empty()));
            m.addItem(item(cmdCopy, "Copy", "Cmd+C", !arrange->getLanes().getSelection().empty()));
            m.addItem(item(cmdPaste, "Paste at Playhead", "Cmd+V", !clipboard.empty()));
            m.addItem(item(cmdDuplicate, "Duplicate", "Cmd+D", !arrange->getLanes().getSelection().empty()));
            m.addSeparator();
            m.addItem(item(cmdSplit, "Split at Playhead", "S"));
            m.addItem(item(cmdDelete, "Delete", "Delete", !arrange->getLanes().getSelection().empty()));
            m.addItem(item(cmdSelectAll, "Select All", "Cmd+A"));
            break;
        case 2:
            m.addItem(item(cmdPlayStop, stopped ? "Play" : "Stop", "Space"));
            m.addItem(item(cmdRecord, engine.getState() == AudioEngine::State::Recording ? "Stop Recording" : "Record", "R"));
            m.addItem(item(cmdToStart, "Go to Start", "Return"));
            m.addSeparator();
            m.addItem(item(cmdPunch, "Punch In/Out", "P", true, engine.punchOn.load()));
            m.addItem(item(cmdPunchIn, "Set Punch In at Playhead", "I"));
            m.addItem(item(cmdPunchOut, "Set Punch Out at Playhead", "O"));
            m.addSeparator();
            m.addItem(item(cmdClick, "Metronome", "C", true, engine.metronomeOn.load()));
            m.addItem(item(cmdCountIn, "Count-in", "", true, engine.countInOn.load()));
            break;
        case 3:
            m.addItem(item(cmdZoomIn, "Zoom In", "="));
            m.addItem(item(cmdZoomOut, "Zoom Out", "-"));
            m.addItem(item(cmdZoomFit, "Zoom to Fit", "Z"));
            m.addSeparator();
            m.addItem(item(cmdMixer, "Mixer", "X", true, mixerVisible));
            break;
        default: break;
    }
    return m;
}

void MainComponent::menuItemSelected(int itemId, int)
{
    perform(itemId);
}

//==============================================================================
void MainComponent::confirmDiscard(std::function<void()> proceed)
{
    if (!project.isDirty() || project.getArrangement().isEmpty())
    {
        proceed();
        return;
    }

    auto options = juce::MessageBoxOptions()
                       .withIconType(juce::MessageBoxIconType::QuestionIcon)
                       .withTitle("Save changes to \"" + project.getName() + "\"?")
                       .withMessage("Your changes will be lost if you don't save them.")
                       .withButton("Save", 1)
                       .withButton("Don't Save", 2)
                       .withButton("Cancel", 0)
                       .withAssociatedComponent(this);

    juce::AlertWindow::showAsync(options, [this, proceed](int result)
    {
        if (result == 1)
            saveProject(project.isUntitled(), [proceed](bool saved) { if (saved) proceed(); });
        else if (result == 2)
            proceed();
    });
}

void MainComponent::requestQuit(std::function<void()> quit)
{
    if (engine.getState() != AudioEngine::State::Stopped)
        engine.stop();
    confirmDiscard(std::move(quit));
}

void MainComponent::saveProject(bool chooseLocation, std::function<void(bool)> then)
{
    if (!chooseLocation)
    {
        juce::String error;
        const bool ok = project.save(error);
        if (!ok)
            showError("Save failed", error);
        if (then) then(ok);
        return;
    }

    projectsRoot().createDirectory();
    chooser = std::make_shared<juce::FileChooser>("Save project as (a folder will be created)",
                                                  projectsRoot().getChildFile(project.isUntitled() ? "My Song" : project.getName()));
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, then](const juce::FileChooser& fc)
    {
        auto result = fc.getResult();
        if (result == juce::File())
        {
            if (then) then(false);
            return;
        }

        auto folder = result.withFileExtension("");
        if (folder.existsAsFile())
        {
            showError("Save failed", folder.getFullPathName() + " is a file, not a folder.");
            if (then) then(false);
            return;
        }

        juce::String error;
        const bool ok = project.saveAs(folder, error);
        if (!ok)
            showError("Save failed", error);
        if (then) then(ok);
    });
}

void MainComponent::openProject()
{
    chooser = std::make_shared<juce::FileChooser>("Open an 8track project", projectsRoot(), "*.8track;*.4track");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
    {
        auto result = fc.getResult();
        if (result == juce::File())
            return;

        juce::String error;
        if (!project.load(result, error))
        {
            showError("Couldn't open project", error);
            return;
        }
        if (error.isNotEmpty())
            showError("Project opened with problems", error);
        arrange->zoomToFit();
    });
}

void MainComponent::importAudio()
{
    chooser = std::make_shared<juce::FileChooser>("Import audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc)
    {
        int track = arrange->getSelectedTrack();
        juce::StringArray errors;
        for (auto& f : fc.getResults())
        {
            juce::String error;
            if (!project.importAudio(f, track, engine.getPosition(), error))
                errors.add(error);
            track = juce::jmin(kNumTracks - 1, track + 1);
        }
        if (!errors.isEmpty())
            showError("Import", errors.joinIntoString("\n"));
    });
}

void MainComponent::exportMix()
{
    chooser = std::make_shared<juce::FileChooser>("Export mix",
                                                  juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                      .getChildFile(project.getName() + ".wav"),
                                                  "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File())
            return;
        file = file.withFileExtension(".wav");

        juce::String error;
        if (!project.exportMix(file, error))
            showError("Export failed", error);
        else
            file.revealToUser();
    });
}

void MainComponent::showAudioSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AudioSettingsDialog(engine));
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = Theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

#if JUCE_DEBUG
void MainComponent::runDebugSnapshot(const juce::StringArray& args)
{
    juce::File output;
    int track = 0;
    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--snapshot" && i + 1 < args.size())
            output = juce::File(args[++i]);
        else if (args[i] == "--import" && i + 1 < args.size())
        {
            juce::String error;
            const auto at = (juce::int64) (engine.getSamplesPerBar() * track);
            if (!project.importAudio(juce::File(args[++i]), track, at, error))
                DBG(error);
            ++track;
        }
        else if (args[i] == "--mixer" && !mixerVisible)
            toggleMixer();
        else if (args[i] == "--midi" && i + 1 < args.size())   // --midi <track>: a MIDI track with notes, piano roll open
        {
            const int t = args[++i].getIntValue();
            engine.getTrack(t).midi = true;
            engine.syncNow();
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* format : engine.getTracktion().getPluginManager().pluginFormatManager.getFormats())
                if (format->getName() == "AudioUnit")
                    format->findAllTypesForFile(found, "AudioUnit:Synths/aumu,dls ,appl");
            juce::String error;
            if (!found.isEmpty())
                engine.setInstrument(t, *found[0], error);
            const auto bar = engine.getSamplesPerBar();
            if (auto* clip = project.createMidiClip(t, bar, 2 * bar))
            {
                const int pitches[] = { 60, 64, 67, 72, 71, 67, 64, 62 };
                for (int n = 0; n < 8; ++n)
                    clip->getSequence().addNote(pitches[n], te::BeatPosition::fromBeats(n), te::BeatDuration::fromBeats(n % 4 == 3 ? 1.0 : 0.5),
                                                90 + n * 4, 0, &project.getEdit().getUndoManager());
                juce::Timer::callAfterDelay(300, [this, t]
                {
                    for (auto& c : project.getArrangement().tracks[(size_t) t])
                        if (c.midi)
                            openPianoRoll(c.id);
                });
            }
        }
        else if (args[i] == "--automate" && i + 1 < args.size())   // --automate <track>: a volume curve, shown
        {
            const int t = args[++i].getIntValue();
            for (auto& target : engine.getAutomationTargets(t))
                if (target.name == "Volume")
                {
                    auto& curve = target.param->getCurve();
                    auto* um = &project.getEdit().getUndoManager();
                    const float unity = target.param->getCurrentValue();
                    const double bar = (double) engine.getSamplesPerBar() / engine.getSampleRate();
                    curve.addPoint(te::TimePosition::fromSeconds(0.0), unity, 0.0f, um);
                    curve.addPoint(te::TimePosition::fromSeconds(bar), unity, 0.0f, um);
                    curve.addPoint(te::TimePosition::fromSeconds(bar * 2.0), unity * 0.35f, 0.0f, um);
                    curve.addPoint(te::TimePosition::fromSeconds(bar * 3.0), unity * 0.35f, 0.0f, um);
                    curve.addPoint(te::TimePosition::fromSeconds(bar * 3.5), unity, 0.0f, um);
                    engine.getTrack(t).automationShown = target.key;
                }
        }
        else if (args[i] == "--insert" && i + 2 < args.size())   // --insert <track> <AU id, e.g. aufx,dely,appl>
        {
            const int t = args[++i].getIntValue();
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* format : engine.getTracktion().getPluginManager().pluginFormatManager.getFormats())
                if (format->getName() == "AudioUnit")
                    format->findAllTypesForFile(found, "AudioUnit:Effects/" + args[++i]);
            juce::String error;
            if (found.isEmpty() || engine.addInsert(t, *found[0], error) == nullptr)
                DBG("insert failed: " + error);
        }
    }

    engine.punchIn = engine.getSamplesPerBar() * 2;
    engine.punchOut = engine.getSamplesPerBar() * 4;
    engine.punchOn = true;
    engine.setPosition(engine.getSamplesPerBar() * 3);
    arrange->zoomToFit();
    if (!project.getArrangement().tracks[0].empty())
    {
        arrange->getLanes().selectAll();
        arrange->getLanes().clearSelection();
    }

    const bool settings = args.contains("--settings");
    juce::Timer::callAfterDelay(1200, [this, output, settings]
    {
        std::unique_ptr<AudioSettingsDialog> dialog;
        if (settings)
        {
            dialog = std::make_unique<AudioSettingsDialog>(engine);
            dialog->setVisible(true);
        }
        auto image = dialog != nullptr ? dialog->createComponentSnapshot(dialog->getLocalBounds(), true, 2.0f)
                                       : createComponentSnapshot(getLocalBounds(), true, 2.0f);
        output.deleteFile();
        juce::FileOutputStream stream(output);
        juce::PNGImageFormat().writeImageToStream(image, stream);
        stream.flush();
        juce::JUCEApplication::getInstance()->quit();
    });
}

void MainComponent::runDiagnostics()
{
    juce::Timer::callAfterDelay(1500, [this]
    {
        auto& dm = engine.getDeviceManager();
        auto setup = dm.getAudioDeviceSetup();
        std::cout << "device type:   " << dm.getCurrentAudioDeviceType() << "\n"
                  << "input device:  '" << setup.inputDeviceName << "'\n"
                  << "output device: '" << setup.outputDeviceName << "'\n";

        auto* device = dm.getCurrentAudioDevice();
        if (device == nullptr)
        {
            std::cout << "NO DEVICE OPEN" << std::endl;
            juce::JUCEApplication::getInstance()->quit();
            return;
        }
        std::cout << "open device:   '" << device->getName() << "' running=" << device->isPlaying()
                  << " sr=" << device->getCurrentSampleRate() << " block=" << device->getCurrentBufferSizeSamples() << "\n"
                  << "input chans available: " << device->getInputChannelNames().joinIntoString(", ") << "\n"
                  << "active input bits: " << device->getActiveInputChannels().toString(2) << "\n"
                  << "inputs: " << engine.getInputs().size() << "\n"
                  << "latency in/out: " << device->getInputLatencyInSamples() << "/" << device->getOutputLatencyInSamples() << std::endl;

        // Arm one track per active input (monitoring off) and watch the meters.
        const auto inputs = engine.getInputs();
        const int numIns = juce::jmin(kNumTracks, (int) inputs.size());
        for (int t = 0; t < numIns; ++t)
        {
            engine.getTrack(t).inputType = (int) (inputs[(size_t) t].numChannels > 1 ? InputType::Stereo : InputType::Mono);
            engine.getTrack(t).inputChannel = inputs[(size_t) t].channel;
            engine.getTrack(t).monitor = (int) MonitorMode::Off;
            engine.getTrack(t).armed = true;
        }
        stopTimer();   // keep the UI timer from draining the meters

        auto peaks = std::make_shared<std::vector<float>>((size_t) juce::jmax(1, numIns), 0.0f);
        for (int i = 1; i <= 30; ++i)
            juce::Timer::callAfterDelay(i * 100, [this, peaks, numIns, i]
            {
                for (int t = 0; t < numIns; ++t)
                    (*peaks)[(size_t) t] = juce::jmax((*peaks)[(size_t) t], engine.getTrack(t).peakL.exchange(0.0f));
                if (i == 30)
                {
                    for (int t = 0; t < numIns; ++t)
                        std::cout << "input " << t + 1 << " peak over 3s: " << (*peaks)[(size_t) t]
                                  << " (" << juce::Decibels::gainToDecibels((*peaks)[(size_t) t], -120.0f) << " dB)\n";
                    std::cout << std::flush;
                    juce::JUCEApplication::getInstance()->quit();
                }
            });
    });
}
#endif

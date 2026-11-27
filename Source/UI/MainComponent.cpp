#include "MainComponent.h"
#include "Busy.h"
#include "AudioSettingsDialog.h"
#include "MidiSettingsDialog.h"
#include "ShortcutHelp.h"
#include "../Audio/DrumSampler.h"
#include "../Audio/Mp3Encoder.h"

using namespace Shortcuts;

namespace
{
    juce::TextEditor* textEditorAt(juce::Component* component)
    {
        for (; component != nullptr; component = component->getParentComponent())
            if (auto* editor = dynamic_cast<juce::TextEditor*>(component))
                return editor;
        return nullptr;
    }

    juce::TextEditor* focusedTextEditor()
    {
        return textEditorAt(juce::Component::getCurrentlyFocusedComponent());
    }

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
    Busy::setHost(this);
    PluginWindowBehaviour::setEngine(&engine, &project);
    project.runInBackground = [](const juce::String& message, const std::function<void()>& work) { Busy::run(message, work); };
    arrange = std::make_unique<ArrangeView>(engine, project);
    transport = std::make_unique<TransportBar>(engine, project, arrange->getView());
    mixer = std::make_unique<MixerPanel>(engine, project);

    addAndMakeVisible(*transport);
    addAndMakeVisible(*arrange);
    arrange->addMouseListener(this, true);
    addChildComponent(*mixer);
    mixer->addMouseListener(this, true);
    pianoRoll = std::make_unique<PianoRoll>(engine, project);
    pianoRoll->addMouseListener(this, true);
    pianoRoll->onClose = [this] { closePianoRoll(); };
    addChildComponent(*pianoRoll);
    arrange->getLanes().onEditMidiClip = [this](juce::int64 id) { openPianoRoll(id); };
    arrange->getLanes().onCut = [this] { perform(cmdCut, Context::Arrangement); };
    arrange->getLanes().onCopy = [this] { perform(cmdCopy, Context::Arrangement); };
    arrange->getLanes().onDuplicate = [this] { askToDuplicate(); };
    arrange->onShowMidiSettings = [this] { perform(cmdMidiSettings); };
    arrange->getLanes().onPaste = [this](int track, juce::int64 position) { pasteClips(clipboard, track, position, false); };
    arrange->getLanes().canPaste = [this] { return !clipboard.empty(); };

    transport->onPlay = [this] { if (engine.getState() == AudioEngine::State::Stopped) engine.play(); };
    transport->onStop = [this] { engine.stop(); };
    transport->onRecall = [this] { perform(cmdRecall); };
    transport->onRecord = [this] { toggleRecord(); };
    transport->onToStart = [this] { perform(cmdToStart); };
    transport->onRangeMode = [this] { perform(cmdPunch); };
    transport->onZoomIn = [this] { arrange->zoom(0.6); };
    transport->onZoomOut = [this] { arrange->zoom(1.0 / 0.6); };
    transport->onToggleMixer = [this] { toggleMixer(); };
    transport->onOpenDrumPatterns = [this] { openDrumPatterns(); };
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
    pianoRoll->setWantsKeyboardFocus(true);
    arrange->getRuler().setWantsKeyboardFocus(true);
    mixer->setWantsKeyboardFocus(true);
    juce::Desktop::getInstance().addFocusChangeListener(this);

    arrange->setInputs(engine.getInputs());
    setSize(1320, 820);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    if (shortcutHost != nullptr)
        shortcutHost->removeKeyListener(this);
    arrange->removeMouseListener(this);
    mixer->removeMouseListener(this);
    pianoRoll->removeMouseListener(this);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    engine.removeChangeListener(this);
    if (engine.getState() != AudioEngine::State::Stopped)
        engine.stop();
}

void MainComponent::parentHierarchyChanged()
{
    auto* top = getTopLevelComponent();
    if (shortcutHost.getComponent() == top)
        return;
    if (shortcutHost != nullptr)
        shortcutHost->removeKeyListener(this);
    shortcutHost = top;
    shortcutHost->addKeyListener(this);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
}

void MainComponent::mouseDown(const juce::MouseEvent& e)
{
    auto* clicked = e.eventComponent;
    if (textEditorAt(clicked) != nullptr)
        return;
    if (pianoRollVisible && (clicked == pianoRoll.get() || pianoRoll->isParentOf(clicked)))
        pianoRoll->grabKeyboardFocus();
    else if (mixerVisible && (clicked == mixer.get() || mixer->isParentOf(clicked)))
        mixer->grabKeyboardFocus();
    else if (clicked != &arrange->getRuler() || !arrange->getRuler().isSongwritingArea(e.position))
        grabKeyboardFocus();
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
    for (int bus = 0; bus < AudioEngine::kStereoBusCount; ++bus)
    {
        auto& output = engine.getBus(bus);
        const auto left = output.peakL.exchange(0.0f);
        const auto right = output.peakR.exchange(0.0f);
        if (mixerVisible)
            mixer->setBusMeter(bus, left, right);
    }
    if (mixerVisible)
    {
        mixer->setMasterMeter(ml, mr);
        mixer->refresh();
    }

    transport->tick(project.canRecall(arrange->getSelectedTrack()));
    transport->setDrumPatternsAvailable(selectedDrumSampler() != nullptr);
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
    // The looper handles its own second press (finish at the end of the pass).
    if (engine.getState() == AudioEngine::State::Recording && engine.getLoopRecord() == AudioEngine::LoopRecord::Off)
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

void MainComponent::askToDuplicate()
{
    if (arrange->getLanes().getSelection().empty())
        return;

    auto* window = new juce::AlertWindow("Duplicate", "How many copies? They go end to end after the selection.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("count", juce::String(lastDuplicateCount), "Copies:");
    if (auto* editor = window->getTextEditor("count"))
    {
        editor->setInputRestrictions(3, "0123456789");
        editor->selectAll();
    }
    window->addButton("Duplicate", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window](int result)
    {
        if (safe == nullptr || result != 1)
            return;
        const int count = window->getTextEditorContents("count").getIntValue();
        if (count < 1)
            return;
        safe->lastDuplicateCount = juce::jmin(count, 100);
        safe->duplicateSelection(safe->lastDuplicateCount);
    }), true);
}

void MainComponent::duplicateSelection(int times)
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
    pasteClips(copied, top, end, false, times);
}

void MainComponent::pasteClips(const std::vector<ClipboardClip>& clips, int track, juce::int64 position, bool movePlayhead, int times)
{
    if (clips.empty() || times < 1)
        return;
    const auto length = Arrangement::lengthOf(clips);

    std::array<bool, kNumTracks> midiTracks {};
    for (int t = 0; t < kNumTracks; ++t)
        midiTracks[(size_t) t] = engine.getTrack(t).midi.load();

    // Where each pasted clip will be, to select them afterwards: their ids change once
    // they are in the Edit.
    std::vector<std::pair<int, juce::int64>> placed;
    if (!project.perform([&](Arrangement& a)
        {
            std::vector<juce::int64> ids;
            for (int copy = 0; copy < times; ++copy)
            {
                const auto pasted = a.pasteClips(clips, track, position + copy * length, midiTracks);
                ids.insert(ids.end(), pasted.begin(), pasted.end());
            }
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
        engine.setPosition(std::max<juce::int64>(0, position) + times * length);
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
    if (engine.getRangeMode() == AudioEngine::RangeMode::Off && engine.getState() != AudioEngine::State::Recording)
        engine.setRangeMode(AudioEngine::RangeMode::Punch);
    project.markDirty();
    arrange->viewChanged();
}

void MainComponent::setRangeMode(AudioEngine::RangeMode mode)
{
    // Tracktion can't switch between punch and loop in the middle of a recording.
    if (engine.getState() == AudioEngine::State::Recording || engine.getLoopRecord() != AudioEngine::LoopRecord::Off)
        return;
    if (!engine.hasRange())
        setPunchPoint(true);   // a two-bar range at the playhead
    engine.setRangeMode(mode);
    project.markDirty();
    transport->showRangeMode();
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

DrumSampler* MainComponent::selectedDrumSampler() const
{
    return dynamic_cast<DrumSampler*>(engine.getInstrument(arrange->getSelectedTrack()));
}

void MainComponent::openDrumPatterns()
{
    if (auto* sampler = selectedDrumSampler())
        PluginWindowBehaviour::showDrumPatterns(*sampler);
}

//==============================================================================
Shortcuts::Context MainComponent::shortcutContext() const
{
    if (focusedTextEditor() != nullptr)
        return Context::Text;
    if (pianoRollVisible && pianoRoll->hasKeyboardFocus(true))
        return Context::PianoRoll;
    if (arrange->getRuler().hasSongwritingCursor() && arrange->getRuler().hasKeyboardFocus(true))
        return Context::Songwriting;
    if (mixerVisible && mixer->hasKeyboardFocus(true))
        return Context::Mixer;
    return Context::Arrangement;
}

void MainComponent::perform(int command)
{
    perform(command, shortcutContext());
}

void MainComponent::perform(int command, Context context)
{
    if (Busy::isBusy())
        return;

    if (context == Context::Text)
        if (auto* editor = focusedTextEditor())
        {
            switch (command)
            {
                case cmdUndo: editor->undo(); return;
                case cmdRedo: editor->redo(); return;
                case cmdCut: editor->cutToClipboard(); return;
                case cmdCopy: editor->copyToClipboard(); return;
                case cmdPaste: editor->pasteFromClipboard(); return;
                case cmdDelete: editor->deleteForwards(false); return;
                case cmdSelectAll: editor->selectAll(); return;
                default: break;
            }
        }

    if (isCurrentlyBlockedByAnotherModalComponent())
        return;

    switch (command)
    {
        case cmdCut: case cmdCopy: case cmdPaste: case cmdDelete: case cmdSelectAll:
        case cmdDuplicate: case cmdSplit: case cmdPrevious: case cmdNext: case cmdPreviousBar: case cmdNextBar:
        case cmdQuantize: case cmdPitchUp: case cmdPitchDown: case cmdOctaveUp: case cmdOctaveDown:
        case cmdCloseEditor: case cmdEditSongNote:
            if (context == Context::PianoRoll)
                pianoRoll->performShortcut(static_cast<Command>(command));
            else if (context == Context::Songwriting)
            {
                if (command == cmdCloseEditor)
                    grabKeyboardFocus();
                else
                    arrange->getRuler().performSongShortcut(static_cast<Command>(command));
            }
            else if (context == Context::Arrangement)
                break;
            menuItemsChanged();
            return;
        default: break;
    }

    const auto state = engine.getState();
    const bool stopped = state == AudioEngine::State::Stopped;
    if (state == AudioEngine::State::Recording &&
        (command == cmdCut || command == cmdPaste || command == cmdDelete || command == cmdDuplicate || command == cmdSplit
         || command == cmdPrevious || command == cmdNext || command == cmdPreviousBar || command == cmdNextBar))
        return;

    switch (command)
    {
        case cmdNew:      if (stopped) confirmDiscard([this] { project.newProject(); arrange->zoomToFit(); }); break;
        case cmdOpen:     if (stopped) confirmDiscard([this] { openProject(); }); break;
        case cmdSave:     if (stopped) saveProject(project.isUntitled()); break;
        case cmdSaveAs:   if (stopped) saveProject(true); break;
        case cmdImport:   if (stopped) importAudio(); break;
        case cmdExport:   exportMix(); break;
        case cmdAudioSettings: showAudioSettings(); break;
        case cmdMidiSettings: showMidiSettings(); break;
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
        case cmdRecall:
        {
            juce::String error;
            if (!project.recall(arrange->getSelectedTrack(), error))
                showError("Keep what I just played", error);
            break;
        }
        case cmdRecord:   toggleRecord(); break;
        case cmdToStart:
            if (engine.getState() == AudioEngine::State::Recording) break;
            if (!stopped) engine.stop();
            engine.setPosition(0);
            arrange->getView().viewStart = 0.0;
            arrange->viewChanged();
            break;
        case cmdPunch:     setRangeMode(AudioEngine::nextRangeMode(engine.getRangeMode())); break;
        case cmdPunchMode: setRangeMode(engine.getRangeMode() == AudioEngine::RangeMode::Punch ? AudioEngine::RangeMode::Off : AudioEngine::RangeMode::Punch); break;
        case cmdLoop:      setRangeMode(engine.getRangeMode() == AudioEngine::RangeMode::Loop ? AudioEngine::RangeMode::Off : AudioEngine::RangeMode::Loop); break;
        case cmdPunchIn:  setPunchPoint(true); break;
        case cmdPunchOut: setPunchPoint(false); break;
        case cmdClick:    engine.metronomeOn = !engine.metronomeOn.load(); project.markDirty(); break;
        case cmdCountIn:  engine.countInOn = !engine.countInOn.load(); project.markDirty(); transport->refresh(); break;

        case cmdZoomIn:   arrange->zoom(0.6); break;
        case cmdZoomOut:  arrange->zoom(1.0 / 0.6); break;
        case cmdZoomFit:  arrange->zoomToFit(); break;
        case cmdMixer:    toggleMixer(); break;
        case cmdDrumSequencer: openDrumPatterns(); break;
        case cmdShowShortcuts: showShortcutHelp(); break;
        case cmdShowTuner: transport->showTuner(); break;
        case cmdAllNotesOff: engine.allNotesOff(); break;
        case cmdPrevious:    movePlayhead(-1, false); break;
        case cmdNext:        movePlayhead(1, false); break;
        case cmdPreviousBar: movePlayhead(-1, true); break;
        case cmdNextBar:     movePlayhead(1, true); break;
        default: break;
    }
    menuItemsChanged();
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    const auto context = shortcutContext();
    if (const auto* binding = Shortcuts::find(context, key))
    {
        perform(binding->command, context);
        return true;
    }
    return false;
}

void MainComponent::movePlayhead(int direction, bool byBar)
{
    const auto grid = Grid::make(engine, arrange->getView());
    const auto step = static_cast<juce::int64>(byBar ? grid.samplesPerBar() : grid.step);
    const auto pos = grid.snap(engine.getPosition()) + direction * step;
    engine.setPosition(juce::jmax<juce::int64>(0, pos));
    arrange->viewChanged();
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Transport", "View", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    const auto context = shortcutContext();
    auto item = [context](int id, const juce::String& text, bool enabled = true, bool ticked = false)
    {
        juce::PopupMenu::Item i(text);
        i.itemID = id;
        i.shortcutKeyDescription = Shortcuts::keysFor(context, id);
        i.isEnabled = enabled;
        i.isTicked = ticked;
        return i;
    };

    const auto* editor = focusedTextEditor();
    const bool editingText = editor != nullptr;
    const bool editingNotes = context == Context::PianoRoll;
    const bool editingSong = context == Context::Songwriting;
    const bool editingClips = context == Context::Arrangement;
    auto& ruler = arrange->getRuler();
    const bool hasSelection = editingText ? !editor->getHighlightedRegion().isEmpty()
        : editingNotes ? pianoRoll->hasSelectedNotes()
        : editingSong ? ruler.hasSelectedSongNote()
        : editingClips && !arrange->getLanes().getSelection().empty();
    const bool canWriteText = editingText && !editor->isReadOnly();
    const bool canWriteProject = !editingText && engine.getState() != AudioEngine::State::Recording;
    const bool stopped = engine.getState() == AudioEngine::State::Stopped;
    juce::PopupMenu m;
    switch (index)
    {
        case 0:
            m.addItem(item(cmdNew, "New Project", stopped));
            m.addItem(item(cmdOpen, "Open Project...", stopped));
            m.addSeparator();
            m.addItem(item(cmdSave, "Save", stopped));
            m.addItem(item(cmdSaveAs, "Save As...", stopped));
            m.addSeparator();
            m.addItem(item(cmdImport, "Import Audio...", stopped));
            m.addItem(item(cmdExport, "Export Audio...", stopped));
            m.addSeparator();
            m.addItem(item(cmdAudioSettings, "Audio Settings..."));
            m.addItem(item(cmdMidiSettings, "MIDI Settings..."));
            m.addItem(item(cmdPlugins, "Plugins..."));
            break;
        case 1:
        {
            m.addItem(item(cmdUndo, "Undo", canWriteText || (stopped && !editingText && project.canUndo())));
            m.addItem(item(cmdRedo, "Redo", canWriteText || (stopped && !editingText && project.canRedo())));
            m.addSeparator();
            m.addItem(item(cmdCut, editingNotes ? "Cut Notes" : editingSong ? "Cut Chord or Lyric" : "Cut",
                           hasSelection && (canWriteText || canWriteProject)));
            m.addItem(item(cmdCopy, editingNotes ? "Copy Notes" : editingSong ? "Copy Chord or Lyric" : "Copy", hasSelection));
            const bool canPaste = editingText ? canWriteText
                : editingNotes ? pianoRoll->canPasteNotesAtCursor()
                : editingSong ? ruler.canPasteSongText()
                : editingClips && !clipboard.empty();
            m.addItem(item(cmdPaste, editingNotes ? "Paste Notes" : editingSong ? "Paste at Lane Cursor"
                                      : editingClips ? "Paste at Playhead" : "Paste",
                           canPaste && (canWriteText || canWriteProject)));
            if (editingClips)
            {
                m.addItem(item(cmdDuplicate, "Duplicate Clips", hasSelection && canWriteProject));
                m.addItem(item(cmdSplit, "Split Clips at Playhead", canWriteProject));
            }
            else if (editingNotes)
            {
                m.addItem(item(cmdDuplicate, "Duplicate Notes", canWriteProject && pianoRoll->canDuplicateNotes()));
                m.addItem(item(cmdQuantize, "Quantize Notes", hasSelection && canWriteProject));
            }
            else if (editingSong)
            {
                m.addItem(item(cmdEditSongNote, "Add or Edit Chord or Lyric…", canWriteProject));
            }
            m.addSeparator();
            m.addItem(item(cmdDelete, editingNotes ? "Delete Notes" : editingSong ? "Delete Chord or Lyric" : "Delete",
                           (canWriteProject && hasSelection) || canWriteText));
            m.addItem(item(cmdSelectAll, editingNotes ? "Select All Notes" : "Select All", editingNotes || editingClips || editingText));
            break;
        }
        case 2:
            m.addItem(item(cmdPlayStop, stopped ? "Play" : "Stop"));
            m.addItem(item(cmdRecord, engine.getState() == AudioEngine::State::Recording ? "Stop Recording" : "Record"));
            m.addItem(item(cmdRecall, "Keep what I just played", project.canRecall(arrange->getSelectedTrack())));
            m.addItem(item(cmdToStart, "Go to Start"));
            m.addSeparator();
            m.addItem(item(cmdPunch, "Range: Off / Punch / Loop"));
            m.addItem(item(cmdPunchMode, "Punch In/Out", true, engine.getRangeMode() == AudioEngine::RangeMode::Punch));
            m.addItem(item(cmdLoop, "Loop", true, engine.getRangeMode() == AudioEngine::RangeMode::Loop));
            m.addItem(item(cmdPunchIn, "Set Range Start at Playhead"));
            m.addItem(item(cmdPunchOut, "Set Range End at Playhead"));
            m.addSeparator();
            m.addItem(item(cmdClick, "Metronome", true, engine.metronomeOn.load()));
            m.addItem(item(cmdCountIn, "Recording Count-in", true, engine.countInOn.load()));
            m.addSeparator();
            m.addItem(item(cmdAllNotesOff, "MIDI Panic (All Notes Off)"));
            break;
        case 3:
            m.addItem(item(cmdZoomIn, "Zoom In"));
            m.addItem(item(cmdZoomOut, "Zoom Out"));
            m.addItem(item(cmdZoomFit, "Zoom to Fit"));
            m.addSeparator();
            m.addItem(item(cmdMixer, "Mixer", true, mixer->isVisible()));
            m.addItem(item(cmdDrumSequencer, "Drum Patterns", selectedDrumSampler() != nullptr));
            m.addItem(item(cmdShowTuner, "Tuner"));
            break;
        case 4:
            m.addItem(item(cmdShowShortcuts, "Keyboard Shortcuts…"));
            break;
        default: break;
    }
    return m;
}

void MainComponent::menuItemSelected(int itemId, int)
{
    perform(itemId);
}

void MainComponent::showShortcutHelp()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ShortcutHelp());
    options.componentToCentreAround = this;
    options.dialogTitle = "Keyboard Shortcuts";
    options.dialogBackgroundColour = Theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync())
        window->setResizeLimits(490, 360, 1100, 1000);
}

//==============================================================================
void MainComponent::confirmDiscard(std::function<void()> proceed)
{
    if (!project.isDirty())
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
    if (Busy::isBusy())
        return;
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
        bool loaded = false;
        Busy::runHere("Opening " + (result.isDirectory() ? result : result.getParentDirectory()).getFileName() + "...",
                      [&] { loaded = project.load(result, error); });
        if (!loaded)
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
    if (engine.getState() != AudioEngine::State::Stopped)
        return;
    if (project.getArrangement().getEnd() <= 0)
    {
        showError("Export Audio", "There's nothing to export yet.");
        return;
    }

    using Settings = Project::ExportSettings;
    auto* dialog = new juce::AlertWindow("Export Audio", {}, juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray formats { "WAV" };
    if (Mp3Encoder::isAvailable())
        formats.add("MP3");
    dialog->addComboBox("format", formats, "Format");
    dialog->addComboBox("quality", {}, "Bit depth / bitrate");

    auto* format = dialog->getComboBoxComponent("format");
    auto* quality = dialog->getComboBoxComponent("quality");
    const auto updateQuality = [format, quality]
    {
        quality->clear(juce::dontSendNotification);
        if (format->getSelectedId() == 2)
        {
            for (auto bitrate : Settings::mp3Bitrates)
                quality->addItem(juce::String(bitrate) + " kbps", bitrate);
            quality->setSelectedId(Settings{}.mp3BitrateKbps, juce::dontSendNotification);
        }
        else
        {
            for (auto depth : Settings::wavBitDepths)
                quality->addItem(juce::String(depth) + (depth == 32 ? "-bit float" : "-bit PCM"), depth);
            quality->setSelectedId(Settings{}.wavBitDepth, juce::dontSendNotification);
        }
    };
    format->setSelectedId(1, juce::dontSendNotification);
    format->onChange = updateQuality;
    updateQuality();
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->addButton("Choose Location...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<MainComponent>(this), format, quality](int result)
        {
            if (safe == nullptr || result != 1)
                return;
            Settings settings;
            if (format->getSelectedId() == 2)
            {
                settings.format = Settings::Format::mp3;
                settings.mp3BitrateKbps = quality->getSelectedId();
            }
            else
            {
                settings.wavBitDepth = quality->getSelectedId();
            }
            safe->chooseExportFile(settings);
        }), true);
}

void MainComponent::chooseExportFile(Project::ExportSettings settings)
{
    const auto extension = "." + juce::String(settings.extension());
    chooser = std::make_shared<juce::FileChooser>("Save audio export",
                                                  juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                      .getChildFile(project.getName() + extension),
                                                  "*" + extension);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [safe = juce::Component::SafePointer<MainComponent>(this), settings, extension](const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (safe == nullptr || chosen == juce::File())
            return;

        const auto file = chosen.hasFileExtension(extension) ? chosen : chosen.withFileExtension(extension);
        const auto render = [safe, settings, file]
        {
            if (safe == nullptr)
                return;
            juce::String error;
            if (!safe->project.exportMix(file, settings, error))
                safe->showError("Export failed", error);
            else
                file.revealToUser();
        };

        // The chooser only confirms overwriting the path it returns.
        if (file != chosen && file.existsAsFile())
        {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Replace existing audio file?")
                .withMessage(file.getFileName() + " already exists in this folder.")
                .withButton("Replace", 1).withButton("Cancel", 0).withAssociatedComponent(safe.getComponent()),
                [render](int result) { if (result == 1) render(); });
        }
        else
            render();
    });
}

void MainComponent::showMidiSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new MidiSettingsDialog(engine));
    options.dialogTitle = "MIDI Settings";
    options.dialogBackgroundColour = Theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
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
        else if (args[i] == "--instrument" && i + 2 < args.size())   // --instrument <track> <8track instrument, e.g. eightTrackAnalogSynth>
        {
            const int t = args[++i].getIntValue();
            engine.setTrackMidi(t, true);
            engine.syncNow();
            juce::String error;
            if (engine.setInstrument(t, args[++i], error) == nullptr)
                DBG("instrument failed: " + error);
        }
        else if (args[i] == "--insert" && i + 2 < args.size())   // --insert <track | master> <AU id, e.g. aufx,dely,appl, or an 8track effect, e.g. eightTrackFilter>
        {
            const int t = args[i + 1] == "master" ? AudioEngine::kMasterBus : args[i + 1].getIntValue();
            ++i;
            if (args[i + 1].startsWith("eightTrack"))
            {
                juce::String error;
                if (engine.addEffect(t, args[++i], error) == nullptr)
                    DBG("insert failed: " + error);
                continue;
            }
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

    const bool settings = args.contains("--settings"), midiSettings = args.contains("--midi-settings");
    juce::Timer::callAfterDelay(1200, [this, output, settings, midiSettings, args]
    {
        std::unique_ptr<juce::Component> dialog;
        if (settings)
            dialog = std::make_unique<AudioSettingsDialog>(engine);
        else if (midiSettings)
            dialog = std::make_unique<MidiSettingsDialog>(engine);
        if (dialog != nullptr)
            dialog->setVisible(true);

        // --plugin-window: the first plugin on track 1 (or the master), in its window.
        juce::Component* window = nullptr;
        if (args.contains("--plugin-window"))
        {
            auto inserts = engine.getInserts(0);
            if (inserts.empty())
                if (auto* instrument = engine.getInstrument(0))
                    inserts.push_back(instrument);
            if (inserts.empty())
                inserts = engine.getInserts(AudioEngine::kMasterBus);
            if (!inserts.empty())
            {
                inserts.front()->showWindowExplicitly();
                window = inserts.front()->windowState->pluginWindow.get();
            }
        }
        if (auto* doc = dynamic_cast<juce::DocumentWindow*>(window); doc != nullptr && doc->getContentComponent() != nullptr)
        {
            // The window swaps its "Opening..." placeholder for the editor, so find the content when the time comes.
            juce::Timer::callAfterDelay(800, [output, safeWindow = juce::Component::SafePointer<juce::DocumentWindow>(doc)]
            {
                auto* safe = safeWindow != nullptr ? safeWindow->getContentComponent() : nullptr;
                if (safe == nullptr)
                {
                    juce::JUCEApplication::getInstance()->quit();
                    return;
                }
                // The plugin's own view is native and doesn't draw into a snapshot; the 8track parts do.
                auto image = safe->createComponentSnapshot(safe->getLocalBounds(), true, 2.0f);
                output.deleteFile();
                juce::FileOutputStream stream(output);
                juce::PNGImageFormat().writeImageToStream(image, stream);
                stream.flush();
                juce::JUCEApplication::getInstance()->quit();
            });
            return;
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

#include "PluginWindows.h"
#include "Busy.h"
#include "EffectEditor.h"
#include "SamplerEditors.h"
#include "../Audio/Effects.h"
#include "Theme.h"

namespace
{
    AudioEngine* midiEngine = nullptr;

    /**
        The strip above a plugin's editor for MIDI assign. With Learn on, touch a control in
        the plugin, then move a knob or fader on a MIDI controller: that controller now moves
        the control. Learn stays on for the next one until it's switched off. The MIDI button
        lists this plugin's assignments, to remove them.
    */
    class MidiLearnBar : public juce::Component, private juce::Timer
    {
    public:
        static constexpr int kHeight = 30;

        MidiLearnBar(te::Plugin& p, AudioEngine& e) : plugin(p), engine(e)
        {
            learn.setClickingTogglesState(true);
            learn.setColour(juce::TextButton::buttonOnColourId, Theme::record);
            learn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            learn.setTooltip("Touch a control in the plugin, then move a knob or fader on your MIDI controller");
            learn.onClick = [this] { setLearning(learn.getToggleState()); };
            addAndMakeVisible(learn);

            assigned.setTooltip("This plugin's MIDI assignments");
            assigned.onClick = [this] { showAssignments(); };
            addAndMakeVisible(assigned);

            status.setColour(juce::Label::textColourId, Theme::textDim);
            status.setFont(Theme::font(12.0f));
            addAndMakeVisible(status);
            for (auto* c : { (juce::Component*) &learn, (juce::Component*) &assigned })
                c->setWantsKeyboardFocus(false);

            showAssigned();
            setSize(400, kHeight);
            startTimerHz(4);   // keeps the count right after undo
        }

        ~MidiLearnBar() override { setLearning(false); }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(Theme::panel);
            g.setColour(Theme::bg);
            g.drawHorizontalLine(getHeight() - 1, 0.0f, (float) getWidth());
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(6, 4);
            learn.setBounds(r.removeFromLeft(92));
            assigned.setBounds(r.removeFromRight(76));
            r.removeFromLeft(8);
            status.setBounds(r);
        }

    private:
        static inline juce::Component::SafePointer<MidiLearnBar> activeLearner;

        void setLearning(bool on)
        {
            if (on)
            {
                if (activeLearner != nullptr && activeLearner != this)
                    activeLearner->setLearning(false);   // one window learns at a time
                activeLearner = this;
                learning = true;
                touched = nullptr;
                snapshot();
                learnStarted = juce::Time::getMillisecondCounterHiRes();
                engine.onMidiLearn = [this](int channel, int controller) { controllerMoved(channel, controller); };
                status.setText("Touch a control in " + plugin.getName() + "...", juce::dontSendNotification);
                startTimerHz(20);
            }
            else if (learning)
            {
                learning = false;
                if (activeLearner == this)
                {
                    engine.onMidiLearn = nullptr;
                    activeLearner = nullptr;
                }
                status.setText({}, juce::dontSendNotification);
                startTimerHz(4);
            }
            learn.setToggleState(learning, juce::dontSendNotification);
            learn.setButtonText(learning ? "Learning..." : "MIDI Learn");
        }

        void snapshot()
        {
            values.clear();
            for (auto* p : plugin.getAutomatableParameters())
                values[p] = p->getCurrentNormalisedValue();
        }

        void timerCallback() override
        {
            showAssigned();
            if (!learning)
                return;

            // The control the user touched is the one whose value moved. Some plugins move
            // parameters by themselves (meters, LFOs): anything that moves in the first moments
            // of learning is ignored.
            const bool settling = juce::Time::getMillisecondCounterHiRes() - learnStarted < 400.0;
            for (auto* p : plugin.getAutomatableParameters())
            {
                const float v = p->getCurrentNormalisedValue();
                auto it = values.find(p);
                if (it == values.end() || std::abs(it->second - v) < 1.0e-4f)
                    continue;
                it->second = v;
                if (settling)
                    restless.insert(p);
                else if (restless.count(p) == 0 && touched != p)
                {
                    touched = p;
                    status.setText(p->getParameterName() + ": now move a knob or fader on your MIDI controller",
                                   juce::dontSendNotification);
                }
            }
        }

        void controllerMoved(int channel, int controller)
        {
            if (touched == nullptr)
            {
                status.setText("Touch a control in " + plugin.getName() + " first, then move the MIDI control",
                               juce::dontSendNotification);
                return;
            }
            engine.assignMidi(AudioEngine::parameterKey(*touched), channel, controller);
            status.setText(touched->getParameterName() + " follows CC " + juce::String(controller)
                               + " on channel " + juce::String(channel) + ". Touch another control, or stop learning.",
                           juce::dontSendNotification);
            touched = nullptr;
            snapshot();
            showAssigned();
        }

        /** This plugin's assignments, with their parameters. */
        std::vector<std::pair<AudioEngine::MidiAssignment, te::AutomatableParameter*>> mine() const
        {
            std::vector<std::pair<AudioEngine::MidiAssignment, te::AutomatableParameter*>> out;
            const auto params = plugin.getAutomatableParameters();
            for (auto& a : engine.getMidiAssignments())
                for (auto* p : params)
                    if (AudioEngine::parameterKey(*p) == a.key)
                        out.push_back({ a, p });
            return out;
        }

        void showAssigned()
        {
            const auto count = (int) mine().size();
            const auto text = count == 0 ? juce::String("MIDI") : "MIDI (" + juce::String(count) + ")";
            if (assigned.getButtonText() != text)
                assigned.setButtonText(text);
        }

        void showAssignments()
        {
            const auto list = mine();
            juce::PopupMenu menu;
            if (list.empty())
                menu.addItem(-1, "Nothing assigned yet: click MIDI Learn", false);
            else
                menu.addSectionHeader("Click one to remove it");
            for (size_t i = 0; i < list.size(); ++i)
                menu.addItem((int) i + 1, list[i].second->getParameterName() + "   CC " + juce::String(list[i].first.controller)
                                              + ", ch " + juce::String(list[i].first.channel));
            if (list.size() > 1)
            {
                menu.addSeparator();
                menu.addItem(1000, "Remove All");
            }

            std::vector<juce::String> keys;
            for (auto& [a, p] : list)
                keys.push_back(a.key);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&assigned),
                               [safe = SafePointer<MidiLearnBar>(this), keys](int result)
            {
                if (safe == nullptr || result <= 0)
                    return;
                if (result == 1000)
                    for (auto& k : keys)
                        safe->engine.removeMidiAssignment(k);
                else if (result - 1 < (int) keys.size())
                    safe->engine.removeMidiAssignment(keys[(size_t) (result - 1)]);
                safe->showAssigned();
            });
        }

        te::Plugin& plugin;
        AudioEngine& engine;
        juce::TextButton learn { "MIDI Learn" }, assigned { "MIDI" };
        juce::Label status;
        bool learning = false;
        double learnStarted = 0;
        std::map<te::AutomatableParameter*, float> values;
        std::set<te::AutomatableParameter*> restless;
        te::AutomatableParameter* touched = nullptr;
    };

    /** Exposes parameters for plugins without a working native editor. */
    class ParameterEditor final : public te::Plugin::EditorComponent
    {
    public:
        explicit ParameterEditor(juce::AudioProcessor& processor) : editor(processor)
        {
            addAndMakeVisible(editor);
            setSize(editor.getWidth(), editor.getHeight());
        }

        bool allowWindowResizing() override { return true; }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return editor.getConstrainer(); }
        void resized() override { editor.setBounds(getLocalBounds()); }

    private:
        juce::GenericAudioProcessorEditor editor;
    };

    /** The window's content: the MIDI Learn strip above the plugin's editor. Follows the
        editor's size, so plugins that resize themselves still fit. */
    class WindowContent : public juce::Component
    {
    public:
        WindowContent(te::Plugin& plugin, std::unique_ptr<te::Plugin::EditorComponent> e) : editor(std::move(e))
        {
            if (midiEngine != nullptr)
            {
                bar = std::make_unique<MidiLearnBar>(plugin, *midiEngine);
                addAndMakeVisible(*bar);
            }
            addAndMakeVisible(*editor);
            fit();
        }

        te::Plugin::EditorComponent& getEditor() { return *editor; }

        void resized() override
        {
            auto r = getLocalBounds();
            if (bar != nullptr)
                bar->setBounds(r.removeFromTop(MidiLearnBar::kHeight));
            if (!fitting)
                editor->setBounds(editor->allowWindowResizing() ? r : r.withSize(editor->getWidth(), editor->getHeight()));
        }

        void childBoundsChanged(juce::Component* child) override
        {
            if (child == editor.get())
                fit();
        }

    private:
        void fit()
        {
            const juce::ScopedValueSetter<bool> guard(fitting, true);
            const int barHeight = bar != nullptr ? MidiLearnBar::kHeight : 0;
            editor->setTopLeftPosition(0, barHeight);
            setSize(juce::jmax(bar != nullptr ? 380 : 8, editor->getWidth()), editor->getHeight() + barHeight);
        }

        std::unique_ptr<MidiLearnBar> bar;
        std::unique_ptr<te::Plugin::EditorComponent> editor;
        bool fitting = false;
    };

    class PluginWindow final : public juce::DocumentWindow, private juce::Timer
    {
    public:
        explicit PluginWindow(te::Plugin& p)
            : DocumentWindow(windowTitle(p), Theme::panel, DocumentWindow::closeButton), plugin(p)
        {
            setUsingNativeTitleBar(true);
            getConstrainer()->setMinimumOnscreenAmounts(80, 80, 80, 80);
            // Floats above the main window until closed: but only while 8track is the active app
            // and no dialog is up, so it never covers other apps or an alert.
            floating.tick = [this]
            {
                const bool shouldFloat = juce::Process::isForegroundProcess()
                                         && juce::Component::getCurrentlyModalComponent() == nullptr;
                if (shouldFloat != isAlwaysOnTop())
                    setAlwaysOnTop(shouldFloat);
            };
            floating.tick();
            floating.startTimer(250);
            setStatus("Opening plugin editor…");
            setTopLeftPosition(plugin.windowState->choosePositionForPluginWindow());
            ensureOnScreen();
            trackBounds = true;

            // Give the desktop window a peer before asking the plugin to build its view.
            startTimer(50);
        }

        ~PluginWindow() override
        {
            floating.stopTimer();
            stopTimer();
            trackBounds = false;
            plugin.edit.flushPluginStateIfNeeded(plugin);
            clearContentComponent();
        }

        void recreateEditorAsync()
        {
            // Release the editor now, before Tracktion can replace the processor.
            setStatus("Reopening plugin editor…");
            startTimer(50);
        }

        void showDrumPatterns()
        {
            if (auto* content = dynamic_cast<WindowContent*>(getContentComponent()))
                if (auto* drums = dynamic_cast<DrumSamplerEditor*>(&content->getEditor()))
                {
                    drums->showPatterns(true);
                    ensureOnScreen();
                    return;
                }
            showPatternsWhenReady = true;
        }

    private:
        static juce::String windowTitle(te::Plugin& p)
        {
            if (auto* track = p.getOwnerTrack())
                return p.getName() + " - " + track->getName();
            return p.getName() + " - Master";
        }

        void timerCallback() override
        {
            if (!isShowing() || isCurrentlyBlockedByAnotherModalComponent())
                return;

            auto* external = dynamic_cast<te::ExternalPlugin*>(&plugin);
            if (external != nullptr && external->isInitialisingAsync())
                return;

            stopTimer();
            auto* processor = external != nullptr ? external->getAudioPluginInstance() : nullptr;
            if (external != nullptr && processor == nullptr)
            {
                setStatus(external->getLoadError());
                return;
            }

            JUCE_AUTORELEASEPOOL
            {
                std::unique_ptr<te::Plugin::EditorComponent> editor;
                {
                    struct Blocker : juce::Component { void inputAttemptWhenModal() override {} };
                    Blocker blocker;
                    blocker.enterModalState(false);
                    editor = plugin.createEditor();
                }
                if (editor == nullptr && processor != nullptr && !processor->getParameters().isEmpty())
                    editor = std::make_unique<ParameterEditor>(*processor);
                if (std::exchange(showPatternsWhenReady, false))
                    if (auto* drums = dynamic_cast<DrumSamplerEditor*>(editor.get()))
                        drums->showPatterns(true);

                if (editor == nullptr)
                {
                    setStatus("This plugin did not provide an editor or any adjustable parameters.");
                    return;
                }

                const bool resizable = editor->allowWindowResizing();
                setContentOwned(new WindowContent(plugin, std::move(editor)), true);
                setResizable(resizable, false);
                ensureOnScreen();
            }
        }

        void setStatus(const juce::String& message)
        {
            auto status = std::make_unique<juce::Label>();
            status->setFont(Theme::font(14.0f));
            status->setColour(juce::Label::textColourId, Theme::text);
            status->setJustificationType(juce::Justification::centred);
            status->setText(message, juce::dontSendNotification);
            status->setSize(420, 92);
            setContentOwned(status.release(), true);
            setResizable(false, false);
            ensureOnScreen();
        }

        void ensureOnScreen()
        {
            const auto bounds = getBounds();
            const auto title = bounds.withHeight(28);
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            for (const auto& area : displays.getRectangleList(true))
                if (auto visible = title.getIntersection(area); visible.getWidth() >= 80 && visible.getHeight() >= 20)
                    return;

            const auto* display = displays.getDisplayForRect(bounds);
            if (display == nullptr)
                display = displays.getPrimaryDisplay();
            if (display != nullptr)
            {
                const auto area = display->userBounds.toNearestInt();
                setTopLeftPosition(area.getX() + juce::jmax(0, (area.getWidth() - getWidth()) / 2),
                                   area.getY() + juce::jmax(0, (area.getHeight() - getHeight()) / 2));
            }
        }

        void broughtToFront() override
        {
            juce::DocumentWindow::broughtToFront();
            ensureOnScreen();
        }

        void moved() override
        {
            if (trackBounds)
            {
                plugin.windowState->lastWindowBounds = getBounds();
                plugin.edit.pluginChanged(plugin);
            }
        }

        void closeButtonPressed() override { plugin.windowState->closeWindowExplicitly(); }
        float getDesktopScaleFactor() const override { return 1.0f; }

        te::Plugin& plugin;
        bool trackBounds = false;
        bool showPatternsWhenReady = false;
        struct Floating : juce::Timer
        {
            std::function<void()> tick;
            void timerCallback() override { tick(); }
        } floating;
    };
}

std::unique_ptr<juce::Component> PluginWindowBehaviour::createPluginWindow(te::PluginWindowState& state)
{
    auto* ws = dynamic_cast<te::Plugin::WindowState*>(&state);
    if (ws == nullptr)
        return {};

    return std::make_unique<PluginWindow>(ws->plugin);
}

void PluginWindowBehaviour::recreatePluginWindowContentAsync(te::Plugin& plugin)
{
    if (auto* window = dynamic_cast<PluginWindow*>(plugin.windowState->pluginWindow.get()))
        return window->recreateEditorAsync();
    UIBehaviour::recreatePluginWindowContentAsync(plugin);
}

//==============================================================================
void showPluginManager(AudioEngine& audio)
{
    auto& engine = audio.getTracktion();
    auto& manager = engine.getPluginManager();

    auto* list = new juce::PluginListComponent(manager.pluginFormatManager,
                                               manager.knownPluginList,
                                               engine.getTemporaryFileManager().getTempFile("PluginScanDeadMansPedal"),
                                               std::addressof(engine.getPropertyStorage().getPropertiesFile()),
                                               true);
    // 8track's own effects and instruments aren't scanned plugins, so the list doesn't show
    // them: say what they are and where to find them, above it.
    struct WithBuiltIns : juce::Component
    {
        explicit WithBuiltIns(juce::Component* l) : list(l)
        {
            juce::StringArray effects, instruments, midi;
            for (auto& t : EffectPlugin::getTypes())
                (t.kind == EffectPlugin::Kind::Instrument ? instruments : t.kind == EffectPlugin::Kind::MidiEffect ? midi : effects).add(t.name);
            note.setText("8track's own (always available, no scan needed)\n"
                         "Effects, in the + menu of any insert slot: " + effects.joinIntoString(", ") + "\n"
                         "Instruments, in a MIDI track's instrument menu: " + instruments.joinIntoString(", ") + "\n"
                         "MIDI effects, in the + menu of a MIDI track: " + midi.joinIntoString(", "),
                         juce::dontSendNotification);
            note.setFont(Theme::font(12.0f));
            note.setColour(juce::Label::textColourId, Theme::text);
            note.setJustificationType(juce::Justification::topLeft);
            addAndMakeVisible(note);
            addAndMakeVisible(*list);
            setSize(760, 600);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            note.setBounds(r.removeFromTop(80).reduced(12, 8));
            list->setBounds(r);
        }
        juce::Label note;
        std::unique_ptr<juce::Component> list;
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new WithBuiltIns(list));
    options.dialogTitle = "Plugins";
    options.dialogBackgroundColour = Theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.useBottomRightCornerResizer = true;
    options.launchAsync();
}

bool showsThirdPartyPlugins(AudioEngine& audio)
{
    return audio.getTracktion().getPropertyStorage().getPropertiesFile().getBoolValue("showThirdPartyPlugins", true);
}

void setShowsThirdPartyPlugins(AudioEngine& audio, bool show)
{
    auto& props = audio.getTracktion().getPropertyStorage().getPropertiesFile();
    props.setValue("showThirdPartyPlugins", show);
    props.saveIfNeeded();
}

void PluginWindowBehaviour::runTaskWithProgressBar(te::ThreadPoolJobWithProgress& task)
{
    Busy::run(task.getJobName(),
              [&task] { while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain) {} },
              [&task] { return task.getCurrentTaskProgress(); });
}

void PluginWindowBehaviour::setEngine(AudioEngine* e, Project* project)
{
    midiEngine = e;
    EffectEditor::install(e, project);   // 8track's own effects open in the same windows
}

void PluginWindowBehaviour::showDrumPatterns(DrumSampler& sampler)
{
    sampler.showWindowExplicitly();
    if (auto* window = dynamic_cast<PluginWindow*>(sampler.windowState->pluginWindow.get()))
        window->showDrumPatterns();
}

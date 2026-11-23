#include "PluginWindows.h"
#include "Theme.h"

namespace
{
    /** Hosts a plugin's own editor (or JUCE's generic one when it has none). */
    struct EditorContent : te::Plugin::EditorComponent
    {
        explicit EditorContent(te::ExternalPlugin& p) : plugin(p)
        {
            JUCE_AUTORELEASEPOOL
            {
                if (auto* instance = plugin.getAudioPluginInstance())
                {
                    editor.reset(instance->createEditorAndMakeActive());
                    if (editor == nullptr)
                        editor = std::make_unique<juce::GenericAudioProcessorEditor>(*instance);
                    addAndMakeVisible(*editor);
                }
            }
            fitEditor();
        }

        bool allowWindowResizing() override { return false; }

        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override
        {
            return editor != nullptr ? editor->getConstrainer() : nullptr;
        }

        void resized() override
        {
            if (editor != nullptr)
                editor->setBounds(getLocalBounds());
        }

        void childBoundsChanged(juce::Component* child) override
        {
            if (child == editor.get())
            {
                plugin.edit.pluginChanged(plugin);
                fitEditor();
            }
        }

        void fitEditor()
        {
            setSize(juce::jmax(8, editor != nullptr ? editor->getWidth() : 0),
                    juce::jmax(8, editor != nullptr ? editor->getHeight() : 0));
        }

        te::ExternalPlugin& plugin;
        std::unique_ptr<juce::AudioProcessorEditor> editor;
    };

    class PluginWindow : public juce::DocumentWindow
    {
    public:
        explicit PluginWindow(te::Plugin& p)
            : DocumentWindow(windowTitle(p), Theme::panel, DocumentWindow::closeButton), plugin(p)
        {
            setUsingNativeTitleBar(true);
            getConstrainer()->setMinimumOnscreenAmounts(0x10000, 50, 30, 50);
            recreateEditor();
            setBoundsConstrained(getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());
            trackBounds = true;
        }

        ~PluginWindow() override
        {
            trackBounds = false;
            plugin.edit.flushPluginStateIfNeeded(plugin);   // keep the plugin's settings in the project
            setEditor(nullptr);
        }

        bool hasEditor() const { return editor != nullptr; }

        void show()
        {
            setVisible(true);
            toFront(true);
            setBoundsConstrained(getBounds());
        }

        void recreateEditor()
        {
            setEditor(nullptr);
            setEditor(plugin.createEditor());
        }

        void recreateEditorAsync()
        {
            setEditor(nullptr);
            juce::Timer::callAfterDelay(50, [safe = SafePointer<PluginWindow>(this)]
            {
                if (safe != nullptr)
                    safe->recreateEditor();
            });
        }

    private:
        static juce::String windowTitle(te::Plugin& p)
        {
            if (auto* track = p.getOwnerTrack())
                return p.getName() + " - " + track->getName();
            return p.getName();
        }

        void setEditor(std::unique_ptr<te::Plugin::EditorComponent> newEditor)
        {
            JUCE_AUTORELEASEPOOL
            {
                setConstrainer(nullptr);
                editor.reset();
                if (newEditor != nullptr)
                {
                    editor = std::move(newEditor);
                    setContentNonOwned(editor.get(), true);
                }
                setResizable(editor == nullptr || editor->allowWindowResizing(), false);
            }
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
        std::unique_ptr<te::Plugin::EditorComponent> editor;
        bool trackBounds = false;
    };
}

std::unique_ptr<juce::Component> PluginWindowBehaviour::createPluginWindow(te::PluginWindowState& state)
{
    auto* ws = dynamic_cast<te::Plugin::WindowState*>(&state);
    if (ws == nullptr)
        return {};

    auto& plugin = ws->plugin;
    if (auto* external = dynamic_cast<te::ExternalPlugin*>(&plugin))
        if (external->getAudioPluginInstance() == nullptr)
            return {};

    std::unique_ptr<PluginWindow> window;
    {
        // Some plugins pump the message loop while opening; keep clicks out of 8track meanwhile.
        struct Blocker : juce::Component { void inputAttemptWhenModal() override {} };
        Blocker blocker;
        blocker.enterModalState(false);
        window = std::make_unique<PluginWindow>(plugin);
    }

    if (!window->hasEditor())
        return {};
    window->show();
    return window;
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
    list->setSize(760, 520);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(list);
    options.dialogTitle = "Plugins";
    options.dialogBackgroundColour = Theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.useBottomRightCornerResizer = true;
    options.launchAsync();
}

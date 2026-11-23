#include <juce_gui_extra/juce_gui_extra.h>
#include "UI/MainComponent.h"

class EightTrackApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        mainWindow = std::make_unique<MainWindow>(getApplicationName());

       #if JUCE_DEBUG
        const auto args = getCommandLineParameterArray();
        if (args.contains("--snapshot"))
            if (auto* content = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                content->runDebugSnapshot(args);
        if (args.contains("--diagnose"))
            if (auto* content = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                content->runDiagnostics();
       #endif
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr)
        {
            quit();
            return;
        }
        if (auto* content = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
            content->requestQuit([] { JUCEApplication::getInstance()->quit(); });
        else
            quit();
    }

    void anotherInstanceStarted(const juce::String&) override {}

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name, Theme::bg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(1200, 560, 10000, 10000);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    DawLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(EightTrackApplication)

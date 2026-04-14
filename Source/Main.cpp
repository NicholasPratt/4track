#include <juce_gui_extra/juce_gui_extra.h>
#include "UI/MainComponent.h"

class FourTrackApplication : public juce::JUCEApplication
{
public:
    FourTrackApplication() {}

    const juce::String getApplicationName() override { return "4Track Recorder"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);

        // Set up file logging - use Documents folder which we know works
        auto logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                          .getChildFile("4track-debug.log");

        fileLogger = std::make_unique<juce::FileLogger>(logFile, "4Track Recorder Log", 1024 * 1024);
        juce::Logger::setCurrentLogger(fileLogger.get());

        juce::Logger::writeToLog("========================================");
        juce::Logger::writeToLog("4Track Recorder v0.5.0 Starting");
        juce::Logger::writeToLog("Log file: " + logFile.getFullPathName());
        juce::Logger::writeToLog("========================================");

        // Also output to stderr for debugging
        std::cerr << "4Track starting - log file: " << logFile.getFullPathName().toStdString() << std::endl;

        mainWindow.reset(new MainWindow(getApplicationName()));
    }

    void shutdown() override
    {
        juce::Logger::writeToLog("4Track Recorder Shutting Down");
        mainWindow = nullptr;
        juce::Logger::setCurrentLogger(nullptr);
        fileLogger = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(juce::String name)
            : DocumentWindow(name,
                juce::Desktop::getInstance().getDefaultLookAndFeel()
                    .findColour(juce::ResizableWindow::backgroundColourId),
                DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

#if JUCE_IOS || JUCE_ANDROID
            setFullScreen(true);
#else
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
#endif

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
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> fileLogger;
};

START_JUCE_APPLICATION(FourTrackApplication)

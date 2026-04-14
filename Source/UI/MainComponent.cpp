#include "MainComponent.h"
#include "AudioSettingsDialog.h"
#include "../Audio/TrackManager.h"

MainComponent::MainComponent()
{
    // Create audio engine
    audioEngine = std::make_unique<AudioEngine>();

    // Title label
    titleLabel.setText("4-TRACK RECORDER", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(28.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    // Version label
    versionLabel.setText("v" APP_VERSION, juce::dontSendNotification);
    versionLabel.setFont(juce::Font(12.0f));
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(versionLabel);

    // Settings button
    settingsButton.setButtonText("Audio Settings");
    settingsButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);
    settingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    settingsButton.addListener(this);
    addAndMakeVisible(settingsButton);

    // Save button
    saveButton.setButtonText("Save As...");
    saveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    saveButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    saveButton.addListener(this);
    addAndMakeVisible(saveButton);

    // Load button
    loadButton.setButtonText("Load Project");
    loadButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkblue);
    loadButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    loadButton.addListener(this);
    addAndMakeVisible(loadButton);

    // New Project button
    newButton.setButtonText("New");
    newButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    newButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    newButton.addListener(this);
    addAndMakeVisible(newButton);

    // Render button
    renderButton.setButtonText("Render");
    renderButton.setColour(juce::TextButton::buttonColourId, juce::Colours::purple);
    renderButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    renderButton.addListener(this);
    addAndMakeVisible(renderButton);

    // Create 4 track strips
    auto* trackManager = audioEngine->getTrackManager();
    for (int i = 0; i < 4; ++i)
    {
        trackStrips[i] = std::make_unique<TrackStrip>(trackManager->getTrack(i));
        addAndMakeVisible(*trackStrips[i]);
    }

    // Create transport bar
    transportBar = std::make_unique<TransportBar>(
        audioEngine->getTransport(),
        audioEngine->getTrackManager(),
        audioEngine->getSampleRate()
    );
    addAndMakeVisible(*transportBar);

    // Listen for device changes
    audioEngine->getDeviceChangeBroadcaster().addChangeListener(this);

    // Initialize track input selectors with current device
    updateTrackInputSelectors();

    // Create default project folder
    createProjectFolderIfNeeded();

    // Listen for transport state changes to start/stop file recording
    audioEngine->getTransport()->onStateChanged([this](TransportController::State state)
    {
        if (state == TransportController::State::RECORDING)
        {
            // Start recording to files
            auto projectFolder = audioEngine->getProjectFolder();
            if (projectFolder != juce::File())
            {
                audioEngine->getTrackManager()->startRecordingToFiles(projectFolder);
            }
        }
        else if (state == TransportController::State::PLAYING &&
                 audioEngine->getTransport()->isPunchModeEnabled())
        {
            // Punch out: RECORDING → PLAYING while punch mode is on.
            // Flush writers without reloading from file — the memory buffer
            // already contains the full track (pre-punch + punched section).
            audioEngine->getTrackManager()->stopRecordingToFilesForPunch();
        }
        else
        {
            // Normal stop: load the recorded files back into memory for playback
            audioEngine->getTrackManager()->stopRecordingToFiles();
        }
    });

    // Register as global key listener so shortcuts work regardless of focus
    setWantsKeyboardFocus(true);

    setSize(1000, 700);
}

void MainComponent::parentHierarchyChanged()
{
    // Register as a global key listener on the top-level window so shortcuts
    // work regardless of which child component currently has keyboard focus.
    if (auto* topLevel = getTopLevelComponent())
        topLevel->addKeyListener(this);
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (!transportBar)
        return false;

    if (key == juce::KeyPress('['))
    {
        transportBar->setMarker(1);
        return true;
    }
    if (key == juce::KeyPress(']'))
    {
        transportBar->setMarker(2);
        return true;
    }
    if (key.getKeyCode() == 'P' || key.getKeyCode() == 'p')
    {
        transportBar->togglePunchMode();
        return true;
    }
    return false;
}

MainComponent::~MainComponent()
{
    // Unregister key listener
    if (auto* topLevel = getTopLevelComponent())
        topLevel->removeKeyListener(this);

    // Auto-save on exit
    autoSaveTracks();

    if (audioEngine)
        audioEngine->getDeviceChangeBroadcaster().removeChangeListener(this);

    settingsButton.removeListener(this);
    newButton.removeListener(this);
    saveButton.removeListener(this);
    loadButton.removeListener(this);
    renderButton.removeListener(this);
    transportBar.reset();
    for (auto& strip : trackStrips)
        strip.reset();
    audioEngine.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    // Black background
    g.fillAll(juce::Colours::black);

    // White border around everything
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 3);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Title at top
    auto titleArea = bounds.removeFromTop(50);
    titleLabel.setBounds(titleArea.removeFromLeft(titleArea.getWidth() - 520));
    newButton.setBounds(titleArea.removeFromLeft(60).reduced(2));
    saveButton.setBounds(titleArea.removeFromLeft(80).reduced(2));
    loadButton.setBounds(titleArea.removeFromLeft(80).reduced(2));
    renderButton.setBounds(titleArea.removeFromLeft(70).reduced(2));
    settingsButton.setBounds(titleArea.removeFromLeft(110).reduced(2));
    versionLabel.setBounds(titleArea);

    bounds.removeFromTop(10); // spacing

    // Transport bar at top
    transportBar->setBounds(bounds.removeFromTop(70).reduced(5));

    bounds.removeFromTop(10); // spacing

    // 4 track strips side by side
    auto trackArea = bounds;
    auto trackWidth = trackArea.getWidth() / 4;

    for (int i = 0; i < 4; ++i)
    {
        trackStrips[i]->setBounds(trackArea.removeFromLeft(trackWidth).reduced(5));
    }
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &settingsButton)
    {
        showAudioSettingsDialog();
    }
    else if (button == &newButton)
    {
        newProject();
    }
    else if (button == &saveButton)
    {
        saveProject();
    }
    else if (button == &loadButton)
    {
        loadProject();
    }
    else if (button == &renderButton)
    {
        renderProject();
    }
}

void MainComponent::showAudioSettingsDialog()
{
    auto dialogContent = std::make_unique<AudioSettingsDialog>(audioEngine->getDeviceManager());

    juce::DialogWindow::LaunchOptions options;
    options.content.set(dialogContent.release(), true);
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = juce::Colours::black;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    options.launchAsync();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &audioEngine->getDeviceChangeBroadcaster())
    {
        // Audio device changed, update track input selectors and transport bar sample rate
        updateTrackInputSelectors();

        if (transportBar)
        {
            transportBar->setSampleRate(audioEngine->getSampleRate());
            juce::Logger::writeToLog("Updated transport bar sample rate: " + juce::String(audioEngine->getSampleRate()));
        }
    }
}

void MainComponent::updateTrackInputSelectors()
{
    if (!audioEngine)
        return;

    int numInputs = audioEngine->getNumInputChannels();

    juce::Logger::writeToLog("Updating track input selectors with " + juce::String(numInputs) + " inputs");

    for (auto& strip : trackStrips)
    {
        if (strip)
            strip->updateInputChannelOptions(numInputs);
    }
}

juce::File MainComponent::getProjectFolder()
{
    if (currentProjectFolder == juce::File())
    {
        // Use a single "Current" folder that gets reused for each session
        // Files are overwritten until user explicitly saves to a new directory
        currentProjectFolder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                   .getChildFile("4track-projects")
                                   .getChildFile("Current");
    }
    return currentProjectFolder;
}

void MainComponent::createProjectFolderIfNeeded()
{
    juce::File projectFolder = getProjectFolder();
    bool folderCreated = false;

    if (!projectFolder.exists())
    {
        projectFolder.createDirectory();
        juce::Logger::writeToLog("Created project folder: " + projectFolder.getFullPathName());
        folderCreated = true;
    }

    // Set the project folder in AudioEngine
    if (audioEngine)
    {
        audioEngine->setProjectFolder(projectFolder);
    }

    // Load existing tracks if folder already existed
    if (!folderCreated && audioEngine)
    {
        auto* trackManager = audioEngine->getTrackManager();
        for (int i = 0; i < 4; ++i)
        {
            juce::File trackFile = projectFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");
            if (trackFile.existsAsFile())
            {
                auto* track = trackManager->getTrack(i);
                if (track)
                {
                    track->loadFromFile(trackFile);
                    juce::Logger::writeToLog("Auto-loaded existing " + trackFile.getFileName());
                }
            }
        }
    }
}

void MainComponent::saveProject()
{
    if (!audioEngine)
        return;

    // Let user choose a new directory to save to
    auto chooser = std::make_shared<juce::FileChooser>("Save Project As...",
                                                        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                            .getChildFile("4track-projects"),
                                                        "",
                                                        true);

    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync(chooserFlags, [this, chooser](const juce::FileChooser&)
    {
        auto result = chooser->getResult();
        if (result == juce::File())
            return; // User cancelled

        // Create the target directory
        if (!result.exists())
            result.createDirectory();

        // Copy all track files to the new location
        auto currentFolder = audioEngine->getProjectFolder();

        for (int i = 0; i < 4; ++i)
        {
            juce::File sourceFile = currentFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");
            juce::File destFile = result.getChildFile("Track" + juce::String(i + 1) + ".wav");

            if (sourceFile.existsAsFile())
            {
                sourceFile.copyFileTo(destFile);
                juce::Logger::writeToLog("Saved " + destFile.getFileName() + " to " + result.getFullPathName());
            }
        }

        juce::NativeMessageBox::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Project Saved",
            "Project saved to:\n" + result.getFullPathName(),
            nullptr);
    });
}

void MainComponent::loadProject()
{
    if (!audioEngine)
        return;

    auto chooser = std::make_shared<juce::FileChooser>("Select Project Folder",
                                                        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("4track-projects"));

    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync(chooserFlags, [this, chooser](const juce::FileChooser&)
    {
        juce::File selectedFolder = chooser->getResult();

        if (selectedFolder == juce::File() || !selectedFolder.isDirectory())
            return;

        currentProjectFolder = selectedFolder;

        auto* trackManager = audioEngine->getTrackManager();

        for (int i = 0; i < 4; ++i)
        {
            juce::File trackFile = currentProjectFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");

            if (trackFile.existsAsFile())
            {
                auto* track = trackManager->getTrack(i);
                if (track)
                {
                    track->loadFromFile(trackFile);
                }
            }
        }

        // Reset to beginning after loading
        audioEngine->getTransport()->stop();

        juce::NativeMessageBox::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Project Loaded",
            "Project loaded from:\n" + currentProjectFolder.getFullPathName(),
            nullptr);
    });
}

void MainComponent::autoSaveTracks()
{
    if (!audioEngine)
        return;

    createProjectFolderIfNeeded();

    auto* trackManager = audioEngine->getTrackManager();
    double sampleRate = audioEngine->getSampleRate();

    int savedCount = 0;

    for (int i = 0; i < 4; ++i)
    {
        auto* track = trackManager->getTrack(i);
        if (track && track->hasRecordedAudio())
        {
            juce::File trackFile = currentProjectFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");
            if (track->saveToFile(trackFile, sampleRate))
            {
                savedCount++;
            }
        }
    }

    if (savedCount > 0)
    {
        juce::Logger::writeToLog("Auto-saved " + juce::String(savedCount) + " track(s) to " + currentProjectFolder.getFullPathName());
    }
}

void MainComponent::newProject()
{
    if (!audioEngine)
        return;

    auto* trackManager = audioEngine->getTrackManager();

    // Check whether there is any recorded audio to warn about
    bool hasAudio = false;
    for (int i = 0; i < 4; ++i)
    {
        auto* track = trackManager->getTrack(i);
        if (track && track->hasRecordedAudio())
        {
            hasAudio = true;
            break;
        }
    }

    if (hasAudio)
    {
        int choice = juce::AlertWindow::showYesNoCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "New Project",
            "You have recorded audio in the current session.\n\n"
            "Starting a new project will permanently delete all track audio files from disk. "
            "This cannot be undone.\n\n"
            "Save your project first if you want to keep this session.",
            "Save First",
            "Discard & New",
            "Cancel",
            nullptr, nullptr);

        if (choice == 0)
            return; // Cancel

        if (choice == 1)
        {
            // Save first — user clicks New again after saving
            saveProject();
            return;
        }
        // choice == 2: Discard & New — fall through
    }

    // Stop transport
    audioEngine->getTransport()->stop();

    // The "Current" scratch folder is always managed by audioEngine, not
    // currentProjectFolder (which may point to a loaded saved project).
    juce::File workingFolder = audioEngine->getProjectFolder();

    // Delete track WAVs from the working folder
    if (workingFolder.exists())
    {
        for (int i = 0; i < 4; ++i)
        {
            juce::File trackFile = workingFolder.getChildFile("Track" + juce::String(i + 1) + ".wav");
            if (trackFile.existsAsFile())
                trackFile.deleteFile();
        }
        juce::Logger::writeToLog("New project: deleted track WAVs from " + workingFolder.getFullPathName());
    }

    // Reset the active project pointer back to the working folder
    currentProjectFolder = workingFolder;

    // Clear all tracks in memory
    for (int i = 0; i < 4; ++i)
    {
        auto* track = trackManager->getTrack(i);
        if (track)
        {
            track->clearBuffer();
            track->setRecordArmed(false);
            track->setMuted(false);
            track->setSolo(false);
        }
    }

    // Sync UI button states
    for (auto& strip : trackStrips)
    {
        if (strip)
            strip->resetUI();
    }

    juce::Logger::writeToLog("New project created - all tracks cleared");
}

void MainComponent::renderProject()
{
    if (!audioEngine)
        return;

    auto* trackManager = audioEngine->getTrackManager();
    double sampleRate = audioEngine->getSampleRate();

    // Find the longest track to determine render length
    int maxLength = 0;
    bool hasAudio = false;

    for (int i = 0; i < 4; ++i)
    {
        auto* track = trackManager->getTrack(i);
        if (track && track->hasRecordedAudio())
        {
            hasAudio = true;
            maxLength = juce::jmax(maxLength, track->getBufferLength());
        }
    }

    if (!hasAudio)
    {
        juce::NativeMessageBox::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Render",
            "No audio to render. Record something first!");
        return;
    }

    // Let user choose output file
    auto chooser = std::make_shared<juce::FileChooser>(
        "Save Rendered Mix As...",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("4track-mix.wav"),
        "*.wav");

    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync(chooserFlags, [this, chooser, maxLength, sampleRate](const juce::FileChooser&)
    {
        auto result = chooser->getResult();
        if (result == juce::File())
            return; // User cancelled

        // Ensure .wav extension
        if (!result.hasFileExtension(".wav"))
            result = result.withFileExtension(".wav");

        // Delete existing file
        if (result.exists())
            result.deleteFile();

        // Create output buffer for the mix
        juce::AudioBuffer<float> mixBuffer(2, maxLength);
        mixBuffer.clear();

        auto* trackManager = audioEngine->getTrackManager();

        // Check solo state
        bool soloActive = trackManager->anySoloEnabled();

        // Mix all tracks
        for (int i = 0; i < 4; ++i)
        {
            auto* track = trackManager->getTrack(i);
            if (!track || !track->hasRecordedAudio())
                continue;

            // Skip if muted or if solo is active and this track isn't soloed
            if (track->isMuted())
                continue;
            if (soloActive && !track->isSolo())
                continue;

            // Render this track into the mix
            track->setPlaybackPosition(0);
            track->processBlock(mixBuffer, 0, maxLength);
        }

        // Add reverb if there are sends
        // Create reverb send buffer and process
        juce::AudioBuffer<float> reverbSendBuffer(2, maxLength);
        reverbSendBuffer.clear();

        for (int i = 0; i < 4; ++i)
        {
            auto* track = trackManager->getTrack(i);
            if (!track || !track->hasRecordedAudio())
                continue;

            if (track->isMuted())
                continue;
            if (soloActive && !track->isSolo())
                continue;

            float sendLevel = track->getReverbSend();
            if (sendLevel > 0.001f)
            {
                track->setPlaybackPosition(0);
                track->addToReverbSend(reverbSendBuffer, 0, maxLength, sendLevel);
            }
        }

        // Process reverb and add to mix
        if (reverbSendBuffer.getMagnitude(0, maxLength) > 0.0001f)
        {
            juce::Reverb renderReverb;
            renderReverb.setSampleRate(sampleRate);

            juce::Reverb::Parameters params;
            params.roomSize = trackManager->getReverbRoomSize();
            params.damping = trackManager->getReverbDamping();
            params.wetLevel = trackManager->getReverbWetLevel();
            params.dryLevel = 0.0f;
            params.width = 1.0f;
            params.freezeMode = 0.0f;
            renderReverb.setParameters(params);

            // Process reverb in chunks to handle long files
            const int chunkSize = 512;
            for (int pos = 0; pos < maxLength; pos += chunkSize)
            {
                int numSamples = juce::jmin(chunkSize, maxLength - pos);
                renderReverb.processStereo(
                    reverbSendBuffer.getWritePointer(0, pos),
                    reverbSendBuffer.getWritePointer(1, pos),
                    numSamples);
            }

            // Add reverb to mix
            for (int ch = 0; ch < 2; ++ch)
            {
                mixBuffer.addFrom(ch, 0, reverbSendBuffer, ch, 0, maxLength);
            }
        }

        // Write to WAV file
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> fileStream(result.createOutputStream());

        if (fileStream == nullptr)
        {
            juce::NativeMessageBox::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Render Error",
                "Could not create output file.");
            return;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(fileStream.get(), sampleRate, 2, 24, {}, 0));

        if (writer == nullptr)
        {
            juce::NativeMessageBox::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Render Error",
                "Could not create WAV writer.");
            return;
        }

        fileStream.release(); // Writer takes ownership

        writer->writeFromAudioSampleBuffer(mixBuffer, 0, maxLength);

        double durationSeconds = static_cast<double>(maxLength) / sampleRate;

        juce::NativeMessageBox::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Render Complete",
            "Rendered " + juce::String(durationSeconds, 1) + " seconds to:\n" + result.getFullPathName());

        juce::Logger::writeToLog("Rendered mix to: " + result.getFullPathName());
    });
}

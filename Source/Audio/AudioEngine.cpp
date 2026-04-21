#include "AudioEngine.h"
#include "TrackManager.h"

AudioEngine::AudioEngine()
{
    // Create components
    trackManager = std::make_unique<TrackManager>();
    transport = std::make_unique<TransportController>();

    // Listen for device changes
    deviceManager.addChangeListener(this);

    // macOS: recording will be silent unless mic permission is granted.
    // Ask up-front and only start the device when permitted.
    auto startAudio = [this]() { initialiseAudioDevice(); };

    if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio)
        || !juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio))
    {
        startAudio();
    }
    else
    {
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
            [this, startAudio](bool granted)
            {
                if (granted)
                {
                    juce::Logger::writeToLog("Microphone permission granted");
                    juce::MessageManager::callAsync(startAudio);
                }
                else
                {
                    juce::Logger::writeToLog("Microphone permission denied - recording will be silent.");
                }
            });
    }
}

AudioEngine::~AudioEngine()
{
    shutdownAudio();
}

void AudioEngine::shutdownAudio()
{
    deviceManager.removeChangeListener(this);
    if (audioDeviceInitialised)
    {
        deviceManager.removeAudioCallback(this);
        deviceManager.closeAudioDevice();
        audioDeviceInitialised = false;
    }
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (!device)
        return;

    currentSampleRate = device->getCurrentSampleRate();
    currentBufferSize = device->getCurrentBufferSizeSamples();

    juce::Logger::writeToLog("Audio device starting - SR: " + juce::String(currentSampleRate) +
                             " Hz, Buffer: " + juce::String(currentBufferSize));

    // Prepare track manager
    if (trackManager)
    {
        trackManager->prepareToPlay(currentSampleRate, currentBufferSize);
    }
}

void AudioEngine::saveDeviceState()
{
    auto state = deviceManager.createStateXml();
    if (state == nullptr)
        return;

    juce::File stateFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile("4track")
                               .getChildFile("audioDevice.xml");

    stateFile.getParentDirectory().createDirectory();
    stateFile.replaceWithText(state->toString());
    juce::Logger::writeToLog("AudioEngine: Saved device state to " + stateFile.getFullPathName());
}

std::unique_ptr<juce::XmlElement> AudioEngine::loadSavedDeviceState()
{
    juce::File stateFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile("4track")
                               .getChildFile("audioDevice.xml");

    if (!stateFile.existsAsFile())
        return nullptr;

    auto xml = juce::XmlDocument::parse(stateFile);
    if (xml != nullptr)
        juce::Logger::writeToLog("AudioEngine: Loaded saved device state from " + stateFile.getFullPathName());

    return xml;
}

void AudioEngine::initialiseAudioDevice()
{
    if (audioDeviceInitialised)
        return;

    juce::Logger::writeToLog("AudioEngine: Initializing audio device...");

    auto savedState = loadSavedDeviceState();

    juce::String error;

    if (savedState != nullptr)
    {
        // Restore the user's previously chosen device / channel configuration
        error = deviceManager.initialise(8, 2, savedState.get(), true);
        juce::Logger::writeToLog("AudioEngine: Initialised from saved state");
    }
    else
    {
        // First run: request default device with up to 8 input channels
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        setup.inputChannels.setRange(0, 8, true);
        setup.outputChannels.setRange(0, 2, true);
        setup.useDefaultInputChannels = false;
        setup.useDefaultOutputChannels = true;

        juce::Logger::writeToLog("AudioEngine: Requesting input channels: " +
                                 juce::String(setup.inputChannels.countNumberOfSetBits()));

        error = deviceManager.initialise(8, 2, nullptr, true, {}, &setup);
    }

    if (error.isNotEmpty())
    {
        juce::Logger::writeToLog("Audio initialization error: " + error);

        // Try fallback with simpler initialization
        juce::Logger::writeToLog("Trying fallback initialization...");
        error = deviceManager.initialiseWithDefaultDevices(2, 2);

        if (error.isNotEmpty())
        {
            juce::Logger::writeToLog("Fallback also failed: " + error);
            return;
        }
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device)
    {
        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        juce::Logger::writeToLog("Audio device: " + device->getName());
        juce::Logger::writeToLog("Sample rate: " + juce::String(currentSampleRate));
        juce::Logger::writeToLog("Buffer size: " + juce::String(currentBufferSize));

        auto activeInputChannels = device->getActiveInputChannels();
        auto activeOutputChannels = device->getActiveOutputChannels();

        juce::Logger::writeToLog("Active input channels: " + juce::String(activeInputChannels.countNumberOfSetBits()));
        juce::Logger::writeToLog("Active output channels: " + juce::String(activeOutputChannels.countNumberOfSetBits()));

        // Log which specific channels are active
        juce::String inputChannelList;
        for (int i = 0; i < 16; ++i)
        {
            if (activeInputChannels[i])
                inputChannelList += juce::String(i) + " ";
        }
        juce::Logger::writeToLog("Input channel indices: " + inputChannelList);

        // Register this as the audio callback
        deviceManager.addAudioCallback(this);
        audioDeviceInitialised = true;

        // Notify UI to refresh input selector options
        deviceChangeBroadcaster.sendChangeMessage();
    }
    else
    {
        juce::Logger::writeToLog("WARNING: No audio device found!");
    }
}

void AudioEngine::audioDeviceStopped()
{
    juce::Logger::writeToLog("Audio device stopped");

    if (trackManager)
    {
        trackManager->releaseResources();
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);

    // Debug logging (once every ~1000 callbacks)
    if (++debugCallbackCounter % 1000 == 0)
    {
        auto state = transport->getState();
        juce::String stateStr = state == TransportController::State::PLAYING ? "PLAYING" :
                               state == TransportController::State::RECORDING ? "RECORDING" : "STOPPED";

        // Calculate peak input level for debugging
        float inputPeak = 0.0f;
        for (int ch = 0; ch < numInputChannels; ++ch)
        {
            if (inputChannelData[ch] != nullptr)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    inputPeak = juce::jmax(inputPeak, std::abs(inputChannelData[ch][i]));
                }
            }
        }

        juce::Logger::writeToLog("Audio callback - inputs: " + juce::String(numInputChannels) +
                                 ", outputs: " + juce::String(numOutputChannels) +
                                 ", samples: " + juce::String(numSamples) +
                                 ", state: " + stateStr +
                                 ", position: " + juce::String(transport->getPosition()) +
                                 ", inputPeak: " + juce::String(inputPeak, 4));
    }

    // Clear output first
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (outputChannelData[ch] != nullptr)
        {
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }

    if (!trackManager || !transport)
        return;

    // Synchronize trackManager position with transport BEFORE processing
    trackManager->setPlaybackPosition(static_cast<int>(transport->getPosition()));

    // Create JUCE audio buffers for easier manipulation
    juce::AudioBuffer<float> inputBuffer(const_cast<float**>(inputChannelData),
                                        numInputChannels, numSamples);

    juce::AudioBuffer<float> outputBuffer(outputChannelData,
                                         numOutputChannels, numSamples);

    // Process audio through track manager
    bool isRecording = transport->isRecording();
    bool isPlaying = transport->isPlaying();

    // Only play audio when actually playing or recording (mute when stopped)
    if (isPlaying || isRecording)
    {
        trackManager->processBlock(outputBuffer, inputBuffer, isRecording);

        // Advance transport position AFTER processing
        transport->advance(numSamples);
    }
    else
    {
        // When stopped, just update meters but don't output audio
        // This prevents audio bleeding through when scrubbing
        juce::AudioBuffer<float> dummyOutput(2, numSamples);
        dummyOutput.clear();
        trackManager->processBlock(dummyOutput, inputBuffer, false);
    }
}

void AudioEngine::showAudioSettings()
{
    // TODO: Implement audio settings dialog
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        juce::Logger::writeToLog("Audio device changed");

        // Persist the new device selection so it survives restarts
        saveDeviceState();

        // Notify UI components about the device change
        deviceChangeBroadcaster.sendChangeMessage();
    }
}

int AudioEngine::getNumInputChannels() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device)
    {
        return device->getActiveInputChannels().countNumberOfSetBits();
    }
    return 0;
}

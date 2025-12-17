#include "AudioEngine.h"
#include "TrackManager.h"

AudioEngine::AudioEngine()
{
    // Create components
    trackManager = std::make_unique<TrackManager>();
    transport = std::make_unique<TransportController>();

    // Initialize audio device
    juce::Logger::writeToLog("AudioEngine: Initializing audio device...");

    auto error = deviceManager.initialiseWithDefaultDevices(2, 2);

    if (error.isNotEmpty())
    {
        juce::Logger::writeToLog("Audio initialization error: " + error);
    }
    else
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device)
        {
            juce::Logger::writeToLog("Audio device: " + device->getName());
            juce::Logger::writeToLog("Sample rate: " + juce::String(device->getCurrentSampleRate()));
            juce::Logger::writeToLog("Buffer size: " + juce::String(device->getCurrentBufferSizeSamples()));
            juce::Logger::writeToLog("Input channels: " + juce::String(device->getActiveInputChannels().countNumberOfSetBits()));
            juce::Logger::writeToLog("Output channels: " + juce::String(device->getActiveOutputChannels().countNumberOfSetBits()));

            // Register this as the audio callback
            deviceManager.addAudioCallback(this);
        }
        else
        {
            juce::Logger::writeToLog("WARNING: No audio device found!");
        }
    }
}

AudioEngine::~AudioEngine()
{
    shutdownAudio();
}

void AudioEngine::shutdownAudio()
{
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
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

        juce::Logger::writeToLog("Audio callback - samples: " + juce::String(numSamples) +
                                 ", state: " + stateStr +
                                 ", position: " + juce::String(transport->getPosition()));
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

    // Create JUCE audio buffers for easier manipulation
    juce::AudioBuffer<float> inputBuffer(const_cast<float**>(inputChannelData),
                                        numInputChannels, numSamples);

    juce::AudioBuffer<float> outputBuffer(outputChannelData,
                                         numOutputChannels, numSamples);

    // Process audio through track manager
    bool isRecording = transport->isRecording();
    trackManager->processBlock(outputBuffer, inputBuffer, isRecording);

    // Update transport position
    if (transport->isPlaying() || transport->isRecording())
    {
        transport->advance(numSamples);
        trackManager->setPlaybackPosition(static_cast<int>(transport->getPosition()));
    }
}

void AudioEngine::showAudioSettings()
{
    // TODO: Implement audio settings dialog
}

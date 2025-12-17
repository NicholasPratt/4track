#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "TransportController.h"
#include <memory>

class TrackManager;

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // AudioIODeviceCallback overrides
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                         int numInputChannels,
                                         float* const* outputChannelData,
                                         int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // Audio device management
    void showAudioSettings();
    void shutdownAudio();

    // Getters
    double getSampleRate() const { return currentSampleRate; }
    int getBufferSize() const { return currentBufferSize; }
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    // Access to components
    TrackManager* getTrackManager() { return trackManager.get(); }
    TransportController* getTransport() { return transport.get(); }

private:
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<TrackManager> trackManager;
    std::unique_ptr<TransportController> transport;

    double currentSampleRate = 44100.0;
    int currentBufferSize = 512;
    int debugCallbackCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

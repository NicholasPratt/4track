#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "TransportController.h"
#include <memory>

class TrackManager;

class AudioEngine : public juce::AudioIODeviceCallback,
                   public juce::ChangeListener
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

    // ChangeListener override
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // Audio device management
    void showAudioSettings();
    void shutdownAudio();
    int getNumInputChannels() const;

    // Getters
    double getSampleRate() const { return currentSampleRate; }
    int getBufferSize() const { return currentBufferSize; }
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    // Access to components
    TrackManager* getTrackManager() { return trackManager.get(); }
    TransportController* getTransport() { return transport.get(); }

    // Device change notifications
    juce::ChangeBroadcaster& getDeviceChangeBroadcaster() { return deviceChangeBroadcaster; }

    // Project folder management
    void setProjectFolder(const juce::File& folder) { currentProjectFolder = folder; }
    juce::File getProjectFolder() const { return currentProjectFolder; }

private:
    void initialiseAudioDevice();

    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<TrackManager> trackManager;
    std::unique_ptr<TransportController> transport;

    double currentSampleRate = 44100.0;
    int currentBufferSize = 512;
    int debugCallbackCounter = 0;
    bool audioDeviceInitialised = false;

    juce::ChangeBroadcaster deviceChangeBroadcaster;
    juce::File currentProjectFolder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

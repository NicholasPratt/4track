#pragma once

#include "Effects.h"
#include <juce_dsp/juce_dsp.h>

/**
    Overdrive, distortion, fuzz and tube drive, followed by a filter model, a Nave
    cabinet IR or a user IR. The clipping runs at 4x the sample rate so loud highs don't alias.
*/
class DriveEffect : public EffectPlugin
{
public:
    explicit DriveEffect(te::PluginCreationInfo);
    ~DriveEffect() override;

    static const char* getPluginName() { return "Drive"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getLatencySeconds() override { return latencySamples / sampleRate; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;
    std::vector<FileSlot> getFileSlots() override;
    void getFileReferences(std::vector<FileReference>&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    enum Model { Overdrive, Distortion, Fuzz, Tube };
    // Cabinet values are stored in projects and automation. Append new choices.
    enum Cab { NoCab, OpenCombo, TwinCombo, ClosedStack, BassCab, ImpulseResponse, FirstFactoryCab };
    static juce::StringArray modelNames() { return { "Overdrive", "Distortion", "Fuzz", "Tube" }; }
    static juce::StringArray cabNames();

    /** The shaping curve of each model, at drive gain g (for tests and the curious). */
    static float shape(Model, float x, float bias);

    /** Loads an impulse response WAV (copied into the project) and switches the cab to it. */
    bool loadImpulseResponse(const juce::File&, juce::String& error);
    juce::File getImpulseResponse() const { return getFileProperty(irProperty); }

    te::AutomatableParameter *model, *drive, *bias, *tone, *cab, *level, *mix;

private:
    void applyImpulseResponse();
    void updateCab(int cabType);

    static inline const juce::Identifier irProperty { "impulseResponse" };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 256 } };
    juce::dsp::ConvolutionMessageQueue factoryCabQueue;
    std::vector<std::unique_ptr<juce::dsp::Convolution>> factoryCabs;
    juce::AudioBuffer<float> dry, work, lows;   // allocated in initialise, never on the audio thread
    int latencySamples = 0, maxBlock = 512, dryPos = 0;
    EffectSvf preHigh[2], toneLow[2][2], dcBlock[2];
    static constexpr int kCabStages = 6;
    juce::dsp::IIR::Filter<float> cabFilters[2][kCabStages];
    int cabNow = -1;
    double preparedRate = 0;
    bool convolutionReady = false;
};

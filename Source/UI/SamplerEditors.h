#pragma once

#include "EffectEditor.h"
#include "../Audio/DrumSampler.h"
#include "../Audio/KeySampler.h"
#include <juce_audio_utils/juce_audio_utils.h>

class AudioEngine;
class Project;
class DrumSequencer;

/** The Drum Sampler's window: a 4x4 grid of pads above the chosen pad's controls. */
class DrumSamplerEditor : public te::Plugin::EditorComponent, private juce::Timer
{
public:
    DrumSamplerEditor(DrumSampler&, AudioEngine*, Project*);
    ~DrumSamplerEditor() override;

    bool allowWindowResizing() override { return false; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }
    void resized() override;
    void paint(juce::Graphics&) override;

    void select(int pad);
    void load(int pad, const juce::File&);
    void showPatterns(bool);

private:
    class PadButton;
    void showKitMenu();
    void timerCallback() override;
    void layout();

    DrumSampler& sampler;
    juce::OwnedArray<PadButton> padButtons;
    std::unique_ptr<EffectControls> padControls;
    EffectControls kitControls;
    juce::TextButton kitChooser { "Load Kit..." };
    juce::TextButton padsTab { "Pads" }, patternsTab { "Patterns" };
    juce::TextButton stopPreview { "Stop Preview" };
    std::unique_ptr<DrumSequencer> patterns;
    bool patternsVisible = false;
    juce::Label hint;
    int selected = 0;
    static constexpr int kPadHeight = 62, kGridGap = 8;
};

/** The Sampler's window: the sound's waveform, recording from an input, a keyboard to try it
    on, and its controls. */
class KeySamplerEditor : public te::Plugin::EditorComponent, public juce::FileDragAndDropTarget,
                         private juce::Timer, private juce::MidiKeyboardState::Listener
{
public:
    KeySamplerEditor(KeySampler&, AudioEngine*);
    ~KeySamplerEditor() override;

    bool allowWindowResizing() override { return false; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }
    void resized() override;
    void paint(juce::Graphics&) override;

    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }
    void filesDropped(const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    void handleNoteOn(juce::MidiKeyboardState*, int, int note, float) override { sampler.playKey(note, true); }
    void handleNoteOff(juce::MidiKeyboardState*, int, int note, float) override { sampler.playKey(note, false); }
    void toggleRecording();
    void finishRecording();
    void refreshInputs();
    juce::Rectangle<int> waveBounds() const;

    KeySampler& sampler;
    AudioEngine* engine;
    std::unique_ptr<InputRecorder> recorder;
    juce::ComboBox input;
    juce::TextButton record { "Record" }, detect { "Find Root" };
    juce::Label status;
    juce::MidiKeyboardState keyState;
    juce::MidiKeyboardComponent keyboard { keyState, juce::MidiKeyboardComponent::horizontalKeyboard };
    EffectControls controls;
    Sample::Ptr shown;
    std::vector<float> peaks;
    bool dragging = false;
    struct InputChoice { int channel, numChannels; };
    std::vector<InputChoice> inputs;
    static constexpr int kWaveHeight = 110, kToolbar = 30, kKeys = 56;
};

/** Gives the samplers their own windows (EffectEditor::install calls this). */
std::unique_ptr<te::Plugin::EditorComponent> createSamplerEditor(EffectPlugin&, AudioEngine*, Project*);

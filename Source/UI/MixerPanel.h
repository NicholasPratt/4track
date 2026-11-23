#pragma once

#include "Widgets.h"
#include "../Project/Project.h"

/** A small labelled rotary knob. */
class Knob : public juce::Component
{
public:
    Knob(const juce::String& label, juce::Colour colour);
    juce::Slider slider;
    void resized() override;
private:
    juce::Label caption;
};

class ChannelStrip : public juce::Component
{
public:
    ChannelStrip(AudioEngine&, Project&, int trackIndex);
    void refresh();
    void setMeter(float l, float r) { meter.setPeaks(l, r); }
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    Project& project;
    TrackParams& params;
    const int index;

    Knob lowCut { "LO CUT", Theme::mute }, highCut { "HI CUT", Theme::mute },
         midFreq { "FREQ", Theme::mute }, midGain { "GAIN", Theme::mute },
         send { "REVERB", Theme::solo }, pan { "PAN", Theme::accent };
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Slider fader;
    juce::Label faderValue;
    LevelMeter meter { true };
};

class MasterStrip : public juce::Component
{
public:
    MasterStrip(AudioEngine&, Project&);
    void refresh();
    void setMeter(float l, float r) { meter.setPeaks(l, r); }
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    Project& project;
    Knob room { "ROOM", Theme::solo }, damping { "DAMP", Theme::solo }, wet { "REVERB", Theme::solo };
    juce::Slider fader;
    juce::Label faderValue;
    LevelMeter meter { true, true };
};

class MixerPanel : public juce::Component
{
public:
    MixerPanel(AudioEngine&, Project&);
    void refresh();
    void setMeter(int track, float l, float r) { strips[(size_t) track]->setMeter(l, r); }
    void setMasterMeter(float l, float r) { master.setMeter(l, r); }
    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeight = 330;

private:
    std::array<std::unique_ptr<ChannelStrip>, kNumTracks> strips;
    MasterStrip master;
};

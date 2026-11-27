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

/** A track's plugin inserts: click an empty slot to add a plugin, click a plugin to
    open its editor, click its power button to bypass it (or turn it back on), right-click
    it to bypass or remove it. */
class InsertSlots : public juce::Component
{
public:
    InsertSlots(AudioEngine&, Project&, int trackIndex);
    void refresh();
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

    static constexpr int kSlotHeight = 16, kGap = 2;
    static int preferredHeight() { return AudioEngine::kMaxInserts * (kSlotHeight + kGap); }

private:
    struct Slot { te::Plugin* plugin; juce::String name; bool enabled; bool operator==(const Slot&) const = default; };
    juce::Rectangle<int> slotBounds(int i) const;
    /** The bypass button at the right of a slot with a plugin in it. */
    juce::Rectangle<int> powerBounds(int i) const;
    void toggleBypass(te::Plugin&);
    void showAddMenu();
    void showPluginMenu(te::Plugin&);

    AudioEngine& engine;
    Project& project;
    const int index;
    std::vector<Slot> slots;
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
         send1 { "B1 LVL", Theme::monitor }, send2 { "B2 LVL", Theme::solo }, pan { "PAN", Theme::accent };
    InsertSlots inserts;
    juce::TextButton mainRoute { "Main" }, bus1Route { "B1" }, bus2Route { "B2" };
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Slider fader;
    juce::Label faderValue;
    LevelMeter meter { true };
};

class OutputStrip : public juce::Component
{
public:
    OutputStrip(AudioEngine&, Project&, int mixerIndex);
    void refresh();
    void setMeter(float l, float r) { meter.setPeaks(l, r); }
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    OutputParams& params;
    const bool bus1;
    const juce::String name;
    const juce::Colour colour;
    Knob room { "ROOM", Theme::monitor }, damping { "DAMP", Theme::monitor }, wet { "WET", Theme::monitor };
    juce::TextButton reverb { "Reverb" }, toMain { "Main" }, toBus2 { "Bus 2" };
    InsertSlots inserts;
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
    void setBusMeter(int bus, float l, float r) { (bus == 0 ? bus1 : bus2).setMeter(l, r); }
    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeight = 330 + 76;   // knobs, inserts, fader

private:
    std::array<std::unique_ptr<ChannelStrip>, kNumTracks> strips;
    OutputStrip bus1, bus2, master;
};

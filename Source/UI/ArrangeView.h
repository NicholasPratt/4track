#pragma once

#include "Ruler.h"
#include "TrackLanes.h"
#include "TrackHeader.h"

/** Ruler on top, track headers on the left, clip lanes in the middle, scrollbars. */
class ArrangeView : public juce::Component,
                    private juce::ScrollBar::Listener
{
public:
    ArrangeView(AudioEngine&, Project&);
    ~ArrangeView() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    /** Call regularly (UI timer): redraws the moving parts and follows the playhead. */
    void tick();
    void viewChanged();
    void projectChanged();

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void zoom(double factor);
    void zoomToFit();
    void setInputs(const std::vector<AudioEngine::Input>& inputs);
    void setMeter(int track, float l, float r) { headers[(size_t) track]->setMeter(l, r); }

    int getSelectedTrack() const { return selectedTrack; }
    TrackLanes& getLanes() { return lanes; }
    TimelineView& getView() { return view; }

    static constexpr int kHeaderWidth = 236;
    static constexpr int kRulerHeight = 34;

private:
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
    void selectTrack(int t);
    void layoutHeaders();

    AudioEngine& engine;
    Project& project;
    TimelineView view;

    Ruler ruler;
    TrackLanes lanes;
    juce::Component headerColumn;
    std::array<std::unique_ptr<TrackHeader>, kNumTracks> headers;
    juce::ScrollBar hScroll { false }, vScroll { true };
    int selectedTrack = 0;
};

#pragma once

#include "Ruler.h"
#include "TrackLanes.h"
#include "TrackHeader.h"

class LyricEditorWindow;

/** Ruler on top, track headers on the left, clip lanes in the middle, scrollbars. */
class ArrangeView : public juce::Component,
                    public juce::DragAndDropContainer,
                    private juce::ScrollBar::Listener
{
public:
    std::function<void()> onShowMidiSettings;   // a track asked for the MIDI Settings window
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
    Ruler& getRuler() { return ruler; }
    TimelineView& getView() { return view; }

    static constexpr int kHeaderWidth = 236;


private:
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
    void selectTrack(int t);
    void layoutHeaders();
    void showLyricEditor();
    juce::int64 contentEndSample() const;

    AudioEngine& engine;
    Project& project;
    TimelineView view;

    Ruler ruler;
    juce::TextButton songwriting { "Show songwriting" }, chordPreview { "Preview off" }, lyricEditor { "Lyric Editor..." };
    std::unique_ptr<LyricEditorWindow> lyricWindow;
    TrackLanes lanes;
    juce::Component headerColumn;
    std::array<std::unique_ptr<TrackHeader>, kNumTracks> headers;
    juce::ScrollBar hScroll { false }, vScroll { true }, lyricScroll { true };
    int selectedTrack = 0;
};

#include "ArrangeView.h"
#include "LyricEditorWindow.h"

ArrangeView::ArrangeView(AudioEngine& e, Project& p)
    : engine(e), project(p), ruler(e, p, view), lanes(e, p, view)
{
    addAndMakeVisible(ruler);
    addAndMakeVisible(songwriting);
    songwriting.setTooltip("Show or hide chords and lyrics");
    songwriting.onClick = [this]
    {
        ruler.setSongwritingExpanded(!ruler.isSongwritingExpanded());
        songwriting.setToggleState(ruler.isSongwritingExpanded(), juce::dontSendNotification);
        songwriting.setButtonText(ruler.isSongwritingExpanded() ? "Hide songwriting" : "Show songwriting");
        chordPreview.setVisible(ruler.isSongwritingExpanded());
        lyricEditor.setVisible(ruler.isSongwritingExpanded());
        viewChanged();
    };
    addChildComponent(chordPreview);
    chordPreview.setTooltip("Play a short chord when you click its symbol");
    chordPreview.setClickingTogglesState(true);
    chordPreview.onClick = [this]
    {
        const bool enabled = chordPreview.getToggleState();
        engine.setChordPreviewEnabled(enabled);
        chordPreview.setButtonText(enabled ? "Preview on" : "Preview off");
    };
    lyricEditor.setTooltip("Write lyrics and drag them onto the timeline");
    lyricEditor.onClick = [this] { showLyricEditor(); };
    addChildComponent(lyricEditor);
    addAndMakeVisible(lanes);
    addAndMakeVisible(headerColumn);
    headerColumn.setInterceptsMouseClicks(false, true);

    for (int t = 0; t < kNumTracks; ++t)
    {
        headers[(size_t) t] = std::make_unique<TrackHeader>(e, p, t);
        headers[(size_t) t]->onChanged = [this] { lanes.repaint(); };
        headers[(size_t) t]->onClicked = [this, t] { selectTrack(t); };
        headers[(size_t) t]->onShowMidiSettings = [this] { if (onShowMidiSettings) onShowMidiSettings(); };
        headerColumn.addAndMakeVisible(*headers[(size_t) t]);
    }

    ruler.onViewChanged = [this] { viewChanged(); };
    ruler.onPunchChanged = [this] { lanes.repaint(); };
    lanes.onViewChanged = [this] { viewChanged(); };
    lanes.onTrackClicked = [this](int t) { selectTrack(t); };

    for (auto* sb : { &hScroll, &vScroll })
    {
        sb->addListener(this);
        sb->setAutoHide(false);
        addAndMakeVisible(sb);
    }
    lyricScroll.addListener(this);
    lyricScroll.setSingleStepSize(1);
    lyricScroll.setWantsKeyboardFocus(false);
    addChildComponent(lyricScroll);
    selectTrack(0);
}

ArrangeView::~ArrangeView()
{
    hScroll.removeListener(this);
    vScroll.removeListener(this);
    lyricScroll.removeListener(this);
}

void ArrangeView::showLyricEditor()
{
    if (lyricWindow == nullptr)
    {
        const auto anchor = lyricEditor.getScreenBounds().withBottom(ruler.getScreenBounds().getBottom());
        lyricWindow = std::make_unique<LyricEditorWindow>(project, anchor);
    }
    lyricWindow->refreshProject();
    lyricWindow->setVisible(true);
    lyricWindow->toFront(true);
}

void ArrangeView::selectTrack(int t)
{
    selectedTrack = t;
    for (int i = 0; i < kNumTracks; ++i)
        headers[(size_t) i]->setSelected(i == t);
}

void ArrangeView::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
    auto corner = juce::Rectangle<int>(0, 0, kHeaderWidth, ruler.timelineHeight());
    g.setColour(Theme::panel);
    g.fillRect(corner);
    g.setColour(Theme::textDim);
    g.setFont(Theme::font(11.0f, true));
    g.drawText("TRACKS", corner.withHeight(34).reduced(10, 0), juce::Justification::centredLeft, false);
    if (ruler.isSongwritingExpanded())
    {
        g.drawText("Chords", 10, Ruler::baseHeight, 90, Ruler::chordHeight, juce::Justification::centredLeft);
        g.drawText("Lyrics", 10, Ruler::baseHeight + Ruler::chordHeight, 90, Ruler::lyricRowHeight,
                   juce::Justification::centredLeft);
    }
    g.setColour(Theme::outline);
    g.drawHorizontalLine(ruler.timelineHeight() - 1, 0.0f, (float) kHeaderWidth);
}

void ArrangeView::resized()
{
    viewChanged();
}

void ArrangeView::viewChanged()
{
    view.viewStart = juce::jmax(0.0, view.viewStart);
    auto r = getLocalBounds();
    auto bottom = r.removeFromBottom(12);
    const int lyricHeight = r.getHeight() - Ruler::baseHeight - Ruler::chordHeight - 144;
    ruler.layoutLyrics(r.getWidth() - kHeaderWidth - 12, juce::jmax(1, lyricHeight / Ruler::lyricRowHeight));
    const int visibleRows = ruler.getVisibleLyricRows();
    lyricScroll.setVisible(ruler.isSongwritingExpanded() && ruler.getLyricRowCount() > visibleRows);
    lyricScroll.setBounds(kHeaderWidth - 16, Ruler::baseHeight + Ruler::chordHeight + 2, 10,
                          visibleRows * Ruler::lyricRowHeight - 4);
    lyricScroll.setRangeLimits(0, ruler.getLyricRowCount(), juce::dontSendNotification);
    lyricScroll.setCurrentRange(ruler.getFirstLyricRow(), visibleRows, juce::dontSendNotification);
    vScroll.setBounds(r.removeFromRight(12).withTrimmedTop(ruler.timelineHeight()));
    bottom.removeFromLeft(kHeaderWidth);
    bottom.removeFromRight(12);
    hScroll.setBounds(bottom);

    auto top = r.removeFromTop(ruler.timelineHeight());
    top.removeFromLeft(kHeaderWidth);
    ruler.setBounds(top);
    songwriting.setBounds(10, 34, kHeaderWidth - 20, Ruler::baseHeight - 34);
    chordPreview.setBounds(kHeaderWidth - 119, Ruler::baseHeight + 3, 95, Ruler::chordHeight - 6);
    lyricEditor.setBounds(kHeaderWidth - 119, Ruler::baseHeight + Ruler::chordHeight + 2, 95, Ruler::lyricRowHeight - 4);

    headerColumn.setBounds(r.removeFromLeft(kHeaderWidth));
    lanes.setBounds(r);

    const int maxScrollY = juce::jmax(0, lanes.getContentHeight() - lanes.getHeight());
    lanes.scrollY = juce::jlimit(0, maxScrollY, lanes.scrollY);
    const double sr = engine.getSampleRate();
    const double visible = view.samplesPerPixel * juce::jmax(1, lanes.getWidth());
    const double contentEnd = juce::jmax((double) contentEndSample() + sr * 30.0,
                                         view.viewStart + visible, sr * 120.0);
    hScroll.setRangeLimits(0.0, contentEnd, juce::dontSendNotification);
    hScroll.setCurrentRange(view.viewStart, visible, juce::dontSendNotification);
    vScroll.setRangeLimits(0.0, (double) lanes.getContentHeight(), juce::dontSendNotification);
    vScroll.setCurrentRange((double) lanes.scrollY, (double) lanes.getHeight(), juce::dontSendNotification);

    layoutHeaders();
    ruler.repaint();
    lanes.repaint();
    repaint(0, 0, kHeaderWidth, ruler.timelineHeight());
}

void ArrangeView::layoutHeaders()
{
    for (int t = 0; t < kNumTracks; ++t)
        headers[(size_t) t]->setBounds(0, t * TrackLanes::kTrackHeight - lanes.scrollY,
                                       kHeaderWidth, TrackLanes::kTrackHeight);
}

void ArrangeView::scrollBarMoved(juce::ScrollBar* sb, double newRangeStart)
{
    if (sb == &lyricScroll)
    {
        ruler.setFirstLyricRow(juce::roundToInt(newRangeStart));
        return;
    }
    if (sb == &hScroll)
        view.viewStart = newRangeStart;
    else
        lanes.scrollY = (int) newRangeStart;
    viewChanged();
}

void ArrangeView::projectChanged()
{
    if (lyricWindow != nullptr)
        lyricWindow->refreshProject();
    lanes.pruneSelection();
    for (auto& h : headers)
        h->refresh();
    viewChanged();
}

void ArrangeView::zoom(double factor)
{
    // Zoom around the playhead if it's on screen, otherwise around the centre.
    float anchor = view.xFor(engine.getPosition());
    if (anchor < 0.0f || anchor > (float) lanes.getWidth())
        anchor = (float) lanes.getWidth() * 0.5f;
    view.zoomAround(anchor, factor);
    viewChanged();
}

juce::int64 ArrangeView::contentEndSample() const
{
    auto end = juce::jmax(project.getArrangement().getEnd(), project.songNotesEnd());
    if (auto* sections = project.getEdit().getArrangerTrack())
        for (auto* clip : sections->getClips())
            end = juce::jmax(end, engine.toSamples(clip->getPosition().getEnd()));
    return end;
}

void ArrangeView::zoomToFit()
{
    const auto end = contentEndSample();
    const double length = end > 0 ? (double) end * 1.05 : engine.getSampleRate() * 60.0;
    view.samplesPerPixel = juce::jlimit(8.0, 20000.0, length / juce::jmax(100, lanes.getWidth()));
    view.viewStart = 0.0;
    viewChanged();
}

void ArrangeView::setInputs(const std::vector<AudioEngine::Input>& inputs)
{
    for (auto& h : headers)
        h->setInputs(inputs);
}

void ArrangeView::tick()
{
    for (auto& h : headers)
        h->refresh();

    if (engine.getState() != AudioEngine::State::Stopped)
    {
        // Page the view along with the playhead.
        const float x = view.xFor(engine.getPosition());
        const float w = (float) lanes.getWidth();
        if (x > w - 30.0f || x < 0.0f)
        {
            view.viewStart = juce::jmax(0.0, (double) engine.getPosition() - (double) w * 0.1 * view.samplesPerPixel);
            viewChanged();
            return;
        }
    }

    ruler.repaint();
    lanes.repaint();
}

void ArrangeView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    // Wheel over the track headers scrolls the tracks vertically.
    lanes.scrollY -= (int) std::round(wheel.deltaY * 300.0);
    viewChanged();
}

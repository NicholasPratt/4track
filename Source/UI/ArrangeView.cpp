#include "ArrangeView.h"

ArrangeView::ArrangeView(AudioEngine& e, Project& p)
    : engine(e), project(p), ruler(e, p, view), lanes(e, p, view)
{
    addAndMakeVisible(ruler);
    addAndMakeVisible(lanes);
    addAndMakeVisible(headerColumn);
    headerColumn.setInterceptsMouseClicks(false, true);

    for (int t = 0; t < kNumTracks; ++t)
    {
        headers[(size_t) t] = std::make_unique<TrackHeader>(e, p, t);
        headers[(size_t) t]->onChanged = [this] { lanes.repaint(); };
        headers[(size_t) t]->onClicked = [this, t] { selectTrack(t); };
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
    selectTrack(0);
}

ArrangeView::~ArrangeView()
{
    hScroll.removeListener(this);
    vScroll.removeListener(this);
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
    auto corner = juce::Rectangle<int>(0, 0, kHeaderWidth, kRulerHeight);
    g.setColour(Theme::panel);
    g.fillRect(corner);
    g.setColour(Theme::textDim);
    g.setFont(Theme::font(11.0f, true));
    g.drawText("TRACKS", corner.reduced(10, 0), juce::Justification::centredLeft, false);
    g.setColour(Theme::outline);
    g.drawHorizontalLine(kRulerHeight - 1, 0.0f, (float) kHeaderWidth);
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    auto bottom = r.removeFromBottom(12);
    vScroll.setBounds(r.removeFromRight(12).withTrimmedTop(kRulerHeight));
    bottom.removeFromLeft(kHeaderWidth);
    bottom.removeFromRight(12);
    hScroll.setBounds(bottom);

    auto top = r.removeFromTop(kRulerHeight);
    top.removeFromLeft(kHeaderWidth);
    ruler.setBounds(top);

    headerColumn.setBounds(r.removeFromLeft(kHeaderWidth));
    lanes.setBounds(r);
    viewChanged();
}

void ArrangeView::layoutHeaders()
{
    for (int t = 0; t < kNumTracks; ++t)
        headers[(size_t) t]->setBounds(0, t * TrackLanes::kTrackHeight - lanes.scrollY,
                                       kHeaderWidth, TrackLanes::kTrackHeight);
}

void ArrangeView::viewChanged()
{
    const int maxScrollY = juce::jmax(0, lanes.getContentHeight() - lanes.getHeight());
    lanes.scrollY = juce::jlimit(0, maxScrollY, lanes.scrollY);
    view.viewStart = juce::jmax(0.0, view.viewStart);

    const double sr = engine.getSampleRate();
    const double visible = view.samplesPerPixel * juce::jmax(1, lanes.getWidth());
    const double contentEnd = juce::jmax((double) project.getArrangement().getEnd() + sr * 30.0,
                                         view.viewStart + visible, sr * 120.0);
    hScroll.setRangeLimits(0.0, contentEnd, juce::dontSendNotification);
    hScroll.setCurrentRange(view.viewStart, visible, juce::dontSendNotification);
    vScroll.setRangeLimits(0.0, (double) lanes.getContentHeight(), juce::dontSendNotification);
    vScroll.setCurrentRange((double) lanes.scrollY, (double) lanes.getHeight(), juce::dontSendNotification);

    layoutHeaders();
    ruler.repaint();
    lanes.repaint();
}

void ArrangeView::scrollBarMoved(juce::ScrollBar* sb, double newRangeStart)
{
    if (sb == &hScroll)
        view.viewStart = newRangeStart;
    else
        lanes.scrollY = (int) newRangeStart;
    viewChanged();
}

void ArrangeView::projectChanged()
{
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

void ArrangeView::zoomToFit()
{
    const auto end = project.getArrangement().getEnd();
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

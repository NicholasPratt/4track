#include "Arrangement.h"
#include <algorithm>
#include <cmath>

const Clip* Arrangement::findClip(juce::int64 id, int* trackOut) const
{
    for (int t = 0; t < kNumTracks; ++t)
        for (auto& c : tracks[(size_t) t])
            if (c.id == id)
            {
                if (trackOut) *trackOut = t;
                return &c;
            }
    return nullptr;
}

Clip* Arrangement::findMutable(juce::int64 id, int* trackOut)
{
    return const_cast<Clip*>(findClip(id, trackOut));
}

juce::int64 Arrangement::getEnd() const
{
    juce::int64 end = 0;
    for (auto& lane : tracks)
        for (auto& c : lane)
            end = std::max(end, c.end());
    return end;
}

bool Arrangement::isEmpty() const
{
    for (auto& lane : tracks)
        if (!lane.empty())
            return false;
    return true;
}

void Arrangement::sortTrack(int track)
{
    auto& lane = tracks[(size_t) track];
    std::sort(lane.begin(), lane.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

void Arrangement::clampFades(Clip& c)
{
    c.fadeIn  = juce::jlimit<juce::int64>(0, c.length, c.fadeIn);
    c.fadeOut = juce::jlimit<juce::int64>(0, c.length, c.fadeOut);
    const auto total = c.fadeIn + c.fadeOut;
    if (total > c.length && total > 0)
    {
        const double scale = (double) c.length / (double) total;
        c.fadeIn  = (juce::int64) std::floor((double) c.fadeIn * scale);
        c.fadeOut = (juce::int64) std::floor((double) c.fadeOut * scale);
    }
}

void Arrangement::clearRange(int track, juce::int64 a, juce::int64 b)
{
    if (track < 0 || track >= kNumTracks || b <= a)
        return;

    auto& lane = tracks[(size_t) track];
    std::vector<Clip> out;
    out.reserve(lane.size() + 1);

    for (const auto& c : lane)
    {
        if (c.end() <= a || c.start >= b)
        {
            out.push_back(c);
            continue;
        }

        const bool keepLeft = c.start < a && (a - c.start) >= kMinClipLength;
        if (keepLeft)
        {
            Clip left = c;
            left.length = a - c.start;
            left.fadeOut = c.midi ? 0 : kEdgeFade;
            clampFades(left);
            out.push_back(left);
        }

        if (c.end() > b && (c.end() - b) >= kMinClipLength)
        {
            Clip right = c;
            const auto cut = b - c.start;
            right.start = b;
            right.sourceOffset += cut;
            right.length = c.end() - b;
            right.fadeIn = c.midi ? 0 : kEdgeFade;
            right.id = keepLeft ? nextClipId++ : c.id;
            clampFades(right);
            out.push_back(right);
        }
    }

    lane = std::move(out);
}

juce::int64 Arrangement::insertClip(int track, Clip clip)
{
    if (track < 0 || track >= kNumTracks || clip.length <= 0 || clip.sourceLength <= 0)
        return 0;

    if (clip.id == 0)
        clip.id = nextClipId++;
    else
        nextClipId = std::max(nextClipId, clip.id + 1);

    clampFades(clip);
    clearRange(track, clip.start, clip.end());
    tracks[(size_t) track].push_back(clip);
    sortTrack(track);
    return clip.id;
}

bool Arrangement::removeClip(juce::int64 id)
{
    for (auto& lane : tracks)
    {
        auto it = std::find_if(lane.begin(), lane.end(), [id](const Clip& c) { return c.id == id; });
        if (it != lane.end())
        {
            lane.erase(it);
            return true;
        }
    }
    return false;
}

bool Arrangement::moveClip(juce::int64 id, int newTrack, juce::int64 newStart)
{
    int oldTrack = -1;
    auto* c = findClip(id, &oldTrack);
    if (c == nullptr || newTrack < 0 || newTrack >= kNumTracks)
        return false;

    newStart = std::max<juce::int64>(0, newStart);
    if (newStart == c->start && newTrack == oldTrack)
        return false;

    Clip moved = *c;
    removeClip(id);
    moved.start = newStart;
    insertClip(newTrack, moved);
    return true;
}

bool Arrangement::trimStart(juce::int64 id, juce::int64 newStart)
{
    int t = -1;
    auto* c = findMutable(id, &t);
    if (c == nullptr)
        return false;

    juce::int64 lowest = c->start - c->sourceOffset;   // can't reveal audio before the source starts
    for (auto& other : tracks[(size_t) t])
        if (other.id != id && other.end() <= c->start)
            lowest = std::max(lowest, other.end());     // or overlap the previous clip
    lowest = std::max<juce::int64>(lowest, 0);

    newStart = juce::jlimit(lowest, c->end() - kMinClipLength, newStart);
    const auto delta = newStart - c->start;
    if (delta == 0)
        return false;

    c->start += delta;
    c->sourceOffset += delta;
    c->length -= delta;
    clampFades(*c);
    return true;
}

bool Arrangement::trimEnd(juce::int64 id, juce::int64 newEnd)
{
    int t = -1;
    auto* c = findMutable(id, &t);
    if (c == nullptr)
        return false;

    juce::int64 highest = c->start + (c->sourceLength - c->sourceOffset);
    for (auto& other : tracks[(size_t) t])
        if (other.id != id && other.start >= c->end())
            highest = std::min(highest, other.start);

    newEnd = juce::jlimit(c->start + kMinClipLength, highest, newEnd);
    if (newEnd == c->end())
        return false;

    c->length = newEnd - c->start;
    clampFades(*c);
    return true;
}

bool Arrangement::splitClip(juce::int64 id, juce::int64 position)
{
    int t = -1;
    auto* c = findMutable(id, &t);
    if (c == nullptr)
        return false;
    if (position - c->start < kMinClipLength || c->end() - position < kMinClipLength)
        return false;

    Clip right = *c;
    const auto cut = position - c->start;
    right.id = nextClipId++;
    right.start = position;
    right.sourceOffset += cut;
    right.length = c->length - cut;
    right.fadeIn = 0;   // the audio is continuous across a split, so no fades there

    c->length = cut;
    c->fadeOut = 0;
    clampFades(*c);
    clampFades(right);

    tracks[(size_t) t].push_back(right);
    sortTrack(t);
    return true;
}

bool Arrangement::setFades(juce::int64 id, juce::int64 fadeIn, juce::int64 fadeOut)
{
    auto* c = findMutable(id);
    if (c == nullptr || c->midi)
        return false;

    fadeIn  = juce::jlimit<juce::int64>(0, c->length, fadeIn);
    fadeOut = juce::jlimit<juce::int64>(0, c->length - fadeIn, fadeOut);
    if (fadeIn == c->fadeIn && fadeOut == c->fadeOut)
        return false;

    c->fadeIn = fadeIn;
    c->fadeOut = fadeOut;
    return true;
}

bool Arrangement::setGain(juce::int64 id, float gain)
{
    auto* c = findMutable(id);
    if (c == nullptr || c->midi || juce::exactlyEqual(c->gain, gain))
        return false;
    c->gain = gain;
    return true;
}

void Arrangement::rescalePositions(double ratio)
{
    auto scale = [ratio](juce::int64 v) { return (juce::int64) std::llround((double) v * ratio); };

    for (auto& lane : tracks)
    {
        for (auto& c : lane)
        {
            c.start = scale(c.start);
            c.sourceOffset = scale(c.sourceOffset);
            c.length = std::max<juce::int64>(1, scale(c.length));
            c.fadeIn = scale(c.fadeIn);
            c.fadeOut = scale(c.fadeOut);
            if (!c.midi)
                c.sourceLength = scale(c.sourceLength);
            c.length = std::min(c.length, c.sourceLength - c.sourceOffset);
            clampFades(c);
        }
    }
}

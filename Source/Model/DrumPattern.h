#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>

/**
    A 16-pad step pattern of 16 to 64 steps, at 8, 16 or 32 steps a bar. Each hit has a type (Prysm's): a plain hit, an accent, a flam (a
    softer grace note just ahead of the hit) or a ghost (a quiet one). A hit can also be a
    roll: 2, 3, 4, 6 or 8 hits inside the step, even, fading away or swelling up.

    `rows` says which steps have a hit; `detail` the hit's type and roll, and is zero (a
    plain hit, no roll) for steps without one, so patterns compare equal when they play the
    same. forEachNote() turns a step into the notes it plays: placing the pattern on a track
    and previewing it both use it.
*/
struct DrumPattern
{
    static constexpr int kSlots = 8, kPads = 16, kMinSteps = 16, kMaxSteps = 64;
    static constexpr std::array<int, 3> kStepsPerBarChoices { 8, 16, 32 };   // eighths, sixteenths, thirty-seconds
    static constexpr int kDefaultStepsPerBar = 16;
    static constexpr int kVelocity = 100, kAccentVelocity = 127, kGhostVelocity = 48;
    static constexpr double kFlamBeats = 0.03;          // the grace note comes this far ahead of the hit (15 ms at 120 BPM)
    static constexpr float kFlamGrace = 0.6f;           // and this loud, relative to it
    static constexpr float kRampFloor = 0.3f;           // a fade ends, and a swell starts, at this much of the hit

    enum class Hit : std::uint8_t { Hit, Accent, Flam, Ghost };
    enum class Ramp : std::uint8_t { Even, Fade, Swell };
    static constexpr std::array<int, 6> kRollHits { 1, 2, 3, 4, 6, 8 };   // index 0: no roll

    /** One step of one pad, unpacked. */
    struct Step
    {
        bool on = false;
        Hit hit = Hit::Hit;
        int roll = 0;            // index into kRollHits
        Ramp ramp = Ramp::Even;
        int hits() const { return kRollHits[static_cast<std::size_t>(std::clamp(roll, 0, 5))]; }
        bool operator==(const Step&) const = default;
    };

    int steps = kMinSteps;
    int stepsPerBar = kDefaultStepsPerBar;   // a bar is 4 beats

    double stepBeats() const { return 4.0 / stepsPerBar; }
    double lengthBeats() const { return steps * stepBeats(); }
    static bool isStepsPerBar(int n) { return std::find(kStepsPerBarChoices.begin(), kStepsPerBarChoices.end(), n) != kStepsPerBarChoices.end(); }
    using Row = std::bitset<kMaxSteps>;
    std::array<Row, kPads> rows {};
    using Detail = std::array<std::uint8_t, kMaxSteps>;
    std::array<Detail, kPads> detail {};   // per step: bits 0-1 the Hit, 2-4 the roll, 5-6 the Ramp

    bool hasHit(int pad, int step) const { return rows[static_cast<std::size_t>(pad)][static_cast<std::size_t>(step)]; }
    bool hasHits() const
    {
        const auto mask = Row().set() >> static_cast<std::size_t>(kMaxSteps - steps);
        return std::any_of(rows.begin(), rows.end(), [&mask](const Row& row) { return (row & mask).any(); });
    }

    Step getStep(int pad, int step) const
    {
        Step s;
        s.on = hasHit(pad, step);
        if (s.on)
            s = unpack(detail[static_cast<std::size_t>(pad)][static_cast<std::size_t>(step)], true);
        return s;
    }

    void setStep(int pad, int step, Step s)
    {
        rows[static_cast<std::size_t>(pad)].set(static_cast<std::size_t>(step), s.on);
        detail[static_cast<std::size_t>(pad)][static_cast<std::size_t>(step)] = s.on ? pack(s) : std::uint8_t(0);
    }

    void clear() { rows = {}; detail = {}; }

    /** Clicking a step steps it through Prysm's order: none, hit, accent, flam, ghost, none.
        Its roll stays as it was. */
    static Step nextHit(Step s)
    {
        if (!s.on)
            return { true, Hit::Hit, s.roll, s.ramp };
        if (s.hit == Hit::Ghost)
            return {};
        s.hit = static_cast<Hit>(static_cast<int>(s.hit) + 1);
        return s;
    }

    static std::uint8_t pack(Step s)
    {
        return static_cast<std::uint8_t>((static_cast<int>(s.hit) & 3) | ((std::clamp(s.roll, 0, 5) & 7) << 2)
                                         | ((static_cast<int>(s.ramp) & 3) << 5));
    }

    static Step unpack(std::uint8_t code, bool on)
    {
        Step s;
        s.on = on;
        s.hit = static_cast<Hit>(code & 3);
        s.roll = std::clamp((code >> 2) & 7, 0, 5);
        s.ramp = static_cast<Ramp>(std::min((code >> 5) & 3, 2));
        return s;
    }

    static int velocityOf(Hit h)
    {
        return h == Hit::Accent ? kAccentVelocity : h == Hit::Ghost ? kGhostVelocity : kVelocity;
    }

    /** Calls f(offsetSteps, lengthSteps, velocity) for each note one pad's step plays, in
        order. Offsets are from the step's start, 0 up to (not including) 1. No allocation:
        the audio thread calls it. */
    template <typename F>
    void forEachNote(int pad, int step, F&& f) const
    {
        const auto s = getStep(pad, step);
        if (!s.on)
            return;
        const int base = velocityOf(s.hit);
        double from = 0.0;
        if (s.hit == Hit::Flam)   // the same gap in time, whatever the step size
        {
            const double grace = kFlamBeats / stepBeats();
            f(0.0, grace, std::max(1, static_cast<int>(static_cast<float>(base) * kFlamGrace + 0.5f)));
            from = grace;
        }
        const int n = s.hits();
        const double spacing = (1.0 - from) / n;
        for (int i = 0; i < n; ++i)
        {
            float scale = 1.0f;
            if (n > 1 && s.ramp != Ramp::Even)
            {
                const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                scale = s.ramp == Ramp::Fade ? 1.0f - (1.0f - kRampFloor) * t : kRampFloor + (1.0f - kRampFloor) * t;
            }
            const int velocity = std::clamp(static_cast<int>(static_cast<float>(base) * scale + 0.5f), 1, 127);
            f(from + i * spacing, std::min(0.5, spacing * 0.5), velocity);
        }
    }

    bool operator==(const DrumPattern&) const = default;
};

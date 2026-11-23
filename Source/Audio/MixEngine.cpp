#include "MixEngine.h"

namespace
{
    float peakOf(const float* data, int n)
    {
        auto range = juce::FloatVectorOperations::findMinAndMax(data, n);
        return juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
    }

    void applySmoothedGain(juce::LinearSmoothedValue<float>& g, float* data, int n)
    {
        if (!g.isSmoothing())
        {
            juce::FloatVectorOperations::multiply(data, g.getTargetValue(), n);
            return;
        }
        for (int i = 0; i < n; ++i)
            data[i] *= g.getNextValue();
    }
}

void MixEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax(64, maxBlockSize);
    trackBuffer.setSize(2, maxBlock);
    sendBuffer.setSize(2, maxBlock);
    reverb.setSampleRate(sampleRate);
    reset();
}

void MixEngine::reset()
{
    for (auto& d : dsp)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            d.lowCut[ch].reset();
            d.highCut[ch].reset();
            d.mid[ch].reset();
        }
        d.appliedLc = d.appliedHc = d.appliedMf = -1.0f;
        d.appliedMg = -999.0f;
        d.lcOn = d.hcOn = d.midOn = false;
        d.gainL.reset(sampleRate, 0.02);
        d.gainR.reset(sampleRate, 0.02);
        d.gainL.setCurrentAndTargetValue(-1.0f);   // -1 marks "snap on first block"
        d.gainR.setCurrentAndTargetValue(-1.0f);
    }
    reverb.reset();
    appliedRoom = appliedDamp = appliedWet = -1.0f;
    masterGain.reset(sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue(-1.0f);
}

void MixEngine::process(const Arrangement* arrangement, const ParamArray& params, MasterParams& master,
                        juce::int64 position, int numSamples, bool playClips,
                        const TrackFeed* feeds, float* outL, float* outR, bool updateMeters)
{
    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin(maxBlock, numSamples - done);
        processChunk(arrangement, params, master, position + done, n, playClips, feeds, done,
                     outL + done, outR + done, updateMeters);
        done += n;
    }
}

void MixEngine::updateFilters(TrackDsp& d, const TrackParams& p)
{
    const float nyquistSafe = (float) sampleRate * 0.45f;

    const float lc = p.lowCut.load(std::memory_order_relaxed);
    const bool lcOn = lc > 20.5f;
    if (lcOn && (!d.lcOn || ! juce::exactlyEqual(lc, d.appliedLc)))
    {
        auto coeffs = juce::IIRCoefficients::makeHighPass(sampleRate, juce::jmin(lc, nyquistSafe));
        for (auto& f : d.lowCut) { f.setCoefficients(coeffs); if (!d.lcOn) f.reset(); }
        d.appliedLc = lc;
    }
    d.lcOn = lcOn;

    const float hc = p.highCut.load(std::memory_order_relaxed);
    const bool hcOn = hc < 19990.0f && hc < nyquistSafe;
    if (hcOn && (!d.hcOn || ! juce::exactlyEqual(hc, d.appliedHc)))
    {
        auto coeffs = juce::IIRCoefficients::makeLowPass(sampleRate, hc);
        for (auto& f : d.highCut) { f.setCoefficients(coeffs); if (!d.hcOn) f.reset(); }
        d.appliedHc = hc;
    }
    d.hcOn = hcOn;

    const float mf = p.midFreq.load(std::memory_order_relaxed);
    const float mg = p.midGainDb.load(std::memory_order_relaxed);
    const bool midOn = std::abs(mg) > 0.1f;
    if (midOn && (!d.midOn || ! juce::exactlyEqual(mf, d.appliedMf) || ! juce::exactlyEqual(mg, d.appliedMg)))
    {
        auto coeffs = juce::IIRCoefficients::makePeakFilter(sampleRate, juce::jmin(mf, nyquistSafe), 1.0,
                                                            juce::Decibels::decibelsToGain(mg));
        for (auto& f : d.mid) { f.setCoefficients(coeffs); if (!d.midOn) f.reset(); }
        d.appliedMf = mf;
        d.appliedMg = mg;
    }
    d.midOn = midOn;
}

void MixEngine::renderClips(const std::vector<Clip>& lane, juce::int64 position, int n, float* l, float* r)
{
    const auto blockEnd = position + n;

    for (const auto& c : lane)
    {
        if (c.start >= blockEnd)
            break;   // lanes are sorted by start
        if (c.end() <= position || c.source == nullptr)
            continue;

        const auto s0 = juce::jmax(position, c.start);
        const auto s1 = juce::jmin(blockEnd, c.end());
        const auto rel0 = s0 - c.start;
        const auto src0 = c.sourceOffset + rel0;
        const auto srcLen = c.source->getLength();
        if (src0 < 0 || src0 >= srcLen)
            continue;

        const int count = (int) juce::jmin(s1 - s0, srcLen - src0);
        const int outOffset = (int) (s0 - position);
        const float* srcL = c.source->audio.getReadPointer(0, (int) src0);
        const float* srcR = c.source->audio.getReadPointer(1, (int) src0);

        const bool inFade = (c.fadeIn > 0 && rel0 < c.fadeIn)
                         || (c.fadeOut > 0 && rel0 + count > c.length - c.fadeOut);
        if (!inFade)
        {
            juce::FloatVectorOperations::addWithMultiply(l + outOffset, srcL, c.gain, count);
            juce::FloatVectorOperations::addWithMultiply(r + outOffset, srcR, c.gain, count);
        }
        else
        {
            for (int i = 0; i < count; ++i)
            {
                const float g = c.gain * c.fadeGainAt(rel0 + i);
                l[outOffset + i] += srcL[i] * g;
                r[outOffset + i] += srcR[i] * g;
            }
        }
    }
}

void MixEngine::processChunk(const Arrangement* arrangement, const ParamArray& params, MasterParams& master,
                             juce::int64 position, int n, bool playClips, const TrackFeed* feeds, int feedOffset,
                             float* outL, float* outR, bool updateMeters)
{
    juce::FloatVectorOperations::clear(outL, n);
    juce::FloatVectorOperations::clear(outR, n);
    sendBuffer.clear(0, n);

    bool anySolo = false;
    for (auto* p : params)
        anySolo = anySolo || p->solo.load(std::memory_order_relaxed);

    float* l = trackBuffer.getWritePointer(0);
    float* r = trackBuffer.getWritePointer(1);
    float* sendL = sendBuffer.getWritePointer(0);
    float* sendR = sendBuffer.getWritePointer(1);

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = *params[(size_t) t];
        auto& d = dsp[(size_t) t];

        juce::FloatVectorOperations::clear(l, n);
        juce::FloatVectorOperations::clear(r, n);

        const auto kind = feeds != nullptr ? feeds[t].kind : TrackFeed::Kind::Playback;
        if (kind == TrackFeed::Kind::Playback)
        {
            if (playClips && arrangement != nullptr)
                renderClips(arrangement->tracks[(size_t) t], position, n, l, r);
        }
        else if (kind == TrackFeed::Kind::LiveInput && feeds[t].left != nullptr)
        {
            juce::FloatVectorOperations::copy(l, feeds[t].left + feedOffset, n);
            juce::FloatVectorOperations::copy(r, (feeds[t].right != nullptr ? feeds[t].right : feeds[t].left) + feedOffset, n);
        }

        updateFilters(d, p);
        if (d.lcOn)  { d.lowCut[0].processSamples(l, n);  d.lowCut[1].processSamples(r, n); }
        if (d.midOn) { d.mid[0].processSamples(l, n);     d.mid[1].processSamples(r, n); }
        if (d.hcOn)  { d.highCut[0].processSamples(l, n); d.highCut[1].processSamples(r, n); }

        const float vol = dbToGain(p.volumeDb.load(std::memory_order_relaxed));
        const float pan = p.pan.load(std::memory_order_relaxed);
        const float targetL = vol * juce::jmin(1.0f, 1.0f - pan);
        const float targetR = vol * juce::jmin(1.0f, 1.0f + pan);
        if (d.gainL.getCurrentValue() < 0.0f)
        {
            d.gainL.setCurrentAndTargetValue(targetL);
            d.gainR.setCurrentAndTargetValue(targetR);
        }
        d.gainL.setTargetValue(targetL);
        d.gainR.setTargetValue(targetR);
        applySmoothedGain(d.gainL, l, n);
        applySmoothedGain(d.gainR, r, n);

        if (updateMeters)
        {
            atomicMax(p.peakL, peakOf(l, n));
            atomicMax(p.peakR, peakOf(r, n));
        }

        const bool audible = !p.mute.load(std::memory_order_relaxed) && (!anySolo || p.solo.load(std::memory_order_relaxed));
        if (!audible)
            continue;

        juce::FloatVectorOperations::add(outL, l, n);
        juce::FloatVectorOperations::add(outR, r, n);

        const float send = p.send.load(std::memory_order_relaxed);
        if (send > 0.0001f)
        {
            juce::FloatVectorOperations::addWithMultiply(sendL, l, send, n);
            juce::FloatVectorOperations::addWithMultiply(sendR, r, send, n);
        }
    }

    // Reverb bus: always processed so tails ring out naturally.
    const float room = master.reverbRoom.load(std::memory_order_relaxed);
    const float damp = master.reverbDamping.load(std::memory_order_relaxed);
    const float wet  = master.reverbWet.load(std::memory_order_relaxed);
    if (! juce::exactlyEqual(room, appliedRoom) || ! juce::exactlyEqual(damp, appliedDamp) || ! juce::exactlyEqual(wet, appliedWet))
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = room;
        rp.damping = damp;
        rp.wetLevel = wet;
        rp.dryLevel = 0.0f;
        rp.width = 1.0f;
        reverb.setParameters(rp);
        appliedRoom = room; appliedDamp = damp; appliedWet = wet;
    }
    reverb.processStereo(sendL, sendR, n);
    juce::FloatVectorOperations::add(outL, sendL, n);
    juce::FloatVectorOperations::add(outR, sendR, n);

    const float mg = dbToGain(master.volumeDb.load(std::memory_order_relaxed));
    if (masterGain.getCurrentValue() < 0.0f)
        masterGain.setCurrentAndTargetValue(mg);
    masterGain.setTargetValue(mg);
    if (masterGain.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = masterGain.getNextValue();
            outL[i] *= g;
            outR[i] *= g;
        }
    }
    else
    {
        juce::FloatVectorOperations::multiply(outL, mg, n);
        juce::FloatVectorOperations::multiply(outR, mg, n);
    }

    if (updateMeters)
    {
        atomicMax(master.peakL, peakOf(outL, n));
        atomicMax(master.peakR, peakOf(outR, n));
    }
}

juce::AudioBuffer<float> MixEngine::renderOffline(const Arrangement& arrangement, const ParamArray& params,
                                                  MasterParams& master, double sampleRate, double tailSeconds)
{
    constexpr int block = 1024;
    MixEngine engine;
    engine.prepare(sampleRate, block);

    const auto total = arrangement.getEnd() + (juce::int64) (tailSeconds * sampleRate);
    juce::AudioBuffer<float> out(2, (int) total);
    out.clear();

    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        const int n = (int) juce::jmin<juce::int64>(block, total - pos);
        engine.process(&arrangement, params, master, pos, n, true, nullptr,
                       out.getWritePointer(0, (int) pos), out.getWritePointer(1, (int) pos), false);
    }
    return out;
}

#include "Dynamics.h"
#include <set>

namespace
{
    juce::String dbText(float v) { return juce::String(v, 1) + " dB"; }
    juce::String msText(float v) { return v >= 1000.0f ? juce::String(v / 1000.0f, 2) + " s" : juce::String(v, v < 10.0f ? 1 : 0) + " ms"; }
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }

    juce::NormalisableRange<float> skewed(float lo, float hi, float centre, float interval = 0.0f)
    {
        juce::NormalisableRange<float> r(lo, hi, interval);
        r.setSkewForCentre(centre);
        return r;
    }

    float coefficient(double ms, double sampleRate) { return (float) std::exp(-1.0 / juce::jmax(1.0, ms * 0.001 * sampleRate)); }

    constexpr float kInfiniteRatio = 19.95f;
}

//==============================================================================
const char* CompressorEffect::xmlTypeName = "eightTrackCompressor";

CompressorEffect::CompressorEffect(te::PluginCreationInfo info) : KeyedEffect(info)
{
    threshold = addKnob("threshold", "Threshold", { -60.0f, 0.0f, 0.1f }, -18.0f, dbText, "Compress");
    ratio     = addKnob("ratio", "Ratio", skewed(1.0f, 20.0f, 4.0f, 0.1f), 4.0f,
                        [](float v) { return v >= kInfiniteRatio ? juce::String::fromUTF8("∞:1") : juce::String(v, 1) + ":1"; }, "Compress");
    knee      = addKnob("knee", "Knee", { 0.0f, 18.0f, 0.1f }, 6.0f, dbText, "Compress");
    attack    = addKnob("attack", "Attack", skewed(0.1f, 100.0f, 10.0f, 0.1f), 10.0f, msText, "Compress");
    release   = addKnob("release", "Release", skewed(10.0f, 2000.0f, 200.0f, 1.0f), 150.0f, msText, "Compress");
    makeup    = addKnob("makeup", "Makeup", { 0.0f, 24.0f, 0.1f }, 0.0f, dbText, "Output");
    mix       = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Output");
    detect    = addChoice("detect", "Detect", { "Peak", "RMS" }, 0, "Key");
    keyFilter = addKnob("keyFilter", "Key Filter", skewed(20.0f, 500.0f, 100.0f, 1.0f), 20.0f,
                        [](float v) { return v < 20.5f ? juce::String("Off") : juce::String(juce::roundToInt(v)) + " Hz"; }, "Key");
    listen    = addToggle("listen", "Listen", false, "Key");
}

CompressorEffect::~CompressorEffect() { notifyListenersOfDeletion(); }

void KeyedEffect::getChannelNames(juce::StringArray* ins, juce::StringArray* outs)
{
    Plugin::getChannelNames(ins, outs);
    if (ins != nullptr)
    {
        ins->add("Key Left");
        ins->add("Key Right");
    }
}

bool CompressorEffect::isControlActive(const te::AutomatableParameter& p) const
{
    return &p != knee || ratio->getCurrentValue() > 1.01f;
}

void CompressorEffect::initialise(const te::PluginInitialisationInfo&) { reset(); }

void CompressorEffect::reset()
{
    keyHp[0].reset();
    keyHp[1].reset();
    rms = 0.0f;
    reduction = 0.0f;
}

void CompressorEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const int channels = juce::jmin(2, b.getNumChannels());
    const bool keyed = hasExternalKey(b.getNumChannels());

    float* io[2] = { b.getWritePointer(0, fc.bufferStartSample), b.getWritePointer(channels - 1, fc.bufferStartSample) };
    const float* key[2] = { keyed ? b.getReadPointer(2, fc.bufferStartSample) : io[0],
                            keyed ? b.getReadPointer(3, fc.bufferStartSample) : io[1] };

    const float thr = threshold->getCurrentValue();
    const float r = ratio->getCurrentValue();
    const float slope = r >= kInfiniteRatio ? -1.0f : 1.0f / r - 1.0f;   // dB of reduction per dB over
    const float kneeDb = knee->getCurrentValue();
    const float attackCoef = coefficient(attack->getCurrentValue(), sampleRate);
    const float releaseCoef = coefficient(release->getCurrentValue(), sampleRate);
    const float rmsCoef = coefficient(10.0, sampleRate);
    const bool useRms = choiceOf(*detect) == 1;
    const bool filtered = keyFilter->getCurrentValue() > 20.5f;
    for (auto& f : keyHp)
        f.set(sampleRate, keyFilter->getCurrentValue(), 0.707);
    const float makeupDb = makeup->getCurrentValue();
    const float wet = mix->getCurrentValue(), dry = 1.0f - wet;
    const bool listening = isOn(*listen);

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        float k[2] = { key[0][i], key[1][i] };
        if (filtered)
            for (int ch = 0; ch < 2; ++ch)
                k[ch] = keyHp[ch].process(k[ch], EffectSvf::HighPass);

        float level = juce::jmax(std::abs(k[0]), std::abs(k[1]));
        if (useRms)
        {
            rms = level * level + (rms - level * level) * rmsCoef;
            level = std::sqrt(rms * 2.0f);   // a sine's RMS reads as its peak
        }
        const float over = juce::Decibels::gainToDecibels(level, -120.0f) - thr;

        // The static curve, with a soft knee
        float target = 0.0f;
        if (kneeDb > 0.0f && std::abs(over) <= kneeDb * 0.5f)
            target = slope * (over + kneeDb * 0.5f) * (over + kneeDb * 0.5f) / (2.0f * kneeDb);
        else if (over > 0.0f)
            target = slope * over;

        reduction = target + (reduction - target) * (target < reduction ? attackCoef : releaseCoef);
        const float g = juce::Decibels::decibelsToGain(reduction + makeupDb);

        if (listening)
        {
            io[0][i] = k[0];
            if (channels > 1) io[1][i] = k[1];
            continue;
        }
        for (int ch = 0; ch < channels; ++ch)
            io[ch][i] = io[ch][i] * (dry + wet * g);
    }
    reductionNow.store(-reduction, std::memory_order_relaxed);
}

bool KeyedEffect::feeds(te::Track& from, te::Track& to)
{
    const auto tracks = te::getAudioTracks(from.edit);
    std::set<te::Track*> visited;
    auto reaches = [&](auto&& self, te::Track& current) -> bool
    {
        if (&current == &to)
            return true;
        auto* source = dynamic_cast<te::AudioTrack*>(&current);
        if (source == nullptr || !visited.insert(source).second)
            return false;

        if (auto* destination = source->getOutput().getDestinationTrack())
            if (self(self, *destination))
                return true;

        // A detector is also a dependency on the source track, even when the key is muted.
        for (auto* track : tracks)
            for (auto* plugin : track->pluginList.getPlugins())
                if (plugin->getSidechainSourceID() == source->itemID && self(self, *track))
                    return true;

        for (auto* plugin : source->pluginList.getPlugins())
            if (auto* send = dynamic_cast<te::AuxSendPlugin*>(plugin); send != nullptr && send->isEnabled())
                for (auto* track : tracks)
                    for (auto* receiver : track->pluginList.getPlugins())
                        if (auto* ret = dynamic_cast<te::AuxReturnPlugin*>(receiver);
                            ret != nullptr && ret->isEnabled() && ret->busNumber == send->getBusNumber() && self(self, *track))
                            return true;
        return false;
    };
    return reaches(reaches, from);
}

void KeyedEffect::removeLoopingKeys(te::Edit& e)
{
    for (auto* p : te::getAllPlugins(e, false))
        if (auto* c = dynamic_cast<KeyedEffect*>(p))
            if (auto* key = c->getKeySource(); key != nullptr)
                if (auto* owner = c->getOwnerTrack(); owner != nullptr && (key == owner || feeds(*owner, *key)))
                    c->setSidechainSourceID({});
}

std::vector<te::AudioTrack*> KeyedEffect::getKeySources()
{
    std::vector<te::AudioTrack*> sources;
    auto* owner = getOwnerTrack();
    for (auto* t : te::getAudioTracks(edit))
        if (t != owner && (owner == nullptr || !feeds(*owner, *t)))
            sources.push_back(t);
    return sources;
}

te::AudioTrack* KeyedEffect::getKeySource()
{
    if (!getSidechainSourceID().isValid())
        return nullptr;
    return dynamic_cast<te::AudioTrack*>(te::findTrackForID(edit, getSidechainSourceID()));
}

void KeyedEffect::setKeySource(te::AudioTrack* track)
{
    if (track != nullptr)
    {
        auto* owner = getOwnerTrack();
        if (&track->edit != &edit || (owner != nullptr && feeds(*owner, *track)))
            return;
    }
    const auto source = track != nullptr ? track->itemID : te::EditItemID();
    if (source == getSidechainSourceID())
        return;

    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction(track != nullptr ? "Key " + getName() + " from " + track->getName()
                                              : "Remove " + getName() + " key");
    setSidechainSourceID(source);
    if (track != nullptr && getNumWires() == 0)
        guessSidechainRouting();
    undo.beginNewTransaction();
}

std::vector<EffectPlugin::Selector> KeyedEffect::getSelectors()
{
    Selector key;
    key.name = "Key Source";
    key.row = "Key";
    key.options = [this]
    {
        juce::StringArray names { "This track" };
        for (auto* t : getKeySources())
            names.add(t->getName());
        return names;
    };
    key.get = [this]
    {
        auto* current = getKeySource();
        if (current == nullptr)
            return 0;
        const auto sources = getKeySources();
        const auto it = std::find(sources.begin(), sources.end(), current);
        return it == sources.end() ? -1 : 1 + (int) (it - sources.begin());
    };
    key.set = [this](int index)
    {
        const auto sources = getKeySources();
        setKeySource(juce::isPositiveAndBelow(index - 1, (int) sources.size()) ? sources[(size_t) (index - 1)] : nullptr);
    };
    return { key };
}

//==============================================================================
const char* LimiterEffect::xmlTypeName = "eightTrackLimiter";

LimiterEffect::LimiterEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    gain     = addKnob("gain", "Gain", { 0.0f, 24.0f, 0.1f }, 0.0f, dbText, "Limit");
    ceiling  = addKnob("ceiling", "Ceiling", { -12.0f, 0.0f, 0.1f }, -0.3f, dbText, "Limit");
    release  = addKnob("release", "Release", skewed(1.0f, 1000.0f, 80.0f, 1.0f), 80.0f, msText, "Limit");
    truePeak = addToggle("truePeak", "True Peak", true, "Limit");
}

LimiterEffect::~LimiterEffect() { notifyListenersOfDeletion(); }

void LimiterEffect::initialise(const te::PluginInitialisationInfo& info)
{
    lookahead = juce::jmax(1, (int) std::round(kLookaheadSeconds * info.sampleRate));
    delay.setSize(2, lookahead);
    wanted.assign((size_t) lookahead + 2, 1.0f);
    minQueue.assign((size_t) lookahead + 2, 0LL);
    smoothing.assign((size_t) lookahead, 1.0f);
    reset();
}

void LimiterEffect::reset()
{
    delay.clear();
    std::fill(smoothing.begin(), smoothing.end(), 1.0f);
    smoothSum = lookahead;
    queueHead = queueTail = 0;
    pos = 0;
    sampleIndex = 0;
    envelope = 1.0f;
    for (auto& h : history)
        std::fill(std::begin(h), std::end(h), 0.0f);
}

void LimiterEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || smoothing.empty())
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const int channels = juce::jmin(2, b.getNumChannels());

    const float inputGain = juce::Decibels::decibelsToGain(gain->getCurrentValue());
    const float limit = juce::Decibels::decibelsToGain(ceiling->getCurrentValue());
    const float releaseCoef = coefficient(release->getCurrentValue(), sampleRate);
    const bool interSample = isOn(*truePeak);
    const int capacity = (int) wanted.size();   // ring for the sliding minimum: lookahead + 2
    float lowest = 1.0f;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        float in[2];
        float peak = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float x = b.getSample(ch, fc.bufferStartSample + i) * inputGain;
            in[ch] = x;
            peak = juce::jmax(peak, std::abs(x));
            if (interSample)
            {
                // Peaks between the last two samples, by Hermite interpolation at 1/4, 1/2 and 3/4.
                auto& h = history[ch];
                const float y0 = h[0], y1 = h[1], y2 = h[2], y3 = x;
                const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                for (float t : { 0.25f, 0.5f, 0.75f })
                    peak = juce::jmax(peak, std::abs(((c3 * t + c2) * t + c1) * t + y1));
                h[0] = y1; h[1] = y2; h[2] = y3;
            }
        }
        const float need = peak > limit ? limit / peak : 1.0f;

        // Sliding minimum of the needed gain over the lookahead (a monotonic queue in a ring).
        while (queueTail != queueHead && wanted[(size_t) ((queueTail - 1 + capacity) % capacity)] >= need)
            queueTail = (queueTail - 1 + capacity) % capacity;
        wanted[(size_t) queueTail] = need;
        minQueue[(size_t) queueTail] = sampleIndex;
        queueTail = (queueTail + 1) % capacity;
        while (minQueue[(size_t) queueHead] < sampleIndex - lookahead)   // keep the last lookahead + 1 samples
            queueHead = (queueHead + 1) % capacity;
        const float hold = wanted[(size_t) queueHead];
        ++sampleIndex;

        // Down at once, back up at the release rate; then a box filter over the lookahead,
        // so the gain is fully down by the time the peak comes out of the delay.
        envelope = hold < envelope ? hold : hold + (envelope - hold) * releaseCoef;
        smoothSum += envelope - smoothing[(size_t) pos];
        smoothing[(size_t) pos] = envelope;
        const float g = (float) (smoothSum / lookahead);
        lowest = juce::jmin(lowest, g);

        for (int ch = 0; ch < channels; ++ch)
        {
            float* d = delay.getWritePointer(ch);
            const float out = d[pos] * g;
            d[pos] = in[ch];
            b.setSample(ch, fc.bufferStartSample + i, juce::jlimit(-limit, limit, out));
        }
        if (++pos >= lookahead)
            pos = 0;
    }
    if (channels == 1 && b.getNumChannels() > 1)
        b.copyFrom(1, fc.bufferStartSample, b, 0, fc.bufferStartSample, fc.bufferNumSamples);
    reductionNow.store(-juce::Decibels::gainToDecibels(lowest, -60.0f), std::memory_order_relaxed);
}

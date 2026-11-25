#include "DrumSampler.h"

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    juce::String dbText(float v) { return juce::String(v, 1) + " dB"; }
}

const char* DrumSampler::xmlTypeName = "eightTrackDrumSampler";

DrumSampler::DrumSampler(te::PluginCreationInfo info) : EffectPlugin(info)
{
    for (int i = 0; i < kPads; ++i)
    {
        const auto row = rowFor(i);
        const auto id = "pad" + juce::String(i + 1);
        const auto name = juce::String(i + 1) + " ";
        auto& p = pads[(size_t) i];
        p.tune   = addKnob(id + "Tune", name + "Tune", { -24.0f, 24.0f, 0.1f }, 0.0f,
                           [](float v) { return signedText(v, 1, " st"); }, row);
        p.attack = addKnob(id + "Attack", name + "Attack", { 0.0f, 200.0f, 0.1f, 0.4f }, 0.0f,
                           [](float v) { return juce::String(v, 1) + " ms"; }, row);
        p.decay  = addKnob(id + "Decay", name + "Decay", { 0.02f, 1.0f, 0.01f }, 1.0f,
                           [](float v) { return v >= 0.995f ? juce::String("Full") : percentText(v); }, row);
        p.tone   = addKnob(id + "Tone", name + "Tone", { -1.0f, 1.0f, 0.01f }, 0.0f,
                           [](float v) { return v < -0.005f ? "Dark " + percentText(-v) : v > 0.005f ? "Bright " + percentText(v) : juce::String("Flat"); }, row);
        p.level  = addKnob(id + "Level", name + "Level", { -48.0f, 6.0f, 0.1f }, 0.0f, dbText, row);
        p.pan    = addKnob(id + "Pan", name + "Pan", { -1.0f, 1.0f, 0.01f }, 0.0f,
                           [](float v) { return std::abs(v) < 0.005f ? juce::String("C") : (v < 0 ? "L" : "R") + juce::String(juce::roundToInt(std::abs(v) * 100.0f)); }, row);
        p.choke  = addChoice(id + "Choke", name + "Choke", { "None", "A", "B", "C", "D" }, 0, row);
        for (auto [param, label] : { std::pair { p.tune, "Tune" }, { p.attack, "Attack" }, { p.decay, "Decay" },
                                     { p.tone, "Tone" }, { p.level, "Level" }, { p.pan, "Pan" }, { p.choke, "Choke" } })
            setLabel(param, label);
    }
    velocity = addKnob("velocity", "Velocity", { 0.0f, 1.0f, 0.01f }, 0.7f, percentText, "Kit");
    volume   = addKnob("volume", "Volume", { -36.0f, 6.0f, 0.1f }, 0.0f, dbText, "Kit");
    for (int i = 0; i < kPads; ++i)
        reloadPad(i);
}

DrumSampler::~DrumSampler() { notifyListenersOfDeletion(); }

void DrumSampler::initialise(const te::PluginInitialisationInfo&) { reset(); }

void DrumSampler::reset()
{
    for (auto& v : voices)
    {
        v.active = false;
        v.sample = nullptr;   // the slots (or the graveyard) still hold it: nothing is freed here
    }
}

juce::File DrumSampler::getPadFile(int pad) const { return getFileProperty(padProperty(pad)); }

void DrumSampler::reloadPad(int pad)
{
    const auto file = getPadFile(pad);
    juce::String error;
    slots[(size_t) pad].set(file.existsAsFile() ? Sample::load(file, error, 30.0) : nullptr);
}

bool DrumSampler::loadPad(int pad, const juce::File& file, juce::String& error)
{
    if (!juce::isPositiveAndBelow(pad, kPads))
        return false;
    if (file == juce::File())
    {
        edit.getUndoManager().beginNewTransaction("Clear pad " + juce::String(pad + 1));
        setFileProperty(padProperty(pad), {});
        slots[(size_t) pad].set(nullptr);
        return true;
    }
    auto sample = Sample::load(file, error, 30.0);
    if (sample == nullptr)
        return false;
    edit.getUndoManager().beginNewTransaction("Load " + file.getFileName() + " on pad " + juce::String(pad + 1));
    setFileProperty(padProperty(pad), adoptFile(file));
    slots[(size_t) pad].set(sample);
    return true;
}

void DrumSampler::getFileReferences(std::vector<FileReference>& refs)
{
    for (int i = 0; i < kPads; ++i)
        refs.push_back({ state, padProperty(i) });
}

void DrumSampler::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    EffectPlugin::restorePluginStateFromValueTree(v);
    for (int i = 0; i < kPads; ++i)
    {
        state.setProperty(padProperty(i), v[padProperty(i)], nullptr);
        reloadPad(i);
    }
}

void DrumSampler::audition(int pad, int vel)
{
    pendingVelocity = vel;
    pendingAudition = pad;
}

void DrumSampler::start(int pad, int vel)
{
    auto sample = slots[(size_t) pad].get();
    activity[(size_t) pad] = 1.0f;
    if (sample == nullptr)
        return;
    const auto& p = pads[(size_t) pad];
    const int group = choiceOf(*p.choke);

    // The pad cuts off its last hit, and its choke group cuts the others.
    for (auto& v : voices)
        if (v.active && (v.pad == pad || (group > 0 && choiceOf(*pads[(size_t) v.pad].choke) == group)))
            v.env.noteOff();

    // A free voice, or the oldest
    Voice* voice = nullptr;
    for (auto& v : voices)
        if (!v.active) { voice = &v; break; }
    if (voice == nullptr)
    {
        voice = &voices[0];
        for (auto& v : voices)
            if (v.started < voice->started)
                voice = &v;
    }
    auto& v = *voice;
    v.sample = sample;
    v.pad = pad;
    v.pos = 0;
    v.step = sample->rate / sampleRate * std::pow(2.0, p.tune->getCurrentValue() / 12.0);
    const float sens = velocity->getCurrentValue();
    v.gain = juce::Decibels::decibelsToGain(p.level->getCurrentValue()) * (1.0f - sens + sens * (float) vel / 127.0f);
    const float pan = p.pan->getCurrentValue();
    v.panL = std::cos((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f) * juce::MathConstants<float>::sqrt2;
    v.panR = std::sin((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f) * juce::MathConstants<float>::sqrt2;
    v.panL = juce::jmin(1.0f, v.panL);
    v.panR = juce::jmin(1.0f, v.panR);

    // Decay: Full plays the whole sound; less fades it out over that share of it.
    const float decay = p.decay->getCurrentValue();
    const double playSeconds = sample->lengthSeconds() / (v.step * sampleRate / sample->rate);
    juce::ADSR::Parameters env;
    env.attack = p.attack->getCurrentValue() / 1000.0f;
    env.decay = decay >= 0.995f ? 0.0f : (float) (decay * playSeconds);
    env.sustain = decay >= 0.995f ? 1.0f : 0.0f;
    env.release = 0.005f;   // when cut off
    v.env.setSampleRate(sampleRate);
    v.env.setParameters(env);
    v.env.reset();
    v.env.noteOn();
    for (auto& t : v.tilt)
    {
        t.low = 0;
        t.set(sampleRate, p.tone->getCurrentValue());
    }
    v.active = true;
    v.started = ++counter;
}

void DrumSampler::render(Voice& v, float* l, float* r, int n)
{
    const auto& s = *v.sample;
    const int length = s.audio.getNumSamples();
    const bool stereo = s.audio.getNumChannels() > 1;
    for (int i = 0; i < n; ++i)
    {
        if (v.pos >= length || !v.env.isActive())
        {
            v.active = false;
            v.sample = nullptr;   // the slot or the graveyard still holds it
            return;
        }
        const float e = v.env.getNextSample() * v.gain;
        const float a = s.read(0, v.pos), b = stereo ? s.read(1, v.pos) : a;
        l[i] += v.tilt[0].process(a) * e * v.panL;
        r[i] += v.tilt[1].process(b) * e * v.panR;
        v.pos += v.step;
    }
}

void DrumSampler::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* out[2] = { b.getWritePointer(0, fc.bufferStartSample), b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample) };
    for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
        juce::FloatVectorOperations::clear(out[ch], fc.bufferNumSamples);

    // Mono track: mix both sides into a scratch, then fold down.
    float right[512];
    float* rightOut = stereo ? out[1] : right;

    if (const int pad = pendingAudition.exchange(-1); pad >= 0 && pad < kPads)
        start(pad, pendingVelocity.load());

    auto* midi = fc.bufferForMidiMessages;
    if (midi != nullptr && midi->isAllNotesOff)
        for (auto& v : voices)
            if (v.active)
                v.env.noteOff();
    if (midi != nullptr)
        midi->sortByTimestamp();
    const int numEvents = midi != nullptr ? midi->size() : 0;
    int next = 0, done = 0;
    while (done < fc.bufferNumSamples)
    {
        while (next < numEvents)
        {
            const auto& m = (*midi)[next];
            const int at = juce::jlimit(0, fc.bufferNumSamples - 1, (int) std::floor((m.getTimeStamp() + fc.midiBufferOffset) * sampleRate));
            if (at > done)
                break;
            if (m.isNoteOn() && juce::isPositiveAndBelow(m.getNoteNumber() - kFirstNote, kPads))
                start(m.getNoteNumber() - kFirstNote, m.getVelocity());
            ++next;   // note-offs don't stop drums: they play out (or are choked)
        }
        int until = fc.bufferNumSamples;
        if (next < numEvents)
            until = juce::jlimit(done + 1, fc.bufferNumSamples,
                                 (int) std::floor(((*midi)[next].getTimeStamp() + fc.midiBufferOffset) * sampleRate));
        if (!stereo)
            until = juce::jmin(until, done + 512);
        if (!stereo)
            juce::FloatVectorOperations::clear(right, until - done);
        for (auto& v : voices)
            if (v.active)
                render(v, out[0] + done, stereo ? rightOut + done : right, until - done);
        if (!stereo)
            for (int i = 0; i < until - done; ++i)
                out[0][done + i] = 0.5f * (out[0][done + i] + right[i]);
        done = until;
    }

    const float gain = juce::Decibels::decibelsToGain(volume->getCurrentValue());
    for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
        juce::FloatVectorOperations::multiply(out[ch], gain, fc.bufferNumSamples);

    const float fade = (float) std::exp(-fc.bufferNumSamples / (0.15 * sampleRate));
    for (auto& a : activity)
        a = a.load() * fade;
}

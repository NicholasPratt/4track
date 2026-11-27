#include "DrumSampler.h"
#include <FactoryDrumSamples.h>

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    juce::String dbText(float v) { return juce::String(v, 1) + " dB"; }

    using Kit = DrumSampler::FactoryKit;
    const juce::Identifier factoryKitID { "factoryKit" };

    struct KitDefinition
    {
        const char* id;
        const char* name;
        std::array<const char*, DrumSampler::kPads> sounds;
    };

    constexpr KitDefinition kits[] {
        { "", "Empty Kit", {} },
        { "acoustic", "Acoustic", { "Kick", "Snare", "Closed Hat", "Open Hat", "Cross Stick", "Rimshot", "Floor Tom",
                                    "Muted Tom", "Rack Tom", "Crash", "Ride", "Pedal Hat", "Ride Bell", "Ghost Snare",
                                    "Alt Kick", "Hat Splash" } },
        { "electronic", "Electronic / Dance", { "Kick", "Snare", "Closed Hat", "Open Hat", "Rimshot", "Clap", "Low Tom",
                                                "Mid Tom", "High Tom", "Crash", "Ride", "Short Hat", "Cowbell", "Alt Snare",
                                                "Sub Kick", "Maracas" } }
    };
}

const char* DrumSampler::xmlTypeName = "eightTrackDrumSampler";

DrumSampler::DrumSampler(te::PluginCreationInfo info) : EffectPlugin(info)
{
    juce::NormalisableRange<float> sendRange(-80.0f, 0.0f, 0.1f);
    sendRange.setSkewForCentre(-12.0f);
    for (int i = 0; i < kPads; ++i)
    {
        const auto row = rowFor(i);
        const auto id = "pad" + juce::String(i + 1);
        const auto name = juce::String(i + 1) + " ";
        auto& p = pads[(size_t) i];
        // Keep the Tune ID for existing projects and automation.
        p.tune   = addKnob(id + "Tune", name + "Pitch", { -24.0f, 24.0f, 0.1f }, 0.0f,
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
        for (auto [param, label] : { std::pair { p.tune, "Pitch" }, { p.attack, "Attack" }, { p.decay, "Decay" },
                                     { p.tone, "Tone" }, { p.level, "Level" }, { p.pan, "Pan" }, { p.choke, "Choke" } })
            setLabel(param, label);
        for (int bus = 0; bus < kSendCount; ++bus)
        {
            const auto label = "Bus " + juce::String(bus + 1);
            auto* send = addKnob(id + "Send" + juce::String(bus + 1), name + label + " Send", sendRange, -80.0f,
                                 [](float v) { return v <= -80.0f ? juce::String("Off") : dbText(v); }, row);
            p.sends[(size_t) bus] = send;
            setLabel(send, label + " Send");
        }
    }
    velocity = addKnob("velocity", "Velocity", { 0.0f, 1.0f, 0.01f }, 0.7f, percentText, "Kit");
    volume   = addKnob("volume", "Volume", { -36.0f, 6.0f, 0.1f }, 0.0f, dbText, "Kit");
    for (int i = 0; i < kPads; ++i)
        reloadPad(i);
}

DrumSampler::~DrumSampler() { notifyListenersOfDeletion(); }

DrumSampler::BusLayout DrumSampler::getBusses() const
{
    auto layout = BusLayout::singleStereoInOut();
    for (int i = 0; i < kSendCount; ++i)
        layout.outputs.push_back(te::ChannelConfiguration::stereo());
    return layout;
}

std::optional<DrumSampler::AuxOutputRoute> DrumSampler::getAuxOutputRoute(int outputBus) const
{
    if (!juce::isPositiveAndBelow(outputBus - 1, kSendCount))
        return {};
    return AuxOutputRoute { outputBus - 1, isEnabled() };
}

void DrumSampler::initialise(const te::PluginInitialisationInfo&) { reset(); }

void DrumSampler::reset()
{
    nextPreviewStep = 0;
    nextPreviewSample = 0.0;
    playingStep.store(-1, std::memory_order_relaxed);
    for (auto& v : voices)
    {
        v.active = false;
        v.sample = nullptr;   // the slots (or the graveyard) still hold it: nothing is freed here
    }
}

juce::File DrumSampler::getPadFile(int pad) const { return getFileProperty(padProperty(pad)); }

const juce::Identifier& DrumSampler::padProperty(int pad)
{
    static const auto properties = []
    {
        std::array<juce::Identifier, kPads> result;
        for (int i = 0; i < kPads; ++i)
            result[(size_t) i] = "pad" + juce::String(i + 1);
        return result;
    }();
    return properties[(size_t) pad];
}

const char* DrumSampler::kitName(FactoryKit kit) { return kits[(size_t) kit].name; }

DrumSampler::FactoryKit DrumSampler::getFactoryKit() const
{
    const auto id = state[factoryKitID].toString();
    if (id == kits[(size_t) Kit::acoustic].id)
        return Kit::acoustic;
    if (id == kits[(size_t) Kit::electronic].id)
        return Kit::electronic;
    return Kit::empty;
}

void DrumSampler::loadFactoryKit(FactoryKit kit)
{
    auto* undo = state.isAChildOf(edit.state) ? getUndoManager() : nullptr;
    if (undo != nullptr)
        undo->beginNewTransaction("Load " + juce::String(kitName(kit)) + " kit");
    state.setProperty(factoryKitID, kits[(size_t) kit].id, undo);
    for (int i = 0; i < kPads; ++i)
        state.removeProperty(padProperty(i), undo);
    if (undo != nullptr)
        undo->beginNewTransaction();
}

void DrumSampler::reloadPad(int pad)
{
    Sample::Ptr sample;
    juce::String error;
    // A saved pad property overrides the factory slot. An empty value explicitly clears it.
    if (state.hasProperty(padProperty(pad)))
    {
        if (const auto file = getPadFile(pad); file.existsAsFile())
            sample = Sample::load(file, error, 30.0);
    }
    else if (const auto kit = getFactoryKit(); kit != Kit::empty)
    {
        const auto& preset = kits[(size_t) kit];
        const auto resource = juce::String(preset.id) + "_" + juce::String(pad + 1).paddedLeft('0', 2) + "_flac";
        int size = 0;
        if (const auto* data = FactoryDrumSamples::getNamedResource(resource.toRawUTF8(), size))
            sample = Sample::load(std::make_unique<juce::MemoryInputStream>(data, (size_t) size, false),
                                  preset.sounds[(size_t) pad], error, 30.0);
        jassert(sample != nullptr);
    }
    slots[(size_t) pad].set(std::move(sample));
}

void DrumSampler::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    EffectPlugin::valueTreePropertyChanged(tree, property);
    if (tree != state || suppressPadReload)
        return;
    for (int i = 0; i < kPads; ++i)
        if (property == padProperty(i) || (property == factoryKitID && !state.hasProperty(padProperty(i))))
            reloadPad(i);
}

bool DrumSampler::loadPad(int pad, const juce::File& file, juce::String& error)
{
    if (!juce::isPositiveAndBelow(pad, kPads))
        return false;
    Sample::Ptr sample;
    if (file != juce::File())
    {
        sample = Sample::load(file, error, 30.0);
        if (sample == nullptr)
            return false;
    }
    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction(file == juce::File() ? "Clear pad " + juce::String(pad + 1)
                                                 : "Load " + file.getFileName() + " on pad " + juce::String(pad + 1));
    const juce::ScopedValueSetter<bool> suppress(suppressPadReload, true);
    setFileProperty(padProperty(pad), file == juce::File() ? file : adoptFile(file));
    slots[(size_t) pad].set(std::move(sample));
    undo.beginNewTransaction();
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
    const juce::ScopedValueSetter<bool> suppress(suppressPadReload, true);
    state.setProperty(factoryKitID, v[factoryKitID].toString(), nullptr);
    for (int i = 0; i < kPads; ++i)
    {
        const auto& property = padProperty(i);
        if (v.hasProperty(property))
            state.setProperty(property, v[property], nullptr);
        else
            state.removeProperty(property, nullptr);
        reloadPad(i);
    }
}

void DrumSampler::audition(int pad, int vel)
{
    pendingVelocity = vel;
    pendingAudition = pad;
}

void DrumSampler::previewPattern(const DrumPattern& pattern, bool restart)
{
    const juce::SpinLock::ScopedLockType lock(previewLock);
    pendingPreview = PreviewRequest { pattern, restart || (pendingPreview && pendingPreview->restart) };
}

void DrumSampler::stopPatternPreview()
{
    const juce::SpinLock::ScopedLockType lock(previewLock);
    pendingPreview = PreviewRequest {};
}

void DrumSampler::applyPreviewRequest(bool transportPlaying)
{
    std::optional<PreviewRequest> request;
    {
        const juce::SpinLock::ScopedTryLockType lock(previewLock);
        if (lock.isLocked())
            request = std::exchange(pendingPreview, {});
    }
    if (transportPlaying || (request && !request->pattern))
    {
        if (activePreview)
            for (auto& voice : voices)
                if (voice.active && voice.preview)
                    voice.env.noteOff();
        activePreview.reset();
        previewNoteCount = 0;
        playingStep.store(-1, std::memory_order_relaxed);
    }
    else if (request)
    {
        if (request->restart || !activePreview)
        {
            nextPreviewStep = 0;
            nextPreviewSample = 0.0;
            previewNoteCount = 0;
            for (auto& voice : voices)
                if (voice.active && voice.preview)
                    voice.env.noteOff();
        }
        activePreview = *request->pattern;
        if (nextPreviewStep >= activePreview->steps)
            nextPreviewStep = 0;
    }
}

void DrumSampler::start(int pad, int vel, bool preview)
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
    v.preview = preview;
    v.pos = 0;
    setVoicePitch(v, p.tune->getCurrentValue());
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

void DrumSampler::setVoicePitch(Voice& voice, float semitones)
{
    voice.pitch = semitones;
    voice.step = voice.sample->rate / sampleRate * std::exp2(semitones / 12.0);
}

void DrumSampler::render(Voice& v, const Outputs& out, int n)
{
    const auto& pad = pads[(size_t) v.pad];
    if (const float pitch = pad.tune->getCurrentValue(); pitch != v.pitch)
        setVoicePitch(v, pitch);
    std::array<float, kSendCount> sendGains {};
    for (int bus = 0; bus < kSendCount; ++bus)
        if (out[(size_t) (3 + 2 * bus)] != nullptr)
            sendGains[(size_t) bus] = juce::Decibels::decibelsToGain(pad.sends[(size_t) bus]->getCurrentValue(), -80.0f);
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
        const float left = v.tilt[0].process(a) * e * v.panL;
        const float right = v.tilt[1].process(b) * e * v.panR;
        out[0][i] += left;
        out[1][i] += right;
        for (int bus = 0; bus < kSendCount; ++bus)
            if (const auto send = sendGains[(size_t) bus]; send > 0.0f)
            {
                out[(size_t) (2 + 2 * bus)][i] += left * send;
                out[(size_t) (3 + 2 * bus)][i] += right * send;
            }
        v.pos += v.step;
    }
}

void DrumSampler::applyToBuffer(const te::PluginRenderContext& fc)
{
    applyPanic(fc);
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const int channels = juce::jmin(kOutputChannels, b.getNumChannels());
    if (channels == 0)
        return;
    const bool stereo = channels > 1;
    Outputs out {};
    for (int ch = 0; ch < channels; ++ch)
    {
        out[(size_t) ch] = b.getWritePointer(ch, fc.bufferStartSample);
        juce::FloatVectorOperations::clear(out[(size_t) ch], fc.bufferNumSamples);
    }

    // Mono track: mix both sides into a scratch, then fold down.
    float right[512];

    if (const int pad = pendingAudition.exchange(-1); pad >= 0 && pad < kPads)
        start(pad, pendingVelocity.load());
    applyPreviewRequest(fc.isPlaying);
    const double previewStepSamples = activePreview ? sampleRate * 60.0 / getTempo() * activePreview->stepBeats() : 0.0;

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
        if (activePreview && nextPreviewSample <= done)
        {
            // Queue the step's notes: a flam's grace and a roll's hits land inside the step
            for (int pad = 0; pad < kPads; ++pad)
                activePreview->forEachNote(pad, nextPreviewStep, [&](double offset, double, int vel)
                {
                    if (previewNoteCount < (int) previewNotes.size())
                        previewNotes[(size_t) previewNoteCount++] = { nextPreviewSample + offset * previewStepSamples, pad, vel };
                });
            playingStep.store(nextPreviewStep, std::memory_order_relaxed);
            nextPreviewStep = (nextPreviewStep + 1) % activePreview->steps;
            nextPreviewSample += previewStepSamples;
        }
        for (int i = 0; i < previewNoteCount;)
        {
            if (previewNotes[(size_t) i].at <= done)
            {
                start(previewNotes[(size_t) i].pad, previewNotes[(size_t) i].velocity, true);
                previewNotes[(size_t) i] = previewNotes[(size_t) --previewNoteCount];
            }
            else
                ++i;
        }
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
        if (activePreview)
            until = juce::jmin(until, (int) std::ceil(nextPreviewSample));
        for (int i = 0; i < previewNoteCount; ++i)
            until = juce::jmin(until, juce::jmax(done + 1, (int) std::ceil(previewNotes[(size_t) i].at)));
        if (!stereo)
            until = juce::jmin(until, done + 512);
        if (!stereo)
            juce::FloatVectorOperations::clear(right, until - done);
        Outputs section {};
        for (int ch = 0; ch < channels; ++ch)
            section[(size_t) ch] = out[(size_t) ch] + done;
        if (!stereo)
            section[1] = right;
        for (auto& v : voices)
            if (v.active)
                render(v, section, until - done);
        if (!stereo)
            for (int i = 0; i < until - done; ++i)
                out[0][done + i] = 0.5f * (out[0][done + i] + right[i]);
        done = until;
    }
    if (activePreview)
        nextPreviewSample -= fc.bufferNumSamples;
    for (int i = 0; i < previewNoteCount; ++i)
        previewNotes[(size_t) i].at -= fc.bufferNumSamples;

    const float gain = juce::Decibels::decibelsToGain(volume->getCurrentValue());
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::multiply(out[(size_t) ch], gain, fc.bufferNumSamples);

    const float fade = (float) std::exp(-fc.bufferNumSamples / (0.15 * sampleRate));
    for (auto& a : activity)
        a = a.load() * fade;
}

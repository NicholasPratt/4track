#include "KeySampler.h"
#include "TunerCapture.h"

namespace
{
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }
    juce::String secondsText(float v) { return v < 1.0f ? juce::String(juce::roundToInt(v * 1000.0f)) + " ms" : juce::String(v, 2) + " s"; }
}

const char* KeySampler::xmlTypeName = "eightTrackSampler";

KeySampler::KeySampler(te::PluginCreationInfo info) : EffectPlugin(info)
{
    root      = addKnob("root", "Root", { 0.0f, 127.0f, 1.0f }, 60.0f,
                        [](float v) { return juce::MidiMessage::getMidiNoteName(juce::roundToInt(v), true, true, 4); }, "Sample");
    fine      = addKnob("fine", "Fine", { -100.0f, 100.0f, 1.0f }, 0.0f, [](float v) { return signedText(v, 0, " ct"); }, "Sample");
    start     = addKnob("start", "Start", { 0.0f, 0.95f, 0.001f }, 0.0f, percentText, "Sample");
    loop      = addToggle("loop", "Loop", false, "Sample");
    loopStart = addKnob("loopStart", "Loop From", { 0.0f, 0.95f, 0.001f }, 0.3f, percentText, "Sample");

    attack    = addKnob("attack", "Attack", { 0.0f, 5.0f, 0.001f, 0.3f }, 0.002f, secondsText, "Envelope");
    decay     = addKnob("decay", "Decay", { 0.0f, 10.0f, 0.001f, 0.3f }, 0.5f, secondsText, "Envelope");
    sustain   = addKnob("sustain", "Sustain", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Envelope");
    release   = addKnob("release", "Release", { 0.005f, 10.0f, 0.001f, 0.3f }, 0.25f, secondsText, "Envelope");

    velocity  = addKnob("velocity", "Velocity", { 0.0f, 1.0f, 0.01f }, 0.7f, percentText, "Play");
    tone      = addKnob("tone", "Tone", { -1.0f, 1.0f, 0.01f }, 0.0f,
                        [](float v) { return v < -0.005f ? "Dark " + percentText(-v) : v > 0.005f ? "Bright " + percentText(v) : juce::String("Flat"); }, "Play");
    volume    = addKnob("volume", "Volume", { -36.0f, 6.0f, 0.1f }, 0.0f, [](float v) { return juce::String(v, 1) + " dB"; }, "Play");
    reload();
}

KeySampler::~KeySampler() { notifyListenersOfDeletion(); }

bool KeySampler::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == loopStart)
        return isOn(*loop);
    return true;
}

void KeySampler::initialise(const te::PluginInitialisationInfo&) { reset(); }

void KeySampler::reset()
{
    for (auto& v : voices)
    {
        v.active = false;
        v.sample = nullptr;
    }
}

void KeySampler::reload()
{
    const auto file = getFile();
    juce::String error;
    slot.set(file.existsAsFile() ? Sample::load(file, error) : nullptr);
}

bool KeySampler::loadFile(const juce::File& file, juce::String& error)
{
    if (file == juce::File())
    {
        edit.getUndoManager().beginNewTransaction("Clear sample");
        setFileProperty(fileProperty, {});
        slot.set(nullptr);
        return true;
    }
    auto sample = Sample::load(file, error);
    if (sample == nullptr)
        return false;
    edit.getUndoManager().beginNewTransaction("Load " + file.getFileName());
    setFileProperty(fileProperty, adoptFile(file));
    slot.set(sample);
    return true;
}

bool KeySampler::useRecording(Sample::Ptr sample, juce::String& error)
{
    if (sample == nullptr || sample->audio.getNumSamples() == 0)
    {
        error = "Nothing was recorded: the input was silent.";
        return false;
    }
    const auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File();
    auto folder = editFile != juce::File() ? editFile.getParentDirectory().getChildFile("Audio")
                                           : juce::File::getSpecialLocation(juce::File::tempDirectory);
    folder.createDirectory();
    const auto file = folder.getNonexistentChildFile("Sampler " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S"), ".wav", false);
    {
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        juce::WavAudioFormat wav;
        auto writer = stream != nullptr ? wav.createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                                          .withSampleRate(sample->rate)
                                                                          .withNumChannels(sample->audio.getNumChannels())
                                                                          .withBitsPerSample(24))
                                        : nullptr;
        if (writer == nullptr || !writer->writeFromAudioSampleBuffer(sample->audio, 0, sample->audio.getNumSamples()))
        {
            error = "Couldn't save the recording to " + folder.getFullPathName() + ".";
            return false;
        }
    }
    sample->name = file.getFileNameWithoutExtension();
    edit.getUndoManager().beginNewTransaction("Record into the Sampler");
    setFileProperty(fileProperty, file);
    slot.set(sample);
    detectRoot();
    return true;
}

bool KeySampler::detectRoot()
{
    auto sample = slot.get();
    if (sample == nullptr)
        return false;
    // The pitch of the steadiest part: 100 ms to 600 ms in, mono.
    const int from = juce::jmin(sample->audio.getNumSamples(), (int) (0.1 * sample->rate));
    const int n = juce::jmin(sample->audio.getNumSamples() - from, (int) (0.5 * sample->rate));
    if (n < (int) (0.05 * sample->rate))
        return false;
    std::vector<float> mono((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        float s = 0;
        for (int ch = 0; ch < sample->audio.getNumChannels(); ++ch)
            s += sample->audio.getSample(ch, from + i);
        mono[(size_t) i] = s / (float) sample->audio.getNumChannels();
    }
    const double hz = TunerCapture::frequencyOf(mono, sample->rate);
    if (hz <= 0)
        return false;
    const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int nearest = juce::roundToInt(note);
    root->setParameter((float) juce::jlimit(0, 127, nearest), juce::sendNotification);
    fine->setParameter((float) juce::jlimit(-100, 100, juce::roundToInt((note - nearest) * 100.0)), juce::sendNotification);
    return true;
}

std::vector<EffectPlugin::FileSlot> KeySampler::getFileSlots()
{
    FileSlot s;
    s.name = "Sound";
    s.row = "Sample";
    s.wildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg";
    s.get = [this] { return getFile(); };
    s.set = [this](const juce::File& f, juce::String& error) { return loadFile(f, error); };
    return { s };
}

void KeySampler::getFileReferences(std::vector<FileReference>& refs) { refs.push_back({ state, fileProperty }); }

void KeySampler::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    EffectPlugin::restorePluginStateFromValueTree(v);
    state.setProperty(fileProperty, v[fileProperty], nullptr);
    reload();
}

void KeySampler::playKey(int note, bool on)
{
    int s1, n1, s2, n2;
    keyFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 > 0)
        keyQueue[(size_t) s1] = on ? note + 1 : -(note + 1);
    keyFifo.finishedWrite(n1);
}

void KeySampler::noteOn(int note, int vel)
{
    auto sample = slot.get();
    if (sample == nullptr)
        return;
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
    v.note = note;
    v.pos = (double) start->getCurrentValue() * sample->audio.getNumSamples();
    const double semis = (double) note - root->getCurrentValue() - fine->getCurrentValue() / 100.0;
    v.step = sample->rate / sampleRate * std::pow(2.0, semis / 12.0);
    const float sens = velocity->getCurrentValue();
    v.gain = 1.0f - sens + sens * (float) vel / 127.0f;
    juce::ADSR::Parameters env;
    env.attack = attack->getCurrentValue();
    env.decay = decay->getCurrentValue();
    env.sustain = sustain->getCurrentValue();
    env.release = release->getCurrentValue();
    v.env.setSampleRate(sampleRate);
    v.env.setParameters(env);
    v.env.reset();
    v.env.noteOn();
    for (auto& t : v.tilt)
    {
        t.low = 0;
        t.set(sampleRate, tone->getCurrentValue());
    }
    v.active = true;
    v.started = ++counter;
}

void KeySampler::noteOff(int note)
{
    for (auto& v : voices)
        if (v.active && v.note == note)
        {
            v.env.noteOff();
            v.note = -1;   // so a new press of the key gets its own voice
        }
}

void KeySampler::render(Voice& v, float* l, float* r, int n)
{
    const auto& s = *v.sample;
    const int length = s.audio.getNumSamples();
    const bool stereo = s.audio.getNumChannels() > 1;
    const bool looping = isOn(*loop);
    const double loopFrom = juce::jmin((double) loopStart->getCurrentValue() * length, length - 64.0);
    const double loopLength = length - loopFrom;
    // Loops crossfade over 20 ms (or a quarter of the loop), so the seam doesn't click.
    const double fade = juce::jmin(0.02 * s.rate, loopLength * 0.25);
    for (int i = 0; i < n; ++i)
    {
        if (!v.env.isActive() || (!looping && v.pos >= length))
        {
            v.active = false;
            v.sample = nullptr;
            return;
        }
        if (looping && v.pos >= length)
            v.pos -= loopLength;
        float a = s.read(0, v.pos), b = stereo ? s.read(1, v.pos) : a;
        if (looping && v.pos > length - fade && fade > 1.0)
        {
            const float t = (float) ((v.pos - (length - fade)) / fade);   // 0..1 into the seam
            const double earlier = v.pos - loopLength;
            const float ea = s.read(0, earlier), eb = stereo ? s.read(1, earlier) : ea;
            const float g0 = std::cos(t * juce::MathConstants<float>::halfPi), g1 = std::sin(t * juce::MathConstants<float>::halfPi);
            a = a * g0 + ea * g1;
            b = b * g0 + eb * g1;
        }
        const float e = v.env.getNextSample() * v.gain;
        l[i] += v.tilt[0].process(a) * e;
        r[i] += v.tilt[1].process(b) * e;
        v.pos += v.step;
    }
}

void KeySampler::applyToBuffer(const te::PluginRenderContext& fc)
{
    applyPanic(fc);
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;
    float* out[2] = { b.getWritePointer(0, fc.bufferStartSample), b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample) };
    for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
        juce::FloatVectorOperations::clear(out[ch], fc.bufferNumSamples);
    float right[512];

    // Keys from the window
    {
        int s1, n1, s2, n2;
        keyFifo.prepareToRead(keyFifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1 + n2; ++i)
        {
            const int k = keyQueue[(size_t) (i < n1 ? s1 + i : s2 + i - n1)];
            if (k > 0) noteOn(k - 1, 100);
            else noteOff(-k - 1);
        }
        keyFifo.finishedRead(n1 + n2);
    }

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
            if (m.isNoteOn())
                noteOn(m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff())
                noteOff(m.getNoteNumber());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                for (auto& v : voices)
                    if (v.active)
                        v.env.noteOff();
            ++next;
        }
        int until = fc.bufferNumSamples;
        if (next < numEvents)
            until = juce::jlimit(done + 1, fc.bufferNumSamples,
                                 (int) std::floor(((*midi)[next].getTimeStamp() + fc.midiBufferOffset) * sampleRate));
        if (!stereo)
        {
            until = juce::jmin(until, done + 512);
            juce::FloatVectorOperations::clear(right, until - done);
        }
        for (auto& v : voices)
            if (v.active)
                render(v, out[0] + done, stereo ? out[1] + done : right, until - done);
        if (!stereo)
            for (int i = 0; i < until - done; ++i)
                out[0][done + i] = 0.5f * (out[0][done + i] + right[i]);
        done = until;
    }

    const float gain = juce::Decibels::decibelsToGain(volume->getCurrentValue());
    for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
        juce::FloatVectorOperations::multiply(out[ch], gain, fc.bufferNumSamples);
}

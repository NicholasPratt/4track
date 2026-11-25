#include "Samples.h"

namespace
{
    /** Replaced samples wait here until no voice holds them, then go, on the message thread. */
    class Graveyard : private juce::Timer, private juce::DeletedAtShutdown
    {
    public:
        static Graveyard& get()
        {
            static Graveyard* instance = new Graveyard();
            return *instance;
        }
        void bury(Sample::Ptr s)
        {
            if (s == nullptr)
                return;
            dead.push_back(std::move(s));
            if (!isTimerRunning())
                startTimer(500);
        }

    private:
        void timerCallback() override
        {
            dead.erase(std::remove_if(dead.begin(), dead.end(), [](auto& s) { return s->getReferenceCount() == 1; }), dead.end());
            if (dead.empty())
                stopTimer();
        }
        std::vector<Sample::Ptr> dead;
    };
}

Sample::Ptr Sample::load(const juce::File& file, juce::String& error, double maxSeconds)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = file.getFileName() + " isn't an audio file 8track can read.";
        return nullptr;
    }
    if (reader->lengthInSamples > (juce::int64) (reader->sampleRate * maxSeconds))
    {
        error = file.getFileName() + " is longer than " + juce::String(juce::roundToInt(maxSeconds)) + " seconds.";
        return nullptr;
    }
    Ptr s = new Sample();
    s->rate = reader->sampleRate;
    s->name = file.getFileNameWithoutExtension();
    s->audio.setSize(juce::jlimit(1, 2, (int) reader->numChannels), (int) reader->lengthInSamples);
    reader->read(&s->audio, 0, (int) reader->lengthInSamples, 0, true, s->audio.getNumChannels() > 1);
    return s;
}

Sample::Ptr Sample::fromBuffer(juce::AudioBuffer<float>&& b, double rate, const juce::String& name)
{
    Ptr s = new Sample();
    s->audio = std::move(b);
    s->rate = rate;
    s->name = name;
    return s;
}

std::vector<float> Sample::peaks(int count) const
{
    std::vector<float> p((size_t) juce::jmax(0, count), 0.0f);
    const int n = audio.getNumSamples();
    if (n == 0 || count <= 0)
        return p;
    for (int i = 0; i < count; ++i)
    {
        const int from = (int) ((juce::int64) n * i / count), to = juce::jmax(from + 1, (int) ((juce::int64) n * (i + 1) / count));
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            p[(size_t) i] = juce::jmax(p[(size_t) i], audio.getMagnitude(ch, from, juce::jmin(to, n) - from));
    }
    return p;
}

void SampleSlot::set(Sample::Ptr next)
{
    Sample::Ptr old;
    {
        const juce::SpinLock::ScopedLockType lock(spin);
        old = current;
        current = std::move(next);
    }
    Graveyard::get().bury(std::move(old));
}

//==============================================================================
InputRecorder::InputRecorder(te::DeviceManager& d) : devices(d)
{
    devices.addAudioInputObserver(this);
}

InputRecorder::~InputRecorder()
{
    devices.removeAudioInputObserver(this);
}

void InputRecorder::start(int firstChannel, int numChannels, double maxSeconds)
{
    {
        const juce::ScopedLock lock(devices.deviceManager.getAudioCallbackLock());
        first = firstChannel;
        count = juce::jlimit(1, 2, numChannels);
        fifo.reset();
        recording = true;
    }
    maxLength = (int) (maxSeconds * deviceRate.load());
    taken.setSize(count, maxLength, false, true);
    takenLength = 0;
    startTimerHz(30);
}

double InputRecorder::getSeconds() const
{
    return takenLength / deviceRate.load();
}

Sample::Ptr InputRecorder::stop(const juce::String& name)
{
    {
        const juce::ScopedLock lock(devices.deviceManager.getAudioCallbackLock());
        recording = false;
    }
    stopTimer();
    drain();
    level = 0.0f;

    // Trim the silence before the first sound (-50 dB), keeping 5 ms ahead of it.
    int start = 0;
    const float threshold = juce::Decibels::decibelsToGain(-50.0f);
    while (start < takenLength)
    {
        bool loud = false;
        for (int ch = 0; ch < taken.getNumChannels(); ++ch)
            loud = loud || std::abs(taken.getSample(ch, start)) > threshold;
        if (loud)
            break;
        ++start;
    }
    start = juce::jmax(0, start - (int) (0.005 * deviceRate.load()));
    const int length = takenLength - start;
    if (length <= 0)
        return nullptr;
    juce::AudioBuffer<float> out(taken.getNumChannels(), length);
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        out.copyFrom(ch, 0, taken, ch, start, length);
    taken.setSize(1, 1);
    takenLength = 0;
    return Sample::fromBuffer(std::move(out), deviceRate.load(), name);
}

void InputRecorder::onAudioInput(const float* const* input, int numInputs, int frames, double sampleRate)
{
    deviceRate = sampleRate;
    if (!recording.load())
        return;
    float peak = 0.0f;
    int s1, n1, s2, n2;
    fifo.prepareToWrite(frames, s1, n1, s2, n2);
    for (int ch = 0; ch < count; ++ch)
    {
        const int source = first + ch;
        const float* in = source < numInputs ? input[source] : nullptr;
        if (in == nullptr)
        {
            if (n1 > 0) ring.clear(ch, s1, n1);
            if (n2 > 0) ring.clear(ch, s2, n2);
            continue;
        }
        if (n1 > 0) ring.copyFrom(ch, s1, in, n1);
        if (n2 > 0) ring.copyFrom(ch, s2, in + n1, n2);
        peak = juce::jmax(peak, juce::FloatVectorOperations::findMaximum(in, frames),
                          -juce::FloatVectorOperations::findMinimum(in, frames));
    }
    fifo.finishedWrite(n1 + n2);
    level = juce::jmax(peak, level.load() * 0.9f);
}

void InputRecorder::drain()
{
    int s1, n1, s2, n2;
    const int ready = fifo.getNumReady();
    fifo.prepareToRead(ready, s1, n1, s2, n2);
    auto copy = [&](int from, int n)
    {
        n = juce::jmin(n, maxLength - takenLength);
        if (n <= 0)
            return;
        for (int ch = 0; ch < taken.getNumChannels(); ++ch)
            taken.copyFrom(ch, takenLength, ring, ch, from, n);
        takenLength += n;
    };
    copy(s1, n1);
    copy(s2, n2);
    fifo.finishedRead(n1 + n2);
}

void InputRecorder::timerCallback()
{
    drain();
    if (takenLength >= maxLength && recording.load())
    {
        stopTimer();
        if (onLimitReached)
            onLimitReached();
    }
}

#include "Drive.h"

namespace
{
    juce::String dbText(float v) { return juce::String(v, 1) + " dB"; }
    juce::String percentText(float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; }

    using Coefs = juce::dsp::IIR::Coefficients<float>;

    /** One cabinet: a few filters that give its low thump, scoop, presence and top roll-off. */
    struct CabStage { enum Kind { HighPass, LowPass, Peak } kind; float hz, q, db; };

    std::vector<CabStage> cabStages(int cab)
    {
        using K = CabStage::Kind;
        switch (cab)
        {
            case DriveEffect::OpenCombo:   // open back 1x12: loose lows, bright
                return { { K::HighPass, 75, 0.7f, 0 }, { K::Peak, 120, 1.0f, 3 }, { K::Peak, 500, 0.9f, -3 },
                         { K::Peak, 2200, 1.5f, 4 }, { K::LowPass, 5500, 0.7f, 0 }, { K::LowPass, 5500, 0.7f, 0 } };
            case DriveEffect::TwinCombo:   // 2x12: fuller, smoother top
                return { { K::HighPass, 70, 0.8f, 0 }, { K::Peak, 110, 1.0f, 2 }, { K::Peak, 1800, 1.2f, 3 },
                         { K::Peak, 3500, 2.0f, -2 }, { K::LowPass, 5000, 0.7f, 0 }, { K::LowPass, 5000, 0.7f, 0 } };
            case DriveEffect::ClosedStack: // closed back 4x12: tight thump, scooped, presence peak
                return { { K::HighPass, 85, 1.1f, 0 }, { K::Peak, 100, 1.0f, 4 }, { K::Peak, 400, 0.8f, -4 },
                         { K::Peak, 2500, 1.2f, 5 }, { K::LowPass, 4500, 0.7f, 0 }, { K::LowPass, 4500, 0.7f, 0 } };
            case DriveEffect::BassCab:     // 1x15: deep, dark
                return { { K::HighPass, 40, 0.7f, 0 }, { K::Peak, 80, 1.0f, 3 }, { K::Peak, 800, 0.9f, -3 },
                         { K::Peak, 1600, 1.5f, 2 }, { K::LowPass, 3000, 0.7f, 0 }, { K::LowPass, 3000, 0.7f, 0 } };
            default:
                return {};
        }
    }
}

const char* DriveEffect::xmlTypeName = "eightTrackDrive";

DriveEffect::DriveEffect(te::PluginCreationInfo info) : EffectPlugin(info)
{
    model = addChoice("model", "Model", modelNames(), Overdrive, "Drive");
    drive = addKnob("drive", "Drive", { 0.0f, 48.0f, 0.1f }, 18.0f, dbText, "Drive");
    bias  = addKnob("bias", "Bias", { -1.0f, 1.0f, 0.01f }, 0.2f, [](float v) { return juce::String(v, 2); }, "Drive");
    tone  = addKnob("tone", "Tone", { 0.0f, 1.0f, 0.01f }, 0.5f, percentText, "Drive");
    cab   = addChoice("cab", "Cabinet", cabNames(), ClosedStack, "Cabinet");
    level = addKnob("level", "Level", { -36.0f, 12.0f, 0.1f }, -12.0f, dbText, "Output");
    mix   = addKnob("mix", "Mix", { 0.0f, 1.0f, 0.01f }, 1.0f, percentText, "Output");
    applyImpulseResponse();
}

DriveEffect::~DriveEffect() { notifyListenersOfDeletion(); }

bool DriveEffect::isControlActive(const te::AutomatableParameter& p) const
{
    if (&p == bias)
        return choiceOf(*model) == Fuzz || choiceOf(*model) == Tube;
    return true;
}

float DriveEffect::shape(Model m, float x, float b)
{
    switch (m)
    {
        case Overdrive:   // soft and symmetric, like a pair of diodes in a feedback loop
            return std::tanh(x);
        case Distortion:  // harder: diodes to ground, flat-topped
        {
            const float y = x / std::pow(1.0f + std::pow(std::abs(x), 2.5f), 0.4f);
            return juce::jlimit(-1.0f, 1.0f, y);
        }
        case Fuzz:        // a starved transistor: lopsided, square, with a bias that sputters
        {
            const float offset = b * 0.6f;
            const float y = std::tanh(2.0f * (x + offset)) - std::tanh(2.0f * offset);
            return juce::jlimit(-1.0f, 1.0f, y > 0.0f ? y : y * (1.0f - 0.5f * std::abs(b)));
        }
        case Tube:        // a triode: rounds the top more than the bottom, so even harmonics
        {
            const float v = x + b * 0.3f;
            const float y = v >= 0.0f ? std::tanh(v) : (std::exp(v) - 1.0f) * 0.9f;
            const float zero = b * 0.3f >= 0.0f ? std::tanh(b * 0.3f) : (std::exp(b * 0.3f) - 1.0f) * 0.9f;
            return y - zero;
        }
    }
    return x;
}

void DriveEffect::initialise(const te::PluginInitialisationInfo& info)
{
    maxBlock = juce::jmax(512, info.blockSizeSamples);
    oversampling = std::make_unique<juce::dsp::Oversampling<float>>(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    oversampling->initProcessing((size_t) maxBlock);
    latencySamples = juce::roundToInt(oversampling->getLatencyInSamples());
    dry.setSize(2, latencySamples + maxBlock + 1);
    work.setSize(2, maxBlock);
    lows.setSize(2, maxBlock);
    convolution.prepare({ info.sampleRate, (juce::uint32) maxBlock, 2 });
    preparedRate = info.sampleRate;
    convolutionReady = true;
    cabNow = -1;
    reset();
}

void DriveEffect::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();
    convolution.reset();
    dry.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        preHigh[ch].reset();
        dcBlock[ch].reset();
        toneLow[ch][0].reset();
        toneLow[ch][1].reset();
        for (auto& f : cabFilters[ch])
            f.reset();
    }
}

void DriveEffect::updateCab(int cabType)
{
    if (cabType == cabNow)
        return;
    cabNow = cabType;
    const auto stages = cabStages(cabType);
    for (int i = 0; i < kCabStages; ++i)
    {
        Coefs::Ptr c;
        if (i < (int) stages.size())
        {
            const auto& st = stages[(size_t) i];
            c = st.kind == CabStage::HighPass ? Coefs::makeHighPass(sampleRate, st.hz, st.q)
              : st.kind == CabStage::LowPass  ? Coefs::makeLowPass(sampleRate, st.hz, st.q)
                                              : Coefs::makePeakFilter(sampleRate, st.hz, st.q, juce::Decibels::decibelsToGain(st.db));
        }
        else
        {
            c = new Coefs(1.0f, 0.0f, 1.0f, 0.0f);   // a wire
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            cabFilters[ch][i].coefficients = c;
            cabFilters[ch][i].reset();
        }
    }
}

void DriveEffect::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples == 0 || oversampling == nullptr)
        return;
    juce::ScopedNoDenormals noDenormals;
    auto& b = *fc.destBuffer;
    const bool stereo = b.getNumChannels() > 1;

    const auto m = (Model) choiceOf(*model);
    const float gain = juce::Decibels::decibelsToGain(drive->getCurrentValue());
    const float bi = bias->getCurrentValue();
    const float out = juce::Decibels::decibelsToGain(level->getCurrentValue());
    const float wet = mix->getCurrentValue(), dryGain = 1.0f - wet;
    const int cabType = choiceOf(*cab);
    updateCab(cabType);
    // Tone: a 12 dB low pass from 1.2 kHz (dark) to 12 kHz (bright), after the clipping.
    const double toneHz = 1200.0 * std::pow(10.0, (double) tone->getCurrentValue());
    // Overdrive and distortion clip mostly the mids, as their pedals do; fuzz and tube take it all.
    const double preHz = m == Overdrive ? 700.0 : m == Distortion ? 150.0 : 20.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        preHigh[ch].set(sampleRate, preHz, 0.5);
        dcBlock[ch].set(sampleRate, 10.0, 0.5);
        toneLow[ch][0].set(sampleRate, toneHz, 0.707);
        toneLow[ch][1].set(sampleRate, toneHz, 0.707);
    }

    const int dryLength = dry.getNumSamples();
    for (int start = 0; start < fc.bufferNumSamples; start += maxBlock)
    {
        const int n = juce::jmin(maxBlock, fc.bufferNumSamples - start);
        float* io[2] = { b.getWritePointer(0, fc.bufferStartSample + start),
                         b.getWritePointer(stereo ? 1 : 0, fc.bufferStartSample + start) };

        for (int ch = 0; ch < 2; ++ch)
            work.copyFrom(ch, 0, io[ch], n);

        // The clean lows overdrive keeps, before the clipping takes the rest.
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const float x = work.getSample(ch, i);
                const float high = preHigh[ch].process(x, EffectSvf::HighPass);
                lows.setSample(ch, i, m == Overdrive ? (x - high) * 0.5f : 0.0f);
                work.setSample(ch, i, high);
            }

        auto block = juce::dsp::AudioBlock<float>(work).getSubBlock(0, (size_t) n);
        auto up = oversampling->processSamplesUp(block);
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            float* d = up.getChannelPointer(ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                d[i] = shape(m, d[i] * gain, bi);
        }
        oversampling->processSamplesDown(block);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = work.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
            {
                float y = d[i] + lows.getSample(ch, i);
                y = dcBlock[ch].process(y, EffectSvf::HighPass);
                y = toneLow[ch][0].process(y, EffectSvf::LowPass);
                d[i] = toneLow[ch][1].process(y, EffectSvf::LowPass);
            }
        }

        if (cabType == ImpulseResponse)
        {
            if (convolutionReady && convolution.getCurrentIRSize() > 0)
            {
                auto cabBlock = juce::dsp::AudioBlock<float>(work).getSubBlock(0, (size_t) n);
                convolution.process(juce::dsp::ProcessContextReplacing<float>(cabBlock));
            }
        }
        else if (cabType != NoCab)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                float* d = work.getWritePointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    float y = d[i];
                    for (auto& f : cabFilters[ch])
                        y = f.processSample(y);
                    d[i] = y;
                }
            }
        }

        // Mix with the input, delayed by the oversampling's latency.
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
        {
            float* line = dry.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
            {
                const int pos = (dryPos + i) % dryLength;
                const int readPos = (pos - latencySamples + dryLength) % dryLength;
                line[pos] = io[ch][i];
                io[ch][i] = dryGain * line[readPos] + wet * out * work.getSample(ch, i);
            }
        }
        dryPos = (dryPos + n) % dryLength;
    }
}

bool DriveEffect::loadImpulseResponse(const juce::File& file, juce::String& error)
{
    if (file == juce::File())
    {
        edit.getUndoManager().beginNewTransaction("Clear impulse response");
        setFileProperty(irProperty, {});
        applyImpulseResponse();
        return true;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "It isn't an audio file 8track can read.";
        return false;
    }
    if (reader->lengthInSamples > (juce::int64) (reader->sampleRate * 10.0))
    {
        error = "Impulse responses can be up to 10 seconds long.";
        return false;
    }
    edit.getUndoManager().beginNewTransaction("Load impulse response " + file.getFileName());
    setFileProperty(irProperty, adoptFile(file));
    cab->setParameter((float) ImpulseResponse, juce::sendNotification);
    applyImpulseResponse();
    return true;
}

void DriveEffect::applyImpulseResponse()
{
    const auto file = getImpulseResponse();
    if (file.existsAsFile())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        const bool isStereo = reader != nullptr && reader->numChannels > 1;
        convolution.loadImpulseResponse(file, isStereo ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
                                        juce::dsp::Convolution::Trim::yes, 0, juce::dsp::Convolution::Normalise::yes);
    }
    else
    {
        // An empty IR: a one-sample impulse (a wire)
        juce::AudioBuffer<float> wire(1, 1);
        wire.setSample(0, 0, 1.0f);
        convolution.loadImpulseResponse(std::move(wire), preparedRate > 0 ? preparedRate : 48000.0,
                                        juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                                        juce::dsp::Convolution::Normalise::no);
    }
}

std::vector<EffectPlugin::FileSlot> DriveEffect::getFileSlots()
{
    FileSlot ir;
    ir.name = "Impulse Response";
    ir.row = "Cabinet";
    ir.wildcard = "*.wav;*.aif;*.aiff;*.flac";
    ir.get = [this] { return getImpulseResponse(); };
    ir.set = [this](const juce::File& f, juce::String& error) { return loadImpulseResponse(f, error); };
    return { ir };
}

void DriveEffect::getFileReferences(std::vector<FileReference>& refs)
{
    refs.push_back({ state, irProperty });
}

void DriveEffect::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    EffectPlugin::restorePluginStateFromValueTree(v);
    state.setProperty(irProperty, v[irProperty], nullptr);
    applyImpulseResponse();
}

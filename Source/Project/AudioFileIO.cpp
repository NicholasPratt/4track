#include "AudioFileIO.h"

namespace
{
    juce::AudioBuffer<float> resampleBuffer(const juce::AudioBuffer<float>& in, double inRate, double outRate)
    {
        const double speed = inRate / outRate;   // input samples consumed per output sample
        const int inLen = in.getNumSamples();
        const int outLen = (int) std::ceil((double) inLen / speed);

        // Pad so the interpolator has look-ahead past the end.
        constexpr int pad = 256;
        juce::AudioBuffer<float> padded(in.getNumChannels(), inLen + pad);
        padded.clear();
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            padded.copyFrom(ch, 0, in, ch, 0, inLen);

        juce::AudioBuffer<float> out(in.getNumChannels(), outLen);
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            juce::WindowedSincInterpolator interp;
            interp.process(speed, padded.getReadPointer(ch), out.getWritePointer(ch), outLen);
        }
        return out;
    }
}

namespace AudioFileIO
{
std::shared_ptr<AudioSourceData> loadSource(const juce::File& file, double targetRate, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "Can't read " + file.getFileName() + " (unsupported or missing file)";
        return nullptr;
    }

    if (reader->lengthInSamples > std::numeric_limits<int>::max() / 2)
    {
        error = file.getFileName() + " is too long";
        return nullptr;
    }

    const int len = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> audio(2, len);
    audio.clear();
    reader->read(&audio, 0, len, 0, true, true);
    if (reader->numChannels == 1)
        audio.copyFrom(1, 0, audio, 0, 0, len);

    auto source = std::make_shared<AudioSourceData>();
    source->file = file;

    if (std::abs(reader->sampleRate - targetRate) > 0.5 && reader->sampleRate > 0)
        source->audio = resampleBuffer(audio, reader->sampleRate, targetRate);
    else
        source->audio = std::move(audio);

    source->buildPeaks();
    return source;
}

std::shared_ptr<AudioSourceData> resampled(const AudioSourceData& source, double inRate, double outRate)
{
    auto copy = std::make_shared<AudioSourceData>();
    copy->file = source.file;
    copy->audio = resampleBuffer(source.audio, inRate, outRate);
    copy->buildPeaks();
    return copy;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate,
              int bitsPerSample, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
    {
        error = "Couldn't write to " + file.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                  .withSampleRate(sampleRate)
                                                  .withNumChannels(audio.getNumChannels())
                                                  .withBitsPerSample(bitsPerSample));
    if (writer == nullptr)
    {
        error = "Couldn't create a WAV writer for " + file.getFileName();
        return false;
    }

    if (!writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()))
    {
        error = "Writing " + file.getFileName() + " failed (disk full?)";
        return false;
    }
    return true;
}
}

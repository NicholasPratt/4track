#include "Mp3Encoder.h"

#if defined(EIGHTTRACK_HAS_LAME)
 #include <lame/lame.h>
 #include <array>
#endif

bool Mp3Encoder::isAvailable()
{
   #if defined(EIGHTTRACK_HAS_LAME)
    return true;
   #else
    return false;
   #endif
}

bool Mp3Encoder::encode([[maybe_unused]] const juce::File& wav, [[maybe_unused]] const juce::File& output,
                        [[maybe_unused]] int bitrateKbps)
{
   #if defined(EIGHTTRACK_HAS_LAME)
    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        juce::WavAudioFormat().createReaderFor(wav.createInputStream().release(), true));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels != 2)
        return false;

    const std::unique_ptr<lame_global_flags, decltype(&lame_close)> encoder(lame_init(), lame_close);
    if (encoder == nullptr)
        return false;

    auto* lame = encoder.get();
    const auto sampleRate = juce::roundToInt(reader->sampleRate);
    if (lame_set_num_channels(lame, 2) < 0
        || lame_set_in_samplerate(lame, sampleRate) < 0
        || lame_set_out_samplerate(lame, sampleRate) < 0
        || lame_set_VBR(lame, vbr_off) < 0
        || lame_set_brate(lame, bitrateKbps) < 0
        || lame_init_params(lame) < 0)
        return false;

    auto stream = output.createOutputStream();
    if (stream == nullptr)
        return false;

    constexpr int blockSize = 4096;
    juce::AudioBuffer<float> pcm(2, blockSize);
    // LAME requires 1.25 * block size + 7200 bytes of output space.
    std::array<unsigned char, blockSize * 5 / 4 + 7200> bytes;
    const auto capacity = (int) bytes.size();
    const auto write = [&](int count)
    {
        return count >= 0 && (count == 0 || stream->write(bytes.data(), (size_t) count));
    };

    for (juce::int64 position = 0; position < reader->lengthInSamples; position += blockSize)
    {
        const auto samples = (int) juce::jmin<juce::int64>(blockSize, reader->lengthInSamples - position);
        if (!reader->read(&pcm, 0, samples, position, true, true)
            || !write(lame_encode_buffer_ieee_float(lame, pcm.getReadPointer(0), pcm.getReadPointer(1),
                                                    samples, bytes.data(), capacity)))
            return false;
    }

    if (!write(lame_encode_flush(lame, bytes.data(), capacity)))
        return false;

    // Replace the placeholder frame with the final tag carrying encoder delay and padding.
    const auto tagSize = lame_get_lametag_frame(lame, bytes.data(), bytes.size());
    if (tagSize > 0 && (tagSize > bytes.size() || !stream->setPosition(0) || !stream->write(bytes.data(), tagSize)))
        return false;

    stream->flush();
    return stream->getStatus().wasOk();
   #else
    return false;
   #endif
}

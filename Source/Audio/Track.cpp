#include "Track.h"

Track::Track(int trackNum)
    : trackNumber(trackNum),
      trackName("Track " + juce::String(trackNum + 1))
{
    // Initialize with 2 channels (stereo), 10 seconds at 44.1kHz
    recordedAudio.setSize(2, 441000);
    recordedAudio.clear();

    // Initialize write buffer
    writeBuffer.setSize(2, WRITE_BUFFER_SIZE);
    writeBuffer.clear();
}

Track::~Track()
{
    stopRecordingToFile();
}

void Track::processBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    float currentPeak = 0.0f;

    // If no recorded audio, nothing to do (use cached flag for thread safety)
    if (!hasAudio.load())
    {
        peakLevel.store(0.0f);
        return;
    }

    // Update filters if parameters changed
    if (filtersNeedUpdate.load())
    {
        updateFilters();
    }

    // Get cached buffer length (thread-safe)
    const int bufferLength = cachedBufferLength;

    // Debug: Log playback activity (once every ~2 seconds)
    static int playbackDebugCounter = 0;
    if (++playbackDebugCounter % 88200 == 0 && bufferLength > 0 && playbackPosition >= 0 && playbackPosition < bufferLength)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Playing at pos=" + juce::String(playbackPosition) +
                                 ", bufferLen=" + juce::String(bufferLength) +
                                 ", muted=" + juce::String(muted ? "YES" : "NO"));
    }

    // If muted, don't output anything but still calculate peak for meters
    const int numChannels = juce::jmin(outputBuffer.getNumChannels(), recordedAudio.getNumChannels());
    const float vol = volume.load();
    const float panValue = pan.load();

    // Calculate left and right gains based on pan
    // Pan: -1.0 = full left, 0.0 = center, 1.0 = full right
    const float leftGain = vol * (1.0f - juce::jmax(0.0f, panValue));
    const float rightGain = vol * (1.0f + juce::jmin(0.0f, panValue));

    // Calculate how many samples we can actually read
    const int availableSamples = juce::jmax(0, bufferLength - playbackPosition);
    const int samplesToRead = juce::jmin(numSamples, availableSamples);

    // Only process if we have samples to read and position is valid
    if (samplesToRead <= 0 || playbackPosition < 0 || playbackPosition >= bufferLength)
    {
        peakLevel.store(0.0f);
        return;
    }

    // Get EQ parameters for bypass checks
    const float lcFreq = lowCutFreq.load();
    const float hcFreq = highCutFreq.load();
    const float midGain = midEqGain.load();
    const bool useLowCut = lcFreq > 20.1f;
    const bool useHighCut = hcFreq < 19999.0f;
    const bool useMidEq = std::abs(midGain) > 0.1f;

    for (int sample = 0; sample < samplesToRead; ++sample)
    {
        const int readPos = playbackPosition + sample;

        float leftSample = 0.0f;
        float rightSample = 0.0f;

        // Read samples from recorded audio
        if (numChannels >= 1 && recordedAudio.getNumChannels() >= 1)
        {
            leftSample = recordedAudio.getSample(0, readPos);
        }

        if (numChannels >= 2 && recordedAudio.getNumChannels() >= 2)
        {
            rightSample = recordedAudio.getSample(1, readPos);
        }

        // Apply EQ filters (before volume/pan)
        if (useLowCut)
        {
            leftSample = lowCutFilterL.processSingleSampleRaw(leftSample);
            rightSample = lowCutFilterR.processSingleSampleRaw(rightSample);
        }

        if (useMidEq)
        {
            leftSample = midEqFilterL.processSingleSampleRaw(leftSample);
            rightSample = midEqFilterR.processSingleSampleRaw(rightSample);
        }

        if (useHighCut)
        {
            leftSample = highCutFilterL.processSingleSampleRaw(leftSample);
            rightSample = highCutFilterR.processSingleSampleRaw(rightSample);
        }

        // Calculate peak after EQ but before volume
        currentPeak = juce::jmax(currentPeak, std::abs(leftSample * leftGain));
        currentPeak = juce::jmax(currentPeak, std::abs(rightSample * rightGain));

        // Add to output (mixing with other tracks) if not muted
        if (!muted)
        {
            if (numChannels >= 1)
            {
                outputBuffer.addSample(0, startSample + sample, leftSample * leftGain);
            }

            if (numChannels >= 2)
            {
                outputBuffer.addSample(1, startSample + sample, rightSample * rightGain);
            }
        }
    }

    // Update peak level (with decay)
    // Only update if we actually have audio to show, otherwise during recording
    // the input monitoring peak level would be overwritten
    if (currentPeak > 0.0001f)
    {
        float oldPeak = peakLevel.load();
        peakLevel.store(juce::jmax(currentPeak, oldPeak * 0.95f));
    }
    else
    {
        // Just apply decay if no playback audio
        float oldPeak = peakLevel.load();
        peakLevel.store(oldPeak * 0.98f); // Slower decay when not playing
    }
}

void Track::addInputSample(float leftSample, float rightSample, int position)
{
    // Only record if armed
    if (!recordArmed)
        return;

    // Clamp input to safe range (-1.0 to 1.0)
    leftSample = juce::jlimit(-1.0f, 1.0f, leftSample);
    rightSample = juce::jlimit(-1.0f, 1.0f, rightSample);

    // Update peak level for input monitoring
    float inputPeak = juce::jmax(std::abs(leftSample), std::abs(rightSample));
    float oldPeak = peakLevel.load();
    peakLevel.store(juce::jmax(inputPeak, oldPeak * 0.95f));

    // Debug logging (once every ~2 seconds at 44.1kHz)
    static int debugCounter = 0;
    if (++debugCounter % 88200 == 0)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Recording at pos=" + juce::String(position) +
                                 ", peak=" + juce::String(inputPeak, 3) +
                                 ", toFile=" + juce::String(isRecordingToFile() ? "YES" : "NO"));
    }

    // Write to file if recording to file - use buffered writing for efficiency
    if (activeWriter != nullptr)
    {
        const juce::ScopedLock sl(writerLock);
        if (activeWriter != nullptr)
        {
            // Add samples to write buffer
            writeBuffer.setSample(0, writeBufferPosition, leftSample);
            writeBuffer.setSample(1, writeBufferPosition, rightSample);
            writeBufferPosition++;

            // Flush buffer when full
            if (writeBufferPosition >= WRITE_BUFFER_SIZE)
            {
                flushWriteBuffer();
            }
        }
    }

    // Also write to memory buffer for monitoring
    if (position >= recordedAudio.getNumSamples())
    {
        ensureBufferSize(position + 44100); // Add 1 second buffer
    }

    if (position >= 0 && position < recordedAudio.getNumSamples())
    {
        if (recordedAudio.getNumChannels() >= 1)
            recordedAudio.setSample(0, position, leftSample);

        if (recordedAudio.getNumChannels() >= 2)
            recordedAudio.setSample(1, position, rightSample);

        // Update cached length and flag as we record
        if (position + 1 > cachedBufferLength)
        {
            cachedBufferLength = position + 1;
            hasAudio.store(true);
        }
    }
}

void Track::addToReverbSend(juce::AudioBuffer<float>& sendBuffer, int startSample, int numSamples, float sendLevel)
{
    if (!hasAudio.load() || sendLevel < 0.001f)
        return;

    const int bufferLength = cachedBufferLength;
    const float vol = volume.load();
    const float panValue = pan.load();

    // Calculate left and right gains (same as processBlock) scaled by send level
    const float leftGain = vol * sendLevel * (1.0f - juce::jmax(0.0f, panValue));
    const float rightGain = vol * sendLevel * (1.0f + juce::jmin(0.0f, panValue));

    const int availableSamples = juce::jmax(0, bufferLength - playbackPosition);
    const int samplesToRead = juce::jmin(numSamples, availableSamples);

    if (samplesToRead <= 0 || playbackPosition < 0 || playbackPosition >= bufferLength)
        return;

    // Don't add to reverb send if muted
    if (muted.load())
        return;

    // Get EQ parameters for bypass checks (reverb send uses same EQ as direct output)
    const float lcFreq = lowCutFreq.load();
    const float hcFreq = highCutFreq.load();
    const float midGainDb = midEqGain.load();
    const bool useLowCut = lcFreq > 20.1f;
    const bool useHighCut = hcFreq < 19999.0f;
    const bool useMidEq = std::abs(midGainDb) > 0.1f;

    for (int sample = 0; sample < samplesToRead; ++sample)
    {
        const int readPos = playbackPosition + sample;

        float leftSample = 0.0f;
        float rightSample = 0.0f;

        if (recordedAudio.getNumChannels() >= 1)
        {
            leftSample = recordedAudio.getSample(0, readPos);
        }

        if (recordedAudio.getNumChannels() >= 2)
        {
            rightSample = recordedAudio.getSample(1, readPos);
        }

        // Note: EQ filters are already applied in processBlock which runs first
        // The reverb send gets the same signal path (EQ is applied there)
        // We just apply volume/pan/send level here

        sendBuffer.addSample(0, startSample + sample, leftSample * leftGain);
        sendBuffer.addSample(1, startSample + sample, rightSample * rightGain);
    }
}

void Track::updateInputMeter(float inputPeak)
{
    float oldPeak = peakLevel.load();
    peakLevel.store(juce::jmax(inputPeak, oldPeak * 0.95f));
}

void Track::clearBuffer()
{
    recordedAudio.clear();
    playbackPosition = 0;
    hasAudio.store(false);
    cachedBufferLength = 0;
    peakLevel.store(0.0f);
}

void Track::ensureBufferSize(int numSamples)
{
    if (numSamples > recordedAudio.getNumSamples())
    {
        juce::AudioBuffer<float> newBuffer(recordedAudio.getNumChannels(), numSamples);
        newBuffer.clear();

        // Copy existing data
        for (int ch = 0; ch < recordedAudio.getNumChannels(); ++ch)
        {
            newBuffer.copyFrom(ch, 0, recordedAudio, ch, 0, recordedAudio.getNumSamples());
        }

        recordedAudio = std::move(newBuffer);
    }
}

void Track::loadAudioData(const juce::AudioBuffer<float>& sourceBuffer)
{
    const int numSamples = sourceBuffer.getNumSamples();
    const int numChannels = juce::jmin(2, sourceBuffer.getNumChannels());

    // Ensure our buffer is large enough
    ensureBufferSize(numSamples);

    // Copy audio data directly to our buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        recordedAudio.copyFrom(ch, 0, sourceBuffer, ch, 0, numSamples);
    }

    // If source is mono and we have stereo, copy to both channels
    if (sourceBuffer.getNumChannels() == 1 && recordedAudio.getNumChannels() >= 2)
    {
        recordedAudio.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }

    playbackPosition = 0;

    // Update cached values
    cachedBufferLength = numSamples;
    hasAudio.store(numSamples > 0);
}

bool Track::saveToFile(const juce::File& file, double sampleRate)
{
    if (!hasRecordedAudio())
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) + ": No audio to save");
        return false;
    }

    // Delete existing file
    if (file.exists())
        file.deleteFile();

    // Create WAV format writer (use member variable wavFormat)
    std::unique_ptr<juce::FileOutputStream> fileStream(file.createOutputStream());

    if (fileStream == nullptr)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) + ": Failed to create file stream");
        return false;
    }

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(fileStream.get(),
                                 sampleRate,
                                 recordedAudio.getNumChannels(),
                                 24, // 24-bit
                                 {},
                                 0));

    if (writer == nullptr)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) + ": Failed to create WAV writer");
        return false;
    }

    fileStream.release(); // Writer takes ownership

    // Find the actual end of recorded audio (last non-zero sample)
    int actualLength = recordedAudio.getNumSamples();
    for (int i = recordedAudio.getNumSamples() - 1; i >= 0; --i)
    {
        bool hasAudio = false;
        for (int ch = 0; ch < recordedAudio.getNumChannels(); ++ch)
        {
            if (std::abs(recordedAudio.getSample(ch, i)) > 0.0001f)
            {
                hasAudio = true;
                break;
            }
        }
        if (hasAudio)
        {
            actualLength = i + 1;
            break;
        }
    }

    // Write audio data
    writer->writeFromAudioSampleBuffer(recordedAudio, 0, actualLength);

    juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                             ": Saved " + juce::String(actualLength) + " samples to " + file.getFileName());

    return true;
}

bool Track::loadFromFile(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) + ": File does not exist: " + file.getFullPathName());
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) + ": Failed to create reader for " + file.getFileName());
        return false;
    }

    // Resize buffer to fit the audio file
    int numSamples = static_cast<int>(reader->lengthInSamples);
    recordedAudio.setSize(2, numSamples, false, true, false);

    // Read audio data
    reader->read(&recordedAudio, 0, numSamples, 0, true, true);

    juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                             ": Loaded " + juce::String(numSamples) + " samples from " + file.getFileName());

    playbackPosition = 0;

    // Update cached values (thread-safe)
    cachedBufferLength = numSamples;
    hasAudio.store(numSamples > 0);

    return true;
}

bool Track::hasRecordedAudio() const
{
    // Use cached atomic flag for thread-safety and performance
    return hasAudio.load();
}

void Track::startRecordingToFile(const juce::File& file, double sampleRate)
{
    stopRecordingToFile(); // Stop any existing recording

    const juce::ScopedLock sl(writerLock);

    // Delete existing file
    if (file.exists())
        file.deleteFile();

    // Create directory if needed
    file.getParentDirectory().createDirectory();

    // Create file stream
    std::unique_ptr<juce::FileOutputStream> fileStream(file.createOutputStream());

    if (fileStream == nullptr)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Failed to create file stream for " + file.getFullPathName());
        return;
    }

    // Create WAV writer
    activeWriter.reset(wavFormat.createWriterFor(fileStream.get(),
                                                  sampleRate,
                                                  2, // stereo
                                                  24, // 24-bit
                                                  {},
                                                  0));

    if (activeWriter == nullptr)
    {
        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Failed to create WAV writer");
        return;
    }

    fileStream.release(); // Writer takes ownership
    currentRecordingFile = file;

    juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                             ": Started recording to " + file.getFullPathName());
}

void Track::stopRecordingToFile()
{
    const juce::ScopedLock sl(writerLock);

    if (activeWriter != nullptr)
    {
        // Flush any remaining buffered samples
        if (writeBufferPosition > 0)
        {
            flushWriteBuffer();
        }

        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Stopped recording to " + currentRecordingFile.getFullPathName());

        activeWriter->flush();
        activeWriter.reset();

        // Load the recorded file back into memory for playback
        if (currentRecordingFile.existsAsFile())
        {
            loadFromFile(currentRecordingFile);
        }

        currentRecordingFile = juce::File();
        writeBufferPosition = 0;
    }
}

void Track::stopRecordingToFileForPunch()
{
    const juce::ScopedLock sl(writerLock);

    if (activeWriter != nullptr)
    {
        if (writeBufferPosition > 0)
            flushWriteBuffer();

        juce::Logger::writeToLog("Track " + juce::String(trackNumber + 1) +
                                 ": Punch out - flushing writer, keeping memory buffer");

        activeWriter->flush();
        activeWriter.reset();
        currentRecordingFile = juce::File();
        writeBufferPosition = 0;
        // Memory buffer already has the complete track (pre-punch + punch),
        // so we deliberately do NOT call loadFromFile here.
    }
}

void Track::flushWriteBuffer()
{
    if (activeWriter != nullptr && writeBufferPosition > 0)
    {
        activeWriter->writeFromAudioSampleBuffer(writeBuffer, 0, writeBufferPosition);
        writeBufferPosition = 0;
    }
}

void Track::updateFilters()
{
    // Low cut (high-pass filter)
    float lcFreq = lowCutFreq.load();
    if (lcFreq > 20.1f)  // Only create filter if not at minimum
    {
        auto lcCoeffs = juce::IIRCoefficients::makeHighPass(sampleRate, lcFreq);
        lowCutFilterL.setCoefficients(lcCoeffs);
        lowCutFilterR.setCoefficients(lcCoeffs);
    }

    // High cut (low-pass filter)
    float hcFreq = highCutFreq.load();
    if (hcFreq < 19999.0f)  // Only create filter if not at maximum
    {
        auto hcCoeffs = juce::IIRCoefficients::makeLowPass(sampleRate, hcFreq);
        highCutFilterL.setCoefficients(hcCoeffs);
        highCutFilterR.setCoefficients(hcCoeffs);
    }

    // Mid EQ (peaking filter)
    float midFreq = midEqFreq.load();
    float midGain = midEqGain.load();
    if (std::abs(midGain) > 0.1f)  // Only create filter if there's boost/cut
    {
        // Q of 1.0 gives a moderate bandwidth
        auto midCoeffs = juce::IIRCoefficients::makePeakFilter(sampleRate, midFreq, 1.0, juce::Decibels::decibelsToGain(midGain));
        midEqFilterL.setCoefficients(midCoeffs);
        midEqFilterR.setCoefficients(midCoeffs);
    }

    filtersNeedUpdate.store(false);
}

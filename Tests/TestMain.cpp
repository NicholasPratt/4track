#include <juce_core/juce_core.h>
#include "../Source/Model/Arrangement.h"
#include "../Source/Audio/MixEngine.h"
#include "../Source/Project/AudioFileIO.h"
#include "../Source/Audio/AudioEngine.h"
#include "../Source/Project/Project.h"

namespace
{
    AudioSourcePtr makeSource(int length, float value)
    {
        auto s = std::make_shared<AudioSourceData>();
        s->audio.setSize(2, length);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill(s->audio.getWritePointer(ch), value, length);
        s->buildPeaks();
        return s;
    }

    /** A source whose sample value is its own index, so offsets are easy to check. */
    AudioSourcePtr makeRamp(int length)
    {
        auto s = std::make_shared<AudioSourceData>();
        s->audio.setSize(2, length);
        for (int i = 0; i < length; ++i)
            for (int ch = 0; ch < 2; ++ch)
                s->audio.setSample(ch, i, (float) i);
        return s;
    }

    Clip clip(AudioSourcePtr src, juce::int64 start, juce::int64 length, juce::int64 offset = 0)
    {
        Clip c;
        c.source = std::move(src);
        c.start = start;
        c.length = length;
        c.sourceOffset = offset;
        return c;
    }

    struct Mixer
    {
        std::array<TrackParams, kNumTracks> tracks;
        MasterParams master;
        MixEngine::ParamArray params {};
        Mixer() { for (int i = 0; i < kNumTracks; ++i) params[(size_t) i] = &tracks[(size_t) i]; }
    };
}

class ArrangementTests : public juce::UnitTest
{
public:
    ArrangementTests() : juce::UnitTest("Arrangement", "8track") {}

    void runTest() override
    {
        beginTest("Recording over the middle of a clip splits it around the new take");
        {
            Arrangement a;
            auto ramp = makeRamp(10000);
            const auto original = a.insertClip(0, clip(ramp, 0, 10000));
            a.insertClip(0, clip(makeSource(2000, 0.5f), 4000, 2000));

            auto& lane = a.tracks[0];
            expectEquals((int) lane.size(), 3);
            expectEquals(lane[0].id, original);
            expectEquals(lane[0].end(), (juce::int64) 4000);
            expectEquals(lane[1].start, (juce::int64) 4000);
            expectEquals(lane[2].start, (juce::int64) 6000);
            expectEquals(lane[2].sourceOffset, (juce::int64) 6000, "right part keeps its place in the source");
            expectEquals(lane[2].end(), (juce::int64) 10000);
        }

        beginTest("Recording over the whole of a shorter clip removes it");
        {
            Arrangement a;
            a.insertClip(0, clip(makeSource(1000, 0.1f), 500, 1000));
            a.insertClip(0, clip(makeSource(5000, 0.2f), 0, 5000));
            expectEquals((int) a.tracks[0].size(), 1);
        }

        beginTest("Recording over the start of a longer clip keeps its tail (the old 4track lost it)");
        {
            Arrangement a;
            a.insertClip(0, clip(makeRamp(60000), 0, 60000));
            a.insertClip(0, clip(makeSource(20000, 0.5f), 0, 20000));
            expectEquals((int) a.tracks[0].size(), 2);
            expectEquals(a.tracks[0][1].start, (juce::int64) 20000);
            expectEquals(a.tracks[0][1].end(), (juce::int64) 60000);
        }

        beginTest("Split keeps the audio continuous");
        {
            Arrangement a;
            const auto id = a.insertClip(1, clip(makeRamp(8000), 1000, 8000));
            expect(a.splitClip(id, 3000));
            auto& lane = a.tracks[1];
            expectEquals((int) lane.size(), 2);
            expectEquals(lane[0].end(), (juce::int64) 3000);
            expectEquals(lane[1].start, (juce::int64) 3000);
            expectEquals(lane[1].sourceOffset, (juce::int64) 2000);
            expectEquals(lane[0].fadeOut, (juce::int64) 0);
            expectEquals(lane[1].fadeIn, (juce::int64) 0);
        }

        beginTest("Trims are limited by the source and by neighbouring clips");
        {
            Arrangement a;
            const auto left = a.insertClip(0, clip(makeRamp(4000), 0, 2000));
            const auto right = a.insertClip(0, clip(makeRamp(4000), 3000, 2000, 1000));

            a.trimEnd(left, 10000);
            expectEquals(a.findClip(left)->end(), (juce::int64) 3000, "stops at the next clip");

            a.trimStart(right, -5000);
            expectEquals(a.findClip(right)->start, (juce::int64) 3000, "next to the left clip");

            a.removeClip(left);
            a.trimStart(right, -5000);
            expectEquals(a.findClip(right)->start, (juce::int64) 2000, "can't reveal audio before the source starts");
            expectEquals(a.findClip(right)->sourceOffset, (juce::int64) 0);
        }

        beginTest("Moving a clip onto another track trims what's underneath");
        {
            Arrangement a;
            const auto moving = a.insertClip(0, clip(makeSource(1000, 0.1f), 0, 1000));
            a.insertClip(1, clip(makeSource(5000, 0.2f), 0, 5000));
            expect(a.moveClip(moving, 1, 2000));
            expect(a.tracks[0].empty());
            expectEquals((int) a.tracks[1].size(), 3);
        }

        beginTest("Undo and redo restore exact snapshots");
        {
            Arrangement a;
            EditHistory h;
            const auto id = a.insertClip(0, clip(makeRamp(5000), 0, 5000));

            h.push(a);
            a.splitClip(id, 2500);
            expectEquals((int) a.tracks[0].size(), 2);

            expect(h.undo(a));
            expectEquals((int) a.tracks[0].size(), 1);
            expectEquals(a.tracks[0][0].length, (juce::int64) 5000);

            expect(h.redo(a));
            expectEquals((int) a.tracks[0].size(), 2);
        }

        beginTest("Fades never exceed the clip");
        {
            Arrangement a;
            const auto id = a.insertClip(0, clip(makeSource(1000, 1.0f), 0, 1000));
            a.setFades(id, 800, 800);
            auto* c = a.findClip(id);
            expect(c->fadeIn + c->fadeOut <= c->length);
        }
    }
};

class MixEngineTests : public juce::UnitTest
{
public:
    MixEngineTests() : juce::UnitTest("MixEngine", "8track") {}

    void runTest() override
    {
        beginTest("A clip plays at its timeline position with its fades");
        {
            Mixer m;
            Arrangement a;
            auto c = clip(makeSource(1000, 0.5f), 1000, 1000);
            c.fadeIn = 100;
            a.insertClip(0, c);

            auto out = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 0.0);
            expectEquals(out.getNumSamples(), 2000);
            expectWithinAbsoluteError(out.getSample(0, 999), 0.0f, 1e-6f);
            expectWithinAbsoluteError(out.getSample(0, 1000), 0.0f, 1e-6f);
            expectWithinAbsoluteError(out.getSample(0, 1050), 0.25f, 0.01f);
            expectWithinAbsoluteError(out.getSample(0, 1500), 0.5f, 1e-4f);
            expectWithinAbsoluteError(out.getSample(1, 1500), 0.5f, 1e-4f);
        }

        beginTest("Mute and solo");
        {
            Mixer m;
            Arrangement a;
            a.insertClip(0, clip(makeSource(500, 0.25f), 0, 500));
            a.insertClip(1, clip(makeSource(500, 0.5f), 0, 500));

            m.tracks[1].mute = true;
            auto out = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 0.0);
            expectWithinAbsoluteError(out.getSample(0, 300), 0.25f, 1e-4f);

            m.tracks[1].mute = false;
            m.tracks[1].solo = true;
            out = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 0.0);
            expectWithinAbsoluteError(out.getSample(0, 300), 0.5f, 1e-4f);
        }

        beginTest("Hard pan and volume");
        {
            Mixer m;
            Arrangement a;
            a.insertClip(0, clip(makeSource(500, 0.5f), 0, 500));
            m.tracks[0].pan = -1.0f;
            m.tracks[0].volumeDb = -6.0206f;
            auto out = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 0.0);
            expectWithinAbsoluteError(out.getSample(0, 300), 0.25f, 1e-3f);
            expectWithinAbsoluteError(out.getSample(1, 300), 0.0f, 1e-6f);
        }

        beginTest("Blocks bigger than the prepared size give identical output");
        {
            Mixer m;
            Arrangement a;
            a.insertClip(0, clip(makeRamp(3000), 100, 2500));

            MixEngine small, big;
            small.prepare(48000.0, 64);
            big.prepare(48000.0, 4096);

            juce::AudioBuffer<float> o1(2, 3000), o2(2, 3000);
            small.process(&a, m.params, m.master, 0, 3000, true, nullptr, o1.getWritePointer(0), o1.getWritePointer(1), false);
            big.process(&a, m.params, m.master, 0, 3000, true, nullptr, o2.getWritePointer(0), o2.getWritePointer(1), false);

            float maxDiff = 0.0f;
            for (int i = 0; i < 3000; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(o1.getSample(0, i) - o2.getSample(0, i)));
            expectWithinAbsoluteError(maxDiff, 0.0f, 1e-4f);
        }

        beginTest("Reverb send uses the EQ'd signal and its tail is rendered");
        {
            Mixer m;
            Arrangement a;
            a.insertClip(0, clip(makeSource(4800, 0.5f), 0, 4800));
            m.tracks[0].send = 1.0f;
            m.tracks[0].volumeDb = -60.0f;   // direct signal off, so only the reverb remains...
            auto silentDry = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 1.0);
            expectEquals(silentDry.getMagnitude(0, 0, silentDry.getNumSamples()), 0.0f,
                         "sends are post-fader");

            m.tracks[0].volumeDb = 0.0f;
            auto withTail = MixEngine::renderOffline(a, m.params, m.master, 48000.0, 1.0);
            expectEquals(withTail.getNumSamples(), 4800 + 48000);
            expect(withTail.getMagnitude(0, 4800 + 1000, 10000) > 0.001f, "reverb tail rings after the clip ends");
        }

        beginTest("Live input feeds replace playback");
        {
            Mixer m;
            Arrangement a;
            a.insertClip(0, clip(makeSource(512, 0.5f), 0, 512));

            MixEngine e;
            e.prepare(48000.0, 512);
            std::vector<float> input(512, 0.125f);
            std::array<MixEngine::TrackFeed, kNumTracks> feeds {};
            feeds[0].kind = MixEngine::TrackFeed::Kind::LiveInput;
            feeds[0].left = feeds[0].right = input.data();

            juce::AudioBuffer<float> out(2, 512);
            e.process(&a, m.params, m.master, 0, 512, true, feeds.data(), out.getWritePointer(0), out.getWritePointer(1), false);
            expectWithinAbsoluteError(out.getSample(0, 256), 0.125f, 1e-4f);

            feeds[0].kind = MixEngine::TrackFeed::Kind::Silence;
            e.process(&a, m.params, m.master, 0, 512, true, feeds.data(), out.getWritePointer(0), out.getWritePointer(1), false);
            expectWithinAbsoluteError(out.getMagnitude(0, 0, 512), 0.0f, 1e-3f);
        }
    }
};

class AudioFileTests : public juce::UnitTest
{
public:
    AudioFileTests() : juce::UnitTest("AudioFileIO", "8track") {}

    void runTest() override
    {
        beginTest("WAV round trip with resampling to the engine rate");
        {
            auto dir = juce::File::createTempFile("8track-tests");
            dir.createDirectory();
            auto file = dir.getChildFile("tone.wav");

            juce::AudioBuffer<float> tone(1, 44100);
            for (int i = 0; i < 44100; ++i)
                tone.setSample(0, i, 0.5f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * (float) i / 44100.0f));

            juce::String error;
            expect(AudioFileIO::writeWav(file, tone, 44100.0, 24, error), error);

            auto same = AudioFileIO::loadSource(file, 44100.0, error);
            expect(same != nullptr, error);
            expectEquals(same->getLength(), (juce::int64) 44100);
            expectWithinAbsoluteError(same->audio.getSample(1, 1000), tone.getSample(0, 1000), 1e-4f,
                                      "mono files are duplicated to stereo");

            auto converted = AudioFileIO::loadSource(file, 48000.0, error);
            expect(converted != nullptr, error);
            expectEquals(converted->getLength(), (juce::int64) 48000);
            expectWithinAbsoluteError(converted->audio.getMagnitude(0, 1000, 40000), 0.5f, 0.02f);

            dir.deleteRecursively();
        }
    }
};


/** Drives AudioEngine's callback directly (no device), 512-sample blocks, one mono input. */
class RecordingTests : public juce::UnitTest
{
public:
    RecordingTests() : juce::UnitTest("Recording", "8track") {}

    struct Driver
    {
        AudioEngine& engine;
        std::vector<float> input = std::vector<float>(512, 0.0f);
        juce::AudioBuffer<float> out { 2, 512 };

        float block(float inputValue)
        {
            std::fill(input.begin(), input.end(), inputValue);
            const float* ins[1] = { input.data() };
            float* outs[2] = { out.getWritePointer(0), out.getWritePointer(1) };
            juce::AudioIODeviceCallbackContext ctx {};
            static_cast<juce::AudioIODeviceCallback&>(engine).audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 512, ctx);
            return out.getMagnitude(0, 0, 512);
        }
    };

    juce::File makeFolder()
    {
        auto dir = juce::File::createTempFile("8track-rec");
        dir.createDirectory();
        return dir;
    }

    void runTest() override
    {
        constexpr juce::int64 bar = 88200;   // 44.1 kHz, 120 BPM, 4/4

        beginTest("Punch: pre-roll plays the old take, the punch range is replaced exactly");
        {
            AudioEngine engine(false);
            Arrangement a;
            a.insertClip(0, clip(makeSource(400000, 0.25f), 0, 400000));
            engine.setArrangement(std::make_shared<Arrangement>(a));

            std::vector<AudioEngine::FinishedTake> takes;
            engine.onTakesFinished = [&](std::vector<AudioEngine::FinishedTake> t) { takes = std::move(t); };

            engine.getTrack(0).armed = true;
            engine.punchIn = 100000;
            engine.punchOut = 150000;
            engine.punchOn = true;
            engine.setPosition(120000);

            auto folder = makeFolder();
            juce::String error;
            expect(engine.record(folder, error), error);
            expectEquals(engine.getPosition(), (juce::int64) 100000 - bar, "pre-roll starts one bar before punch-in");

            Driver d { engine };
            float preRollMin = 1.0f, insideMax = 0.0f;
            while (engine.getPosition() < 170000)
            {
                const auto pos = engine.getPosition();
                const float level = d.block(0.5f);
                if (pos + 512 <= 100000) preRollMin = juce::jmin(preRollMin, level);
                if (pos >= 100000 && pos + 512 <= 150000) insideMax = juce::jmax(insideMax, level);
            }
            expectWithinAbsoluteError(preRollMin, 0.25f, 0.01f, "old take audible during pre-roll");
            expectWithinAbsoluteError(insideMax, 0.0f, 0.001f, "old take silenced inside the punch (monitoring off)");

            engine.stop();
            expectEquals((int) takes.size(), 1);
            if (takes.size() == 1)
            {
                expectEquals(takes[0].track, 0);
                expectEquals(takes[0].start, (juce::int64) 100000, "take starts exactly at punch-in");
                expectEquals(takes[0].length, (juce::int64) 50000, "take ends exactly at punch-out");
                expectWithinAbsoluteError(takes[0].source->audio.getSample(0, 25000), 0.5f, 0.001f, "recorded input is on disk");
            }
            expectEquals(engine.getPosition(), (juce::int64) 120000, "stop returns to where you were");
            folder.deleteRecursively();
        }

        beginTest("Count-in: one bar of click, then recording starts at the playhead");
        {
            AudioEngine engine(false);
            std::vector<AudioEngine::FinishedTake> takes;
            engine.onTakesFinished = [&](std::vector<AudioEngine::FinishedTake> t) { takes = std::move(t); };
            engine.getTrack(2).armed = true;
            engine.countInOn = true;
            engine.metronomeOn = false;
            engine.setPosition(2 * bar);

            auto folder = makeFolder();
            juce::String error;
            expect(engine.record(folder, error), error);
            expectEquals(engine.getPosition(), bar);

            Driver d { engine };
            float countInPeak = 0.0f, afterPeak = 0.0f;
            while (engine.getPosition() < 2 * bar + 30000)
            {
                const auto pos = engine.getPosition();
                const float level = d.block(0.3f);
                if (pos + 512 <= 2 * bar) countInPeak = juce::jmax(countInPeak, level);
                else if (pos >= 2 * bar) afterPeak = juce::jmax(afterPeak, level);
            }
            const auto end = engine.getPosition();
            expect(countInPeak > 0.05f, "click audible during count-in");
            expectWithinAbsoluteError(afterPeak, 0.0f, 0.001f, "no click after count-in when the metronome is off");

            engine.stop();
            expectEquals((int) takes.size(), 1);
            if (takes.size() == 1)
            {
                expectEquals(takes[0].track, 2);
                expectEquals(takes[0].start, 2 * bar);
                expectEquals(takes[0].length, end - 2 * bar);
            }
            folder.deleteRecursively();
        }

        beginTest("Pressing record during playback punches in without moving the playhead");
        {
            AudioEngine engine(false);
            std::vector<AudioEngine::FinishedTake> takes;
            engine.onTakesFinished = [&](std::vector<AudioEngine::FinishedTake> t) { takes = std::move(t); };
            engine.getTrack(1).armed = true;
            engine.countInOn = true;   // must not apply mid-playback

            Driver d { engine };
            engine.play();
            for (int i = 0; i < 10; ++i) d.block(0.1f);

            auto folder = makeFolder();
            juce::String error;
            expect(engine.record(folder, error), error);
            expectEquals(engine.getPosition(), (juce::int64) 5120);
            for (int i = 0; i < 10; ++i) d.block(0.1f);

            engine.stop();
            expectEquals((int) takes.size(), 1);
            if (takes.size() == 1)
            {
                expectEquals(takes[0].start, (juce::int64) 5120);
                expectEquals(takes[0].length, (juce::int64) 5120);
            }
            expectEquals(engine.getPosition(), (juce::int64) 0, "stop returns to where playback started");
            folder.deleteRecursively();
        }

        beginTest("Armed tracks meter their input even with monitoring off (and stay silent)");
        {
            AudioEngine engine(false);
            engine.getTrack(0).armed = true;
            engine.getTrack(0).monitor = (int) MonitorMode::Off;
            Driver d { engine };
            const float outLevel = d.block(0.5f);
            expectWithinAbsoluteError(engine.getTrack(0).peakL.load(), 0.5f, 1e-4f);
            expectWithinAbsoluteError(outLevel, 0.0f, 1e-6f, "not monitored, so not heard");

            engine.getTrack(0).monitor = (int) MonitorMode::Auto;
            expectWithinAbsoluteError(d.block(0.5f), 0.5f, 1e-3f, "auto monitoring is heard while stopped");
        }

        beginTest("Recording with nothing armed is refused with a message");
        {
            AudioEngine engine(false);
            juce::String error;
            auto folder = makeFolder();
            expect(!engine.record(folder, error));
            expect(error.isNotEmpty());
            expect(engine.getState() == AudioEngine::State::Stopped);
            folder.deleteRecursively();
        }
    }
};

class ProjectFileTests : public juce::UnitTest
{
public:
    ProjectFileTests() : juce::UnitTest("ProjectFiles", "8track") {}

    void runTest() override
    {
        beginTest("Saves as project.8track and still opens projects saved as 4track");
        {
            auto dir = juce::File::createTempFile("8track-project");
            AudioEngine engine(false);
            engine.bpm = 97.0;
            Project project(engine);
            juce::String error;
            expect(project.saveAs(dir, error), error);
            auto saved = dir.getChildFile(Project::kProjectFileName);
            expect(saved.existsAsFile(), "new projects use the new file name");
            expect(saved.loadFileAsString().contains("<EIGHTTRACK"));

            // Turn it into what the app wrote before the rename.
            auto legacy = dir.getChildFile(Project::kLegacyProjectFileName);
            legacy.replaceWithText(saved.loadFileAsString().replace("EIGHTTRACK", "FOURTRACK"));
            saved.deleteFile();

            AudioEngine engine2(false);
            Project reopened(engine2);
            error = {};
            expect(reopened.load(dir, error), error);
            expect(!reopened.isUntitled());
            expectWithinAbsoluteError(engine2.bpm.load(), 97.0, 1e-9, "settings read from the old file");

            error = {};
            expect(!reopened.load(juce::File::createTempFile("not-a-project"), error));
            expect(error.contains("8track"));
            dir.deleteRecursively();
        }
    }
};

static ArrangementTests arrangementTests;
static ProjectFileTests projectFileTests;
static RecordingTests recordingTests;
static MixEngineTests mixEngineTests;
static AudioFileTests audioFileTests;

int main()
{
    juce::MessageManager::getInstance();
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("8track");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult(i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string(failures)) << std::endl;
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return failures == 0 ? 0 : 1;
}

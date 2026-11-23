#include <juce_core/juce_core.h>
#include "../Source/Model/Arrangement.h"
#include "../Source/Audio/AudioEngine.h"
#include "../Source/Project/Project.h"

namespace
{
    /** Stand-in for an audio file: the Arrangement only needs its identity and length. */
    struct Source
    {
        juce::File file;
        juce::int64 length = 0;
    };

    Source makeSource(int length, float /*value*/ = 0.0f)
    {
        static int counter = 0;
        return { juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("src" + juce::String(++counter) + ".wav"), length };
    }

    Source makeRamp(int length) { return makeSource(length); }

    Clip clip(const Source& src, juce::int64 start, juce::int64 length, juce::int64 offset = 0)
    {
        Clip c;
        c.file = src.file;
        c.sourceLength = src.length;
        c.start = start;
        c.length = length;
        c.sourceOffset = offset;
        return c;
    }

    /** Writes a stereo WAV of a constant value, for tests that need a real file. */
    juce::File writeWav(const juce::File& file, double sampleRate, int length, float value)
    {
        file.deleteFile();
        juce::AudioBuffer<float> audio(2, length);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill(audio.getWritePointer(ch), value, length);

        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate(sampleRate)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(24));
        writer->writeFromAudioSampleBuffer(audio, 0, length);
        return file;
    }
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

/**
    Project and AudioEngine against a real Tracktion Edit: every edit made through the
    Arrangement must land in the Edit, survive undo/redo and a save/reopen, and old
    8track projects must import.
*/
class TracktionProjectTests : public juce::UnitTest
{
public:
    TracktionProjectTests() : juce::UnitTest("TracktionProject", "8track") {}

    void runTest() override
    {
        auto dir = juce::File::createTempFile("8track-te");
        dir.createDirectory();

        AudioEngine engine;
        const double sr = engine.getSampleRate();
        const int len = (int) sr;   // one second
        auto wav = writeWav(dir.getChildFile("source.wav"), sr, len, 0.5f);

        {
            Project project(engine);
            const auto untitledFolder = project.getFolder();
            juce::String error;

            beginTest("Importing audio puts a clip in the Edit, undoably");
            expect(project.importAudio(wav, 2, (juce::int64) sr, error), error);
            expectEquals((int) project.getArrangement().tracks[2].size(), 1);
            auto c = project.getArrangement().tracks[2].front();
            expectEquals(c.start, (juce::int64) sr);
            expectEquals(c.length, (juce::int64) len);
            expectEquals(c.sourceLength, (juce::int64) len);
            expectEquals(engine.getAudioTrack(2)->getClips().size(), 1, "the clip is in the Tracktion track");

            expect(project.undo());
            expectEquals((int) project.getArrangement().tracks[2].size(), 0, "undo removes it");
            expect(project.redo());
            expectEquals((int) project.getArrangement().tracks[2].size(), 1, "redo brings it back");

            beginTest("Move, split, fade and gain are written to the Edit");
            const auto id = project.getArrangement().tracks[2].front().id;
            expect(project.perform([&](Arrangement& a) { return a.moveClip(id, 5, 0); }));
            expectEquals((int) project.getArrangement().tracks[5].size(), 1, "moved to track 6");
            expectEquals(engine.getAudioTrack(5)->getClips().size(), 1);
            expectEquals(engine.getAudioTrack(2)->getClips().size(), 0);

            const auto movedId = project.getArrangement().tracks[5].front().id;
            expect(project.perform([&](Arrangement& a) { return a.splitClip(movedId, len / 4); }));
            const auto lane = project.getArrangement().tracks[5];
            expectEquals((int) lane.size(), 2, "split in two");
            expectEquals(engine.getAudioTrack(5)->getClips().size(), 2);
            expectEquals(lane[0].length + lane[1].length, (juce::int64) len, "nothing lost");
            expectEquals(lane[1].sourceOffset, (juce::int64) (len / 4), "right half plays from the split point");

            const auto rightId = lane[1].id;
            expect(project.perform([&](Arrangement& a) { return a.setFades(rightId, 1000, 2000) && a.setGain(rightId, 0.5f); }));
            auto* right = project.getArrangement().findClip(rightId);
            expect(right != nullptr);
            if (right != nullptr)
            {
                expect(std::abs(right->fadeIn - 1000) <= 1, "fade-in stored");
                expect(std::abs(right->fadeOut - 2000) <= 1, "fade-out stored");
                expectWithinAbsoluteError(right->gain, 0.5f, 0.001f, "gain stored");
            }

            beginTest("Mixer settings reach Tracktion's plugins and come back after undo");
            engine.getTrack(5).volumeDb = -6.0f;
            engine.getTrack(5).lowCut = 120.0f;
            engine.syncNow();
            auto* volume = engine.getAudioTrack(5)->getVolumePlugin();
            expectWithinAbsoluteError(volume->getVolumeDb(), -6.0f, 0.05f);
            volume->setVolumeDb(-12.0f);   // as undo or an automation change would
            engine.syncNow();
            expectWithinAbsoluteError(engine.getTrack(5).volumeDb.load(), -12.0f, 0.05f, "the UI follows the Edit");

            beginTest("Save As, then reopen, gives back the same clips and mixer");
            auto saved = dir.getChildFile("Song");
            expect(project.saveAs(saved, error), error);
            expect(saved.getChildFile(Project::kProjectFileName).existsAsFile());
            expect(saved.getChildFile("Audio").getChildFile("source.wav").existsAsFile(), "audio copied into the project");
            expect(!project.isDirty());

            const auto before = project.getArrangement();
            project.newProject();
            expect(project.getArrangement().isEmpty(), "new project is empty");

            error = {};
            expect(project.load(saved, error), error);
            expect(error.isEmpty(), error);
            expectEquals((int) project.getArrangement().tracks[5].size(), 2);
            for (size_t i = 0; i < before.tracks[5].size() && i < project.getArrangement().tracks[5].size(); ++i)
            {
                auto& x = before.tracks[5][i];
                auto& y = project.getArrangement().tracks[5][i];
                expect(std::abs(x.start - y.start) <= 1 && std::abs(x.length - y.length) <= 1
                       && std::abs(x.sourceOffset - y.sourceOffset) <= 1, "clip " + juce::String((int) i) + " unchanged");
                expect(y.file.isAChildOf(saved), "clip points into the saved project");
            }
            expectWithinAbsoluteError(engine.getTrack(5).volumeDb.load(), -12.0f, 0.05f, "volume reopened");
            expectWithinAbsoluteError(engine.getTrack(5).lowCut.load(), 120.0f, 0.5f, "low cut reopened");

            beginTest("Projects saved before Tracktion import with their clips and mixer");
            auto legacy = dir.getChildFile("Old");
            legacy.getChildFile("Audio").createDirectory();
            wav.copyFileTo(legacy.getChildFile("Audio/take.wav"));
            juce::XmlElement xml("EIGHTTRACK");
            xml.setAttribute("sampleRate", sr);
            xml.setAttribute("bpm", 97.0);
            auto* tr = xml.createNewChildElement("TRACK");
            tr->setAttribute("index", 3);
            tr->setAttribute("name", "Bass");
            tr->setAttribute("volumeDb", -3.0);
            auto* cx = tr->createNewChildElement("CLIP");
            cx->setAttribute("file", "Audio/take.wav");
            cx->setAttribute("start", juce::String((juce::int64) (2 * sr)));
            cx->setAttribute("offset", "0");
            cx->setAttribute("length", juce::String(len / 2));
            xml.writeTo(legacy.getChildFile(Project::kProjectFileName));

            error = {};
            expect(project.load(legacy, error), error);
            expectEquals((int) project.getArrangement().tracks[3].size(), 1);
            if (!project.getArrangement().tracks[3].empty())
            {
                auto& lc = project.getArrangement().tracks[3].front();
                expect(std::abs(lc.start - (juce::int64) (2 * sr)) <= 1);
                expect(std::abs(lc.length - len / 2) <= 1);
            }
            expectEquals(engine.getTrack(3).name, juce::String("Bass"));
            expectWithinAbsoluteError(engine.getAudioTrack(3)->getVolumePlugin()->getVolumeDb(), -3.0f, 0.05f);
            expectWithinAbsoluteError(engine.bpm.load(), 97.0, 1e-6);
            expect(project.isDirty(), "imported projects need saving in the new format");
            expect(!project.canUndo(), "the import isn't an undo step");

            beginTest("Export renders a WAV with the mix and a tail");
            auto out = dir.getChildFile("mix.wav");
            error = {};
            expect(project.exportMix(out, error), error);
            if (auto reader = std::unique_ptr<juce::AudioFormatReader>(juce::WavAudioFormat().createReaderFor(out.createInputStream().release(), true)))
            {
                expect(reader->lengthInSamples >= (juce::int64) (2 * sr + len / 2), "covers the clip");
                juce::AudioBuffer<float> buf(2, (int) reader->lengthInSamples);
                reader->read(&buf, 0, buf.getNumSamples(), 0, true, true);
                expect(buf.getMagnitude(0, (int) (2.2 * sr), (int) (0.1 * sr)) > 0.05f, "the clip is audible");
                expect(buf.getMagnitude(0, 0, (int) (0.5 * sr)) < 0.001f, "silence before it");
            }
            else
            {
                expect(false, "export produced a readable WAV");
            }
            untitledFolder.deleteRecursively();   // where the import went before Save As
        }

        dir.deleteRecursively();
    }
};

/** Plugin inserts, using Apple's AUDelay (on every Mac) as the plugin. */
class PluginInsertTests : public juce::UnitTest
{
public:
    PluginInsertTests() : juce::UnitTest("PluginInserts", "8track") {}

    void runTest() override
    {
        AudioEngine engine;
        auto& formats = engine.getTracktion().getPluginManager().pluginFormatManager;

        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* format : formats.getFormats())
            if (format->getName() == "AudioUnit")
                format->findAllTypesForFile(found, "AudioUnit:Effects/aufx,dely,appl");
        if (found.isEmpty())
        {
            logMessage("AUDelay not available; skipping plugin tests");
            return;
        }
        const auto delay = *found[0];
        auto& known = engine.getKnownPlugins();
        const bool wasKnown = known.getTypeForIdentifierString(delay.createIdentifierString()) != nullptr;
        known.addType(delay);   // projects find their plugins in the scanned list

        auto dir = juce::File::createTempFile("8track-plugins");
        dir.createDirectory();
        {
            Project project(engine);
            const auto untitledFolder = project.getFolder();
            juce::String error;

            beginTest("A plugin goes after the EQ and before the fader");
            auto* plugin = engine.addInsert(1, delay, error);
            expect(plugin != nullptr, error);
            expectEquals((int) engine.getInserts(1).size(), 1);
            auto& list = engine.getAudioTrack(1)->pluginList;
            if (plugin != nullptr)
            {
                expect(list.indexOf(plugin) < list.indexOf(engine.getAudioTrack(1)->getVolumePlugin()), "pre-fader");
                expect(dynamic_cast<te::EqualiserPlugin*>(list[list.indexOf(plugin) - 2]) != nullptr
                       || dynamic_cast<te::LowPassPlugin*>(list[list.indexOf(plugin) - 1]) != nullptr, "after the filters");
            }
            expectEquals((int) engine.getInserts(0).size(), 0, "other tracks untouched");

            beginTest("Adding a plugin is undoable");
            expect(project.undo());
            expectEquals((int) engine.getInserts(1).size(), 0);
            expect(project.redo());
            expectEquals((int) engine.getInserts(1).size(), 1);

            beginTest("Tracks are limited to four plugins");
            for (int i = 1; i < AudioEngine::kMaxInserts; ++i)
                expect(engine.addInsert(1, delay, error) != nullptr, error);
            error = {};
            expect(engine.addInsert(1, delay, error) == nullptr);
            expect(error.isNotEmpty());
            while (engine.getInserts(1).size() > 1)
                engine.removeInsert(*engine.getInserts(1).back());
            expectEquals((int) engine.getInserts(1).size(), 1);

            beginTest("Plugins and their bypass survive save and reopen, with the mixer intact");
            engine.getInserts(1).front()->setEnabled(false);
            engine.getTrack(1).volumeDb = -4.0f;
            auto saved = dir.getChildFile("WithPlugin");
            error = {};
            expect(project.saveAs(saved, error), error);
            project.newProject();
            expectEquals((int) engine.getInserts(1).size(), 0, "a new project has no plugins");
            expect(project.load(saved, error), error);
            auto reopened = engine.getInserts(1);
            expectEquals((int) reopened.size(), 1);
            if (!reopened.empty())
            {
                expect(dynamic_cast<te::ExternalPlugin*>(reopened.front()) != nullptr);
                expect(!reopened.front()->isEnabled(), "still bypassed");
            }
            expectWithinAbsoluteError(engine.getTrack(1).volumeDb.load(), -4.0f, 0.05f);
            expectEquals((int) engine.getInserts(0).size(), 0, "the mixer's own plugins aren't inserts");

            beginTest("Removing a plugin");
            if (!reopened.empty())
                engine.removeInsert(*reopened.front());
            expectEquals((int) engine.getInserts(1).size(), 0);
            untitledFolder.deleteRecursively();
        }
        dir.deleteRecursively();
        if (!wasKnown)
            known.removeType(delay);
    }
};

static ArrangementTests arrangementTests;
static TracktionProjectTests tracktionProjectTests;
static PluginInsertTests pluginInsertTests;

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // keep every line if a test crashes
    juce::SystemStats::setApplicationCrashHandler([](void*)
    {
        std::cerr << "CRASH\n" << juce::SystemStats::getStackBacktrace() << std::endl;
        std::_Exit(2);
    });
    juce::ScopedJuceInitialiser_GUI juceInit;   // Tracktion needs the message manager and GUI basics
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("8track");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult(i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}

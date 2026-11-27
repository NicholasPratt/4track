#include "../Source/UI/PianoRoll.h"
#include "../Source/Audio/TunerCapture.h"
#include "../Source/Audio/Mp3Encoder.h"
#include "../Source/UI/LyricRows.h"
#include "../Source/UI/LyricEditorWindow.h"
#include "../Source/UI/Ruler.h"
#include "../Source/UI/DrumSequencer.h"
#include "../Source/UI/SamplerEditors.h"
#include "../Source/UI/TrackLanes.h"
#include "../Source/UI/ShortcutHelp.h"
#include <juce_core/juce_core.h>
#include "../Source/Audio/RecordingThumbnail.h"
#include "../Source/Model/Arrangement.h"
#include "../Source/Audio/AudioEngine.h"
#include "../Source/Audio/Effects.h"
#include "../Source/Audio/Dynamics.h"
#include "../Source/Audio/Drive.h"
#include "../Source/Audio/ChorusEffect.h"
#include "../Source/Audio/DoublerEffect.h"
#include "../Source/Audio/EqDeEsser.h"
#include "../Source/Audio/ReverbEffect.h"
#include "../Source/Audio/Arpeggiator.h"
#include "../Source/Audio/AnalogSynth.h"
#include "../Source/Audio/DrumSampler.h"
#include "../Source/Audio/KeySampler.h"
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

        beginTest("Copy and paste keep the clips' spacing across tracks");
        {
            Arrangement a;
            auto src = makeSource(100000);
            const auto first = a.insertClip(1, clip(src, 1000, 2000));
            const auto second = a.insertClip(3, clip(src, 4000, 1000, 500));
            a.insertClip(1, clip(src, 9000, 1000));   // not copied

            auto copied = a.copyClips({ first, second });
            expectEquals((int) copied.size(), 2);
            expectEquals(Arrangement::lengthOf(copied), (juce::int64) 4000, "from the first start to the last end");

            std::array<bool, kNumTracks> allAudio {};
            const auto ids = a.pasteClips(copied, 4, 20000, allAudio);
            expectEquals((int) ids.size(), 2);
            expectEquals((int) a.tracks[4].size(), 1);
            expectEquals((int) a.tracks[6].size(), 1);
            expectEquals(a.tracks[4][0].start, (juce::int64) 20000);
            expectEquals(a.tracks[6][0].start, (juce::int64) 23000, "same distance after the first clip");
            expectEquals(a.tracks[6][0].sourceOffset, (juce::int64) 500, "plays the same part of the file");
            expect(a.tracks[4][0].id != first && a.tracks[6][0].id != second, "new clips");
            expectEquals((int) a.tracks[1].size(), 2, "originals stay");

            beginTest("Pasting goes over what's there, and skips tracks it doesn't fit");
            Arrangement b = a;
            b.pasteClips(copied, 6, 23500, allAudio);   // the second clip would be on track 9
            expectEquals((int) b.tracks[6].size(), 2, "the old clip is trimmed, not deleted");
            expectEquals(b.tracks[6][0].end(), (juce::int64) 23500);
            expectEquals(b.tracks[6][1].start, (juce::int64) 23500);

            std::array<bool, kNumTracks> midiOn4 {};
            midiOn4[4] = true;
            Arrangement c = a;
            const auto some = c.pasteClips(copied, 4, 40000, midiOn4);
            expectEquals((int) some.size(), 1, "audio isn't pasted onto a MIDI track");
            expectEquals((int) c.tracks[4].size(), 1);
            expectEquals((int) c.tracks[6].size(), 2);
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

            beginTest("Pasted audio clips are new clips in the Edit, with the same settings");
            {
                const auto copied = project.getArrangement().copyClips({ rightId });
                std::array<bool, kNumTracks> allAudio {};
                expect(project.perform([&](Arrangement& a) { return !a.pasteClips(copied, 0, 0, allAudio).empty(); }));
                expectEquals(engine.getAudioTrack(0)->getClips().size(), 1);
                const auto& pasted = project.getArrangement().tracks[0];
                expectEquals((int) pasted.size(), 1);
                if (pasted.size() == 1)
                {
                    expectEquals(pasted[0].sourceOffset, (juce::int64) (len / 4));
                    expectWithinAbsoluteError(pasted[0].gain, 0.5f, 0.001f);
                    expect(std::abs(pasted[0].fadeIn - 1000) <= 1);
                    expect(pasted[0].id != rightId);
                }
                expectEquals((int) project.getArrangement().tracks[5].size(), 2, "the original is still there");
                expect(project.undo());
                expectEquals(engine.getAudioTrack(0)->getClips().size(), 0, "undo removes the paste");
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
                expectEquals((int) reader->bitsPerSample, 24);
                expectWithinAbsoluteError((double) reader->lengthInSamples / reader->sampleRate,
                                          2.0 + len / (2.0 * sr) + 3.0, 0.03);
                juce::AudioBuffer<float> buf(2, (int) reader->lengthInSamples);
                reader->read(&buf, 0, buf.getNumSamples(), 0, true, true);
                expect(buf.getMagnitude(0, (int) (2.2 * sr), (int) (0.1 * sr)) > 0.05f, "the clip is audible");
                expect(buf.getMagnitude(0, 0, (int) (0.5 * sr)) < 0.001f, "silence before it");
            }
            else
            {
                expect(false, "export produced a readable WAV");
            }

            beginTest("WAV export applies the selected bit depth");
            Project::ExportSettings settings;
            for (int depth : { 16, 32 })
            {
                settings.wavBitDepth = depth;
                expect(project.exportMix(out, settings, error), error);
                auto reader = std::unique_ptr<juce::AudioFormatReader>(
                    juce::WavAudioFormat().createReaderFor(out.createInputStream().release(), true));
                expect(reader != nullptr, "export produced a readable WAV");
                if (reader != nullptr)
                {
                    expectEquals((int) reader->bitsPerSample, depth);
                    expectEquals((int) reader->usesFloatingPointData, depth == 32 ? 1 : 0);
                }
            }

            beginTest("An invalid export leaves the existing destination intact");
            const auto savedSize = out.getSize();
            settings.wavBitDepth = 12;
            expect(!project.exportMix(out, settings, error));
            expectEquals(out.getSize(), savedSize);

            if (Mp3Encoder::isAvailable())
            {
                beginTest("MP3 exports are audible and use the selected constant bitrate");
                settings.format = Project::ExportSettings::Format::mp3;
                auto mp3 = dir.getChildFile("mix.mp3");
                for (int bitrate : { 64, 192, 320 })
                {
                    settings.mp3BitrateKbps = bitrate;
                    expect(project.exportMix(mp3, settings, error), error);
                    const auto seconds = 2.0 + len / (2.0 * sr) + 3.0;
                    const auto actualKbps = (double) mp3.getSize() * 8.0 / seconds / 1000.0;
                    expectWithinAbsoluteError(actualKbps, (double) bitrate, bitrate * 0.03);
                   #if JUCE_USE_MP3AUDIOFORMAT
                    auto reader = std::unique_ptr<juce::AudioFormatReader>(
                        juce::MP3AudioFormat().createReaderFor(mp3.createInputStream().release(), true));
                    expect(reader != nullptr, "export produced a readable MP3");
                    if (reader != nullptr)
                    {
                        const auto count = (int) (0.1 * reader->sampleRate);
                        juce::AudioBuffer<float> buffer(2, count);
                        expect(reader->read(&buffer, 0, count, (juce::int64) (2.2 * reader->sampleRate), true, true));
                        expect(buffer.getMagnitude(0, count) > 0.05f, "the clip is audible");
                    }
                   #endif
                }
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

            beginTest("The master bus takes plugins too, before the master fader");
            {
                expectEquals((int) engine.getInserts(AudioEngine::kMasterBus).size(), 0, "none to start with");
                auto* onMaster = engine.addInsert(AudioEngine::kMasterBus, delay, error);
                expect(onMaster != nullptr, error);
                expectEquals((int) engine.getInserts(AudioEngine::kMasterBus).size(), 1);
                expect(project.getEdit().getMasterPluginList().indexOf(onMaster) >= 0, "in Tracktion's master list");
                expectEquals((int) engine.getInserts(1).size(), 1, "track plugins untouched");

                expect(project.undo());
                expectEquals((int) engine.getInserts(AudioEngine::kMasterBus).size(), 0, "undoable");
                expect(project.redo());

                for (int i = 1; i < AudioEngine::kMaxInserts; ++i)
                    engine.addInsert(AudioEngine::kMasterBus, delay, error);
                error = {};
                expect(engine.addInsert(AudioEngine::kMasterBus, delay, error) == nullptr && error.startsWith("The master"),
                       "limited to four");
                while (engine.getInserts(AudioEngine::kMasterBus).size() > 1)
                    engine.removeInsert(*engine.getInserts(AudioEngine::kMasterBus).back());

                expect(project.save(error), error);
                expect(project.load(saved, error), error);
                expectEquals((int) engine.getInserts(AudioEngine::kMasterBus).size(), 1, "kept when saved");
                expectWithinAbsoluteError(engine.getMaster().volumeDb.load(), 0.0f, 0.05f, "master fader intact");
                reopened = engine.getInserts(1);
            }

            beginTest("Both stereo buses have independent pre-fader inserts");
            for (int index = AudioEngine::kFirstStereoBus; index < AudioEngine::kFirstStereoBus + AudioEngine::kStereoBusCount; ++index)
            {
                expectEquals(static_cast<int>(engine.getInserts(index).size()), 0);
                auto* insert = engine.addInsert(index, delay, error);
                expect(insert != nullptr, error);
                if (insert != nullptr)
                {
                    auto* bus = engine.getAudioTrack(index);
                    expect(bus->pluginList.indexOf(insert) >= 0);
                    expect(bus->pluginList.indexOf(insert) < bus->pluginList.indexOf(bus->getVolumePlugin()));
                    insert->setEnabled(false);
                }
            }
            expect(project.save(error), error);
            expect(project.load(saved, error), error);
            for (int index = AudioEngine::kFirstStereoBus; index < AudioEngine::kFirstStereoBus + AudioEngine::kStereoBusCount; ++index)
            {
                const auto effects = engine.getInserts(index);
                expectEquals(static_cast<int>(effects.size()), 1);
                if (!effects.empty())
                    expect(!effects.front()->isEnabled());
            }
            reopened = engine.getInserts(1);

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

class StereoBusTests : public juce::UnitTest
{
public:
    StereoBusTests() : juce::UnitTest("Stereo buses", "8track") {}

    void runTest() override
    {
        beginTest("Main is the default path and Bus 1 owns the reverb");
        AudioEngine engine;
        Project project(engine);
        const auto untitledFolder = project.getFolder();
        using Route = AudioEngine::Route;
        expect(engine.hasTrackRoute(0, Route::Main));
        expect(!engine.hasTrackRoute(0, Route::Bus1));
        expect(!engine.hasTrackRoute(0, Route::Bus2));
        expect(engine.isBusReverbEnabled());
        expect(!engine.bus1FeedsBus2());
        auto* firstBus = engine.getAudioTrack(AudioEngine::kFirstStereoBus);
        auto* secondBus = engine.getAudioTrack(AudioEngine::kFirstStereoBus + 1);
        expectEquals(firstBus->pluginList.getPluginsOfType<te::ReverbPlugin>().size(), 1);
        expect(secondBus->pluginList.getPluginsOfType<te::ReverbPlugin>().isEmpty());
        expect(project.getEdit().getMasterPluginList().getPluginsOfType<te::ReverbPlugin>().isEmpty());

        auto directory = juce::File::createTempFile("8track-buses");
        expect(directory.createDirectory().wasOk());
        const auto sampleRate = engine.getSampleRate();
        const auto samples = juce::roundToInt(sampleRate * 0.5);
        const auto source = directory.getChildFile("stereo.wav");
        {
            juce::AudioBuffer<float> signal(2, samples);
            for (int i = 0; i < samples; ++i)
            {
                const auto time = i / sampleRate;
                signal.setSample(0, i, 0.2f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 200 * time)));
                signal.setSample(1, i, 0.1f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 300 * time)));
            }
            std::unique_ptr<juce::OutputStream> stream = source.createOutputStream();
            auto writer = juce::WavAudioFormat().createWriterFor(stream, juce::AudioFormatWriterOptions()
                .withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(24));
            expect(writer != nullptr && writer->writeFromAudioSampleBuffer(signal, 0, samples));
        }

        juce::String error;
        expect(project.importAudio(source, 0, 0, error), error);
        const auto render = [&](double start = 0.2)
        {
            const auto output = directory.getChildFile("mix.wav");
            expect(project.exportMix(output, error), error);
            auto reader = std::unique_ptr<juce::AudioFormatReader>(
                juce::WavAudioFormat().createReaderFor(output.createInputStream().release(), true));
            if (reader == nullptr)
            {
                expect(false, "The mix should be readable");
                return std::array<float, 2> {};
            }
            const int length = juce::roundToInt(reader->sampleRate * 0.08);
            juce::AudioBuffer<float> audio(2, length);
            expect(reader->read(&audio, 0, length, juce::roundToInt(reader->sampleRate * start), true, true));
            return std::array { audio.getRMSLevel(0, 0, length), audio.getRMSLevel(1, 0, length) };
        };
        const auto direct = render();
        expect(direct[0] > 0.07f && direct[1] > 0.035f);
        expectWithinAbsoluteError(direct[0] / juce::jmax(0.001f, direct[1]), 2.0f, 0.02f);

        beginTest("Each send works independently in stereo and parallel sends sum");
        engine.setBusReverbEnabled(false);
        engine.setTrackRoute(0, Route::Main, false);
        const auto silent = render();
        expect(silent[0] < 0.0001f && silent[1] < 0.0001f);
        engine.setTrackRoute(0, Route::Bus1, true);
        const auto bus1 = render();
        engine.setTrackRoute(0, Route::Bus1, false);
        engine.setTrackRoute(0, Route::Bus2, true);
        const auto bus2 = render();
        engine.setTrackRoute(0, Route::Main, true);
        const auto parallel = render();
        for (size_t channel = 0; channel < 2; ++channel)
        {
            expectWithinAbsoluteError(bus1[channel], direct[channel], 0.005f);
            expectWithinAbsoluteError(bus2[channel], direct[channel], 0.005f);
            expectWithinAbsoluteError(parallel[channel], direct[channel] * 2.0f, 0.005f);
        }

        beginTest("Routing Bus 1 to Bus 2 applies both stereo bus faders and no direct copy");
        engine.setTrackRoute(0, Route::Main, false);
        engine.setTrackRoute(0, Route::Bus2, false);
        engine.setTrackRoute(0, Route::Bus1, true);
        engine.getBus(0).volumeDb = -6.0f;
        engine.getBus(1).volumeDb = -12.0f;
        engine.setBus1FeedsBus2(true);
        const auto chained = render();
        for (size_t channel = 0; channel < 2; ++channel)
            expectWithinAbsoluteError(chained[channel], direct[channel] * juce::Decibels::decibelsToGain(-18.0f), 0.003f);

        beginTest("An insert on Bus 2 processes the output of Bus 1");
        auto filter = project.getEdit().getPluginCache().createNewPlugin(te::LowPassPlugin::xmlTypeName, {});
        auto* lowPass = dynamic_cast<te::LowPassPlugin*>(filter.get());
        expect(lowPass != nullptr);
        if (lowPass != nullptr)
        {
            lowPass->frequency->setParameter(40.0f, juce::sendNotification);
            auto& list = secondBus->pluginList;
            list.insertPlugin(filter, list.indexOf(secondBus->getVolumePlugin()), nullptr);
            expectEquals(static_cast<int>(engine.getInserts(AudioEngine::kFirstStereoBus + 1).size()), 1);
            expect(engine.getInserts(AudioEngine::kFirstStereoBus).empty());
            const auto filtered = render();
            expect(filtered[0] < chained[0] * 0.3f && filtered[1] < chained[1] * 0.3f);
            engine.removeInsert(*filter);
        }
        filter = nullptr;

        beginTest("The routed reverb is audible in the export tail");
        engine.setBusReverbEnabled(true);
        engine.getBus(0).volumeDb = 0.0f;
        engine.getBus(1).volumeDb = 0.0f;
        const auto tail = render(0.53);
        expect(tail[0] > 0.0001f || tail[1] > 0.0001f);

        beginTest("Routing is undoable and survives saving and reopening");
        engine.setTrackRoute(0, Route::Main, true);
        expect(project.undo());
        expect(!engine.hasTrackRoute(0, Route::Main));
        engine.setBus1FeedsBus2(false);
        expect(project.undo());
        expect(engine.bus1FeedsBus2());
        const auto saved = directory.getChildFile("Session");
        expect(project.saveAs(saved, error), error);
        expect(project.load(saved, error), error);
        expect(!engine.hasTrackRoute(0, Route::Main));
        expect(engine.hasTrackRoute(0, Route::Bus1));
        expect(!engine.hasTrackRoute(0, Route::Bus2));
        expect(engine.bus1FeedsBus2());
        expect(engine.isBusReverbEnabled());

        beginTest("Drum pads send independently to the mixer buses before the track fader");
        project.newProject();
        engine.setTrackMidi(0, true);
        engine.setBusReverbEnabled(false);
        auto* drums = dynamic_cast<DrumSampler*>(engine.setInstrument(0, DrumSampler::xmlTypeName, error));
        expect(drums != nullptr, error);
        if (drums != nullptr)
        {
            for (auto* send : drums->pads[0].sends)
            {
                const auto& range = send->valueRange;
                expectWithinAbsoluteError(range.convertFrom0to1(0.5f), -12.0f, 0.001f, "useful send levels are within the first half of travel");
                expectWithinAbsoluteError(send->getCurrentValue(), -80.0f, 0.001f, "the default stays Off");
            }
            expect(drums->loadPad(0, source, error), error);
            expect(drums->loadPad(1, source, error), error);
            DrumPattern pattern;
            pattern.rows[0].set(0);
            pattern.rows[1].set(8);
            project.setDrumPattern(0, pattern);
            expect(project.placeDrumPattern(0, 0, te::BeatPosition()) != nullptr);
            const double secondHit = project.getEdit().tempoSequence.toTime(te::BeatPosition::fromBeats(2.0)).inSeconds();
            const auto dry = render();
            expect(dry[0] > 0.05f && dry[1] > 0.025f);

            engine.setTrackRoute(0, Route::Main, false);
            const auto noSend = render();
            expect(noSend[0] < 0.0001f && noSend[1] < 0.0001f, "pad sends default to Off");
            drums->pads[0].sends[0]->setParameter(-6.0f, juce::sendNotification);
            drums->pads[1].sends[1]->setParameter(-12.0f, juce::sendNotification);
            engine.getTrack(0).volumeDb = -18.0f;

            engine.getBus(1).volumeDb = -100.0f;
            const auto routed1 = render();
            const auto otherPad1 = render(secondHit + 0.2);
            engine.getBus(1).volumeDb = 0.0f;
            engine.getBus(0).volumeDb = -100.0f;
            const auto routed2 = render(secondHit + 0.2);
            const auto otherPad2 = render();
            for (size_t ch = 0; ch < 2; ++ch)
            {
                expectWithinAbsoluteError(routed1[ch], dry[ch] * juce::Decibels::decibelsToGain(-6.0f), 0.004f, "Bus 1 sends pad 1");
                expectWithinAbsoluteError(routed2[ch], dry[ch] * juce::Decibels::decibelsToGain(-12.0f), 0.004f, "Bus 2 sends pad 2");
                expect(otherPad1[ch] < 0.0001f && otherPad2[ch] < 0.0001f, "each bus only gets its assigned pad");
            }

            if (auto* gate = dynamic_cast<GateEffect*>(engine.addEffect(0, GateEffect::xmlTypeName, error)))
            {
                gate->setKeySource(engine.getAudioTrack(AudioEngine::kFirstStereoBus));
                expect(gate->getKeySource() == engine.getAudioTrack(AudioEngine::kFirstStereoBus));
                engine.getBus(0).volumeDb = 0.0f;
                engine.getBus(1).volumeDb = -100.0f;
                expectWithinAbsoluteError(render()[0], routed1[0], 0.004f, "a downstream gate can use the bus as its key");
                engine.removeInsert(*gate);
            }
            else
                expect(false, error);

            const auto drumProject = directory.getChildFile("Drums");
            expect(project.saveAs(drumProject, error), error);
            expect(project.load(drumProject, error), error);
            auto* reopened = dynamic_cast<DrumSampler*>(engine.getInstrument(0));
            expect(reopened != nullptr);
            if (reopened != nullptr)
            {
                expectWithinAbsoluteError(reopened->pads[0].sends[0]->getCurrentValue(), -6.0f, 0.001f);
                expectWithinAbsoluteError(reopened->pads[0].sends[1]->getCurrentValue(), -80.0f, 0.001f);
                expectWithinAbsoluteError(reopened->pads[1].sends[1]->getCurrentValue(), -12.0f, 0.001f);
            }
        }
        untitledFolder.deleteRecursively();
        directory.deleteRecursively();
    }
};

/** Stereo input pairs, on whatever audio interface is connected (needs two input channels). */
class StereoInputTests : public juce::UnitTest
{
public:
    StereoInputTests() : juce::UnitTest("StereoInputs", "8track") {}

    static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(300); }

    void runTest() override
    {
        bool wasPaired = false;
        {
            AudioEngine engine;
            const auto inputs = engine.getInputs();
            logMessage("inputs: " + juce::String((int) inputs.size()));
            if (!engine.canPairInputs(0))
            {
                logMessage("Fewer than two input channels active; skipping stereo tests");
                return;
            }
            wasPaired = engine.isInputPaired(0);
            if (wasPaired)
            {
                engine.setInputPaired(0, false);
                pump();
            }

            beginTest("Pairing In 1 and In 2 makes one stereo input");
            engine.getTrack(3).inputChannel = 1;   // a track on In 2
            engine.setInputPaired(0, true);
            pump();
            expect(engine.isInputPaired(0));
            auto now = engine.getInputs();
            expect(!now.empty() && now.front().channel == 0 && now.front().numChannels == 2 && now.front().name == "In 1/2",
                   now.empty() ? "none" : now.front().name);
            expectEquals(engine.getTrack(3).inputChannel.load(), 0, "the track on In 2 now records In 1/2");
            expectEquals(engine.getTrack(3).inputType.load(), (int) InputType::Stereo);
        }

        beginTest("Pairs are kept when 8track starts again");
        {
            AudioEngine engine;
            expect(engine.isInputPaired(0), "still paired");

            beginTest("Unpairing gives two mono inputs again");
            engine.setInputPaired(0, false);
            pump();
            expect(!engine.isInputPaired(0));
            auto now = engine.getInputs();
            expect(now.size() >= 2 && now[0].name == "In 1" && now[1].name == "In 2");

            if (wasPaired)
            {
                engine.setInputPaired(0, true);
                pump();
            }
        }
    }
};

/** MIDI tracks, using Apple's DLS synth (on every Mac) as the instrument. */
class MidiTrackTests : public juce::UnitTest
{
public:
    MidiTrackTests() : juce::UnitTest("MidiTracks", "8track") {}

    void runTest() override
    {
        AudioEngine engine;
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* format : engine.getTracktion().getPluginManager().pluginFormatManager.getFormats())
            if (format->getName() == "AudioUnit")
                format->findAllTypesForFile(found, "AudioUnit:Synths/aumu,dls ,appl");
        if (found.isEmpty())
        {
            logMessage("DLSMusicDevice not available; skipping MIDI tests");
            return;
        }
        const auto synth = *found[0];
        auto& known = engine.getKnownPlugins();
        const bool wasKnown = known.getTypeForIdentifierString(synth.createIdentifierString()) != nullptr;

        auto dir = juce::File::createTempFile("8track-midi");
        dir.createDirectory();
        {
            Project project(engine);
            const auto untitledFolder = project.getFolder();
            juce::String error;
            const auto bar = engine.getSamplesPerBar();   // 2 s at 120 bpm
            const int t = 4;

            beginTest("A track becomes a MIDI track with an instrument");
            engine.getTrack(t).midi = true;
            engine.syncNow();
            auto* instrument = engine.setInstrument(t, synth, error);
            expect(instrument != nullptr, error);
            expect(engine.getInstrument(t) == instrument);
            expect(engine.getInserts(t).empty(), "the instrument isn't an insert");
            expectEquals(engine.getAudioTrack(t)->pluginList.indexOf(instrument), 0, "first in the chain");

            beginTest("An empty MIDI clip takes notes");
            auto* clip = project.createMidiClip(t, 0, 2 * bar);
            expect(clip != nullptr);
            if (clip == nullptr)
                return;
            auto* um = &project.getEdit().getUndoManager();
            clip->getSequence().addNote(60, te::BeatPosition::fromBeats(0.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
            clip->getSequence().addNote(64, te::BeatPosition::fromBeats(4.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
            expectEquals((int) project.getArrangement().tracks[(size_t) t].size(), 1);
            expect(project.getArrangement().tracks[(size_t) t].front().midi);

            beginTest("Splitting a MIDI clip keeps every note where it was");
            const auto id = project.getArrangement().tracks[(size_t) t].front().id;
            expect(project.perform([&](Arrangement& a) { return a.splitClip(id, bar); }));
            auto lane = project.getArrangement().tracks[(size_t) t];
            expectEquals((int) lane.size(), 2);
            if (lane.size() == 2)
            {
                expect(lane[0].midi && lane[1].midi);
                auto* left = project.getMidiClip(lane[0].id);
                auto* right = project.getMidiClip(lane[1].id);
                expect(left != nullptr && right != nullptr);
                if (left != nullptr && right != nullptr)
                {
                    auto heardIn = [&](te::MidiClip& c, int pitch) -> double
                    {
                        for (auto* n : c.getSequence().getNotes())
                            if (n->getNoteNumber() == pitch)
                            {
                                const auto time = n->getEditStartTime(c);
                                if (c.getPosition().time.contains(time))
                                    return time.inSeconds();
                            }
                        return -1.0;
                    };
                    expectWithinAbsoluteError(heardIn(*left, 60), 0.0, 0.001, "C4 at 0 s in the left half");
                    expectWithinAbsoluteError(heardIn(*right, 64), 2.0, 0.001, "E4 at 2 s in the right half");
                    expectEquals(heardIn(*left, 64), -1.0, "E4 not in the left half");
                }
            }
            expect(project.undo());
            expectEquals((int) project.getArrangement().tracks[(size_t) t].size(), 1, "undo joins it again");

            beginTest("Trimming a MIDI clip's start cuts it, and leaves the notes where they were");
            {
                const auto whole = project.getArrangement().tracks[(size_t) t].front();
                expect(project.perform([&](Arrangement& a) { return a.trimStart(whole.id, bar / 2); }));
                const auto trimmed = project.getArrangement().tracks[(size_t) t].front();
                expect(trimmed.start == bar / 2 && trimmed.end() == whole.end(), "only the start moves");
                if (auto* c = project.getMidiClip(trimmed.id))
                {
                    double e4 = -1.0;
                    for (auto* n : c->getSequence().getNotes())
                        if (n->getNoteNumber() == 64)
                            e4 = n->getEditStartTime(*c).inSeconds();
                    expectWithinAbsoluteError(e4, 2.0, 0.001, "E4 still at 2 s");
                    expectWithinAbsoluteError(c->getPosition().getStart().inSeconds(), engine.toTime(bar / 2).inSeconds(), 0.001);
                }
                else
                    expect(false, "the trimmed clip is there");
                expect(project.undo());
            }
            {
                const auto again = project.getArrangement().tracks[(size_t) t].front().id;   // the trim replaced the split's redo
                expect(project.perform([&](Arrangement& a) { return a.splitClip(again, bar); }));
            }

            beginTest("A pasted MIDI clip has its own copy of the notes");
            {
                const auto leftId = project.getArrangement().tracks[(size_t) t].front().id;
                const auto copied = project.getArrangement().copyClips({ leftId });
                std::array<bool, kNumTracks> midiTracks {};
                midiTracks[(size_t) t] = true;
                expect(project.perform([&](Arrangement& a) { return !a.pasteClips(copied, t, 4 * bar, midiTracks).empty(); }));
                const auto& lane3 = project.getArrangement().tracks[(size_t) t];
                expectEquals((int) lane3.size(), 3);
                auto* pasted = lane3.size() == 3 ? project.getMidiClip(lane3[2].id) : nullptr;
                auto* original = project.getMidiClip(leftId);
                expect(pasted != nullptr && original != nullptr && pasted != original);
                if (pasted != nullptr && original != nullptr)
                {
                    bool c4AtEight = false;
                    for (auto* n : pasted->getSequence().getNotes())
                        if (n->getNoteNumber() == 60 && std::abs(n->getEditStartTime(*pasted).inSeconds() - 8.0) < 0.001)
                            c4AtEight = true;
                    expect(c4AtEight, "C4 plays at the start of the copy, 8 s");

                    const int before = pasted->getSequence().getNumNotes();
                    original->getSequence().addNote(72, te::BeatPosition::fromBeats(1.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
                    expectEquals(pasted->getSequence().getNumNotes(), before, "editing the original leaves the copy alone");
                    project.undo();
                }
                project.undo();
                expectEquals((int) project.getArrangement().tracks[(size_t) t].size(), 2, "undo removes the paste");
            }

            beginTest("MIDI clips have no fades or gain");
            const auto rightId = project.getArrangement().tracks[(size_t) t].back().id;
            expect(!project.perform([&](Arrangement& a) { return a.setFades(rightId, 100, 100); }));

            beginTest("MIDI track, instrument and notes survive save and reopen");
            auto saved = dir.getChildFile("WithMidi");
            expect(project.saveAs(saved, error), error);
            project.newProject();
            expect(!engine.getTrack(t).midi.load(), "a new project's tracks are audio");
            expect(project.load(saved, error), error);
            expect(engine.getTrack(t).midi.load(), "still a MIDI track");
            expect(engine.getInstrument(t) != nullptr, "instrument reloaded");
            int notes = 0;
            for (auto& c : project.getArrangement().tracks[(size_t) t])
                if (auto* mc = project.getMidiClip(c.id))
                    notes += mc->getSequence().getNumNotes();
            expectEquals(notes, 4, "both halves keep their copy of the two notes");

            beginTest("A MIDI track's input, channel and output are saved with the project");
            expect(engine.getMidiRouting(t) == AudioEngine::MidiRouting {}, "every input, any channel, no output to start with");
            AudioEngine::MidiRouting routing;
            routing.input = "Some Keyboard";
            routing.channel = 3;
            routing.output = "Some Synth";
            routing.replaceInstrument = false;
            engine.setMidiRouting(t, routing);
            expect(engine.getMidiRouting(t) == routing);
            auto& chain = engine.getAudioTrack(t)->pluginList;
            expect(dynamic_cast<MidiOutPlugin*>(chain.getPlugins()[0].get()) != nullptr, "MIDI Out is first, so it sees the notes");
            expectEquals(chain.indexOf(engine.getInstrument(t)), 1, "the instrument comes after it");
            expect(engine.getInserts(t).empty(), "it isn't one of the track's inserts");

            auto routed = dir.getChildFile("Routed");
            expect(project.saveAs(routed, error), error);
            project.newProject();
            expect(engine.getMidiRouting(t) == AudioEngine::MidiRouting {}, "a new project has no routing");
            expect(project.load(routed, error), error);
            expect(engine.getMidiRouting(t) == routing, "the routing comes back");
            expect(engine.getInstrument(t) != nullptr && dynamic_cast<MidiOutPlugin*>(engine.getAudioTrack(t)->pluginList.getPlugins()[0].get()) != nullptr);

            engine.setMidiRouting(t, {});
            expect(engine.getMidiRouting(t) == AudioEngine::MidiRouting {}, "back to every input, no output");
            expectEquals(engine.getAudioTrack(t)->pluginList.indexOf(engine.getInstrument(t)), 0, "MIDI Out is gone");

            beginTest("Export plays the instrument");
            auto out = dir.getChildFile("midi.wav");
            expect(project.exportMix(out, error), error);
            if (auto reader = std::unique_ptr<juce::AudioFormatReader>(juce::WavAudioFormat().createReaderFor(out.createInputStream().release(), true)))
            {
                juce::AudioBuffer<float> buf(2, (int) reader->lengthInSamples);
                reader->read(&buf, 0, buf.getNumSamples(), 0, true, true);
                const auto sr = engine.getSampleRate();
                expect(buf.getMagnitude(0, (int) (0.05 * sr), (int) (0.3 * sr)) > 0.01f, "the first note sounds");
                expect(buf.getMagnitude(0, (int) (2.05 * sr), (int) (0.3 * sr)) > 0.01f, "the second note sounds");
            }
            else
            {
                expect(false, "export produced a readable WAV");
            }
            untitledFolder.deleteRecursively();
        }
        dir.deleteRecursively();
        if (!wasKnown)
            known.removeType(synth);
    }
};

/** Parameter automation on mixer controls, through Tracktion's curves. */
class AutomationTests : public juce::UnitTest
{
public:
    AutomationTests() : juce::UnitTest("Automation", "8track") {}

    void runTest() override
    {
        auto dir = juce::File::createTempFile("8track-auto");
        dir.createDirectory();
        AudioEngine engine;
        const double sr = engine.getSampleRate();
        auto wav = writeWav(dir.getChildFile("tone.wav"), sr, (int) (2 * sr), 0.5f);
        {
            Project project(engine);
            const auto untitledFolder = project.getFolder();
            juce::String error;
            expect(project.importAudio(wav, 0, 0, error), error);

            beginTest("A track lists its mixer controls for automation");
            auto targets = engine.getAutomationTargets(0);
            expect(targets.size() >= 7);
            const AudioEngine::AutomationTarget* volume = nullptr;
            for (auto& t : targets)
                if (t.name == "Volume")
                    volume = &t;
            expect(volume != nullptr && volume->group == "Mixer");
            if (volume == nullptr)
                return;
            const auto key = volume->key;
            expect(engine.findAutomationTarget(0, key) == volume->param);
            expect(engine.findAutomationTarget(1, key) == nullptr, "keys belong to one track");

            beginTest("Volume automation is heard in the export");
            auto* param = volume->param;
            auto* um = &project.getEdit().getUndoManager();
            const float unity = param->getCurrentValue();
            const float silent = param->getValueRange().getStart();
            um->beginNewTransaction("Automation");
            auto& curve = param->getCurve();
            curve.addPoint(te::TimePosition::fromSeconds(0.0), unity, 0.0f, um);
            curve.addPoint(te::TimePosition::fromSeconds(1.0), unity, 0.0f, um);
            curve.addPoint(te::TimePosition::fromSeconds(1.01), silent, 0.0f, um);
            um->beginNewTransaction();
            expectEquals(curve.getNumPoints(), 3);

            auto out = dir.getChildFile("auto.wav");
            expect(project.exportMix(out, error), error);
            if (auto reader = std::unique_ptr<juce::AudioFormatReader>(juce::WavAudioFormat().createReaderFor(out.createInputStream().release(), true)))
            {
                juce::AudioBuffer<float> buf(2, (int) reader->lengthInSamples);
                reader->read(&buf, 0, buf.getNumSamples(), 0, true, true);
                expect(buf.getMagnitude(0, (int) (0.4 * sr), (int) (0.2 * sr)) > 0.1f, "full level before 1 s");
                expect(buf.getMagnitude(0, (int) (1.4 * sr), (int) (0.4 * sr)) < 0.01f, "silent after the curve drops");
            }
            else
            {
                expect(false, "export produced a readable WAV");
            }

            beginTest("Automation survives save and reopen, under the same key");
            auto saved = dir.getChildFile("Automated");
            expect(project.saveAs(saved, error), error);
            project.newProject();
            expect(project.load(saved, error), error);
            auto* reloaded = engine.findAutomationTarget(0, key);
            expect(reloaded != nullptr);
            if (reloaded != nullptr)
                expectEquals(reloaded->getCurve().getNumPoints(), 3);

            beginTest("Adding a point is undoable");
            if (reloaded != nullptr)
            {
                auto* um2 = &project.getEdit().getUndoManager();
                um2->beginNewTransaction("Point");
                reloaded->getCurve().addPoint(te::TimePosition::fromSeconds(1.5), unity, 0.0f, um2);
                um2->beginNewTransaction();
                expectEquals(reloaded->getCurve().getNumPoints(), 4);
                expect(project.undo());
                expectEquals(reloaded->getCurve().getNumPoints(), 3);
            }

            beginTest("Automating a filter switches it on");
            for (auto& t : engine.getAutomationTargets(0))
                if (t.name == "Low Cut")
                {
                    auto* um3 = &project.getEdit().getUndoManager();
                    t.param->getCurve().addPoint(te::TimePosition::fromSeconds(0.0), 200.0f, 0.0f, um3);
                    engine.syncNow();
                    bool on = false;
                    for (auto* plugin : engine.getAudioTrack(0)->pluginList.getPlugins())
                        if (auto* lp = dynamic_cast<te::LowPassPlugin*>(plugin); lp != nullptr && !lp->isLowPass())
                            on = lp->isEnabled();
                    expect(on, "the low-cut filter is running");
                }
            untitledFolder.deleteRecursively();
        }
        dir.deleteRecursively();
    }
};

/** Punch / loop modes and the looper's choice of take. */
class LoopTests : public juce::UnitTest
{
public:
    LoopTests() : juce::UnitTest("Loop", "8track") {}

    void runTest() override
    {
        AudioEngine engine;
        const double sr = engine.getSampleRate();
        auto dir = juce::File::createTempFile("8track-loop");
        dir.createDirectory();
        {
            Project project(engine);
            const auto untitledFolder = project.getFolder();
            juce::String error;

            beginTest("The range steps through off, punch and loop, never both");
            using Mode = AudioEngine::RangeMode;
            expect(AudioEngine::nextRangeMode(Mode::Off) == Mode::Punch);
            expect(AudioEngine::nextRangeMode(Mode::Punch) == Mode::Loop);
            expect(AudioEngine::nextRangeMode(Mode::Loop) == Mode::Off);
            engine.punchIn = 0;
            engine.punchOut = (juce::int64) (sr * 3);
            engine.setRangeMode(Mode::Loop);
            engine.syncNow();
            expect(project.getEdit().getTransport().looping.get(), "Tracktion loops");
            expect(!project.getEdit().recordingPunchInOut.get(), "and doesn't punch");
            engine.setRangeMode(Mode::Punch);
            engine.syncNow();
            expect(!project.getEdit().getTransport().looping.get());
            expect(project.getEdit().recordingPunchInOut.get());

            beginTest("Loop mode is saved with the project");
            engine.setRangeMode(Mode::Loop);
            auto saved = dir.getChildFile("Looped");
            expect(project.saveAs(saved, error), error);
            project.newProject();
            expect(engine.getRangeMode() == Mode::Off, "a new project has no range");
            expect(project.load(saved, error), error);
            expect(engine.getRangeMode() == Mode::Loop, "loop is back");
            expectEquals(engine.punchOut.load(), (juce::int64) (sr * 3));

            beginTest("A loop recording keeps its last complete pass");
            {
                const int loopLen = (int) (sr * 3);
                auto audio = project.getAudioFolder();
                audio.createDirectory();
                auto pass1 = writeWav(audio.getChildFile("take.wav"), sr, loopLen, 0.1f);
                auto pass2 = writeWav(audio.getChildFile("take_take_2.wav"), sr, loopLen, 0.2f);
                auto pass3 = writeWav(audio.getChildFile("take_take_3.wav"), sr, loopLen - (int) (sr * 0.010), 0.3f);   // stopped 10 ms early

                auto clip = engine.getAudioTrack(0)->insertWaveClip("take", pass1, { { te::TimePosition(), te::TimePosition::fromSeconds(3.0) }, {} }, false);
                expect(clip != nullptr);
                if (clip != nullptr)
                {
                    for (auto& f : { pass1, pass2, pass3, pass1 })   // the order Tracktion lists them in
                        clip->addTake(f);
                    engine.keepLastFullPass(*clip);
                    expect(clip->getSourceFileReference().getFile() == pass2, "the second pass, not the unfinished third");
                    expect(!clip->state.getChildWithName(te::IDs::TAKES).isValid(), "no takes left");
                }
            }
            untitledFolder.deleteRecursively();
        }
        dir.deleteRecursively();
    }
};

class ClipOperationTests : public juce::UnitTest
{
public:
    ClipOperationTests() : juce::UnitTest("ClipOperations", "8track") {}

    /** One sample of a file, at a time in seconds. */
    static float sampleAt(const juce::File& file, double seconds)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
        if (reader == nullptr)
            return -100.0f;
        juce::AudioBuffer<float> buffer((int) reader->numChannels, 1);
        reader->read(&buffer, 0, 1, (juce::int64) (seconds * reader->sampleRate), true, true);
        return buffer.getSample(0, 0);
    }

    void runTest() override
    {
        auto dir = juce::File::createTempFile("8track-ops");
        dir.createDirectory();

        AudioEngine engine;
        const double sr = engine.getSampleRate();
        const auto second = (juce::int64) sr;
        auto wav = writeWav(dir.getChildFile("half.wav"), sr, (int) second, 0.5f);
        {
            Project project(engine);
            juce::String error;
            const int t = 1;

            beginTest("Normalise sets the clip's gain so it peaks at 0 dB");
            expect(project.importAudio(wav, t, 0, error), error);
            const auto first = project.getArrangement().tracks[t].front().id;
            expect(project.normaliseClips({ first }));
            expectWithinAbsoluteError(project.getArrangement().tracks[t].front().gain, 2.0f, 0.01f);
            expect(project.undo());
            expectWithinAbsoluteError(project.getArrangement().tracks[t].front().gain, 1.0f, 0.001f, "undo puts the gain back");

            beginTest("Joining audio clips renders them, gaps and gain included, into one clip");
            expect(project.importAudio(wav, t, second * 3 / 2, error), error);   // half a second after the first ends
            const auto second2 = project.getArrangement().tracks[t].back().id;
            project.perform([second2](Arrangement& a) { return a.setGain(second2, 2.0f); });
            expect(!project.canJoin({ first }), "one clip can't be joined");
            expect(project.canJoin({ first, second2 }));

            auto joined = project.joinClips({ first, second2 }, error);
            expectEquals((int) joined.size(), 1, error);
            expectEquals((int) project.getArrangement().tracks[t].size(), 1);
            const auto c = project.getArrangement().tracks[t].front();
            expectEquals(c.start, (juce::int64) 0);
            expectEquals(c.length, second * 5 / 2);
            expectEquals(c.sourceLength, second * 5 / 2);
            expect(c.file.existsAsFile() && c.file.isAChildOf(project.getAudioFolder()), "a new file in the project");
            expectWithinAbsoluteError(sampleAt(c.file, 0.5), 0.5f, 0.001f, "the first clip");
            expectWithinAbsoluteError(sampleAt(c.file, 1.25), 0.0f, 0.0001f, "silence in the gap");
            expectWithinAbsoluteError(sampleAt(c.file, 2.0), 1.0f, 0.001f, "the second clip, with its gain");
            expectEquals(engine.getAudioTrack(t)->getClips().size(), 1, "one clip in the Edit");

            expect(project.undo());
            expectEquals((int) project.getArrangement().tracks[t].size(), 2, "undo gives back both clips");

            beginTest("Transposing moves every note, undoably");
            const int m = 4;
            engine.getTrack(m).midi = true;
            engine.syncNow();
            const auto bar = engine.getSamplesPerBar();
            auto* a = project.createMidiClip(m, 0, bar);
            auto* b = project.createMidiClip(m, 2 * bar, bar);
            expect(a != nullptr && b != nullptr);
            if (a == nullptr || b == nullptr)
                return;
            auto* um = &project.getEdit().getUndoManager();
            a->getSequence().addNote(60, te::BeatPosition::fromBeats(0.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
            a->getSequence().addNote(64, te::BeatPosition::fromBeats(1.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
            b->getSequence().addNote(67, te::BeatPosition::fromBeats(0.0), te::BeatDuration::fromBeats(1.0), 100, 0, um);
            um->beginNewTransaction();
            const auto aId = (juce::int64) a->itemID.getRawID(), bId = (juce::int64) b->itemID.getRawID();

            expect(project.transposeClips({ aId }, 12));
            expectEquals(a->getSequence().getNotes()[0]->getNoteNumber(), 72);
            expectEquals(a->getSequence().getNotes()[1]->getNoteNumber(), 76);
            expectEquals(b->getSequence().getNotes()[0]->getNoteNumber(), 67, "other clips are left alone");
            expect(project.undo());
            expectEquals(a->getSequence().getNotes()[0]->getNoteNumber(), 60, "undo");
            expect(project.transposeClips({ aId }, -1));
            expectEquals(a->getSequence().getNotes()[0]->getNoteNumber(), 59);

            beginTest("Joining MIDI clips merges their notes into one clip");
            joined = project.joinClips({ aId, bId }, error);
            expectEquals((int) joined.size(), 1, error);
            expectEquals((int) project.getArrangement().tracks[m].size(), 1);
            const auto mc = project.getArrangement().tracks[m].front();
            expectEquals(mc.start, (juce::int64) 0);
            expectEquals(mc.length, 3 * bar);
            if (auto* merged = project.getMidiClip(mc.id))
            {
                const auto& notes = merged->getSequence().getNotes();
                expectEquals(notes.size(), 3);
                if (notes.size() == 3)
                {
                    expectEquals(notes[2]->getNoteNumber(), 67);
                    expectWithinAbsoluteError(notes[2]->getStartBeat().inBeats(), 2.0 * engine.beatsPerBar.load(), 0.001,
                                              "the second clip's note keeps its place");
                }
            }
            else
                expect(false, "no merged clip");
        }
        dir.deleteRecursively();
    }
};

class MidiInputRoutingTests : public juce::UnitTest
{
public:
    MidiInputRoutingTests() : juce::UnitTest("MidiInputRouting", "8track") {}

    static te::VirtualMidiInputDevice* trackInput(AudioEngine& engine, int track)
    {
        for (auto& d : engine.getTracktion().getDeviceManager().getMidiInDevices())
            if (d->getName() == "8track Track " + juce::String(track + 1))
                return dynamic_cast<te::VirtualMidiInputDevice*>(d.get());
        return nullptr;
    }

    void runTest() override
    {
        AudioEngine engine;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(300);   // Tracktion's device scan makes the inputs
        Project project(engine);
        const int t = 2;

        beginTest("Each MIDI track records from its own input, filtered to its channel");
        auto* input = trackInput(engine, t);
        expect(input != nullptr, "a virtual input for the track");
        if (input == nullptr)
            return;

        engine.getTrack(t).midi = true;
        engine.syncNow();
        AudioEngine::MidiRouting routing;
        routing.channel = 3;
        engine.setMidiRouting(t, routing);
        expect(input->isChannelAllowed(3) && !input->isChannelAllowed(4), "only channel 3 gets through");

        bool targeted = false, physicalTargeted = false;
        for (auto* instance : project.getEdit().getAllInputDevices())
        {
            const bool toTrack = instance->getTargets().contains(engine.getAudioTrack(t)->itemID);
            if (&instance->getInputDevice() == input)
                targeted = toTrack;
            else if (instance->getInputDevice().getDeviceType() == te::InputDevice::physicalMidiDevice && toTrack)
                physicalTargeted = true;
        }
        expect(targeted, "the track's input feeds it");
        expect(!physicalTargeted, "hardware inputs reach it only through that input");

        engine.setMidiRouting(t, {});
        expect(input->isChannelAllowed(4), "any channel again");
    }
};

class MidiAssignTests : public juce::UnitTest
{
public:
    MidiAssignTests() : juce::UnitTest("MidiAssign", "8track") {}

    void runTest() override
    {
        auto dir = juce::File::createTempFile("8track-assign");
        dir.createDirectory();
        AudioEngine engine;
        {
            Project project(engine);
            juce::String error;

            auto targets = engine.getAutomationTargets(3);
            te::AutomatableParameter* lowCut = nullptr;
            te::AutomatableParameter* pan = nullptr;
            for (auto& target : targets)
            {
                if (target.name == "Low Cut") lowCut = target.param;
                if (target.name == "Pan") pan = target.param;
            }
            expect(lowCut != nullptr && pan != nullptr);
            if (lowCut == nullptr || pan == nullptr)
                return;
            const auto lowCutKey = AudioEngine::parameterKey(*lowCut), panKey = AudioEngine::parameterKey(*pan);

            beginTest("An assigned controller moves its parameter");
            expect(engine.findParameter(lowCutKey) == lowCut, "found by key on any track");
            engine.assignMidi(lowCutKey, 1, 74);
            engine.handleController(1, 74, 127);
            expectWithinAbsoluteError(lowCut->getCurrentNormalisedValue(), 1.0f, 0.001f);
            engine.handleController(1, 74, 0);
            expectWithinAbsoluteError(lowCut->getCurrentNormalisedValue(), 0.0f, 0.001f);
            engine.handleController(2, 74, 127);
            expectWithinAbsoluteError(lowCut->getCurrentNormalisedValue(), 0.0f, 0.001f, "another channel does nothing");

            beginTest("A controller moves one parameter, and a parameter follows one controller");
            engine.assignMidi(panKey, 1, 74);   // takes CC 74 from the low cut
            expect(!engine.getMidiAssignment(lowCutKey).has_value());
            expectEquals(engine.getMidiAssignment(panKey)->controller, 74);
            engine.assignMidi(panKey, 1, 10);   // moves pan to CC 10
            expectEquals((int) engine.getMidiAssignments().size(), 1);
            expectEquals(engine.getMidiAssignment(panKey)->controller, 10);

            beginTest("Assigning is undoable");
            expect(project.undo());
            expectEquals(engine.getMidiAssignment(panKey)->controller, 74, "back on CC 74");

            beginTest("Assignments are saved with the project");
            engine.assignMidi(lowCutKey, 5, 20);
            auto folder = dir.getChildFile("Assigned");
            expect(project.saveAs(folder, error), error);
            project.newProject();
            expect(engine.getMidiAssignments().empty(), "a new project has none");
            expect(project.load(folder, error), error);
            expectEquals((int) engine.getMidiAssignments().size(), 2);
            auto* reloaded = engine.findParameter(lowCutKey);
            expect(reloaded != nullptr, "the key still finds the parameter");
            engine.handleController(5, 20, 127);
            if (reloaded != nullptr)
                expectWithinAbsoluteError(reloaded->getCurrentNormalisedValue(), 1.0f, 0.001f);

            beginTest("Removing an assignment");
            engine.removeMidiAssignment(lowCutKey);
            expect(!engine.getMidiAssignment(lowCutKey).has_value());
            expectEquals((int) engine.getMidiAssignments().size(), 1);
        }
        dir.deleteRecursively();
    }
};

class RecordingPreviewTests : public juce::UnitTest
{
public:
    RecordingPreviewTests() : UnitTest("Recording waveform", "8track") {}

    void runTest() override
    {
        beginTest("A metronome transient stays at its beat as later blocks arrive");
        constexpr int beat = 24000, length = 49152, samplesPerPixel = 64;
        juce::AudioBuffer<float> audio(1, length);
        audio.clear();
        for (int i = 0; i < 96; ++i)
            audio.setSample(0, beat + i, i % 2 ? 1.0f : -1.0f);

        juce::AudioFormatManager formats;
        juce::AudioThumbnailCache cache(4);
        AudioUIBehaviour behaviour;
        for (int block : { 64, 127, 128, 512 })
        {
            auto thumb = behaviour.createAudioThumbnail(1024, formats, cache);
            thumb->reset(1, 48000, 0);
            for (int p = 0; p < length; p += block)
            {
                const int count = juce::jmin(block, length - p);
                thumb->addBlock(p, audio, p, count);
                expect(thumb->getNumSamplesFinished() <= p + count);
            }
            expectEquals(thumb->getNumSamplesFinished(), (juce::int64) length);
            juce::Image image(juce::Image::ARGB, length / samplesPerPixel, 100, true);
            juce::Graphics graphics(image);
            graphics.setColour(juce::Colours::white);
            thumb->drawChannels(graphics, image.getBounds(), 0, length / 48000.0, 1.0f);
            int first = -1, last = -1;
            for (int x = 0; x < image.getWidth(); ++x)
                if (image.getPixelAt(x, 10).getAlpha() > 32)
                {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            expectEquals(first, beat / samplesPerPixel);
            expectEquals(last, (beat + 95) / samplesPerPixel);
        }
    }
};

class RecordingWriterTests : public juce::UnitTest
{
public:
    RecordingWriterTests() : UnitTest("Recording writer", "8track") {}

    void runTest() override
    {
        beginTest("Eight interleaved stereo takes preserve every sample and their live peaks");
        AudioEngine engine;
        auto& teEngine = engine.getTracktion();
        auto& thread = teEngine.getWaveInputRecordingThread();
        te::WaveInputRecordingThread::ScopedInitialiser recording(thread);
        juce::WavAudioFormat wav;
        std::array<std::unique_ptr<te::AudioFileWriter>, 8> writers;
        std::array<te::RecordingThumbnailManager::Thumbnail::Ptr, 8> thumbs;
        const int frames = 32768;
        juce::AudioBuffer<float> audio(2, frames);
        for (int sample = 0; sample < frames; ++sample)
        {
            const float value = (float) ((sample % 251) - 125) / 256.0f;
            audio.setSample(0, sample, value);
            audio.setSample(1, sample, -value);
        }

        for (size_t t = 0; t < writers.size(); ++t)
        {
            auto file = juce::File::createTempFile("take.wav");
            writers[t] = std::make_unique<te::AudioFileWriter>(te::AudioFile(teEngine, file),
                                                               &wav, 2, 48000, 24, juce::StringPairArray(), 0);
            expect(writers[t]->isOpen());
            thumbs[t] = teEngine.getRecordingThumbnailManager().getThumbnailFor(file);
            thumbs[t]->reset(2, 48000);
        }

        for (int pos = 0; pos < frames; pos += 127)
            for (size_t t = 0; t < writers.size(); ++t)
                thread.addBlockToRecord(*writers[t], audio, pos, juce::jmin(127, frames - pos), thumbs[t]);

        for (size_t t = 0; t < writers.size(); ++t)
        {
            thread.waitForWriterToFinish(*writers[t]);
            writers[t]->closeForWriting();
            expectEquals(thumbs[t]->thumb->getNumSamplesFinished(), (juce::int64) frames);
            auto file = thumbs[t]->file;
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
            expect(reader != nullptr);
            if (reader != nullptr)
            {
                expectEquals(reader->lengthInSamples, (juce::int64) frames);
                juce::AudioBuffer<float> result(2, frames);
                expect(reader->read(&result, 0, frames, 0, true, true));
                for (int ch = 0; ch < 2; ++ch)
                    expect(std::equal(audio.getReadPointer(ch), audio.getReadPointer(ch) + frames,
                                      result.getReadPointer(ch)), "WAV samples differ from input");
            }
            reader.reset();
            file.deleteFile();
        }
    }
};

static RecordingWriterTests recordingWriterTests;
static RecordingPreviewTests recordingPreviewTests;
class SongwritingTests : public juce::UnitTest
{
public:
    SongwritingTests() : UnitTest("Songwriting", "8track") {}

    void runTest() override
    {
        auto dir = juce::File::createTempFile("songwriting");
        dir.createDirectory();
        {
            AudioEngine engine;
            Project project(engine);
            const auto sr = (int) engine.getSampleRate();
            juce::String error;
            auto song = dir.getChildFile("Song");
            beginTest("A fresh session is clean and tempo edits count before recording any clips");
            juce::MessageManager::getInstance()->runDispatchLoopUntil(120);
            expect(!project.isDirty());
            engine.bpm = 96;
            engine.syncNow();
            expect(project.isDirty() && project.getArrangement().isEmpty());
            beginTest("Save As preserves different recordings with the same name, including alternate takes");
            expect(project.prepareFolder(error), error);
            auto source = dir.getChildFile("sources");
            source.createDirectory();
            auto file1 = writeWav(source.getChildFile("vocal.wav"), sr, sr * 5, 0.25f);
            auto file2 = writeWav(dir.getChildFile("vocal.wav"), sr, sr * 5, 0.75f);
            song.getChildFile("Audio").createDirectory();
            auto existing = writeWav(song.getChildFile("Audio/vocal.wav"), sr, sr, 0.5f);
            auto time = [](double s) { return te::TimePosition::fromSeconds(s); };
            auto* track = engine.getAudioTrack(0);
            auto wave = track->insertWaveClip("vocal", file1, { { time(0), time(5) }, {} }, false);
            wave->addTake(file1);
            wave->addTake(file2);
            engine.keepLastFullPass(*wave);
            auto id = (juce::int64) wave->itemID.getRawID();
            project.chooseTake(id, 0);
            expect(project.saveAs(song, error), error);
            wave = nullptr;
            source.deleteRecursively();
            file2.deleteFile();
            expect(project.load(song, error), error);
            auto find = [&]() { return project.getArrangement().findClip(id); };
            expect(find() != nullptr);
            if (!find()) return;
            expectEquals(find()->takes.size(), 2);
            expectWithinAbsoluteError(ClipOperationTests::sampleAt(existing, 0), 0.5f, 0.001f);
            expectWithinAbsoluteError(ClipOperationTests::sampleAt(find()->file, 0), 0.25f, 0.001f);
            project.chooseTake(id, 1);
            expectWithinAbsoluteError(ClipOperationTests::sampleAt(find()->file, 0), 0.75f, 0.001f);
            expect(project.undo());
            expectWithinAbsoluteError(ClipOperationTests::sampleAt(find()->file, 0), 0.25f, 0.001f);

            beginTest("Duplicate a named range with crossing clips, a rest, and track automation in one undo step");
            project.addSection("Verse", {time(1), time(3)});
            project.addSection("Chorus", {time(3), time(5)});
            project.createMidiClip(1, sr, sr);
            auto* param = engine.getAudioTrack(0)->getVolumePlugin()->volParam.get();
            auto& curve = param->getCurve();
            auto* um = &project.getEdit().getUndoManager();
            curve.addPoint(time(1.5), 0.25f, 0, um);
            curve.addPoint(time(2.5), 0.75f, 0, um);
            auto& masterCurve = project.getEdit().getMasterVolumePlugin()->volParam->getCurve();
            masterCurve.addPoint(time(1.5), 0.5f, 0, um);
            masterCurve.addPoint(time(2.5), 0.125f, 0, um);
            auto* sections = project.getEdit().getArrangerTrack();
            project.duplicateSection(*dynamic_cast<te::ArrangerClip*>(sections->getClips().getFirst()));
            expectEquals(sections->getClips().size(), 3);
            int labelsAtExpectedPosition = 0;
            for (auto* label : sections->getClips())
            {
                const auto start = engine.toSamples(label->getPosition().getStart());
                if ((label->getName() == "Chorus" && start == sr * 5)
                     || (label->getName() == "Verse" && (start == sr || start == sr * 3)))
                    ++labelsAtExpectedPosition;
            }
            expectEquals(labelsAtExpectedPosition, 3);
            const auto& lane = project.getArrangement().tracks[0];
            expectEquals((int) lane.size(), 4);
            if (lane.size() == 4)
            {
                expectEquals(lane[2].start, (juce::int64) sr * 3);
                expectEquals(lane[2].sourceOffset, (juce::int64) sr);
                expectEquals(lane[3].start, (juce::int64) sr * 5);
            }
            const auto& midi = project.getArrangement().tracks[1];
            expectEquals((int) midi.size(), 2);
            if (midi.size() == 2)
                expectEquals(midi[1].end(), (juce::int64) sr * 4);
            expectWithinAbsoluteError(masterCurve.getValueAt(time(4.5), 0), 0.125f, 0.001f);
            expectWithinAbsoluteError(curve.getValueAt(time(3.5), 0), 0.25f, 0.001f);
            expectWithinAbsoluteError(curve.getValueAt(time(4.5), 0), 0.75f, 0.001f);
            expect(project.undo());
            expectEquals(sections->getClips().size(), 2);
            expectEquals((int) project.getArrangement().tracks[0].size(), 1);
            expectEquals(curve.getNumPoints(), 2);
            expect(project.save(error), error);
            expect(project.load(song, error), error);
            expectEquals(project.getEdit().getArrangerTrack()->getClips().getFirst()->getName(), juce::String("Verse"));
        }
        dir.deleteRecursively();
        beginTest("Chord and lyric entries follow beats, copy with sections, and survive reopening");
        {
            AudioEngine engine;
            Project project(engine);
            using Entry = Project::SongEntry;
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            auto& tempo = project.getEdit().tempoSequence;
            project.setSongNote(Entry::Chord, beat(0), "Am7");
            project.setSongNote(Entry::Lyric, beat(1), "First line\nSecond line");
            project.setSongNote(Entry::Chord, beat(4), "G/B");
            expect(project.addSection("Verse", { tempo.toTime(beat(0)), tempo.toTime(beat(4)) }).wasOk());
            auto* section = dynamic_cast<te::ArrangerClip*>(project.getEdit().getArrangerTrack()->getClips().getFirst());
            expect(section != nullptr);
            if (section != nullptr)
                project.duplicateSection(*section);
            auto chords = project.songNotes(Entry::Chord);
            expectEquals((int) chords.size(), 3);
            if (chords.size() == 3)
            {
                expectEquals(chords[1].text, juce::String("Am7"));
                expectWithinAbsoluteError(chords[1].beat.inBeats(), 4.0, 1e-9);
                expectWithinAbsoluteError(chords[2].beat.inBeats(), 8.0, 1e-9);
            }
            auto lyrics = project.songNotes(Entry::Lyric);
            expectEquals((int) lyrics.size(), 2);
            if (lyrics.size() == 2)
                expectWithinAbsoluteError(lyrics[1].beat.inBeats(), 5.0, 1e-9);
            expect(project.undo());
            expectEquals((int) project.songNotes(Entry::Lyric).size(), 1);
            expect(project.redo());
            engine.bpm = 60;
            engine.syncNow();
            expectWithinAbsoluteError(tempo.toTime(project.songNotes(Entry::Lyric).front().beat).inSeconds(), 1.0, 1e-9);

            auto destination = juce::File::createTempFile("8track-writing");
            juce::String error;
            expect(project.saveAs(destination, error), error);
            expect(project.load(destination, error), error);
            lyrics = project.songNotes(Entry::Lyric);
            expectEquals((int) lyrics.size(), 2);
            if (!lyrics.empty())
                expectEquals(lyrics.front().text, juce::String("First line\nSecond line"));
            project.setSongNote(Entry::Lyric, beat(1), {}, lyrics.front().entry);
            expectEquals((int) project.songNotes(Entry::Lyric).size(), 1);
            expect(project.undo());
            expectEquals((int) project.songNotes(Entry::Lyric).size(), 2);
            destination.deleteRecursively();
        }

        beginTest("A draft line drops at the lyric beat and stays in the draft");
        {
            AudioEngine engine;
            Project project(engine);
            using Entry = Project::SongEntry;
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            project.setLyricDraft(juce::String::fromUTF8("A draft line\r\n\r\nUnplaced déjà vu\rLast line\r\n"));
            const auto draft = juce::String::fromUTF8("A draft line\n\nUnplaced déjà vu\nLast line\n");
            expectEquals(project.getLyricDraft(), draft);
            expect(project.isDirty());
            expect(project.songNotes(Entry::Lyric).empty());

            TimelineView view;
            Ruler ruler(engine, project, view);
            ruler.setSize(800, 160);
            ruler.setSongwritingExpanded(true);
            ruler.layoutLyrics(800, 2);
            const auto& tempo = project.getEdit().tempoSequence;
            const int x = juce::roundToInt(view.xFor(engine.toSamples(tempo.toTime(beat(4.18)))));
            const int lyricY = Ruler::baseHeight + Ruler::chordHeight + 5;
            const int chordY = Ruler::baseHeight + 5;
            const juce::var line(new LyricLineDrag(project.getEdit().state, "A draft line"));
            const juce::var stale(new LyricLineDrag(juce::ValueTree("EDIT"), "Wrong project"));
            ruler.itemDropped({ line, nullptr, { x, chordY } });
            ruler.itemDropped({ stale, nullptr, { x, lyricY } });
            expect(project.songNotes(Entry::Lyric).empty());
            ruler.itemDropped({ line, nullptr, { x, lyricY } });
            const auto lyrics = project.songNotes(Entry::Lyric);
            expectEquals(static_cast<int>(lyrics.size()), 1);
            if (!lyrics.empty())
            {
                expect(lyrics.front().beat == beat(4));
                expectEquals(lyrics.front().text, juce::String("A draft line"));
            }
            expectEquals(project.getLyricDraft(), draft);
            expect(project.undo());
            expect(project.songNotes(Entry::Lyric).empty());
            expectEquals(project.getLyricDraft(), draft);
            expect(project.redo());

            beginTest("Timeline import follows beat and row order, and the draft exports and reopens");
            project.setSongNote(Entry::Lyric, beat(1), "First on the timeline");
            project.setSongNote(Entry::Chord, beat(2), "Am");
            project.setSongNote(Entry::Lyric, beat(4), "Second at four");
            expectEquals(project.timelineLyricText(), juce::String("First on the timeline\nA draft line\nSecond at four"));
            const auto parent = juce::File::createTempFile("8track-lyric-draft");
            const auto folder = parent.getChildFile("Song title");
            juce::String error;
            expect(project.saveAs(folder, error), error);
            const auto sheet = parent.getChildFile("Lyrics.txt");
            expect(sheet.replaceWithText(project.lyricSheetText(), false, false, "\n"));
            expectEquals(sheet.loadFileAsString(), juce::String("Song title\n==========\n\n") + draft);
            project.newProject();
            expect(project.getLyricDraft().isEmpty());
            expect(project.load(folder, error), error);
            expectEquals(project.getLyricDraft(), draft);
            expectEquals(project.timelineLyricText(), juce::String("First on the timeline\nA draft line\nSecond at four"));
            parent.deleteRecursively();
        }

        beginTest("Lyrics keep beat precedence and preserve the order of lines sharing a beat");
        {
            AudioEngine engine;
            Project project(engine);
            using Entry = Project::SongEntry;
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            project.setSongNote(Entry::Lyric, beat(4), "Oldest at four");
            project.setSongNote(Entry::Chord, beat(4), "G");
            project.setSongNote(Entry::Lyric, beat(1), "Earlier");
            project.setSongNote(Entry::Lyric, beat(4), "Second at four");
            project.setSongNote(Entry::Lyric, beat(7), "Moved");
            auto notes = [&] { return project.songNotes(Entry::Lyric); };
            auto texts = [&]
            {
                juce::StringArray lines;
                for (const auto& note : notes())
                    lines.add(note.text);
                return lines;
            };
            auto initial = notes();
            expectEquals(static_cast<int>(initial.size()), 4);
            if (initial.size() == 4)
            {
                const auto oldest = initial[1].entry, moved = initial[3].entry;
                expect(!project.adjacentLyric(initial[0].entry, 1).isValid());
                project.moveLyric(moved, beat(4));
                expect(notes().back().beat == beat(4));
                expect(texts() == juce::StringArray({ "Earlier", "Oldest at four", "Second at four", "Moved" }));
                expect(project.undo());
                expect(notes().back().beat == beat(7));
                expect(project.redo());

                project.reorderLyric(moved, -1);
                project.reorderLyric(oldest, 1);
                expect(texts() == juce::StringArray({ "Earlier", "Moved", "Oldest at four", "Second at four" }));
                project.mergeLyric(oldest, -1);
                expect(texts() == juce::StringArray({ "Earlier", "Moved Oldest at four", "Second at four" }));
                expect(project.undo());
                expect(texts() == juce::StringArray({ "Earlier", "Moved", "Oldest at four", "Second at four" }));
                project.mergeLyric(moved, 1);
                expect(texts() == juce::StringArray({ "Earlier", "Moved Oldest at four", "Second at four" }));

                auto destination = juce::File::createTempFile("8track-lyric-order");
                juce::String error;
                expect(project.saveAs(destination, error), error);
                expect(project.load(destination, error), error);
                expect(texts() == juce::StringArray({ "Earlier", "Moved Oldest at four", "Second at four" }));
                auto restored = notes();
                if (restored.size() == 3)
                {
                    project.setSongNote(Entry::Lyric, restored[2].beat, "Updated", restored[2].entry);
                    expect(texts() == juce::StringArray({ "Earlier", "Moved Oldest at four", "Updated" }));
                }
                destination.deleteRecursively();
            }
        }

        beginTest("Place chord notes into MIDI clips and undo an approved audio-track conversion");
        {
            AudioEngine engine;
            Project project(engine);
            auto& tempo = project.getEdit().tempoSequence;
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            using Entry = Project::SongEntry;
            project.setSongNote(Entry::Chord, beat(0), "G/B");
            project.setSongNote(Entry::Chord, beat(2), "Fmaj7");
            expectWithinAbsoluteError(project.chordDuration(beat(0)).inBeats(), 2.0, 0.00001);
            expectWithinAbsoluteError(project.chordDuration(beat(2)).inBeats(), 4.0, 0.00001);

            const te::TimeRange range(tempo.toTime(beat(4)), tempo.toTime(beat(6)));
            expect(project.placeChord(0, range, "G/B", false) == nullptr);
            expect(!engine.getTrack(0).midi.load());
            expect(project.getArrangement().tracks[0].empty());

            auto* chord = project.placeChord(0, range, "G/B", true);
            expect(chord != nullptr);
            expect(engine.getTrack(0).midi.load());
            if (chord != nullptr)
            {
                expectEquals(chord->getName(), juce::String("G/B"));
                expect(chord->getPosition().time == range);
                expectEquals(chord->getSequence().getNumNotes(), 4);
                juce::Array<int> pitches;
                for (auto* note : chord->getSequence().getNotes())
                {
                    pitches.add(note->getNoteNumber());
                    expectWithinAbsoluteError(note->getStartBeat().inBeats(), 0.0, 0.00001);
                    expectWithinAbsoluteError(note->getLengthBeats().inBeats(), 2.0, 0.00001);
                }
                expect(pitches == juce::Array<int>({ 59, 67, 71, 74 }));
            }
            expect(!project.canPlaceChord(0, range));
            expect(project.placeChord(0, range, "Fmaj7", false) == nullptr);
            expect(project.undo());
            expect(!engine.getTrack(0).midi.load());
            expect(project.getArrangement().tracks[0].empty());
            expect(project.redo());
            expect(engine.getTrack(0).midi.load());
            expectEquals((int) project.getArrangement().tracks[0].size(), 1);
            const te::TimeRange following(tempo.toTime(beat(6)), tempo.toTime(beat(10)));
            expect(project.placeChord(0, following, "Fmaj7", false) != nullptr);
            expectEquals((int) project.getArrangement().tracks[0].size(), 2);
            expect(project.placeChord(1, following, "unsupported", true) == nullptr);
            expect(!engine.getTrack(1).midi.load());
        }

        beginTest("Drum patterns of 16 to 64 steps edit in pages, save, and place independent MIDI clips");
        {
            AudioEngine engine;
            Project project(engine);
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            expectEquals(project.getDrumPattern(0).steps, 16);
            expect(!project.getDrumPattern(0).hasHits());
            expect(project.placeDrumPattern(0, 0, beat(0)) == nullptr);

            auto bank = project.getEdit().state.getOrCreateChildWithName("eightTrackDrumPatterns", nullptr);
            juce::ValueTree legacy("drumPattern");
            legacy.setProperty("slot", 1, nullptr);
            legacy.setProperty("steps", 24, nullptr);
            legacy.setProperty("pad16", 1 << 23, nullptr);
            bank.appendChild(legacy, nullptr);
            expectEquals(project.getDrumPattern(1).steps, 24);
            expect(project.getDrumPattern(1).hasHit(15, 23), "old integer masks still load");
            juce::ValueTree longer("drumPattern");   // saved when patterns went up to 256 steps
            longer.setProperty("slot", 6, nullptr);
            longer.setProperty("steps", 128, nullptr);
            longer.setProperty("pad1", "b1" + juce::String::repeatedString("0", 98) + "1", nullptr);   // steps 1 and 100
            bank.appendChild(longer, nullptr);
            expectEquals(project.getDrumPattern(6).steps, 64, "a longer pattern loads as 64 steps");
            expect(project.getDrumPattern(6).hasHit(0, 0), "keeping the hits in them");
            bank.removeChild(longer, nullptr);

            DrumPattern pattern;
            pattern.steps = 64;
            pattern.rows[0].set(0).set(4);
            pattern.rows[15].set(63);
            project.setDrumPattern(7, pattern);
            expect(project.getDrumPattern(7) == pattern);
            expect(!project.getDrumPattern(0).hasHits());
            expect(project.undo());
            expect(!project.getDrumPattern(7).hasHits());
            expect(project.redo());
            expect(project.getDrumPattern(7) == pattern);
            expect(project.placeDrumPattern(0, 7, beat(2)) == nullptr, "audio tracks reject patterns");

            engine.setTrackMidi(0, true);
            TimelineView view;
            view.samplesPerPixel = engine.getSamplesPerBeat() / 40.0;
            TrackLanes lanes(engine, project, view);
            lanes.setSize(960, 2 * TrackLanes::kTrackHeight);
            juce::String error;
            if (auto* drums = dynamic_cast<DrumSampler*>(engine.setInstrument(0, DrumSampler::xmlTypeName, error)))
            {
                DrumSamplerEditor editor(*drums, &engine, &project);
                editor.showPatterns(true);
                auto* panel = dynamic_cast<DrumSequencer*>(editor.findChildWithID("drum-patterns"));
                expect(panel != nullptr && panel->isVisible());
                if (panel != nullptr)
                {
                    juce::TextButton* slot = nullptr;
                    for (auto* child : panel->getChildren())
                        if (child->getTitle() == Project::drumPatternName(7))
                            slot = dynamic_cast<juce::TextButton*>(child);
                    expect(slot != nullptr);
                    if (slot != nullptr)
                    {
                        slot->onClick();
                        auto* length = dynamic_cast<juce::Slider*>(panel->findChildWithID("pattern-length"));
                        auto* pages = dynamic_cast<juce::ComboBox*>(panel->findChildWithID("pattern-page"));
                        expect(length != nullptr && pages != nullptr);
                        if (length != nullptr && pages != nullptr)
                        {
                            expectWithinAbsoluteError(length->getValue(), 64.0, 1.0e-6);
                            length->setValue(63.0, juce::sendNotificationSync);
                            expectEquals(project.getDrumPattern(7).steps, 63);
                            expectEquals(pages->getItemText(3), juce::String("Steps 49-63"));
                            length->setValue(24.4, juce::sendNotificationSync);
                            expectEquals(project.getDrumPattern(7).steps, 24, "whole steps only");
                            length->setValue(200.0, juce::sendNotificationSync);
                            expectEquals(project.getDrumPattern(7).steps, 64, "64 steps at most");
                            length->setValue(1.0, juce::sendNotificationSync);
                            expectEquals(project.getDrumPattern(7).steps, 16, "16 at least");
                            if (auto* division = dynamic_cast<juce::ComboBox*>(panel->findChildWithID("pattern-division")))
                            {
                                expectEquals(division->getSelectedId(), 16);
                                division->setSelectedId(32, juce::sendNotificationSync);
                                expectEquals(project.getDrumPattern(7).stepsPerBar, 32, "the menu sets steps per bar");
                                division->setSelectedId(16, juce::sendNotificationSync);
                            }
                            else
                                expect(false, "there's a steps-per-bar menu");
                            length->setValue(64.0, juce::sendNotificationSync);
                            pages->setSelectedId(4, juce::sendNotificationSync);
                            expectEquals(pages->getText(), juce::String("Steps 49-64"));
                            const auto lastCell = juce::Point<float>(static_cast<float>(panel->getWidth() - 20),
                                                                      static_cast<float>(panel->getHeight() - 20));
                            const auto now = juce::Time::getCurrentTime();
                            const auto click = juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                lastCell, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                                0, 0, 0, 0, 0, panel, panel, now, lastCell, now, 1, false);
                            panel->mouseDown(click);
                            expect(project.getDrumPattern(7).getStep(15, 63).hit == DrumPattern::Hit::Accent,
                                   "the last cell edits step 64: a hit becomes an accent");
                            panel->mouseDown(click);
                            expect(project.getDrumPattern(7).getStep(15, 63).hit == DrumPattern::Hit::Flam, "then a flam");
                            panel->mouseDown(click);
                            expect(project.getDrumPattern(7).getStep(15, 63).hit == DrumPattern::Hit::Ghost, "then a ghost");
                            panel->mouseDown(click);
                            expect(!project.getDrumPattern(7).hasHit(15, 63), "then nothing");
                            panel->mouseDown(click);
                            expect(project.getDrumPattern(7) == pattern, "and a plain hit again");
                        }
                        const auto x = juce::roundToInt(view.xFor(engine.toSamples(project.getEdit().tempoSequence.toTime(beat(2)))));
                        const juce::DragAndDropTarget::SourceDetails drag(7, slot, { x, TrackLanes::kTrackHeight / 2 });
                        expect(lanes.isInterestedInDragSource(drag));
                        lanes.itemDragEnter(drag);
                        lanes.itemDropped(drag);
                    }
                }
            }
            else
                expect(false, error);
            const auto& selection = lanes.getSelection();
            expectEquals(static_cast<int>(selection.size()), 1, "dropping a slot selects the new clip");
            const auto clipId = selection.empty() ? juce::int64(0) : *selection.begin();
            auto* clip = project.getMidiClip(clipId);
            expect(clip != nullptr);
            if (clip != nullptr)
            {
                expectEquals(clip->getName(), juce::String("Drum Pattern 8"));
                const auto range = project.getEdit().tempoSequence.toBeats(clip->getPosition().time);
                expectWithinAbsoluteError(range.getStart().inBeats(), 2.0, 1.0e-6);
                expectWithinAbsoluteError(range.getLength().inBeats(), 16.0, 1.0e-6, "64 steps: 4 bars of 16ths");
                const auto& notes = clip->getSequence().getNotes();
                expectEquals(notes.size(), 3);
                if (notes.size() == 3)
                {
                    expectEquals(notes[0]->getNoteNumber(), 48);
                    expectEquals(notes[2]->getNoteNumber(), 63);
                    expectWithinAbsoluteError(notes[1]->getStartBeat().inBeats(), 1.0, 1.0e-6);
                    expectWithinAbsoluteError(notes[2]->getStartBeat().inBeats(), 15.75, 1.0e-6);
                }
            }
            expect(project.placeDrumPattern(0, 7, beat(4)) == nullptr, "overlapping clips are rejected");
            expect(project.placeDrumPattern(0, 7, beat(18)) != nullptr, "clips can touch");
            expectEquals(static_cast<int>(project.getArrangement().tracks[0].size()), 2);
            expect(project.undo());
            expectEquals(static_cast<int>(project.getArrangement().tracks[0].size()), 1);

            pattern.steps = 16;
            project.setDrumPattern(7, pattern);
            if (auto* original = project.getMidiClip(clipId))
                expectEquals(original->getSequence().getNumNotes(), 3, "slot edits leave placed clips alone");
            if (auto* shorter = project.placeDrumPattern(0, 7, beat(18)))
                expectEquals(shorter->getSequence().getNumNotes(), 2, "only steps within the length are placed");
            else
                expect(false, "the shortened pattern can be placed");

            auto destination = juce::File::createTempFile("8track-drum-patterns");
            expect(project.saveAs(destination, error), error);
            expect(project.load(destination, error), error);
            expect(project.getDrumPattern(7) == pattern);
            expect(project.getDrumPattern(1).hasHit(15, 23));
            expectEquals(static_cast<int>(project.getArrangement().tracks[0].size()), 2);
            destination.deleteRecursively();
        }

        beginTest("Drum hits: accent, flam, ghost and rolls play as notes, save, undo, and old patterns stay plain hits");
        {
            using Step = DrumPattern::Step;
            using Hit = DrumPattern::Hit;
            using Ramp = DrumPattern::Ramp;

            // Clicking cycles none, hit, accent, flam, ghost, none, keeping the roll
            Step s;
            std::vector<Hit> seen;
            for (int i = 0; i < 4; ++i)
            {
                s = DrumPattern::nextHit(s);
                seen.push_back(s.hit);
            }
            expect(seen == std::vector<Hit> { Hit::Hit, Hit::Accent, Hit::Flam, Hit::Ghost } && s.on);
            expect(!DrumPattern::nextHit(s).on, "after a ghost, none");
            expect(DrumPattern::nextHit({ false, Hit::Hit, 3, Ramp::Fade }).roll == 3, "the roll stays");

            DrumPattern pattern;
            pattern.setStep(0, 0, { true, Hit::Accent, 0, Ramp::Even });
            pattern.setStep(1, 4, { true, Hit::Flam, 0, Ramp::Even });
            pattern.setStep(2, 8, { true, Hit::Ghost, 0, Ramp::Even });
            pattern.setStep(3, 12, { true, Hit::Hit, 3, Ramp::Fade });   // 4 hits, fading
            pattern.setStep(4, 12, { true, Hit::Hit, 5, Ramp::Swell });  // 8 hits, swelling
            pattern.setStep(5, 2, { true, Hit::Hit, 0, Ramp::Even });

            struct Note { double at, length; int velocity; };
            auto notes = [&](int pad, int step)
            {
                std::vector<Note> out;
                pattern.forEachNote(pad, step, [&](double at, double length, int v) { out.push_back({ at, length, v }); });
                return out;
            };
            expect(notes(0, 0).size() == 1 && notes(0, 0)[0].velocity == 127, "an accent is full velocity");
            expect(notes(5, 2).size() == 1 && notes(5, 2)[0].velocity == 100, "a hit is 100");
            expect(notes(2, 8).size() == 1 && notes(2, 8)[0].velocity == DrumPattern::kGhostVelocity, "a ghost is quiet");
            const auto flam = notes(1, 4);
            expect(flam.size() == 2 && flam[0].at == 0.0 && flam[0].velocity < flam[1].velocity
                       && std::abs(flam[1].at - DrumPattern::kFlamBeats / pattern.stepBeats()) < 1.0e-9, "a flam: a softer grace note, then the hit");
            const auto fade = notes(3, 12);
            expect(fade.size() == 4, "a roll of 4");
            expect(fade.size() == 4 && fade[0].velocity == 100 && fade[3].velocity == 30 && fade[1].velocity > fade[2].velocity,
                   "fading from the hit's velocity");
            expect(fade.size() == 4 && std::abs(fade[2].at - 0.5) < 1.0e-9 && fade[3].at < 1.0, "spread evenly inside the step");
            const auto swell = notes(4, 12);
            expect(swell.size() == 8 && swell.front().velocity < swell.back().velocity && swell.back().velocity == 100, "a swell of 8");
            expect(notes(6, 0).empty(), "an empty step plays nothing");

            // Clearing a step forgets its type, so equal-sounding patterns compare equal
            auto cleared = pattern;
            cleared.setStep(0, 0, {});
            cleared.setStep(0, 0, { true, Hit::Hit, 0, Ramp::Even });
            auto plain = pattern;
            plain.setStep(0, 0, {});
            plain.rows[0].set(0);
            expect(cleared == plain, "a re-entered hit is plain");

            AudioEngine engine;
            Project project(engine);
            const auto beat = [](double value) { return te::BeatPosition::fromBeats(value); };
            project.setDrumPattern(2, pattern);
            expect(project.getDrumPattern(2) == pattern, "hit types and rolls are kept");
            auto changed = pattern;
            changed.setStep(3, 12, { true, Hit::Accent, 1, Ramp::Even });
            project.setDrumPattern(2, changed);
            expect(project.getDrumPattern(2) == changed, "changing only a roll is saved");
            expect(project.undo());
            expect(project.getDrumPattern(2) == pattern, "and undoes");

            auto bank = project.getEdit().state.getOrCreateChildWithName("eightTrackDrumPatterns", nullptr);
            juce::ValueTree old("drumPattern");
            old.setProperty("slot", 3, nullptr);
            old.setProperty("pad1", "b10001", nullptr);
            bank.appendChild(old, nullptr);
            expect(project.getDrumPattern(3).getStep(0, 4) == Step { true, Hit::Hit, 0, Ramp::Even }, "an older pattern's hits are plain");

            engine.setTrackMidi(0, true);
            if (auto* clip = project.placeDrumPattern(0, 2, beat(0)))
            {
                auto& seq = clip->getSequence();
                expectEquals(seq.getNumNotes(), 1 + 2 + 1 + 4 + 8 + 1, "every hit, grace note and roll hit is a note");
                auto velocitiesAt = [&](int noteNumber)
                {
                    std::vector<std::pair<double, int>> out;
                    for (auto* n : seq.getNotes())
                        if (n->getNoteNumber() == noteNumber)
                            out.push_back({ n->getStartBeat().inBeats(), n->getVelocity() });
                    std::sort(out.begin(), out.end());
                    return out;
                };
                const auto kick = velocitiesAt(DrumSampler::kFirstNote);
                expect(kick.size() == 1 && kick[0].second == 127);
                const auto flamNotes = velocitiesAt(DrumSampler::kFirstNote + 1);
                expect(flamNotes.size() == 2 && std::abs(flamNotes[0].first - 1.0) < 1.0e-9
                           && std::abs(flamNotes[1].first - (1.0 + DrumPattern::kFlamBeats)) < 1.0e-9,
                       "the flam lands on beat 2, the hit just after");
                const auto roll = velocitiesAt(DrumSampler::kFirstNote + 3);
                expect(roll.size() == 4 && std::abs(roll[1].first - (3.0 + 0.0625)) < 1.0e-9, "the roll is in sixteenths of the step");
            }
            else
                expect(false, "the pattern places");

            // Steps per bar: 8, 16 or 32. The length stays in steps, so the clip's length follows
            auto eighths = pattern;
            eighths.stepsPerBar = 8;
            project.setDrumPattern(4, eighths);
            expectEquals(project.getDrumPattern(4).stepsPerBar, 8, "8 steps a bar is kept");
            if (auto* clip = project.placeDrumPattern(0, 4, beat(32)))
            {
                const auto range = project.getEdit().tempoSequence.toBeats(clip->getPosition().time);
                expectWithinAbsoluteError(range.getLength().inBeats(), 8.0, 1.0e-6, "16 eighths: 2 bars");
                double snare = -1.0;
                for (auto* n : clip->getSequence().getNotes())
                    if (n->getNoteNumber() == DrumSampler::kFirstNote + 2)
                        snare = n->getStartBeat().inBeats();
                expectWithinAbsoluteError(snare, 4.0, 1.0e-9, "step 9 of eighths is beat 5");
                double grace = -1.0, hit = -1.0;
                for (auto* n : clip->getSequence().getNotes())
                    if (n->getNoteNumber() == DrumSampler::kFirstNote + 1)
                        (grace < 0.0 ? grace : hit) = n->getStartBeat().inBeats();
                expectWithinAbsoluteError(std::abs(hit - grace), DrumPattern::kFlamBeats, 1.0e-9, "a flam's gap doesn't grow with the step");
            }
            else
                expect(false, "the eighths pattern places");
            auto thirtySeconds = pattern;
            thirtySeconds.stepsPerBar = 32;
            thirtySeconds.steps = 64;
            expectWithinAbsoluteError(thirtySeconds.lengthBeats(), 8.0, 1.0e-9, "64 thirty-seconds: 2 bars");
            auto odd = pattern;
            odd.stepsPerBar = 12;
            project.setDrumPattern(5, odd);
            expectEquals(project.getDrumPattern(5).stepsPerBar, 16, "only 8, 16 or 32");
            expectEquals(project.getDrumPattern(3).stepsPerBar, 16, "older patterns are 16 a bar");

            auto destination = juce::File::createTempFile("8track-drum-hits");
            juce::String error;
            expect(project.saveAs(destination, error), error);
            expect(project.load(destination, error), error);
            expect(project.getDrumPattern(2) == pattern, "hit types and rolls survive saving");
            expectEquals(project.getDrumPattern(4).stepsPerBar, 8, "and so does the division");
            destination.deleteRecursively();
        }

        beginTest("Factory drum kits load all pads and preview through the stopped transport");
        {
            AudioEngine engine;
            auto& dm = engine.getTracktion().getDeviceManager();
            auto& io = dm.getHostedAudioDeviceInterface();
            te::HostedAudioDeviceInterface::Parameters settings;
            settings.sampleRate = 48000;
            settings.blockSize = 128;
            io.initialise(settings);
            io.prepareToPlay(settings.sampleRate, settings.blockSize);
            dm.dispatchPendingUpdates();
            Project project(engine);
            engine.setTrackMidi(0, true);
            juce::String error;
            using Kit = DrumSampler::FactoryKit;
            auto* drums = dynamic_cast<DrumSampler*>(engine.setInstrument(0, DrumSampler::xmlTypeName, error));
            expect(drums != nullptr, error);
            if (drums != nullptr)
            {
                auto checkKit = [&](Kit kit)
                {
                    expect(drums->getFactoryKit() == kit);
                    for (int pad = 0; pad < DrumSampler::kPads; ++pad)
                    {
                        const auto sample = drums->getPadSample(pad);
                        expect(sample != nullptr && sample->audio.getMagnitude(0, sample->audio.getNumSamples()) > 0.05f,
                               "audible factory sound on pad " + juce::String(pad + 1));
                        expect(drums->getPadFile(pad) == juce::File(), "factory sounds need no external files");
                    }
                };
                checkKit(Kit::acoustic);
                expect(drums->loadPad(0, {}, error), error);
                expect(drums->getPadSample(0) == nullptr);
                drums->loadFactoryKit(Kit::electronic);
                checkKit(Kit::electronic);
                expect(project.undo());
                expect(drums->getFactoryKit() == Kit::acoustic && drums->getPadSample(0) == nullptr);
                expect(project.undo());
                checkKit(Kit::acoustic);
                expect(project.redo() && project.redo());
                checkKit(Kit::electronic);

                engine.setTrackRoute(0, AudioEngine::Route::Main, false);
                drums->pads[4].sends[1]->setParameter(0.0f, juce::sendNotification);
                engine.syncNow();
                dm.dispatchPendingUpdates();
                project.getEdit().dispatchPendingUpdatesSynchronously();
                DrumPattern pattern;
                pattern.rows[4].set(0).set(4);
                project.setDrumPattern(0, pattern);
                {
                    DrumSamplerEditor editor(*drums, &engine, &project);
                    auto findButton = [](juce::Component& parent, const juce::String& text)
                    {
                        for (auto* child : parent.getChildren())
                            if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr && button->getButtonText() == text)
                                return button;
                        return static_cast<juce::TextButton*>(nullptr);
                    };
                    if (auto* patternsTab = findButton(editor, "Patterns"))
                        patternsTab->onClick();
                    auto* panel = editor.findChildWithID("drum-patterns");
                    expect(panel != nullptr && panel->isVisible(), "the sampler opens the pattern editor");
                    auto* preview = panel != nullptr ? findButton(*panel, "Preview") : nullptr;
                    expect(preview != nullptr && preview->isEnabled(), "the embedded pattern can be previewed");
                    if (preview != nullptr)
                        preview->onClick();
                    const auto pos = engine.getPosition();
                    juce::AudioBuffer<float> block(2, settings.blockSize);
                    juce::MidiBuffer midi;
                    float peak = 0.0f;
                    bool wrapped = false;
                    int previousStep = -1;
                    for (int i = 0; i < 1000; ++i)
                    {
                        block.clear();
                        io.processBlock(block, midi);
                        peak = juce::jmax(peak, block.getMagnitude(0, settings.blockSize));
                        const int currentStep = drums->getPreviewStep();
                        wrapped |= currentStep >= 0 && currentStep < previousStep;
                        previousStep = currentStep;
                    }
                    expect(peak > 0.02f, "the pad send carries the live preview to Bus 2 while stopped");
                    expect(wrapped, "the preview loops at the pattern length");
                    expect(engine.getPosition() == pos && engine.getState() == AudioEngine::State::Stopped,
                           "the song stays stopped at the same position");
                    expect(project.getArrangement().tracks[0].empty(), "preview creates no clips");

                    const int stepBeforeSwitch = drums->getPreviewStep();
                    if (auto* padsTab = findButton(editor, "Pads"))
                        padsTab->onClick();
                    auto* stopPreview = findButton(editor, "Stop Preview");
                    expect(stopPreview != nullptr && stopPreview->isVisible(), "the Pads tab can stop the preview");
                    const int blocksPerStep = static_cast<int>(std::ceil(engine.getSamplesPerBeat() * DrumPattern().stepBeats() / settings.blockSize));
                    for (int i = 0; i < blocksPerStep; ++i)
                    {
                        block.clear();
                        io.processBlock(block, midi);
                    }
                    const int stepAfterSwitch = drums->getPreviewStep();
                    const int progress = (stepAfterSwitch - stepBeforeSwitch + pattern.steps) % pattern.steps;
                    expect(stepAfterSwitch >= 0 && progress >= 1 && progress <= 2, "preview advances across the tab switch");

                    if (stopPreview != nullptr)
                        stopPreview->onClick();
                    for (int i = 0; i < 32; ++i)
                    {
                        block.clear();
                        io.processBlock(block, midi);
                    }
                    expectEquals(drums->getPreviewStep(), -1);
                    expect(block.getMagnitude(0, settings.blockSize) < 1.0e-6f);
                }

                expect(drums->loadPad(0, {}, error), error);
                const auto destination = juce::File::createTempFile("8track-factory-drums");
                expect(project.saveAs(destination, error), error);
                expect(project.load(destination, error), error);
                if (auto* reopened = dynamic_cast<DrumSampler*>(engine.getInstrument(0)))
                {
                    expect(reopened->getFactoryKit() == Kit::electronic);
                    expect(reopened->getPadSample(0) == nullptr, "an explicitly cleared pad stays empty");
                    expect(reopened->getPadSample(1) != nullptr);
                }
                else
                    expect(false, "the drum sampler reopened");
                destination.deleteRecursively();
            }
        }

        beginTest("Recover a played impulse on the selected armed track during playback");
        AudioEngine engine;
        auto& dm = engine.getTracktion().getDeviceManager();
        auto& io = dm.getHostedAudioDeviceInterface();
        te::HostedAudioDeviceInterface::Parameters params;
        params.sampleRate = 48000;
        params.blockSize = 128;
        io.initialise(params);
        io.prepareToPlay(params.sampleRate, params.blockSize);
        dm.dispatchPendingUpdates();
        for (auto* input : dm.getWaveInputDevices())
            input->setEnabled(true);
        dm.dispatchPendingUpdates();
        Project project(engine);
        engine.getTrack(0).armed = true;
        engine.getTrack(1).armed = true;
        engine.syncNow();
        engine.setPosition(48000 * 4);
        engine.play();
        juce::AudioBuffer<float> block(2, params.blockSize);
        juce::MidiBuffer notes;
        juce::int64 impulsePosition = 0;
        for (int i = 0; i < 240; ++i)
        {
            block.clear();
            if (i == 120)
            {
                auto* ctx = project.getEdit().getCurrentPlaybackContext();
                impulsePosition = engine.toSamples(ctx->globalStreamTimeToEditTime(dm.getCurrentStreamTime()));
                block.setSample(0, 0, 0.75f);
            }
            io.processBlock(block, notes);
        }
        expect(project.canRecall(0));
        expect(!project.canRecall(2));
        juce::String error;
        expect(project.recall(0, error), error);
        expect(project.getArrangement().tracks[1].empty());
        const auto& clips = project.getArrangement().tracks[0];
        expectEquals((int) clips.size(), 1);
        if (clips.size() == 1)
        {
            auto clip = clips.front();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(clip.file.createInputStream().release(), true));
            expect(reader != nullptr);
            if (reader)
            {
                juce::AudioBuffer<float> data(1, (int) reader->lengthInSamples);
                reader->read(&data, 0, data.getNumSamples(), 0, true, false);
                const float* samples = data.getReadPointer(0);
                auto peak = std::max_element(samples, samples + data.getNumSamples());
                expectWithinAbsoluteError(*peak, 0.75f, 0.001f);
                expectWithinAbsoluteError(clip.start - clip.sourceOffset + (peak - samples), impulsePosition, (juce::int64) 1);
            }
            expect(project.undo());
            expect(project.getArrangement().tracks[0].empty());
        }
        engine.stop();

        beginTest("Recover buffered MIDI notes into the armed MIDI track and undo");
        engine.getTrack(2).midi = true;
        engine.getTrack(2).armed = true;
        engine.syncNow();
        auto id = engine.getAudioTrack(2)->itemID;
        te::MidiInputDevice* midiInput = nullptr;
        for (auto* instance : project.getEdit().getAllInputDevices())
            if (instance->isRecordingActive(id))
                midiInput = dynamic_cast<te::MidiInputDevice*>(&instance->getInputDevice());
        expect(midiInput != nullptr);
        if (midiInput)
        {
            auto on = juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100);
            auto off = juce::MidiMessage::noteOff(1, 64);
            midiInput->handleIncomingMessage(on);
            juce::Thread::sleep(5);
            midiInput->handleIncomingMessage(off);
            expect(project.canRecall(2));
            error.clear();
            expect(project.recall(2, error), error);
            auto& midiLane = project.getArrangement().tracks[2];
            expectEquals((int) midiLane.size(), 1);
            if (!midiLane.empty())
            {
                auto* recovered = project.getMidiClip(midiLane.front().id);
                expect(recovered != nullptr && recovered->getSequence().getNumNotes() == 1);
                expect(project.undo());
                expect(project.getArrangement().tracks[2].empty());
            }
        }
        beginTest("Chord preview plays on the device only when switched on");
        expect(ChordPreview::notesFor("Fmaj7") == juce::Array<int>({ 65, 69, 72, 76 }));
        expect(ChordPreview::notesFor("G/B") == juce::Array<int>({ 59, 67, 71, 74 }));
        expect(ChordPreview::notesFor("unsupported").isEmpty());
        engine.previewChord("Am7");
        block.clear();
        io.processBlock(block, notes);
        expectWithinAbsoluteError(block.getMagnitude(0, params.blockSize), 0.0f, 1e-6f);
        engine.setChordPreviewEnabled(true);
        engine.previewChord("Am7");
        block.clear();
        io.processBlock(block, notes);
        expect(block.getMagnitude(0, params.blockSize) > 0.001f);
        engine.setChordPreviewEnabled(false);
        block.clear();
        io.processBlock(block, notes);
        expectWithinAbsoluteError(block.getMagnitude(0, params.blockSize), 0.0f, 1e-6f);
        project.getFolder().deleteRecursively();
    }
};

class MetronomeModeTests : public juce::UnitTest
{
public:
    MetronomeModeTests() : UnitTest("Metronome modes", "8track") {}

    void runTest() override
    {
        beginTest("A playback count-in is audible with record-only enabled, then the song starts without click");
        AudioEngine engine;
        auto& devices = engine.getTracktion().getDeviceManager();
        auto& io = devices.getHostedAudioDeviceInterface();
        te::HostedAudioDeviceInterface::Parameters settings;
        settings.sampleRate = 48000;
        settings.blockSize = 128;
        io.initialise(settings);
        io.prepareToPlay(settings.sampleRate, settings.blockSize);
        devices.dispatchPendingUpdates();
        Project project(engine);
        auto dir = juce::File::createTempFile("8track-metronome");
        dir.createDirectory();
        auto source = writeWav(dir.getChildFile("guide.wav"), settings.sampleRate, 5 * 48000, 0.125f);
        juce::String error;
        expect(project.importAudio(source, 0, 0, error), error);
        engine.bpm = 120;
        engine.metronomeOn = true;
        engine.metronomeRecordOnly = true;
        engine.countInOn = false;
        engine.playbackCountInOn = true;
        engine.syncNow();
        const auto start = (juce::int64) (3 * settings.sampleRate);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
        engine.setPosition(start);
        engine.play();
        expectWithinAbsoluteError(project.getEdit().getTransport().getPlaybackCountInEndSeconds(), 3.0, 1e-8);

        juce::AudioBuffer<float> block(2, settings.blockSize);
        juce::MidiBuffer midi;
        int clicks = 0, silent = 0, song = 0;
        float peakAfter = 0;
        bool clickActive = false;
        for (int i = 0; i < 920; ++i)
        {
            if (i % 8 == 0)
                juce::Thread::sleep(2);
            block.clear();
            io.processBlock(block, midi);
            auto* context = project.getEdit().getCurrentPlaybackContext();
            const double time = context->getUnloopedPosition().inSeconds();
            const auto magnitude = block.getMagnitude(0, settings.blockSize);
            if (time < 2.99)
            {
                const bool active = magnitude > 0.0001f;
                if (active && !clickActive) ++clicks;
                clickActive = active;
                if (magnitude < 1e-6f) ++silent;
            }
            else if (time > 3.05)
            {
                peakAfter = juce::jmax(peakAfter, magnitude);
                if (block.getRMSLevel(0, 0, settings.blockSize) > 0.05f) ++song;
            }
        }
        expect(clicks >= 4 && silent > 100, "count-in clicks and silent gaps");
        expect(song > 10, "song audio after the count-in");
        expectWithinAbsoluteError(peakAfter, 0.125f, 0.001f, "no metronome mixed into the playing song");
        engine.stop();
        expectEquals(engine.getPosition(), start);

        beginTest("Playback count-in runs once before a loop starting at bar one");
        engine.getTrack(0).mute = true;
        engine.metronomeOn = false;
        engine.punchIn = 0;
        engine.punchOut = 48000;
        engine.setRangeMode(AudioEngine::RangeMode::Loop);
        engine.syncNow();
        engine.setPosition(0);
        engine.play();
        bool heardCountIn = false;
        double earliest = 2.0;
        float peakDuringLoop = 0;
        for (int i = 0; i < 1450; ++i)
        {
            block.clear();
            io.processBlock(block, midi);
            const auto magnitude = block.getMagnitude(0, settings.blockSize);
            auto* context = project.getEdit().getCurrentPlaybackContext();
            const auto elapsed = context->getUnloopedPosition().inSeconds();
            earliest = juce::jmin(earliest, elapsed);
            if (elapsed < 0)
                heardCountIn = heardCountIn || magnitude > 0.001f;
            if (elapsed > 0.05)
                peakDuringLoop = juce::jmax(peakDuringLoop, magnitude);
        }
        expect(heardCountIn, "first timeline position " + juce::String(earliest));
        expectWithinAbsoluteError(peakDuringLoop, 0.0f, 1e-6f);
        expect(project.getEdit().getCurrentPlaybackContext()->getUnloopedPosition().inSeconds() > 1.0);
        engine.stop();

        beginTest("Count-in and record-only preferences are recalled per project");
        auto saved = dir.getChildFile("Song");
        expect(project.saveAs(saved, error), error);
        project.newProject();
        expect(!engine.playbackCountInOn.load());
        expect(project.load(saved, error), error);
        expect(!engine.countInOn.load());
        expect(engine.playbackCountInOn.load());
        expect(engine.metronomeRecordOnly.load());
        dir.deleteRecursively();
    }
};

static MetronomeModeTests metronomeModeTests;
class LyricLayoutTests : public juce::UnitTest
{
public:
    LyricLayoutTests() : UnitTest("Lyric rows", "8track") {}

    void runTest() override
    {
        beginTest("Later lyrics go below earlier overlapping lyrics and reuse rows when there is room");
        expect(lyricRowsFor({ { 0, 45 }, { 20, 60 }, { 40, 90 }, { 60, 76 }, { 100, 120 } })
               == std::vector<int>({ 0, 1, 2, 3, 0 }));
        expect(lyricRowsFor({ { 0, 20 }, { 20, 40 }, { 40, 60 } }) == std::vector<int>({ 0, 0, 0 }));

        beginTest("An inserted earlier lyric takes the top row while an edited short line can fit beside it");
        expect(lyricRowsFor({ { 40, 100 }, { 70, 130 } }) == std::vector<int>({ 0, 1 }));
        expect(lyricRowsFor({ { 20, 85 }, { 40, 100 }, { 70, 130 } }) == std::vector<int>({ 0, 1, 2 }));
        expect(lyricRowsFor({ { 20, 40 }, { 40, 100 }, { 70, 130 } }) == std::vector<int>({ 0, 0, 1 }));
    }
};

static LyricLayoutTests lyricLayoutTests;
class PluginWindowRequestTests : public juce::UnitTest
{
public:
    PluginWindowRequestTests() : UnitTest("Plugin windows", "8track") {}

    void runTest() override
    {
        struct Windows final : te::UIBehaviour
        {
            std::unique_ptr<juce::Component> createPluginWindow(te::PluginWindowState&) override
            {
                ++created;
                return std::make_unique<juce::Component>();
            }

            int created = 0;
        };

        auto ui = std::make_unique<Windows>();
        auto* windows = ui.get();
        AudioEngine engine(std::move(ui));
        Project project(engine);
        auto* plugin = engine.getAudioTrack(0)->getVolumePlugin();
        auto& state = *plugin->windowState;
        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(150); };

        beginTest("An explicit open waits out a modal and opens exactly one window");
        juce::Component modal;
        modal.enterModalState(false);
        plugin->showWindowExplicitly();
        plugin->showWindowExplicitly();
        pump();
        expectEquals(windows->created, 0);
        modal.exitModalState(0);
        pump();
        expect(state.isWindowShowing());
        expectEquals(windows->created, 1);
        plugin->showWindowExplicitly();
        expectEquals(windows->created, 1);
        state.closeWindowExplicitly();
        expect(!state.isWindowShowing());

        beginTest("Closing a deferred window cancels the pending open");
        modal.enterModalState(false);
        plugin->showWindowExplicitly();
        state.closeWindowExplicitly();
        modal.exitModalState(0);
        pump();
        expect(!state.isWindowShowing());
        expectEquals(windows->created, 1);
    }
};

static PluginWindowRequestTests pluginWindowRequestTests;
class TunerTests : public juce::UnitTest
{
public:
    TunerTests() : UnitTest("Tuner", "8track") {}

    void runTest() override
    {
        beginTest("The pitch detector identifies guitar and bass fundamentals, including harmonic-rich notes");
        constexpr double rate = 12000;
        std::array<float, TunerCapture::analysisSize> wave {};
        for (double frequency : { 30.868, 41.203, 82.407, 440.0, 659.255, 1318.51 })
        {
            for (size_t i = 0; i < wave.size(); ++i)
            {
                const double phase = juce::MathConstants<double>::twoPi * frequency * static_cast<double>(i) / rate;
                wave[i] = static_cast<float>(0.15 * std::sin(phase) + 0.25 * std::sin(2 * phase));
            }
            const auto detected = TunerCapture::frequencyOf(wave, rate);
            expect(detected > 0);
            if (detected > 0)
                expectWithinAbsoluteError(1200.0 * std::log2(detected / frequency), 0.0, 3.0);
        }

        wave.fill(0.0f);
        expectWithinAbsoluteError(TunerCapture::frequencyOf(wave, rate), 0.0, 1e-9);
        juce::Random random(17);
        for (auto& sample : wave)
            sample = random.nextFloat() - 0.5f;
        expectWithinAbsoluteError(TunerCapture::frequencyOf(wave, rate), 0.0, 1e-9);

        beginTest("The tuner picks up armed hardware inputs and leaves device output silent");
        AudioEngine engine;
        auto& dm = engine.getTracktion().getDeviceManager();
        auto& io = dm.getHostedAudioDeviceInterface();
        te::HostedAudioDeviceInterface::Parameters settings;
        settings.sampleRate = 48000;
        settings.blockSize = 128;
        io.initialise(settings);
        io.prepareToPlay(settings.sampleRate, settings.blockSize);
        dm.dispatchPendingUpdates();
        dm.setAllWaveInputsToNumChannels(1);
        for (auto* input : dm.getWaveInputDevices())
            input->setEnabled(true);
        dm.dispatchPendingUpdates();

        Project project(engine);
        engine.getTrack(0).inputChannel = 0;
        engine.getTrack(1).inputChannel = 1;
        engine.getTrack(1).armed = true;
        engine.syncNow();
        const auto inputs = engine.getArmedInputChannels();
        expectEquals(static_cast<int>(inputs.size()), 1);
        if (inputs.empty())
            return;
        expectEquals(inputs.front().channel, 1);
        TunerCapture tuner(dm);
        TunerCapture::Channels channels;
        channels.fill(-1);
        channels.front() = inputs.front().channel;
        tuner.setChannels(channels);

        juce::AudioBuffer<float> block(2, settings.blockSize);
        juce::MidiBuffer midi;
        auto feedInputs = [&]
        {
            float peak = 0;
            for (int i = 0; i < 80; ++i)
            {
                for (int s = 0; s < settings.blockSize; ++s)
                {
                    const double phase = juce::MathConstants<double>::twoPi * (i * settings.blockSize + s) / settings.sampleRate;
                    block.setSample(0, s, static_cast<float>(0.7 * std::sin(440 * phase)));
                    block.setSample(1, s, static_cast<float>(0.15 * std::sin(110 * phase)));
                }
                io.processBlock(block, midi);
                peak = juce::jmax(peak, block.getMagnitude(0, settings.blockSize));
            }
            return peak;
        };
        expectWithinAbsoluteError(feedInputs(), 0.0f, 1e-7f);
        const auto reading = tuner.readPitch();
        expect(reading.has_value());
        if (reading)
        {
            expectEquals(reading->channel, 1);
            expectWithinAbsoluteError(reading->frequency, 110.0, 0.1);
        }

        engine.getTrack(0).armed = true;
        expectEquals(static_cast<int>(engine.getArmedInputChannels().size()), 2);
        channels[0] = 0;
        channels[1] = 1;
        tuner.setChannels(channels);
        expectWithinAbsoluteError(feedInputs(), 0.0f, 1e-7f);
        const auto strongest = tuner.readPitch();
        expect(strongest.has_value());
        if (strongest)
        {
            expectEquals(strongest->channel, 0);
            expectWithinAbsoluteError(strongest->frequency, 440.0, 1.0);
        }

        engine.getTrack(0).armed = false;
        engine.getTrack(1).armed = false;
        expect(engine.getArmedInputChannels().empty());
        channels.fill(-1);
        tuner.setChannels(channels);
        expect(!tuner.readPitch());
    }
};

static TunerTests tunerTests;
class PianoRollTests : public juce::UnitTest
{
public:
    PianoRollTests() : UnitTest("Piano roll", "8track") {}

    void runTest() override
    {
        beginTest("Right-drag selects a group, and paste preserves its note data across clips");
        AudioEngine engine;
        Project project(engine);
        using te::BeatPosition;
        const auto beat = [](double value) { return BeatPosition::fromBeats(value); };
        const auto length = te::BeatDuration::fromBeats(0.25);
        auto& tempo = project.getEdit().tempoSequence;
        auto sample = [&](double n) { return engine.toSamples(tempo.toTime(beat(n))); };
        engine.setTrackMidi(0, true);
        auto* source = project.createMidiClip(0, 0, sample(8));
        auto* destination = project.createMidiClip(0, sample(8), sample(8));
        expect(source != nullptr && destination != nullptr);
        if (source == nullptr || destination == nullptr)
            return;

        auto* first = source->getSequence().addNote(60, beat(0.31), length, 73, 2, nullptr);
        first->setNoteOffVelocity(31, nullptr);
        source->getSequence().addNote(64, beat(0.68), length, 108, 0, nullptr);
        source->getSequence().addNote(68, beat(5), length, 110, 0, nullptr);

        PianoRoll roll(engine, project);
        constexpr int width = 980, height = 406;
        roll.setSize(width, height);
        roll.setVisible(true);
        const auto clipID = [](const auto& clip) { return static_cast<juce::int64>(clip.itemID.getRawID()); };
        roll.setClip(clipID(*source));

        const auto pointAt = [](double n, float y)
        {
            return juce::Point<float>(PianoRoll::kKeyboardWidth + static_cast<float>(n) * (width - PianoRoll::kKeyboardWidth) / 8.0f, y);
        };
        const int rows = (height - PianoRoll::kHeaderHeight - PianoRoll::kExpressionHeight) / PianoRoll::kRowHeight;
        const auto pitches = source->getSequence().getNoteNumberRange();
        const int top = (pitches.getStart() + pitches.getEnd()) / 2 + rows / 2;
        const auto y = [top](int pitch) { return static_cast<float>(PianoRoll::kHeaderHeight + (top - pitch) * PianoRoll::kRowHeight + 4); };
        const auto now = juce::Time::getCurrentTime();
        auto mouse = [&](auto position, auto origin, int buttons, bool dragged = false)
        {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(buttons),
                                   0, 0, 0, 0, 0, &roll, &roll, now, origin, now, 1, dragged);
        };
        const auto a = pointAt(0.1, y(65)), b = pointAt(1.6, y(59));
        roll.mouseDown(mouse(a, a, juce::ModifierKeys::rightButtonModifier));
        roll.mouseDrag(mouse(b, a, juce::ModifierKeys::rightButtonModifier, true));
        roll.mouseUp(mouse(b, a, 0, true));
        expect(roll.hasSelectedNotes());
        const juce::ModifierKeys command(juce::ModifierKeys::commandModifier);
        roll.keyPressed(juce::KeyPress('C', command, 'c'));
        roll.setClip(clipID(*destination));
        engine.setPosition(sample(10));
        roll.keyPressed(juce::KeyPress('V', command, 'v'));

        auto& notes = destination->getSequence();
        expectEquals(notes.getNumNotes(), 2);
        if (notes.getNumNotes() != 2)
            return;
        expectWithinAbsoluteError(notes.getNote(0)->getStartBeat().inBeats(), 2.0, 1e-6);
        expectWithinAbsoluteError(notes.getNote(1)->getStartBeat().inBeats(), 2.37, 1e-6);
        expectEquals(notes.getNote(0)->getNoteNumber(), 60);
        expectEquals(notes.getNote(1)->getNoteNumber(), 64);
        expectEquals(notes.getNote(0)->getNoteOffVelocity(), 31);
        expectEquals(notes.getNote(1)->getVelocity(), 108);

        beginTest("Quantize snaps selected starts to the chosen grid without changing duration");
        juce::ComboBox* grid = nullptr;
        juce::ComboBox* expression = nullptr;
        for (auto* child : roll.getChildren())
            if (auto* box = dynamic_cast<juce::ComboBox*>(child))
            {
                if (box->getItemText(0) == "1/2") grid = box;
                else if (box->getItemText(0) == "Velocity") expression = box;
            }
        expect(grid != nullptr && expression != nullptr);
        if (grid == nullptr || expression == nullptr)
            return;
        for (const int denominator : { 2, 4, 8, 16, 32 })
            expect(grid->indexOfItemId(denominator) >= 0);
        grid->setSelectedId(8, juce::dontSendNotification);
        roll.keyPressed(juce::KeyPress('Q', {}, 'q'));
        expectWithinAbsoluteError(notes.getNote(1)->getStartBeat().inBeats(), 2.5, 1e-6);
        expectWithinAbsoluteError(notes.getNote(1)->getLengthBeats().inBeats(), length.inBeats(), 1e-6);
        expect(project.undo());
        expectWithinAbsoluteError(notes.getNote(1)->getStartBeat().inBeats(), 2.37, 1e-6);

        beginTest("Dragging a velocity bar changes the selected notes by the same amount");
        auto valueY = [](int value)
        {
            return height - 6 - static_cast<float>(value) * (PianoRoll::kExpressionHeight - 12) / 127;
        };
        const auto velocity = pointAt(2, valueY(73)), louder = velocity.translated(0, -5);
        roll.mouseDown(mouse(velocity, velocity, juce::ModifierKeys::leftButtonModifier));
        roll.mouseDrag(mouse(louder, velocity, juce::ModifierKeys::leftButtonModifier, true));
        roll.mouseUp(mouse(louder, velocity, 0, true));
        expectEquals(notes.getNote(0)->getVelocity(), 83);
        expectEquals(notes.getNote(1)->getVelocity(), 118);
        expect(project.undo());
        expectEquals(notes.getNote(0)->getVelocity(), 73);

        beginTest("CC points can be added, moved, and removed with undo");
        expression->setSelectedId(66, juce::dontSendNotification); // CC 64, sustain
        const auto controller = pointAt(1.01, valueY(64)), shifted = pointAt(1.51, valueY(95));
        roll.mouseDown(mouse(controller, controller, juce::ModifierKeys::leftButtonModifier));
        roll.mouseDrag(mouse(shifted, controller, juce::ModifierKeys::leftButtonModifier, true));
        roll.mouseUp(mouse(shifted, controller, 0, true));
        auto* cc = notes.getControllerEventAt(beat(1.5), 64);
        expect(cc != nullptr);
        if (cc != nullptr)
        {
            expect(cc->getBeatPosition() == beat(1.5));
            expectEquals(cc->getControllerValue() >> 7, 95);
        }
        roll.mouseDown(mouse(shifted, shifted, juce::ModifierKeys::rightButtonModifier));
        roll.mouseUp(mouse(shifted, shifted, 0));
        expectEquals(notes.getNumControllerEvents(), 0);
        expect(project.undo());
        expectEquals(notes.getNumControllerEvents(), 1);

        beginTest("Piano roll arrows move selected notes without changing spacing");
        roll.keyPressed(juce::KeyPress('A', command, 0));
        expect(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, {}, 0)));
        expectWithinAbsoluteError(notes.getNote(0)->getStartBeat().inBeats(), 2.5, 1e-6);
        expectWithinAbsoluteError(notes.getNote(1)->getStartBeat().inBeats(), 2.87, 1e-6);
        expect(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
        expectEquals(notes.getNote(0)->getNoteNumber(), 72);
        expectEquals(notes.getNote(1)->getNoteNumber(), 76);
        expect(project.undo());
        expect(project.undo());

        beginTest("Piano roll duplicate places a copy after the selection and is undoable");
        expect(roll.keyPressed(juce::KeyPress('D', command, 0)));
        expectEquals(notes.getNumNotes(), 4);
        if (notes.getNumNotes() == 4)
        {
            expectWithinAbsoluteError(notes.getNote(2)->getStartBeat().inBeats(), 2.62, 1e-6);
            expectWithinAbsoluteError(notes.getNote(3)->getStartBeat().inBeats(), 2.99, 1e-6);
            expectEquals(notes.getNote(2)->getNoteNumber(), 60);
            expectEquals(notes.getNote(3)->getVelocity(), 108);
        }
        expect(project.undo());
        expectEquals(notes.getNumNotes(), 2);
    }
};

class ShortcutTests : public juce::UnitTest
{
public:
    ShortcutTests() : UnitTest("Keyboard shortcuts", "8track") {}

    void runTest() override
    {
        using namespace Shortcuts;
        beginTest("Identical keys dispatch according to focus and text input keeps its keys");
        const auto key = [](int code, int flags = 0) { return juce::KeyPress(code, flags, 0); };
        const auto verify = [&](Context context, const juce::KeyPress& k, Command command)
        {
            const auto* binding = Shortcuts::find(context, k);
            expect(binding != nullptr && binding->context == context && binding->command == command);
        };
        verify(Context::Arrangement, key(juce::KeyPress::leftKey), cmdPrevious);
        verify(Context::PianoRoll, key(juce::KeyPress::leftKey), cmdPrevious);
        verify(Context::Songwriting, key(juce::KeyPress::leftKey), cmdPrevious);
        verify(Context::PianoRoll, key('d', juce::ModifierKeys::commandModifier), cmdDuplicate);
        verify(Context::Songwriting, key(juce::KeyPress::returnKey), cmdEditSongNote);
        expect(Shortcuts::find(Context::PianoRoll, key('S')) == nullptr);
        expect(Shortcuts::find(Context::Mixer, key(juce::KeyPress::backspaceKey)) == nullptr);
        expect(Shortcuts::find(Context::Text, key(juce::KeyPress::spaceKey)) == nullptr);
        expect(Shortcuts::find(Context::Text, key('Z', juce::ModifierKeys::commandModifier)) == nullptr);
        expect(Shortcuts::find(Context::Arrangement, key('S', juce::ModifierKeys::altModifier)) == nullptr);
        const auto* help = Shortcuts::find(Context::PianoRoll, key('/', juce::ModifierKeys::commandModifier));
        expect(help != nullptr && help->command == cmdShowShortcuts);
        const auto* tuner = Shortcuts::find(Context::PianoRoll, key('t'));
        expect(tuner != nullptr && tuner->command == cmdShowTuner);
        expect(Shortcuts::find(Context::Text, key('t')) == nullptr);

        beginTest("The songwriting cursor deletes only the selected entry and clears the target on navigation");
        AudioEngine engine;
        Project project(engine);
        TimelineView timeline;
        Ruler ruler(engine, project, timeline);
        ruler.setSize(800, 160);
        ruler.setSongwritingExpanded(true);
        using Entry = Project::SongEntry;
        const auto first = te::BeatPosition::fromBeats(2), second = te::BeatPosition::fromBeats(3);
        project.setSongNote(Entry::Chord, first, "Cm");
        project.setSongNote(Entry::Chord, second, "G7");
        project.setSongNote(Entry::Lyric, first, "This line stays");
        const auto clickChord = [&](te::BeatPosition beat)
        {
            const auto x = timeline.xFor(engine.toSamples(project.getEdit().tempoSequence.toTime(beat)));
            const juce::Point<float> point(x + 3.0f, Ruler::baseHeight + 10.0f);
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), point,
                                     juce::ModifierKeys::leftButtonModifier, 0, 0, 0, 0, 0,
                                     &ruler, &ruler, now, point, now, 1, false);
            ruler.mouseDown(e);
            ruler.mouseUp(e);
        };
        clickChord(first);
        expect(ruler.hasSelectedSongNote());
        ruler.performSongShortcut(cmdNext);
        expect(!ruler.hasSelectedSongNote());
        ruler.performSongShortcut(cmdDelete);
        expectEquals(static_cast<int>(project.songNotes(Entry::Chord).size()), 2);
        clickChord(second);
        ruler.performSongShortcut(cmdDelete);
        const auto remaining = project.songNotes(Entry::Chord);
        expectEquals(static_cast<int>(remaining.size()), 1);
        if (!remaining.empty())
            expectEquals(remaining.front().text, juce::String("Cm"));
        expectEquals(static_cast<int>(project.songNotes(Entry::Lyric).size()), 1);
        expect(project.undo());
        expectEquals(static_cast<int>(project.songNotes(Entry::Chord).size()), 2);

        beginTest("Shortcut help shows the shared bindings and filters the list");
        ShortcutHelp page;
        auto* search = page.findChildWithID("shortcut-search");
        juce::ListBox* list = nullptr;
        for (auto* child : page.getChildren())
            if (auto* box = dynamic_cast<juce::ListBox*>(child))
                list = box;
        expect(search != nullptr && list != nullptr);
        if (auto* editor = dynamic_cast<juce::TextEditor*>(search); editor != nullptr && list != nullptr)
        {
            auto* model = list->getListBoxModel();
            const auto allRows = model->getNumRows();
            editor->setText("Quantize", false);
            editor->onTextChange();
            expect(allRows > 2);
            expectEquals(model->getNumRows(), 2);
            expect(model->getNameForRow(1).contains("Quantize"));
            editor->setText(JUCE_MAC ? "Cmd+E" : "Ctrl+E", false);
            editor->onTextChange();
            expectEquals(model->getNumRows(), 2);
            expect(model->getNameForRow(1).contains("Export audio"));
        }
    }
};

/** 8track's own effects: in the insert slots like any plugin, and what each one does to sound. */
class EffectTests : public juce::UnitTest
{
public:
    EffectTests() : juce::UnitTest("Effects", "8track") {}

    static constexpr double kRate = 48000.0;

    /** An effect outside any track, initialised for offline processing. */
    template <typename T>
    struct Standalone
    {
        Standalone(te::Edit& edit)
        {
            plugin = edit.getPluginCache().createNewPlugin(T::xmlTypeName, {});
            effect = dynamic_cast<T*>(plugin.get());
            plugin->baseClassInitialise({ te::TimePosition(), kRate, 512 });
        }
        ~Standalone() { plugin->baseClassDeinitialise(); }
        te::Plugin::Ptr plugin;
        T* effect = nullptr;
        T* operator->() { return effect; }
    };

    static void set(te::AutomatableParameter* p, float v) { p->setParameter(v, juce::sendNotification); }

    static void render(te::Plugin& p, juce::AudioBuffer<float>& b, bool playing = false, double startSeconds = 0.0)
    {
        for (int s = 0; s < b.getNumSamples(); s += 512)
        {
            const int n = juce::jmin(512, b.getNumSamples() - s);
            const te::TimeRange range(te::TimePosition::fromSeconds(startSeconds + s / kRate), te::TimeDuration::fromSeconds(n / kRate));
            te::PluginRenderContext fc(&b, s, n, nullptr, 0.0, range, playing, false, false, false);
            p.applyToBuffer(fc);
        }
    }

    static float sampleAt(const juce::File& file, double seconds)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
        if (reader == nullptr)
            return -100.0f;
        juce::AudioBuffer<float> buffer((int) reader->numChannels, 1);
        reader->read(&buffer, 0, 1, (juce::int64) (seconds * reader->sampleRate), true, true);
        return buffer.getSample(0, 0);
    }

    static juce::AudioBuffer<float> sine(double hz, double seconds, float level = 0.5f, int channels = 2)
    {
        juce::AudioBuffer<float> b(channels, (int) (seconds * kRate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < channels; ++ch)
                b.setSample(ch, i, level * (float) std::sin(juce::MathConstants<double>::twoPi * hz * i / kRate));
        return b;
    }

    static float rms(const juce::AudioBuffer<float>& b, int ch, double from, double to)
    {
        const int start = (int) (from * kRate), end = juce::jmin(b.getNumSamples(), (int) (to * kRate));
        return b.getRMSLevel(ch, start, end - start);
    }

    /** The strongest frequency within 10% of a guess: a DFT scanned in 0.05 Hz steps. */
    static double frequency(const juce::AudioBuffer<float>& b, double from, double to, double guess)
    {
        const int start = (int) (from * kRate), end = (int) (to * kRate);
        double best = 0, bestPower = -1;
        for (double hz = guess * 0.9; hz <= guess * 1.1; hz += 0.05)
        {
            double re = 0, im = 0;
            const double w = juce::MathConstants<double>::twoPi * hz / kRate;
            for (int i = start; i < end; ++i)
            {
                re += b.getSample(0, i) * std::cos(w * i);
                im += b.getSample(0, i) * std::sin(w * i);
            }
            if (re * re + im * im > bestPower)
                bestPower = re * re + im * im, best = hz;
        }
        return best;
    }

    /** Level of one frequency in dB (a DFT bin, Hann windowed), relative to full scale. */
    static float levelAt(const juce::AudioBuffer<float>& b, double hz, double from, double to)
    {
        const int start = (int) (from * kRate), end = (int) (to * kRate), n = end - start;
        double re = 0, im = 0, wsum = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * i / n);
            const double x = b.getSample(0, start + i) * w;
            re += x * std::cos(juce::MathConstants<double>::twoPi * hz * (start + i) / kRate);
            im += x * std::sin(juce::MathConstants<double>::twoPi * hz * (start + i) / kRate);
            wsum += w;
        }
        return juce::Decibels::gainToDecibels((float) (2.0 * std::sqrt(re * re + im * im) / wsum), -200.0f);
    }

    void expectNear(double actual, double expected, double relative, const juce::String& what)
    {
        expect(std::abs(actual - expected) <= relative * expected, what + ": " + juce::String(actual) + ", expected " + juce::String(expected));
    }

    void runTest() override
    {
        AudioEngine engine;
        auto dir = juce::File::createTempFile("8track-effects");
        dir.createDirectory();
        {
            Project project(engine);
            auto& edit = project.getEdit();
            juce::String error;

            beginTest("Each effect goes in an insert slot, undoes, and offers its controls for automation");
            std::vector<EffectPlugin::Type> types;
            for (auto& t : EffectPlugin::getTypes())
                if (t.kind == EffectPlugin::Kind::Effect)
                    types.push_back(t);
            expect(types.size() >= 7);
            std::map<juce::String, int> trackOf;   // the track each effect went on
            for (size_t i = 0; i < types.size(); ++i)
            {
                const int track = (int) i % kNumTracks;
                trackOf[types[i].xmlType] = track;
                auto* plugin = engine.addEffect(track, types[i].xmlType, error);
                expect(plugin != nullptr, error);
                const auto inserts = engine.getInserts(track);
                expect(!inserts.empty() && inserts.back() == plugin, types[i].name);
                expectEquals(plugin->getName(), types[i].name);
                const auto targets = engine.getAutomationTargets(track);
                expect(std::any_of(targets.begin(), targets.end(), [&](auto& t) { return t.group == types[i].name; }),
                       types[i].name + " can be automated");
            }
            auto find = [&](const char* xmlType) -> te::Plugin*
            {
                for (auto* p : engine.getInserts(trackOf[xmlType]))
                    if (p->getPluginType() == xmlType)
                        return p;
                return nullptr;
            };
            expect(engine.addEffect(7, "eightTrackNothing", error) == nullptr && error.isNotEmpty());
            const int lastTrack = trackOf[types.back().xmlType];
            const auto before = engine.getInserts(lastTrack).size();
            edit.getUndoManager().undo();
            expectEquals((int) engine.getInserts(lastTrack).size(), (int) before - 1, "undo removes the last one");
            edit.getUndoManager().redo();
            expectEquals((int) engine.getInserts(lastTrack).size(), (int) before, "and redo brings it back");

            beginTest("Effects follow the song tempo");
            auto* delay = dynamic_cast<DelayEffect*>(find(DelayEffect::xmlTypeName));
            expect(delay != nullptr);
            engine.bpm = 90.0;
            engine.syncNow();
            expectWithinAbsoluteError(delay->getTempo(), 90.0, 1.0e-6);
            expectWithinAbsoluteError(delay->getDelaySeconds(), 60.0 / 90.0, 1.0e-6, "a synced quarter note");

            beginTest("Effect settings and bypass survive save and reopen");
            set(delay->feedback, 0.6f);
            set(delay->pingPong, 1.0f);
            dynamic_cast<FilterEffect*>(find(FilterEffect::xmlTypeName))->mode->setParameter(2.0f, juce::sendNotification);
            find(GateEffect::xmlTypeName)->setEnabled(false);
            auto saved = dir.getChildFile("WithEffects");
            expect(project.saveAs(saved, error), error);
            project.newProject();
            expect(project.load(saved, error), error);
            if (auto* reopened = dynamic_cast<DelayEffect*>(find(DelayEffect::xmlTypeName)))
            {
                expectWithinAbsoluteError(reopened->feedback->getCurrentValue(), 0.6f, 1.0e-4f);
                expect(reopened->pingPong->getCurrentValue() > 0.5f);
                expectWithinAbsoluteError(reopened->getTempo(), 90.0, 1.0e-6, "and the tempo");
            }
            else expect(false, "the delay came back");
            if (auto* filter = dynamic_cast<FilterEffect*>(find(FilterEffect::xmlTypeName)))
                expectEquals(juce::roundToInt(filter->mode->getCurrentValue()), 2, "band pass");
            expect(!find(GateEffect::xmlTypeName)->isEnabled(), "the gate is still bypassed");

            auto& fresh = project.getEdit();

            beginTest("Noise Gate: closes on quiet noise, opens for a note, and closes after it");
            {
                Standalone<GateEffect> gate(fresh);
                juce::AudioBuffer<float> b(2, (int) kRate);
                juce::Random r(1);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const bool note = i >= kRate * 0.25 && i < kRate * 0.5;
                    const float v = note ? 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 440.0 * i / kRate)
                                         : 0.001f * (r.nextFloat() * 2.0f - 1.0f);
                    b.setSample(0, i, v);
                    b.setSample(1, i, v);
                }
                render(*gate.plugin, b);
                expect(rms(b, 0, 0.0, 0.25) < 1.0e-6f, "quiet noise is gated");
                expectWithinAbsoluteError(rms(b, 0, 0.27, 0.5), 0.5f / std::sqrt(2.0f), 0.01f, "the note passes untouched");
                expect(rms(b, 1, 0.9, 1.0) < 1.0e-5f, "silent again after hold and release");

                set(gate->range, -20.0f);
                juce::AudioBuffer<float> quiet(2, (int) kRate / 2);
                for (int i = 0; i < quiet.getNumSamples(); ++i)
                    quiet.setSample(0, i, 0.001f), quiet.setSample(1, i, 0.001f);
                render(*gate.plugin, quiet);
                expectWithinAbsoluteError(quiet.getSample(0, quiet.getNumSamples() - 1), 0.0001f, 1.0e-5f, "Floor turns it down by 20 dB");
            }

            beginTest("Delay: echoes on the beat, repeats with feedback, and ping-pongs");
            {
                Standalone<DelayEffect> d(fresh);
                d->setTempo(120.0);
                set(d->mix, 1.0f);   // echoes only
                set(d->feedback, 0.5f);
                set(d->lowCut, 20.0f);
                set(d->highCut, 20000.0f);
                juce::AudioBuffer<float> b(2, (int) (kRate * 1.1));
                b.clear();
                b.setSample(0, 0, 1.0f);
                b.setSample(1, 0, 1.0f);
                render(*d.plugin, b);
                expectEquals(b.findMinMax(0, 0, 23000).getEnd(), 0.0f, "nothing before the first echo");
                expectWithinAbsoluteError(b.getSample(0, 24000), 1.0f, 1.0e-3f, "a quarter note at 120 bpm is 24000 samples");
                expectWithinAbsoluteError(b.getSample(0, 48000), 0.5f, 1.0e-3f, "the second echo at half level");
                expect(d->getTailLength() > 2.0, "renders keep the echoes");

                d.plugin->reset();
                set(d->sync, 0.0f);
                set(d->time, 100.0f);
                set(d->pingPong, 1.0f);
                b.clear();
                b.setSample(0, 0, 1.0f);
                b.setSample(1, 0, 1.0f);
                render(*d.plugin, b);
                expectWithinAbsoluteError(b.getSample(0, 4800), 1.0f, 1.0e-3f, "100 ms, first echo on the left");
                expectWithinAbsoluteError(b.getSample(1, 4800), 0.0f, 1.0e-3f);
                expectWithinAbsoluteError(b.getSample(1, 9600), 0.5f, 1.0e-3f, "then the right");
                expectWithinAbsoluteError(b.getSample(0, 9600), 0.0f, 1.0e-3f);
            }

            beginTest("Auto Pan: sweeps left and right, and locks to the song position");
            {
                Standalone<AutoPanEffect> pan(fresh);
                set(pan->sync, 0.0f);
                set(pan->rate, 1.0f);
                juce::AudioBuffer<float> b(2, (int) kRate);
                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill(b.getWritePointer(ch), 1.0f, b.getNumSamples());
                render(*pan.plugin, b);
                expectWithinAbsoluteError(b.getSample(0, 0), 1.0f, 0.01f, "starts centred");
                expect(b.getSample(0, 12000) < 0.05f && b.getSample(1, 12000) > 1.35f, "a quarter cycle in: right");
                expect(b.getSample(1, 36000) < 0.05f && b.getSample(0, 36000) > 1.35f, "three quarters: left");

                pan.plugin->reset();
                set(pan->sync, 1.0f);
                pan->division->setParameter((float) EffectPlugin::divisionNames().indexOf("1/1"), juce::sendNotification);
                pan->setTempo(120.0);
                juce::AudioBuffer<float> c(2, 4096);
                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill(c.getWritePointer(ch), 1.0f, c.getNumSamples());
                render(*pan.plugin, c, true, 0.5);   // one beat into a four-beat cycle
                expect(c.getSample(0, 2000) < 0.05f && c.getSample(1, 2000) > 1.35f, "synced: right on beat 2");
            }

            beginTest("Filter: low, high pass and notch, with a tempo-synced LFO on the cutoff");
            {
                Standalone<FilterEffect> f(fresh);
                set(f->cutoff, 500.0f);
                set(f->slope, 1.0f);
                auto high = sine(5000.0, 0.5), low = sine(100.0, 0.5);
                render(*f.plugin, high);
                f.plugin->reset();
                render(*f.plugin, low);
                expect(juce::Decibels::gainToDecibels(rms(high, 0, 0.1, 0.5) / 0.3536f) < -60.0f, "24 dB low pass removes 5 kHz");
                expectWithinAbsoluteError(rms(low, 0, 0.1, 0.5), 0.3536f, 0.02f, "and keeps 100 Hz");

                set(f->mode, (float) EffectSvf::HighPass);
                f.plugin->reset();
                low = sine(100.0, 0.5);
                render(*f.plugin, low);
                expect(juce::Decibels::gainToDecibels(rms(low, 0, 0.1, 0.5) / 0.3536f) < -30.0f, "high pass removes 100 Hz");

                set(f->mode, (float) EffectSvf::Notch);
                set(f->cutoff, 1000.0f);
                set(f->slope, 0.0f);
                f.plugin->reset();
                auto mid = sine(1000.0, 0.5);
                render(*f.plugin, mid);
                expect(juce::Decibels::gainToDecibels(rms(mid, 0, 0.2, 0.5) / 0.3536f) < -30.0f, "notch removes its frequency");

                set(f->mode, (float) EffectSvf::LowPass);
                set(f->lfoDepth, 2.0f);
                set(f->lfoSync, 1.0f);
                f->lfoDivision->setParameter((float) EffectPlugin::divisionNames().indexOf("1/1"), juce::sendNotification);
                f->setTempo(120.0);
                f.plugin->reset();
                juce::AudioBuffer<float> b(2, 512);
                b.clear();
                render(*f.plugin, b, true, 0.5);   // the sine LFO's peak
                const float hz = FilterEffect::kMinHz * std::pow(FilterEffect::kMaxHz / FilterEffect::kMinHz, f->getDisplay().value);
                expectWithinAbsoluteError(hz, 4000.0f, 100.0f, "+2 octaves at the LFO's peak");
            }

            beginTest("Pitch Shifter: shifts by semitones, keeps pitch when shifting formants, and lines dry up with wet");
            {
                Standalone<PitchEffect> p(fresh);
                const double latency = p.plugin->getLatencySeconds();
                expect(latency > 0.01 && latency < 0.15, "latency " + juce::String(latency));

                // Signalsmith Stretch tracks peaks between FFT bins, so a pure tone can land up to
                // about 20 cents off: allow 1.5%.
                set(p->pitch, 12.0f);
                auto b = sine(220.0, 2.0);
                render(*p.plugin, b);
                expectNear(frequency(b, 0.5, 1.9, 440.0), 440.0, 0.015, "an octave up");

                set(p->pitch, -7.0f);
                p.plugin->reset();
                b = sine(220.0, 2.0);
                render(*p.plugin, b);
                expectNear(frequency(b, 0.5, 1.9, 146.8), 220.0 * std::pow(2.0, -7.0 / 12.0), 0.015, "a fifth down");

                set(p->pitch, 0.0f);
                set(p->formant, 5.0f);
                p.plugin->reset();
                b = sine(220.0, 2.0);
                render(*p.plugin, b);
                expectNear(frequency(b, 0.5, 1.9, 220.0), 220.0, 0.002, "formants move, pitch doesn't");

                set(p->mix, 0.0f);
                p.plugin->reset();
                const auto in = sine(220.0, 1.0);
                b = in;
                render(*p.plugin, b);
                const int offset = juce::roundToInt(latency * kRate);
                float worst = 0.0f;
                for (int i = offset; i < b.getNumSamples(); ++i)
                    worst = juce::jmax(worst, std::abs(b.getSample(0, i) - in.getSample(0, i - offset)));
                expect(worst < 1.0e-6f, "dry is the input, delayed by the reported latency");
            }

            beginTest("Compressor: turns down what goes over the threshold by the ratio");
            {
                Standalone<CompressorEffect> c(fresh);
                set(c->threshold, -18.0f);
                set(c->ratio, 4.0f);
                set(c->knee, 0.0f);
                set(c->attack, 1.0f);
                set(c->detect, 1.0f);   // RMS, so a sine reads steadily
                auto b = sine(200.0, 1.0);   // peaks at -6 dB: 12 dB over
                render(*c.plugin, b);
                const float outDb = juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) * std::sqrt(2.0f));
                expectWithinAbsoluteError(outDb, -15.0f, 0.75f, "12 dB over at 4:1 comes out 3 dB over");
                expectWithinAbsoluteError(c->getDisplay().value * 24.0f, 9.0f, 1.0f, "9 dB of reduction shown");

                set(c->ratio, 20.0f);
                c.plugin->reset();
                b = sine(200.0, 1.0);
                render(*c.plugin, b);
                expectWithinAbsoluteError(juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) * std::sqrt(2.0f)), -18.0f, 0.75f,
                                          "infinite ratio holds it at the threshold");

                set(c->ratio, 4.0f);
                c.plugin->reset();
                b = sine(200.0, 1.0, 0.05f);   // -26 dB: under
                render(*c.plugin, b);
                expectWithinAbsoluteError(rms(b, 0, 0.5, 1.0), 0.05f / std::sqrt(2.0f), 0.001f, "under the threshold: untouched");
            }

            beginTest("Compressor key sources: other tracks and buses, never a loop");
            {
                juce::String err;
                auto* comp = dynamic_cast<CompressorEffect*>(engine.addEffect(5, CompressorEffect::xmlTypeName, err));
                expect(comp != nullptr, err);
                if (comp != nullptr)
                {
                    engine.setTrackRoute(5, AudioEngine::Route::Bus1, true);
                    engine.syncNow();
                    auto* own = engine.getAudioTrack(5);
                    auto* bus1 = engine.getAudioTrack(AudioEngine::kFirstStereoBus);
                    auto* bus2 = engine.getAudioTrack(AudioEngine::kFirstStereoBus + 1);
                    const auto sources = comp->getKeySources();
                    auto has = [&](te::AudioTrack* t) { return std::find(sources.begin(), sources.end(), t) != sources.end(); };
                    expect(!has(own), "not its own track");
                    expect(!has(bus1), "not a bus the track feeds");
                    expect(has(engine.getAudioTrack(0)) && has(engine.getAudioTrack(7)), "the other tracks");

                    expect(has(bus2), "Bus 2: the track's send to it is off");
                    comp->setKeySource(bus2);
                    engine.setTrackRoute(5, AudioEngine::Route::Bus2, true);
                    expect(comp->getKeySource() == nullptr, "sending to the key bus disconnects the key: no loop");
                    fresh.getUndoManager().undo();
                    expect(comp->getKeySource() == bus2 && !engine.hasTrackRoute(5, AudioEngine::Route::Bus2),
                           "one undo puts back both");

                    comp->setKeySource(engine.getAudioTrack(0));
                    expect(comp->getKeySource() == engine.getAudioTrack(0));
                    expectEquals(comp->getNumWires(), 4, "main and key wired");
                    expectEquals(comp->getSelectors().front().get(), 1, "the menu shows the first track");
                    fresh.getUndoManager().undo();
                    expect(comp->getKeySource() == bus2, "undo goes back to the previous key");
                    comp->setKeySource(nullptr);
                    expect(comp->getKeySource() == nullptr && comp->getSelectors().front().get() == 0, "back to this track");
                    auto* gate = dynamic_cast<GateEffect*>(engine.addEffect(0, GateEffect::xmlTypeName, err));
                    expect(gate != nullptr, err);
                    if (gate != nullptr)
                    {
                        gate->setKeySource(own);
                        expect(gate->getKeySource() == own);
                        comp->setKeySource(engine.getAudioTrack(0));
                        expect(comp->getKeySource() == nullptr, "a gate and compressor cannot key each other");

                        gate->setKeySource(bus2);
                        expect(gate->getKeySource() == bus2);
                        engine.setTrackRoute(0, AudioEngine::Route::Bus2, true);
                        expect(gate->getKeySource() == nullptr, "a new send disconnects a looping gate key");
                        expect(project.undo());
                        expect(gate->getKeySource() == bus2 && !engine.hasTrackRoute(0, AudioEngine::Route::Bus2));
                        engine.removeInsert(*gate);
                    }
                    engine.removeInsert(*comp);
                    engine.setTrackRoute(5, AudioEngine::Route::Bus1, false);
                }
            }

            beginTest("Limiter: nothing over the ceiling, with the latency it reports");
            {
                Standalone<LimiterEffect> l(fresh);
                set(l->gain, 12.0f);
                set(l->ceiling, -1.0f);
                set(l->release, 5.0f);
                juce::AudioBuffer<float> b(2, (int) kRate);
                juce::Random r(3);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float burst = (i / 4800) % 2 == 0 ? 0.9f : 0.1f;   // loud and quiet every 100 ms
                    const float v = burst * (float) std::sin(i * 0.3) + 0.05f * (r.nextFloat() - 0.5f);
                    b.setSample(0, i, v);
                    b.setSample(1, i, -v);
                }
                const auto input = b;
                render(*l.plugin, b);
                const float limit = juce::Decibels::decibelsToGain(-1.0f);
                float peak = 0.0f, truePeak = 0.0f;
                for (int i = 1; i + 2 < b.getNumSamples(); ++i)
                {
                    peak = juce::jmax(peak, std::abs(b.getSample(0, i)));
                    // A 4x Hermite reconstruction between samples
                    const float y0 = b.getSample(0, i - 1), y1 = b.getSample(0, i), y2 = b.getSample(0, i + 1), y3 = b.getSample(0, i + 2);
                    for (float t : { 0.25f, 0.5f, 0.75f })
                    {
                        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                        truePeak = juce::jmax(truePeak, std::abs(((c3 * t + c2) * t + c1) * t + y1));
                    }
                }
                expect(peak <= limit + 1.0e-6f, "sample peak " + juce::String(peak));
                expect(truePeak <= limit * 1.01f, "inter-sample peak " + juce::String(truePeak));
                const int latency = juce::roundToInt(l.plugin->getLatencySeconds() * kRate);
                expectEquals(latency, 144, "3 ms at 48 kHz");
                // Quiet parts come through at the gain, delayed by the latency.
                const int quiet = 4800 + 2400;   // the middle of the first quiet burst
                expectWithinAbsoluteError(b.getSample(0, quiet + latency), input.getSample(0, quiet) * 3.98f, 0.02f,
                                          "below the ceiling: just the gain (after the release)");
            }

            beginTest("Drive: every model adds harmonics; the lopsided ones add even harmonics");
            {
                Standalone<DriveEffect> d(fresh);
                set(d->cab, (float) DriveEffect::NoCab);
                set(d->tone, 1.0f);
                set(d->drive, 24.0f);
                set(d->bias, 0.5f);
                for (int m = 0; m < 4; ++m)
                {
                    set(d->model, (float) m);
                    d.plugin->reset();
                    auto b = sine(250.0, 0.5, 0.3f);
                    render(*d.plugin, b);
                    const float f1 = levelAt(b, 250.0, 0.1, 0.5), h2 = levelAt(b, 500.0, 0.1, 0.5), h3 = levelAt(b, 750.0, 0.1, 0.5);
                    const auto name = DriveEffect::modelNames()[m];
                    // Overdrive clips the mids (above ~700 Hz) and keeps the lows clean, so 250 Hz drives it less.
                    expect(h3 - f1 > (m == DriveEffect::Overdrive ? -36.0f : -30.0f), name + ": 3rd harmonic " + juce::String(h3 - f1, 1) + " dB");
                    if (m == DriveEffect::Fuzz || m == DriveEffect::Tube)
                        expect(h2 - f1 > -30.0f, name + ": 2nd harmonic " + juce::String(h2 - f1, 1) + " dB");
                    else
                        expect(h2 - f1 < -50.0f, name + ": symmetric, no 2nd harmonic: " + juce::String(h2 - f1, 1) + " dB");
                }

                beginTest("Drive: oversampled, so a loud high note doesn't alias");
                set(d->model, (float) DriveEffect::Distortion);
                set(d->drive, 36.0f);
                d.plugin->reset();
                auto high = sine(7000.0, 0.5, 0.5f);
                render(*d.plugin, high);
                // The 5th harmonic (35 kHz) would fold back to 13 kHz without oversampling.
                const float alias = levelAt(high, 13000.0, 0.1, 0.5) - levelAt(high, 7000.0, 0.1, 0.5);
                expect(alias < -45.0f, "alias at 13 kHz: " + juce::String(alias, 1) + " dB");

                beginTest("Drive: the 4x12 cabinet rolls off the top");
                set(d->drive, 0.0f);
                set(d->model, (float) DriveEffect::Tube);
                set(d->bias, 0.0f);
                set(d->cab, (float) DriveEffect::ClosedStack);
                d.plugin->reset();
                auto mid = sine(1000.0, 0.5, 0.01f), top = sine(9000.0, 0.5, 0.01f);
                render(*d.plugin, mid);
                d.plugin->reset();
                render(*d.plugin, top);
                const float rolloff = levelAt(top, 9000.0, 0.1, 0.5) - levelAt(mid, 1000.0, 0.1, 0.5);
                expect(rolloff < -20.0f, "9 kHz vs 1 kHz: " + juce::String(rolloff, 1) + " dB");
            }

            beginTest("Drive: bundled Nave cabinets render immediately and save by choice");
            {
                Standalone<DriveEffect> d(fresh);
                set(d->drive, 0.0f);
                set(d->model, (float) DriveEffect::Tube);
                set(d->tone, 1.0f);

                auto renderCab = [&](int cabinet)
                {
                    set(d->cab, (float) cabinet);
                    d.plugin->reset();
                    juce::AudioBuffer<float> click(2, 16384);
                    click.clear();
                    click.setSample(0, 128, 0.1f);
                    click.setSample(1, 128, 0.1f);
                    render(*d.plugin, click);
                    return click;
                };

                const auto names = DriveEffect::cabNames();
                expectEquals(names.size() - (int) DriveEffect::FirstFactoryCab, 9);
                const auto bypass = renderCab(DriveEffect::NoCab);
                auto previous = bypass;
                for (int i = DriveEffect::FirstFactoryCab; i < names.size(); ++i)
                {
                    auto response = renderCab(i);
                    const int count = response.getNumSamples();
                    expect(response.getMagnitude(0, 0, count) > 1.0e-5f, names[i] + " produces audio");
                    response.addFrom(1, 0, bypass, 1, 0, count, -1.0f);
                    expect(response.getMagnitude(1, 0, count) > 1.0e-5f, names[i] + " filters the right channel");
                    const auto original = response;
                    response.addFrom(0, 0, previous, 0, 0, count, -1.0f);
                    expect(response.getMagnitude(0, 0, count) > 1.0e-5f, names[i] + " changes the response");
                    previous = original;
                }

                Standalone<DriveEffect> restored(fresh);
                restored.plugin->restorePluginStateFromValueTree(d.plugin->state.createCopy());
                expectEquals(juce::roundToInt(restored->cab->getCurrentValue()), names.size() - 1);
                expect(restored->getImpulseResponse() == juce::File(), "a factory cabinet needs no project audio file");
            }

            beginTest("Drive: an impulse response loads into the project and is the cabinet");
            {
                Standalone<DriveEffect> d(fresh);
                auto ir = dir.getChildFile("cab-ir.wav");
                {
                    juce::AudioBuffer<float> impulse(1, 1000);
                    impulse.clear();
                    impulse.setSample(0, 0, 1.0f);
                    impulse.setSample(0, 480, 1.0f);   // and an echo 10 ms later
                    ir.deleteFile();
                    std::unique_ptr<juce::OutputStream> stream = ir.createOutputStream();
                    juce::WavAudioFormat wav;
                    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
                    writer->writeFromAudioSampleBuffer(impulse, 0, impulse.getNumSamples());
                }
                juce::String err;
                expect(d->loadImpulseResponse(ir, err), err);
                expectEquals(juce::roundToInt(d->cab->getCurrentValue()), (int) DriveEffect::ImpulseResponse, "the cabinet switches to it");
                const auto loaded = d->getImpulseResponse();
                expect(loaded.existsAsFile() && loaded != ir, "copied into the project: " + loaded.getFullPathName());
                std::vector<EffectPlugin::FileReference> refs;
                d->getFileReferences(refs);
                expect(refs.size() == 1 && refs.front().state[refs.front().property].toString().isNotEmpty(), "Save As will copy it");

                set(d->drive, 0.0f);
                set(d->model, (float) DriveEffect::Tube);
                set(d->bias, 0.0f);
                set(d->tone, 1.0f);
                juce::AudioBuffer<float> silence(2, 4800);
                for (int i = 0; i < 20; ++i)   // the convolution loads its IR in the background
                {
                    silence.clear();
                    render(*d.plugin, silence);
                    juce::Thread::sleep(20);
                }
                juce::AudioBuffer<float> click(2, 4800);
                click.clear();
                for (int i = 1000; i < 1010; ++i)
                    click.setSample(0, i, 0.01f), click.setSample(1, i, 0.01f);
                render(*d.plugin, click);
                const int latency = juce::roundToInt(d.plugin->getLatencySeconds() * kRate);
                auto energy = [&](int from, int to)
                {
                    float e = 0.0f;
                    for (int i = from; i < to; ++i)
                        e += click.getSample(0, i) * click.getSample(0, i);
                    return e;
                };
                const float direct = energy(1000 + latency - 5, 1000 + latency + 30);
                const float echo = energy(1480 + latency - 5, 1480 + latency + 30);
                const float between = energy(1000 + latency + 100, 1480 + latency - 50);
                expect(direct > 1.0e-8f && echo > direct * 0.5f, "the click and its echo 10 ms later");
                expect(between < echo * 0.05f, "nothing much in between (just the filters' tail)");

                expect(d->loadImpulseResponse({}, err));
                expect(d->getImpulseResponse() == juce::File(), "cleared");
            }

            beginTest("Chorus: Juno is wide stereo at the input's level; CE-2 is mono and moving; Dimension is wide; Mix 0 is dry");
            {
                Standalone<ChorusEffect> c(fresh);
                auto correlation = [](const juce::AudioBuffer<float>& b, int from)
                {
                    double lr = 0, ll = 0, rr = 0;
                    for (int i = from; i < b.getNumSamples(); ++i)
                    {
                        const double l = b.getSample(0, i), r = b.getSample(1, i);
                        lr += l * r; ll += l * l; rr += r * r;
                    }
                    return lr / std::sqrt(ll * rr + 1.0e-20);
                };
                auto wobble = [](const juce::AudioBuffer<float>& b, int ch)   // how much the level moves, 20 ms windows
                {
                    float lo = 1e9f, hi = 0.0f;
                    for (int start = (int) kRate / 4; start + 960 < b.getNumSamples(); start += 960)
                    {
                        const float level = b.getRMSLevel(ch, start, 960);
                        lo = juce::jmin(lo, level); hi = juce::jmax(hi, level);
                    }
                    return juce::Decibels::gainToDecibels(hi / juce::jmax(lo, 1.0e-9f));
                };

                set(c->mode, (float) ChorusEffect::JunoI);
                set(c->hiss, 0.0f);
                auto b = sine(440.0, 2.0, 0.3f);
                render(*c.plugin, b);
                expect(correlation(b, (int) kRate / 4) < 0.9, "Juno I: left and right differ");
                expectWithinAbsoluteError(juce::Decibels::gainToDecibels(rms(b, 0, 0.25, 2.0) / (0.3f / std::sqrt(2.0f))), 0.0f, 3.0f,
                                          "about the input's level");

                set(c->mode, (float) ChorusEffect::CE2);
                set(c->depth, 1.0f);
                set(c->rate, 1.0f);
                c.plugin->reset();
                b = sine(1000.0, 2.0, 0.3f);
                render(*c.plugin, b);
                expectWithinAbsoluteError((float) correlation(b, (int) kRate / 4), 1.0f, 1.0e-4f, "CE-2 is mono");
                expect(wobble(b, 0) > 3.0f, "CE-2 beats as the delay moves: " + juce::String(wobble(b, 0), 1) + " dB");

                set(c->mode, (float) ChorusEffect::Dimension3);
                c.plugin->reset();
                b = sine(1000.0, 2.0, 0.3f);
                render(*c.plugin, b);
                expect(correlation(b, (int) kRate / 4) < 0.95, "Dimension is wide");

                set(c->mix, 0.0f);
                c.plugin->reset();
                const auto in = sine(330.0, 0.5, 0.3f);
                b = in;
                render(*c.plugin, b);
                float diff = 0.0f;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    diff = juce::jmax(diff, std::abs(b.getSample(0, i) - in.getSample(0, i)));
                expect(diff < 1.0e-6f, "Mix 0: dry");
            }

            beginTest("Doubler: the double comes in late and detuned, wide in stereo; Mix 0 is dry; 1 voice is mono");
            {
                Standalone<DoublerEffect> d(fresh);
                set(d->variation, 0.0f);
                set(d->lowCut, 20.0f);
                set(d->highCut, 20000.0f);

                // A click: with Mix all the way up, only the doubles, about Delay later.
                set(d->mix, 1.0f);
                set(d->voices, (float) DoublerEffect::One);
                set(d->detune, 0.0f);
                set(d->delay, 25.0f);
                juce::AudioBuffer<float> click(2, 9600);
                click.clear();
                click.setSample(0, 1000, 1.0f), click.setSample(1, 1000, 1.0f);
                render(*d.plugin, click);
                int peakAt = 0;
                for (int i = 0; i < click.getNumSamples(); ++i)
                    if (std::abs(click.getSample(0, i)) > std::abs(click.getSample(0, peakAt)))
                        peakAt = i;
                expectWithinAbsoluteError(peakAt - 1000, juce::roundToInt(0.025 * kRate), 4, "the double is 25 ms late");
                expect(std::abs(click.getSample(0, 1000)) < 1.0e-6f, "no lead at Mix 100%");

                // Detuned: a sine through the double comes out a few cents off.
                set(d->detune, 20.0f);
                d.plugin->reset();
                auto b = sine(440.0, 2.0, 0.3f);
                render(*d.plugin, b);
                int crossings = 0;
                const int from = (int) kRate / 2, to = (int) (kRate * 1.9);
                for (int i = from + 1; i < to; ++i)
                    if (b.getSample(0, i - 1) < 0.0f && b.getSample(0, i) >= 0.0f)
                        ++crossings;
                const double hz = crossings / ((to - from) / kRate);
                const double cents = 1200.0 * std::log2(hz / 440.0);
                expectWithinAbsoluteError((float) cents, 20.0f, 4.0f, "+20 cents: " + juce::String(cents, 1));
                float diff = 0.0f;
                for (int i = from; i < to; ++i)
                    diff = juce::jmax(diff, std::abs(b.getSample(0, i) - b.getSample(1, i)));
                expect(diff < 1.0e-6f, "1 voice is in the centre");

                // Two voices, spread: left and right differ.
                set(d->voices, (float) DoublerEffect::Two);
                set(d->variation, 0.5f);
                set(d->width, 1.0f);
                set(d->mix, 0.5f);
                d.plugin->reset();
                b = sine(300.0, 2.0, 0.3f);
                render(*d.plugin, b);
                double lr = 0, ll = 0, rr = 0;
                for (int i = from; i < to; ++i)
                {
                    const double l = b.getSample(0, i), r = b.getSample(1, i);
                    lr += l * r; ll += l * l; rr += r * r;
                }
                expect(lr / std::sqrt(ll * rr) < 0.95, "2 voices are wide");
                expect(b.getMagnitude(0, b.getNumSamples()) < 1.0f, "and don't blow up");

                set(d->mix, 0.0f);
                d.plugin->reset();
                const auto in = sine(330.0, 0.5, 0.3f);
                b = in;
                render(*d.plugin, b);
                diff = 0.0f;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    diff = juce::jmax(diff, std::abs(b.getSample(0, i) - in.getSample(0, i)));
                expect(diff < 1.0e-6f, "Mix 0: dry");
            }

            beginTest("EQ: flat by default; a bell, a cut and Output do what the curve says; a band that's off does nothing");
            {
                Standalone<EqEffect> q(fresh);
                auto levelDb = [&](double hz)
                {
                    q.plugin->reset();
                    auto b = sine(hz, 1.0, 0.25f);
                    render(*q.plugin, b);
                    return juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) / (0.25f / std::sqrt(2.0f)));
                };
                for (double hz : { 50.0, 1000.0, 12000.0 })
                    expectWithinAbsoluteError(levelDb(hz), 0.0f, 0.01f, "flat at " + juce::String(hz) + " Hz");

                auto& bell = q->bands[2];
                set(bell.type, (float) EqEffect::Bell);
                set(bell.freq, 1000.0f);
                set(bell.gain, 6.0f);
                set(bell.q, 1.0f);
                expectWithinAbsoluteError(levelDb(1000.0), 6.0f, 0.1f, "+6 dB at the bell's frequency");
                expectWithinAbsoluteError(levelDb(60.0), 0.0f, 0.2f, "nothing far below it");
                expectWithinAbsoluteError((float) q->responseDb(1000.0), 6.0f, 0.01f, "the curve agrees");
                expectWithinAbsoluteError(levelDb(2000.0), (float) q->responseDb(2000.0), 0.1f, "the curve agrees off-centre");

                auto& cut = q->bands[0];
                set(cut.on, 1.0f);
                set(cut.type, (float) EqEffect::LowCut);
                set(cut.freq, 400.0f);
                expect(levelDb(50.0) < -30.0f, "the low cut removes 50 Hz");
                set(cut.on, 0.0f);
                expectWithinAbsoluteError(levelDb(50.0), 0.0f, 0.2f, "switched off, it doesn't");
                expect(!q->isControlActive(*cut.freq) && q->isControlActive(*cut.on), "an off band's controls are dimmed, not its switch");
                set(cut.on, 1.0f);
                expect(!q->isControlActive(*cut.gain) && q->isControlActive(*bell.gain), "a cut has no gain");

                set(cut.on, 0.0f);
                set(bell.gain, 0.0f);
                set(q->output, -6.0f);
                q.plugin->reset();
                auto b = sine(1000.0, 1.0, 0.25f);
                render(*q.plugin, b);
                render(*q.plugin, b);   // the ramp has settled by the second pass
                expectWithinAbsoluteError(juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) / (0.25f / std::sqrt(2.0f))), -12.0f, 0.05f,
                                          "Output -6 dB, twice");

                q->after.active = true;
                q.plugin->reset();
                b = sine(1000.0, 0.2, 0.25f);
                render(*q.plugin, b);
                std::vector<float> seen(1024);
                q->after.read(seen.data(), (int) seen.size());
                expectWithinAbsoluteError(seen.back(), b.getSample(0, b.getNumSamples() - 1), 1.0e-6f, "the analyser sees the output");
            }

            beginTest("De-Esser: turns down loud highs by up to Range, leaves the lows; Split keeps the lows under an S; Listen plays the highs");
            {
                Standalone<DeEsserEffect> d(fresh);
                auto levelDb = [&](std::function<float(int)> signal)
                {
                    d.plugin->reset();
                    juce::AudioBuffer<float> b(2, (int) kRate);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample(0, i, signal(i)), b.setSample(1, i, signal(i));
                    const auto in = b;
                    render(*d.plugin, b);
                    return juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) / rms(in, 0, 0.5, 1.0));
                };
                auto tone = [](double hz, float level) { return [=](int i) { return level * (float) std::sin(juce::MathConstants<double>::twoPi * hz * i / kRate); }; };

                set(d->threshold, -30.0f);
                set(d->range, 10.0f);
                set(d->mode, (float) DeEsserEffect::Wide);
                expectWithinAbsoluteError(levelDb(tone(9000.0, 0.5f)), -10.0f, 0.5f, "Wide: a loud 9 kHz tone is down by Range");
                set(d->mode, (float) DeEsserEffect::Split);
                const float split = levelDb(tone(14000.0, 0.5f));
                expect(split < -8.5f && split > -10.5f, "Split: a loud 14 kHz tone is down by about Range: " + juce::String(split, 1) + " dB");
                expectWithinAbsoluteError(levelDb(tone(200.0, 0.5f)), 0.0f, 0.1f, "200 Hz: untouched");
                expectWithinAbsoluteError(levelDb(tone(9000.0, 0.01f)), 0.0f, 0.2f, "a quiet 9 kHz tone: under the threshold");

                // A loud S over a low note: Split turns down only the S; Wide turns down both
                auto mixture = [](int i)
                {
                    const double t = i / kRate;
                    return 0.4f * (float) std::sin(juce::MathConstants<double>::twoPi * 150.0 * t)
                         + 0.4f * (float) std::sin(juce::MathConstants<double>::twoPi * 9000.0 * t);
                };
                auto lowsAfter = [&](int modeValue)
                {
                    set(d->mode, (float) modeValue);
                    d.plugin->reset();
                    juce::AudioBuffer<float> b(2, (int) kRate);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample(0, i, mixture(i)), b.setSample(1, i, mixture(i));
                    render(*d.plugin, b);
                    juce::IIRFilter lp;
                    lp.setCoefficients(juce::IIRCoefficients::makeLowPass(kRate, 1000.0));
                    lp.processSamples(b.getWritePointer(0), b.getNumSamples());
                    juce::IIRFilter lp2;
                    lp2.setCoefficients(juce::IIRCoefficients::makeLowPass(kRate, 1000.0));
                    lp2.processSamples(b.getWritePointer(0), b.getNumSamples());
                    return juce::Decibels::gainToDecibels(rms(b, 0, 0.5, 1.0) / (0.4f / std::sqrt(2.0f)));
                };
                expectWithinAbsoluteError(lowsAfter(DeEsserEffect::Split), 0.0f, 0.5f, "Split: the low note stays");
                expect(lowsAfter(DeEsserEffect::Wide) < -6.0f, "Wide: the low note dips with the S");

                set(d->mode, (float) DeEsserEffect::Split);
                set(d->listen, 1.0f);
                expect(levelDb(tone(150.0, 0.5f)) < -20.0f, "Listen: the lows are gone");
                expect(levelDb(tone(12000.0, 0.5f)) > -1.0f, "Listen: the highs are there");
            }

            beginTest("Reverb: the tail follows Decay, waits for Pre-delay, and Mix 0 is dry");
            {
                Standalone<ReverbEffect> rv(fresh);
                auto db = [](float v) { return juce::Decibels::gainToDecibels(v, -200.0f); };
                auto impulse = [](double seconds)
                {
                    juce::AudioBuffer<float> b(2, (int) (seconds * kRate));
                    b.clear();
                    b.setSample(0, 0, 1.0f);
                    b.setSample(1, 0, 1.0f);
                    return b;
                };
                set(rv->mix, 1.0f);
                set(rv->preDelay, 0.0f);
                auto drop = [&](float seconds)   // how far the tail falls from 0.3 s to 1.3 s
                {
                    set(rv->decay, seconds);
                    rv.plugin->reset();
                    auto b = impulse(1.5);
                    render(*rv.plugin, b);
                    return db(rms(b, 0, 0.25, 0.35)) - db(rms(b, 0, 1.25, 1.35));
                };
                const float shortDrop = drop(0.8f), longDrop = drop(5.0f);
                logMessage("REVERB short " + juce::String(shortDrop) + " long " + juce::String(longDrop));
                expect(shortDrop > 25.0f, "a 0.8 s decay falls " + juce::String(shortDrop, 1) + " dB in a second");
                expect(longDrop < 20.0f && longDrop > 3.0f, "a 5 s decay falls " + juce::String(longDrop, 1) + " dB");

                for (int sp = 0; sp < 6; ++sp)
                {
                    set(rv->space, (float) sp);
                    set(rv->decay, 2.0f);
                    rv.plugin->reset();
                    auto b = impulse(1.0);
                    render(*rv.plugin, b);
                    const float level = rms(b, 0, 0.0, 1.0);
                    expect(std::isfinite(level) && level > 1.0e-4f && level < 0.5f,
                           ReverbEffect::spaceNames()[sp] + " rings: " + juce::String(db(level), 1) + " dB");
                }

                set(rv->space, (float) ReverbEffect::Plate);
                set(rv->preDelay, 200.0f);
                rv.plugin->reset();
                auto b = impulse(0.5);
                render(*rv.plugin, b);
                expect(b.getMagnitude(0, 0, (int) (0.19 * kRate)) < 1.0e-4f, "nothing before the pre-delay");
                expect(b.getMagnitude(0, (int) (0.2 * kRate), (int) (0.3 * kRate)) > 1.0e-3f, "then the reverb");

                set(rv->mix, 0.0f);
                rv.plugin->reset();
                const auto in = sine(330.0, 0.5, 0.3f);
                b = in;
                render(*rv.plugin, b);
                float diff = 0.0f;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    diff = juce::jmax(diff, std::abs(b.getSample(0, i) - in.getSample(0, i)));
                expect(diff < 1.0e-6f, "Mix 0: dry");
            }

            beginTest("Reverb: shimmer climbs an octave, and stays bounded at full shimmer and decay");
            {
                Standalone<ReverbEffect> rv(fresh);
                set(rv->mix, 1.0f);
                set(rv->decay, 4.0f);
                set(rv->preDelay, 0.0f);
                set(rv->lowCut, 20.0f);
                auto burstThenSilence = [&]
                {
                    auto b = sine(440.0, 3.0, 0.3f);
                    for (int ch = 0; ch < 2; ++ch)
                        b.clear(ch, (int) (0.4 * kRate), b.getNumSamples() - (int) (0.4 * kRate));
                    return b;
                };
                auto octaveOverRoot = [&](float amount)
                {
                    set(rv->shimmer, amount);
                    rv.plugin->reset();
                    auto b = burstThenSilence();
                    render(*rv.plugin, b);
                    return levelAt(b, 880.0, 1.5, 2.5) - levelAt(b, 440.0, 1.5, 2.5);
                };
                const float plain = octaveOverRoot(0.0f), shimmering = octaveOverRoot(0.8f);
                logMessage("SHIMMER " + juce::String(plain) + " " + juce::String(shimmering));
                expect(shimmering - plain > 10.0f, "the octave in the tail: " + juce::String(plain, 1) + " dB without shimmer, "
                                                   + juce::String(shimmering, 1) + " dB with");

                set(rv->space, (float) ReverbEffect::Cathedral);
                set(rv->decay, 30.0f);
                set(rv->shimmer, 1.0f);
                set(rv->interval, 4.0f);   // two octaves
                rv.plugin->reset();
                juce::Random random(7);
                juce::AudioBuffer<float> noise(2, (int) (10.0 * kRate));
                for (int i = 0; i < noise.getNumSamples(); ++i)
                {
                    const float v = i < (int) (2.0 * kRate) ? random.nextFloat() * 1.6f - 0.8f : 0.0f;
                    noise.setSample(0, i, v);
                    noise.setSample(1, i, v);
                }
                render(*rv.plugin, noise);
                const float peak = noise.getMagnitude(0, noise.getNumSamples());
                logMessage("PEAK " + juce::String(peak));
                expect(std::isfinite(peak) && peak < 8.0f, "bounded: peak " + juce::String(peak, 2));
            }

            beginTest("Arpeggiator: the patterns");
            {
                using A = ArpeggiatorEffect;
                const std::vector<int> held { 64, 60, 67 };
                expect(A::pattern(A::Up, held, 1) == std::vector<int> { 60, 64, 67 });
                expect(A::pattern(A::Up, held, 2) == std::vector<int> { 60, 64, 67, 72, 76, 79 });
                expect(A::pattern(A::Down, held, 1) == std::vector<int> { 67, 64, 60 });
                expect(A::pattern(A::UpDown, held, 1) == std::vector<int> { 60, 64, 67, 64 });
                expect(A::pattern(A::DownUp, held, 1) == std::vector<int> { 67, 64, 60, 64 });
                expect(A::pattern(A::UpAndDown, held, 1) == std::vector<int> { 60, 64, 67, 67, 64, 60 });
                expect(A::pattern(A::AsPlayed, held, 1) == std::vector<int> { 64, 60, 67 });
                expect(A::pattern(A::Chord, held, 3) == held, "chords ignore octaves");
                expect(A::pattern(A::Up, { 120 }, 2) == std::vector<int> { 120 }, "nothing above 127");
            }

            beginTest("Arpeggiator: in time with the song, stopping when you let go, latching, swinging");
            {
                Standalone<ArpeggiatorEffect> arp(fresh);
                arp->setTempo(120.0);   // a 1/16 step is 0.125 s
                struct Event { double time; int note; bool on; };
                // Runs the arp for a while; `play` gives the keys to press or release in each block.
                auto run = [&](double seconds, bool playing, std::function<void(double, te::MidiMessageArray&)> play)
                {
                    std::vector<Event> out;
                    juce::AudioBuffer<float> audio(2, 512);
                    for (int s0 = 0; s0 < (int) (seconds * kRate); s0 += 512)
                    {
                        const double t = s0 / kRate;
                        te::MidiMessageArray midi;
                        if (play)
                            play(t, midi);
                        audio.clear();
                        const te::TimeRange range(te::TimePosition::fromSeconds(t), te::TimeDuration::fromSeconds(512 / kRate));
                        te::PluginRenderContext fc(&audio, 0, 512, &midi, 0.0, range, playing, false, false, false);
                        arp.plugin->applyToBuffer(fc);
                        for (auto& m : midi)
                            if (m.isNoteOnOrOff() && (t + m.getTimeStamp() < seconds - 1.0e-9 || !m.isNoteOn()))
                                out.push_back({ t + m.getTimeStamp(), m.getNoteNumber(), m.isNoteOn() });   // the last block runs over
                    }
                    return out;
                };
                auto press = [](te::MidiMessageArray& midi, std::initializer_list<int> notes)
                {
                    for (int n : notes)
                        midi.addMidiMessage(juce::MidiMessage::noteOn(1, n, (juce::uint8) 100), 0.0, {});
                };
                auto onsOf = [](const std::vector<Event>& events)
                {
                    std::vector<Event> ons;
                    for (auto& e : events)
                        if (e.on)
                            ons.push_back(e);
                    return ons;
                };

                auto events = run(1.0, true, [&](double t, auto& midi) { if (t == 0.0) press(midi, { 67, 60, 64 }); });
                auto ons = onsOf(events);
                expectEquals((int) ons.size(), 8, "eight sixteenths in a second at 120");
                const int expected[] = { 60, 64, 67, 60, 64, 67, 60, 64 };
                for (size_t i = 0; i < ons.size() && i < 8; ++i)
                {
                    expectEquals(ons[i].note, expected[i]);
                    expectWithinAbsoluteError(ons[i].time, 0.125 * (double) i, 1.0e-6, "on the grid");
                }
                int offs = 0;
                for (auto& e : events)
                    if (!e.on && std::abs(std::fmod(e.time, 0.125) - 0.0625) < 1.0e-6)
                        ++offs;
                expect(offs >= 7, "half-step gates: " + juce::String(offs));

                arp.plugin->reset();
                events = run(0.6, false, [&](double t, auto& midi)
                {
                    if (t > 0.1 && t < 0.1 + 512 / kRate)
                        press(midi, { 60 });
                    if (t > 0.4 && t < 0.4 + 512 / kRate)
                        midi.addMidiMessage(juce::MidiMessage::noteOff(1, 60), 0.0, {});
                });
                ons = onsOf(events);
                expect(!ons.empty() && ons.front().time < 0.1 + 2 * 512 / kRate, "stopped: the first note comes at once");
                expect(!ons.empty() && ons.back().time < 0.41, "and it stops when you let go");
                int sounding = 0;
                for (auto& e : events)
                    sounding += e.on ? 1 : -1;
                expectEquals(sounding, 0, "every note is let go");

                arp.plugin->reset();
                set(arp->latch, 1.0f);
                events = run(1.0, true, [&](double t, auto& midi)
                {
                    if (t == 0.0)
                        press(midi, { 60, 64 });
                    if (t > 0.2 && t < 0.2 + 512 / kRate)
                        for (int n : { 60, 64 })
                            midi.addMidiMessage(juce::MidiMessage::noteOff(1, n), 0.0, {});
                    if (t > 0.5 && t < 0.5 + 512 / kRate)
                        press(midi, { 72 });   // a new chord replaces the latched one
                });
                ons = onsOf(events);
                expectEquals((int) ons.size(), 8, "latched: it keeps going");
                expect(ons.size() == 8 && ons[7].note == 72 && ons[5].note == 72, "the new key replaces the latched chord");
                set(arp->latch, 0.0f);

                arp.plugin->reset();
                set(arp->swing, 0.5f);
                ons = onsOf(run(0.5, true, [&](double t, auto& midi) { if (t == 0.0) press(midi, { 60 }); }));
                expect(ons.size() >= 4, "swung notes");
                if (ons.size() >= 4)
                {
                    expectWithinAbsoluteError(ons[1].time, 0.125 + 0.03125, 1.0e-6, "the off-beat is late");
                    expectWithinAbsoluteError(ons[2].time, 0.25, 1.0e-6, "the beat isn't");
                }
                set(arp->swing, 0.0f);
            }

            beginTest("Arpeggiator: only on MIDI tracks, ahead of the instrument");
            {
                error.clear();
                expect(engine.addEffect(0, ArpeggiatorEffect::xmlTypeName, error) == nullptr && error.contains("MIDI"),
                       "not on an audio track");
                engine.setTrackMidi(7, true);
                auto* a = engine.addEffect(7, ArpeggiatorEffect::xmlTypeName, error);
                expect(a != nullptr, error);
                auto& list = engine.getAudioTrack(7)->pluginList;
                expect(list.indexOf(a) == 0, "first in the chain, so the instrument hears it");
                edit.getUndoManager().undo();
                expect(list.indexOf(a) < 0, "and it undoes");
                engine.setTrackMidi(7, false);
            }

            beginTest("Analog Synth: plays in tune, releases, is silent between notes, and goes mono");
            {
                Standalone<AnalogSynth> syn(fresh);
                set(syn->cutoff, 1.0f);
                set(syn->envAmount, 0.0f);
                set(syn->resonance, 0.0f);
                set(syn->sub, 0.0f);
                set(syn->chorus, 0.0f);
                set(syn->release, 0.1f);
                // Plays `notes` (on at `on`, off at `off`, seconds) into a fresh buffer.
                struct Note { int number; double on, off; };
                auto play = [&](std::vector<Note> notes, double seconds)
                {
                    juce::AudioBuffer<float> b(2, (int) (seconds * kRate));
                    b.clear();
                    for (int s0 = 0; s0 < b.getNumSamples(); s0 += 512)
                    {
                        const int n = juce::jmin(512, b.getNumSamples() - s0);
                        const double t0 = s0 / kRate, t1 = (s0 + n) / kRate;
                        te::MidiMessageArray midi;
                        for (auto& note : notes)
                        {
                            if (note.on >= t0 && note.on < t1)
                                midi.addMidiMessage(juce::MidiMessage::noteOn(1, note.number, (juce::uint8) 100), note.on - t0, {});
                            if (note.off >= t0 && note.off < t1)
                                midi.addMidiMessage(juce::MidiMessage::noteOff(1, note.number), note.off - t0, {});
                        }
                        const te::TimeRange range(te::TimePosition::fromSeconds(t0), te::TimeDuration::fromSeconds(n / kRate));
                        te::PluginRenderContext fc(&b, s0, n, &midi, 0.0, range, true, false, false, false);
                        syn.plugin->applyToBuffer(fc);
                    }
                    return b;
                };
                auto db = [](float v) { return juce::Decibels::gainToDecibels(v, -200.0f); };

                auto silent = play({}, 0.5);
                expect(db(rms(silent, 0, 0.0, 0.5)) < -90.0f, "no notes, no hiss: " + juce::String(db(rms(silent, 0, 0.0, 0.5)), 1) + " dB");

                auto a = play({ { 69, 0.1, 0.6 } }, 1.2);
                const double hz = frequency(a, 0.3, 0.55, 440.0);
                expectWithinAbsoluteError(hz, 440.0, 440.0 * 0.01, "A4 at 8'");
                expect(levelAt(a, 220.0, 0.3, 0.55) < levelAt(a, 440.0, 0.3, 0.55) - 40.0f, "not an octave down");
                expect(db(rms(a, 0, 0.3, 0.55)) > -30.0f, "and loud enough: " + juce::String(db(rms(a, 0, 0.3, 0.55)), 1) + " dB");
                expect(db(rms(a, 0, 1.0, 1.2)) < db(rms(a, 0, 0.3, 0.55)) - 50.0f, "it lets go");
                expect(rms(a, 0, 0.0, 0.095) < 1.0e-4f, "nothing before the note");
                int onset = -1;
                for (int i = 0; i < a.getNumSamples() && onset < 0; ++i)
                    if (std::abs(a.getSample(0, i)) > 1.0e-3f)
                        onset = i;
                expect(onset >= (int) (0.1 * kRate) && onset < (int) (0.1 * kRate) + 480, "the note starts on its sample: " + juce::String(onset));

                set(syn->range, 2.0f);
                auto up = play({ { 69, 0.0, 0.5 } }, 0.5);
                expectWithinAbsoluteError(frequency(up, 0.2, 0.45, 880.0), 880.0, 880.0 * 0.01, "4' is an octave up");
                expect(levelAt(up, 440.0, 0.2, 0.45) < levelAt(up, 880.0, 0.2, 0.45) - 40.0f, "and nothing at 440");
                set(syn->range, 1.0f);

                set(syn->voiceMode, (float) AnalogSynth::Mono);
                auto mono = play({ { 57, 0.0, 0.8 }, { 64, 0.3, 0.8 } }, 0.8);
                expectWithinAbsoluteError(frequency(mono, 0.5, 0.75, 329.63), 329.63, 329.63 * 0.015, "Mono: the last key wins");
                expect(levelAt(mono, 220.0, 0.5, 0.75) < levelAt(mono, 329.63, 0.5, 0.75) - 30.0f, "and the first note's gone");

                set(syn->voiceMode, (float) AnalogSynth::PolyI);
                auto chord = play({ { 57, 0.0, 0.8 }, { 64, 0.0, 0.8 } }, 0.8);
                expect(levelAt(chord, 220.0, 0.3, 0.75) > -40.0f && levelAt(chord, 329.63, 0.3, 0.75) > -40.0f, "Poly: both notes");

                set(syn->chorus, 3.0f);
                auto wide = play({ { 60, 0.0, 0.8 } }, 0.8);
                double lr = 0, ll = 0, rr = 0;
                for (int i = (int) (0.2 * kRate); i < (int) (0.7 * kRate); ++i)
                {
                    lr += wide.getSample(0, i) * wide.getSample(1, i);
                    ll += wide.getSample(0, i) * wide.getSample(0, i);
                    rr += wide.getSample(1, i) * wide.getSample(1, i);
                }
                expect(lr / std::sqrt(ll * rr) < 0.95, "the chorus spreads it");
            }

            // Plays MIDI notes into an instrument: { note, on, off } in seconds.
            struct Hit { int note; double on, off; };
            auto playInto = [&](te::Plugin& p, std::vector<Hit> notes, double seconds)
            {
                p.reset();   // nothing left sounding from the last go
                juce::AudioBuffer<float> b(2, (int) (seconds * kRate));
                b.clear();
                for (int s0 = 0; s0 < b.getNumSamples(); s0 += 512)
                {
                    const int n = juce::jmin(512, b.getNumSamples() - s0);
                    const double t0 = s0 / kRate, t1 = (s0 + n) / kRate;
                    te::MidiMessageArray midi;
                    for (auto& h : notes)
                    {
                        if (h.on >= t0 && h.on < t1)
                            midi.addMidiMessage(juce::MidiMessage::noteOn(1, h.note, (juce::uint8) 127), h.on - t0, {});
                        if (h.off >= t0 && h.off < t1)
                            midi.addMidiMessage(juce::MidiMessage::noteOff(1, h.note), h.off - t0, {});
                    }
                    const te::TimeRange range(te::TimePosition::fromSeconds(t0), te::TimeDuration::fromSeconds(n / kRate));
                    te::PluginRenderContext fc(&b, s0, n, &midi, 0.0, range, true, false, false, false);
                    p.applyToBuffer(fc);
                }
                return b;
            };
            auto writeTone = [&](const juce::String& name, double hz, double seconds)
            {
                auto f = dir.getChildFile(name);
                f.deleteFile();
                auto tone = sine(hz, seconds, 0.5f, 1);
                std::unique_ptr<juce::OutputStream> stream = f.createOutputStream();
                juce::WavAudioFormat wav;
                auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
                writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
                return f;
            };

            beginTest("MIDI panic: a held note on 8track's instruments lets go");
            {
                Standalone<AnalogSynth> syn(fresh);
                set(syn->release, 0.1f);
                juce::AudioBuffer<float> b(2, (int) kRate);
                b.clear();
                for (int s0 = 0; s0 < b.getNumSamples(); s0 += 512)
                {
                    const int n = juce::jmin(512, b.getNumSamples() - s0);
                    te::MidiMessageArray midi;
                    if (s0 == 0)
                        midi.addMidiMessage(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0.0, {});   // never let go
                    if (s0 <= (int) (0.4 * kRate) && s0 + n > (int) (0.4 * kRate))
                        syn.plugin->midiPanic();
                    const te::TimeRange range(te::TimePosition::fromSeconds(s0 / kRate), te::TimeDuration::fromSeconds(n / kRate));
                    te::PluginRenderContext fc(&b, s0, n, &midi, 0.0, range, true, false, false, false);
                    syn.plugin->applyToBuffer(fc);
                }
                expect(rms(b, 0, 0.2, 0.4) > 1.0e-3f, "the note sounds");
                expect(rms(b, 0, 0.8, 1.0) < rms(b, 0, 0.2, 0.4) * 1.0e-3f, "and stops after the panic");
            }

            beginTest("Drum Sampler: pads play their sounds from C2 up, tuned, choked, copied into the project");
            {
                Standalone<DrumSampler> d(fresh);
                const auto hit = writeTone("hit.wav", 1000.0, 0.4);
                const auto hat = writeTone("hat.wav", 3000.0, 0.4);
                juce::String err;
                expect(d->loadPad(0, hit, err), err);
                expect(d->loadPad(2, hat, err), err);
                expect(d->getPadFile(0).existsAsFile() && d->getPadFile(0) != hit, "copied into the project");
                std::vector<EffectPlugin::FileReference> refs;
                d->getFileReferences(refs);
                expectEquals((int) refs.size(), (int) DrumSampler::kPads, "every pad's file goes with Save As");
                expect(!d->loadPad(1, dir.getChildFile("nothing.wav"), err) && err.isNotEmpty(), "a missing file says why");

                auto b = playInto(*d.plugin, { { 36, 0.0, 0.1 } }, 0.2);
                expect(b.getMagnitude(0, b.getNumSamples()) < 1.0e-6f, "the previous C1 trigger is silent");
                b = playInto(*d.plugin, { { 48, 0.0, 0.05 } }, 0.5);
                expectWithinAbsoluteError(frequency(b, 0.05, 0.35, 1000.0), 1000.0, 10.0, "C2 plays pad 1, all the way (note-off doesn't stop it)");
                expect(rms(b, 0, 0.42, 0.5) < 1.0e-4f, "then stops at the end of the sound");
                b = playInto(*d.plugin, { { 49, 0.0, 0.1 } }, 0.2);
                expect(b.getMagnitude(0, b.getNumSamples()) < 1.0e-6f, "an empty pad is silent");

                set(d->pads[0].tune, 12.0f);
                b = playInto(*d.plugin, { { 48, 0.0, 0.1 } }, 0.3);
                expectWithinAbsoluteError(frequency(b, 0.02, 0.18, 2000.0), 2000.0, 20.0, "+12 is an octave up");
                set(d->pads[0].tune, 0.0f);

                d.plugin->reset();
                d->audition(0);
                b = juce::AudioBuffer<float>(2, (int) (0.1 * kRate));
                b.clear();
                render(*d.plugin, b);
                set(d->pads[0].tune, -12.0f);
                render(*d.plugin, b);
                expectWithinAbsoluteError(frequency(b, 0.02, 0.09, 500.0), 500.0, 5.0, "Pitch changes the sample already playing");
                set(d->pads[0].tune, 0.0f);

                set(d->pads[0].decay, 0.25f);
                b = playInto(*d.plugin, { { 48, 0.0, 0.1 } }, 0.4);
                expect(rms(b, 0, 0.15, 0.3) < 1.0e-3f && rms(b, 0, 0.01, 0.05) > 0.1f, "a quarter decay fades it in the first quarter");
                set(d->pads[0].decay, 1.0f);

                set(d->pads[0].choke, 1.0f);
                set(d->pads[2].choke, 1.0f);
                b = playInto(*d.plugin, { { 50, 0.0, 0.1 }, { 48, 0.1, 0.2 } }, 0.35);
                expect(levelAt(b, 3000.0, 0.15, 0.3) < levelAt(b, 1000.0, 0.15, 0.3) - 40.0f, "the second pad chokes the first");

                set(d->pads[0].pan, -1.0f);
                b = playInto(*d.plugin, { { 48, 0.0, 0.1 } }, 0.2);
                expect(rms(b, 1, 0.02, 0.18) < 1.0e-4f && rms(b, 0, 0.02, 0.18) > 0.1f, "panned hard left");

                d->audition(0, 127);
                b = juce::AudioBuffer<float>(2, (int) (0.1 * kRate));
                b.clear();
                render(*d.plugin, b);
                expect(rms(b, 0, 0.0, 0.1) > 0.1f, "clicking a pad plays it");
                expect(d->getActivity(0) > 0.1f, "and lights it");

                const auto padState = d.plugin->state.createCopy();
                Standalone<DrumSampler> again(fresh);
                again.plugin->restorePluginStateFromValueTree(padState);
                expect(again->getPadSample(0) != nullptr && again->getPadSample(2) != nullptr, "the pads come back with the project");
            }

            beginTest("Sampler: plays one sound up the keyboard from its root, loops, and keeps a recording");
            {
                Standalone<KeySampler> k(fresh);
                const auto a = writeTone("a440.wav", 440.0, 0.5);
                juce::String err;
                expect(k->loadFile(a, err), err);
                set(k->root, 69.0f);
                auto b = playInto(*k.plugin, { { 81, 0.0, 0.3 } }, 0.3);
                expectWithinAbsoluteError(frequency(b, 0.02, 0.2, 880.0), 880.0, 9.0, "an octave above the root");
                b = playInto(*k.plugin, { { 57, 0.0, 0.3 } }, 0.3);
                expectWithinAbsoluteError(frequency(b, 0.02, 0.2, 220.0), 220.0, 3.0, "an octave below");
                expect(levelAt(b, 440.0, 0.02, 0.2) < levelAt(b, 220.0, 0.02, 0.2) - 40.0f, "and not the original");

                b = playInto(*k.plugin, { { 69, 0.0, 1.5 } }, 1.0);
                expect(rms(b, 0, 0.6, 0.9) < 1.0e-4f, "without Loop, it stops at the end");
                set(k->loop, 1.0f);
                b = playInto(*k.plugin, { { 69, 0.0, 1.5 } }, 1.0);
                expect(rms(b, 0, 0.6, 0.9) > 0.1f, "with Loop, it keeps going while held");
                set(k->release, 0.01f);
                b = playInto(*k.plugin, { { 69, 0.0, 0.6 } }, 1.0);
                expect(rms(b, 0, 0.7, 1.0) < 1.0e-4f, "and stops when let go");
                set(k->loop, 0.0f);

                // A recording (as if from an input): kept in the project, its note found.
                auto tone = sine(329.63, 1.0, 0.4f, 1);
                auto take = Sample::fromBuffer(std::move(tone), kRate, "take");
                expect(k->useRecording(take, err), err);
                expect(k->getFile().existsAsFile() && k->getFile().getParentDirectory().getFileName() == "Audio",
                       "saved into the project's Audio folder: " + k->getFile().getFullPathName());
                expectEquals(juce::roundToInt(k->root->getCurrentValue()), 64, "Root is E4, heard from the recording");
                b = playInto(*k.plugin, { { 76, 0.0, 0.3 } }, 0.3);
                expectWithinAbsoluteError(frequency(b, 0.02, 0.2, 659.26), 659.26, 7.0, "and it plays in tune from there");
                expect(!k->useRecording(nullptr, err) && err.isNotEmpty(), "a silent take says so");
            }

            beginTest("Sidechain in the real mix: a muted key track ducks another track");
            {
                const double sr = engine.getSampleRate();
                auto loud = writeWav(dir.getChildFile("key.wav"), sr, (int) sr * 2, 0.5f);
                auto soft = writeWav(dir.getChildFile("soft.wav"), sr, (int) sr * 2, 0.05f);
                project.newProject();
                juce::String err;
                expect(project.importAudio(loud, 0, 0, err), err);
                expect(project.importAudio(soft, 1, 0, err), err);
                engine.getTrack(0).mute = true;   // the key still feeds the compressor
                engine.syncNow();
                auto* comp = dynamic_cast<CompressorEffect*>(engine.addEffect(1, CompressorEffect::xmlTypeName, err));
                expect(comp != nullptr, err);
                set(comp->threshold, -20.0f);
                set(comp->ratio, 20.0f);
                set(comp->attack, 0.1f);
                set(comp->knee, 0.0f);

                auto unkeyed = dir.getChildFile("unkeyed.wav"), keyed = dir.getChildFile("keyed.wav");
                expect(project.exportMix(unkeyed, err), err);
                comp->setKeySource(engine.getAudioTrack(0));
                expect(project.exportMix(keyed, err), err);
                const float plain = std::abs(sampleAt(unkeyed, 1.0)), ducked = std::abs(sampleAt(keyed, 1.0));
                expect(plain > 0.01f, "the soft track is under the threshold: " + juce::String(plain));
                // The key is 14 dB over -20 dB, so at (nearly) infinite ratio the soft track drops ~14 dB.
                expectWithinAbsoluteError(juce::Decibels::gainToDecibels(ducked / plain), -14.0f, 1.5f,
                                          "keyed: ducked by the muted track");

                beginTest("Limiter on the master: the export never goes over the ceiling");
                auto* limiter = dynamic_cast<LimiterEffect*>(engine.addEffect(AudioEngine::kMasterBus, LimiterEffect::xmlTypeName, err));
                expect(limiter != nullptr, err);
                engine.removeInsert(*comp);
                engine.getTrack(0).mute = false;
                engine.syncNow();
                set(limiter->gain, 12.0f);
                set(limiter->ceiling, -3.0f);
                auto limited = dir.getChildFile("limited.wav");
                expect(project.exportMix(limited, err), err);
                expectWithinAbsoluteError(std::abs(sampleAt(limited, 1.0)), juce::Decibels::decibelsToGain(-3.0f), 0.01f,
                                          "held at -3 dB");
            }

            beginTest("Gate keys from a muted track and saves its detector source");
            {
                const double sr = engine.getSampleRate();
                const auto key = writeWav(dir.getChildFile("gate-key.wav"), sr, (int) (sr * 0.5), 0.5f);
                const auto source = writeWav(dir.getChildFile("gate-source.wav"), sr, (int) (sr * 2), 0.08f);
                project.newProject();
                juce::String error;
                expect(project.importAudio(key, 0, (juce::int64) (sr * 0.5), error), error);
                expect(project.importAudio(source, 1, 0, error), error);
                engine.getTrack(0).mute = true;
                engine.syncNow();

                auto* gate = dynamic_cast<GateEffect*>(engine.addEffect(1, GateEffect::xmlTypeName, error));
                expect(gate != nullptr, error);
                if (gate != nullptr)
                {
                    set(gate->threshold, -18.0f);
                    set(gate->hold, 0.0f);
                    set(gate->release, 20.0f);
                    gate->setEnabled(false);
                    const auto dry = dir.getChildFile("gate-dry.wav");
                    expect(project.exportMix(dry, error), error);
                    gate->setEnabled(true);
                    gate->setKeySource(engine.getAudioTrack(0));
                    expect(gate->getKeySource() == engine.getAudioTrack(0));
                    expectEquals(gate->getNumWires(), 4);

                    const auto destination = dir.getChildFile("gate-project");
                    expect(project.saveAs(destination, error), error);
                    expect(project.load(destination, error), error);
                    const auto inserts = engine.getInserts(1);
                    auto* restored = inserts.empty() ? nullptr : dynamic_cast<GateEffect*>(inserts.front());
                    expect(restored != nullptr && restored->getKeySource() == engine.getAudioTrack(0));

                    const auto keyed = dir.getChildFile("gate-keyed.wav");
                    expect(project.exportMix(keyed, error), error);
                    const auto audible = sampleAt(dry, 0.75);
                    expect(audible > 0.01f, "there is signal for the gate to open");
                    expectWithinAbsoluteError(sampleAt(keyed, 0.25), 0.0f, 0.001f, "closed before the key");
                    expectWithinAbsoluteError(sampleAt(keyed, 0.75), audible, 0.001f, "the key opens the gate without mixing into the output");
                    expectWithinAbsoluteError(sampleAt(keyed, 1.5), 0.0f, 0.001f, "closed after the key");
                }
            }

            beginTest("Every effect handles mono, and never makes NaNs");
            for (auto& type : types)
            {
                auto plugin = project.getEdit().getPluginCache().createNewPlugin(type.xmlType, {});
                plugin->baseClassInitialise({ te::TimePosition(), kRate, 512 });
                for (auto* param : plugin->getAutomatableParameters())
                    param->setParameter(param->getValueRange().getEnd(), juce::sendNotification);   // everything up
                auto mono = sine(330.0, 0.5, 0.9f, 1);
                render(*plugin, mono, true);
                bool finite = true;
                for (int i = 0; i < mono.getNumSamples(); ++i)
                    finite = finite && std::isfinite(mono.getSample(0, i));
                expect(finite, type.name);
                plugin->baseClassDeinitialise();
            }
        }
        dir.deleteRecursively();
    }
};

static ShortcutTests shortcutTests;
static PianoRollTests pianoRollTests;
static SongwritingTests songwritingTests;
static LoopTests loopTests;
static MidiAssignTests midiAssignTests;
static MidiInputRoutingTests midiInputRoutingTests;
static ClipOperationTests clipOperationTests;
static ArrangementTests arrangementTests;
static TracktionProjectTests tracktionProjectTests;
static PluginInsertTests pluginInsertTests;
static StereoBusTests stereoBusTests;
static StereoInputTests stereoInputTests;
static MidiTrackTests midiTrackTests;
static AutomationTests automationTests;
static EffectTests effectTests;

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

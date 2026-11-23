#include "Project.h"
#include "AudioFileIO.h"
#include <map>

namespace
{
    juce::File projectsRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("8track-projects");
    }
}

Project::Project(AudioEngine& e) : engine(e)
{
    positionsRate = engine.getSampleRate();
    folder = makeUntitledFolder();
    publish();
}

juce::File Project::makeUntitledFolder() const
{
    // Created on disk only when something is recorded or imported into it.
    return projectsRoot().getChildFile(".untitled")
               .getChildFile(juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"));
}

void Project::publish()
{
    engine.setArrangement(std::make_shared<Arrangement>(arrangement));
}

void Project::changed()
{
    publish();
    if (onChanged)
        onChanged();
}

void Project::markDirty()
{
    dirty = true;
}

bool Project::perform(const std::function<bool(Arrangement&)>& edit)
{
    Arrangement before = arrangement;
    if (!edit(arrangement))
        return false;

    history.push(before);
    dirty = true;
    changed();
    return true;
}

bool Project::undo()
{
    if (!history.undo(arrangement))
        return false;
    dirty = true;
    changed();
    return true;
}

bool Project::redo()
{
    if (!history.redo(arrangement))
        return false;
    dirty = true;
    changed();
    return true;
}

void Project::addTakes(const std::vector<AudioEngine::FinishedTake>& takes)
{
    if (takes.empty())
        return;

    perform([&](Arrangement& a)
    {
        for (auto& take : takes)
        {
            Clip c;
            c.source = take.source;
            c.start = take.start;
            c.length = take.length;
            c.fadeIn = c.fadeOut = Arrangement::kEdgeFade;

            // A take that started during the count-in begins before zero; drop that part.
            if (c.start < 0)
            {
                c.sourceOffset = -c.start;
                c.length += c.start;
                c.start = 0;
            }
            if (c.length >= Arrangement::kMinClipLength)
                a.insertClip(take.track, c);
        }
        return true;
    });
}

bool Project::importAudio(const juce::File& file, int track, juce::int64 position, juce::String& error)
{
    auto audioFolder = getAudioFolder();
    if (!audioFolder.createDirectory())
    {
        error = "Couldn't create " + audioFolder.getFullPathName();
        return false;
    }

    auto dest = audioFolder.getNonexistentChildFile(file.getFileNameWithoutExtension(), file.getFileExtension(), false);
    if (!file.copyFileTo(dest))
    {
        error = "Couldn't copy " + file.getFileName() + " into the project";
        return false;
    }

    auto source = AudioFileIO::loadSource(dest, engine.getSampleRate(), error);
    if (source == nullptr)
    {
        dest.deleteFile();
        return false;
    }

    return perform([&](Arrangement& a)
    {
        Clip c;
        c.source = source;
        c.start = juce::jmax<juce::int64>(0, position);
        c.length = source->getLength();
        c.fadeIn = c.fadeOut = Arrangement::kEdgeFade;
        return a.insertClip(track, c) != 0;
    });
}

//==============================================================================
void Project::newProject()
{
    if (engine.getState() != AudioEngine::State::Stopped)
        engine.stop();

    arrangement = {};
    history.clear();
    folder = makeUntitledFolder();
    untitled = true;
    dirty = false;
    positionsRate = engine.getSampleRate();

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = engine.getTrack(t);
        p.resetMixer();
        p.name = "Track " + juce::String(t + 1);
    }
    auto& m = engine.getMaster();
    m.volumeDb = 0.0f;
    m.reverbRoom = 0.5f;
    m.reverbDamping = 0.5f;
    m.reverbWet = 0.33f;
    engine.punchOn = false;
    engine.punchIn = 0;
    engine.punchOut = 0;
    engine.setPosition(0);

    changed();
}

std::unique_ptr<juce::XmlElement> Project::toXml() const
{
    auto root = std::make_unique<juce::XmlElement>("EIGHTTRACK");
    root->setAttribute("version", 2);
    root->setAttribute("sampleRate", positionsRate);
    root->setAttribute("bpm", engine.bpm.load());
    root->setAttribute("beatsPerBar", engine.beatsPerBar.load());
    root->setAttribute("metronome", engine.metronomeOn.load());
    root->setAttribute("countIn", engine.countInOn.load());
    root->setAttribute("punchOn", engine.punchOn.load());
    root->setAttribute("punchIn", juce::String(engine.punchIn.load()));
    root->setAttribute("punchOut", juce::String(engine.punchOut.load()));

    auto& m = const_cast<AudioEngine&>(engine).getMaster();
    auto* master = root->createNewChildElement("MASTER");
    master->setAttribute("volumeDb", m.volumeDb.load());
    master->setAttribute("reverbRoom", m.reverbRoom.load());
    master->setAttribute("reverbDamping", m.reverbDamping.load());
    master->setAttribute("reverbWet", m.reverbWet.load());

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& p = const_cast<AudioEngine&>(engine).getTrack(t);
        auto* tr = root->createNewChildElement("TRACK");
        tr->setAttribute("index", t);
        tr->setAttribute("name", p.name);
        tr->setAttribute("volumeDb", p.volumeDb.load());
        tr->setAttribute("pan", p.pan.load());
        tr->setAttribute("send", p.send.load());
        tr->setAttribute("lowCut", p.lowCut.load());
        tr->setAttribute("highCut", p.highCut.load());
        tr->setAttribute("midFreq", p.midFreq.load());
        tr->setAttribute("midGainDb", p.midGainDb.load());
        tr->setAttribute("mute", p.mute.load());
        tr->setAttribute("solo", p.solo.load());
        tr->setAttribute("monitor", p.monitor.load());
        tr->setAttribute("inputType", p.inputType.load());
        tr->setAttribute("inputChannel", p.inputChannel.load());

        for (auto& c : arrangement.tracks[(size_t) t])
        {
            auto* clip = tr->createNewChildElement("CLIP");
            clip->setAttribute("file", c.source->file.getRelativePathFrom(folder).replaceCharacter('\\', '/'));
            clip->setAttribute("start", juce::String(c.start));
            clip->setAttribute("offset", juce::String(c.sourceOffset));
            clip->setAttribute("length", juce::String(c.length));
            clip->setAttribute("fadeIn", juce::String(c.fadeIn));
            clip->setAttribute("fadeOut", juce::String(c.fadeOut));
            clip->setAttribute("gain", c.gain);
        }
    }
    return root;
}

bool Project::save(juce::String& error)
{
    jassert(!untitled);
    if (!folder.createDirectory())
    {
        error = "Couldn't create " + folder.getFullPathName();
        return false;
    }

    auto xml = toXml();
    auto file = folder.getChildFile(kProjectFileName);
    auto temp = file.getSiblingFile(juce::String(kProjectFileName) + ".tmp");
    if (!xml->writeTo(temp) || !temp.moveFileTo(file))
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }
    dirty = false;
    return true;
}

bool Project::saveAs(const juce::File& newFolder, juce::String& error)
{
    auto audioDir = newFolder.getChildFile("Audio");
    if (!audioDir.createDirectory())
    {
        error = "Couldn't create " + audioDir.getFullPathName();
        return false;
    }

    // Copy every file the arrangement uses into the new project's Audio folder.
    for (auto& lane : arrangement.tracks)
    {
        for (auto& c : lane)
        {
            auto& src = c.source->file;
            if (src.isAChildOf(newFolder))
                continue;

            auto dest = audioDir.getChildFile(src.getFileName());
            if (!dest.existsAsFile() && !src.copyFileTo(dest))
            {
                error = "Couldn't copy " + src.getFileName() + " to " + audioDir.getFullPathName();
                return false;
            }
            src = dest;   // later saves and recordings use the new location
        }
    }

    folder = newFolder;
    untitled = false;
    return save(error);
}

bool Project::load(const juce::File& folderOrFile, juce::String& error)
{
    auto projectFolder = folderOrFile.isDirectory() ? folderOrFile : folderOrFile.getParentDirectory();
    auto file = projectFolder.getChildFile(kProjectFileName);
    if (!file.existsAsFile())
        file = projectFolder.getChildFile(kLegacyProjectFileName);   // saved before the rename

    auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || !(xml->hasTagName("EIGHTTRACK") || xml->hasTagName("FOURTRACK")))
    {
        error = "No 8track project found in " + projectFolder.getFullPathName();
        return false;
    }

    if (engine.getState() != AudioEngine::State::Stopped)
        engine.stop();

    const double engineRate = engine.getSampleRate();
    const double fileRate = xml->getDoubleAttribute("sampleRate", engineRate);
    const double ratio = engineRate / fileRate;
    auto scale = [ratio](const juce::String& v) { return (juce::int64) std::llround((double) v.getLargeIntValue() * ratio); };

    Arrangement loaded;
    std::map<juce::String, AudioSourcePtr> sources;
    juce::StringArray missing;

    for (auto* tr : xml->getChildWithTagNameIterator("TRACK"))
    {
        const int t = tr->getIntAttribute("index", -1);
        if (t < 0 || t >= kNumTracks)
            continue;

        auto& p = engine.getTrack(t);
        p.name = tr->getStringAttribute("name", "Track " + juce::String(t + 1));
        p.volumeDb = (float) tr->getDoubleAttribute("volumeDb", 0.0);
        p.pan = (float) tr->getDoubleAttribute("pan", 0.0);
        p.send = (float) tr->getDoubleAttribute("send", 0.0);
        p.lowCut = (float) tr->getDoubleAttribute("lowCut", 20.0);
        p.highCut = (float) tr->getDoubleAttribute("highCut", 20000.0);
        p.midFreq = (float) tr->getDoubleAttribute("midFreq", 1000.0);
        p.midGainDb = (float) tr->getDoubleAttribute("midGainDb", 0.0);
        p.mute = tr->getBoolAttribute("mute");
        p.solo = tr->getBoolAttribute("solo");
        p.armed = false;
        p.monitor = tr->getIntAttribute("monitor", (int) MonitorMode::Off);
        p.inputType = tr->getIntAttribute("inputType", (int) InputType::Mono);
        p.inputChannel = tr->getIntAttribute("inputChannel", 0);

        for (auto* cx : tr->getChildWithTagNameIterator("CLIP"))
        {
            const auto rel = cx->getStringAttribute("file");
            auto& source = sources[rel];
            if (source == nullptr)
            {
                juce::String loadError;
                source = AudioFileIO::loadSource(projectFolder.getChildFile(rel), engineRate, loadError);
                if (source == nullptr)
                {
                    missing.addIfNotAlreadyThere(rel);
                    continue;
                }
            }

            Clip c;
            c.source = source;
            c.start = scale(cx->getStringAttribute("start"));
            c.sourceOffset = scale(cx->getStringAttribute("offset"));
            c.length = juce::jmin(scale(cx->getStringAttribute("length")), source->getLength() - c.sourceOffset);
            c.fadeIn = scale(cx->getStringAttribute("fadeIn"));
            c.fadeOut = scale(cx->getStringAttribute("fadeOut"));
            c.gain = (float) cx->getDoubleAttribute("gain", 1.0);
            if (c.length > 0)
                loaded.insertClip(t, c);
        }
    }

    if (auto* m = xml->getChildByName("MASTER"))
    {
        auto& master = engine.getMaster();
        master.volumeDb = (float) m->getDoubleAttribute("volumeDb", 0.0);
        master.reverbRoom = (float) m->getDoubleAttribute("reverbRoom", 0.5);
        master.reverbDamping = (float) m->getDoubleAttribute("reverbDamping", 0.5);
        master.reverbWet = (float) m->getDoubleAttribute("reverbWet", 0.33);
    }

    engine.bpm = xml->getDoubleAttribute("bpm", 120.0);
    engine.beatsPerBar = xml->getIntAttribute("beatsPerBar", 4);
    engine.metronomeOn = xml->getBoolAttribute("metronome");
    engine.countInOn = xml->getBoolAttribute("countIn", true);
    engine.punchOn = xml->getBoolAttribute("punchOn");
    engine.punchIn = scale(xml->getStringAttribute("punchIn", "0"));
    engine.punchOut = scale(xml->getStringAttribute("punchOut", "0"));
    engine.setPosition(0);

    arrangement = std::move(loaded);
    history.clear();
    folder = projectFolder;
    untitled = false;
    dirty = ratio != 1.0;   // positions were converted; saving will store them at the new rate
    positionsRate = engineRate;
    changed();

    if (!missing.isEmpty())
        error = "Some audio files are missing and were skipped:\n" + missing.joinIntoString("\n");
    return true;
}

bool Project::exportMix(const juce::File& file, juce::String& error)
{
    if (arrangement.isEmpty())
    {
        error = "There's nothing to export yet.";
        return false;
    }

    auto mix = MixEngine::renderOffline(arrangement, engine.getParamArray(), engine.getMaster(),
                                        engine.getSampleRate(), 3.0);
    return AudioFileIO::writeWav(file, mix, engine.getSampleRate(), 24, error);
}

void Project::handleSampleRateChange()
{
    const double newRate = engine.getSampleRate();
    if (std::abs(newRate - positionsRate) < 0.5 || positionsRate <= 0.0 || arrangement.isEmpty())
    {
        if (arrangement.isEmpty() && std::abs(newRate - positionsRate) >= 0.5)
        {
            // Nothing to convert, but keep punch points at the same musical position.
            const double ratio = newRate / juce::jmax(1.0, positionsRate);
            engine.punchIn = (juce::int64) std::llround((double) engine.punchIn.load() * ratio);
            engine.punchOut = (juce::int64) std::llround((double) engine.punchOut.load() * ratio);
        }
        positionsRate = newRate;
        return;
    }

    // Resample every source in use and rescale positions. Undo history refers to the
    // old sample positions, so it's cleared.
    std::map<const AudioSourceData*, AudioSourcePtr> converted;
    for (auto& lane : arrangement.tracks)
        for (auto& c : lane)
        {
            auto& conv = converted[c.source.get()];
            if (conv == nullptr)
                conv = AudioFileIO::resampled(*c.source, positionsRate, newRate);
            c.source = conv;
        }

    const double ratio = newRate / positionsRate;
    arrangement.rescalePositions(ratio);
    engine.punchIn = (juce::int64) std::llround((double) engine.punchIn.load() * ratio);
    engine.punchOut = (juce::int64) std::llround((double) engine.punchOut.load() * ratio);
    engine.setPosition((juce::int64) std::llround((double) engine.getPosition() * ratio));

    history.clear();
    positionsRate = newRate;
    dirty = true;
    changed();
}

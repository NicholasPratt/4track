#include "Project.h"
#include <map>
#include <set>

namespace
{
    juce::File projectsRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("8track-projects");
    }

    bool sameClips(const Arrangement& a, const Arrangement& b)
    {
        for (size_t t = 0; t < a.tracks.size(); ++t)
        {
            if (a.tracks[t].size() != b.tracks[t].size())
                return false;
            for (size_t i = 0; i < a.tracks[t].size(); ++i)
            {
                const auto& x = a.tracks[t][i];
                const auto& y = b.tracks[t][i];
                if (x.id != y.id || x.midi != y.midi || x.file != y.file || x.start != y.start || x.length != y.length
                    || x.sourceOffset != y.sourceOffset || x.sourceLength != y.sourceLength
                    || x.fadeIn != y.fadeIn || x.fadeOut != y.fadeOut || std::abs(x.gain - y.gain) > 1.0e-5f)
                    return false;
            }
        }
        return true;
    }

    float gainToDb(float gain) { return gain <= 0.00001f ? -100.0f : juce::Decibels::gainToDecibels(gain); }
}

Project::Project(AudioEngine& e) : engine(e)
{
    engine.addChangeListener(this);
    newProject();
}

Project::~Project()
{
    engine.removeChangeListener(this);
    cancelPendingUpdate();
    if (edit != nullptr)
    {
        engine.stop();
        editState.removeListener(this);
        engine.attachEdit(nullptr);
    }
}

juce::File Project::makeUntitledFolder() const
{
    // Created on disk only when something is recorded, imported or saved into it.
    return projectsRoot().getChildFile(".untitled")
               .getChildFile(juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"));
}

juce::int64 Project::fileLength(const juce::File& f) const
{
    // Read synchronously (and cached): Tracktion's clip source length is 0 until its
    // background scan of the file finishes, and the Arrangement needs it straight away.
    const auto key = f.getFullPathName() + "@" + juce::String(engine.getSampleRate());
    if (auto it = lengthCache.find(key); it != lengthCache.end() && f.existsAsFile())
        return it->second;

    te::AudioFile audio(engine.getTracktion(), f);
    const auto length = audio.isValid() ? (juce::int64) std::llround(audio.getLength() * engine.getSampleRate()) : 0;
    if (length > 0)
        lengthCache[key] = length;
    return length;
}

//==============================================================================
void Project::setEdit(std::unique_ptr<te::Edit> newEdit, bool fresh)
{
    engine.stop();
    settingUp = true;

    if (edit != nullptr)
    {
        editState.removeListener(this);
        engine.attachEdit(nullptr);
    }
    edit = std::move(newEdit);   // the old Edit is deleted here, after the engine has let go of it
    editState = edit->state;

    engine.attachEdit(edit.get(), fresh);
    editState.addListener(this);

    arrangement = {};
    rebuild();
    cancelPendingUpdate();
    settingUp = false;
    dirty = false;
    if (onChanged)
        onChanged();
}

void Project::rebuild()
{
    if (edit == nullptr)
        return;

    Arrangement next;
    juce::int64 maxId = 0;

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto* track = engine.getAudioTrack(t);
        if (track == nullptr)
            continue;

        for (auto* teClip : track->getClips())
        {
            if (auto* midiClip = dynamic_cast<te::MidiClip*>(teClip))
            {
                const auto pos = midiClip->getPosition();
                Clip c;
                c.id = (juce::int64) midiClip->itemID.getRawID();
                c.midi = true;
                c.midiState = midiClip->state;
                c.sourceLength = Clip::kMidiSourceLength;
                c.start = engine.toSamples(pos.getStart());
                c.length = engine.toSamples(pos.getLength());
                c.sourceOffset = engine.toSamples(pos.getOffset());
                next.tracks[(size_t) t].push_back(c);
                maxId = juce::jmax(maxId, c.id);
                continue;
            }

            auto* wave = dynamic_cast<te::WaveAudioClip*>(teClip);
            if (wave == nullptr)
                continue;

            const auto pos = wave->getPosition();
            Clip c;
            c.id = (juce::int64) wave->itemID.getRawID();
            c.file = wave->getOriginalFile();
            c.sourceLength = fileLength(c.file);
            c.start = engine.toSamples(pos.getStart());
            c.length = engine.toSamples(pos.getLength());
            c.sourceOffset = engine.toSamples(pos.getOffset());
            c.fadeIn = engine.toSamples(wave->getFadeIn());
            c.fadeOut = engine.toSamples(wave->getFadeOut());
            c.gain = juce::Decibels::decibelsToGain(wave->getGainDB(), -100.0f);
            next.tracks[(size_t) t].push_back(c);
            maxId = juce::jmax(maxId, c.id);
        }
        std::sort(next.tracks[(size_t) t].begin(), next.tracks[(size_t) t].end(),
                  [](const Clip& a, const Clip& b) { return a.start < b.start; });
    }
    // New clips made by Arrangement edits get ids above every clip in the Edit.
    next.nextClipId = maxId + 1;

    if (!sameClips(next, arrangement))
    {
        arrangement = std::move(next);
        if (onChanged && !settingUp)
            onChanged();
    }
    arrangement.nextClipId = next.nextClipId;
}

void Project::writeToEdit(const Arrangement& target)
{
    // Clips currently in the Edit, by id, with the track they are on.
    std::map<juce::int64, std::pair<te::Clip*, int>> existing;
    for (int t = 0; t < kNumTracks; ++t)
        if (auto* track = engine.getAudioTrack(t))
            for (auto* c : track->getClips())
                if (dynamic_cast<te::AudioClipBase*>(c) != nullptr || dynamic_cast<te::MidiClip*>(c) != nullptr)
                    existing[(juce::int64) c->itemID.getRawID()] = { c, t };

    std::set<juce::int64> kept;
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto* track = engine.getAudioTrack(t);
        for (auto& c : target.tracks[(size_t) t])
        {
            const te::ClipPosition position { { engine.toTime(c.start), engine.toTime(c.end()) },
                                              engine.toDuration(c.sourceOffset) };
            te::Clip* clip = nullptr;

            if (auto it = existing.find(c.id); it != existing.end())
            {
                clip = it->second.first;
                kept.insert(c.id);
                if (it->second.second != t)
                    clip->moveTo(*track);
                clip->setPosition(position);
            }
            else if (c.midi)
            {
                // A new MIDI clip: a copy of the clip it was cut from (split, trimmed by a
                // move), or an empty one.
                if (c.midiState.isValid())
                    clip = track->insertClipWithState(c.midiState.createCopy(), c.midiState[te::IDs::name].toString(),
                                                      te::TrackItem::Type::midi, position, false, false);
                else
                    clip = track->insertMIDIClip(position.time, nullptr).get();
                if (clip != nullptr)
                    clip->setPosition(position);
            }
            else
            {
                auto made = track->insertWaveClip(c.file.getFileNameWithoutExtension(), c.file, position, false);
                if (made == nullptr)
                    continue;
                made->getSourceFileReference().setToFile(c.file, te::SourceFileReference::PathStyle::chooseBest, false);
                clip = made.get();
            }

            if (auto* audio = dynamic_cast<te::AudioClipBase*>(clip))
            {
                audio->setFadeIn(engine.toDuration(c.fadeIn));
                audio->setFadeOut(engine.toDuration(c.fadeOut));
                audio->setGainDB(gainToDb(c.gain));
            }
        }
    }

    for (auto& [id, entry] : existing)
        if (kept.count(id) == 0)
            entry.first->removeFromParent();
}

//==============================================================================
bool Project::perform(const std::function<bool(Arrangement&)>& fn)
{
    Arrangement next = arrangement;
    if (!fn(next))
        return false;

    edit->getUndoManager().beginNewTransaction("Edit");
    writeToEdit(next);
    edit->getUndoManager().beginNewTransaction();
    dirty = true;
    rebuild();
    if (onChanged)
        onChanged();
    return true;
}

bool Project::canUndo() const { return edit != nullptr && edit->getUndoManager().canUndo(); }
bool Project::canRedo() const { return edit != nullptr && edit->getUndoManager().canRedo(); }

bool Project::undo()
{
    if (!canUndo() || !edit->getUndoManager().undo())
        return false;
    dirty = true;
    rebuild();
    if (onChanged)
        onChanged();
    return true;
}

bool Project::redo()
{
    if (!canRedo() || !edit->getUndoManager().redo())
        return false;
    dirty = true;
    rebuild();
    if (onChanged)
        onChanged();
    return true;
}

void Project::markDirty()
{
    dirty = true;
}

bool Project::prepareFolder(juce::String& error)
{
    if (!getAudioFolder().createDirectory())
    {
        error = "Couldn't create " + getAudioFolder().getFullPathName();
        return false;
    }
    if (!editFile().existsAsFile() && !te::EditFileOperations(*edit).writeToFile(editFile(), false))
    {
        error = "Couldn't write " + editFile().getFullPathName();
        return false;
    }
    return true;
}

te::MidiClip* Project::createMidiClip(int track, juce::int64 start, juce::int64 length)
{
    juce::int64 newId = 0;
    perform([&](Arrangement& a)
    {
        Clip c;
        c.midi = true;
        c.sourceLength = Clip::kMidiSourceLength;
        c.start = juce::jmax<juce::int64>(0, start);
        c.length = length;
        newId = a.insertClip(track, c);
        return newId != 0;
    });
    if (newId == 0)
        return nullptr;

    // The Edit gave the new clip its own id; it's the MIDI clip now at that position.
    for (auto& c : arrangement.tracks[(size_t) track])
        if (c.midi && std::abs(c.start - juce::jmax<juce::int64>(0, start)) <= 1)
            return getMidiClip(c.id);
    return nullptr;
}

te::MidiClip* Project::getMidiClip(juce::int64 clipId) const
{
    if (edit == nullptr)
        return nullptr;
    return dynamic_cast<te::MidiClip*>(te::findClipForID(*edit, te::EditItemID::fromRawID((juce::uint64) clipId)));
}

bool Project::importAudio(const juce::File& file, int track, juce::int64 position, juce::String& error)
{
    if (engine.getTrack(track).midi.load())
    {
        error = engine.getTrack(track).name + " is a MIDI track; audio goes on audio tracks";
        return false;
    }
    if (!te::AudioFile(engine.getTracktion(), file).isValid())
    {
        error = "Couldn't read " + file.getFileName() + " as audio";
        return false;
    }

    if (!prepareFolder(error))
        return false;

    auto dest = getAudioFolder().getNonexistentChildFile(file.getFileNameWithoutExtension(), file.getFileExtension(), false);
    if (!file.copyFileTo(dest))
    {
        error = "Couldn't copy " + file.getFileName() + " into the project";
        return false;
    }

    const auto length = fileLength(dest);
    return perform([&](Arrangement& a)
    {
        Clip c;
        c.file = dest;
        c.sourceLength = length;
        c.start = juce::jmax<juce::int64>(0, position);
        c.length = length;
        c.fadeIn = c.fadeOut = Arrangement::kEdgeFade;
        return a.insertClip(track, c) != 0;
    });
}

//==============================================================================
void Project::newProject()
{
    folder = makeUntitledFolder();
    untitled = true;
    setEdit(te::createEmptyEdit(engine.getTracktion(), editFile()), true);
}

bool Project::save(juce::String& error)
{
    jassert(!untitled);
    if (!folder.createDirectory())
    {
        error = "Couldn't create " + folder.getFullPathName();
        return false;
    }

    engine.syncNow();
    auto file = editFile();
    auto temp = file.getSiblingFile(juce::String(kProjectFileName) + ".tmp");
    if (!te::EditFileOperations(*edit).writeToFile(temp, false) || !temp.moveFileTo(file))
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }
    dirty = false;
    if (onChanged)
        onChanged();
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

    // Where each clip's audio is now. Read before the Edit file moves: relative paths
    // resolve against it.
    std::vector<std::pair<te::WaveAudioClip::Ptr, juce::File>> sources;
    for (int t = 0; t < kNumTracks; ++t)
        for (auto* c : engine.getAudioTrack(t)->getClips())
            if (auto* wave = dynamic_cast<te::WaveAudioClip*>(c))
                sources.push_back({ wave, wave->getOriginalFile() });

    // Copy every file the clips use into the new project's Audio folder.
    std::map<juce::String, juce::File> copied;
    for (auto& [clip, src] : sources)
    {
        auto& dest = copied[src.getFullPathName()];
        if (dest != juce::File())
            continue;
        dest = src.isAChildOf(newFolder) ? src : audioDir.getChildFile(src.getFileName());
        if (dest != src && !dest.existsAsFile() && !src.copyFileTo(dest))
        {
            error = "Couldn't copy " + src.getFileName() + " to " + audioDir.getFullPathName();
            return false;
        }
    }

    const auto newFile = newFolder.getChildFile(kProjectFileName);
    edit->editFileRetriever = [newFile] { return newFile; };   // new recordings and relative paths use it
    folder = newFolder;
    untitled = false;
    if (!te::EditFileOperations(*edit).writeToFile(newFile, false))   // must exist for relative paths
    {
        error = "Couldn't write " + newFile.getFullPathName();
        return false;
    }

    for (auto& [clip, src] : sources)
        clip->getSourceFileReference().setToFile(copied[src.getFullPathName()],
                                                 te::SourceFileReference::PathStyle::alwaysRelative, false);

    return save(error);
}

bool Project::load(const juce::File& folderOrFile, juce::String& error)
{
    auto projectFolder = folderOrFile.isDirectory() ? folderOrFile : folderOrFile.getParentDirectory();
    auto file = projectFolder.getChildFile(kProjectFileName);
    if (!file.existsAsFile())
        file = projectFolder.getChildFile(kLegacyProjectFileName);   // saved before the rename

    auto xml = juce::XmlDocument::parse(file);
    if (xml != nullptr && (xml->hasTagName("EIGHTTRACK") || xml->hasTagName("FOURTRACK")))
        return importLegacy(*xml, projectFolder, error);

    if (xml == nullptr || !xml->hasTagName(te::IDs::EDIT.toString()))
    {
        error = "No 8track project found in " + projectFolder.getFullPathName();
        return false;
    }

    auto loaded = te::loadEditFromFile(engine.getTracktion(), file);
    if (loaded == nullptr)
    {
        error = "Couldn't open " + file.getFullPathName();
        return false;
    }
    const auto editFileForProject = projectFolder.getChildFile(kProjectFileName);
    loaded->editFileRetriever = [editFileForProject] { return editFileForProject; };

    folder = projectFolder;
    untitled = false;
    setEdit(std::move(loaded), false);

    juce::StringArray missing;
    for (auto& lane : arrangement.tracks)
        for (auto& c : lane)
            if (!c.file.existsAsFile())
                missing.addIfNotAlreadyThere(c.file.getFileName());
    if (!missing.isEmpty())
        error = "Some audio files are missing:\n" + missing.joinIntoString("\n");
    return true;
}

bool Project::importLegacy(const juce::XmlElement& xml, const juce::File& projectFolder, juce::String& error)
{
    folder = projectFolder;
    untitled = false;
    setEdit(te::createEmptyEdit(engine.getTracktion(), editFile()), true);

    const double engineRate = engine.getSampleRate();
    const double fileRate = xml.getDoubleAttribute("sampleRate", engineRate);
    const double ratio = engineRate / fileRate;
    auto scale = [ratio](const juce::String& v) { return (juce::int64) std::llround((double) v.getLargeIntValue() * ratio); };

    Arrangement loaded;
    std::map<juce::String, juce::int64> lengths;
    juce::StringArray missing;

    for (auto* tr : xml.getChildWithTagNameIterator("TRACK"))
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
        p.inputType = (int) InputType::Mono;
        p.inputChannel = tr->getIntAttribute("inputChannel", 0);

        for (auto* cx : tr->getChildWithTagNameIterator("CLIP"))
        {
            const auto rel = cx->getStringAttribute("file");
            const auto file = projectFolder.getChildFile(rel);
            if (lengths.count(rel) == 0)
                lengths[rel] = fileLength(file);
            if (lengths[rel] <= 0)
            {
                missing.addIfNotAlreadyThere(rel);
                continue;
            }

            Clip c;
            c.file = file;
            c.sourceLength = lengths[rel];
            c.start = scale(cx->getStringAttribute("start"));
            c.sourceOffset = scale(cx->getStringAttribute("offset"));
            c.length = juce::jmin(scale(cx->getStringAttribute("length")), c.sourceLength - c.sourceOffset);
            c.fadeIn = scale(cx->getStringAttribute("fadeIn"));
            c.fadeOut = scale(cx->getStringAttribute("fadeOut"));
            c.gain = (float) cx->getDoubleAttribute("gain", 1.0);
            if (c.length > 0)
                loaded.insertClip(t, c);
        }
    }

    if (auto* m = xml.getChildByName("MASTER"))
    {
        auto& master = engine.getMaster();
        master.volumeDb = (float) m->getDoubleAttribute("volumeDb", 0.0);
        master.reverbRoom = (float) m->getDoubleAttribute("reverbRoom", 0.5);
        master.reverbDamping = (float) m->getDoubleAttribute("reverbDamping", 0.5);
        master.reverbWet = (float) m->getDoubleAttribute("reverbWet", 0.33);
    }

    engine.bpm = xml.getDoubleAttribute("bpm", 120.0);
    engine.beatsPerBar = xml.getIntAttribute("beatsPerBar", 4);
    engine.metronomeOn = xml.getBoolAttribute("metronome");
    engine.countInOn = xml.getBoolAttribute("countIn", true);
    engine.punchOn = xml.getBoolAttribute("punchOn");
    engine.punchIn = scale(xml.getStringAttribute("punchIn", "0"));
    engine.punchOut = scale(xml.getStringAttribute("punchOut", "0"));
    engine.syncNow();

    // Place the clips, then make the import the starting point rather than an undo step.
    if (!editFile().existsAsFile())
        te::EditFileOperations(*edit).writeToFile(editFile(), false);   // relative clip paths need it
    settingUp = true;
    writeToEdit(loaded);
    edit->getUndoManager().clearUndoHistory();
    rebuild();
    settingUp = false;
    engine.setPosition(0);

    dirty = true;   // saving converts the project to the new format
    if (onChanged)
        onChanged();

    if (!missing.isEmpty())
        error = "Some audio files are missing and were skipped:\n" + missing.joinIntoString("\n");
    return true;
}

bool Project::exportMix(const juce::File& file, juce::String& error)
{
    const auto end = arrangement.getEnd();
    if (end <= 0)
    {
        error = "There's nothing to export yet.";
        return false;
    }

    engine.stop();
    engine.syncNow();
    file.deleteFile();

    const te::TimeRange range { te::TimePosition(), engine.toTime(end) + te::TimeDuration::fromSeconds(3.0) };   // reverb tail
    const auto tracks = te::toBitSet(te::getAllTracks(*edit));
    if (!te::Renderer::renderToFile("Export", file, *edit, range, tracks, true, false, {}, false) || !file.existsAsFile())
    {
        error = "Couldn't render the mix to " + file.getFullPathName();
        return false;
    }
    return true;
}

//==============================================================================
void Project::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) { editTreeChanged(tree); }
void Project::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)            { editTreeChanged(parent); }
void Project::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)     { editTreeChanged(parent); }
void Project::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)             { editTreeChanged(parent); }

void Project::editTreeChanged(const juce::ValueTree& tree)
{
    if (settingUp)
        return;

    // The playhead and input routing live in the Edit too, but aren't changes to the song.
    for (auto t = tree; t.isValid(); t = t.getParent())
        if (t.hasType(te::IDs::TRANSPORT) || t.hasType(te::IDs::INPUTDEVICES))
            return;

    dirty = true;
    triggerAsyncUpdate();   // e.g. a recording finished, or the mixer changed
}

void Project::handleAsyncUpdate()
{
    rebuild();
    if (onChanged)
        onChanged();
}

void Project::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();   // the device (and maybe its sample rate) changed
}

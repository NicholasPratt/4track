#include "Project.h"
#include "../Audio/Effects.h"
#include "../Audio/Mp3Encoder.h"
#include <filesystem>
#include <map>
#include <set>
#include <span>

namespace
{
    juce::File projectsRoot()
    {
       #if JUCE_UNIT_TESTS
        static const auto root = juce::File::createTempFile("8track-test-projects");
        return root;
       #else
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("8track-projects");
       #endif
    }

    const juce::Identifier songwritingID { "eightTrackSongwriting" };
    const juce::Identifier songBeatID { "songBeat" };
    const juce::Identifier songTextID { "songText" };
    const juce::Identifier lyricDraftID { "lyricDraft" };

    juce::Identifier songEntryID(Project::SongEntry kind)
    {
        return kind == Project::SongEntry::Chord ? "chordSymbol" : "lyricLine";
    }

    te::BeatPosition songBeat(const juce::ValueTree& entry)
    {
        return te::BeatPosition::fromBeats(static_cast<double>(entry[songBeatID]));
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
                if (x.id != y.id || x.takes != y.takes || x.activeTake != y.activeTake || x.midi != y.midi || x.file != y.file || x.start != y.start || x.length != y.length
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
            for (auto take : wave->state.getChildWithName(AudioEngine::takeListID))
                c.takes.add(te::SourceFileReference(*edit, take, AudioEngine::takePathID).getFile());
            c.activeTake = juce::jmax(0, c.takes.indexOf(c.file));
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
                engine.storeTakes(*made, c.takes);
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
    engine.syncNow();
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
    engine.syncNow();
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

//==============================================================================
namespace
{
    /** The given clips, grouped by track, in time order. */
    std::map<int, std::vector<Clip>> clipsByTrack(const Arrangement& a, const std::set<juce::int64>& ids)
    {
        std::map<int, std::vector<Clip>> out;
        for (int t = 0; t < kNumTracks; ++t)
            for (auto& c : a.tracks[(size_t) t])   // each lane is sorted by start
                if (ids.count(c.id) != 0)
                    out[t].push_back(c);
        return out;
    }
}

void Project::background(const juce::String& message, const std::function<void()>& work)
{
    if (runInBackground)
        runInBackground(message, work);
    else
        work();
}

bool Project::canJoin(const std::set<juce::int64>& ids) const
{
    for (auto& [track, clips] : clipsByTrack(arrangement, ids))
        if (clips.size() >= 2)
            return true;
    return false;
}

std::set<juce::int64> Project::joinClips(const std::set<juce::int64>& ids, juce::String& error)
{
    if (edit == nullptr)
        return {};

    std::map<int, std::vector<Clip>> groups;
    for (auto& [track, clips] : clipsByTrack(arrangement, ids))
        if (clips.size() >= 2)
            groups[track] = clips;
    if (groups.empty())
    {
        error = "Select two or more clips on the same track to join them.";
        return {};
    }

    // Render each track's audio clips into one new file first; nothing changes if one fails.
    struct Render { int track; std::vector<Clip> clips; juce::File dest; };
    std::vector<Render> renders;
    std::map<int, juce::int64> joinedStart;
    for (auto& [track, clips] : groups)
    {
        joinedStart[track] = clips.front().start;
        if (clips.front().midi)
            continue;
        if (!prepareFolder(error))
            return {};
        auto dest = getAudioFolder().getNonexistentChildFile(clips.front().file.getFileNameWithoutExtension() + " joined",
                                                             ".wav", false);
        dest.create();   // reserves the name for the next track's file
        renders.push_back({ track, clips, dest });
    }

    bool rendered = true;
    if (!renders.empty())
    {
        const double rate = engine.getSampleRate();
        auto& formats = engine.getTracktion().getAudioFileFormatManager();
        background(renders.size() > 1 ? "Joining clips on " + juce::String((int) renders.size()) + " tracks..." : "Joining clips...",
                   [&]
                   {
                       for (auto& r : renders)
                           if (rendered)
                               rendered = renderClips(r.clips, r.dest, rate, formats, error);
                   });
    }
    if (!rendered)
    {
        for (auto& r : renders)
            r.dest.deleteFile();
        return {};
    }

    Arrangement next = arrangement;
    for (auto& r : renders)
    {
        juce::int64 end = 0;
        for (auto& c : r.clips)
        {
            end = juce::jmax(end, c.end());
            next.removeClip(c.id);
        }
        Clip joined;
        joined.file = r.dest;
        joined.start = r.clips.front().start;
        joined.sourceLength = joined.length = end - joined.start;
        next.insertClip(r.track, joined);   // the fades and gains are in the audio now
    }

    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Join");
    writeToEdit(next);
    for (auto& [track, clips] : groups)
    {
        if (!clips.front().midi)
            continue;
        juce::Array<te::MidiClip*> midiClips;
        for (auto& c : clips)
            if (auto* m = getMidiClip(c.id))
                midiClips.add(m);
        if (midiClips.size() >= 2)
            te::mergeMidiClips(midiClips, nullptr);
    }
    um.beginNewTransaction();
    dirty = true;
    rebuild();
    if (onChanged)
        onChanged();

    std::set<juce::int64> joined;
    for (auto& [track, start] : joinedStart)
        for (auto& c : arrangement.tracks[(size_t) track])
            if (c.start == start)
                joined.insert(c.id);
    return joined;
}

bool Project::renderClips(const std::vector<Clip>& clips, const juce::File& dest, double rate,
                          te::AudioFileFormatManager& formats, juce::String& error)
{
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    int channels = 1;
    for (auto& c : clips)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.readFormatManager.createReaderFor(c.file));
        if (reader == nullptr)
        {
            error = "Couldn't read " + c.file.getFileName();
            return false;
        }
        channels = juce::jmax(channels, juce::jmin(2, (int) reader->numChannels));
        readers.push_back(std::move(reader));
    }

    dest.deleteFile();   // a FileOutputStream appends to an existing file
    std::unique_ptr<juce::OutputStream> stream = dest.createOutputStream();
    if (stream == nullptr)
    {
        error = "Couldn't write " + dest.getFullPathName();
        return false;
    }
    // 32-bit float, so clips with gain above 0 dB don't clip.
    auto writer = formats.getWavFormat()->createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                                      .withSampleRate(rate)
                                                                      .withNumChannels(channels)
                                                                      .withBitsPerSample(32)
                                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
    {
        error = "Couldn't write " + dest.getFullPathName();
        return false;
    }

    constexpr int kBlock = 8192;
    juce::AudioBuffer<float> buffer(channels, kBlock);
    auto writeSilence = [&](juce::int64 samples)
    {
        buffer.clear();
        for (juce::int64 done = 0; done < samples;)
        {
            const int n = (int) juce::jmin<juce::int64>(kBlock, samples - done);
            if (!writer->writeFromAudioSampleBuffer(buffer, 0, n))
                return false;
            done += n;
        }
        return true;
    };

    juce::int64 written = clips.front().start;   // timeline position of the next sample to write
    for (size_t i = 0; i < clips.size(); ++i)
    {
        const auto& c = clips[i];
        auto& reader = *readers[i];
        if (!writeSilence(c.start - written))
            return false;

        // The file may be at another rate than the device: read it through a resampler.
        const double ratio = reader.sampleRate > 0 ? reader.sampleRate / rate : 1.0;
        const bool resample = std::abs(ratio - 1.0) > 1.0e-9;
        const auto fileStart = (juce::int64) std::llround((double) c.sourceOffset * ratio);
        juce::AudioFormatReaderSource source(&reader, false);
        juce::ResamplingAudioSource resampler(&source, false, channels);
        if (resample)
        {
            source.setNextReadPosition(fileStart);
            resampler.setResamplingRatio(ratio);
            resampler.prepareToPlay(kBlock, rate);
        }

        for (juce::int64 done = 0; done < c.length;)
        {
            const int n = (int) juce::jmin<juce::int64>(kBlock, c.length - done);
            buffer.clear();
            if (resample)
                resampler.getNextAudioBlock(juce::AudioSourceChannelInfo(&buffer, 0, n));
            else
                reader.read(&buffer, 0, n, fileStart + done, true, channels > 1);
            if (reader.numChannels == 1 && channels > 1)
                buffer.copyFrom(1, 0, buffer, 0, 0, n);

            for (int s = 0; s < n; ++s)
            {
                const float g = c.gain * c.fadeGainAt(done + s);
                for (int ch = 0; ch < channels; ++ch)
                    buffer.getWritePointer(ch)[s] *= g;
            }
            if (!writer->writeFromAudioSampleBuffer(buffer, 0, n))
                return false;
            done += n;
        }
        written = c.end();
    }
    writer.reset();   // finishes the file
    return true;
}

float Project::peakOf(const Clip& c, juce::AudioFormatManager& formats, double sampleRate)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(c.file));
    if (reader == nullptr || reader->numChannels == 0)
        return 0.0f;
    const double ratio = reader->sampleRate / sampleRate;
    const auto start = (juce::int64) std::llround((double) c.sourceOffset * ratio);
    const auto length = (juce::int64) std::llround((double) c.length * ratio);
    std::vector<juce::Range<float>> levels(reader->numChannels);
    reader->readMaxLevels(start, length, levels.data(), (int) reader->numChannels);
    float peak = 0.0f;
    for (auto& r : levels)
        peak = juce::jmax(peak, r.getEnd(), -r.getStart());
    return peak;
}

bool Project::normaliseClips(const std::set<juce::int64>& ids)
{
    std::vector<Clip> audio;
    for (auto& [track, clips] : clipsByTrack(arrangement, ids))
        for (auto& c : clips)
            if (!c.midi)
                audio.push_back(c);
    if (audio.empty())
        return false;

    std::map<juce::int64, float> gains;
    const double rate = engine.getSampleRate();
    auto& formats = engine.getTracktion().getAudioFileFormatManager().readFormatManager;
    background(audio.size() > 1 ? "Normalising " + juce::String((int) audio.size()) + " clips..." : "Normalising...", [&]
    {
        for (auto& c : audio)
            if (const float peak = peakOf(c, formats, rate); peak > 1.0e-6f)
                gains[c.id] = juce::jmin(1.0f / peak, juce::Decibels::decibelsToGain(40.0f));
    });

    return perform([gains](Arrangement& a)
    {
        bool any = false;
        for (auto& [id, gain] : gains)
            any = a.setGain(id, gain) || any;
        return any;
    });
}

bool Project::transposeClips(const std::set<juce::int64>& ids, int semitones)
{
    if (edit == nullptr || semitones == 0)
        return false;
    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Transpose");
    bool any = false;
    for (auto id : ids)
        if (auto* clip = getMidiClip(id))
            for (auto* note : clip->getSequence().getNotes())
            {
                const int pitch = juce::jlimit(0, 127, note->getNoteNumber() + semitones);
                if (pitch != note->getNoteNumber())
                {
                    note->setNoteNumber(pitch, &um);
                    any = true;
                }
            }
    um.beginNewTransaction();
    if (any)
    {
        dirty = true;
        if (onChanged)
            onChanged();
    }
    return any;
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
    bool copiedOk = false;
    background("Importing " + file.getFileName() + "...", [&] { copiedOk = file.copyFileTo(dest); });
    if (!copiedOk)
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

void Project::chooseTake(juce::int64 id, int index)
{
    auto* clip = dynamic_cast<te::WaveAudioClip*>(te::findClipForID(*edit, te::EditItemID::fromRawID((juce::uint64) id)));
    if (clip == nullptr)
        return;
    auto take = clip->state.getChildWithName(AudioEngine::takeListID).getChild(index);
    if (!take.isValid())
        return;
    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Choose take");
    clip->state.setProperty(te::IDs::source, take[AudioEngine::takePathID], &um);
    um.beginNewTransaction();
    rebuild();
}

bool Project::canRecall(int track) const
{
    if (engine.getState() == AudioEngine::State::Recording || !engine.getTrack(track).armed.load())
        return false;
    auto id = engine.getAudioTrack(track)->itemID;
    for (auto* input : edit->getAllInputDevices())
        if (input->isRecordingActive(id) && input->hasRetrospectiveData())
            return true;
    return false;
}

bool Project::recall(int track, juce::String& error)
{
    if (!canRecall(track))
    {
        error = "No buffered performance on the selected armed track.";
        return false;
    }
    if (!prepareFolder(error))
        return false;
    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Keep what I just played");
    const auto id = engine.getAudioTrack(track)->itemID;
    juce::Array<te::InputDeviceInstance*> inputs;
    for (auto* input : edit->getAllInputDevices())
        if (input->isRecordingActive(id))
            inputs.add(input);
    int count = 0;
    {
        te::InputDevice::setRetrospectiveLock(engine.getTracktion(), inputs, true);
        const juce::ScopeGuard unlock { [&] { te::InputDevice::setRetrospectiveLock(engine.getTracktion(), inputs, false); } };
        for (auto* input : inputs)
        {
            if (auto* wave = dynamic_cast<te::WaveInputDevice*>(&input->getInputDevice()))
                wave->setFilenameMask(getAudioFolder().getChildFile("%track%_%date%_%time%").getFullPathName());
            count += input->applyRetrospectiveRecord(true, id).size();
        }
    }
    um.beginNewTransaction();
    if (count == 0)
        error = "No performance could be recovered from this input.";
    rebuild();
    return count > 0;
}

juce::Result Project::addSection(const juce::String& name, te::TimeRange range)
{
    if (name.trim().isEmpty() || range.isEmpty())
        return juce::Result::fail("Choose a range and enter a section name.");
    if (auto* sections = edit->getArrangerTrack())
        for (auto* existing : sections->getClips())
            if (existing->getPosition().time.overlaps(range))
                return juce::Result::fail("This range overlaps " + existing->getName() + ". Choose an unused range.");
    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Name song section");
    edit->ensureArrangerTrack();
    edit->getArrangerTrack()->insertNewClip(te::TrackItem::Type::arranger, name.trim(), range, nullptr);
    um.beginNewTransaction();
    return juce::Result::ok();
}

std::vector<Project::SongNote> Project::songNotes(SongEntry kind) const
{
    std::vector<SongNote> notes;
    for (auto entry : edit->state.getChildWithName(songwritingID))
        if (entry.hasType(songEntryID(kind)))
            notes.push_back({ songBeat(entry), entry[songTextID].toString(), entry });
    std::stable_sort(notes.begin(), notes.end(), [](const SongNote& a, const SongNote& b) { return a.beat < b.beat; });
    return notes;
}

juce::String Project::getLyricDraft() const
{
    return edit->state.getChildWithName(songwritingID).getProperty(lyricDraftID).toString();
}

void Project::setLyricDraft(const juce::String& draft)
{
    const auto text = draft.replace("\r\n", "\n").replaceCharacter('\r', '\n');
    if (text != getLyricDraft())
        edit->state.getOrCreateChildWithName(songwritingID, nullptr).setProperty(lyricDraftID, text, nullptr);
}

juce::String Project::timelineLyricText() const
{
    juce::StringArray lines;
    for (const auto& note : songNotes(SongEntry::Lyric))
        lines.add(note.text.replace("\r\n", " ").replaceCharacters("\r\n", "  "));
    return lines.joinIntoString("\n");
}

juce::String Project::lyricSheetText() const
{
    const auto title = getName().replaceCharacters("\r\n", "  ").trim();
    return title + "\n" + juce::String::repeatedString("=", title.length()) + "\n\n"
        + getLyricDraft().trimCharactersAtEnd("\n") + "\n";
}

void Project::setSongNote(SongEntry kind, te::BeatPosition beat, const juce::String& text, juce::ValueTree entry)
{
    if (beat < te::BeatPosition())
        return;

    auto root = edit->state.getChildWithName(songwritingID);
    if (entry.isValid())
    {
        if (!root.isValid() || entry.getParent() != root || !entry.hasType(songEntryID(kind)))
            return;
    }
    else if (kind == SongEntry::Chord)
    {
        for (auto note : root)
            if (note.hasType(songEntryID(kind)) && songBeat(note) == beat)
            {
                entry = note;
                break;
            }
    }

    const auto value = text.trim();
    if ((value.isEmpty() && !entry.isValid()) || (entry.isValid() && entry[songTextID].toString() == value))
        return;

    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction(kind == SongEntry::Chord ? "Edit chord" : "Edit lyric");
    if (value.isEmpty())
        root.removeChild(entry, &undo);
    else if (entry.isValid())
        entry.setProperty(songTextID, value, &undo);
    else
    {
        root = edit->state.getOrCreateChildWithName(songwritingID, &undo);
        entry = juce::ValueTree(songEntryID(kind));
        entry.setProperty(songBeatID, beat.inBeats(), nullptr);
        entry.setProperty(songTextID, value, nullptr);
        root.appendChild(entry, &undo);
    }
    undo.beginNewTransaction();
}

void Project::moveLyric(juce::ValueTree lyric, te::BeatPosition beat)
{
    auto root = edit->state.getChildWithName(songwritingID);
    if (!lyric.hasType(songEntryID(SongEntry::Lyric)) || !root.isValid() || lyric.getParent() != root
        || beat < te::BeatPosition() || songBeat(lyric) == beat)
        return;

    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction("Move lyric to beat");
    lyric.setProperty(songBeatID, beat.inBeats(), &undo);
    // A line moved onto an occupied beat goes after the lines already there.
    root.moveChild(root.indexOf(lyric), root.getNumChildren() - 1, &undo);
    undo.beginNewTransaction();
}

juce::ValueTree Project::adjacentLyric(juce::ValueTree lyric, int direction) const
{
    auto root = edit->state.getChildWithName(songwritingID);
    if (!lyric.hasType(songEntryID(SongEntry::Lyric)) || !root.isValid() || lyric.getParent() != root
        || (direction != -1 && direction != 1))
        return {};

    const auto beat = songBeat(lyric);
    for (int i = root.indexOf(lyric) + direction; juce::isPositiveAndBelow(i, root.getNumChildren()); i += direction)
    {
        const auto candidate = root.getChild(i);
        if (candidate.hasType(lyric.getType()) && songBeat(candidate) == beat)
            return candidate;
    }
    return {};
}

void Project::reorderLyric(juce::ValueTree lyric, int direction)
{
    const auto adjacent = adjacentLyric(lyric, direction);
    if (!adjacent.isValid())
        return;

    auto root = lyric.getParent();
    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction(direction < 0 ? "Move lyric up" : "Move lyric down");
    root.moveChild(root.indexOf(lyric), root.indexOf(adjacent), &undo);
    undo.beginNewTransaction();
}

void Project::mergeLyric(juce::ValueTree lyric, int direction)
{
    auto adjacent = adjacentLyric(lyric, direction);
    if (!adjacent.isValid())
        return;

    // Keep the upper line's position and read the two phrases in row order.
    auto upper = direction < 0 ? adjacent : lyric;
    auto lower = direction < 0 ? lyric : adjacent;
    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction("Merge lyrics");
    upper.setProperty(songTextID, upper[songTextID].toString().trimEnd() + " " + lower[songTextID].toString().trimStart(), &undo);
    upper.getParent().removeChild(lower, &undo);
    undo.beginNewTransaction();
}

juce::int64 Project::songNotesEnd() const
{
    double last = -1.0;
    for (auto entry : edit->state.getChildWithName(songwritingID))
        last = juce::jmax(last, (double) entry[songBeatID]);
    return last < 0 ? 0 : engine.toSamples(edit->tempoSequence.toTime(te::BeatPosition::fromBeats(last + 1.0)));
}

te::BeatDuration Project::chordDuration(te::BeatPosition source) const
{
    for (const auto& note : songNotes(SongEntry::Chord))
        if (note.beat > source)
            return note.beat - source;
    return te::BeatDuration::fromBeats(edit->tempoSequence.getTimeSigAt(source).numerator.get());
}

bool Project::canPlaceChord(int track, te::TimeRange range) const
{
    if (!juce::isPositiveAndBelow(track, kNumTracks) || range.isEmpty()
        || range.getStart() < te::TimePosition() || engine.getState() == AudioEngine::State::Recording)
        return false;

    auto* lane = engine.getAudioTrack(track);
    if (!engine.getTrack(track).midi.load())
        return lane->getClips().isEmpty();

    for (auto* clip : lane->getClips())
        if (clip->getPosition().time.overlaps(range))
            return false;
    return true;
}

te::MidiClip* Project::placeChord(int track, te::TimeRange range, const juce::String& symbol, bool allowConversion)
{
    if (!canPlaceChord(track, range))
        return nullptr;
    const bool convert = !engine.getTrack(track).midi.load();
    if (convert && !allowConversion)
        return nullptr;
    const auto notes = ChordPreview::notesFor(symbol);
    if (notes.isEmpty())
        return nullptr;

    auto& undo = edit->getUndoManager();
    undo.beginNewTransaction("Place " + symbol + " chord");
    auto clip = engine.getAudioTrack(track)->insertMIDIClip(symbol, range, nullptr);
    if (clip == nullptr)
        return nullptr;

    const auto duration = edit->tempoSequence.toBeats(range).getLength();
    for (int pitch : notes)
        clip->getSequence().addNote(pitch, te::BeatPosition(), duration, 90, 0, &undo);
    if (convert)
        engine.setTrackMidi(track, true);
    undo.beginNewTransaction();
    rebuild();
    return clip.get();
}

void Project::duplicateSection(te::ArrangerClip& section)
{
    const auto range = section.getPosition().time;
    const auto length = range.getLength();
    auto& um = edit->getUndoManager();
    um.beginNewTransaction("Duplicate " + section.getName());
    const te::TimeRange destination(range.getEnd(), length);
    // Snapshot the curves before inserting space, including master-bus automation.
    std::vector<std::pair<te::AutomatableParameter::Ptr, te::AutomationCurve>> automation;
    for (auto* item : te::getAllAutomatableEditItems(*edit))
        for (auto* param : item->getAutomatableParameters())
        {
            auto& curve = param->getCurve();
            if (curve.getNumPoints() > 0)
                automation.emplace_back(param, te::AutomationCurve(*edit, curve.timeBase, {}, curve.state.createCopy()));
        }

    const auto& tempo = edit->tempoSequence;
    auto beatAtBoundary = [&](te::TimePosition time)
    {
        const auto beat = tempo.toBeats(time);
        const auto wholeBeat = te::BeatPosition::fromBeats(std::round(beat.inBeats()));
        return engine.toSamples(time) == engine.toSamples(tempo.toTime(wholeBeat)) ? wholeBeat : beat;
    };
    const te::BeatRange sectionBeats(beatAtBoundary(range.getStart()), beatAtBoundary(range.getEnd()));
    const auto beatLength = sectionBeats.getLength();
    auto song = edit->state.getChildWithName(songwritingID);
    std::vector<juce::ValueTree> copies;
    for (auto entry : song)
    {
        const auto beat = songBeat(entry);
        if (beat >= sectionBeats.getEnd())
            entry.setProperty(songBeatID, (beat + beatLength).inBeats(), &um);
        else if (beat >= sectionBeats.getStart())
        {
            auto copy = entry.createCopy();
            copy.setProperty(songBeatID, (beat + beatLength).inBeats(), nullptr);
            copies.push_back(copy);
        }
    }
    for (auto& copy : copies)
        song.appendChild(copy, &um);

    te::insertSpaceIntoEdit(*edit, destination);
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto* track = engine.getAudioTrack(t);
        track->splitAt(range.getStart());
        const auto clips = track->getClips();
        for (auto* clip : clips)
            if (range.contains(clip->getPosition().time))
                if (auto copy = te::duplicateClip(*clip))
                    copy->setStart(clip->getPosition().getStart() + length, false, true);
    }
    for (auto& [param, source] : automation)
        te::mergeCurve(param->getCurve(), destination, source, range.getStart(), param->getCurrentBaseValue(),
                       te::TimeDuration(), false, false);
    edit->getArrangerTrack()->insertNewClip(te::TrackItem::Type::arranger, section.getName(),
                                           te::TimeRange(range.getEnd(), length), nullptr);
    um.beginNewTransaction();
    rebuild();
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
    struct Source { juce::ValueTree state; juce::Identifier property; juce::File file; juce::var original; };
    std::vector<Source> sources;
    auto collect = [&](juce::ValueTree state, const juce::Identifier& property)
    {
        sources.push_back({ state, property, te::SourceFileReference(*edit, state, property).getFile(), state[property] });
    };
    for (int t = 0; t < kNumTracks; ++t)
        for (auto* clip : engine.getAudioTrack(t)->getClips())
            if (auto* wave = dynamic_cast<te::WaveAudioClip*>(clip))
            {
                collect(wave->state, te::IDs::source);
                for (auto take : wave->state.getChildWithName(AudioEngine::takeListID))
                    collect(take, AudioEngine::takePathID);
            }
    for (auto& ref : EffectPlugin::findFileReferences(*edit))   // samples and impulse responses
        if (ref.state[ref.property].toString().isNotEmpty())
            collect(ref.state, ref.property);

    error.clear();
    std::map<juce::File, juce::File> copied;
    std::vector<juce::File> newFiles;
    bool saved = false;
    const juce::ScopeGuard cleanup { [&]
    {
        if (!saved)
            for (const auto& file : newFiles)
                file.deleteFile();
    } };
    background("Copying audio into " + newFolder.getFileName(), [&]
    {
        for (auto& source : sources)
        {
            const auto src = source.file;
            if (copied.contains(src))
                continue;
            if (!src.existsAsFile())
            {
                error = "Missing audio file " + src.getFullPathName();
                return;
            }
            auto dest = src;
            if (!src.isAChildOf(newFolder))
            {
                const auto preferred = audioDir.getChildFile(src.getFileName());
                std::error_code failure;
                do
                {
                    dest = preferred.getNonexistentSibling();
                    // An existing destination must never be replaced, even if another save just created it.
                    std::filesystem::copy_file(src.getFullPathName().toStdString(), dest.getFullPathName().toStdString(),
                                               std::filesystem::copy_options::none, failure);
                } while (failure == std::errc::file_exists);
                if (failure)
                {
                    error = "Couldn't copy " + src.getFileName() + " to " + dest.getFullPathName();
                    return;
                }
                newFiles.push_back(dest);
            }
            copied.emplace(src, dest);
        }
    });
    if (error.isNotEmpty())
        return false;

    auto oldRetriever = edit->editFileRetriever;
    const auto oldFolder = folder;
    const bool wasUntitled = untitled, wasDirty = dirty;
    folder = newFolder;
    untitled = false;
    edit->editFileRetriever = [file = editFile()] { return file; };
    for (auto& source : sources)
        source.state.setProperty(source.property, copied.at(source.file).getRelativePathFrom(newFolder), nullptr);
    saved = save(error);
    if (saved)
        return true;

    folder = oldFolder;
    untitled = wasUntitled;
    edit->editFileRetriever = oldRetriever;
    for (auto& source : sources)
        source.state.setProperty(source.property, source.original, nullptr);
    dirty = wasDirty;
    return false;
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
        const auto send = (float) tr->getDoubleAttribute("send", 0.0);
        if (send > 0.0f)
        {
            p.sendLevels[0] = send;
            engine.setTrackRoute(t, AudioEngine::Route::Bus1, true);
        }
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
        auto& reverb = engine.getBusReverb();
        reverb.room = (float) m->getDoubleAttribute("reverbRoom", 0.5);
        reverb.damping = (float) m->getDoubleAttribute("reverbDamping", 0.5);
        reverb.wet = (float) m->getDoubleAttribute("reverbWet", 0.33);
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
    return exportMix(file, ExportSettings{}, error);
}

bool Project::exportMix(const juce::File& file, const ExportSettings& settings, juce::String& error)
{
    error.clear();
    if (engine.getState() == AudioEngine::State::Recording)
    {
        error = "Stop recording before exporting audio.";
        return false;
    }

    const auto end = arrangement.getEnd();
    if (end <= 0)
    {
        error = "There's nothing to export yet.";
        return false;
    }

    const bool mp3 = settings.format == ExportSettings::Format::mp3;
    if (mp3 && !Mp3Encoder::isAvailable())
    {
        error = "MP3 export requires a build with LAME.";
        return false;
    }

    const auto supported = mp3 ? std::span<const int>(ExportSettings::mp3Bitrates)
                               : std::span<const int>(ExportSettings::wavBitDepths);
    if (std::ranges::find(supported, mp3 ? settings.mp3BitrateKbps : settings.wavBitDepth) == supported.end())
    {
        error = "Choose a supported bit depth or bitrate.";
        return false;
    }

    if (!file.hasFileExtension(settings.extension()) || file.isDirectory() || !file.hasWriteAccess())
    {
        error = "Choose a writable ." + juce::String(settings.extension()) + " file.";
        return false;
    }

    engine.stop();
    engine.syncNow();

    const juce::TemporaryFile output(file, juce::TemporaryFile::useHiddenFile);
    const juce::TemporaryFile intermediate(file.withFileExtension("wav"), juce::TemporaryFile::useHiddenFile);
    juce::WavAudioFormat wavFormat;
    te::Renderer::Parameters render(*edit);
    render.destFile = mp3 ? intermediate.getFile() : output.getFile();
    render.audioFormat = &wavFormat;
    render.bitDepth = mp3 ? 24 : settings.wavBitDepth;
    render.sampleRateForAudio = mp3 ? 44100.0 : engine.getSampleRate();
    render.blockSizeForAudio = edit->engine.getDeviceManager().getBlockSize();
    render.time = { te::TimePosition(), engine.toTime(end) + te::TimeDuration::fromSeconds(3.0) };
    render.useMasterPlugins = true;
    render.canRenderInMono = !mp3;

    {
        const auto allTracks = te::getAllTracks(*edit);
        render.tracksToDo = te::toBitSet(allTracks);
        te::Track::Array tracks;
        for (auto* track : allTracks)
            tracks.add(track);
        const te::Renderer::ScopedClipSlotDisabler slotDisabler(*edit, tracks);
        if (te::Renderer::renderToFile("Rendering audio...", render) == juce::File())
        {
            error = "Couldn't render the mix to " + file.getFullPathName();
            return false;
        }
    }

    bool encoded = true;
    bool saved = false;
    background(mp3 ? "Encoding MP3..." : "Saving WAV...", [&]
    {
        if (mp3)
            encoded = Mp3Encoder::encode(intermediate.getFile(), output.getFile(), settings.mp3BitrateKbps);
        if (encoded && output.getFile().getSize() > 0)
            saved = output.overwriteTargetFileWithTemporary();
    });
    if (!saved)
    {
        error = encoded ? "Couldn't save " + file.getFullPathName() : "Couldn't encode the rendered mix as MP3.";
        return false;
    }

    return true;
}

//==============================================================================
void Project::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType(songwritingID) && property == lyricDraftID)
    {
        if (!settingUp)
            markDirty();
        return;
    }
    if (tree == editState && (property == te::IDs::modifiedBy || property == te::IDs::lastSignificantChange))
        return;
    editTreeChanged(tree);
}

void Project::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree& child) { editTreeChanged(child); }

void Project::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (!child.hasType(te::IDs::INPUTDEVICES) && !child.hasType(te::IDs::TRANSPORT))
        editTreeChanged(parent);
}

void Project::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int to)
{
    // Tracktion reorders these top-level settings while rebuilding device state.
    const auto child = parent.getChild(to);
    if (parent == editState && (child.hasType(te::IDs::SCENES) || child.hasType("AUXBUSNAMES")))
        return;
    editTreeChanged(parent);
}

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

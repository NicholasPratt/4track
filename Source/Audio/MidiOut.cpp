#include "MidiOut.h"

MidiOutSender::MidiOutSender()
{
    pending.reserve(kQueueSize);
    startTimer(1);
}

MidiOutSender::~MidiOutSender()
{
    stopTimer();
    for (int p = 0; p < kMaxPorts; ++p)
        allNotesOff(p);
}

int MidiOutSender::openPort(const juce::String& identifier)
{
    if (identifier.isEmpty())
        return -1;
    const std::scoped_lock lock(portLock);
    for (int p = 0; p < kMaxPorts; ++p)
        if (portIds[(size_t) p] == identifier && ports[(size_t) p] != nullptr)
            return p;
    for (int p = 0; p < kMaxPorts; ++p)
        if (ports[(size_t) p] == nullptr)
        {
            ports[(size_t) p] = juce::MidiOutput::openDevice(identifier);
            if (ports[(size_t) p] == nullptr)
                return -1;
            portIds[(size_t) p] = identifier;
            return p;
        }
    return -1;
}

void MidiOutSender::setPortDelayMs(int port, double ms)
{
    if (port >= 0 && port < kMaxPorts)
        portDelayMs[(size_t) port] = ms;
}

void MidiOutSender::send(int port, const juce::MidiMessage& m, double offsetSeconds)
{
    if (port < 0 || port >= kMaxPorts || m.getRawDataSize() > 3 || m.getRawDataSize() == 0)
        return;   // sysex isn't forwarded

    Event e;
    e.timeMs = juce::Time::getMillisecondCounterHiRes() + latencyMs.load() + portDelayMs[(size_t) port].load()
             + offsetSeconds * 1000.0;
    e.generation = generation[(size_t) port].load();
    e.port = port;
    e.size = m.getRawDataSize();
    std::memcpy(e.data, m.getRawData(), (size_t) e.size);

    if (!queue.try_push(e))
        queueAllNotesOff(port);
}

void MidiOutSender::allNotesOff(int port)
{
    if (port < 0 || port >= kMaxPorts)
        return;
    const std::scoped_lock lock(portLock);
    generation[(size_t) port].fetch_add(1);
    resetPortLocked(port);
}

void MidiOutSender::resetPortLocked(int port)
{
    appliedGeneration[(size_t) port] = generation[(size_t) port].load();
    if (auto& out = ports[(size_t) port])
        for (int ch = 1; ch <= 16; ++ch)
        {
            out->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 64, 0));
            out->sendMessageNow(juce::MidiMessage::allNotesOff(ch));
        }
}

void MidiOutSender::queueAllNotesOff(int port)
{
    if (port >= 0 && port < kMaxPorts)
        generation[(size_t) port].fetch_add(1);
}

void MidiOutSender::hiResTimerCallback()
{
    Event event;
    for (int i = 0; i < kQueueSize && queue.try_pop(event); ++i)
        pending.push_back(event);

    const std::scoped_lock lock(portLock);
    for (int port = 0; port < kMaxPorts; ++port)
        if (appliedGeneration[(size_t) port] != generation[(size_t) port].load())
            resetPortLocked(port);

    std::erase_if(pending, [&](const Event& e)
    {
        return e.generation != appliedGeneration[(size_t) e.port];
    });
    if (pending.empty())
        return;

    // Send what's due, in time order; keep the rest for later.
    std::stable_sort(pending.begin(), pending.end(), [](const Event& a, const Event& b) { return a.timeMs < b.timeMs; });
    const double now = juce::Time::getMillisecondCounterHiRes();
    size_t sent = 0;
    for (; sent < pending.size() && pending[sent].timeMs <= now; ++sent)
        if (auto& out = ports[(size_t) pending[sent].port])
            out->sendMessageNow(juce::MidiMessage(pending[sent].data, pending[sent].size));
    pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t) sent);
}

//==============================================================================
const char* MidiOutPlugin::xmlTypeName = "eightTrackMidiOut";

MidiOutPlugin::MidiOutPlugin(te::PluginCreationInfo info) : te::Plugin(info)
{
    deviceName.referTo(state, "midiOutDevice", nullptr);
    replaceInstrument.referTo(state, "replaceInstrument", nullptr, false);
}

MidiOutPlugin::~MidiOutPlugin()
{
    notifyListenersOfDeletion();
}

juce::ValueTree MidiOutPlugin::create()
{
    juce::ValueTree v(te::IDs::PLUGIN);
    v.setProperty(te::IDs::type, xmlTypeName, nullptr);
    return v;
}

void MidiOutPlugin::connect(MidiOutSender& s, int newPort)
{
    const int old = port.exchange(newPort);
    sender = &s;
    replace = replaceInstrument.get();
    if (old != newPort && old >= 0)
        s.allNotesOff(old);
}

void MidiOutPlugin::initialise(const te::PluginInitialisationInfo&) { wasPlaying = false; }

void MidiOutPlugin::deinitialise()
{
    if (auto* s = sender.load())
        s->queueAllNotesOff(port.load());
}

void MidiOutPlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    auto* s = sender.load();
    const int p = port.load();
    if (fc.bufferForMidiMessages == nullptr || s == nullptr || p < 0)
        return;

    auto& midi = *fc.bufferForMidiMessages;
    if (midi.isAllNotesOff || (wasPlaying && !fc.isPlaying))
        s->queueAllNotesOff(p);
    wasPlaying = fc.isPlaying;

    double mutedSeconds = 0;
    const auto& transport = edit.getTransport();
    const auto countInEnd = transport.getPlaybackCountInEndSeconds();
    if (std::isfinite(countInEnd))
        if (auto* context = transport.getCurrentPlaybackContext(); context != nullptr
            && context->getUnloopedPosition().inSeconds() < countInEnd)
            mutedSeconds = countInEnd - fc.editTime.getStart().inSeconds();

    for (auto& m : midi)
        if (mutedSeconds <= 0 || m.getTimeStamp() >= mutedSeconds)
            s->send(p, m, m.getTimeStamp());

    if (replace.load())
        midi.clear();
}

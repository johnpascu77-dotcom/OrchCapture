#include "OrchCaptureLink.h"
#include "OrchCaptureProcessor.h"

#include <algorithm>

namespace
{
    constexpr int kPollMs = 3000;          // worker loop interval
    constexpr int kConnectTimeoutMs = 300; // blocking connect - worker thread only, never the message thread

    void sendJson (juce::InterprocessConnection& connection, const juce::var& value)
    {
        const auto json = juce::JSON::toString (value, true);
        connection.sendMessage (juce::MemoryBlock (json.toRawUTF8(), json.getNumBytesAsUTF8()));
    }
}

// ===================== connection / server objects =====================
//
// callbacksOnMessageThread == false: every connection callback arrives on the
// connection's own background thread, so message parsing (a take can be a few
// thousand notes of JSON) never touches the host message thread. Shared state
// is mutex-guarded for that reason.

class OrchCaptureLink::ClientConnection : public juce::InterprocessConnection
{
public:
    explicit ClientConnection (OrchCaptureLink& o) : juce::InterprocessConnection (false), owner (o) {}
    ~ClientConnection() override { disconnect(); }

    void connectionMade() override { owner.clientConnected.store (true); }
    void connectionLost() override { owner.clientConnected.store (false); }
    void messageReceived (const juce::MemoryBlock&) override {} // coordinator never replies to clients

private:
    OrchCaptureLink& owner;
};

class OrchCaptureLink::CoordinatorConnection : public juce::InterprocessConnection
{
public:
    explicit CoordinatorConnection (OrchCaptureLink& ownerIn)
        : juce::InterprocessConnection (false), owner (ownerIn) {}

    ~CoordinatorConnection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override { owner.onClientGone (this); }

    void messageReceived (const juce::MemoryBlock& message) override
    {
        const auto json = juce::String::fromUTF8 (static_cast<const char*> (message.getData()),
                                                  static_cast<int> (message.getSize()));
        juce::var parsed;
        if (juce::JSON::parse (json, parsed).wasOk() && parsed.isObject())
            owner.onClientMessage (this, parsed);
    }

private:
    OrchCaptureLink& owner;
};

class OrchCaptureLink::CoordinatorServer : public juce::InterprocessConnectionServer
{
public:
    explicit CoordinatorServer (OrchCaptureLink& ownerIn) : owner (ownerIn) {}
    ~CoordinatorServer() override { stop(); }

    juce::InterprocessConnection* createConnectionObject() override
    {
        auto connection = std::make_unique<CoordinatorConnection> (owner);
        auto* raw = connection.get();
        owner.registerServerConnection (std::move (connection));
        return raw;
    }

private:
    OrchCaptureLink& owner;
};

// ============================== link ==================================

OrchCaptureLink::OrchCaptureLink (OrchCaptureAudioProcessor& p)
    : juce::Thread ("OrchCapture link"), processor (p)
{
    localUid = juce::Uuid().toString();
    startThread (juce::Thread::Priority::background);
}

OrchCaptureLink::~OrchCaptureLink()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);

    // run() already tore these down on its way out; harmless to repeat.
    client.reset();
    teardownServer();
}

juce::File OrchCaptureLink::lockFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("orchcapture-coordinator.lock");
}

void OrchCaptureLink::teardownServer()
{
    if (server != nullptr)
    {
        server->stop();
        server.reset();
    }

    {
        std::lock_guard<std::mutex> lock (lanesMutex);
        serverConnections.clear();
        lanes.clear();
    }

    if (wroteLockFile)
    {
        lockFile().deleteFile();
        wroteLockFile = false;
    }
}

// ---- worker thread ----

void OrchCaptureLink::run()
{
    while (! threadShouldExit())
    {
        reconcileMode();

        if (mode.load() != Mode::Coordinator)
            serviceClient();

        wait (kPollMs);
    }

    client.reset();
    teardownServer();
}

void OrchCaptureLink::reconcileMode()
{
    const bool wantCoordinator = processor.isCoordinatorParamOn();

    if (wantCoordinator)
    {
        if (client != nullptr)
        {
            client.reset();
            clientConnected.store (false);
            clientWasConnected = false;
        }

        if (server == nullptr)
        {
            auto candidate = std::make_unique<CoordinatorServer> (*this);
            if (candidate->beginWaitingForSocket (kPort))
            {
                server = std::move (candidate);
                mode.store (Mode::Coordinator);
            }
            else
            {
                mode.store (Mode::CoordinatorPortBusy);
            }
        }
        else
        {
            mode.store (Mode::Coordinator);
        }

        if (server != nullptr)
        {
            if (! lockFile().existsAsFile())
                lockFile().replaceWithText (juce::String (kPort));
            wroteLockFile = true;
        }
    }
    else
    {
        if (server != nullptr || wroteLockFile)
            teardownServer();

        mode.store (Mode::Client);

        if (client == nullptr)
        {
            client = std::make_unique<ClientConnection> (*this);
            clientWasConnected = false;
            clientLastPushedGeneration = -1;
        }
    }
}

void OrchCaptureLink::serviceClient()
{
    if (client == nullptr)
        return;

    if (! client->isConnected())
    {
        clientConnected.store (false);
        clientWasConnected = false;

        // The gate: only reach for the socket when a coordinator has announced
        // itself. No coordinator on the rig == one cheap file check per poll.
        if (lockFile().existsAsFile())
            client->connectToSocket ("127.0.0.1", kPort, kConnectTimeoutMs);

        return;
    }

    const bool playing = processor.isTransportPlayingForUi();
    const int generation = processor.getTakeGenerationForUi();

    const bool justConnected = ! clientWasConnected;
    const bool takeJustCompleted = clientWasPlaying && ! playing;
    const bool generationChanged = generation != clientLastPushedGeneration;

    if (justConnected || takeJustCompleted || generationChanged)
    {
        pushLaneFromClient (true);
        clientLastPushedGeneration = generation;
    }
    else
    {
        pushLaneFromClient (false);
    }

    clientWasConnected = true;
    clientWasPlaying = playing;
}

void OrchCaptureLink::pushLaneFromClient (bool includeNotes)
{
    if (client == nullptr || ! client->isConnected())
        return;

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("t", includeNotes ? "lane" : "status");
    obj->setProperty ("uid", localUid);
    obj->setProperty ("name", processor.getTrackNameForUi());
    obj->setProperty ("role", processor.getTapRoleForUi());
    obj->setProperty ("playing", processor.isTransportPlayingForUi());
    obj->setProperty ("nc", processor.getTakeNoteCountForUi());
    obj->setProperty ("kc", processor.getTakeKeyswitchCountForUi());
    obj->setProperty ("len", processor.getTakeLengthQuarterNotesForUi());

    if (includeNotes)
    {
        const auto options = processor.buildExportOptions();
        obj->setProperty ("tempo", options.tempoBpm);
        obj->setProperty ("ksmode", static_cast<int> (options.keyswitchMode));
        obj->setProperty ("qgrid", options.quantizeGridPpq);
        obj->setProperty ("notes", ocap::takeToVar (processor.snapshotTake()));
    }

    sendJson (*client, juce::var (obj));
}

// ---- coordinator side (connection threads) ----

void OrchCaptureLink::registerServerConnection (std::unique_ptr<CoordinatorConnection> connection)
{
    std::lock_guard<std::mutex> lock (lanesMutex);
    serverConnections.push_back (std::move (connection));
}

void OrchCaptureLink::onClientMessage (CoordinatorConnection* connection, const juce::var& message)
{
    const auto type = message["t"].toString();
    const auto uid = message["uid"].toString();
    if (uid.isEmpty())
        return;

    std::lock_guard<std::mutex> lock (lanesMutex);

    auto& lane = lanes[uid];
    if (lane.uid.isEmpty())
    {
        lane.uid = uid;
        lane.firstSeenOrder = ++laneOrderCounter;
    }
    lane.conn = connection;
    lane.trackName = message.getProperty ("name", lane.trackName).toString();
    lane.role = static_cast<int> (message.getProperty ("role", lane.role));
    lane.playing = static_cast<bool> (message.getProperty ("playing", lane.playing));
    lane.noteCount = static_cast<int> (message.getProperty ("nc", lane.noteCount));
    lane.keyswitchCount = static_cast<int> (message.getProperty ("kc", lane.keyswitchCount));
    lane.lengthPpq = static_cast<double> (message.getProperty ("len", lane.lengthPpq));

    if (type == "lane")
    {
        lane.tempoBpm = static_cast<double> (message.getProperty ("tempo", lane.tempoBpm));
        lane.ksMode = static_cast<int> (message.getProperty ("ksmode", lane.ksMode));
        lane.quantizeGridPpq = static_cast<double> (message.getProperty ("qgrid", lane.quantizeGridPpq));
        lane.notes = ocap::takeFromVar (message["notes"]);
        lane.haveNotes = true;
    }
}

void OrchCaptureLink::onClientGone (CoordinatorConnection* connection)
{
    juce::WeakReference<OrchCaptureLink> weak (this);
    juce::MessageManager::callAsync ([weak, connection]
    {
        if (auto* self = weak.get())
        {
            std::lock_guard<std::mutex> lock (self->lanesMutex);

            for (auto it = self->lanes.begin(); it != self->lanes.end();)
                it = (it->second.conn == connection) ? self->lanes.erase (it) : std::next (it);

            self->serverConnections.erase (
                std::remove_if (self->serverConnections.begin(), self->serverConnections.end(),
                                [connection] (const std::unique_ptr<CoordinatorConnection>& entry)
                                { return entry.get() == connection; }),
                self->serverConnections.end());
        }
    });
}

// ---- editor queries (message thread) ----

double OrchCaptureLink::getSessionTempoBpm() const
{
    return juce::jmax (1.0, processor.getCurrentTempoBpm());
}

std::vector<OrchCaptureLink::LaneRow> OrchCaptureLink::getLaneRows() const
{
    std::vector<LaneRow> rows;

    LaneRow local;
    local.uid = localUid;
    local.trackName = processor.getTrackNameForUi();
    local.role = processor.getTapRoleForUi();
    local.noteCount = processor.getTakeNoteCountForUi();
    local.keyswitchCount = processor.getTakeKeyswitchCountForUi();
    local.lengthPpq = processor.getTakeLengthQuarterNotesForUi();
    local.playing = processor.isTransportPlayingForUi();
    local.isLocal = true;
    local.haveTake = local.noteCount > 0;
    rows.push_back (local);

    if (mode.load() == Mode::Coordinator)
    {
        std::lock_guard<std::mutex> lock (lanesMutex);

        std::vector<const Lane*> sorted;
        sorted.reserve (lanes.size());
        for (const auto& entry : lanes)
            sorted.push_back (&entry.second);
        std::sort (sorted.begin(), sorted.end(),
                   [] (const Lane* a, const Lane* b) { return a->firstSeenOrder < b->firstSeenOrder; });

        for (const auto* lane : sorted)
        {
            LaneRow row;
            row.uid = lane->uid;
            row.trackName = lane->trackName;
            row.role = lane->role;
            row.noteCount = lane->noteCount;
            row.keyswitchCount = lane->keyswitchCount;
            row.lengthPpq = lane->lengthPpq;
            row.playing = lane->playing;
            row.isLocal = false;
            row.haveTake = lane->haveNotes && ! lane->notes.empty();
            rows.push_back (row);
        }
    }

    return rows;
}

std::vector<ocap::TakeForExport> OrchCaptureLink::collectTakesForExport (
    const std::set<juce::String>& excludedUids) const
{
    std::vector<ocap::TakeForExport> out;

    if (mode.load() != Mode::Coordinator)
        return out;

    if (excludedUids.count (localUid) == 0)
    {
        auto localNotes = processor.snapshotTake();
        if (! localNotes.empty())
            out.push_back ({ processor.buildExportOptions(), std::move (localNotes) });
    }

    std::lock_guard<std::mutex> lock (lanesMutex);

    std::vector<const Lane*> sorted;
    sorted.reserve (lanes.size());
    for (const auto& entry : lanes)
        sorted.push_back (&entry.second);
    std::sort (sorted.begin(), sorted.end(),
               [] (const Lane* a, const Lane* b) { return a->firstSeenOrder < b->firstSeenOrder; });

    for (const auto* lane : sorted)
    {
        if (! lane->haveNotes || lane->notes.empty())
            continue;
        if (excludedUids.count (lane->uid) != 0)
            continue;

        ocap::TakeExportOptions options;
        options.trackName = lane->trackName.isNotEmpty() ? lane->trackName : juce::String ("Lane");
        options.tempoBpm = lane->tempoBpm;
        options.ticksPerQuarterNote = 960;
        options.keyswitchMode = static_cast<ocap::KeyswitchExportMode> (juce::jlimit (0, 3, lane->ksMode));
        options.tapRole = lane->role;
        options.quantizeGridPpq = lane->quantizeGridPpq;

        out.push_back ({ options, lane->notes });
    }

    return out;
}

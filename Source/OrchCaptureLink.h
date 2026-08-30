#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <JuceHeader.h>

#include "OrchCaptureTakeLogic.h"

class OrchCaptureAudioProcessor;

// Phase 2 coordinator link. Every OrchCapture instance owns one.
//
// ALL socket work runs on this object's own background thread - NEVER the host
// message thread - because an orchestral rig has ~50-100 instances and a
// blocking connect on each, on the one shared message thread, freezes the host
// (learned the hard way 2026-08-30: OrchCaptureLink was a juce::Timer and it
// froze Bitwig).
//
// Discovery is via a lock file: the coordinator instance writes
// <temp>/orchcapture-coordinator.lock while it holds port 47826. A client only
// attempts a socket connection when that file exists, so the common "no
// coordinator in the rig" state costs one File::existsAsFile() per poll and
// nothing else.
//
// - `coordinator` parameter OFF -> client: when the lock file exists, keep a
//   connection to 127.0.0.1:47826 and push this instance's lane (track name,
//   role, and - on every completed take - the full note list) to the
//   coordinator.
// - `coordinator` parameter ON -> server: bind port 47826, write the lock file,
//   collect every client's lane. "Drag ALL / Save ALL" merges the coordinator's
//   own take plus every collected lane into one multi-track SMF. If the port is
//   already held it reports that and keeps retrying so it can take over.
class OrchCaptureLink : private juce::Thread
{
public:
    explicit OrchCaptureLink (OrchCaptureAudioProcessor&);
    ~OrchCaptureLink() override;

    static constexpr int kPort = 47826;

    // ---- thread-safe queries for the editor (message thread) ----

    enum class Mode { Client, Coordinator, CoordinatorPortBusy };
    Mode getMode() const { return mode.load(); }
    bool isClientConnected() const { return clientConnected.load(); }

    struct LaneRow
    {
        juce::String trackName;
        int role = 0;          // 0 Performance, 1 Articulation
        int noteCount = 0;
        int keyswitchCount = 0;
        double lengthPpq = 0.0;
        bool playing = false;
        bool isLocal = false;  // the coordinator instance's own lane
        bool haveTake = false; // a completed take has been received
    };

    std::vector<LaneRow> getLaneRows() const;
    std::vector<ocap::TakeForExport> collectTakesForExport() const;
    double getSessionTempoBpm() const;

private:
    class ClientConnection;
    class CoordinatorConnection;
    class CoordinatorServer;

    void run() override;
    void reconcileMode();
    void serviceClient();
    void pushLaneFromClient (bool includeNotes);
    void teardownServer();
    static juce::File lockFile();

    void registerServerConnection (std::unique_ptr<CoordinatorConnection>);
    void onClientMessage (CoordinatorConnection*, const juce::var&);
    void onClientGone (CoordinatorConnection*);
    friend class ClientConnection;
    friend class CoordinatorConnection;
    friend class CoordinatorServer;

    struct Lane
    {
        juce::String uid, trackName;
        int role = 0, ksMode = 0;
        double tempoBpm = 120.0;
        bool playing = false;
        int noteCount = 0, keyswitchCount = 0;
        double lengthPpq = 0.0;
        std::vector<ocap::CapturedNote> notes;
        bool haveNotes = false;
        int firstSeenOrder = 0;
        CoordinatorConnection* conn = nullptr;
    };

    OrchCaptureAudioProcessor& processor;

    std::atomic<Mode> mode { Mode::Client };
    std::atomic<bool> clientConnected { false };

    // owned + touched only by the worker thread (run())
    std::unique_ptr<CoordinatorServer> server;
    std::unique_ptr<ClientConnection> client;
    bool wroteLockFile = false;
    bool clientWasConnected = false;
    bool clientWasPlaying = false;
    int clientLastPushedGeneration = -1;

    mutable std::mutex lanesMutex;
    std::vector<std::unique_ptr<CoordinatorConnection>> serverConnections;
    std::map<juce::String, Lane> lanes; // key = uid
    int laneOrderCounter = 0;
    juce::String localUid;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchCaptureLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchCaptureLink)
};

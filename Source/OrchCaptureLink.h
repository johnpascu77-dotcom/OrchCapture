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
// - When this instance's `coordinator` parameter is OFF: runs as a client,
//   reconnecting to 127.0.0.1:47826 on a timer and pushing its lane (track
//   name, role, and - on every completed take - the full note list) to
//   whichever instance is the coordinator.
// - When `coordinator` is ON: binds port 47826 as the server, collecting every
//   client's lane. "Export All" then merges the coordinator's own take plus
//   every collected lane into one multi-track SMF (matched Performance / "<name>
//   KS" pairs). If the port is already taken (another instance is the
//   coordinator) it falls back to running as a client and reports that.
//
// Same local-socket pattern as Composer Mastermind's PatternSyncServer /
// McpBridgeServer and the TransportCompanion - proven to work across sandboxed
// plugin instances in Bitwig.
class OrchCaptureLink : private juce::Timer
{
public:
    explicit OrchCaptureLink (OrchCaptureAudioProcessor&);
    ~OrchCaptureLink() override;

    static constexpr int kPort = 47826;

    // ---- message-thread queries for the editor ----

    enum class Mode { Client, Coordinator, CoordinatorPortBusy };
    Mode getMode() const { return mode.load(); }
    bool isClientConnected() const;

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

    // Coordinator: its own lane first, then every connected client (stable
    // first-seen order). Client / port-busy: just this instance's own lane.
    std::vector<LaneRow> getLaneRows() const;

    // Coordinator only: its own take plus every client lane that has one,
    // ready for ocap::writeMergedTakeMidi. Empty otherwise.
    std::vector<ocap::TakeForExport> collectTakesForExport() const;

    double getSessionTempoBpm() const;

private:
    class ClientConnection;
    class CoordinatorConnection;
    class CoordinatorServer;

    void timerCallback() override;
    void reconcileMode();
    void pushLaneFromClient (bool includeNotes);

    void registerServerConnection (std::unique_ptr<CoordinatorConnection>);
    void onClientMessage (CoordinatorConnection*, const juce::var&);
    void onClientGone (CoordinatorConnection*);
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

    std::unique_ptr<CoordinatorServer> server;
    std::unique_ptr<ClientConnection> client;

    mutable std::mutex lanesMutex;
    std::vector<std::unique_ptr<CoordinatorConnection>> serverConnections;
    std::map<juce::String, Lane> lanes; // key = uid
    int laneOrderCounter = 0;
    juce::String localUid;

    // client-side take-change tracking
    bool clientWasConnected = false;
    bool clientWasPlaying = false;
    int clientLastPushedGeneration = -1;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchCaptureLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchCaptureLink)
};

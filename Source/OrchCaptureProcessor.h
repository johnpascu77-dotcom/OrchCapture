#pragma once

#include <atomic>
#include <vector>
#include <JuceHeader.h>

#include "OrchCaptureTakeLogic.h"

// OrchCapture - Phase 1 MVP.
//
// A transparent MIDI-effect plugin that sits LAST on an instrument track (after
// OrchNoteMapper, before the instrument) and records the exact post-full-chain
// note stream that track plays, plus the track's own name. One "most recent
// take" per instance, exported as a Dorico-ready Standard MIDI File - per
// instance in Phase 1, merged by a coordinator in Phase 2.
class OrchCaptureAudioProcessor final : public juce::AudioProcessor
{
public:
    OrchCaptureAudioProcessor();
    ~OrchCaptureAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void updateTrackProperties (const TrackProperties& properties) override;

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ---- Message-thread API for the editor -------------------------------

    // Track name as reported by the host (VST3 channel context), or a
    // sensible fallback if the host has not sent one.
    juce::String getTrackNameForUi() const;

    // A copy of the current take, safe to hold and export.
    std::vector<ocap::CapturedNote> snapshotTake() const;

    int getTakeNoteCountForUi() const { return takeNoteCountUi.load(); }
    double getTakeLengthQuarterNotesForUi() const { return takeLengthPpqUi.load(); }
    bool isTransportPlayingForUi() const { return transportPlayingUi.load(); }
    bool hasHostTrackNameForUi() const { return haveHostTrackName.load(); }
    int getLastCapturedNoteForUi() const { return lastCapturedNoteUi.load(); }

    double getCurrentTempoBpm() const { return currentBpmUi.load(); }

    // Discard the take and start listening fresh. Safe to call while stopped or
    // playing (the next captured note begins a new buffer either way).
    void clearTake();

    ocap::TakeExportOptions buildExportOptions() const;

private:
    struct OpenNote
    {
        int channel = 1;
        int note = 60;
        int velocity = 100;
        double ppqOn = 0.0;
    };

    void resetTake (double takeOriginPpq);
    void finalizeOpenNotes (double ppqOff);

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* enableParam = nullptr;
    std::atomic<float>* resetOnPlayParam = nullptr;

    double sampleRate = 44100.0;

    // Capture buffer + open-note list. Guarded by captureLock: processBlock
    // appends briefly, the editor snapshots. Contention is near-zero (orchestral
    // mono parts, and export happens with the transport stopped).
    juce::SpinLock captureLock;
    std::vector<ocap::CapturedNote> capturedNotes;
    std::vector<OpenNote> openNotes;
    double takeStartPpq = 0.0;

    // Set by clearTake() on the message thread; consumed at the top of the
    // next processBlock so the reset lands with a known playhead position.
    std::atomic<bool> pendingClear { false };

    // Transport-follow state (audio thread only).
    bool wasPlaying = false;
    double lastBlockEndPpq = 0.0;
    bool haveLastBlockEnd = false;

    // Host track name (message thread writes, editor + audio-thread fallback read).
    juce::CriticalSection trackNameLock;
    juce::String hostTrackName;
    std::atomic<bool> haveHostTrackName { false };

    // UI status mirrors.
    std::atomic<int> takeNoteCountUi { 0 };
    std::atomic<double> takeLengthPpqUi { 0.0 };
    std::atomic<bool> transportPlayingUi { false };
    std::atomic<int> lastCapturedNoteUi { -1 };
    std::atomic<double> currentBpmUi { 120.0 };

    void refreshTakeStatus();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchCaptureAudioProcessor)
};

#pragma once

#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// Pure take -> Standard MIDI File logic for OrchCapture. No plugin host, no
// GUI, no real-time note tracking - the processor owns all of that and hands a
// finished list of notes to this file. This is the part the take-logic check
// tool exercises directly.
namespace ocap
{
    // One finished note as captured off an instrument track's post-chain MIDI
    // stream. Times are in quarter notes (ppq), already relative to the take's
    // own start (the processor subtracts takeStartPpq before storing).
    struct CapturedNote
    {
        double ppqOn = 0.0;
        double ppqOff = 0.0;
        int note = 60;       // 0..127
        int velocity = 100;  // 1..127
        int channel = 1;     // 1..16
        bool isKeyswitch = false; // note fell inside the tap's KS zone at capture time
    };

    // How keyswitch notes are laid out in the exported SMF.
    enum class KeyswitchExportMode
    {
        Inline = 0,     // everything on one note track (default - don't pretend to classify)
        SeparateTrack,  // musical notes on track 1, keyswitches on track 2 "<name> KS"
        Exclude,        // musical notes only
        KeyswitchOnly   // keyswitches only (track named "<name> KS")
    };

    struct TakeExportOptions
    {
        juce::String trackName { "OrchCapture" };
        double tempoBpm = 120.0;
        int ticksPerQuarterNote = 960;
        KeyswitchExportMode keyswitchMode = KeyswitchExportMode::Inline;
        int tapRole = 0;             // 0 = Performance, 1 = Articulation - merged-export ordering only
        double quantizeGridPpq = 0.0; // 0 = as performed; else snap onset+release to this grid
    };

    // One take plus how it wants to be laid out - the unit the coordinator
    // collects from each connected lane for a merged export.
    struct TakeForExport
    {
        TakeExportOptions options;
        std::vector<CapturedNote> notes;
    };

    // A single named note track, ready to become one MidiMessageSequence.
    struct NoteTrack
    {
        juce::String name;
        std::vector<CapturedNote> notes;
    };

    // Which tracks the merged rig export keeps.
    enum class MergedContent { NotesAndKeyswitches = 0, NotesOnly, KeyswitchesOnly };

    // A rehearsal-mark / section label at a bar position, for the merged file's
    // tempo track.
    struct SectionMarker
    {
        double bar = 0.0;
        juce::String label;
    };

    struct MergedExportOptions
    {
        juce::String sessionName { "OrchCapture session" };
        double tempoBpm = 120.0;
        int ticksPerQuarterNote = 960;
        double barLengthPpq = 4.0; // 4/4
        MergedContent content = MergedContent::NotesAndKeyswitches;
        std::vector<SectionMarker> markers;
        juce::StringArray scoreOrder; // instrument track names, in the order they should appear
    };

    // Smallest note length we will write, in quarter notes - guards against a
    // zero- or negative-length note from a note-off that arrived in the same
    // sample as its note-on.
    constexpr double kMinNoteLengthPpq = 1.0 / 128.0;

    // Name of the separate keyswitch track for a given instrument track name.
    juce::String keyswitchTrackName (const juce::String& trackName);

    // Drop out-of-range notes, clamp onsets to >= 0, force ppqOff to sit at
    // least kMinNoteLengthPpq past ppqOn, and sort by onset (then pitch). Pure;
    // returns a new vector. Preserves the isKeyswitch flag.
    std::vector<CapturedNote> normalizeTake (std::vector<CapturedNote> notes);

    // Onset ppq of the earliest note to release ppq of the last note to stop.
    // 0 for an empty take.
    double takeLengthPpq (const std::vector<CapturedNote>& notes);

    // Snap every onset and release to the nearest multiple of gridPpq, keeping
    // a minimum length of one grid unit. gridPpq <= 0 returns the take
    // untouched. Pure.
    std::vector<CapturedNote> quantizeTake (std::vector<CapturedNote> notes, double gridPpq);

    // Parse "bar:label" tokens (one per line, or comma-separated) into markers.
    // Malformed tokens are skipped. e.g. "0:Intro, 16:A, 32:B".
    std::vector<SectionMarker> parseSectionMarkers (const juce::String& text);

    // Parse a comma / newline separated list of instrument track names, trimmed,
    // empties dropped.
    juce::StringArray parseScoreOrder (const juce::String& text);

    // The note-track layout for one take, per options.keyswitchMode, without
    // writing anything. Input is normalised internally.
    std::vector<NoteTrack> planNoteTracks (const std::vector<CapturedNote>& notes,
                                           const TakeExportOptions& options);

    // Write a Format-1 SMF to `out`:
    //   track 0      - name + tempo meta
    //   track 1 (..2) - one or two note tracks, per options.keyswitchMode
    // Input is normalised internally, so the caller may pass a raw take.
    void writeTakeMidi (const std::vector<CapturedNote>& notes,
                        const TakeExportOptions& options,
                        juce::OutputStream& out);

    // Write one merged Format-1 SMF for the whole rig: track 0 = tempo/name +
    // any section markers, then every take's note tracks. Tracks are ordered by
    // options.scoreOrder (names not listed fall after, in first-seen order);
    // within an instrument, Performance (tapRole 0) precedes Articulation
    // (tapRole 1). options.content filters note vs "<name> KS" tracks. Empty
    // takes contribute nothing.
    void writeMergedTakeMidi (const std::vector<TakeForExport>& takes,
                              const MergedExportOptions& options,
                              juce::OutputStream& out);

    // Compact JSON round-trip for a take, for the coordinator socket link.
    juce::var takeToVar (const std::vector<CapturedNote>& notes);
    std::vector<CapturedNote> takeFromVar (const juce::var& value);
}

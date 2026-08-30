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

    // Write a Format-1 SMF to `out`:
    //   track 0      - name + tempo meta
    //   track 1 (..2) - one or two note tracks, per options.keyswitchMode
    // Input is normalised internally, so the caller may pass a raw take.
    void writeTakeMidi (const std::vector<CapturedNote>& notes,
                        const TakeExportOptions& options,
                        juce::OutputStream& out);
}

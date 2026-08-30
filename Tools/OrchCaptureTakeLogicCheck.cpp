#include "OrchCaptureTakeLogic.h"

#include <iostream>
#include <string>

namespace
{
    int failures = 0;

    void check (bool condition, const std::string& label)
    {
        if (condition)
        {
            std::cout << "[PASS] " << label << "\n";
        }
        else
        {
            std::cerr << "[FAIL] " << label << "\n";
            ++failures;
        }
    }

    ocap::CapturedNote makeNote (double on, double off, int note, int vel = 100, int ch = 1)
    {
        ocap::CapturedNote n;
        n.ppqOn = on;
        n.ppqOff = off;
        n.note = note;
        n.velocity = vel;
        n.channel = ch;
        return n;
    }

    // Count note-ons in track `trackIndex` of an SMF held in `data`, and report
    // the tick of the first note-on and the track's first text-meta string.
    struct TrackScan
    {
        int noteOns = 0;
        int noteOffs = 0;
        double firstNoteOnTick = -1.0;
        juce::String firstText;
        int numTracks = 0;
    };

    TrackScan scan (const juce::MemoryBlock& data, int trackIndex)
    {
        TrackScan out;
        juce::MemoryInputStream in (data, false);
        juce::MidiFile mf;
        if (! mf.readFrom (in))
            return out;

        out.numTracks = mf.getNumTracks();
        if (trackIndex >= out.numTracks)
            return out;

        const auto* seq = mf.getTrack (trackIndex);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& m = seq->getEventPointer (i)->message;
            if (m.isTextMetaEvent() && out.firstText.isEmpty())
                out.firstText = m.getTextFromTextMetaEvent();
            if (m.isNoteOn())
            {
                if (out.firstNoteOnTick < 0.0)
                    out.firstNoteOnTick = m.getTimeStamp();
                ++out.noteOns;
            }
            if (m.isNoteOff())
                ++out.noteOffs;
        }
        return out;
    }
}

int main()
{
    std::cout << "OrchCaptureTakeLogicCheck\n-------------------------\n";

    // normalizeTake: sorts by onset, then pitch.
    {
        std::vector<ocap::CapturedNote> take {
            makeNote (2.0, 3.0, 64),
            makeNote (0.0, 1.0, 67),
            makeNote (0.0, 1.0, 60),
        };
        const auto n = ocap::normalizeTake (take);
        check (n.size() == 3, "normalizeTake keeps all in-range notes");
        check (n[0].ppqOn == 0.0 && n[0].note == 60, "normalizeTake sorts onset then pitch (C first)");
        check (n[1].note == 67, "normalizeTake second note is the higher same-onset pitch");
        check (n[2].ppqOn == 2.0, "normalizeTake orders the later onset last");
    }

    // normalizeTake: drops out-of-range, clamps onset, forces positive length.
    {
        std::vector<ocap::CapturedNote> take {
            makeNote (-1.0, -0.5, 60),   // negative onset -> clamped to 0, length forced positive
            makeNote (1.0, 1.0, 200),    // out-of-range pitch -> dropped
            makeNote (1.0, 1.0, 62),     // zero length -> forced to kMinNoteLengthPpq
        };
        const auto n = ocap::normalizeTake (take);
        check (n.size() == 2, "normalizeTake drops the out-of-range pitch");
        check (n[0].ppqOn == 0.0 && n[0].ppqOff > n[0].ppqOn, "normalizeTake clamps onset and keeps length > 0");
        for (const auto& note : n)
            check (note.ppqOff >= note.ppqOn + ocap::kMinNoteLengthPpq, "normalizeTake enforces the minimum note length");
    }

    // takeLengthPpq.
    {
        std::vector<ocap::CapturedNote> take { makeNote (1.0, 2.5, 60), makeNote (0.5, 4.0, 62) };
        check (std::abs (ocap::takeLengthPpq (take) - 4.0) < 1.0e-9, "takeLengthPpq is the last release ppq");
        check (ocap::takeLengthPpq ({}) == 0.0, "takeLengthPpq is 0 for an empty take");
    }

    // writeTakeMidi: format, track count, meta name, tick placement.
    {
        std::vector<ocap::CapturedNote> take {
            makeNote (0.0, 1.0, 60, 100, 1),
            makeNote (1.0, 2.0, 64, 90, 1),
            makeNote (2.0, 4.0, 67, 80, 1),
        };
        ocap::TakeExportOptions opts;
        opts.trackName = "Violin I";
        opts.tempoBpm = 100.0;
        opts.ticksPerQuarterNote = 960;

        juce::MemoryOutputStream mos;
        ocap::writeTakeMidi (take, opts, mos);
        juce::MemoryBlock data (mos.getData(), mos.getDataSize());

        const auto meta = scan (data, 0);
        check (meta.numTracks == 2, "writeTakeMidi produces a 2-track (Format 1) file");
        check (meta.firstText == "Violin I", "track 0 carries the track name meta");

        const auto notes = scan (data, 1);
        check (notes.firstText == "Violin I", "track 1 carries the track name meta");
        check (notes.noteOns == 3, "track 1 has one note-on per captured note");
        check (notes.noteOffs == 3, "track 1 has a matching note-off per note");
        check (std::abs (notes.firstNoteOnTick - 0.0) < 1.0e-6, "first note-on lands at tick 0");
    }

    // writeTakeMidi: onset scaling by ticksPerQuarterNote.
    {
        std::vector<ocap::CapturedNote> take { makeNote (1.5, 2.0, 60) };
        ocap::TakeExportOptions opts;
        opts.ticksPerQuarterNote = 480;

        juce::MemoryOutputStream mos;
        ocap::writeTakeMidi (take, opts, mos);
        juce::MemoryBlock data (mos.getData(), mos.getDataSize());

        const auto notes = scan (data, 1);
        check (std::abs (notes.firstNoteOnTick - 720.0) < 1.0e-6, "onset 1.5 q at 480 tpqn -> tick 720");
    }

    // writeTakeMidi: empty take still writes a valid 2-track file with no notes.
    {
        juce::MemoryOutputStream mos;
        ocap::writeTakeMidi ({}, {}, mos);
        juce::MemoryBlock data (mos.getData(), mos.getDataSize());
        const auto notes = scan (data, 1);
        check (notes.numTracks == 2, "empty take writes a 2-track file");
        check (notes.noteOns == 0, "empty take writes no note-ons");
    }

    std::cout << "-------------------------\n";
    if (failures == 0)
    {
        std::cout << "[PASS] OrchCaptureTakeLogicCheck passed.\n";
        return 0;
    }

    std::cerr << "[FAIL] " << failures << " failure(s).\n";
    return 1;
}

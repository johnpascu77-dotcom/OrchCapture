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

    ocap::CapturedNote makeKs (double on, double off, int note, int ch = 1)
    {
        auto n = makeNote (on, off, note, 100, ch);
        n.isKeyswitch = true;
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

    // normalizeTake preserves the isKeyswitch flag.
    {
        std::vector<ocap::CapturedNote> take { makeKs (0.0, 0.5, 12), makeNote (0.0, 1.0, 60) };
        const auto n = ocap::normalizeTake (take);
        check (n.size() == 2 && n[0].note == 12 && n[0].isKeyswitch && ! n[1].isKeyswitch,
               "normalizeTake carries isKeyswitch through");
    }

    // KS export modes. Take = 2 musical notes + 3 keyswitches.
    {
        const auto take = [] {
            std::vector<ocap::CapturedNote> t {
                makeNote (0.0, 1.0, 60), makeNote (1.0, 2.0, 64),
                makeKs (0.0, 0.1, 12), makeKs (1.0, 1.1, 13), makeKs (2.0, 2.1, 12),
            };
            return t;
        }();

        ocap::TakeExportOptions opts;
        opts.trackName = "Cello";

        auto writeWith = [&] (ocap::KeyswitchExportMode mode) {
            opts.keyswitchMode = mode;
            juce::MemoryOutputStream mos;
            ocap::writeTakeMidi (take, opts, mos);
            return juce::MemoryBlock (mos.getData(), mos.getDataSize());
        };

        {
            const auto d = writeWith (ocap::KeyswitchExportMode::Inline);
            const auto t1 = scan (d, 1);
            check (t1.numTracks == 2 && t1.noteOns == 5, "Inline: all 5 notes on one track");
        }
        {
            const auto d = writeWith (ocap::KeyswitchExportMode::Exclude);
            const auto t1 = scan (d, 1);
            check (t1.numTracks == 2 && t1.noteOns == 2, "Exclude: only the 2 musical notes");
        }
        {
            const auto d = writeWith (ocap::KeyswitchExportMode::KeyswitchOnly);
            const auto t1 = scan (d, 1);
            check (t1.numTracks == 2 && t1.noteOns == 3, "KS Only: only the 3 keyswitches");
            check (t1.firstText == "Cello KS", "KS Only: track named '<name> KS'");
        }
        {
            const auto d = writeWith (ocap::KeyswitchExportMode::SeparateTrack);
            const auto t1 = scan (d, 1);
            const auto t2 = scan (d, 2);
            check (t1.numTracks == 3, "Separate Track: meta + musical + KS = 3 tracks");
            check (t1.noteOns == 2 && t1.firstText == "Cello", "Separate Track: musical track keeps the name");
            check (t2.noteOns == 3 && t2.firstText == "Cello KS", "Separate Track: KS track named '<name> KS'");
        }
    }

    // SeparateTrack with no keyswitches collapses to the plain 2-track file.
    {
        std::vector<ocap::CapturedNote> take { makeNote (0.0, 1.0, 60) };
        ocap::TakeExportOptions opts;
        opts.trackName = "Flute";
        opts.keyswitchMode = ocap::KeyswitchExportMode::SeparateTrack;

        juce::MemoryOutputStream mos;
        ocap::writeTakeMidi (take, opts, mos);
        juce::MemoryBlock data (mos.getData(), mos.getDataSize());
        const auto t1 = scan (data, 1);
        check (t1.numTracks == 2 && t1.noteOns == 1, "Separate Track with no KS -> 2-track file, no empty KS staff");
    }

    check (ocap::keyswitchTrackName ("Viola") == "Viola KS", "keyswitchTrackName appends ' KS'");
    check (ocap::keyswitchTrackName ("") == "OrchCapture KS", "keyswitchTrackName falls back on an empty name");

    // takeToVar / takeFromVar round-trip.
    {
        std::vector<ocap::CapturedNote> take {
            makeNote (0.25, 1.75, 60, 111, 3), makeKs (2.0, 2.1, 14, 2),
        };
        const auto back = ocap::takeFromVar (ocap::takeToVar (take));
        check (back.size() == 2, "take var round-trip keeps the note count");
        check (std::abs (back[0].ppqOn - 0.25) < 1e-9 && std::abs (back[0].ppqOff - 1.75) < 1e-9
                   && back[0].note == 60 && back[0].velocity == 111 && back[0].channel == 3
                   && ! back[0].isKeyswitch,
               "take var round-trip preserves all fields of a musical note");
        check (back[1].note == 14 && back[1].isKeyswitch, "take var round-trip preserves the KS flag");
    }

    // planNoteTracks matches the modes.
    {
        std::vector<ocap::CapturedNote> take { makeNote (0, 1, 60), makeKs (0, 0.1, 12) };
        ocap::TakeExportOptions o;
        o.trackName = "Horn";

        o.keyswitchMode = ocap::KeyswitchExportMode::Inline;
        check (ocap::planNoteTracks (take, o).size() == 1, "planNoteTracks Inline -> 1 track");

        o.keyswitchMode = ocap::KeyswitchExportMode::SeparateTrack;
        const auto sep = ocap::planNoteTracks (take, o);
        check (sep.size() == 2 && sep[0].name == "Horn" && sep[1].name == "Horn KS",
               "planNoteTracks Separate Track -> 'Horn' + 'Horn KS'");
    }

    // writeMergedTakeMidi: grouping by first-seen track name, Perf before Artic.
    {
        auto perf = [] (const char* name) {
            ocap::TakeForExport t;
            t.options.trackName = name;
            t.options.tapRole = 0;
            t.options.keyswitchMode = ocap::KeyswitchExportMode::Inline;
            t.notes = { makeNote (0, 1, 60), makeNote (1, 2, 62) };
            return t;
        };
        auto artic = [] (const char* name) {
            ocap::TakeForExport t;
            t.options.trackName = name;
            t.options.tapRole = 1;
            t.options.keyswitchMode = ocap::KeyswitchExportMode::KeyswitchOnly;
            t.notes = { makeKs (0, 0.1, 12), makeKs (1, 1.1, 13) };
            return t;
        };

        // Deliberately out of order: Cello artic first, then Viola perf/artic, then Cello perf.
        std::vector<ocap::TakeForExport> takes { artic ("Cello"), perf ("Viola"), artic ("Viola"), perf ("Cello") };

        juce::MemoryOutputStream mos;
        ocap::writeMergedTakeMidi (takes, "Session", 96.0, 960, mos);
        juce::MemoryBlock data (mos.getData(), mos.getDataSize());

        juce::MemoryInputStream in (data, false);
        juce::MidiFile mf;
        check (mf.readFrom (in), "merged file parses");
        check (mf.getNumTracks() == 5, "merged file = tempo + Cello + Cello KS + Viola + Viola KS");

        auto trackName = [&] (int i) {
            const auto* seq = mf.getTrack (i);
            for (int e = 0; e < seq->getNumEvents(); ++e)
                if (seq->getEventPointer (e)->message.isTextMetaEvent())
                    return seq->getEventPointer (e)->message.getTextFromTextMetaEvent();
            return juce::String();
        };
        check (trackName (1) == "Cello" && trackName (2) == "Cello KS", "merged: Cello group first (first-seen), Perf before Artic");
        check (trackName (3) == "Viola" && trackName (4) == "Viola KS", "merged: Viola group second");
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

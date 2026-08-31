#include "OrchCaptureTakeLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ocap
{
    juce::String keyswitchTrackName (const juce::String& trackName)
    {
        return (trackName.isNotEmpty() ? trackName : juce::String ("OrchCapture")) + " KS";
    }

    std::vector<CapturedNote> normalizeTake (std::vector<CapturedNote> notes)
    {
        std::vector<CapturedNote> out;
        out.reserve (notes.size());

        for (auto n : notes)
        {
            if (n.note < 0 || n.note > 127)
                continue;

            n.channel = juce::jlimit (1, 16, n.channel);
            n.velocity = juce::jlimit (1, 127, n.velocity);
            n.ppqOn = juce::jmax (0.0, n.ppqOn);
            n.ppqOff = juce::jmax (n.ppqOff, n.ppqOn + kMinNoteLengthPpq);

            out.push_back (n);
        }

        std::sort (out.begin(), out.end(), [] (const CapturedNote& a, const CapturedNote& b)
        {
            if (a.ppqOn != b.ppqOn)
                return a.ppqOn < b.ppqOn;
            return a.note < b.note;
        });

        return out;
    }

    double takeLengthPpq (const std::vector<CapturedNote>& notes)
    {
        double earliest = std::numeric_limits<double>::max();
        double latest = 0.0;
        bool any = false;

        for (const auto& n : notes)
        {
            any = true;
            earliest = juce::jmin (earliest, n.ppqOn);
            latest = juce::jmax (latest, n.ppqOff);
        }

        if (! any)
            return 0.0;

        return juce::jmax (0.0, latest - juce::jmin (earliest, 0.0));
    }

    std::vector<CapturedNote> quantizeTake (std::vector<CapturedNote> notes, double gridPpq)
    {
        if (gridPpq <= 0.0)
            return notes;

        for (auto& n : notes)
        {
            const double on = juce::jmax (0.0, std::round (n.ppqOn / gridPpq) * gridPpq);
            double off = std::round (n.ppqOff / gridPpq) * gridPpq;
            if (off < on + gridPpq)
                off = on + gridPpq;
            n.ppqOn = on;
            n.ppqOff = off;
        }

        return notes;
    }

    std::vector<SectionMarker> parseSectionMarkers (const juce::String& text)
    {
        std::vector<SectionMarker> out;

        juce::StringArray tokens;
        tokens.addTokens (text, ",\n;", "");

        for (auto token : tokens)
        {
            token = token.trim();
            if (token.isEmpty())
                continue;

            const int colon = token.indexOfChar (':');
            if (colon <= 0)
                continue;

            const auto barText = token.substring (0, colon).trim();
            const auto label = token.substring (colon + 1).trim();
            if (label.isEmpty() || ! barText.containsOnly ("0123456789.-"))
                continue;

            out.push_back ({ juce::jmax (0.0, barText.getDoubleValue()), label });
        }

        return out;
    }

    std::vector<TempoMark> parseTempoMarks (const juce::String& text)
    {
        std::vector<TempoMark> out;

        juce::StringArray tokens;
        tokens.addTokens (text, ",\n;", "");

        for (auto token : tokens)
        {
            token = token.trim();
            const int colon = token.indexOfChar (':');
            if (colon <= 0)
                continue;

            const auto barText = token.substring (0, colon).trim();
            const auto bpmText = token.substring (colon + 1).trim();
            if (! barText.containsOnly ("0123456789.-") || ! bpmText.containsOnly ("0123456789.-"))
                continue;

            const double bpm = bpmText.getDoubleValue();
            if (bpm <= 0.0)
                continue;

            out.push_back ({ juce::jmax (1.0, barText.getDoubleValue()), bpm });
        }

        return out;
    }

    juce::StringArray parseScoreOrder (const juce::String& text)
    {
        juce::StringArray out;
        out.addTokens (text, ",\n;", "");
        out.trim();
        out.removeEmptyStrings();
        return out;
    }

    std::vector<NoteTrack> planNoteTracks (const std::vector<CapturedNote>& notes,
                                           const TakeExportOptions& options)
    {
        const auto normalized = normalizeTake (quantizeTake (notes, options.quantizeGridPpq));

        std::vector<CapturedNote> musical, keyswitch;
        for (const auto& n : normalized)
            (n.isKeyswitch ? keyswitch : musical).push_back (n);

        std::vector<NoteTrack> tracks;
        switch (options.keyswitchMode)
        {
            case KeyswitchExportMode::Inline:
                tracks.push_back ({ options.trackName, normalized });
                break;
            case KeyswitchExportMode::Exclude:
                tracks.push_back ({ options.trackName, musical });
                break;
            case KeyswitchExportMode::KeyswitchOnly:
                tracks.push_back ({ keyswitchTrackName (options.trackName), keyswitch });
                break;
            case KeyswitchExportMode::SeparateTrack:
                tracks.push_back ({ options.trackName, musical });
                if (! keyswitch.empty())
                    tracks.push_back ({ keyswitchTrackName (options.trackName), keyswitch });
                break;
        }

        return tracks;
    }

    namespace
    {
        int usPerQuarter (double bpm)
        {
            return static_cast<int> (std::llround (60000000.0 / juce::jmax (1.0, bpm)));
        }

        juce::MidiMessageSequence tempoMetaTrack (const juce::String& name, double tempoBpm,
                                                 const std::vector<SectionMarker>& markers = {},
                                                 const std::vector<TempoMark>& tempoChanges = {},
                                                 double barLengthPpq = 4.0, int tpqn = 960)
        {
            const double barPpq = juce::jmax (0.25, barLengthPpq);

            juce::MidiMessageSequence meta;
            meta.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);

            if (tempoChanges.empty())
            {
                meta.addEvent (juce::MidiMessage::tempoMetaEvent (usPerQuarter (tempoBpm)), 0.0);
            }
            else
            {
                bool haveAtZero = false;
                for (const auto& t : tempoChanges)
                {
                    const double tick = juce::jmax (0.0, t.bar - 1.0) * barPpq * tpqn;
                    if (tick <= 0.0)
                        haveAtZero = true;
                    meta.addEvent (juce::MidiMessage::tempoMetaEvent (usPerQuarter (t.bpm)), tick);
                }
                if (! haveAtZero) // always anchor a tempo at the start
                    meta.addEvent (juce::MidiMessage::tempoMetaEvent (usPerQuarter (tempoBpm)), 0.0);
            }

            for (const auto& m : markers)
            {
                // m.bar is 1-indexed (bar 1 == the take's start == tick 0), to
                // match how Bitwig / Dorico number bars.
                const double tick = juce::jmax (0.0, m.bar - 1.0) * barPpq * tpqn;
                meta.addEvent (juce::MidiMessage::textMetaEvent (6, m.label), tick); // 6 = marker
            }

            meta.updateMatchedPairs();
            return meta;
        }

        bool isKeyswitchTrackName (const juce::String& name)
        {
            return name.endsWith (" KS");
        }

        bool keepTrack (const juce::String& name, MergedContent content)
        {
            switch (content)
            {
                case MergedContent::NotesOnly:        return ! isKeyswitchTrackName (name);
                case MergedContent::KeyswitchesOnly:  return isKeyswitchTrackName (name);
                case MergedContent::NotesAndKeyswitches:
                default:                              return true;
            }
        }

        void appendNoteTrack (juce::MidiFile& midiFile, const NoteTrack& track, int tpqn)
        {
            juce::MidiMessageSequence seq;
            seq.addEvent (juce::MidiMessage::textMetaEvent (3, track.name), 0.0);

            for (const auto& n : track.notes)
            {
                const double onTick = n.ppqOn * tpqn;
                const double offTick = juce::jmax (onTick + 1.0, n.ppqOff * tpqn);

                seq.addEvent (juce::MidiMessage::noteOn (n.channel, n.note,
                                                        static_cast<juce::uint8> (n.velocity)), onTick);
                seq.addEvent (juce::MidiMessage::noteOff (n.channel, n.note), offTick);
            }

            seq.updateMatchedPairs();
            midiFile.addTrack (seq);
        }
    }

    void writeTakeMidi (const std::vector<CapturedNote>& notes,
                        const TakeExportOptions& options,
                        juce::OutputStream& out)
    {
        const int tpqn = juce::jlimit (24, 3840, options.ticksPerQuarterNote);

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (tpqn);
        midiFile.addTrack (tempoMetaTrack (options.trackName, options.tempoBpm));

        for (const auto& track : planNoteTracks (notes, options))
            appendNoteTrack (midiFile, track, tpqn);

        midiFile.writeTo (out);
    }

    void writeMergedTakeMidi (const std::vector<TakeForExport>& takes,
                              const MergedExportOptions& options,
                              juce::OutputStream& out)
    {
        const int tpqn = juce::jlimit (24, 3840, options.ticksPerQuarterNote);

        // First-seen order of instrument track names (fallback for anything not
        // named in options.scoreOrder).
        juce::StringArray firstSeen;
        for (const auto& t : takes)
            firstSeen.addIfNotAlreadyThere (t.options.trackName);

        const auto rank = [&] (const juce::String& name)
        {
            const int explicitIdx = options.scoreOrder.indexOf (name);
            if (explicitIdx >= 0)
                return explicitIdx;
            return options.scoreOrder.size() + juce::jmax (0, firstSeen.indexOf (name));
        };

        std::vector<size_t> laneOrder (takes.size());
        for (size_t i = 0; i < takes.size(); ++i)
            laneOrder[i] = i;

        std::stable_sort (laneOrder.begin(), laneOrder.end(), [&] (size_t a, size_t b)
        {
            const int ra = rank (takes[a].options.trackName);
            const int rb = rank (takes[b].options.trackName);
            if (ra != rb)
                return ra < rb;
            return takes[a].options.tapRole < takes[b].options.tapRole;
        });

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (tpqn);
        midiFile.addTrack (tempoMetaTrack (options.sessionName.isNotEmpty() ? options.sessionName
                                                                           : juce::String ("OrchCapture session"),
                                           options.tempoBpm, options.markers, options.tempoChanges,
                                           options.barLengthPpq, tpqn));

        for (const size_t idx : laneOrder)
            for (const auto& track : planNoteTracks (takes[idx].notes, takes[idx].options))
                if (keepTrack (track.name, options.content))
                    appendNoteTrack (midiFile, track, tpqn);

        midiFile.writeTo (out);
    }

    juce::var takeToVar (const std::vector<CapturedNote>& notes)
    {
        juce::Array<juce::var> arr;
        arr.ensureStorageAllocated (static_cast<int> (notes.size()));

        for (const auto& n : notes)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("on", n.ppqOn);
            o->setProperty ("off", n.ppqOff);
            o->setProperty ("n", n.note);
            o->setProperty ("v", n.velocity);
            o->setProperty ("c", n.channel);
            o->setProperty ("ks", n.isKeyswitch);
            arr.add (juce::var (o));
        }

        return arr;
    }

    std::vector<CapturedNote> takeFromVar (const juce::var& value)
    {
        std::vector<CapturedNote> out;

        if (const auto* arr = value.getArray())
        {
            out.reserve (static_cast<size_t> (arr->size()));
            for (const auto& item : *arr)
            {
                CapturedNote n;
                n.ppqOn = static_cast<double> (item.getProperty ("on", 0.0));
                n.ppqOff = static_cast<double> (item.getProperty ("off", 0.0));
                n.note = static_cast<int> (item.getProperty ("n", 60));
                n.velocity = static_cast<int> (item.getProperty ("v", 100));
                n.channel = static_cast<int> (item.getProperty ("c", 1));
                n.isKeyswitch = static_cast<bool> (item.getProperty ("ks", false));
                out.push_back (n);
            }
        }

        return out;
    }
}

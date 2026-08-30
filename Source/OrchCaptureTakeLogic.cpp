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

    std::vector<NoteTrack> planNoteTracks (const std::vector<CapturedNote>& notes,
                                           const TakeExportOptions& options)
    {
        const auto normalized = normalizeTake (notes);

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
        juce::MidiMessageSequence tempoMetaTrack (const juce::String& name, double tempoBpm)
        {
            juce::MidiMessageSequence meta;
            meta.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);
            const int microsecondsPerQuarter =
                static_cast<int> (std::llround (60000000.0 / juce::jmax (1.0, tempoBpm)));
            meta.addEvent (juce::MidiMessage::tempoMetaEvent (microsecondsPerQuarter), 0.0);
            meta.updateMatchedPairs();
            return meta;
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
                              const juce::String& sessionName,
                              double tempoBpm,
                              int ticksPerQuarterNote,
                              juce::OutputStream& out)
    {
        const int tpqn = juce::jlimit (24, 3840, ticksPerQuarterNote);

        // First-seen order of instrument track names.
        juce::StringArray order;
        for (const auto& t : takes)
            order.addIfNotAlreadyThere (t.options.trackName);

        // Stable index into `takes`, sorted by (first-seen trackName, tapRole).
        std::vector<size_t> laneOrder (takes.size());
        for (size_t i = 0; i < takes.size(); ++i)
            laneOrder[i] = i;

        std::stable_sort (laneOrder.begin(), laneOrder.end(), [&] (size_t a, size_t b)
        {
            const int ga = order.indexOf (takes[a].options.trackName);
            const int gb = order.indexOf (takes[b].options.trackName);
            if (ga != gb)
                return ga < gb;
            return takes[a].options.tapRole < takes[b].options.tapRole;
        });

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (tpqn);
        midiFile.addTrack (tempoMetaTrack (sessionName.isNotEmpty() ? sessionName
                                                                    : juce::String ("OrchCapture session"),
                                           tempoBpm));

        for (const size_t idx : laneOrder)
            for (const auto& track : planNoteTracks (takes[idx].notes, takes[idx].options))
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

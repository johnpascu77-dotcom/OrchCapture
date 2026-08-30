#include "OrchCaptureTakeLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

    namespace
    {
        void appendNoteTrack (juce::MidiFile& midiFile,
                              const juce::String& name,
                              const std::vector<CapturedNote>& notes,
                              int tpqn)
        {
            juce::MidiMessageSequence track;
            track.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);

            for (const auto& n : notes)
            {
                const double onTick = n.ppqOn * tpqn;
                const double offTick = juce::jmax (onTick + 1.0, n.ppqOff * tpqn);

                track.addEvent (juce::MidiMessage::noteOn (n.channel, n.note,
                                                          static_cast<juce::uint8> (n.velocity)), onTick);
                track.addEvent (juce::MidiMessage::noteOff (n.channel, n.note), offTick);
            }

            track.updateMatchedPairs();
            midiFile.addTrack (track);
        }
    }

    void writeTakeMidi (const std::vector<CapturedNote>& notes,
                        const TakeExportOptions& options,
                        juce::OutputStream& out)
    {
        const int tpqn = juce::jlimit (24, 3840, options.ticksPerQuarterNote);
        const auto normalized = normalizeTake (notes);

        std::vector<CapturedNote> musical, keyswitch;
        for (const auto& n : normalized)
            (n.isKeyswitch ? keyswitch : musical).push_back (n);

        // Which note tracks to emit, in order.
        std::vector<std::pair<juce::String, const std::vector<CapturedNote>*>> noteTracks;
        switch (options.keyswitchMode)
        {
            case KeyswitchExportMode::Inline:
                noteTracks.emplace_back (options.trackName, &normalized);
                break;
            case KeyswitchExportMode::Exclude:
                noteTracks.emplace_back (options.trackName, &musical);
                break;
            case KeyswitchExportMode::KeyswitchOnly:
                noteTracks.emplace_back (keyswitchTrackName (options.trackName), &keyswitch);
                break;
            case KeyswitchExportMode::SeparateTrack:
                noteTracks.emplace_back (options.trackName, &musical);
                if (! keyswitch.empty())
                    noteTracks.emplace_back (keyswitchTrackName (options.trackName), &keyswitch);
                break;
        }

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (tpqn);

        // Track 0: name + tempo. Dorico reads the first text meta as the
        // sequence name; the tempo keeps the imported part at the right speed.
        juce::MidiMessageSequence meta;
        meta.addEvent (juce::MidiMessage::textMetaEvent (3, options.trackName), 0.0);
        const int microsecondsPerQuarter =
            static_cast<int> (std::llround (60000000.0 / juce::jmax (1.0, options.tempoBpm)));
        meta.addEvent (juce::MidiMessage::tempoMetaEvent (microsecondsPerQuarter), 0.0);
        meta.updateMatchedPairs();
        midiFile.addTrack (meta);

        for (const auto& [name, vec] : noteTracks)
            appendNoteTrack (midiFile, name, *vec, tpqn);

        midiFile.writeTo (out);
    }
}

#include "OrchCaptureTakeLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ocap
{
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

    void writeTakeMidi (const std::vector<CapturedNote>& notes,
                        const TakeExportOptions& options,
                        juce::OutputStream& out)
    {
        const int tpqn = juce::jlimit (24, 3840, options.ticksPerQuarterNote);
        const auto normalized = normalizeTake (notes);

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

        // Track 1: the instrument name again (Dorico assigns the staff from the
        // track name) and the notes, as performed.
        juce::MidiMessageSequence track;
        track.addEvent (juce::MidiMessage::textMetaEvent (3, options.trackName), 0.0);

        for (const auto& n : normalized)
        {
            const double onTick = n.ppqOn * tpqn;
            const double offTick = juce::jmax (onTick + 1.0, n.ppqOff * tpqn);

            track.addEvent (juce::MidiMessage::noteOn (n.channel, n.note,
                                                      static_cast<juce::uint8> (n.velocity)), onTick);
            track.addEvent (juce::MidiMessage::noteOff (n.channel, n.note), offTick);
        }

        track.updateMatchedPairs();
        midiFile.addTrack (track);

        midiFile.writeTo (out);
    }
}

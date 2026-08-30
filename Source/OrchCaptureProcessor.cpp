#include "OrchCaptureProcessor.h"
#include "OrchCaptureEditor.h"
#include "OrchCaptureLink.h"

#include <algorithm>
#include <utility>

namespace
{
    constexpr size_t kMaxCapturedNotes = 200000; // ~hours of a mono part; a safety rail
    constexpr size_t kMaxOpenNotes = 4096;
    constexpr double kRewindBeatsForNewTake = 0.5;
}

OrchCaptureAudioProcessor::OrchCaptureAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchCaptureParameters", createParameterLayout())
{
    enableParam = parameters.getRawParameterValue ("enable");
    resetOnPlayParam = parameters.getRawParameterValue ("resetOnPlay");
    tapRoleParam = parameters.getRawParameterValue ("tapRole");
    ksZoneMinParam = parameters.getRawParameterValue ("ksZoneMin");
    ksZoneMaxParam = parameters.getRawParameterValue ("ksZoneMax");
    ksExportModeParam = parameters.getRawParameterValue ("ksExportMode");
    coordinatorParam = parameters.getRawParameterValue ("coordinator");

    capturedNotes.reserve (4096);
    openNotes.reserve (256);

    link = std::make_unique<OrchCaptureLink> (*this);
}

OrchCaptureAudioProcessor::~OrchCaptureAudioProcessor()
{
    link.reset();
}

juce::AudioProcessorValueTreeState::ParameterLayout OrchCaptureAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "enable", 1 }, "Capture Enabled", true));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "resetOnPlay", 1 }, "New Take On Play", true));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "tapRole", 1 }, "Tap Role",
        juce::StringArray { "Performance", "Articulation" }, 0));

    // Unified keyswitch zone. Default 12..23 = OrchNoteMapper's unified source
    // window (marker at C-1 = MIDI 12, spread by Randomize Pitch 0..6). Only
    // meaningful upstream of OrchNoteMapper, i.e. in the Articulation role.
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMin", 1 }, "KS Zone Min", 0, 127, 12));
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMax", 1 }, "KS Zone Max", 0, 127, 23));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "ksExportMode", 1 }, "KS Export",
        juce::StringArray { "Inline", "Separate Track", "Exclude", "KS Only" }, 0));

    // One instance in the rig turns this on to become the export hub: it binds
    // the local coordinator socket, collects every other instance's take, and
    // its editor gains "Export All".
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "coordinator", 1 }, "Coordinator", false));

    return { params.begin(), params.end() };
}

void OrchCaptureAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    const juce::SpinLock::ScopedLockType lock (captureLock);
    capturedNotes.clear();
    openNotes.clear();
    takeStartPpq = 0.0;
    wasPlaying = false;
    haveLastBlockEnd = false;
    lastBlockEndPpq = 0.0;
    refreshTakeStatus();
}

void OrchCaptureAudioProcessor::releaseResources() {}

bool OrchCaptureAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const { return true; }

void OrchCaptureAudioProcessor::resetTake (double takeOriginPpq)
{
    // Caller holds captureLock.
    capturedNotes.clear();
    openNotes.clear();
    takeStartPpq = takeOriginPpq;
    lastCapturedNoteUi.store (-1);
    takeNoteCountUi.store (0);
    takeKeyswitchCountUi.store (0);
    takeLengthPpqUi.store (0.0);
    takeGenerationUi.fetch_add (1);
}

bool OrchCaptureAudioProcessor::noteInKeyswitchZone (int note) const noexcept
{
    int lo = ksZoneMinParam != nullptr ? juce::roundToInt (ksZoneMinParam->load()) : 12;
    int hi = ksZoneMaxParam != nullptr ? juce::roundToInt (ksZoneMaxParam->load()) : 23;
    if (lo > hi)
        std::swap (lo, hi);
    return note >= lo && note <= hi;
}

void OrchCaptureAudioProcessor::finalizeOpenNotes (double ppqOff)
{
    // Caller holds captureLock. ppqOff is absolute; capturedNotes are stored
    // relative to takeStartPpq.
    for (const auto& open : openNotes)
    {
        if (capturedNotes.size() >= kMaxCapturedNotes)
            break;

        ocap::CapturedNote n;
        n.channel = open.channel;
        n.note = open.note;
        n.velocity = open.velocity;
        n.ppqOn = juce::jmax (0.0, open.ppqOn - takeStartPpq);
        n.ppqOff = juce::jmax (n.ppqOn, ppqOff - takeStartPpq);
        n.isKeyswitch = noteInKeyswitchZone (open.note);
        capturedNotes.push_back (n);
    }

    openNotes.clear();
}

void OrchCaptureAudioProcessor::refreshTakeStatus()
{
    // Caller holds captureLock.
    double maxOffRel = 0.0;
    int keyswitchCount = 0;
    for (const auto& n : capturedNotes)
    {
        maxOffRel = juce::jmax (maxOffRel, n.ppqOff);
        if (n.isKeyswitch)
            ++keyswitchCount;
    }
    for (const auto& open : openNotes)
        maxOffRel = juce::jmax (maxOffRel, open.ppqOn - takeStartPpq);

    takeNoteCountUi.store (static_cast<int> (capturedNotes.size()));
    takeKeyswitchCountUi.store (keyswitchCount);
    takeLengthPpqUi.store (juce::jmax (0.0, maxOffRel));
    lastCapturedNoteUi.store (capturedNotes.empty() ? -1 : capturedNotes.back().note);
}

void OrchCaptureAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // MIDI effect: no audio to produce, and midiMessages passes straight
    // through untouched - this plugin only observes.
    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    bool playing = false;
    bool havePpq = false;
    double bpm = currentBpmUi.load();
    double blockStartPpq = lastBlockEndPpq;

    if (auto* transport = getPlayHead())
    {
        if (const auto pos = transport->getPosition())
        {
            playing = pos->getIsPlaying();

            if (const auto hostBpm = pos->getBpm(); hostBpm && *hostBpm > 0.0)
                bpm = *hostBpm;

            if (const auto ppq = pos->getPpqPosition())
            {
                blockStartPpq = *ppq;
                havePpq = true;
            }
        }
    }

    currentBpmUi.store (bpm);
    transportPlayingUi.store (playing);

    const double ppqPerSample = sampleRate > 0.0 ? (bpm / 60.0) / sampleRate : 0.0;
    const double blockEndPpq = blockStartPpq + numSamples * ppqPerSample;

    const bool enabled = enableParam != nullptr && enableParam->load() >= 0.5f;
    const bool resetOnPlay = resetOnPlayParam == nullptr || resetOnPlayParam->load() >= 0.5f;

    if (! enabled)
    {
        wasPlaying = playing;
        lastBlockEndPpq = blockEndPpq;
        haveLastBlockEnd = havePpq;
        return;
    }

    const juce::SpinLock::ScopedLockType lock (captureLock);

    if (pendingClear.exchange (false))
        resetTake (havePpq ? blockStartPpq : 0.0);

    bool startNewTake = false;
    if (playing && ! wasPlaying)
        startNewTake = resetOnPlay;
    else if (playing && wasPlaying && haveLastBlockEnd && havePpq
             && blockStartPpq < lastBlockEndPpq - kRewindBeatsForNewTake)
        startNewTake = true; // transport jumped backwards - treat as a new take

    if (startNewTake)
        resetTake (blockStartPpq);

    if (! playing && wasPlaying)
        finalizeOpenNotes (haveLastBlockEnd ? lastBlockEndPpq : takeStartPpq);

    if (playing && havePpq)
    {
        for (const auto metadata : midiMessages)
        {
            const auto message = metadata.getMessage();
            const double eventPpq = blockStartPpq + metadata.samplePosition * ppqPerSample;

            if (message.isNoteOn() && message.getVelocity() > 0)
            {
                if (openNotes.size() >= kMaxOpenNotes)
                    openNotes.erase (openNotes.begin());

                OpenNote open;
                open.channel = juce::jlimit (1, 16, message.getChannel());
                open.note = juce::jlimit (0, 127, message.getNoteNumber());
                open.velocity = juce::jlimit (1, 127, static_cast<int> (message.getVelocity()));
                open.ppqOn = eventPpq;
                openNotes.push_back (open);
            }
            else if (message.isNoteOff() || (message.isNoteOn() && message.getVelocity() == 0))
            {
                const int channel = juce::jlimit (1, 16, message.getChannel());
                const int note = juce::jlimit (0, 127, message.getNoteNumber());

                const auto it = std::find_if (openNotes.begin(), openNotes.end(),
                    [&] (const OpenNote& o) { return o.channel == channel && o.note == note; });

                if (it != openNotes.end() && capturedNotes.size() < kMaxCapturedNotes)
                {
                    ocap::CapturedNote n;
                    n.channel = it->channel;
                    n.note = it->note;
                    n.velocity = it->velocity;
                    n.ppqOn = juce::jmax (0.0, it->ppqOn - takeStartPpq);
                    n.ppqOff = juce::jmax (n.ppqOn, eventPpq - takeStartPpq);
                    n.isKeyswitch = noteInKeyswitchZone (it->note);
                    capturedNotes.push_back (n);
                }

                if (it != openNotes.end())
                    openNotes.erase (it);
            }
            else if (message.isAllNotesOff() || message.isAllSoundOff())
            {
                finalizeOpenNotes (eventPpq);
            }
        }
    }

    wasPlaying = playing;
    lastBlockEndPpq = blockEndPpq;
    haveLastBlockEnd = havePpq;

    refreshTakeStatus();
}

void OrchCaptureAudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (properties.name.has_value())
    {
        const juce::ScopedLock sl (trackNameLock);
        hostTrackName = *properties.name;
        haveHostTrackName.store (hostTrackName.isNotEmpty());
    }
}

juce::String OrchCaptureAudioProcessor::getTrackNameForUi() const
{
    const juce::ScopedLock sl (trackNameLock);
    if (haveHostTrackName.load() && hostTrackName.isNotEmpty())
        return hostTrackName;
    return "OrchCapture";
}

std::vector<ocap::CapturedNote> OrchCaptureAudioProcessor::snapshotTake() const
{
    const juce::SpinLock::ScopedLockType lock (captureLock);
    return capturedNotes;
}

void OrchCaptureAudioProcessor::clearTake()
{
    pendingClear.store (true);
}

int OrchCaptureAudioProcessor::getTapRoleForUi() const
{
    return tapRoleParam != nullptr && tapRoleParam->load() >= 0.5f ? 1 : 0;
}

bool OrchCaptureAudioProcessor::isCoordinatorParamOn() const
{
    return coordinatorParam != nullptr && coordinatorParam->load() >= 0.5f;
}

ocap::TakeExportOptions OrchCaptureAudioProcessor::buildExportOptions() const
{
    ocap::TakeExportOptions options;
    options.trackName = getTrackNameForUi();
    options.tempoBpm = juce::jmax (1.0, currentBpmUi.load());
    options.ticksPerQuarterNote = 960;

    const int mode = ksExportModeParam != nullptr
        ? juce::jlimit (0, 3, juce::roundToInt (ksExportModeParam->load())) : 0;
    options.keyswitchMode = static_cast<ocap::KeyswitchExportMode> (mode);

    return options;
}

juce::AudioProcessorEditor* OrchCaptureAudioProcessor::createEditor()
{
    return new OrchCaptureAudioProcessorEditor (*this);
}

bool OrchCaptureAudioProcessor::hasEditor() const { return true; }
const juce::String OrchCaptureAudioProcessor::getName() const { return JucePlugin_Name; }
bool OrchCaptureAudioProcessor::acceptsMidi() const { return true; }
bool OrchCaptureAudioProcessor::producesMidi() const { return true; }
bool OrchCaptureAudioProcessor::isMidiEffect() const { return true; }
double OrchCaptureAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int OrchCaptureAudioProcessor::getNumPrograms() { return 1; }
int OrchCaptureAudioProcessor::getCurrentProgram() { return 0; }
void OrchCaptureAudioProcessor::setCurrentProgram (int) {}
const juce::String OrchCaptureAudioProcessor::getProgramName (int) { return {}; }
void OrchCaptureAudioProcessor::changeProgramName (int, const juce::String&) {}

void OrchCaptureAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = parameters.copyState(); state.isValid())
        if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
            copyXmlToBinary (*xml, destData);
}

void OrchCaptureAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes)))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OrchCaptureAudioProcessor();
}

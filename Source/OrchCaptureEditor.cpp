#include "OrchCaptureEditor.h"
#include "OrchCaptureTakeLogic.h"

namespace
{
    const juce::Colour kBackground = juce::Colour::fromRGB (18, 24, 31);
    const juce::Colour kAccent     = juce::Colour::fromRGB (120, 210, 160);
    const juce::Colour kPadIdle    = juce::Colour::fromRGB (34, 44, 40);

    void styleLabel (juce::Label& label, float size, bool bold = false)
    {
        label.setColour (juce::Label::textColourId, juce::Colours::white);
        label.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    juce::File tempMidiPath (const juce::String& trackName)
    {
        const auto stem = juce::File::createLegalFileName (
            trackName.isNotEmpty() ? trackName : juce::String ("OrchCapture"));
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("OrchCapture_" + stem + "_"
                                  + juce::String (juce::Time::getCurrentTime().toMilliseconds())
                                  + ".mid");
    }
}

// ============================ DragPad ================================

void OrchCaptureAudioProcessorEditor::DragPad::paint (juce::Graphics& g)
{
    const bool ready = canDrag && canDrag();

    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (ready ? kPadIdle : juce::Colour::fromRGB (28, 32, 36));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (ready ? kAccent : juce::Colour::fromRGB (70, 80, 78));
    g.drawRoundedRectangle (bounds, 6.0f, 1.5f);

    g.setColour (ready ? juce::Colours::white : juce::Colour::fromRGB (130, 140, 138));
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText (ready ? "Drag MIDI out  \xe2\x86\x92" : "Drag MIDI out (no take)",
                getLocalBounds(), juce::Justification::centred);
}

void OrchCaptureAudioProcessorEditor::DragPad::mouseDown (const juce::MouseEvent& e)
{
    dragStart = e.position;
    dragFired = false;
}

void OrchCaptureAudioProcessorEditor::DragPad::mouseDrag (const juce::MouseEvent& e)
{
    if (dragFired || onRequestFile == nullptr)
        return;

    if (e.position.getDistanceFrom (dragStart) < 6.0f)
        return;

    dragFired = true; // one attempt per mouse-down

    const auto file = onRequestFile();
    if (file == juce::File())
        return;

    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        container->performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
}

// ============================ Editor =================================

OrchCaptureAudioProcessorEditor::OrchCaptureAudioProcessorEditor (OrchCaptureAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setResizable (true, true);
    setResizeLimits (460, 320, 900, 620);
    setSize (540, 380);

    auto& params = audioProcessor.getParameters();

    titleLabel.setText ("OrchCapture", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    styleLabel (titleLabel, 26.0f, true);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Post-chain MIDI take recorder", juce::dontSendNotification);
    subtitleLabel.setJustificationType (juce::Justification::centred);
    styleLabel (subtitleLabel, 13.0f);
    addAndMakeVisible (subtitleLabel);

    buildLabel.setText ("Build: Phase 1 (MVP)", juce::dontSendNotification);
    buildLabel.setJustificationType (juce::Justification::centred);
    buildLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 160, 170));
    buildLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (buildLabel);

    enableButton.setButtonText ("Capture Enabled");
    enableButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (enableButton);
    enableAttachment = std::make_unique<ButtonAttachment> (params, "enable", enableButton);

    resetOnPlayButton.setButtonText ("New take on transport start");
    resetOnPlayButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (resetOnPlayButton);
    resetOnPlayAttachment = std::make_unique<ButtonAttachment> (params, "resetOnPlay", resetOnPlayButton);

    trackNameLabel.setJustificationType (juce::Justification::centredLeft);
    styleLabel (trackNameLabel, 14.0f, true);
    addAndMakeVisible (trackNameLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (245, 205, 120));
    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);

    dragPad.onRequestFile = [this] { return writeTakeToTempFile(); };
    dragPad.canDrag = [this]
    {
        return ! audioProcessor.isTransportPlayingForUi() && audioProcessor.getTakeNoteCountForUi() > 0;
    };
    addAndMakeVisible (dragPad);

    saveButton.onClick = [this] { saveTakeToFolder(); };
    addAndMakeVisible (saveButton);

    clearButton.onClick = [this] { audioProcessor.clearTake(); };
    addAndMakeVisible (clearButton);

    updateStatus();
    startTimerHz (10);
}

OrchCaptureAudioProcessorEditor::~OrchCaptureAudioProcessorEditor()
{
    stopTimer();
}

void OrchCaptureAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (kAccent);
    g.drawRoundedRectangle (bounds, 8.0f, 2.0f);
}

void OrchCaptureAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (22, 16);

    titleLabel.setBounds (area.removeFromTop (32));
    subtitleLabel.setBounds (area.removeFromTop (18));
    buildLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (12);

    trackNameLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (14);

    enableButton.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    resetOnPlayButton.setBounds (area.removeFromTop (26));
    area.removeFromTop (16);

    dragPad.setBounds (area.removeFromTop (52));
    area.removeFromTop (10);

    auto row = area.removeFromTop (30);
    saveButton.setBounds (row.removeFromLeft (row.getWidth() * 2 / 3).reduced (0, 2));
    row.removeFromLeft (8);
    clearButton.setBounds (row.reduced (0, 2));
}

void OrchCaptureAudioProcessorEditor::timerCallback()
{
    updateStatus();
    dragPad.repaint();
}

void OrchCaptureAudioProcessorEditor::updateStatus()
{
    const auto name = audioProcessor.getTrackNameForUi();
    trackNameLabel.setText ("Track: " + name
                                + (audioProcessor.hasHostTrackNameForUi() ? "" : "  (host sent no name)"),
                            juce::dontSendNotification);

    const int count = audioProcessor.getTakeNoteCountForUi();
    const double lengthPpq = audioProcessor.getTakeLengthQuarterNotesForUi();
    const double bars = lengthPpq / 4.0; // 4/4 assumed for the readout
    const bool playing = audioProcessor.isTransportPlayingForUi();

    juce::String text;
    text << "Take: " << count << (count == 1 ? " note" : " notes")
         << "  |  " << juce::String (bars, 1) << " bars"
         << "  |  " << (playing ? "recording" : "stopped");

    const int last = audioProcessor.getLastCapturedNoteForUi();
    if (last >= 0)
        text << "  |  last " << juce::MidiMessage::getMidiNoteName (last, true, true, 4);

    statusLabel.setText (text, juce::dontSendNotification);
}

juce::File OrchCaptureAudioProcessorEditor::writeTakeToTempFile()
{
    if (audioProcessor.isTransportPlayingForUi())
        return {};

    const auto take = audioProcessor.snapshotTake();
    if (take.empty())
        return {};

    const auto options = audioProcessor.buildExportOptions();
    const auto file = tempMidiPath (options.trackName);

    juce::FileOutputStream stream (file);
    if (! stream.openedOk())
        return {};

    ocap::writeTakeMidi (take, options, stream);
    stream.flush();
    return file;
}

void OrchCaptureAudioProcessorEditor::saveTakeToFolder()
{
    if (audioProcessor.isTransportPlayingForUi() || audioProcessor.getTakeNoteCountForUi() == 0)
    {
        statusLabel.setText ("Nothing to save (stop the transport and record a take first)",
                             juce::dontSendNotification);
        return;
    }

    const auto options = audioProcessor.buildExportOptions();
    const auto suggested = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile (juce::File::createLegalFileName (options.trackName) + ".mid");

    fileChooser = std::make_unique<juce::FileChooser> ("Save take as MIDI", suggested, "*.mid");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto target = fc.getResult();
        if (target == juce::File())
            return;

        const auto take = audioProcessor.snapshotTake();
        if (take.empty())
            return;

        auto file = target.hasFileExtension ("mid") ? target : target.withFileExtension ("mid");

        juce::FileOutputStream stream (file);
        if (! stream.openedOk())
        {
            statusLabel.setText ("Could not write " + file.getFullPathName(), juce::dontSendNotification);
            return;
        }

        stream.setPosition (0);
        stream.truncate();
        ocap::writeTakeMidi (take, audioProcessor.buildExportOptions(), stream);
        stream.flush();
        statusLabel.setText ("Saved " + file.getFileName(), juce::dontSendNotification);
    });
}

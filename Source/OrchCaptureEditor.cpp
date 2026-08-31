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
    const juce::String arrow (juce::CharPointer_UTF8 ("\xe2\x86\x92")); // U+2192 RIGHTWARDS ARROW
    const juce::String text = labelText ? labelText()
                                        : juce::String (ready ? "Drag MIDI out  " + arrow
                                                              : "Drag MIDI out (no take)");
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
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
    setResizeLimits (480, 560, 940, 1180);
    setSize (580, 866);

    auto& params = audioProcessor.getParameters();

    titleLabel.setText ("OrchCapture", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    styleLabel (titleLabel, 26.0f, true);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Post-chain MIDI take recorder", juce::dontSendNotification);
    subtitleLabel.setJustificationType (juce::Justification::centred);
    styleLabel (subtitleLabel, 13.0f);
    addAndMakeVisible (subtitleLabel);

    buildLabel.setText ("Build: Phase 2 (coordinator)", juce::dontSendNotification);
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

    tapRoleLabel.setText ("Tap Role", juce::dontSendNotification);
    styleLabel (tapRoleLabel, 13.0f, true);
    addAndMakeVisible (tapRoleLabel);
    tapRoleBox.addItemList ({ "Performance (tail)", "Articulation (pre-Mapper)" }, 1);
    tapRoleBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour::fromRGB (28, 36, 40));
    tapRoleBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    tapRoleBox.setColour (juce::ComboBox::outlineColourId, kAccent);
    addAndMakeVisible (tapRoleBox);
    tapRoleAttachment = std::make_unique<ComboBoxAttachment> (params, "tapRole", tapRoleBox);
    tapRoleBox.onChange = [this] { layoutRoleControls(); };

    ksZoneLabel.setText ("KS Zone (min / max)", juce::dontSendNotification);
    styleLabel (ksZoneLabel, 13.0f, true);
    addAndMakeVisible (ksZoneLabel);

    for (auto* s : { &ksZoneMinSlider, &ksZoneMaxSlider })
    {
        s->setSliderStyle (juce::Slider::IncDecButtons);
        s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 46, 22);
        s->setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour::fromRGB (28, 36, 40));
        s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colour::fromRGB (70, 85, 90));
        addAndMakeVisible (*s);
    }
    ksZoneMinAttachment = std::make_unique<SliderAttachment> (params, "ksZoneMin", ksZoneMinSlider);
    ksZoneMaxAttachment = std::make_unique<SliderAttachment> (params, "ksZoneMax", ksZoneMaxSlider);

    ksExportLabel.setText ("KS Export", juce::dontSendNotification);
    styleLabel (ksExportLabel, 13.0f, true);
    addAndMakeVisible (ksExportLabel);
    ksExportBox.addItemList ({ "Inline", "Separate Track", "Exclude", "KS Only" }, 1);
    ksExportBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour::fromRGB (28, 36, 40));
    ksExportBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    ksExportBox.setColour (juce::ComboBox::outlineColourId, kAccent);
    addAndMakeVisible (ksExportBox);
    ksExportAttachment = std::make_unique<ComboBoxAttachment> (params, "ksExportMode", ksExportBox);
    ksExportBox.onChange = [this] { layoutRoleControls(); };

    const auto styleCombo = [] (juce::ComboBox& b)
    {
        b.setColour (juce::ComboBox::backgroundColourId, juce::Colour::fromRGB (28, 36, 40));
        b.setColour (juce::ComboBox::textColourId, juce::Colours::white);
        b.setColour (juce::ComboBox::outlineColourId, kAccent);
    };

    quantizeLabel.setText ("Quantize", juce::dontSendNotification);
    styleLabel (quantizeLabel, 13.0f, true);
    addAndMakeVisible (quantizeLabel);
    quantizeBox.addItemList ({ "Off", "1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32" }, 1);
    styleCombo (quantizeBox);
    addAndMakeVisible (quantizeBox);
    quantizeAttachment = std::make_unique<ComboBoxAttachment> (params, "quantizeGrid", quantizeBox);

    coordinatorButton.setButtonText ("Coordinator (rig export hub, port 47826)");
    coordinatorButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (coordinatorButton);
    coordinatorAttachment = std::make_unique<ButtonAttachment> (params, "coordinator", coordinatorButton);
    coordinatorButton.onClick = [this] { resized(); };

    mergedContentLabel.setText ("Merged Content", juce::dontSendNotification);
    styleLabel (mergedContentLabel, 13.0f, true);
    addAndMakeVisible (mergedContentLabel);
    mergedContentBox.addItemList ({ "Notes + KS", "Notes only", "KS only" }, 1);
    styleCombo (mergedContentBox);
    addAndMakeVisible (mergedContentBox);
    mergedContentAttachment = std::make_unique<ComboBoxAttachment> (params, "mergedContent", mergedContentBox);

    autoSaveButton.setButtonText ("Auto-save merged .mid on transport stop");
    autoSaveButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (autoSaveButton);
    autoSaveAttachment = std::make_unique<ButtonAttachment> (params, "autoSaveOnStop", autoSaveButton);

    addAndMakeVisible (autoSaveFolderButton);
    autoSaveFolderButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Auto-save folder",
                                                           juce::File (audioProcessor.getAutoSaveFolder()));
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (dir.isDirectory())
            {
                audioProcessor.setAutoSaveFolder (dir.getFullPathName());
                autoSaveFolderLabel.setText (dir.getFullPathName(), juce::dontSendNotification);
            }
        });
    };

    autoSaveFolderLabel.setJustificationType (juce::Justification::centredLeft);
    autoSaveFolderLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (150, 200, 175));
    autoSaveFolderLabel.setFont (juce::FontOptions (11.0f));
    autoSaveFolderLabel.setText (audioProcessor.getAutoSaveFolder().isNotEmpty()
                                     ? audioProcessor.getAutoSaveFolder()
                                     : juce::String ("(no folder set - auto-save is inactive)"),
                                 juce::dontSendNotification);
    addAndMakeVisible (autoSaveFolderLabel);

    const auto styleTextEditor = [this] (juce::TextEditor& te, const juce::String& placeholder)
    {
        te.setMultiLine (true, false);
        te.setReturnKeyStartsNewLine (true);
        te.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (24, 30, 34));
        te.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        te.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (60, 74, 70));
        te.setFont (juce::FontOptions (12.0f));
        te.setTextToShowWhenEmpty (placeholder, juce::Colour::fromRGB (110, 120, 118));
        addAndMakeVisible (te);
    };

    markersLabel.setText ("Section markers  (bar:label, ... - bar is 1-indexed)", juce::dontSendNotification);
    styleLabel (markersLabel, 12.0f);
    addAndMakeVisible (markersLabel);
    styleTextEditor (markersEditor, "1:Intro, 9:Test 1");
    markersEditor.setText (audioProcessor.getMarkersText(), juce::dontSendNotification);
    markersEditor.onFocusLost = [this] { audioProcessor.setMarkersText (markersEditor.getText()); };

    tempoLabel.setText ("Tempo marks  (bar:bpm, ... - not read from Bitwig)", juce::dontSendNotification);
    styleLabel (tempoLabel, 12.0f);
    addAndMakeVisible (tempoLabel);
    styleTextEditor (tempoEditor, "1:58, 9:72");
    tempoEditor.setText (audioProcessor.getTempoText(), juce::dontSendNotification);
    tempoEditor.onFocusLost = [this] { audioProcessor.setTempoText (tempoEditor.getText()); };

    scoreOrderLabel.setText ("Score order  (track names, comma / newline)", juce::dontSendNotification);
    styleLabel (scoreOrderLabel, 12.0f);
    addAndMakeVisible (scoreOrderLabel);
    styleTextEditor (scoreOrderEditor, "Piccolo, Flute 1, Oboe 1, ...");
    scoreOrderEditor.setText (audioProcessor.getScoreOrderText(), juce::dontSendNotification);
    scoreOrderEditor.onFocusLost = [this] { audioProcessor.setScoreOrderText (scoreOrderEditor.getText()); };

    coordinatorStatusLabel.setJustificationType (juce::Justification::centredLeft);
    coordinatorStatusLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (150, 200, 175));
    coordinatorStatusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (coordinatorStatusLabel);

    laneList.setColour (juce::ListBox::backgroundColourId, juce::Colour::fromRGB (22, 28, 32));
    laneList.setColour (juce::ListBox::outlineColourId, juce::Colour::fromRGB (60, 74, 70));
    laneList.setOutlineThickness (1);
    laneList.setRowHeight (20);
    addAndMakeVisible (laneList);

    trackNameLabel.setJustificationType (juce::Justification::centredLeft);
    styleLabel (trackNameLabel, 14.0f, true);
    addAndMakeVisible (trackNameLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (245, 205, 120));
    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);

    dragPad.onRequestFile = [this]
    {
        return coordinatorActive() ? writeMergedToTempFile() : writeTakeToTempFile();
    };
    dragPad.canDrag = [this]
    {
        if (audioProcessor.isTransportPlayingForUi())
            return false;
        if (coordinatorActive())
        {
            const auto muted = excludedUids();
            for (const auto& row : laneRows)
                if (row.haveTake && muted.count (row.uid) == 0)
                    return true;
            return false;
        }
        return audioProcessor.getTakeNoteCountForUi() > 0;
    };
    dragPad.labelText = [this] () -> juce::String
    {
        const juce::String arrow (juce::CharPointer_UTF8 ("\xe2\x86\x92"));
        const bool ready = dragPad.canDrag && dragPad.canDrag();
        const bool all = coordinatorActive();
        const juce::String noun = all ? "ALL MIDI" : "MIDI";

        if (ready)
            return "Drag " + noun + " out  " + arrow;
        if (audioProcessor.isTransportPlayingForUi())
            return "Drag " + noun + " out (stop the transport)";
        return all ? "Drag ALL MIDI out (no takes yet)" : "Drag MIDI out (no take)";
    };
    addAndMakeVisible (dragPad);

    saveButton.onClick = [this] { saveToFolder(); };
    addAndMakeVisible (saveButton);

    clearButton.onClick = [this] { audioProcessor.clearTake(); };
    addAndMakeVisible (clearButton);

    layoutRoleControls();
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
    const bool coord = audioProcessor.isCoordinatorParamOn();
    lastCoordinatorVisible = coord;

    juce::Component* coordWidgets[] {
        &mergedContentLabel, &mergedContentBox, &autoSaveButton, &autoSaveFolderButton,
        &autoSaveFolderLabel, &markersLabel, &markersEditor,
        &tempoLabel, &tempoEditor, &scoreOrderLabel, &scoreOrderEditor, &laneList
    };
    for (auto* c : coordWidgets)
        c->setVisible (coord);

    auto area = getLocalBounds().reduced (22, 16);

    titleLabel.setBounds (area.removeFromTop (32));
    subtitleLabel.setBounds (area.removeFromTop (18));
    buildLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (10);

    trackNameLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (3);
    statusLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (12);

    enableButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (2);
    resetOnPlayButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (10);

    const auto labelledRow = [&area] (juce::Component& label, juce::Component& field, int fieldW)
    {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (130));
        field.setBounds (r.removeFromLeft (fieldW));
        area.removeFromTop (6);
    };

    {
        auto rowR = area.removeFromTop (26);
        tapRoleLabel.setBounds (rowR.removeFromLeft (130));
        tapRoleBox.setBounds (rowR);
        area.removeFromTop (6);
    }
    {
        auto rowZ = area.removeFromTop (26);
        ksZoneLabel.setBounds (rowZ.removeFromLeft (130));
        ksZoneMinSlider.setBounds (rowZ.removeFromLeft (100));
        rowZ.removeFromLeft (10);
        ksZoneMaxSlider.setBounds (rowZ.removeFromLeft (100));
        area.removeFromTop (6);
    }
    labelledRow (ksExportLabel, ksExportBox, 180);
    labelledRow (quantizeLabel, quantizeBox, 110);
    area.removeFromTop (6);

    coordinatorButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (2);
    coordinatorStatusLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (6);

    // Bottom-anchored: drag pad + button row.
    auto row = area.removeFromBottom (30);
    saveButton.setBounds (row.removeFromLeft (row.getWidth() * 2 / 3).reduced (0, 2));
    row.removeFromLeft (8);
    clearButton.setBounds (row.reduced (0, 2));
    area.removeFromBottom (10);
    dragPad.setBounds (area.removeFromBottom (48));
    area.removeFromBottom (10);

    if (coord)
    {
        labelledRow (mergedContentLabel, mergedContentBox, 150);
        {
            auto rowA = area.removeFromTop (24);
            autoSaveButton.setBounds (rowA.removeFromLeft (rowA.getWidth() - 140));
            autoSaveFolderButton.setBounds (rowA.reduced (0, 1));
        }
        autoSaveFolderLabel.setBounds (area.removeFromTop (16));
        area.removeFromTop (8);
        markersLabel.setBounds (area.removeFromTop (16));
        markersEditor.setBounds (area.removeFromTop (34));
        area.removeFromTop (5);
        tempoLabel.setBounds (area.removeFromTop (16));
        tempoEditor.setBounds (area.removeFromTop (34));
        area.removeFromTop (5);
        scoreOrderLabel.setBounds (area.removeFromTop (16));
        scoreOrderEditor.setBounds (area.removeFromTop (34));
        area.removeFromTop (8);
        laneList.setBounds (area);
    }
}

void OrchCaptureAudioProcessorEditor::layoutRoleControls()
{
    const bool articulation = audioProcessor.getTapRoleForUi() == 1;

    subtitleLabel.setText (articulation ? "Unified keyswitch stream (pre-Mapper tap)"
                                        : "Post-chain MIDI take recorder (notation source)",
                           juce::dontSendNotification);

    const bool ksZoneInPlay = articulation || ksExportBox.getSelectedItemIndex() > 0;
    ksZoneLabel.setEnabled (ksZoneInPlay);
    ksZoneMinSlider.setEnabled (ksZoneInPlay);
    ksZoneMaxSlider.setEnabled (ksZoneInPlay);
}

bool OrchCaptureAudioProcessorEditor::coordinatorActive() const
{
    return audioProcessor.getLink().getMode() == OrchCaptureLink::Mode::Coordinator;
}

std::set<juce::String> OrchCaptureAudioProcessorEditor::excludedUids() const
{
    return mutedLaneUids;
}

std::vector<ocap::TakeForExport> OrchCaptureAudioProcessorEditor::collectFilteredTakes() const
{
    return audioProcessor.getLink().collectTakesForExport (mutedLaneUids);
}

void OrchCaptureAudioProcessorEditor::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int> (laneRows.size()))
        return;

    const auto uid = laneRows[static_cast<size_t> (row)].uid;
    if (uid.isEmpty())
        return;

    if (mutedLaneUids.count (uid) != 0)
        mutedLaneUids.erase (uid);
    else
        mutedLaneUids.insert (uid);

    laneList.repaint();
    dragPad.repaint();
}

void OrchCaptureAudioProcessorEditor::timerCallback()
{
    updateStatus();

    auto& link = audioProcessor.getLink();
    const auto linkMode = link.getMode();

    if (audioProcessor.isCoordinatorParamOn() != lastCoordinatorVisible)
        resized();

    laneRows = link.getLaneRows();
    laneList.updateContent();
    laneList.repaint();

    juce::String coordText;
    switch (linkMode)
    {
        case OrchCaptureLink::Mode::Coordinator:
        {
            int withTakes = 0;
            for (const auto& r : laneRows)
                if (r.haveTake)
                    ++withTakes;
            coordText << "Coordinator active  |  " << (int) laneRows.size() << " lanes  |  "
                      << withTakes << " with a take";

            const int saves = audioProcessor.getAutoSaveCountForUi();
            if (saves != lastAutoSaveCount)
            {
                lastAutoSaveCount = saves;
                statusLabel.setText ("Auto-saved " + audioProcessor.getLastAutoSaveNameForUi(),
                                     juce::dontSendNotification);
            }
            if (saves > 0)
                coordText << "  |  auto-saved " << saves << (saves == 1 ? " file" : " files");
            break;
        }
        case OrchCaptureLink::Mode::CoordinatorPortBusy:
            coordText = "Coordinator: port 47826 already held by another instance - running as a client";
            break;
        case OrchCaptureLink::Mode::Client:
            coordText = link.isClientConnected() ? "Client: connected to the coordinator"
                                                 : "Client: no coordinator on the rig yet";
            break;
    }
    coordinatorStatusLabel.setText (coordText, juce::dontSendNotification);
    dragPad.repaint();
}

int OrchCaptureAudioProcessorEditor::getNumRows()
{
    return static_cast<int> (laneRows.size());
}

void OrchCaptureAudioProcessorEditor::paintListBoxItem (int row, juce::Graphics& g,
                                                        int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int> (laneRows.size()))
        return;

    const auto& r = laneRows[static_cast<size_t> (row)];
    const bool muted = mutedLaneUids.count (r.uid) != 0;

    g.setColour (row % 2 == 0 ? juce::Colour::fromRGB (24, 30, 34)
                              : juce::Colour::fromRGB (20, 26, 30));
    g.fillRect (0, 0, width, height);

    if (r.playing && ! muted)
    {
        g.setColour (juce::Colour::fromRGB (245, 205, 120));
        g.fillEllipse (6.0f, height * 0.5f - 3.0f, 6.0f, 6.0f);
    }

    const auto textColour = muted ? juce::Colour::fromRGB (90, 98, 96)
                                  : (r.haveTake ? juce::Colours::white
                                                : juce::Colour::fromRGB (120, 130, 128));
    g.setColour (textColour);
    g.setFont (juce::FontOptions (12.0f));

    juce::String line;
    line << (r.trackName.isNotEmpty() ? r.trackName : juce::String ("(unnamed)"))
         << "  \xc2\xb7  " << (r.role == 1 ? "Artic" : "Perf")
         << (r.isLocal ? "  (this)" : "")
         << "   " << r.noteCount << "n";
    if (r.keyswitchCount > 0)
        line << " / " << r.keyswitchCount << "ks";
    line << "   " << juce::String (r.lengthPpq / 4.0, 1) << "b";
    if (muted)
        line << "   - excluded";

    g.drawText (line, 18, 0, width - 22, height, juce::Justification::centredLeft);

    if (muted)
    {
        g.setColour (textColour);
        const float y = height * 0.5f;
        g.drawLine (18.0f, y, (float) width - 8.0f, y, 1.0f);
    }
}

void OrchCaptureAudioProcessorEditor::updateStatus()
{
    const auto name = audioProcessor.getTrackNameForUi();
    trackNameLabel.setText ("Track: " + name
                                + (audioProcessor.hasHostTrackNameForUi() ? "" : "  (host sent no name)"),
                            juce::dontSendNotification);

    const int count = audioProcessor.getTakeNoteCountForUi();
    const int ksCount = audioProcessor.getTakeKeyswitchCountForUi();
    const double lengthPpq = audioProcessor.getTakeLengthQuarterNotesForUi();
    const double bars = lengthPpq / 4.0; // 4/4 assumed for the readout
    const bool playing = audioProcessor.isTransportPlayingForUi();

    juce::String text;
    text << "Take: " << count << (count == 1 ? " note" : " notes")
         << " (" << ksCount << " KS)"
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

juce::File OrchCaptureAudioProcessorEditor::writeMergedToTempFile()
{
    if (audioProcessor.isTransportPlayingForUi())
        return {};

    const auto takes = collectFilteredTakes();
    if (takes.empty())
        return {};

    const auto file = tempMidiPath ("session");

    juce::FileOutputStream stream (file);
    if (! stream.openedOk())
        return {};

    ocap::writeMergedTakeMidi (takes, audioProcessor.buildMergedExportOptions(), stream);
    stream.flush();
    return file;
}

void OrchCaptureAudioProcessorEditor::saveToFolder()
{
    const bool merged = coordinatorActive();

    if (audioProcessor.isTransportPlayingForUi())
    {
        statusLabel.setText ("Stop the transport first", juce::dontSendNotification);
        return;
    }

    const bool haveSomething = merged ? ! collectFilteredTakes().empty()
                                      : audioProcessor.getTakeNoteCountForUi() > 0;
    if (! haveSomething)
    {
        statusLabel.setText (merged ? "No lane has a take yet" : "Record a take first",
                             juce::dontSendNotification);
        return;
    }

    const auto stem = merged ? juce::String ("OrchCapture session")
                             : audioProcessor.buildExportOptions().trackName;
    const auto suggested = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile (juce::File::createLegalFileName (stem) + ".mid");

    fileChooser = std::make_unique<juce::FileChooser> (merged ? "Save merged rig MIDI" : "Save take as MIDI",
                                                       suggested, "*.mid");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    fileChooser->launchAsync (chooserFlags, [this, merged] (const juce::FileChooser& fc)
    {
        const auto target = fc.getResult();
        if (target == juce::File())
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

        if (merged)
        {
            const auto takes = collectFilteredTakes();
            if (takes.empty())
                return;
            ocap::writeMergedTakeMidi (takes, audioProcessor.buildMergedExportOptions(), stream);
        }
        else
        {
            const auto take = audioProcessor.snapshotTake();
            if (take.empty())
                return;
            ocap::writeTakeMidi (take, audioProcessor.buildExportOptions(), stream);
        }

        stream.flush();
        statusLabel.setText ("Saved " + file.getFileName(), juce::dontSendNotification);
    });
}

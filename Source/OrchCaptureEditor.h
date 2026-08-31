#pragma once

#include <set>
#include <vector>
#include <JuceHeader.h>
#include "OrchCaptureProcessor.h"
#include "OrchCaptureLink.h"

class OrchCaptureAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::DragAndDropContainer,
                                        private juce::Timer,
                                        private juce::ListBoxModel
{
public:
    explicit OrchCaptureAudioProcessorEditor (OrchCaptureAudioProcessor&);
    ~OrchCaptureAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateStatus();
    void layoutRoleControls();

    // ListBoxModel - the coordinator's lane list.
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    bool coordinatorActive() const;
    std::set<juce::String> excludedUids() const;
    std::vector<ocap::TakeForExport> collectFilteredTakes() const;

    // Writes the current take (or, on the coordinator, the merged rig take) to a
    // temp .mid and returns it, or an invalid File{} if there is nothing to
    // export / the transport is still playing.
    juce::File writeTakeToTempFile();
    juce::File writeMergedToTempFile();
    void saveToFolder();

    // Small drag source: drag off it to drop the take's .mid onto a track.
    class DragPad : public juce::Component
    {
    public:
        std::function<juce::File()> onRequestFile;
        std::function<bool()> canDrag;
        std::function<juce::String()> labelText;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

    private:
        juce::Point<float> dragStart;
        bool dragFired = false;
    };

    OrchCaptureAudioProcessor& audioProcessor;

    juce::Label titleLabel, subtitleLabel, buildLabel;
    juce::ToggleButton enableButton;
    juce::ToggleButton resetOnPlayButton;

    juce::Label tapRoleLabel;
    juce::ComboBox tapRoleBox;
    juce::Label ksZoneLabel;
    juce::Slider ksZoneMinSlider, ksZoneMaxSlider;
    juce::Label ksExportLabel;
    juce::ComboBox ksExportBox;
    juce::Label quantizeLabel;
    juce::ComboBox quantizeBox;

    juce::ToggleButton coordinatorButton;
    juce::Label coordinatorStatusLabel;
    juce::ListBox laneList { "lanes", this };
    juce::Label mergedContentLabel;
    juce::ComboBox mergedContentBox;
    juce::ToggleButton autoSaveButton;
    juce::TextButton autoSaveFolderButton { "Auto-save folder..." };
    juce::Label autoSaveFolderLabel;
    int lastAutoSaveCount = 0;
    juce::Label markersLabel;
    juce::TextEditor markersEditor;
    juce::Label tempoLabel;
    juce::TextEditor tempoEditor;
    juce::Label scoreOrderLabel;
    juce::TextEditor scoreOrderEditor;

    std::set<juce::String> mutedLaneUids;
    bool lastCoordinatorVisible = false;

    juce::Label trackNameLabel;
    juce::Label statusLabel;

    DragPad dragPad;
    juce::TextButton saveButton { "Save .mid to folder..." };
    juce::TextButton clearButton { "Clear Take" };

    std::vector<OrchCaptureLink::LaneRow> laneRows;

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<ButtonAttachment> enableAttachment;
    std::unique_ptr<ButtonAttachment> resetOnPlayAttachment;
    std::unique_ptr<ComboBoxAttachment> tapRoleAttachment;
    std::unique_ptr<SliderAttachment> ksZoneMinAttachment;
    std::unique_ptr<SliderAttachment> ksZoneMaxAttachment;
    std::unique_ptr<ComboBoxAttachment> ksExportAttachment;
    std::unique_ptr<ComboBoxAttachment> quantizeAttachment;
    std::unique_ptr<ButtonAttachment> coordinatorAttachment;
    std::unique_ptr<ComboBoxAttachment> mergedContentAttachment;
    std::unique_ptr<ButtonAttachment> autoSaveAttachment;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchCaptureAudioProcessorEditor)
};

#pragma once

#include <JuceHeader.h>
#include "OrchCaptureProcessor.h"

class OrchCaptureAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::DragAndDropContainer,
                                        private juce::Timer
{
public:
    explicit OrchCaptureAudioProcessorEditor (OrchCaptureAudioProcessor&);
    ~OrchCaptureAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateStatus();

    // Writes the current take to a temp .mid and returns it, or an invalid
    // File{} if there is nothing to export / the transport is still playing.
    juce::File writeTakeToTempFile();
    void saveTakeToFolder();

    // Small drag source: drag off it to drop the take's .mid onto a track.
    class DragPad : public juce::Component
    {
    public:
        std::function<juce::File()> onRequestFile;
        std::function<bool()> canDrag;

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

    juce::Label trackNameLabel;
    juce::Label statusLabel;

    DragPad dragPad;
    juce::TextButton saveButton { "Save .mid to folder..." };
    juce::TextButton clearButton { "Clear Take" };

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<ButtonAttachment> enableAttachment;
    std::unique_ptr<ButtonAttachment> resetOnPlayAttachment;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchCaptureAudioProcessorEditor)
};

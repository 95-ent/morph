#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// DropZoneComponent — fixed 80px strip at the top of the main UI.
// Handles drag-and-drop of audio files and folder import via FileChooser.
//==============================================================================
class DropZoneComponent  : public juce::Component,
                            public juce::FileDragAndDropTarget
{
public:
    explicit DropZoneComponent (MorphAudioProcessor& processor);
    ~DropZoneComponent() override = default;

    // Called after all queued uploads complete; notify = success / fail
    std::function<void()> onUploadComplete;

    //==========================================================================
    // juce::Component
    //==========================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

    //==========================================================================
    // juce::FileDragAndDropTarget
    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;

private:
    MorphAudioProcessor& proc;

    enum class State { Idle, DragOver, Uploading, Success };
    State currentState { State::Idle };

    int uploadedCount { 0 };
    int totalCount    { 0 };

    juce::TextButton importFolderButton;
    juce::Timer*     flashTimer { nullptr };

    void uploadFiles (const juce::Array<juce::File>& files);
    void uploadNext  (juce::Array<juce::File> files, int index);

    void setState (State s);

    static bool isAudioFile (const juce::File& f);
    static juce::Array<juce::File> collectAudioFiles (const juce::File& folder);

    static constexpr uint32_t kTeal   = 0xff1A90A0;
    static constexpr uint32_t kBG     = 0xff1A1A1A;
    static constexpr uint32_t kText   = 0xffF5F5F0;
    static constexpr uint32_t kGreen  = 0xff2ecc71;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DropZoneComponent)
};

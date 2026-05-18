#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// MorphAudioProcessorEditor — minimal bridge UI.
//
// One purpose: show Track / Project mode toggle, live BPM + Key, and
// companion connection state. The Water companion app owns the library.
//==============================================================================
class MorphAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit MorphAudioProcessorEditor (MorphAudioProcessor& p);
    ~MorphAudioProcessorEditor() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    MorphAudioProcessor& audioProcessor;

    // Hit rects computed in resized() / paint()
    juce::Rectangle<float> toggleRect, keyPickRect, helpRect_;

    void timerCallback() override;
    int  syncTick      { 0 };
    bool lastPlayState { false };
    bool isMorphed_    { false };
    int  morphPollTick_{ 0 };

    // Brand palette
    static constexpr uint32_t kTeal   = 0xff1A90A0;   // Water teal — KEY
    static constexpr uint32_t kMauve  = 0xff8B5CF6;   // Morph mauve — BPM / magic
    static constexpr uint32_t kMauve2 = 0xff6D28D9;   // darker mauve for gradient end
    static constexpr uint32_t kText   = 0xffE8EAF0;
    static constexpr uint32_t kDim    = 0xff4a5568;

    std::unique_ptr<juce::Drawable> waterLogo_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphAudioProcessorEditor)
};

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// LoginComponent — centered card overlay displayed when the user is not
// authenticated.  On success it calls onLoginSuccess so the editor can
// swap this component out for the main UI.
//==============================================================================
class LoginComponent  : public juce::Component
{
public:
    explicit LoginComponent (MorphAudioProcessor& processor);
    ~LoginComponent() override = default;

    //==========================================================================
    // Set this to be notified when login succeeds (called on message thread)
    //==========================================================================
    std::function<void()> onLoginSuccess;

    //==========================================================================
    // juce::Component overrides
    //==========================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    MorphAudioProcessor& proc;

    juce::Label      logoLabel;
    juce::Label      emailLabel;
    juce::TextEditor emailField;
    juce::Label      passwordLabel;
    juce::TextEditor passwordField;
    juce::TextButton signInButton;
    juce::TextButton oauthButton;    // "Connect with Water" browser OAuth
    juce::Label      orLabel;        // "or"
    juce::Label      errorLabel;

    bool busy { false };

    void attemptLogin();
    void startOAuth();
    void setError (const juce::String& msg);
    void setBusy (bool b);

    // Water brand colours
    static constexpr uint32_t kBG       = 0xff1A1A1A;
    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kText     = 0xffF5F5F0;
    static constexpr uint32_t kBorder   = 0x1Affffff;  // rgba(255,255,255,0.1)
    static constexpr uint32_t kCardBG   = 0xff222222;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoginComponent)
};

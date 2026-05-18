#include "LoginComponent.h"

//==============================================================================
LoginComponent::LoginComponent (MorphAudioProcessor& processor)
    : proc (processor)
{
    // Logo label
    logoLabel.setText ("Water  Morph", juce::dontSendNotification);
    logoLabel.setJustificationType (juce::Justification::centred);
    logoLabel.setFont (juce::Font (22.0f, juce::Font::bold));
    logoLabel.setColour (juce::Label::textColourId, juce::Colour (kTeal));
    addAndMakeVisible (logoLabel);

    // Email
    emailLabel.setText ("Email", juce::dontSendNotification);
    emailLabel.setFont (juce::Font (12.0f));
    emailLabel.setColour (juce::Label::textColourId, juce::Colour (kText).withAlpha (0.6f));
    addAndMakeVisible (emailLabel);

    emailField.setInputRestrictions (256);
    emailField.setReturnKeyStartsNewLine (false);
    emailField.setTextToShowWhenEmpty ("you@email.com", juce::Colour (kText).withAlpha (0.3f));
    emailField.setColour (juce::TextEditor::backgroundColourId,  juce::Colour (0xff2a2a2a));
    emailField.setColour (juce::TextEditor::textColourId,        juce::Colour (kText));
    emailField.setColour (juce::TextEditor::outlineColourId,     juce::Colour (kBorder));
    emailField.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (kTeal));
    addAndMakeVisible (emailField);

    // Password
    passwordLabel.setText ("Password", juce::dontSendNotification);
    passwordLabel.setFont (juce::Font (12.0f));
    passwordLabel.setColour (juce::Label::textColourId, juce::Colour (kText).withAlpha (0.6f));
    addAndMakeVisible (passwordLabel);

    passwordField.setPasswordCharacter (0x2022);  // bullet
    passwordField.setInputRestrictions (128);
    passwordField.setReturnKeyStartsNewLine (false);
    passwordField.setTextToShowWhenEmpty ("password", juce::Colour (kText).withAlpha (0.3f));
    passwordField.setColour (juce::TextEditor::backgroundColourId,  juce::Colour (0xff2a2a2a));
    passwordField.setColour (juce::TextEditor::textColourId,        juce::Colour (kText));
    passwordField.setColour (juce::TextEditor::outlineColourId,     juce::Colour (kBorder));
    passwordField.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (kTeal));
    passwordField.onReturnKey = [this] { attemptLogin(); };
    addAndMakeVisible (passwordField);

    // Sign In button
    signInButton.setButtonText ("Sign In");
    signInButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (kTeal));
    signInButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (kText));
    signInButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (kText));
    signInButton.onClick = [this] { attemptLogin(); };
    addAndMakeVisible (signInButton);

    // "or" divider
    orLabel.setText ("or", juce::dontSendNotification);
    orLabel.setFont (juce::Font (11.0f));
    orLabel.setColour (juce::Label::textColourId, juce::Colour (kText).withAlpha (0.3f));
    orLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (orLabel);

    // "Connect with Water" — OAuth browser flow
    oauthButton.setButtonText ("Connect with Water");
    oauthButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff2a2a2a));
    oauthButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (kTeal));
    oauthButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (kTeal));
    oauthButton.onClick = [this] { startOAuth(); };
    addAndMakeVisible (oauthButton);

    // Error label (hidden until needed)
    errorLabel.setFont (juce::Font (11.0f));
    errorLabel.setColour (juce::Label::textColourId, juce::Colours::tomato);
    errorLabel.setJustificationType (juce::Justification::centred);
    errorLabel.setVisible (false);
    addAndMakeVisible (errorLabel);
}

//==============================================================================
void LoginComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBG));

    auto card = getLocalBounds()
                    .withSizeKeepingCentre (300, 360)
                    .toFloat();

    g.setColour (juce::Colour (kCardBG));
    g.fillRoundedRectangle (card, 10.0f);

    g.setColour (juce::Colour (kBorder));
    g.drawRoundedRectangle (card, 10.0f, 1.0f);
}

//==============================================================================
void LoginComponent::resized()
{
    auto card = getLocalBounds().withSizeKeepingCentre (300, 360);
    auto area = card.reduced (24);

    logoLabel.setBounds (area.removeFromTop (32));
    area.removeFromTop (16);

    emailLabel.setBounds (area.removeFromTop (16));
    emailField.setBounds (area.removeFromTop (32));
    area.removeFromTop (10);

    passwordLabel.setBounds (area.removeFromTop (16));
    passwordField.setBounds (area.removeFromTop (32));
    area.removeFromTop (16);

    signInButton.setBounds (area.removeFromTop (36));
    area.removeFromTop (8);

    errorLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);

    orLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    oauthButton.setBounds (area.removeFromTop (36));
}

//==============================================================================
void LoginComponent::attemptLogin()
{
    if (busy) return;

    juce::String email    = emailField.getText().trim();
    juce::String password = passwordField.getText();

    if (email.isEmpty() || password.isEmpty())
    {
        setError ("Please enter your email and password.");
        return;
    }

    setBusy (true);
    errorLabel.setVisible (false);

    proc.login (email, password,
        [this] (bool ok, juce::String errMsg)
        {
            setBusy (false);
            if (ok)
            {
                if (onLoginSuccess) onLoginSuccess();
            }
            else
            {
                setError (errMsg.isEmpty() ? "Sign in failed. Please try again." : errMsg);
            }
        });
}

void LoginComponent::startOAuth()
{
    if (busy) return;
    setBusy (true);
    errorLabel.setVisible (false);
    oauthButton.setButtonText ("Opening browser...");

    proc.startOAuthFlow ([this] (bool ok, juce::String errMsg)
    {
        setBusy (false);
        if (ok)
        {
            if (onLoginSuccess) onLoginSuccess();
        }
        else
        {
            oauthButton.setButtonText ("Connect with Water");
            setError (errMsg.isEmpty() ? "Authorization failed. Please try again." : errMsg);
        }
    });
}

void LoginComponent::setError (const juce::String& msg)
{
    errorLabel.setText (msg, juce::dontSendNotification);
    errorLabel.setVisible (true);
}

void LoginComponent::setBusy (bool b)
{
    busy = b;
    signInButton.setEnabled (!b);
    oauthButton.setEnabled (!b);
    signInButton.setButtonText (b ? "Signing in..." : "Sign In");
}

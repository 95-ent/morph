#include "PluginEditor.h"
#include "CompanionLink.h"

static constexpr int kPlugW = 380;
static constexpr int kPlugH = 240;

//==============================================================================
MorphAudioProcessorEditor::MorphAudioProcessorEditor (MorphAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setResizable (false, false);
    setSize (kPlugW, kPlugH);
    startTimer (200);

    // Exact SVG path from the web app logo.tsx (viewBox 100 260 660 330).
    // parseSVGPath returns it in source coordinates; we transform in paint().
    static const juce::String kLogoPath {
        "M546.7,571.21c-20.04-1.97-34.8-11.88-44.82-29.5"
        "-9.91-17.42-15.33-36.35-19.99-55.61-4.92-20.35-9.69-40.74-14.97-61"
        "-6.4-24.59-13.03-49.13-20.07-73.54-2.18-7.55-5.1-15.23-9.33-21.78"
        "-7.14-11.05-21.93-13.79-30.37-1.48-6.33,9.23-10.81,20.12-14.25,30.86"
        "-6.28,19.61-11.51,39.58-16.55,59.56-4.88,19.35-8.78,38.94-13.47,58.34"
        "-3.33,13.78-6.42,27.71-11.13,41.04-4.78,13.5-10.56,26.82-20.81,37.49"
        "-11.24,11.69-24.87,16.1-40.91,14.62-15.23-1.4-25.32-10.84-33.75-21.89"
        "-14.02-18.39-19.48-40.62-24.98-62.53-7.36-29.34-13.93-58.88-20.77-88.35"
        "-4.8-20.65-10.46-41.05-19.66-60.19-2.53-5.27-6.92-9.94-11.32-13.93"
        "-4.33-3.93-10.01-3.43-15.25-1.1-6.27,2.79-15.49-3.19-17.19-11.27"
        "-1.43-6.78,2.31-12.46,8.15-16.35,8.34-5.56,17.29-6.09,26.21-4.2"
        ",11.45,2.42,20.75,8.92,28.25,18.18,13.5,16.67,19.87,36.51,25.06,56.68"
        ",8.26,32.11,15.18,64.56,23.49,96.65,5.11,19.74,11.55,39.14,17.85,58.55"
        ",1.44,4.43,4.38,8.52,7.25,12.3,8.82,11.61,23.64,7.13,28.69-3.38"
        ",5.06-10.53,9.63-21.48,12.71-32.72,6.14-22.41,11.06-45.16,16.58-67.75"
        ",5.52-22.57,10.37-45.34,17.02-67.58,4.42-14.78,10.27-29.44,17.71-42.94"
        ",7.32-13.28,18.36-23.86,34.23-27.41,17.38-3.89,35.53,.14,47.63,14.78"
        ",9.6,11.61,16.15,24.87,20.38,39.31,5.04,17.22,10.69,34.28,15.1,51.65"
        ",5.1,20.07,8.82,40.48,13.75,60.6,4.14,16.88,8.68,33.69,13.97,50.24"
        ",2.64,8.27,6.53,16.41,11.31,23.66,7.12,10.81,20.49,11.21,27.73,.51"
        ",5.73-8.48,10.43-18.08,13.5-27.83,6.01-19.08,10.9-38.53,15.6-57.99"
        ",3.16-13.09,5.22-26.45,7.72-39.69,.51-2.71,6.53-4.71,9.43-2.95"
        ",2.38,1.45,4.7,3.01,6.97,4.63,4.93,3.52,7.95,8.1,8.97,14.28"
        ",2.01,12.07-1.45,23.47-3.68,35.03-1.75,9.07-3.14,18.29-5.8,27.11"
        "-7.36,24.49-13.41,49.59-30.08,70.14-6.71,8.28-24.37,16.56-36.12,16.77Z"
    };
    {
        // Store raw path (source coords 100–760 x, 260–590 y).
        // drawWithin() in paint() scales it to the header slot.
        juce::Path raw = juce::Drawable::parseSVGPath (kLogoPath);
        waterLogo_ = std::make_unique<juce::DrawablePath>();
        static_cast<juce::DrawablePath*> (waterLogo_.get())->setPath (raw);
        static_cast<juce::DrawablePath*> (waterLogo_.get())
            ->setFill (juce::FillType (juce::Colour (kTeal)));
    }
}

MorphAudioProcessorEditor::~MorphAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void MorphAudioProcessorEditor::resized()
{
    const float w    = (float) getWidth();
    const float togW = 160.f;
    const float togH = 26.f;
    const float togX = (w - togW) * 0.5f;
    const float togY = 46.f;

    toggleRect  = { togX, togY, togW, togH };
    keyPickRect = { w * 0.5f + 8.f, 84.f, w * 0.5f - 20.f, 56.f };
}

//==============================================================================
void MorphAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();

    // ── Background ───────────────────────────────────────────────────────────
    g.fillAll (juce::Colour (0xff050a0b));
    {
        juce::ColourGradient glow (juce::Colour (0xff091c24), 0.f, 0.f,
                                   juce::Colour (0xff050a0b), w * 0.6f, h * 0.5f, true);
        g.setGradientFill (glow);
        g.fillAll();
    }

    // ── Header — logo left, connection dot right ─────────────────────────────
    const bool connected = CompanionLink::get().isConnected();

    if (waterLogo_)
    {
        static_cast<juce::DrawablePath*> (waterLogo_.get())
            ->setFill (juce::FillType (juce::Colours::white.withAlpha (0.85f)));
        waterLogo_->drawWithin (g,
            juce::Rectangle<float> (14.f, 9.f, 20.f, 18.f),
            juce::RectanglePlacement::centred, 1.0f);
    }

    // Connection status — right-aligned dot + label
    {
        const float dotR  = 4.f;
        const float dotCX = w - 14.f - dotR;
        const float dotCY = 18.f;
        g.setColour (connected ? juce::Colour (0xff22c55e) : juce::Colour (0xffef4444));
        g.fillEllipse (dotCX - dotR, dotCY - dotR, dotR * 2.f, dotR * 2.f);

        g.setFont (juce::Font (10.f));
        g.setColour (connected ? juce::Colour (0xff86efac) : juce::Colour (0xfffca5a5));
        const juce::String connLabel = connected ? "Connected" : "Open Water";
        g.drawText (connLabel, 40, 9, (int)(w - 60.f), 18, juce::Justification::centred, false);

        helpRect_ = juce::Rectangle<float> (dotCX - dotR - 4.f, 6.f, dotR * 2.f + 28.f, 24.f);
    }

    // Hairline
    g.setColour (juce::Colour (0xff1a2028));
    g.fillRect (0.f, 34.f, w, 1.f);

    // ── Mode toggle ──────────────────────────────────────────────────────────
    using Mode = MorphAudioProcessor::ReferenceMode;
    const bool isTrack = audioProcessor.getReferenceMode() == Mode::kTrack;

    const float tr    = toggleRect.getX();
    const float ty    = toggleRect.getY();
    const float tw    = toggleRect.getWidth();
    const float th    = toggleRect.getHeight();
    const float halfW = tw * 0.5f;

    g.setColour (juce::Colour (0xff0c1318));
    g.fillRoundedRectangle (toggleRect, th * 0.5f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawRoundedRectangle (toggleRect, th * 0.5f, 1.f);

    // Active pill
    const float pillW = halfW - 3.f;
    const float pillH = th - 6.f;
    const float pillY = ty + 3.f;
    const float pillX = isTrack ? tr + halfW + 0.f : tr + 3.f;
    {
        juce::ColourGradient pg (juce::Colour (kTeal), pillX, pillY,
                                 juce::Colour (0xff0f7282), pillX + pillW, pillY, false);
        g.setGradientFill (pg);
        g.fillRoundedRectangle (pillX, pillY, pillW, pillH, pillH * 0.5f);
    }

    g.setFont (juce::Font (11.f, juce::Font::bold));
    g.setColour (!isTrack ? juce::Colours::white : juce::Colour (0xff4b5563));
    g.drawText ("Project", (int)tr, (int)ty, (int)halfW, (int)th, juce::Justification::centred, false);
    g.setColour (isTrack ? juce::Colours::white : juce::Colour (0xff4b5563));
    g.drawText ("Track",   (int)(tr + halfW), (int)ty, (int)halfW, (int)th, juce::Justification::centred, false);

    // ── BPM + KEY — big centred readout ──────────────────────────────────────
    const double       bpm    = audioProcessor.getEffectiveBPM();
    const juce::String key    = isTrack ? audioProcessor.getDetectedKey()
                                        : audioProcessor.getProjectKey();
    const bool         hasBpm = (bpm > 0.0);
    const bool         hasKey = (key.isNotEmpty() && key != "?");

    const juce::String bpmStr = hasBpm ? juce::String ((int)std::round (bpm)) : "--";
    const juce::String keyStr = hasKey ? key : "--";

    const juce::Colour valueCol = isMorphed_             ? juce::Colour (kMauve)
                                : (connected && hasBpm)   ? juce::Colour (0xfff0f4f8)
                                : hasBpm                  ? juce::Colour (kTeal)
                                :                           juce::Colour (0xff374151);

    // Vertical centre of the data zone (between toggle and status)
    const float zoneTop = ty + th + 8.f;
    const float zoneBot = h - 42.f;
    const float zoneMid = (zoneTop + zoneBot) * 0.5f;

    const float half = w * 0.5f;

    // BPM — right half of left panel
    g.setFont (juce::Font (38.f, juce::Font::bold));
    g.setColour (valueCol);
    g.drawText (bpmStr, 0, (int)(zoneMid - 24.f), (int)(half - 12.f), 48,
                juce::Justification::centredRight, false);
    g.setFont (juce::Font (9.5f));
    g.setColour (juce::Colour (0xff4b5563));
    g.drawText ("BPM", 0, (int)(zoneMid + 24.f), (int)(half - 12.f), 14,
                juce::Justification::centredRight, false);

    // Divider
    g.setColour (juce::Colour (0xff1e2530));
    g.fillRect (half - 0.5f, zoneMid - 20.f, 1.f, 40.f);

    // KEY — left half of right panel
    g.setFont (juce::Font (38.f, juce::Font::bold));
    g.setColour (valueCol);
    g.drawText (keyStr, (int)(half + 12.f), (int)(zoneMid - 24.f), (int)(half - 20.f), 48,
                juce::Justification::centredLeft, false);
    g.setFont (juce::Font (9.5f));
    g.setColour (juce::Colour (0xff4b5563));
    g.drawText (isTrack ? "KEY  auto" : "KEY  tap",
                (int)(half + 12.f), (int)(zoneMid + 24.f), (int)(half - 20.f), 14,
                juce::Justification::centredLeft, false);

    // Tap underline — Project mode only
    if (!isTrack && hasKey)
    {
        g.setColour (valueCol.withAlpha (0.22f));
        g.fillRect (half + 12.f, zoneMid + 23.f, 48.f, 1.f);
    }

    // ── Status strip ─────────────────────────────────────────────────────────
    {
        const float sy = h - 40.f;
        g.setColour (juce::Colour (0xff070d10));
        g.fillRect (0.f, sy, w, 40.f);
        g.setColour (juce::Colour (0xff1a2028));
        g.fillRect (0.f, sy, w, 1.f);

        juce::String msg;
        juce::Colour col = juce::Colour (0xff374151);

        if (audioProcessor.isAnalysing())
        {
            msg = juce::CharPointer_UTF8 ("\xe2\x96\xb6  Scanning\xe2\x80\xa6");
            col = juce::Colour (kTeal);
        }
        else if (isTrack)
        {
            const juce::String k = audioProcessor.getDetectedKey();
            if (k.isNotEmpty() && k != "?")
            {
                msg = juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93  ")) + k
                      + "  " + juce::String ((int)std::round (audioProcessor.getEffectiveBPM())) + " BPM";
                col = juce::Colour (0xff6b7280);
            }
            else
                msg = "Play audio to scan";
        }
        else
        {
            const juce::String k = audioProcessor.getProjectKey();
            msg = k.isEmpty() ? "Tap KEY to set project key" : juce::CharPointer_UTF8 ("\xe2\x9c\x93  Key locked");
            if (!k.isEmpty()) col = juce::Colour (0xff6b7280);
        }

        g.setFont (juce::Font (10.5f));
        g.setColour (col);
        g.drawText (msg, 16, (int)(sy + 12), (int)(w - 32), 16,
                    juce::Justification::centredLeft, false);
    }
}

//==============================================================================
void MorphAudioProcessorEditor::mouseUp (const juce::MouseEvent& e)
{
    using Mode = MorphAudioProcessor::ReferenceMode;
    auto pt = e.position;

    if (helpRect_.contains (pt))
    {
        // Show the companion window (Electron app). Falls back to watermorph://
        // protocol if companion is not connected, which launches it if installed.
        CompanionLink::get().requestShowWindow();
        return;
    }

    if (toggleRect.contains (pt))
    {
        // Left half = Project, right half = Track
        if (pt.x < toggleRect.getCentreX())
            audioProcessor.setReferenceMode (Mode::kProject);
        else
            audioProcessor.setReferenceMode (Mode::kTrack);
    }
    else if (keyPickRect.contains (pt)
             && audioProcessor.getReferenceMode() == Mode::kTrack)
    {
        // Track mode: open native file picker → BRAIN analyzes full file instantly
        // (no playback needed; file stays local, optionally uploaded to Water)
        auto chooser = std::make_shared<juce::FileChooser> (
            "Select audio file to analyze",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.m4a",
            false);

        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile())
                    audioProcessor.analyzeFileDirectly (f);
            });
        return;
    }
    else if (keyPickRect.contains (pt)
             && audioProcessor.getReferenceMode() == Mode::kProject)
    {
        // Key picker popup — 24 keys (12 maj + 12 min)
        static const char* kKeys[] = {
            "Cmaj","Dbmaj","Dmaj","Ebmaj","Emaj","Fmaj",
            "F#maj","Gmaj","Abmaj","Amaj","Bbmaj","Bmaj",
            "Cmin","Dbmin","Dmin","Ebmin","Emin","Fmin",
            "F#min","Gmin","Abmin","Amin","Bbmin","Bmin"
        };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Major");
        for (int i = 0; i < 12; ++i)
            menu.addItem (i + 1, juce::String (kKeys[i]),
                          true, audioProcessor.getProjectKey() == kKeys[i]);
        menu.addSectionHeader ("Minor");
        for (int i = 12; i < 24; ++i)
            menu.addItem (i + 1, juce::String (kKeys[i]),
                          true, audioProcessor.getProjectKey() == kKeys[i]);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
            [this] (int result)
            {
                static const char* kKeys[] = {
                    "Cmaj","Dbmaj","Dmaj","Ebmaj","Emaj","Fmaj",
                    "F#maj","Gmaj","Abmaj","Amaj","Bbmaj","Bmaj",
                    "Cmin","Dbmin","Dmin","Ebmin","Emin","Fmin",
                    "F#min","Gmin","Abmin","Amin","Bbmin","Bmin"
                };
                if (result >= 1 && result <= 24)
                    audioProcessor.setProjectKey (kKeys[result - 1]);
            });
        return;
    }

    repaint();
}

//==============================================================================
void MorphAudioProcessorEditor::timerCallback()
{
    repaint();

    // Poll morphed state from companion (written by WaterMorphCompanion when React lock changes)
    if (++morphPollTick_ >= 3)  // every 600ms (3 × 200ms timer)
    {
        morphPollTick_ = 0;
        const juce::File stateFile ("/tmp/water-morph-isMorphed");
        if (stateFile.existsAsFile())
        {
            const bool morphed = stateFile.loadFileAsString().trim() == "1";
            if (morphed != isMorphed_) { isMorphed_ = morphed; repaint(); }
        }
    }

    using Mode = MorphAudioProcessor::ReferenceMode;
    const bool trackMode  = audioProcessor.getReferenceMode() == Mode::kTrack;
    const double bpm      = audioProcessor.getEffectiveBPM();
    const double ppq      = audioProcessor.readCurrentPPQPosition();
    const double timeSecs = (bpm > 0.0) ? (ppq / bpm * 60.0) : 0.0;
    const bool   playing  = audioProcessor.getIsPlaying();

    // Send TRANSPORT on every tick while playing so the web player stays
    // phase-locked to the DAW grid — especially critical when the user
    // switches to a new loop mid-session (new loop starts at 0, TRANSPORT
    // immediately seeks it to the correct bar-relative position).
    // When stopped, only send on the transition to avoid redundant pauses.
    if (playing)
    {
        CompanionLink::get().sendTransport (true, timeSecs, ppq);
        lastPlayState = true;
    }
    else if (lastPlayState)
    {
        lastPlayState = false;
        CompanionLink::get().sendTransport (false, timeSecs, ppq);
    }

    if (++syncTick >= 5)   // 5 × 200 ms = 1 s
    {
        syncTick = 0;
        const juce::String key = trackMode ? audioProcessor.getDetectedKey()
                                           : audioProcessor.getProjectKey();
        CompanionLink::get().sendSync (bpm, key,
                                       trackMode ? "track" : "project",
                                       timeSecs);
    }
}

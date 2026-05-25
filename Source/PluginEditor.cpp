#include "PluginEditor.h"
#include "CompanionLink.h"

static constexpr int kPlugW = 460;
static constexpr int kPlugH = 300;

//==============================================================================
MorphAudioProcessorEditor::MorphAudioProcessorEditor (MorphAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setResizable (false, false);
    setSize (kPlugW, kPlugH);
    startTimer (200);

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
        juce::Path raw = juce::Drawable::parseSVGPath (kLogoPath);
        waterLogo_ = std::make_unique<juce::DrawablePath>();
        static_cast<juce::DrawablePath*> (waterLogo_.get())->setPath (raw);
        static_cast<juce::DrawablePath*> (waterLogo_.get())
            ->setFill (juce::FillType (juce::Colours::white));
    }
}

MorphAudioProcessorEditor::~MorphAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void MorphAudioProcessorEditor::resized() {}

//==============================================================================
void MorphAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float w         = (float) getWidth();
    const float h         = (float) getHeight();
    const bool  connected = CompanionLink::get().isConnected();

    // ── Background ────────────────────────────────────────────────────────────
    g.fillAll (juce::Colour (0xff0a0a0a));

    if (isMorphed_)
    {
        g.setColour (juce::Colour (kMauve).withAlpha (0.55f));
        g.fillRect (0.f, 0.f, w, 2.f);
    }
    else if (connected)
    {
        g.setColour (juce::Colour (kTeal).withAlpha (0.40f));
        g.fillRect (0.f, 0.f, w, 2.f);
    }

    // ── Header ────────────────────────────────────────────────────────────────
    const float kHeaderH = 40.f;
    g.setColour (juce::Colour (0xff111111));
    g.fillRect (0.f, 2.f, w, kHeaderH - 2.f);

    if (waterLogo_)
    {
        juce::Colour logoCol = isMorphed_
            ? juce::Colour (kMauve).withAlpha (0.85f)
            : juce::Colours::white.withAlpha (connected ? 0.75f : 0.30f);
        static_cast<juce::DrawablePath*> (waterLogo_.get())
            ->setFill (juce::FillType (logoCol));
        waterLogo_->drawWithin (g,
            juce::Rectangle<float> (14.f, 9.f, 24.f, 22.f),
            juce::RectanglePlacement::centred, 1.0f);
    }

    {
        juce::String lbl  = isMorphed_ ? juce::CharPointer_UTF8 ("\xe2\x9c\xa6  Morphed")
                          : connected  ? "Connected to Water Studio"
                                       : "Open Water";
        juce::Colour col  = isMorphed_ ? juce::Colour (kMauve).withAlpha (0.85f)
                          : connected  ? juce::Colour (kTeal).withAlpha (0.85f)
                                       : juce::Colours::white.withAlpha (0.22f);
        g.setFont (juce::Font (10.f, juce::Font::bold));
        g.setColour (col);
        g.drawText (lbl, 44, 0, (int)(w - 88.f), (int)kHeaderH,
                    juce::Justification::centred, false);
    }

    {
        const float dotR  = 3.5f;
        const float dotCX = w - 16.f;
        const float dotCY = kHeaderH * 0.5f;
        juce::Colour dotCol = isMorphed_ ? juce::Colour (kMauve)
                            : connected  ? juce::Colour (kTeal)
                                         : juce::Colours::white.withAlpha (0.15f);
        g.setColour (dotCol);
        g.fillEllipse (dotCX - dotR, dotCY - dotR, dotR * 2.f, dotR * 2.f);
        helpRect_ = { 0.f, 0.f, w, kHeaderH };
    }

    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (0.f, kHeaderH, w, 1.f);

    // ── BPM + KEY ─────────────────────────────────────────────────────────────
    // Manual key overrides auto-detected key. Auto-detection is a fallback only.
    const juce::String manualKey   = audioProcessor.getProjectKey();
    const juce::String detectedKey = audioProcessor.getDetectedKey();
    const bool hasManual  = manualKey.isNotEmpty();
    const bool hasDetected = detectedKey.isNotEmpty() && detectedKey != "?";

    const juce::String key    = hasManual ? manualKey : (hasDetected ? detectedKey : "");
    const bool         hasKey = key.isNotEmpty();

    const float  scanProg    = audioProcessor.getAnalysisProgress();   // 0..1 buffer fill
    const bool   brainGaveUp = audioProcessor.getBrainAttempts() >= 1;
    const bool   isScanning  = !hasKey && !audioProcessor.isAnalysing() && scanProg > 0.01f && !brainGaveUp;

    const double bpm    = audioProcessor.getEffectiveBPM();
    const bool   hasBpm = (bpm > 0.0);

    const juce::String bpmStr = hasBpm ? juce::String ((int)std::round (bpm)) : "--";
    const juce::String keyStr = hasKey ? key : "--";

    juce::Colour valueCol;
    if (isMorphed_)  valueCol = juce::Colour (kMauve);
    else if (hasKey) valueCol = juce::Colours::white.withAlpha (0.92f);
    else             valueCol = juce::Colours::white.withAlpha (0.18f);

    const float zoneTop = kHeaderH + 12.f;
    const float zoneBot = h - 40.f;
    const float zoneMid = (zoneTop + zoneBot) * 0.5f;
    const float half    = w * 0.5f;

    // KEY tap zone — full left column, clearly bounded
    const float kBtnX = 8.f;
    const float kBtnY = zoneTop + 4.f;
    const float kBtnW = half - 16.f;
    const float kBtnH = zoneBot - zoneTop - 8.f;
    keyPickRect_ = { kBtnX, kBtnY, kBtnW, kBtnH };

    // KEY area background — always visible, signals it's tappable
    g.setColour (juce::Colour (0xff161616));
    g.fillRoundedRectangle (kBtnX, kBtnY, kBtnW, kBtnH, 12.f);
    juce::Colour borderCol = hasKey
        ? (isMorphed_ ? juce::Colour (kMauve).withAlpha (0.45f) : juce::Colour (kTeal).withAlpha (0.35f))
        : juce::Colours::white.withAlpha (0.12f);
    g.setColour (borderCol);
    g.drawRoundedRectangle (kBtnX, kBtnY, kBtnW, kBtnH, 12.f, 1.f);

    // KEY — small label at top of box
    g.setFont (juce::Font (9.f, juce::Font::plain));
    g.setColour (juce::Colours::white.withAlpha (hasKey ? 0.28f : 0.13f));
    g.drawText ("KEY", (int)kBtnX, (int)(kBtnY + 9.f), (int)kBtnW, 12,
                juce::Justification::centred, false);

    // KEY value — large, bold, centered in box
    const float kValCX = kBtnX + kBtnW * 0.5f;
    const float kValCY = kBtnY + kBtnH * 0.5f - 4.f;
    g.setFont (juce::Font (44.f, juce::Font::bold));
    g.setColour (hasKey ? valueCol : juce::Colours::white.withAlpha (0.13f));
    g.drawText (keyStr, (int)kBtnX, (int)(kValCY - 26.f), (int)kBtnW, 52,
                juce::Justification::centred, false);

    // Scanning progress bar — thin fill along bottom of KEY box
    if (isScanning || audioProcessor.isAnalysing())
    {
        const float barX  = kBtnX + 10.f;
        const float barY  = kBtnY + kBtnH - 5.f;
        const float barW  = kBtnW - 20.f;
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRoundedRectangle (barX, barY, barW, 2.f, 1.f);
        float fill = audioProcessor.isAnalysing() ? 1.f : scanProg;
        g.setColour (juce::Colour (kTeal).withAlpha (0.55f));
        g.fillRoundedRectangle (barX, barY, barW * fill, 2.f, 1.f);
    }

    // ── Scan button — bottom of KEY box ──────────────────────────────────────
    {
        const float sbW = 82.f, sbH = 26.f;
        const float sbX = kBtnX + (kBtnW - sbW) * 0.5f;
        const float sbY = kBtnY + kBtnH - sbH - 10.f;
        scanRect_ = { sbX, sbY, sbW, sbH };

        const bool scanning = audioProcessor.isAnalysing() || audioProcessor.getAnalysisProgress() > 0.01f;

        if (scanning)
        {
            // Outer glow — clearly KEY is being analysed
            g.setColour (juce::Colour (kTeal).withAlpha (0.10f));
            g.fillRoundedRectangle (sbX - 4.f, sbY - 4.f, sbW + 8.f, sbH + 8.f, 18.f);
            g.setColour (juce::Colour (kTeal).withAlpha (0.18f));
            g.fillRoundedRectangle (sbX, sbY, sbW, sbH, 14.f);
            g.setColour (juce::Colour (kTeal).withAlpha (0.55f));
            g.drawRoundedRectangle (sbX, sbY, sbW, sbH, 14.f, 0.8f);
            g.setFont (juce::Font (10.5f, juce::Font::bold));
            g.setColour (juce::Colour (kTeal));
            juce::String lbl = juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xbb  "))
                               + juce::String ((int)(scanProg * 100)) + "%";
            g.drawText (lbl, (int)sbX, (int)sbY, (int)sbW, (int)sbH,
                        juce::Justification::centred, false);
        }
        else if (!hasKey)
        {
            // No key yet — invite user to scan (teal, prominent)
            g.setColour (juce::Colour (kTeal).withAlpha (0.14f));
            g.fillRoundedRectangle (sbX, sbY, sbW, sbH, 14.f);
            g.setColour (juce::Colour (kTeal).withAlpha (0.45f));
            g.drawRoundedRectangle (sbX, sbY, sbW, sbH, 14.f, 0.8f);
            g.setFont (juce::Font (10.5f, juce::Font::bold));
            g.setColour (juce::Colour (kTeal).withAlpha (0.90f));
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x86\xbb  Scan Key"),
                        (int)sbX, (int)sbY, (int)sbW, (int)sbH,
                        juce::Justification::centred, false);
        }
        else
        {
            // Key found — dim re-scan
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (sbX, sbY, sbW, sbH, 14.f);
            g.setColour (juce::Colours::white.withAlpha (0.14f));
            g.drawRoundedRectangle (sbX, sbY, sbW, sbH, 14.f, 0.8f);
            g.setFont (juce::Font (10.5f, juce::Font::bold));
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x86\xbb  Re-Scan"),
                        (int)sbX, (int)sbY, (int)sbW, (int)sbH,
                        juce::Justification::centred, false);
        }

        // Small context label above button
        if (!scanning)
        {
            g.setFont (juce::Font (9.5f, juce::Font::plain));
            juce::String ctx = hasManual ? "locked  \xc2\xb7  tap to change"
                             : hasKey    ? "auto-detect  \xc2\xb7  tap to set"
                                         : "";
            if (ctx.isNotEmpty())
            {
                g.setColour (juce::Colours::white.withAlpha (0.28f));
                g.drawText (ctx, (int)kBtnX, (int)(sbY - 16.f), (int)kBtnW, 14,
                            juce::Justification::centred, false);
            }
        }
    }

    // Divider
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (half - 0.5f, zoneMid - 20.f, 1.f, 40.f);

    // BPM — centered in right half
    g.setFont (juce::Font (48.f, juce::Font::bold));
    g.setColour (valueCol);
    g.drawText (bpmStr, (int)half, (int)(zoneMid - 30.f), (int)(w - half), 58,
                juce::Justification::centred, false);

    g.setFont (juce::Font (10.f, juce::Font::plain));
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawText ("BPM", (int)half, (int)(zoneMid + 32.f), (int)(w - half), 16,
                juce::Justification::centred, false);


    // ── Status strip ─────────────────────────────────────────────────────────
    {
        const float sy = h - 38.f;
        g.setColour (juce::Colour (0xff0d0d0d));
        g.fillRect (0.f, sy, w, 38.f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRect (0.f, sy, w, 1.f);

        juce::String msg;
        juce::Colour col = juce::Colours::white.withAlpha (0.20f);

        if (audioProcessor.isAnalysing())
        {
            msg = juce::CharPointer_UTF8 ("\xe2\x96\xb6  Detecting key\xe2\x80\xa6");
            col = juce::Colour (kTeal);
        }
        else if (hasKey)
        {
            msg = juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93  ")) + key
                  + "  \xe2\x80\x94  " + juce::String ((int)std::round (bpm)) + " BPM"
                  + "   \xe2\x80\x94  tap KEY to change";
            col = isMorphed_ ? juce::Colour (kMauve).withAlpha (0.7f)
                             : juce::Colours::white.withAlpha (0.30f);
        }
        else if (isScanning)
        {
            msg = juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f  Scanning audio\xe2\x80\xa6  "))
                  + juce::String ((int)(scanProg * 100)) + "%  \xe2\x80\x94  keep playing";
            col = juce::Colour (kTeal).withAlpha (0.80f);
        }
        else if (brainGaveUp)
        {
            msg = juce::CharPointer_UTF8 ("Could not detect key  \xe2\x80\x94  tap KEY to set manually");
            col = juce::Colours::white.withAlpha (0.30f);
        }
        else
        {
            msg = juce::CharPointer_UTF8 ("Play your session  \xe2\x80\x94  key detects automatically");
        }

        g.setFont (juce::Font (10.5f));
        g.setColour (col);
        g.drawText (msg, 16, (int)(sy + 12), (int)(w - 52), 16,
                    juce::Justification::centredLeft, false);

        g.setFont (juce::Font (8.f));
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawText ("v1.0", 0, (int)(sy + 12), (int)(w - 12), 16,
                    juce::Justification::centredRight, false);
    }
}

//==============================================================================
void MorphAudioProcessorEditor::mouseUp (const juce::MouseEvent& e)
{
    const auto pt = e.position;

    if (helpRect_.contains (pt))
    {
        CompanionLink::get().requestShowWindow();
        return;
    }

    if (scanRect_.contains (pt))
    {
        audioProcessor.resetAnalysis();
        repaint();
        return;
    }

    if (keyPickRect_.contains (pt))
    {
        static const char* kKeys[] = {
            "Cmaj","Dbmaj","Dmaj","Ebmaj","Emaj","Fmaj",
            "F#maj","Gmaj","Abmaj","Amaj","Bbmaj","Bmaj",
            "Cmin","Dbmin","Dmin","Ebmin","Emin","Fmin",
            "F#min","Gmin","Abmin","Amin","Bbmin","Bmin"
        };

        juce::PopupMenu menu;
        const juce::String cur = audioProcessor.getProjectKey();
        if (cur.isNotEmpty())
            menu.addItem (25, "Clear (use auto-detect)", true, false);
        menu.addSectionHeader ("Major");
        for (int i = 0; i < 12; ++i)
            menu.addItem (i + 1, juce::String (kKeys[i]), true, cur == kKeys[i]);
        menu.addSectionHeader ("Minor");
        for (int i = 12; i < 24; ++i)
            menu.addItem (i + 1, juce::String (kKeys[i]), true, cur == kKeys[i]);

        auto screenRect = localAreaToGlobal (keyPickRect_.toNearestInt());
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetScreenArea (screenRect)
                                .withMinimumWidth (130),
            [this] (int result)
            {
                static const char* kKeys[] = {
                    "Cmaj","Dbmaj","Dmaj","Ebmaj","Emaj","Fmaj",
                    "F#maj","Gmaj","Abmaj","Amaj","Bbmaj","Bmaj",
                    "Cmin","Dbmin","Dmin","Ebmin","Emin","Fmin",
                    "F#min","Gmin","Abmin","Amin","Bbmin","Bmin"
                };
                if (result == 25)
                    audioProcessor.setProjectKey ({});
                else if (result >= 1 && result <= 24)
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

    if (++morphPollTick_ >= 3)
    {
        morphPollTick_ = 0;
        const juce::File stateFile ("/tmp/water-morph-isMorphed");
        if (stateFile.existsAsFile())
        {
            const bool morphed = stateFile.loadFileAsString().trim() == "1";
            if (morphed != isMorphed_) { isMorphed_ = morphed; repaint(); }
        }
    }

    const double bpm      = audioProcessor.getEffectiveBPM();
    const double ppq      = audioProcessor.readCurrentPPQPosition();
    const double timeSecs = (bpm > 0.0) ? (ppq / bpm * 60.0) : 0.0;
    const bool   playing  = audioProcessor.getIsPlaying();
    isPlaying_ = playing;

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

    if (++syncTick >= 5)
    {
        syncTick = 0;
        const juce::String manualKey  = audioProcessor.getProjectKey();
        const juce::String key = manualKey.isNotEmpty() ? manualKey : audioProcessor.getDetectedKey();
        CompanionLink::get().sendSync (bpm, key, manualKey.isNotEmpty() ? "set" : "auto", timeSecs);
    }
}

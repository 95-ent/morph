#include "LibraryComponent.h"
#include "DropZoneComponent.h"
#include "CompanionLink.h"
#include <cmath>

//==============================================================================
namespace
{
    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kTealGlow = 0xff1DC8DC;
    static constexpr uint32_t kText     = 0xffF0F0EC;
    static constexpr uint32_t kBorder   = 0x0Dffffff;
    static constexpr uint32_t kBG       = 0xff080808;
    static constexpr uint32_t kBGRow    = 0xff0d0d0d;

    juce::String formatDuration (int s)
    {
        if (s <= 0) return {};
        return juce::String::formatted ("%d:%02d", s / 60, s % 60);
    }

    int keyToMidi (const juce::String& key)
    {
        struct Entry { const char* name; int semitone; };
        static const Entry table[] = {
            {"C",  0}, {"Cmaj",0}, {"Cmin",0},
            {"C#", 1}, {"C#maj",1},{"C#min",1}, {"Db",1},{"Dbmaj",1},{"Dbmin",1},
            {"D",  2}, {"Dmaj",2}, {"Dmin",2},
            {"D#", 3}, {"D#maj",3},{"D#min",3}, {"Eb",3},{"Ebmaj",3},{"Ebmin",3},
            {"E",  4}, {"Emaj",4}, {"Emin",4},
            {"F",  5}, {"Fmaj",5}, {"Fmin",5},
            {"F#", 6}, {"F#maj",6},{"F#min",6}, {"Gb",6},{"Gbmaj",6},{"Gbmin",6},
            {"G",  7}, {"Gmaj",7}, {"Gmin",7},
            {"G#", 8}, {"G#maj",8},{"G#min",8}, {"Ab",8},{"Abmaj",8},{"Abmin",8},
            {"A",  9}, {"Amaj",9}, {"Amin",9},
            {"A#",10}, {"A#maj",10},{"A#min",10},{"Bb",10},{"Bbmaj",10},{"Bbmin",10},
            {"B", 11}, {"Bmaj",11},{"Bmin",11},
        };
        juce::String k = key.trim();
        for (auto& e : table)
            if (k.equalsIgnoreCase (e.name)) return e.semitone;
        return 0;
    }

    int semitonesBetweenKeys (const juce::String& from, const juce::String& to)
    {
        int diff = keyToMidi (to) - keyToMidi (from);
        if (diff > 6)  diff -= 12;
        if (diff < -6) diff += 12;
        return diff;
    }

    static constexpr uint32_t kMauve     = 0xffA855F7;
    static constexpr uint32_t kMauveGlow = 0xffC084FC;

    void drawChip (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label,
                   bool active, bool enabled = true, uint32_t activeHue = kTeal)
    {
        juce::Colour fill   = active ? juce::Colour (activeHue).withAlpha (0.22f)
                                     : juce::Colour (0x10ffffff);
        juce::Colour border = active ? juce::Colour (activeHue).withAlpha (0.70f)
                                     : juce::Colour (0x18ffffff);
        juce::Colour text   = active ? juce::Colour (activeHue).brighter (0.25f)
                                     : juce::Colour (kText).withAlpha (enabled ? 0.55f : 0.25f);

        g.setColour (fill);
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (border);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 0.8f);
        g.setColour (text);
        g.setFont (juce::Font (10.0f, juce::Font::bold));
        g.drawText (label, r, juce::Justification::centred, false);
    }
}

//==============================================================================
// MorphBar
//==============================================================================
MorphBar::MorphBar() {}

void MorphBar::setHostBPM (double bpm)         { targetBPM = bpm; repaint(); }
void MorphBar::setTargetKey (const juce::String& k) { targetKey = k; repaint(); }
void MorphBar::setSyncMode (bool isTrack)       { isTrackMode = isTrack; repaint(); }
int  MorphBar::getSemitones() const             { return 0; }

void MorphBar::paint (juce::Graphics& g)
{
    int w = getWidth(), h = getHeight();

    juce::ColourGradient bg (juce::Colour (0xff0a0a0a), 0.0f, 0.0f,
                              juce::Colour (kBG), (float) w, 0.0f, false);
    g.setGradientFill (bg);
    g.fillRect (getLocalBounds());

    g.setColour (juce::Colour (kBorder));
    g.drawHorizontalLine (h - 1, 0.0f, (float) w);

    // --- MORPH button — mauve when active ---
    {
        if (morphOn)
        {
            juce::ColourGradient grd (juce::Colour (kMauve),
                                      (float) morphBtnBounds.getX(), 0.0f,
                                      juce::Colour (kMauve).darker (0.25f),
                                      (float) morphBtnBounds.getRight(), 0.0f, false);
            g.setGradientFill (grd);
        }
        else
        {
            g.setColour (juce::Colour (0xff141414));
        }
        g.fillRoundedRectangle (morphBtnBounds.toFloat(), 6.0f);
        g.setColour (morphOn ? juce::Colour (kMauveGlow).withAlpha (0.9f)
                             : juce::Colour (0x30ffffff));
        g.drawRoundedRectangle (morphBtnBounds.toFloat().reduced (0.5f), 6.0f, 1.0f);
        g.setColour (morphOn ? juce::Colour (kText) : juce::Colour (kText).withAlpha (0.45f));
        g.setFont (juce::Font (10.5f, juce::Font::bold));
        g.drawText ("MORPH", morphBtnBounds, juce::Justification::centred, false);
    }

    // KEY chip — mauve when active, always visible
    drawChip (g, keyChipBounds, "KEY  " + targetKey, keyOn && morphOn, morphOn, kMauve);

    // TEMPO chip — mauve when active, always visible
    drawChip (g, tempoChipBounds,
              "TEMPO  " + juce::String (static_cast<int> (targetBPM)) + " BPM",
              tempoOn && morphOn, morphOn, kMauve);

    // SYNC button (right side) — shows current reference mode
    {
        juce::String syncLabel  = isTrackMode ? "TRACK" : "PROJECT";
        juce::Colour syncFill   = juce::Colour (0xff0e0e0e);
        juce::Colour syncBorder = isTrackMode ? juce::Colour (kTeal).withAlpha (0.55f)
                                              : juce::Colour (0x28ffffff);
        juce::Colour syncText   = isTrackMode ? juce::Colour (kTeal)
                                              : juce::Colour (kText).withAlpha (0.40f);

        g.setColour (syncFill);
        g.fillRoundedRectangle (syncBtnBounds.toFloat(), 5.0f);
        g.setColour (syncBorder);
        g.drawRoundedRectangle (syncBtnBounds.toFloat().reduced (0.5f), 5.0f, 0.8f);
        g.setColour (syncText);
        g.setFont (juce::Font (9.5f, juce::Font::bold));
        g.drawText (syncLabel, syncBtnBounds, juce::Justification::centred, false);
    }
}

void MorphBar::resized()
{
    int h    = getHeight();
    int padV = (h - 22) / 2;

    morphBtnBounds  = { 8,   padV, 58,  22 };
    keyChipBounds   = { 74,  padV, 90,  22 };
    tempoChipBounds = { 170, padV, 108, 22 };

    // SYNC button anchored to right edge
    syncBtnBounds = { getWidth() - 80, padV, 72, 22 };
}

void MorphBar::mouseDown (const juce::MouseEvent& e)
{
    auto p = e.getPosition();

    if (morphBtnBounds.contains (p))
    {
        morphOn = !morphOn;
        repaint();
        if (onChange) onChange (morphOn, keyOn, tempoOn);
    }
    else if (keyChipBounds.contains (p))
    {
        static const char* kKeys[] = {
            "Cmin","C#min","Dmin","D#min","Emin","Fmin","F#min","Gmin","G#min","Amin","A#min","Bmin",
            "Cmaj","C#maj","Dmaj","D#maj","Emaj","Fmaj","F#maj","Gmaj","G#maj","Amaj","A#maj","Bmaj"
        };
        juce::PopupMenu menu;
        menu.addItem (1, "Off (no key morph)", true, !keyOn);
        menu.addSeparator();
        for (int i = 0; i < 24; ++i)
            menu.addItem (i + 2, kKeys[i], true, targetKey == kKeys[i]);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
            [this] (int result)
            {
                if (result == 1)
                {
                    keyOn = false;
                }
                else if (result >= 2)
                {
                    static const char* kk[] = {
                        "Cmin","C#min","Dmin","D#min","Emin","Fmin","F#min","Gmin","G#min","Amin","A#min","Bmin",
                        "Cmaj","C#maj","Dmaj","D#maj","Emaj","Fmaj","F#maj","Gmaj","G#maj","Amaj","A#maj","Bmaj"
                    };
                    targetKey = kk[result - 2];
                    keyOn = true;
                }
                repaint();
                if (onChange) onChange (morphOn, keyOn, tempoOn);
            });
    }
    else if (tempoChipBounds.contains (p))
    {
        tempoOn = !tempoOn;
        if (!tempoOn && !keyOn) keyOn = true;
        repaint();
        if (onChange) onChange (morphOn, keyOn, tempoOn);
    }
    else if (syncBtnBounds.contains (p))
    {
        if (onSyncToggled) onSyncToggled();
    }
}

//==============================================================================
// NowPlayingBar
//==============================================================================
NowPlayingBar::NowPlayingBar()
{
    setInterceptsMouseClicks (true, true);

    volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange (0.0, 1.0);
    volumeSlider.setValue (0.85, juce::dontSendNotification);
    volumeSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (kTeal));
    volumeSlider.setColour (juce::Slider::trackColourId,      juce::Colour (kTeal).withAlpha (0.4f));
    volumeSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff1a1a1a));
    volumeSlider.onValueChange = [this] {
        if (onGainChanged) onGainChanged ((float) volumeSlider.getValue());
    };
    addAndMakeVisible (volumeSlider);
}

void NowPlayingBar::setTrack (const TrackInfo& t)  { current = t; hasTrack = true; repaint(); }
void NowPlayingBar::clearTrack()                    { hasTrack = false; repaint(); }
void NowPlayingBar::setStatus (const juce::String& s) { statusText = s; repaint(); }

void NowPlayingBar::resized()
{
    // Volume slider on far right; leave a few pixels of padding
    int h    = getHeight();
    int padV = (h - 18) / 2;
    volumeSlider.setBounds (getWidth() - 94, padV, 86, 18);
}

void NowPlayingBar::paint (juce::Graphics& g)
{
    int w = getWidth(), h = getHeight();
    g.fillAll (juce::Colour (kBG));

    // Top border — teal accent fade left→right
    juce::ColourGradient topGrd (juce::Colour (kTeal).withAlpha (0.5f), (float) w * 0.15f, 0.0f,
                                  juce::Colour (kTeal).withAlpha (0.0f), (float) w * 0.6f, 0.0f, false);
    g.setGradientFill (topGrd);
    g.fillRect (0, 0, w, 1);

    // Volume icon hint (small speaker bar to the left of the slider)
    {
        float vx = (float)(w - 98);
        float vy = (float) h * 0.5f;
        g.setColour (juce::Colour (kText).withAlpha (0.18f));
        g.fillRoundedRectangle (vx, vy - 4.0f, 3.0f, 8.0f, 1.0f);
        g.fillRoundedRectangle (vx + 5.0f, vy - 6.0f, 2.0f, 12.0f, 1.0f);
    }

    // Status text when idle
    if (!hasTrack)
    {
        if (statusText.isNotEmpty())
        {
            g.setColour (juce::Colour (kText).withAlpha (0.25f));
            g.setFont (juce::Font (10.0f));
            g.drawText (statusText, 14, 0, w - 110, h, juce::Justification::centredLeft, false);
        }
        return;
    }

    // Subtle radial glow from left when playing
    juce::ColourGradient glow (juce::Colour (kTeal).withAlpha (0.09f), 18.0f, (float) h * 0.5f,
                                juce::Colour (kTeal).withAlpha (0.0f), (float) w * 0.5f, (float) h * 0.5f, true);
    g.setGradientFill (glow);
    g.fillRect (0, 0, w, h);

    // Pause bars icon
    g.setColour (juce::Colour (kTealGlow));
    float bh = h * 0.40f, cy = h * 0.5f, bx = 13.0f;
    g.fillRoundedRectangle (bx,       cy - bh * 0.5f, 2.5f, bh, 1.0f);
    g.fillRoundedRectangle (bx + 5.5f, cy - bh * 0.5f, 2.5f, bh, 1.0f);

    // Track name — fits in space left of KEY/BPM pills
    g.setFont (juce::Font (11.5f, juce::Font::bold));
    g.setColour (juce::Colour (kText));
    g.drawText (current.name, 30, 0, w - 230, h, juce::Justification::centredLeft, true);

    // KEY pill
    if (current.key.isNotEmpty())
    {
        juce::Rectangle<float> pill (w - 222.0f, (h - 17) * 0.5f, 44.0f, 17.0f);
        juce::ColourGradient pg (juce::Colour (kTeal).withAlpha (0.28f), pill.getX(), 0.0f,
                                  juce::Colour (kTeal).withAlpha (0.14f), pill.getRight(), 0.0f, false);
        g.setGradientFill (pg);
        g.fillRoundedRectangle (pill, 8.5f);
        g.setColour (juce::Colour (kTealGlow));
        g.setFont (juce::Font (9.5f));
        g.drawText (current.key, pill.toNearestInt(), juce::Justification::centred, false);
    }

    // BPM text
    if (current.bpm > 0.0)
    {
        g.setColour (juce::Colour (kText).withAlpha (0.28f));
        g.setFont (juce::Font (10.0f));
        g.drawText (juce::String (static_cast<int> (current.bpm)) + " bpm",
                    w - 172, 0, 48, h, juce::Justification::centredRight, false);
    }
}

//==============================================================================
// TrackRowComponent
//==============================================================================
TrackRowComponent::TrackRowComponent()
{
    setInterceptsMouseClicks (true, false);
}

void TrackRowComponent::update (const TrackInfo&           track,
                                 bool                        isPlaying,
                                 bool                        isExporting,
                                 const juce::File&           readyWAV,
                                 const MorphDisplay&         morph,
                                 const juce::Array<float>&   waveform,
                                 std::function<void()>        onPlayClicked,
                                 std::function<void()>        onDragRequested)
{
    trackData     = track;
    playing       = isPlaying;
    exporting     = isExporting;
    cachedWAV     = readyWAV;
    morphDisplay  = morph;
    waveformData  = waveform;
    playCallback  = std::move (onPlayClicked);
    dragCallback  = std::move (onDragRequested);
    dragStarted   = false;
    repaint();
}

void TrackRowComponent::mouseDown (const juce::MouseEvent&)
{
    dragStarted = false;
    if (playCallback) playCallback();
}

void TrackRowComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted) return;
    if (e.getDistanceFromDragStart() < 8) return;

    if (!cachedWAV.existsAsFile())
    {
        // WAV not exported yet — kick off the export (dots will appear in the row).
        // User can try dragging again once dots turn to the grip icon.
        if (dragCallback) dragCallback();
        return;
    }

    dragStarted = true;
    CompanionLink::get().requestDrag (cachedWAV.getFullPathName());
}

void TrackRowComponent::drawPlayPauseIcon (juce::Graphics& g,
                                            juce::Rectangle<float> area,
                                            bool isPlaying)
{
    float cx = area.getCentreX(), cy = area.getCentreY(), r = area.getWidth() * 0.5f;

    if (isPlaying)
    {
        juce::ColourGradient grd (juce::Colour (kTeal), cx - r, cy,
                                   juce::Colour (kTealGlow), cx + r, cy, false);
        g.setGradientFill (grd);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.setColour (juce::Colour (kText));
        float bw = r * 0.22f, bh = r * 0.68f;
        g.fillRoundedRectangle (cx - r * 0.3f - bw * 0.5f, cy - bh * 0.5f, bw, bh, 1.0f);
        g.fillRoundedRectangle (cx + r * 0.1f,              cy - bh * 0.5f, bw, bh, 1.0f);
    }
    else if (hovered)
    {
        g.setColour (juce::Colour (kTeal).withAlpha (0.60f));
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        juce::Path tri;
        float tw = r * 0.48f, th = r * 0.76f, tx = cx - tw * 0.28f;
        tri.addTriangle (tx, cy - th * 0.5f, tx, cy + th * 0.5f, tx + tw, cy);
        g.setColour (juce::Colour (kText));
        g.fillPath (tri);
    }
    else
    {
        g.setColour (juce::Colour (kText).withAlpha (0.10f));
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);
        juce::Path tri;
        float tw = r * 0.46f, th = r * 0.72f, tx = cx - tw * 0.28f;
        tri.addTriangle (tx, cy - th * 0.5f, tx, cy + th * 0.5f, tx + tw, cy);
        g.setColour (juce::Colour (kText).withAlpha (0.12f));
        g.fillPath (tri);
    }
}

void TrackRowComponent::paint (juce::Graphics& g)
{
    int w = getWidth(), h = getHeight();

    g.setColour (juce::Colour (kBG));
    g.fillRect (getLocalBounds());

    if (playing)
    {
        juce::ColourGradient grd (juce::Colour (kTeal).withAlpha (0.10f), 0.0f, 0.0f,
                                   juce::Colour (kTeal).withAlpha (0.0f),  (float) w * 0.55f, 0.0f, false);
        g.setGradientFill (grd);
        g.fillRect (getLocalBounds());

        juce::ColourGradient accent (juce::Colour (kTealGlow), 0.0f, 0.0f,
                                      juce::Colour (kTeal).withAlpha (0.0f), 3.0f, 0.0f, false);
        g.setGradientFill (accent);
        g.fillRect (0, 0, 3, h);
    }
    else if (hovered)
    {
        g.setColour (juce::Colour (0xff121212));
        g.fillRect (getLocalBounds());
    }

    g.setColour (juce::Colour (kBorder));
    g.drawHorizontalLine (h - 1, 0.0f, (float) w);

    // Waveform visualization — drawn behind text, in the name column
    if (waveformData.size() > 1)
    {
        int    nameLeft   = 43;
        int    nameRight  = w - 162;  // leave space for duration/key/bpm columns
        float  waveAlpha  = playing ? 0.22f : 0.09f;
        float  waveX      = (float) nameLeft;
        float  waveW      = (float) (nameRight - nameLeft);
        float  centerY    = (float) h * 0.5f;
        float  maxBarH    = (float) h * 0.38f;
        float  step       = waveW / (float) waveformData.size();
        float  barW       = juce::jmax (1.0f, step * 0.55f);

        juce::Colour waveColour = playing ? juce::Colour (kTealGlow) : juce::Colour (kText);
        g.setColour (waveColour.withAlpha (waveAlpha));

        for (int i = 0; i < waveformData.size(); ++i)
        {
            float amp  = waveformData[i];
            float barH = amp * maxBarH;
            g.fillRoundedRectangle (waveX + i * step, centerY - barH,
                                    barW, barH * 2.0f, barW * 0.5f);
        }
    }

    float iconSize = 22.0f, iconX = 10.0f, iconY = (h - iconSize) * 0.5f;
    drawPlayPauseIcon (g, { iconX, iconY, iconSize, iconSize }, playing);

    int dragX    = w - 16;
    int bpmRight = dragX - 4;
    int keyRight = bpmRight - 40;
    int timeRight = keyRight - 44;
    int nameLeft  = (int)(iconX + iconSize + 10.0f);
    int nameRight = timeRight - 8;

    g.setFont (juce::Font (12.5f, juce::Font::bold));
    g.setColour (playing ? juce::Colour (kTealGlow) : juce::Colour (kText));
    g.drawText (trackData.name, nameLeft, 4, nameRight - nameLeft, 18,
                juce::Justification::centredLeft, true);

    if (trackData.tags.isNotEmpty())
    {
        g.setFont (juce::Font (9.5f));
        g.setColour (playing ? juce::Colour (kTeal).withAlpha (0.55f)
                             : juce::Colour (kText).withAlpha (0.22f));
        g.drawText (trackData.tags, nameLeft, 24, nameRight - nameLeft, 14,
                    juce::Justification::centredLeft, true);
    }

    juce::String dur = formatDuration (trackData.duration);
    if (dur.isNotEmpty())
    {
        g.setFont (juce::Font (10.0f));
        g.setColour (juce::Colour (kText).withAlpha (0.18f));
        g.drawText (dur, timeRight - 36, 0, 36, h, juce::Justification::centredRight, false);
    }

    if (trackData.key.isNotEmpty())
    {
        bool keyMorphed = morphDisplay.morphOn && morphDisplay.keyOn;
        juce::String keyLabel = keyMorphed ? morphDisplay.targetKey : trackData.key;

        if (keyMorphed)
        {
            // Proper capsule pill: corner radius = height/2
            juce::Rectangle<float> pill ((float)(keyRight - 44), (h - 18) * 0.5f, 44.0f, 18.0f);
            float cr = pill.getHeight() * 0.5f;
            g.setColour (juce::Colour (kMauve).withAlpha (0.20f));
            g.fillRoundedRectangle (pill, cr);
            g.setColour (juce::Colour (kMauve).withAlpha (0.55f));
            g.drawRoundedRectangle (pill.reduced (0.5f), cr, 0.8f);
            g.setColour (juce::Colour (kMauveGlow));
            g.setFont (juce::Font (10.0f, juce::Font::bold));
            g.drawText (keyLabel, pill.toNearestInt(), juce::Justification::centred, false);
        }
        else
        {
            g.setColour (playing ? juce::Colour (kTealGlow)
                                 : juce::Colour (kText).withAlpha (0.30f));
            g.setFont (juce::Font (10.5f));
            g.drawText (keyLabel, keyRight - 44, 0, 44, h, juce::Justification::centredRight, false);
        }
    }

    if (trackData.bpm > 0.0)
    {
        bool bpmMorphed = morphDisplay.morphOn && morphDisplay.tempoOn;
        juce::String bpmLabel = bpmMorphed
            ? juce::String (static_cast<int> (morphDisplay.targetBPM))
            : juce::String (static_cast<int> (trackData.bpm));

        if (bpmMorphed)
            g.setColour (juce::Colour (kMauve));
        else
            g.setColour (playing ? juce::Colour (kTeal) : juce::Colour (kText).withAlpha (0.18f));

        g.setFont (juce::Font (10.5f));
        g.drawText (bpmLabel, bpmRight - 38, 0, 38, h, juce::Justification::centredRight, false);
    }

    // Drag handle dots
    {
        bool wavReady = cachedWAV.existsAsFile();
        float dx = (float)(w - 14), dy = (float)(h) * 0.5f;
        if (exporting)
        {
            g.setColour (juce::Colour (kTeal).withAlpha (0.4f));
            for (int i = 0; i < 3; ++i)
                g.fillEllipse (dx - 1.0f, dy - 7.0f + i * 6.0f, 3.0f, 3.0f);
        }
        else if (wavReady)
        {
            g.setColour (juce::Colour (kText).withAlpha (hovered ? 0.45f : 0.18f));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    g.fillEllipse (dx + col * 5.0f - 3.0f, dy - 7.0f + row * 6.0f, 2.5f, 2.5f);
        }
        else if (hovered)
        {
            g.setColour (juce::Colour (kText).withAlpha (0.10f));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    g.fillEllipse (dx + col * 5.0f - 3.0f, dy - 7.0f + row * 6.0f, 2.5f, 2.5f);
        }
    }
}

//==============================================================================
// LibraryComponent
//==============================================================================
LibraryComponent::LibraryComponent (MorphAudioProcessor& processor)
    : proc (processor)
{
    // Morph bar
    morphBar.onChange = [this] (bool on, bool keyOn, bool tempoOn)
    {
        onMorphChanged (on, keyOn, tempoOn);
    };
    morphBar.onSyncToggled = [this]
    {
        using Mode = MorphAudioProcessor::ReferenceMode;
        bool isTrack = proc.getReferenceMode() == Mode::kTrack;
        proc.setReferenceMode (isTrack ? Mode::kProject : Mode::kTrack);
    };
    addAndMakeVisible (morphBar);

    // Type filter chips
    addAndMakeVisible (typeMelo);
    addAndMakeVisible (typeBeat);
    addAndMakeVisible (typeToplines);
    addAndMakeVisible (typeSongs);

    auto chipStyle = [](juce::TextButton& b, const juce::String& label)
    {
        b.setButtonText (label);
        b.setClickingTogglesState (true);
        b.setToggleState (false, juce::dontSendNotification);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff111111));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kTeal).withAlpha (0.18f));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kText).withAlpha (0.38f));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kTealGlow));
    };
    chipStyle (typeMelo,     "Melo");
    chipStyle (typeBeat,     "Beat");
    chipStyle (typeToplines, "Toplines");
    chipStyle (typeSongs,    "Songs");

    for (auto* b : { &typeMelo, &typeBeat, &typeToplines, &typeSongs })
        b->onClick = [this] { filterTracks (searchBar.getText()); };

    searchBar.setTextToShowWhenEmpty ("Search tracks...", juce::Colour (kText).withAlpha (0.2f));
    searchBar.setColour (juce::TextEditor::backgroundColourId,     juce::Colour (0xff0d0d0d));
    searchBar.setColour (juce::TextEditor::textColourId,           juce::Colour (kText));
    searchBar.setColour (juce::TextEditor::outlineColourId,        juce::Colour (0x18ffffff));
    searchBar.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (kTeal));
    searchBar.onTextChange = [this] { filterTracks (searchBar.getText()); };
    addAndMakeVisible (searchBar);

    refreshButton.setButtonText ("Refresh");
    refreshButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff0d0d0d));
    refreshButton.setColour (juce::TextButton::textColourOffId, juce::Colour (kTeal));
    refreshButton.onClick = [this] { refresh(); };
    addAndMakeVisible (refreshButton);

    // Drop zone (owned here, positioned between search and track list)
    dropZone = std::make_unique<DropZoneComponent> (proc);
    dropZone->onUploadComplete = [this] { refresh(); };
    addAndMakeVisible (*dropZone);

    trackList.setModel (this);
    trackList.setRowHeight (48);
    trackList.setColour (juce::ListBox::backgroundColourId, juce::Colour (kBG));
    trackList.setColour (juce::ListBox::outlineColourId,    juce::Colours::transparentBlack);
    trackList.setOutlineThickness (0);
    addAndMakeVisible (trackList);

    // Wire volume slider in NowPlayingBar to processor gain
    nowPlaying.onGainChanged = [this] (float g) { proc.setPreviewGain (g); };
    addAndMakeVisible (nowPlaying);

    // Launch companion (non-blocking — spins up in ~200ms on first run)
    std::thread ([] { CompanionLink::get().ensureRunning(); }).detach();

    refresh();
}

//==============================================================================
void LibraryComponent::setHostBPM (double bpm)
{
    currentHostBPM = bpm;
    morphBar.setHostBPM (bpm);
    trackList.updateContent();
    trackList.repaint();
}

void LibraryComponent::setDetectedKey (const juce::String& key)
{
    morphBar.setTargetKey (key);
    trackList.updateContent();
    trackList.repaint();
}

void LibraryComponent::setSyncMode (bool isTrack)
{
    morphBar.setSyncMode (isTrack);
    trackList.updateContent();
    trackList.repaint();
}

void LibraryComponent::setLoadStatus (const juce::String& status)
{
    nowPlaying.setStatus (status);
}

void LibraryComponent::setOnlineStatus (bool online)
{
    if (onlineStatus != online)
    {
        onlineStatus = online;
        repaint();
    }
}

void LibraryComponent::tickWaveformCache()
{
    using S = MorphAudioProcessor::PreviewStatus;
    if (proc.getPreviewStatus() != S::Ready) return;

    juce::String id = proc.getCurrentlyLoadedTrackId();
    if (id.isEmpty() || waveformCache.contains (id)) return;

    // Compute on a background thread — never block message or audio thread
    std::thread ([this, id]
    {
        auto data = proc.getWaveformForLoadedTrack (160);
        juce::MessageManager::callAsync ([this, id, data = std::move (data)]
        {
            if (!waveformCache.contains (id))
            {
                waveformCache.set (id, data);
                trackList.updateContent();
                trackList.repaint();
            }
        });
    }).detach();
}

//==============================================================================
void LibraryComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBG));

    // Column header row — positioned after morphBar(34) + chips(30) + search(36) + dropZone(60)
    static constexpr int kHeaderY = 34 + 30 + 36 + 60;
    juce::Rectangle<int> headerArea (0, kHeaderY, getWidth(), 20);
    {
        g.setColour (juce::Colour (0xff060606));
        g.fillRect (headerArea);
        g.setColour (juce::Colour (kBorder));
        g.drawHorizontalLine (headerArea.getBottom() - 1,
                              (float) headerArea.getX(), (float) headerArea.getRight());
        g.setColour (juce::Colour (kText).withAlpha (0.16f));
        g.setFont (juce::Font (9.0f));
        int w  = getWidth();
        int y  = headerArea.getY();
        int hw = headerArea.getHeight();
        g.drawText ("name",  42,      y, 100, hw, juce::Justification::centredLeft,  false);
        g.drawText ("time",  w - 148, y,  34, hw, juce::Justification::centredRight, false);
        g.drawText ("key",   w - 108, y,  40, hw, juce::Justification::centredRight, false);
        g.drawText ("bpm",   w - 62,  y,  46, hw, juce::Justification::centredRight, false);
    }

    if (loading)
    {
        g.setColour (juce::Colour (kText).withAlpha (0.22f));
        g.setFont (juce::Font (12.0f));
        g.drawText ("Loading library...",
                    getLocalBounds().withTrimmedTop (180), juce::Justification::centred, false);
    }
    else if (filteredTracks.isEmpty() && !apiError)
    {
        g.setColour (juce::Colour (kText).withAlpha (0.14f));
        g.setFont (juce::Font (12.0f));
        g.drawText ("Your Water library will appear here.",
                    getLocalBounds().withTrimmedTop (180), juce::Justification::centred, false);
    }

    if (!filteredTracks.isEmpty())
    {
        auto topBar = getLocalBounds().removeFromTop (34 + 30);
        g.setColour (juce::Colour (kText).withAlpha (0.12f));
        g.setFont (juce::Font (9.0f));
        g.drawText (juce::String (filteredTracks.size()) + " tracks",
                    topBar.withTrimmedRight (84), juce::Justification::centredRight, false);
    }
}

void LibraryComponent::resized()
{
    auto area = getLocalBounds();

    // Now playing bar (40px bottom) — includes volume slider
    nowPlaying.setBounds (area.removeFromBottom (40));

    // MorphBar (34px top) — MORPH + KEY + TEMPO + SYNC
    morphBar.setBounds (area.removeFromTop (34));

    // Type filter chips row (30px)
    auto chipRow = area.removeFromTop (30).reduced (8, 5);
    int cw = 56, ch = chipRow.getHeight();
    typeMelo    .setBounds (chipRow.getX() + 0,              chipRow.getY(), cw,      ch);
    typeBeat    .setBounds (chipRow.getX() + cw + 4,         chipRow.getY(), cw,      ch);
    typeToplines.setBounds (chipRow.getX() + cw * 2 + 8,     chipRow.getY(), cw + 20, ch);
    typeSongs   .setBounds (chipRow.getX() + cw * 2 + cw + 32, chipRow.getY(), cw,   ch);

    // Search row (36px)
    auto searchRow = area.removeFromTop (36).reduced (8, 5);
    searchBar   .setBounds (searchRow.withTrimmedRight (76));
    refreshButton.setBounds (searchRow.withTrimmedLeft (searchRow.getWidth() - 68));

    // Drop zone (60px)
    dropZone->setBounds (area.removeFromTop (60));

    // Column headers (20px) — drawn in paint, just consume space
    area.removeFromTop (20);

    trackList.setBounds (area);
}

//==============================================================================
void LibraryComponent::refresh()
{
    loading = true;
    repaint();

    proc.loadTracksFromAPI ([this] (bool ok, juce::Array<TrackInfo> tracks)
    {
        loading = false;
        if (ok)
        {
            apiError  = false;
            allTracks = tracks;
            filterTracks (searchBar.getText());
        }
        else
        {
            apiError = true;
            allTracks.clear();
            filteredTracks.clear();
            trackList.updateContent();
            if (onNeedsReauth) onNeedsReauth();
        }
        repaint();
    });
}

//==============================================================================
void LibraryComponent::onMorphChanged (bool morphOnVal, bool keyOnVal, bool tempoOnVal)
{
    proc.setMorphBPMEnabled (morphOnVal && tempoOnVal);
    proc.setMorphKeyEnabled (morphOnVal && keyOnVal);

    if (keyOnVal)
        proc.setProjectKey (morphBar.getTargetKey());

    // Live-update semitones for the currently playing track
    if (!currentlyPlayingId.isEmpty())
    {
        for (const auto& t : filteredTracks)
        {
            if (t.id == currentlyPlayingId)
            {
                int semitones = (morphOnVal && keyOnVal && t.key.isNotEmpty())
                              ? computeSemitones (t.key, morphBar.getTargetKey())
                              : 0;
                proc.setPreviewSemitones (semitones);
                break;
            }
        }
    }

    cachedWAVs.clear();
    trackList.updateContent();
    trackList.repaint();
}

//==============================================================================
void LibraryComponent::filterTracks (const juce::String& query)
{
    filteredTracks.clear();
    juce::String q = query.trim().toLowerCase();

    bool anyTypeActive = typeMelo.getToggleState() || typeBeat.getToggleState()
                      || typeToplines.getToggleState() || typeSongs.getToggleState();

    for (auto& t : allTracks)
    {
        if (!q.isEmpty())
        {
            bool matches = t.name.toLowerCase().contains (q)
                        || t.key.toLowerCase().contains (q)
                        || t.tags.toLowerCase().contains (q)
                        || juce::String (t.bpm).contains (q);
            if (!matches) continue;
        }

        if (anyTypeActive)
        {
            juce::String idType = t.idType.toLowerCase();
            juce::String tags   = t.tags.toLowerCase();
            bool pass = false;
            if (typeMelo    .getToggleState() && (idType == "melody"  || idType.contains ("melo") || tags.contains ("melo"))) pass = true;
            if (typeBeat    .getToggleState() && (idType == "beat"    || tags.contains ("beat") || tags.contains ("drum")))    pass = true;
            if (typeToplines.getToggleState() && (idType == "topline" || tags.contains ("topline") || tags.contains ("vocal"))) pass = true;
            if (typeSongs   .getToggleState() && (idType == "song"    || tags.contains ("song")))                               pass = true;
            if (!pass) continue;
        }

        filteredTracks.add (t);
    }
    trackList.updateContent();
    repaint();
}

//==============================================================================
int LibraryComponent::computeSemitones (const juce::String& trackKey,
                                         const juce::String& targetKey)
{
    return semitonesBetweenKeys (trackKey, targetKey);
}

//==============================================================================
int LibraryComponent::getNumRows() { return filteredTracks.size(); }

juce::Component* LibraryComponent::refreshComponentForRow (int row, bool,
                                                             juce::Component* existing)
{
    auto* rowComp = dynamic_cast<TrackRowComponent*> (existing);
    if (rowComp == nullptr) rowComp = new TrackRowComponent();

    if (juce::isPositiveAndBelow (row, filteredTracks.size()))
    {
        const TrackInfo& track = filteredTracks.getReference (row);
        bool playing     = (track.id == currentlyPlayingId);
        bool isExporting = exportingIds.contains (track.id);

        juce::File readyWAV;
        if (cachedWAVs.contains (track.id))
            readyWAV = cachedWAVs[track.id];

        MorphDisplay morph;
        morph.morphOn   = morphBar.isMorphOn();
        morph.keyOn     = morphBar.isKeyOn();
        morph.tempoOn   = morphBar.isTempoOn();
        morph.targetBPM = currentHostBPM;
        morph.targetKey = morphBar.getTargetKey();

        // Waveform — served from cache (computed once on load, never on the message thread)
        juce::Array<float> waveform;
        if (waveformCache.contains (track.id))
            waveform = waveformCache[track.id];

        rowComp->update (track, playing, isExporting, readyWAV, morph, waveform,
            [this, track] { onPlayClicked (track); },
            [this, track] { onDragRequested (track); });
    }
    return rowComp;
}

//==============================================================================
void LibraryComponent::onPlayClicked (const TrackInfo& track)
{
    if (currentlyPlayingId == track.id)
    {
        proc.stopPreview();
        currentlyPlayingId = {};
        nowPlaying.clearTrack();
    }
    else
    {
        proc.stopPreview();
        currentlyPlayingId = track.id;

        int semitones = 0;
        if (morphBar.isMorphOn() && morphBar.isKeyOn() && track.key.isNotEmpty())
            semitones = computeSemitones (track.key, morphBar.getTargetKey());

        proc.startPreview (track.id, track.demoHash, track.bpm, semitones);
        nowPlaying.setTrack (track);

        // Pre-export WAV in background while user listens — drag will be instant.
        onDragRequested (track);
    }
    trackList.updateContent();
    trackList.repaint();
}

//==============================================================================
void LibraryComponent::onDragRequested (const TrackInfo& track)
{
    if (cachedWAVs.contains (track.id) && cachedWAVs[track.id].existsAsFile())
        return;

    if (exportingIds.contains (track.id))
        return;

    double targetBPM = (morphBar.isMorphOn() && morphBar.isTempoOn() && currentHostBPM > 0.0)
                     ? currentHostBPM : track.bpm;

    int semitones = 0;
    if (morphBar.isMorphOn() && morphBar.isKeyOn() && track.key.isNotEmpty())
        semitones = computeSemitones (track.key, morphBar.getTargetKey());

    exportingIds.add (track.id);
    trackList.updateContent();

    std::thread ([this, track, targetBPM, semitones]()
    {
        juce::File wav = proc.exportMorphedWAV (track.id, track.demoHash, track.bpm, targetBPM, semitones);

        juce::MessageManager::callAsync ([this, track, wav]()
        {
            exportingIds.removeAllInstancesOf (track.id);
            if (wav.existsAsFile())
                cachedWAVs.set (track.id, wav);
            trackList.updateContent();
            trackList.repaint();
        });
    }).detach();
}

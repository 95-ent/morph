#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "DropZoneComponent.h"

//==============================================================================
// MorphDisplay — passed from LibraryComponent to each row so it knows how to
// render KEY/BPM columns when morph is active.
//==============================================================================
struct MorphDisplay
{
    bool         morphOn  { false };
    bool         keyOn    { true  };
    bool         tempoOn  { true  };
    double       targetBPM { 120.0 };
    juce::String targetKey;
};

//==============================================================================
// TrackRowComponent — Water-style row.
// Click anywhere to play/pause. Drag the row to drop a morphed WAV into the DAW.
// No explicit Export button — zero friction.
//==============================================================================
class TrackRowComponent  : public juce::Component,
                            public juce::DragAndDropContainer
{
public:
    TrackRowComponent();
    ~TrackRowComponent() override = default;

    void update (const TrackInfo&           track,
                 bool                        isPlaying,
                 bool                        isExporting,
                 const juce::File&           readyWAV,
                 const MorphDisplay&         morph,
                 const juce::Array<float>&   waveform,
                 std::function<void()>        onPlayClicked,
                 std::function<void()>        onDragRequested);

    void paint     (juce::Graphics& g) override;
    void resized   () override {}
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }

private:
    TrackInfo          trackData;
    MorphDisplay       morphDisplay;
    juce::Array<float> waveformData;
    bool               playing   { false };
    bool               exporting { false };
    bool               hovered   { false };
    juce::File         cachedWAV;

    std::function<void()> playCallback;
    std::function<void()> dragCallback;

    bool dragStarted { false };

    void drawPlayPauseIcon (juce::Graphics& g, juce::Rectangle<float> area, bool isPlaying);

    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kTealGlow = 0xff1DC8DC;
    static constexpr uint32_t kMauve    = 0xffA855F7;
    static constexpr uint32_t kMauveGlow= 0xffC084FC;
    static constexpr uint32_t kText     = 0xffF0F0EC;
    static constexpr uint32_t kBorder   = 0x0Dffffff;
    static constexpr uint32_t kBG       = 0xff0d0d0d;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRowComponent)
};

//==============================================================================
// NowPlayingBar — bottom strip: track info + volume slider
//==============================================================================
class NowPlayingBar : public juce::Component
{
public:
    NowPlayingBar();
    void setTrack  (const TrackInfo& t);
    void clearTrack();
    void setStatus (const juce::String& s);
    void paint   (juce::Graphics& g) override;
    void resized () override;

    std::function<void(float)> onGainChanged;

private:
    TrackInfo    current;
    bool         hasTrack  { false };
    juce::String statusText;

    juce::Slider volumeSlider;

    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kTealGlow = 0xff1DC8DC;
    static constexpr uint32_t kText     = 0xffF5F5F0;
    static constexpr uint32_t kBG       = 0xff0e0e0e;
};

//==============================================================================
// MorphBar — top strip: MORPH toggle + KEY/TEMPO chips + SYNC mode button
//==============================================================================
class MorphBar : public juce::Component
{
public:
    std::function<void(bool morphOn, bool keyOn, bool tempoOn)> onChange;
    std::function<void()> onSyncToggled;

    MorphBar();
    void paint   (juce::Graphics& g) override;
    void resized () override;
    void mouseDown (const juce::MouseEvent& e) override;

    void setHostBPM  (double bpm);
    void setTargetKey (const juce::String& key);
    void setSyncMode  (bool isTrack);

    bool isMorphOn()  const noexcept { return morphOn; }
    bool isKeyOn()    const noexcept { return keyOn;   }
    bool isTempoOn()  const noexcept { return tempoOn; }
    juce::String getTargetKey() const { return targetKey; }
    double       getTargetBPM() const { return targetBPM; }
    int          getSemitones() const;

private:
    bool         morphOn     { false };
    bool         keyOn       { true  };
    bool         tempoOn     { true  };
    bool         isTrackMode { false };
    double       targetBPM   { 120.0 };
    juce::String targetKey   { "Cmin" };

    juce::Rectangle<int> morphBtnBounds;
    juce::Rectangle<int> keyChipBounds;
    juce::Rectangle<int> tempoChipBounds;
    juce::Rectangle<int> syncBtnBounds;

    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kTealGlow = 0xff1DC8DC;
    static constexpr uint32_t kMauve    = 0xffA855F7;
    static constexpr uint32_t kMauveGlow= 0xffC084FC;
    static constexpr uint32_t kText     = 0xffF0F0EC;
    static constexpr uint32_t kBG       = 0xff080808;
    static constexpr uint32_t kBorder   = 0x1Affffff;
};

//==============================================================================
// LibraryComponent
//==============================================================================
class LibraryComponent  : public juce::Component,
                           public juce::ListBoxModel
{
public:
    explicit LibraryComponent (MorphAudioProcessor& processor);
    ~LibraryComponent() override = default;

    void paint   (juce::Graphics& g) override;
    void resized () override;

    void refresh();
    void setHostBPM         (double bpm);
    void setDetectedKey     (const juce::String& key);
    void setSyncMode        (bool isTrack);
    void setLoadStatus      (const juce::String& status);
    void setOnlineStatus    (bool online);
    /** Called from PluginEditor timer — caches waveform once the loaded track is ready. */
    void tickWaveformCache();

    std::function<void()> onNeedsReauth;

    // ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected,
                                              juce::Component* existing) override;
    void listBoxItemClicked (int, const juce::MouseEvent&) override {}

private:
    MorphAudioProcessor& proc;

    MorphBar         morphBar;
    juce::TextButton typeMelo     { "Melo"     };
    juce::TextButton typeBeat     { "Beat"     };
    juce::TextButton typeToplines { "Toplines" };
    juce::TextButton typeSongs    { "Songs"    };
    juce::TextEditor searchBar;
    juce::TextButton refreshButton;
    juce::ListBox    trackList;
    NowPlayingBar    nowPlaying;

    std::unique_ptr<DropZoneComponent> dropZone;

    juce::Array<TrackInfo> allTracks;
    juce::Array<TrackInfo> filteredTracks;

    juce::String currentlyPlayingId;
    double       currentHostBPM { 120.0 };

    juce::HashMap<juce::String, juce::File>         cachedWAVs;
    juce::HashMap<juce::String, juce::Array<float>> waveformCache;
    juce::Array<juce::String>                        exportingIds;

    bool loading      { false };
    bool apiError     { false };
    bool onlineStatus { true  };  // updated from PluginEditor connectivity check

    void onMorphChanged  (bool morphOn, bool keyOn, bool tempoOn);
    void filterTracks    (const juce::String& query);
    void onPlayClicked   (const TrackInfo& track);
    void onDragRequested (const TrackInfo& track);
    int  computeSemitones (const juce::String& trackKey, const juce::String& targetKey);

    static constexpr uint32_t kTeal     = 0xff1A90A0;
    static constexpr uint32_t kTealGlow = 0xff1DC8DC;
    static constexpr uint32_t kText     = 0xffF5F5F0;
    static constexpr uint32_t kBorder   = 0x1Affffff;
    static constexpr uint32_t kBG       = 0xff080808;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryComponent)
};

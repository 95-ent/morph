#pragma once

#include <JuceHeader.h>
#include <limits>
#include "WaterAPI.h"
#include "MorphEngine.h"

//==============================================================================
// MorphAudioProcessor — the DSP + state core.
//
// Audio thread  : processBlock() only.  Reads atomics; touches MorphEngine.
// Message thread: all network callbacks, UI interactions, login/logout.
// Background    : WaterAPI calls (spawned internally).
//==============================================================================
class MorphAudioProcessor  : public juce::AudioProcessor,
                             private juce::Timer
{
public:
    //==========================================================================
    MorphAudioProcessor();
    ~MorphAudioProcessor() override;

    //==========================================================================
    // AudioProcessor interface
    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Infinite tail forces Logic Pro to keep calling processBlock even on silent
    // tracks — required so hostBPM stays live when the track has no audio content.
    double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }

    int getNumPrograms()                              override { return 1; }
    int getCurrentProgram()                           override { return 0; }
    void setCurrentProgram (int)                      override {}
    const juce::String getProgramName (int)           override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Auth
    //==========================================================================
    /** Async email/password login.  callback(success, errorMessage) */
    void login (const juce::String& email,
                const juce::String& password,
                std::function<void(bool, juce::String)> callback);

    /** OAuth device flow — opens browser, polls until authorised.
        callback(success, errorMessage) dispatched on message thread. */
    void startOAuthFlow (std::function<void(bool, juce::String)> callback);

    /** Cancel an in-progress OAuth poll (e.g. user dismissed the login screen). */
    void cancelOAuthFlow();

    void logout();

    bool            isLoggedIn() const noexcept { return loggedIn.load(); }
    juce::String    getAuthToken() const        { return authToken; }

    /** Refresh the access token using the stored refresh token.
        callback(success) dispatched on message thread. */
    void refreshTokenIfNeeded (std::function<void(bool)> callback);

    //==========================================================================
    // Library
    //==========================================================================
    /** Async.  callback(success, tracks) dispatched on message thread. */
    void loadTracksFromAPI (std::function<void(bool, juce::Array<TrackInfo>)> callback);

    //==========================================================================
    // Upload
    //==========================================================================
    /** Async.  callback(success, loopId) dispatched on message thread. */
    void uploadFile (const juce::File& file,
                     std::function<void(bool, juce::String)> callback);

    //==========================================================================
    // Preview / Morph
    //==========================================================================
    /** Kick off loading + real-time playback for a track.
        demoHash distinguishes direct Supabase URL (set) vs proxy URL (empty).
        sourceBPM and semitones set the initial morph params. */
    void startPreview (const juce::String& trackId,
                       const juce::String& demoHash,
                       double              sourceBPM,
                       int                 semitones = 0);

    void stopPreview();

    bool isPreviewActive()    const noexcept { return previewActive.load(); }
    bool isStandaloneActive() const noexcept { return standaloneActive.load(); }
    juce::int64 getProcessBlockGen() const noexcept { return processBlockGen.load(); }
    void setStandaloneActive (bool b);

    // Morph enable flags — set by UI when Morph button / chip state changes
    void setMorphBPMEnabled (bool b) noexcept { morphBPMEnabled.store (b); }
    void setMorphKeyEnabled (bool b) noexcept { morphKeyEnabled.store (b); }

    // Preview output gain (0.0–1.0), set by volume slider
    void setPreviewGain (float g) noexcept { previewGain.store (juce::jlimit (0.0f, 1.0f, g)); }

    // Update semitones without reloading (called when user changes key in MorphBar while playing)
    void setPreviewSemitones (int s) noexcept { previewSemitones.store (s); }

    // Project-mode target key (user-selected in plugin UI)
    void         setProjectKey (const juce::String& k);
    juce::String getProjectKey() const;

    /** Export a morphed WAV to /tmp and return the file. */
    juce::File exportMorphedWAV (const juce::String& trackId,
                                  const juce::String& demoHash,
                                  double              sourceBPM,
                                  double              targetBPM,
                                  int                 targetKey);

    //==========================================================================
    // Reference mode
    //==========================================================================
    enum class ReferenceMode { kProject, kTrack };

    void          setReferenceMode (ReferenceMode m) noexcept { referenceMode.store (m); }
    ReferenceMode getReferenceMode() const noexcept           { return referenceMode.load(); }

    /** Returns the BPM that drives morphing: host BPM in Project mode,
        BRAIN-detected track BPM in Track mode (120 until first result). */
    double getEffectiveBPM() const noexcept;

    /** Returns the detected key string ("Dmin", "Cmaj", etc.) or "?" until first result. */
    juce::String getDetectedKey() const;

    /** True while BRAIN analysis subprocess is running. */
    bool isAnalysing() const noexcept { return analysisInProgress.load(); }


    /** Key signature read from Logic's project (sharpsOrFlats*2 + (major?0:1)), -1 = unavailable. */
    static juce::String decodeHostKey (int encoded) noexcept;

    /** Returns a downsampled waveform (numPoints floats, 0..1) for the currently loaded
        track. Empty array if no track is loaded or the track doesn't match trackId. */
    juce::Array<float> getWaveformForLoadedTrack (int numPoints) const;
    juce::String       getCurrentlyLoadedTrackId() const { return morphEngine.loadedTrackId(); }

    //==========================================================================
    // Host BPM — updated in processBlock AND readable from any thread via
    // getPlayHead (Logic Pro exposes project tempo even when transport stopped).
    //==========================================================================
    double getHostBPM() const noexcept { return hostBPM.load(); }

    /** Reads the live project BPM via getPlayHead(). Falls back to the cached
        hostBPM if the playhead is unavailable (safe to call from any thread). */
    double readCurrentBPM() const noexcept;

    /** Reads ppqPosition from the playhead on the message thread.
        Returns 0.0 if the playhead is unavailable. */
    double readCurrentPPQPosition() const noexcept;

    /** Returns true if the DAW transport is currently playing.
        Must only be called from the message thread. */
    bool getIsPlaying() const noexcept;

    /** Analyze an audio file directly — reads the full file, runs BRAIN script,
        updates detectedTrackBPM / detectedTrackKey without needing playback.
        Runs asynchronously; result available seconds later via getDetectedKey(). */
    void analyzeFileDirectly (const juce::File& audioFile);

    //==========================================================================
    // Preview load status (for UI feedback)
    //==========================================================================
    enum class PreviewStatus { Idle, Loading, Ready, Failed };
    PreviewStatus getPreviewStatus() const noexcept { return previewStatus.load(); }

    //==========================================================================
    // Cached track list (for UI; updated by loadTracksFromAPI callback)
    //==========================================================================
    juce::Array<TrackInfo> getCachedTracks() const;
    void setCachedTracks (const juce::Array<TrackInfo>& tracks);

private:
    // juce::Timer — polls playhead for BPM on message thread every 250ms so
    // the display updates when the user changes tempo without playing audio.
    void timerCallback() override;

    //==========================================================================
    // Persistent settings
    //==========================================================================
    juce::ApplicationProperties appProperties;
    void                        loadPersistedTokens();
    void                        persistTokens (const juce::String& access, const juce::String& refresh);

    // OAuth poll helper — schedules next poll 2s later, recurses until success/timeout
    void scheduleOAuthPoll (const juce::String& sessionId,
                            juce::int64         deadline,
                            std::function<void(bool, juce::String)> cb);

    //==========================================================================
    // State
    //==========================================================================
    juce::String                  authToken;
    juce::String                  refreshToken;
    std::atomic<bool>             loggedIn         { false };
    std::atomic<bool>             oauthPollActive  { false };
    std::atomic<double>           hostBPM          { 120.0 };
    std::atomic<bool>             previewActive    { false };
    std::atomic<bool>             pendingPreviewStart { false };
    std::atomic<double>           previewSourceBPM { 120.0 };
    std::atomic<int>              previewSemitones { 0 };
    std::atomic<bool>             morphBPMEnabled  { false };
    std::atomic<bool>             morphKeyEnabled  { false };
    std::atomic<float>            previewGain      { 0.85f };
    std::atomic<ReferenceMode>    referenceMode    { ReferenceMode::kProject };
    std::atomic<PreviewStatus>    previewStatus    { PreviewStatus::Idle };
    mutable juce::CriticalSection projKeyLock;
    juce::String                  projectKey       { "" };  // empty until BRAIN auto-detects

    //==========================================================================
    // BRAIN analysis — same pipeline as detect-bpm-key.py
    // Audio thread writes to analysisBuffer; background thread calls Python.
    //==========================================================================
    static constexpr int kAnalysisSec = 5;    // seconds of audio before first BRAIN pass

    // Buffer filled on audio thread (mono, downmixed)
    std::vector<float>      analysisBuffer;
    int                     analysisWritePos  { 0 };
    double                  sampleRateCache   { 44100.0 };

    // Results — guarded by analysisLock
    mutable juce::CriticalSection analysisLock;
    double       detectedTrackBPM { 120.0 };
    juce::String detectedTrackKey { "?" };

    std::atomic<bool> analysisInProgress { false };
    double            lastAnalysisTime   { -60.0 };  // seconds since plugin start
    double            pluginRunTimeSec   { 0.0 };    // incremented in processBlock

    // Trigger a background BRAIN analysis of the captured audio
    void maybeScheduleAnalysis() noexcept;
    void runBrainAnalysis (std::vector<float> monoBuffer, double sr);

    //==========================================================================
    // Engine
    //==========================================================================
    MorphEngine morphEngine;

    //==========================================================================
    // Standalone preview path — plays audio even when DAW transport is stopped.
    // standaloneActive=true  → MorphStandaloneSource owns morphEngine
    // standaloneActive=false → processBlock owns morphEngine (DAW playing)
    //==========================================================================
    std::atomic<bool>     standaloneActive    { false };
    std::atomic<juce::int64> processBlockGen  { 0 };
    bool                  standaloneDeviceOpen { false };

    // Companion sync — processor timer keeps companion connected even
    // when the editor window is closed.
    int  companionSyncTick { 0 };
    bool companionLastPlay { false };

    std::unique_ptr<juce::AudioSource> standaloneSource;
    juce::AudioDeviceManager           standaloneDevice;
    juce::AudioSourcePlayer            standalonePlayer;

    void startStandalonePreview();
    void stopStandalonePreview();

    friend struct MorphStandaloneSource;

    //==========================================================================
    // Cached tracks (guarded by tracksLock)
    //==========================================================================
    mutable juce::CriticalSection  tracksLock;
    juce::Array<TrackInfo>         cachedTracks;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphAudioProcessor)
};

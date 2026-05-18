#pragma once

#include <JuceHeader.h>
#include "../Libs/signalsmith-stretch/signalsmith-stretch.h"

//==============================================================================
// MorphEngine — wraps Signalsmith Stretch for real-time preview playback and
// offline WAV export.  Audio-thread access is protected via CriticalSection.
//
// Lifecycle:
//   1. loadTrack()    — download + decode audio to an in-memory AudioBuffer.
//   2. setMorphParams() — compute stretch ratio and pitch shift.
//   3. processBlock()  — called from the audio thread each block.
//   4. exportWAV()    — offline render to /tmp; returns the file path.
//==============================================================================
class MorphEngine
{
public:
    MorphEngine();
    ~MorphEngine();

    //==========================================================================
    // Configuration (called once when the processor is prepared)
    //==========================================================================
    void prepare (double sampleRate, int blockSize);

    //==========================================================================
    // Track loading — runs on a background thread; returns true on success.
    // Caller should call this from off the audio thread.
    //==========================================================================
    bool loadTrack (const juce::String& trackId,
                    const juce::String& demoHash,
                    const juce::String& token);

    //==========================================================================
    // Set time-stretch and pitch-shift parameters.
    // sourceBPM      — the BPM stored in the track metadata
    // targetBPM      — the host's current BPM (atomic read from processor)
    // semitoneOffset — +/- semitones to shift pitch
    //==========================================================================
    void setMorphParams (double sourceBPM,
                         double targetBPM,
                         int    semitoneOffset);

    //==========================================================================
    // Audio-thread callback — mixes morphed audio into buffer.
    // Returns true while there is still source audio to play.
    //==========================================================================
    bool processBlock (juce::AudioBuffer<float>& buffer,
                       int numSamples);

    //==========================================================================
    // Seek back to the beginning of the loaded track.
    //==========================================================================
    void reset();

    //==========================================================================
    // Offline export — renders full track and writes a 44100 Hz stereo WAV.
    // Returns an invalid File on failure.
    //==========================================================================
    juce::File exportWAV (const juce::String& trackId,
                          double targetBPM,
                          int    targetKey);

    //==========================================================================
    bool isTrackLoaded() const noexcept { return trackLoaded.load(); }
    juce::String loadedTrackId() const  { return currentTrackId; }

    /** Returns peak-amplitude waveform data — numPoints floats in [0,1].
        Safe to call from the message thread (reads under engineLock). */
    juce::Array<float> getWaveformData (int numPoints) const;

private:
    // Format infrastructure
    juce::AudioFormatManager formatManager;

    // Source audio (decoded from network, 32-bit float stereo at source SR)
    juce::AudioBuffer<float> sourceBuffer;
    double                    sourceSampleRate { 44100.0 };
    int                       readHead         { 0 };

    // Signalsmith stretch instances — one for real-time, one for offline export
    signalsmith::stretch::SignalsmithStretch<float> realtimeStretch;

    // Morph parameters
    double stretchRatio  { 1.0 };
    double pitchFactor   { 1.0 };   // semitone-derived linear pitch multiplier

    // Host configuration
    double hostSampleRate { 44100.0 };
    int    hostBlockSize  { 512 };

    // State flags
    std::atomic<bool> trackLoaded { false };
    juce::String      currentTrackId;

    // Thread safety for stretch state and readHead
    juce::CriticalSection engineLock;

    // Download + decode a URL to sourceBuffer; called on background thread
    bool downloadAndDecode (const juce::URL& audioURL, const juce::String& token = {});

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphEngine)
};

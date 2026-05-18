#pragma once
#include <JuceHeader.h>

struct LocalDetectResult
{
    double       bpm = 120.0;
    juce::String key = "";   // e.g. "Dmin", "Cmaj", "" = not enough audio yet
};

/**
 * Offline BPM + key detection using JUCE's FFT — no Python, no network.
 *
 * BPM : spectral-flux onset envelope → autocorrelation peak in 50–220 BPM range
 *       + half/double-time correction heuristic.
 * Key : chromagram (pitch-class profile via STFT) correlated against
 *       Krumhansl-Schmuckler major and minor tonal profiles for all 12 roots.
 *
 * Typical accuracy: BPM ±1–2, key correct ~80 % on produced material.
 * Requires at least ~5 s of audio; 20 s gives best results.
 */
LocalDetectResult localDetectBpmAndKey (const float* mono,
                                        int          numSamples,
                                        double       sampleRate);

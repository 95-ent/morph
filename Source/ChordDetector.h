#pragma once
#include <JuceHeader.h>

//==============================================================================
// CHORDS — source unique de vérité pour détection accords + KEY + (futur) MIDI
//
// Utilisé dans : plugin Morph (C++, offline, sandbox-safe)
//                Water ingest pipeline (voir chord_detector.py — même algo)
//
// Algorithme :
//   Pass 1 — FFT 32768 pts / hop 2048 → chroma par frame + spectral flux
//   Pass 2 — Rejet frames percussives (flux > médiane×3) pour isoler harmonie
//   Pass 3 — Cosine similarity contre 24 templates triade (12 maj + 12 min)
//   Pass 4 — Scoring chord-key (table compatibilité musicale, tonic=5, VII=4…)
//   Fallback — K-S Pearson sur chroma accumulé si peu d'accords détectés
//   BPM    — autocorrélation onset flux (toutes frames, drums utiles ici)
//
// Précision attendue : KEY ~85-90% sur material produit, BPM ±1-2 BPM
// NEVER change kRefHz (261.626 = C4). Changing it to 440 shifts all keys +9.
//==============================================================================

struct ChordDetectResult
{
    double       bpm = 120.0;
    juce::String key = "";    // e.g. "Emin", "Cmaj", "" = not enough audio yet
};

ChordDetectResult chordDetectBpmAndKey (const float* mono,
                                        int          numSamples,
                                        double       sampleRate);

// Legacy alias — kept for backwards compatibility during transition
using LocalDetectResult = ChordDetectResult;
inline LocalDetectResult localDetectBpmAndKey (const float* m, int n, double sr)
{
    return chordDetectBpmAndKey (m, n, sr);
}

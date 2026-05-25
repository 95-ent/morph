#define _USE_MATH_DEFINES
#include "ChordDetector.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

//==============================================================================
// CHORD-BASED KEY DETECTION (replaces weak chroma→K-S direct approach)
//
// Pipeline:
//   Pass 1 — FFT per frame → per-frame chroma + spectral flux
//   Pass 2 — reject percussive frames (drum transients pollute harmony)
//   Pass 3 — detect best triad chord per tonal frame (cosine similarity)
//   Pass 4 — score 24 keys by chord-key compatibility table (music theory)
//   Fallback — K-S correlation on accumulated chroma if chord data sparse
//   BPM    — autocorrelation of full onset envelope (drums help here)
//==============================================================================
static constexpr int    kFFTOrder = 15;         // 2^15 = 32768
static constexpr int    kFFTSize  = 1 << kFFTOrder;
static constexpr int    kHopSize  = 2048;
static constexpr double kRefHz    = 261.626;    // C4 — NEVER change to 440

// K-S profiles — fallback only, same as Python BRAIN MAJ/MIN_PROFILE
static const float kMaj[12] = { 6.35f,2.23f,3.48f,2.33f,4.38f,4.09f,
                                  2.52f,5.19f,2.39f,3.66f,2.29f,2.88f };
static const float kMin[12] = { 6.33f,2.68f,3.52f,5.38f,2.60f,3.53f,
                                  2.54f,4.75f,3.98f,2.69f,3.34f,3.17f };
static const char* kNotes[12] = { "C","Db","D","Eb","E","F",
                                    "Gb","G","Ab","A","Bb","B" };

//==============================================================================
// Chord-key compatibility
// Chord index: 0-11 = major (root 0-11), 12-23 = minor (root 0-11)
// Key   index: same encoding
// Returns weight 0-5 (0 = not diatonic, 5 = tonic)
//==============================================================================
static int chordKeyCompat (int chord, int key)
{
    int  cr   = chord % 12;
    bool cmaj = chord < 12;
    int  kr   = key   % 12;
    bool kmaj = key   < 12;
    int  iv   = (cr - kr + 12) % 12;   // chord root interval above key root

    if (kmaj)   // major key diatonic chords
    {
        if (cmaj)  { if (iv==0)  return 5;  // I
                     if (iv==5)  return 4;  // IV
                     if (iv==7)  return 4;  // V
                   }
        else       { if (iv==2)  return 3;  // ii
                     if (iv==4)  return 2;  // iii
                     if (iv==9)  return 3;  // vi
                     if (iv==11) return 1;  // vii°
                   }
    }
    else        // natural minor key diatonic chords
    {
        if (!cmaj) { if (iv==0)  return 5;  // i
                     if (iv==5)  return 3;  // iv
                     if (iv==7)  return 2;  // v
                     if (iv==2)  return 1;  // ii°
                   }
        else       { if (iv==3)  return 2;  // III
                     if (iv==8)  return 3;  // VI
                     if (iv==10) return 4;  // VII
                   }
    }
    return 0;
}

//==============================================================================
static float pearsonCorr (const float* a, const float* b, int n)
{
    float ma = 0.0f, mb = 0.0f;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n;  mb /= n;
    float num = 0.0f, varA = 0.0f, varB = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float ea = a[i] - ma, eb = b[i] - mb;
        num += ea * eb;  varA += ea * ea;  varB += eb * eb;
    }
    float denom = std::sqrt (varA * varB);
    return denom < 1e-9f ? 0.0f : num / denom;
}

//==============================================================================
ChordDetectResult chordDetectBpmAndKey (const float* mono,
                                        int          numSamples,
                                        double       sampleRate)
{
    if (numSamples < kFFTSize) return {};

    std::vector<float> hann (kFFTSize);
    for (int i = 0; i < kFFTSize; ++i)
        hann[i] = 0.5f * (1.0f - std::cos (2.0f * float (M_PI) * i / (kFFTSize - 1)));

    juce::dsp::FFT fft (kFFTOrder);
    std::vector<float> fftBuf (kFFTSize * 2);

    int numFrames = (numSamples - kFFTSize) / kHopSize;
    if (numFrames <= 0) return {};

    // 24 chord templates: major triad = {root, M3 (+4), P5 (+7)}, minor = {root, m3 (+3), P5 (+7)}
    // Each template is L2-normalised (3 active bins → factor = 1/sqrt(3))
    static constexpr float kInvSqrt3 = 0.57735027f;
    std::vector<std::array<float, 12>> tmpl (24);
    for (int r = 0; r < 12; ++r)
    {
        tmpl[r].fill (0.0f);
        tmpl[r][r % 12]          = kInvSqrt3;
        tmpl[r][(r + 4) % 12]    = kInvSqrt3;   // major third
        tmpl[r][(r + 7) % 12]    = kInvSqrt3;   // perfect fifth

        tmpl[r + 12].fill (0.0f);
        tmpl[r + 12][r % 12]     = kInvSqrt3;
        tmpl[r + 12][(r+3) % 12] = kInvSqrt3;   // minor third
        tmpl[r + 12][(r+7) % 12] = kInvSqrt3;
    }

    // Per-frame storage (~32 KB for 15 s at 44100/2048)
    std::vector<std::array<double, 12>> frameCh (numFrames);
    std::vector<float>                  onsetEnv (numFrames, 0.0f);
    std::vector<float>                  prevMag  (kFFTSize / 2 + 1, 0.0f);

    //==========================================================================
    // Pass 1 — FFT all frames: chroma + spectral flux
    //==========================================================================
    for (int frame = 0; frame < numFrames; ++frame)
    {
        const float* src = mono + frame * kHopSize;
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < kFFTSize; ++i)
            fftBuf[i] = src[i] * hann[i];

        fft.performFrequencyOnlyForwardTransform (fftBuf.data());
        const int numBins = kFFTSize / 2;

        float peakMag = 0.0f;
        for (int b = 1; b < numBins; ++b)
            if (fftBuf[b] > peakMag) peakMag = fftBuf[b];
        float noiseFloor = peakMag * peakMag * 1e-3f;

        // Spectral flux
        float flux = 0.0f;
        for (int b = 1; b < numBins; ++b)
        {
            float diff = fftBuf[b] - prevMag[b];
            if (diff > 0.0f) flux += diff;
            prevMag[b] = fftBuf[b];
        }
        onsetEnv[frame] = flux;

        // Chroma with inverse-freq weighting (fundmentals > harmonics, like CQT)
        auto& ch = frameCh[frame];
        ch.fill (0.0);
        for (int bin = 1; bin < numBins; ++bin)
        {
            double freq = bin * sampleRate / kFFTSize;
            if (freq < 27.5 || freq > 4200.0) continue;
            float power = fftBuf[bin] * fftBuf[bin];
            if (power < noiseFloor) continue;

            double pc = 12.0 * std::log2 (freq / kRefHz);
            pc = std::fmod (pc, 12.0);
            if (pc < 0.0) pc += 12.0;

            int    lo   = (int) pc % 12;
            int    hi   = (lo + 1) % 12;
            double frac = pc - std::floor (pc);
            double w    = kRefHz / std::max (kRefHz, freq);   // 1.0 at C4, falls above

            ch[lo] += power * (1.0 - frac) * w;
            ch[hi] += power * frac          * w;
        }
    }

    //==========================================================================
    // Pass 2 — reject percussive frames (flux > median × 3)
    // Median is more robust than mean when hard drum hits skew the average
    //==========================================================================
    std::vector<float> sf = onsetEnv;
    std::sort (sf.begin(), sf.end());
    float medFlux  = sf[numFrames / 2];
    float fluxThr  = std::max (medFlux * 3.0f, 1e-6f);

    //==========================================================================
    // Pass 3 — chord detection on tonal frames
    //==========================================================================
    int    chordHist[24]   = {};   // how many frames each chord was detected
    double chromaTotal[12] = {};   // accumulated chroma for K-S fallback

    for (int frame = 0; frame < numFrames; ++frame)
    {
        if (onsetEnv[frame] > fluxThr) continue;   // skip percussive frame

        const auto& ch = frameCh[frame];

        // L2-normalise frame chroma
        double norm = 0.0;
        for (int i = 0; i < 12; ++i) norm += ch[i] * ch[i];
        if (norm < 1e-9) continue;
        norm = std::sqrt (norm);

        float cn[12];
        for (int i = 0; i < 12; ++i)
        {
            cn[i] = float (ch[i] / norm);
            chromaTotal[i] += ch[i];
        }

        // Cosine similarity with 24 chord templates (templates already unit-norm)
        float bestSim = 0.0f;
        int   bestC   = -1;
        for (int c = 0; c < 24; ++c)
        {
            float sim = 0.0f;
            for (int i = 0; i < 12; ++i) sim += cn[i] * tmpl[c][i];
            if (sim > bestSim) { bestSim = sim; bestC = c; }
        }
        // Threshold > flat-chroma baseline (~0.50) to require real harmonic structure
        if (bestC >= 0 && bestSim > 0.55f)
            chordHist[bestC]++;
    }

    //==========================================================================
    // Pass 4 — key scoring: chord-key compatibility (primary) + K-S (fallback)
    //==========================================================================
    int totalChordVotes = 0;
    for (int c = 0; c < 24; ++c) totalChordVotes += chordHist[c];

    float keyScore[24] = {};

    // Primary: sum chord votes × compatibility weight
    if (totalChordVotes > 0)
        for (int k = 0; k < 24; ++k)
            for (int c = 0; c < 24; ++c)
                keyScore[k] += float (chordHist[c]) * chordKeyCompat (c, k);

    // Secondary: K-S Pearson correlation on accumulated chroma
    // Weighted so chord votes dominate when abundant; K-S leads when chords sparse
    {
        float chromaF[12];
        double sum = 0;
        for (int i = 0; i < 12; ++i) sum += chromaTotal[i];
        for (int i = 0; i < 12; ++i)
            chromaF[i] = (sum > 0) ? float (chromaTotal[i] / sum) : 0.0f;

        // Scale: chord score can reach totalChordVotes*5; keep K-S influence small
        float ksW = (totalChordVotes > 10) ? float (totalChordVotes) * 0.05f : 5.0f;

        for (int root = 0; root < 12; ++root)
        {
            float rot[12];
            for (int i = 0; i < 12; ++i) rot[i] = chromaF[(i + root) % 12];
            keyScore[root]      += pearsonCorr (rot, kMaj, 12) * ksW;
            keyScore[root + 12] += pearsonCorr (rot, kMin, 12) * ksW;
        }
    }

    // Best key
    int bestK = 0;
    for (int k = 1; k < 24; ++k)
        if (keyScore[k] > keyScore[bestK]) bestK = k;

    // Always display as minor: if major detected, convert to relative minor (same notes, root -3 st)
    int displayK = (bestK < 12) ? (((bestK + 9) % 12) + 12) : bestK;
    juce::String key = juce::String (kNotes[displayK % 12]) + "min";

    //==========================================================================
    // BPM — autocorrelation of full onset envelope (all frames, drums included)
    //==========================================================================
    double bpm = 120.0;
    int    N   = (int) onsetEnv.size();

    if (N > 8)
    {
        double framesPerSec = sampleRate / kHopSize;
        int    lagMin = std::max (1, (int) (framesPerSec * 60.0 / 220.0));
        int    lagMax = std::min (N / 2, (int) (framesPerSec * 60.0 / 50.0));

        float bestAC  = 0.0f;
        int   bestLag = lagMin;
        for (int lag = lagMin; lag <= lagMax; ++lag)
        {
            float ac = 0.0f;
            for (int i = 0; i < N - lag; ++i)
                ac += onsetEnv[i] * onsetEnv[i + lag];
            if (ac > bestAC) { bestAC = ac; bestLag = lag; }
        }

        bpm = framesPerSec * 60.0 / bestLag;
        if (bpm < 80.0  && bpm * 2.0 <= 220.0) bpm *= 2.0;
        if (bpm > 160.0 && bpm / 2.0 >=  50.0) bpm /= 2.0;
    }

    return { bpm, key };
}

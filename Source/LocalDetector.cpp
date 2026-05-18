#include "LocalDetector.h"
#include <cmath>

//==============================================================================
// Constants
//==============================================================================
static constexpr int kFFTOrder = 11;                  // 2^11 = 2048
static constexpr int kFFTSize  = 1 << kFFTOrder;
static constexpr int kHopSize  = 512;

// Krumhansl-Schmuckler tonal profiles
static const float kMaj[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
                                  2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
static const float kMin[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
                                  2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
static const char* kNotes[12] = { "C","Db","D","Eb","E","F",
                                    "Gb","G","Ab","A","Bb","B" };

//==============================================================================
// Helpers
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
        num  += ea * eb;
        varA += ea * ea;
        varB += eb * eb;
    }
    float denom = std::sqrt (varA * varB);
    return denom < 1e-9f ? 0.0f : num / denom;
}

//==============================================================================
LocalDetectResult localDetectBpmAndKey (const float* mono,
                                        int          numSamples,
                                        double       sampleRate)
{
    if (numSamples < kFFTSize)
        return {};

    // --- Hann window ---
    std::vector<float> hann (kFFTSize);
    for (int i = 0; i < kFFTSize; ++i)
        hann[i] = 0.5f * (1.0f - std::cos (2.0f * float (M_PI) * i / (kFFTSize - 1)));

    juce::dsp::FFT fft (kFFTOrder);
    // performFrequencyOnlyForwardTransform needs a buffer of 2*kFFTSize floats
    std::vector<float> fftBuf (kFFTSize * 2);

    int numFrames = (numSamples - kFFTSize) / kHopSize;

    double chroma[12] = {};
    std::vector<float> onsetEnv;
    onsetEnv.reserve (numFrames);
    std::vector<float> prevMag (kFFTSize / 2 + 1, 0.0f);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const float* src = mono + frame * kHopSize;

        // Windowed frame → FFT buffer (zero-pad imaginary half)
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < kFFTSize; ++i)
            fftBuf[i] = src[i] * hann[i];

        // In-place: first kFFTSize/2+1 elements become magnitudes
        fft.performFrequencyOnlyForwardTransform (fftBuf.data());

        // --- Chromagram ---
        for (int bin = 1; bin < kFFTSize / 2; ++bin)
        {
            double freq = bin * sampleRate / kFFTSize;
            if (freq < 27.5 || freq > 4200.0) continue;       // piano range A0–C8
            double midi  = 12.0 * std::log2 (freq / 440.0) + 69.0;
            int    pc    = ((int) std::round (midi) % 12 + 12) % 12;
            chroma[pc]  += fftBuf[bin];
        }

        // --- Spectral-flux onset ---
        float flux = 0.0f;
        for (int bin = 1; bin < kFFTSize / 2; ++bin)
        {
            float diff = fftBuf[bin] - prevMag[bin];
            if (diff > 0.0f) flux += diff;
            prevMag[bin] = fftBuf[bin];
        }
        onsetEnv.push_back (flux);
    }

    //==========================================================================
    // Key — Krumhansl-Schmuckler correlation
    //==========================================================================
    float chromaF[12];
    {
        double sum = 0;
        for (int i = 0; i < 12; ++i) sum += chroma[i];
        for (int i = 0; i < 12; ++i)
            chromaF[i] = (sum > 0) ? float (chroma[i] / sum) : 0.0f;
    }

    float bestCorr = -2.0f;
    int   bestRoot = 0;
    bool  bestMaj  = true;

    for (int root = 0; root < 12; ++root)
    {
        // Rotate chromagram so this root is at index 0
        float rotated[12];
        for (int i = 0; i < 12; ++i)
            rotated[i] = chromaF[(i + root) % 12];

        float cm = pearsonCorr (rotated, kMaj, 12);
        float cn = pearsonCorr (rotated, kMin, 12);
        if (cm > bestCorr) { bestCorr = cm; bestRoot = root; bestMaj = true;  }
        if (cn > bestCorr) { bestCorr = cn; bestRoot = root; bestMaj = false; }
    }

    juce::String key = juce::String (kNotes[bestRoot]) + (bestMaj ? "maj" : "min");

    //==========================================================================
    // BPM — autocorrelation of onset envelope
    //==========================================================================
    double bpm  = 120.0;
    int    N    = (int) onsetEnv.size();

    if (N > 8)
    {
        double framesPerSec = sampleRate / kHopSize;
        int    lagMin       = std::max (1, (int) (framesPerSec * 60.0 / 220.0));
        int    lagMax       = std::min (N / 2, (int) (framesPerSec * 60.0 / 50.0));

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

        // Half/double-time heuristic
        if (bpm < 80.0  && bpm * 2.0 <= 220.0) bpm *= 2.0;
        if (bpm > 160.0 && bpm / 2.0 >=  50.0) bpm /= 2.0;
    }

    return { bpm, key };
}

#include "MorphEngine.h"
#include "WaterAPI.h"

//==============================================================================
MorphEngine::MorphEngine()
{
    // Use only embedded pure-C++ decoders — Logic's AU sandbox blocks AudioToolbox,
    // which CoreAudioFormat (registered by registerBasicFormats) depends on.
    formatManager.registerFormat (new juce::WavAudioFormat(),  true);
    formatManager.registerFormat (new juce::AiffAudioFormat(), true);
#if JUCE_USE_MP3AUDIOFORMAT
    formatManager.registerFormat (new juce::MP3AudioFormat(), false);
#endif
}

MorphEngine::~MorphEngine() {}

//==============================================================================
void MorphEngine::prepare (double sampleRate, int blockSize)
{
    const juce::ScopedLock sl (engineLock);
    hostSampleRate = sampleRate;
    hostBlockSize  = blockSize;

    realtimeStretch.presetDefault (2, static_cast<float> (sampleRate));
    realtimeStretch.setTransposeFactor (1.0f);  // identity pitch on init
}

//==============================================================================
bool MorphEngine::loadTrack (const juce::String& trackId,
                              const juce::String& demoHash,
                              const juce::String& token)
{
    bool loaded = false;

    // 1) Try direct Supabase public URL (no auth) when demo_hash is set
    if (demoHash.isNotEmpty())
    {
        juce::URL directURL = WaterAPI::getAudioStreamURL (trackId, demoHash);
        loaded = downloadAndDecode (directURL, {});
    }

    // 2) Fall back to proxy URL with Bearer auth
    if (!loaded)
    {
        juce::URL proxyURL = WaterAPI::getAudioStreamURL (trackId, {});
        loaded = downloadAndDecode (proxyURL, token);
    }

    if (!loaded)
        return false;

    const juce::ScopedLock sl (engineLock);
    readHead       = 0;
    currentTrackId = trackId;
    trackLoaded.store (true);

    realtimeStretch.reset();
    realtimeStretch.setTransposeFactor (static_cast<float> (pitchFactor));

    return true;
}

//==============================================================================
bool MorphEngine::downloadAndDecode (const juce::URL& audioURL, const juce::String& token)
{
    int statusCode = 0;
    juce::String authHeader = token.isNotEmpty() ? ("Authorization: Bearer " + token + "\r\n") : juce::String{};
    auto netStream = audioURL.createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withExtraHeaders (authHeader)
            .withConnectionTimeoutMs (30000)
            .withStatusCode (&statusCode));

    if (netStream == nullptr || (statusCode != 0 && (statusCode < 200 || statusCode >= 300)))
    {
        juce::File ("/tmp/morph_debug.txt").appendText (
            juce::Time::getCurrentTime().toString (true, true)
            + "  FAIL-NET  status:" + juce::String (statusCode)
            + "  " + audioURL.toString (true) + "\n");
        return false;
    }

    // Download entirely into memory — no temp file, no sandbox file-access issues
    juce::MemoryOutputStream memOut;
    memOut.writeFromInputStream (*netStream, -1);
    const size_t dataSize = memOut.getDataSize();

    // Log size + first 16 bytes for format identification
    {
        juce::String header;
        const auto* bytes = static_cast<const uint8_t*> (memOut.getData());
        for (size_t i = 0; i < juce::jmin ((size_t) 16, dataSize); ++i)
            header += juce::String::toHexString (bytes[i]) + " ";

        juce::File ("/tmp/morph_debug.txt").appendText (
            juce::Time::getCurrentTime().toString (true, true)
            + "  DOWNLOADED  " + audioURL.toString (true)
            + "  size:" + juce::String ((int) dataSize)
            + "  bytes:[" + header.trim() + "]\n");
    }

    // Decode directly from memory. MemoryInputStream (no copy) points into memOut's buffer;
    // memOut outlives the reader because it is declared first in this scope.
    auto* reader = formatManager.createReaderFor (
        std::make_unique<juce::MemoryInputStream> (memOut.getData(), dataSize, false));

    if (reader == nullptr)
    {
        juce::File ("/tmp/morph_debug.txt").appendText (
            juce::Time::getCurrentTime().toString (true, true)
            + "  FAIL-DECODE  createReaderFor null (in-memory)\n");
        return false;
    }

    juce::File ("/tmp/morph_debug.txt").appendText (
        juce::Time::getCurrentTime().toString (true, true)
        + "  SUCCESS  " + audioURL.toString (true)
        + "  sampleRate:" + juce::String (reader->sampleRate)
        + "  length:" + juce::String ((int) reader->lengthInSamples) + "\n");

    std::unique_ptr<juce::AudioFormatReader> readerOwner (reader);

    const juce::ScopedLock sl (engineLock);
    double fileSampleRate = reader->sampleRate;

    int rawLen = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> rawBuf (2, rawLen);
    reader->read (&rawBuf, 0, rawLen, 0, true, true);

    if (std::abs (fileSampleRate - hostSampleRate) < 1.0)
    {
        sourceBuffer = std::move (rawBuf);
    }
    else
    {
        double ratio  = hostSampleRate / fileSampleRate;
        int    outLen = static_cast<int> (rawLen * ratio) + 2;
        sourceBuffer.setSize (2, outLen);

        juce::MemoryAudioSource rawSrc (rawBuf, false);
        juce::ResamplingAudioSource rawResampler (&rawSrc, false, 2);
        rawResampler.setResamplingRatio (fileSampleRate / hostSampleRate);
        rawResampler.prepareToPlay (outLen, hostSampleRate);

        juce::AudioSourceChannelInfo info (&sourceBuffer, 0, outLen);
        rawResampler.getNextAudioBlock (info);
    }

    sourceSampleRate = hostSampleRate;
    return true;
}

//==============================================================================
void MorphEngine::setMorphParams (double sourceBPM,
                                   double targetBPM,
                                   int    semitoneOffset)
{
    const juce::ScopedLock sl (engineLock);

    stretchRatio = (sourceBPM > 0.0 && targetBPM > 0.0)
                 ? (targetBPM / sourceBPM)
                 : 1.0;

    // Linear pitch multiplier from semitone offset: 2^(n/12)
    pitchFactor = std::pow (2.0, static_cast<double> (semitoneOffset) / 12.0);

    realtimeStretch.setTransposeFactor (static_cast<float> (pitchFactor));
}

//==============================================================================
bool MorphEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                 int numSamples)
{
    const juce::ScopedLock sl (engineLock);

    if (!trackLoaded.load() || sourceBuffer.getNumSamples() == 0)
        return false;

    const int totalSourceSamples = sourceBuffer.getNumSamples();
    const int srcChannels        = sourceBuffer.getNumChannels();
    const int outChannels        = buffer.getNumChannels();

    // How many source samples to consume for numSamples output at this stretch ratio?
    // stretchRatio = sourceBPM / targetBPM, so faster target BPM => ratio < 1 => fewer src needed.
    int sourceNeeded = static_cast<int> (std::ceil (numSamples * stretchRatio));
    if (sourceNeeded <= 0) return true; // ratio mismatch — keep alive, output silence

    // Wrap readHead if at or past end (seamless loop)
    if (readHead >= totalSourceSamples)
        readHead = 0;

    // Clamp to what's available from this position
    int sourceThisBlock = juce::jmin (sourceNeeded, totalSourceSamples - readHead);

    std::vector<const float*> inPtrs  (static_cast<size_t> (srcChannels));
    std::vector<float*>       outPtrs (static_cast<size_t> (outChannels));

    for (int ch = 0; ch < srcChannels; ++ch)
        inPtrs[static_cast<size_t> (ch)] = sourceBuffer.getReadPointer (ch, readHead);

    for (int ch = 0; ch < outChannels; ++ch)
        outPtrs[static_cast<size_t> (ch)] = buffer.getWritePointer (ch);

    realtimeStretch.process (inPtrs.data(),  sourceThisBlock,
                              outPtrs.data(), numSamples);

    readHead += sourceThisBlock;

    // Loop back to start when track ends
    if (readHead >= totalSourceSamples)
        readHead = 0;

    // Mirror mono to stereo
    if (srcChannels == 1 && outChannels >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    return true; // preview always loops
}

//==============================================================================
void MorphEngine::reset()
{
    const juce::ScopedLock sl (engineLock);
    readHead = 0;
    realtimeStretch.reset();
}

//==============================================================================
juce::File MorphEngine::exportWAV (const juce::String& trackId,
                                    double targetBPM,
                                    int    targetKey)
{
    const juce::ScopedLock sl (engineLock);

    if (!trackLoaded.load() || sourceBuffer.getNumSamples() == 0)
        return {};

    juce::File outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("morph-" + trackId
                                          + "-" + juce::String (static_cast<int> (targetBPM))
                                          + "-" + juce::String (targetKey)
                                          + ".wav");

    // Create the WAV writer
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (new juce::FileOutputStream (outFile),
                                   hostSampleRate,
                                   2,   // stereo
                                   24,  // bit depth
                                   {},
                                   0));

    if (writer == nullptr)
        return {};

    // Set up an offline stretch instance
    signalsmith::stretch::SignalsmithStretch<float> offlineStretch;
    offlineStretch.presetDefault (2, static_cast<float> (hostSampleRate));
    offlineStretch.setTransposeFactor (static_cast<float> (pitchFactor));

    const int   blockSize  = 512;
    const int   srcTotal   = sourceBuffer.getNumSamples();
    int         srcCursor  = 0;

    juce::AudioBuffer<float> outputBlock (2, blockSize);

    while (srcCursor < srcTotal)
    {
        int srcChunk = juce::jmin (blockSize, srcTotal - srcCursor);

        const float* inL = sourceBuffer.getReadPointer (0, srcCursor);
        const float* inR = sourceBuffer.getNumChannels() >= 2
                         ? sourceBuffer.getReadPointer (1, srcCursor)
                         : inL;

        const float* inputPtrs[2]  = { inL, inR };

        float* outL = outputBlock.getWritePointer (0);
        float* outR = outputBlock.getWritePointer (1);
        float* outputPtrs[2] = { outL, outR };

        // stretchRatio = sourceBPM / targetBPM; output = input / ratio samples
        int outSamples = static_cast<int> (srcChunk / stretchRatio);
        outSamples = juce::jmax (1, outSamples);
        outSamples = juce::jmin (outSamples, blockSize);

        offlineStretch.process (inputPtrs, srcChunk,
                                outputPtrs, outSamples);

        writer->writeFromAudioSampleBuffer (outputBlock, 0, outSamples);
        srcCursor += srcChunk;
    }

    // Flush remaining latency tail from the stretch engine
    {
        float* outL = outputBlock.getWritePointer (0);
        float* outR = outputBlock.getWritePointer (1);
        float* outputPtrs[2] = { outL, outR };
        offlineStretch.flush (outputPtrs, blockSize);
        writer->writeFromAudioSampleBuffer (outputBlock, 0, blockSize);
    }

    writer->flush();
    return outFile;
}

//==============================================================================
juce::Array<float> MorphEngine::getWaveformData (int numPoints) const
{
    const juce::ScopedLock sl (engineLock);

    juce::Array<float> result;
    if (!trackLoaded.load() || sourceBuffer.getNumSamples() == 0 || numPoints <= 0)
        return result;

    result.resize (numPoints);

    const int totalSamples = sourceBuffer.getNumSamples();
    const int numCh        = sourceBuffer.getNumChannels();

    for (int i = 0; i < numPoints; ++i)
    {
        int start = (int) ((int64_t) i       * totalSamples / numPoints);
        int end   = (int) ((int64_t) (i + 1) * totalSamples / numPoints);
        end = juce::jmin (end, totalSamples);

        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* ptr = sourceBuffer.getReadPointer (ch);
            for (int s = start; s < end; ++s)
                peak = juce::jmax (peak, std::abs (ptr[s]));
        }
        result.set (i, peak);
    }

    // Normalize so the loudest bar reaches 1.0
    float maxPeak = 0.0f;
    for (int i = 0; i < numPoints; ++i)
        maxPeak = juce::jmax (maxPeak, result[i]);
    if (maxPeak > 0.0f)
        for (int i = 0; i < numPoints; ++i)
            result.set (i, result[i] / maxPeak);

    return result;
}

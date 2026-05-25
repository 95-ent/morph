#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LocalDetector.h"
#include "CompanionLink.h"

//==============================================================================
// Standalone audio source — feeds morphEngine when DAW transport is stopped.
// Owns morphEngine only when proc.standaloneActive == true.
//==============================================================================
struct MorphStandaloneSource : juce::AudioSource
{
    MorphAudioProcessor& proc;
    explicit MorphStandaloneSource (MorphAudioProcessor& p) : proc (p) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        if (!proc.previewActive.load() || !proc.standaloneActive.load())
        {
            info.clearActiveBufferRegion();
            return;
        }

        double srcBPM = proc.previewSourceBPM.load();
        double tgtBPM = proc.morphBPMEnabled.load() ? proc.hostBPM.load() : srcBPM;
        int    sems   = proc.morphKeyEnabled.load() ? proc.previewSemitones.load() : 0;
        proc.morphEngine.setMorphParams (srcBPM, tgtBPM, sems);

        juce::AudioBuffer<float> tmp (info.buffer->getNumChannels(), info.numSamples);
        tmp.clear();
        proc.morphEngine.processBlock (tmp, info.numSamples);
        tmp.applyGain (proc.previewGain.load());

        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample, tmp, ch, 0, info.numSamples);
    }
};

//==============================================================================
MorphAudioProcessor::MorphAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Set up persistent ApplicationProperties (stores auth token between sessions)
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "MorphPlugin";
    opts.filenameSuffix      = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    opts.folderName          = "Water/Morph";
    appProperties.setStorageParameters (opts);

    loadPersistedTokens();
    startTimer (250);   // Poll playhead for BPM even when processBlock isn't running
}

MorphAudioProcessor::~MorphAudioProcessor()
{
    stopTimer();
    stopStandalonePreview();
    morphEngine.reset();
}

//==============================================================================
void MorphAudioProcessor::loadPersistedTokens()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        juce::String access  = settings->getValue ("authToken");
        juce::String refresh = settings->getValue ("refreshToken");
        if (access.isNotEmpty())
        {
            authToken    = access;
            refreshToken = refresh;
            loggedIn.store (true);
        }
    }
}

void MorphAudioProcessor::persistTokens (const juce::String& access,
                                          const juce::String& refresh)
{
    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue ("authToken",    access);
        settings->setValue ("refreshToken", refresh);
        settings->saveIfNeeded();
    }
}

//==============================================================================
void MorphAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    morphEngine.prepare (sampleRate, samplesPerBlock);
    sampleRateCache  = sampleRate;
    pluginRunTimeSec = 0.0;
    // Read project BPM at init — playhead is valid here in Logic Pro
    double initBPM = readCurrentBPM();
    hostBPM.store (initBPM > 0.0 ? initBPM : 120.0);
    lastAnalysisTime = -60.0;

    // Pre-allocate analysis buffer (mono, kAnalysisSec seconds)
    int bufSize = static_cast<int> (sampleRate * kAnalysisSec);
    analysisBuffer.assign (bufSize, 0.0f);
    analysisWritePos = 0;
}

void MorphAudioProcessor::releaseResources()
{
    previewActive.store (false);
    morphEngine.reset();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool MorphAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}
#endif

//==============================================================================
void MorphAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Track how often processBlock is called so the UI timer can detect DAW stop
    processBlockGen.fetch_add (1, std::memory_order_relaxed);

    // Always read host BPM via modern API; deprecated fallback for older hosts
    bool dawIsPlaying = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                if (*bpm > 0.0) hostBPM.store (*bpm);
            dawIsPlaying = pos->getIsPlaying();
        }
    }

    // If DAW just started playing while standalone owns morphEngine → hand off
    if (dawIsPlaying && standaloneActive.load())
        standaloneActive.store (false);

    // Advance plugin clock
    const double blockDuration = buffer.getNumSamples() / sampleRateCache;
    pluginRunTimeSec += blockDuration;

    // Always accumulate audio for key detection via BRAIN — works on all DAWs.
    // No API exposes project key reliably; we detect it from the audio itself.
    if (buffer.getNumChannels() > 0)
    {
        const int numSamples = buffer.getNumSamples();
        const int numCh      = buffer.getNumChannels();
        const int bufSize    = static_cast<int> (analysisBuffer.size());

        // Background thread signals reset on BRAIN failure — apply on audio thread
        if (analysisResetPending.exchange (false, std::memory_order_relaxed))
            analysisWritePos = 0;

        for (int i = 0; i < numSamples && analysisWritePos < bufSize; ++i)
        {
            float mono = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                mono += buffer.getReadPointer (ch)[i];
            analysisBuffer[analysisWritePos++] = mono / static_cast<float> (numCh);
        }

        // Expose fill progress to message thread (once per block, not per sample)
        analysisWritePosSnapshot.store (analysisWritePos, std::memory_order_relaxed);

        // Buffer full → trigger analysis (if not already running and cooldown elapsed)
        if (analysisWritePos >= bufSize)
            maybeScheduleAnalysis();
    }

    // Clear any extra output channels that have no matching input
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Quantized preview start — wait for next bar boundary when transport is rolling
    if (pendingPreviewStart.load() && !previewActive.load())
    {
        bool startNow = true;
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                if (pos->getIsPlaying())
                {
                    startNow = false;
                    if (auto ppq = pos->getPpqPosition())
                    {
                        double ppqInBeat  = std::fmod (*ppq, 1.0);  // beat-level sync (Splice-style)
                        double blockBeats = buffer.getNumSamples() / sampleRateCache
                                           * hostBPM.load() / 60.0;
                        if (ppqInBeat < blockBeats)
                            startNow = true;
                    }
                    else
                    {
                        startNow = true;  // no ppq → start immediately
                    }
                }
                // Transport stopped → start immediately
            }
        }
        if (startNow)
        {
            pendingPreviewStart.store (false);
            morphEngine.reset();
            previewActive.store (true);
        }
    }

    // Only process audio here when DAW owns morphEngine (standaloneActive == false)
    if (previewActive.load() && !standaloneActive.load())
    {
        double srcBPM    = previewSourceBPM.load();
        double targetBPM = morphBPMEnabled.load() ? hostBPM.load() : srcBPM;
        int    semitones = morphKeyEnabled.load() ? previewSemitones.load() : 0;
        morphEngine.setMorphParams (srcBPM, targetBPM, semitones);

        juce::AudioBuffer<float> previewBuf (buffer.getNumChannels(), buffer.getNumSamples());
        previewBuf.clear();
        morphEngine.processBlock (previewBuf, previewBuf.getNumSamples());
        previewBuf.applyGain (previewGain.load());
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.addFrom (ch, 0, previewBuf, ch, 0, buffer.getNumSamples());
    }
}

//==============================================================================
double MorphAudioProcessor::readCurrentBPM() const noexcept
{
    // Try the live playhead first — Logic Pro provides project tempo even when
    // transport is stopped, so this works regardless of playback state.
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                if (*bpm > 0.0) return *bpm;
        }
        // Fallback to deprecated API for older hosts
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (ph->getCurrentPosition (info) && info.bpm > 0.0)
            return info.bpm;
    }
    return hostBPM.load();
}

//==============================================================================
// Polls playhead on the message thread — catches BPM changes made in the DAW
// transport bar while audio is not flowing (no processBlock calls).
void MorphAudioProcessor::timerCallback()
{
    // 1 — Update BPM from live playhead (catches manual tempo changes in Logic)
    bool  playing  = false;
    double ppq     = 0.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                if (*bpm > 0.0) hostBPM.store (*bpm);
            playing = pos->getIsPlaying();
            if (auto p = pos->getPpqPosition()) ppq = *p;
        }
    }

    // 2 — Send TRANSPORT / SYNC to companion (keeps companion connected even
    //     when the editor window is closed; editor sends at 200 ms when open,
    //     but this 250 ms fallback fires regardless).
    {
        const double bpm      = getEffectiveBPM();
        const double timeSecs = (bpm > 0.0) ? (ppq / bpm * 60.0) : 0.0;

        if (playing)
        {
            CompanionLink::get().sendTransport (true, timeSecs, ppq);
            companionLastPlay = true;
        }
        else if (companionLastPlay)
        {
            companionLastPlay = false;
            CompanionLink::get().sendTransport (false, timeSecs, ppq);
        }

        if (++companionSyncTick >= 4)   // 4 × 250 ms = 1 s
        {
            companionSyncTick = 0;
            const bool trackMode = getReferenceMode() == ReferenceMode::kTrack;
            const juce::String key = trackMode ? getDetectedKey() : getProjectKey();
            CompanionLink::get().sendSync (bpm, key,
                                           trackMode ? "track" : "project",
                                           timeSecs);
        }
    }

    // 3 — Check if companion detected a new audio file (written by Logic audio watcher)
    juce::File ipcFile ("/tmp/water-morph-analyze-file.txt");
    if (ipcFile.existsAsFile())
    {
        auto path = ipcFile.loadFileAsString().trim();
        ipcFile.deleteFile();
        if (path.isNotEmpty())
            analyzeFileDirectly (juce::File (path));
    }
}

//==============================================================================
double MorphAudioProcessor::readCurrentPPQPosition() const noexcept
{
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto ppq = pos->getPpqPosition())
                return *ppq;
    return 0.0;
}

bool MorphAudioProcessor::getIsPlaying() const noexcept
{
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            return pos->getIsPlaying();
    return false;
}

//==============================================================================
double MorphAudioProcessor::getEffectiveBPM() const noexcept
{
    // Always prefer host playhead BPM — Logic provides project tempo even when
    // transport is stopped. Track mode keeps audio-based key detection but uses
    // the same authoritative BPM as Project mode to avoid vocal/drum analysis drift.
    double hostBpm = readCurrentBPM();
    if (hostBpm >= 50.0 && hostBpm <= 220.0)
        return hostBpm;

    // Playhead unavailable (rare) — fall back to audio-detected BPM.
    const juce::ScopedLock sl (analysisLock);
    return detectedTrackBPM;
}

juce::String MorphAudioProcessor::getDetectedKey() const
{
    const juce::ScopedLock sl (analysisLock);
    return detectedTrackKey;
}

double MorphAudioProcessor::getDetectedTrackBPM() const
{
    const juce::ScopedLock sl (analysisLock);
    return detectedTrackBPM;
}

float MorphAudioProcessor::getAnalysisProgress() const noexcept
{
    const int bufSize = static_cast<int> (analysisBuffer.size());
    if (bufSize <= 0) return 0.f;
    const int pos = analysisWritePosSnapshot.load (std::memory_order_relaxed);
    return juce::jlimit (0.f, 1.f, (float)pos / (float)bufSize);
}

//==============================================================================
// BRAIN Analysis — same pipeline as detect-bpm-key.py
//==============================================================================
void MorphAudioProcessor::maybeScheduleAnalysis() noexcept
{
    // Once BRAIN confirmed a key, never re-analyse — user can tap KEY to change manually
    if (brainKeyConfirmed.load()) return;
    if (analysisInProgress.load()) return;
    if (brainAttempts.load() >= 1) return;   // one shot only — never loop
    if ((pluginRunTimeSec - lastAnalysisTime) < 8.0) return;

    lastAnalysisTime = pluginRunTimeSec;
    analysisInProgress.store (true);

    // Take a snapshot of the buffer (audio thread → background thread handoff)
    // Do NOT reset analysisWritePos here — buffer stays "full" while BRAIN runs,
    // preventing spurious re-triggers. Reset only happens on failure via analysisResetPending.
    std::vector<float> snapshot = analysisBuffer;
    double sr = sampleRateCache;

    std::thread ([this, buf = std::move (snapshot), sr]() mutable
    {
        runBrainAnalysis (std::move (buf), sr);
    }).detach();
}

void MorphAudioProcessor::runBrainAnalysis (std::vector<float> monoBuffer, double sr)
{
    // Step 1 — C++ detection for BPM + KEY (sandbox-safe, no subprocess)
    {
        auto local = localDetectBpmAndKey (monoBuffer.data(), (int) monoBuffer.size(), sr);
        {
            const juce::ScopedLock sl (analysisLock);
            if (local.bpm >= 50.0 && local.bpm <= 220.0)
                detectedTrackBPM = local.bpm;
            if (local.key.isNotEmpty())
                detectedTrackKey = local.key;
        }
        // Commit key immediately — doesn't require Python, works inside Logic sandbox.
        // Only auto-sets if user hasn't manually chosen a key yet.
        if (local.key.isNotEmpty() && getProjectKey().isEmpty())
        {
            setProjectKey (local.key);
            brainKeyConfirmed.store (true);
        }
    }

    // Step 2 — Write buffer to JUCE temp dir (Logic sandbox allows this path)
    juce::File tmpWav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("morph_brain_analysis.wav");

    {
        juce::WavAudioFormat wavFmt;
        std::unique_ptr<juce::FileOutputStream> fos (tmpWav.createOutputStream());
        if (!fos || fos->failedToOpen())
        {
            brainAttempts.fetch_add (1, std::memory_order_relaxed);
            analysisInProgress.store (false);
            return;
        }
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wavFmt.createWriterFor (fos.get(), sr, 1, 16, {}, 0));
        if (!writer)
        {
            brainAttempts.fetch_add (1, std::memory_order_relaxed);
            analysisInProgress.store (false);
            return;
        }
        fos.release();

        constexpr int kChunk = 4096;
        int remaining = static_cast<int> (monoBuffer.size());
        int offset    = 0;
        while (remaining > 0)
        {
            int n = std::min (remaining, kChunk);
            const float* ptr = monoBuffer.data() + offset;
            writer->writeFromFloatArrays (&ptr, 1, n);
            offset    += n;
            remaining -= n;
        }
    }

    // Step 3 — Script path
    juce::String scriptPath =
        "/Users/Sebastien Graux/GRAUX Dropbox/Sébastien Graux"
        "/GRAUX_SYSTEM/02_Tech/library/scripts/detect-bpm-key.py";

    if (!juce::File (scriptPath).existsAsFile())
    {
        brainAttempts.fetch_add (1, std::memory_order_relaxed);
        analysisInProgress.store (false);
        return;
    }

    // Step 4 — Call Python directly (bash -l blocked by Logic sandbox)
    juce::String wavPath = tmpWav.getFullPathName();
    juce::StringArray args { "/opt/homebrew/bin/python3.11", scriptPath, wavPath };

    juce::ChildProcess proc;
    if (!proc.start (args))
    {
        brainAttempts.fetch_add (1, std::memory_order_relaxed);
        analysisInProgress.store (false);
        return;
    }

    proc.waitForProcessToFinish (120000);
    juce::String output = proc.readAllProcessOutput().trim();

    // Step 5 — Parse JSON: {"bpm": 93, "key": "Dmin", "confidence": 0.85, "method": "..."}
    auto json = juce::JSON::parse (output);
    if (json.isObject())
    {
        double bpm = json.getProperty ("bpm", 0.0);
        juce::String key = json.getProperty ("key", "?").toString();

        if (bpm >= 50.0 && bpm <= 220.0)
        {
            const juce::ScopedLock sl (analysisLock);
            detectedTrackBPM = bpm;
            detectedTrackKey = key;
        }

        if (key.isNotEmpty() && key != "?")
        {
            setProjectKey (key);
            brainKeyConfirmed.store (true);
            brainAttempts.store (0);
        }
        else
        {
            brainAttempts.fetch_add (1, std::memory_order_relaxed);
        }
    }
    else
    {
        brainAttempts.fetch_add (1, std::memory_order_relaxed);
    }

    tmpWav.deleteFile();
    analysisInProgress.store (false);
}

//==============================================================================
void MorphAudioProcessor::analyzeFileDirectly (const juce::File& audioFile)
{
    if (!audioFile.existsAsFile()) return;
    if (analysisInProgress.exchange (true)) return;   // already running

    // Indirection: pass original file directly to the Python script — no WAV
    // conversion needed, no playback required. Analysis only, never uploaded.
    std::thread ([this, audioFile]()
    {
        juce::String pythonPath;
        for (auto& c : { "/opt/homebrew/bin/python3.11", "/opt/homebrew/bin/python3",
                          "/usr/local/bin/python3",       "/usr/bin/python3" })
        {
            if (juce::File (c).existsAsFile()) { pythonPath = c; break; }
        }
        if (pythonPath.isEmpty()) { analysisInProgress.store (false); return; }

        const juce::String scriptPath =
            "/Users/Sebastien Graux/GRAUX Dropbox/Sébastien Graux"
            "/GRAUX_SYSTEM/02_Tech/library/scripts/detect-bpm-key.py";
        if (!juce::File (scriptPath).existsAsFile())
            { analysisInProgress.store (false); return; }

        juce::ChildProcess cp;
        juce::StringArray args { pythonPath, scriptPath,
                                  audioFile.getFullPathName(),
                                  "--filename", audioFile.getFileName() };
        if (!cp.start (args)) { analysisInProgress.store (false); return; }

        cp.waitForProcessToFinish (60000);
        auto json = juce::JSON::parse (cp.readAllProcessOutput().trim());
        if (json.isObject())
        {
            double       bpm = (double) json.getProperty ("bpm", 0.0);
            juce::String key = json.getProperty ("key", "?").toString();
            if (bpm >= 50.0 && bpm <= 220.0)
            {
                const juce::ScopedLock sl (analysisLock);
                detectedTrackBPM = bpm;
                detectedTrackKey = key;
            }
            if (key.isNotEmpty() && key != "?" && getProjectKey().isEmpty())
                setProjectKey (key);
        }
        analysisInProgress.store (false);
    }).detach();
}

//==============================================================================
// Auth
//==============================================================================
void MorphAudioProcessor::login (const juce::String& email,
                                  const juce::String& password,
                                  std::function<void(bool, juce::String)> callback)
{
    WaterAPI::login (email, password,
        [this, cb = std::move (callback)] (bool ok, juce::String token, juce::String error)
        {
            if (ok)
            {
                authToken    = token;
                refreshToken = {};   // email/password login doesn't return refresh token via our endpoint yet
                loggedIn.store (true);
                persistTokens (token, {});
            }
            cb (ok, error);
        });
}

void MorphAudioProcessor::logout()
{
    cancelOAuthFlow();
    authToken    = {};
    refreshToken = {};
    loggedIn.store (false);
    previewActive.store (false);
    morphEngine.reset();
    persistTokens ({}, {});
}

//==============================================================================
void MorphAudioProcessor::startOAuthFlow (std::function<void(bool, juce::String)> callback)
{
    cancelOAuthFlow();
    oauthPollActive.store (true);

    WaterAPI::authStart ([this, cb = std::move (callback)] (bool ok, juce::String sessionId)
    {
        if (!ok || sessionId.isEmpty())
        {
            oauthPollActive.store (false);
            cb (false, "Could not start auth session. Check your internet connection.");
            return;
        }

        // Open the authorization page in the user's default browser
        juce::URL (juce::String (WaterAPI::kBaseURL) + "/plugin-auth?session_id=" + sessionId)
            .launchInDefaultBrowser();

        // Poll with 5-minute deadline
        auto deadline = juce::Time::currentTimeMillis() + (5LL * 60 * 1000);
        scheduleOAuthPoll (sessionId, deadline, cb);
    });
}

void MorphAudioProcessor::cancelOAuthFlow()
{
    oauthPollActive.store (false);
}

void MorphAudioProcessor::scheduleOAuthPoll (const juce::String& sessionId,
                                              juce::int64 deadline,
                                              std::function<void(bool, juce::String)> cb)
{
    juce::Timer::callAfterDelay (2000, [this, sessionId, deadline, cb]()
    {
        if (!oauthPollActive.load()) return;

        if (juce::Time::currentTimeMillis() > deadline)
        {
            oauthPollActive.store (false);
            cb (false, "Connection timed out. Please try again.");
            return;
        }

        WaterAPI::authPoll (sessionId,
            [this, sessionId, deadline, cb] (bool ok, juce::String access, juce::String refresh)
            {
                if (!oauthPollActive.load()) return;

                if (ok && access.isNotEmpty())
                {
                    oauthPollActive.store (false);
                    authToken    = access;
                    refreshToken = refresh;
                    loggedIn.store (true);
                    persistTokens (access, refresh);
                    cb (true, {});
                }
                else
                {
                    // Still pending — schedule next poll
                    scheduleOAuthPoll (sessionId, deadline, cb);
                }
            });
    });
}

//==============================================================================
void MorphAudioProcessor::refreshTokenIfNeeded (std::function<void(bool)> callback)
{
    if (refreshToken.isEmpty())
    {
        callback (false);
        return;
    }

    WaterAPI::authRefresh (refreshToken,
        [this, cb = std::move (callback)] (bool ok, juce::String access, juce::String refresh)
        {
            if (ok && access.isNotEmpty())
            {
                authToken    = access;
                refreshToken = refresh;
                persistTokens (access, refresh);
            }
            cb (ok);
        });
}

//==============================================================================
// Library
//==============================================================================
void MorphAudioProcessor::loadTracksFromAPI (
    std::function<void(bool, juce::Array<TrackInfo>)> callback)
{
    WaterAPI::fetchAllTracks (authToken, {},
        [this, cb = std::move (callback)] (bool ok, juce::Array<TrackInfo> tracks)
        {
            if (ok) setCachedTracks (tracks);
            cb (ok, tracks);
        });
}

juce::Array<TrackInfo> MorphAudioProcessor::getCachedTracks() const
{
    const juce::ScopedLock sl (tracksLock);
    return cachedTracks;
}

void MorphAudioProcessor::setCachedTracks (const juce::Array<TrackInfo>& tracks)
{
    const juce::ScopedLock sl (tracksLock);
    cachedTracks = tracks;
}

//==============================================================================
// Upload
//==============================================================================
void MorphAudioProcessor::uploadFile (const juce::File& file,
                                       std::function<void(bool, juce::String)> callback)
{
    WaterAPI::uploadFile (file, authToken, std::move (callback));
}

//==============================================================================
// Preview
//==============================================================================
void MorphAudioProcessor::startPreview (const juce::String& trackId,
                                         const juce::String& demoHash,
                                         double              sourceBPM,
                                         int                 semitones)
{
    pendingPreviewStart.store (false);
    previewActive.store (false);
    previewSourceBPM.store (sourceBPM);
    previewSemitones.store (semitones);

    // Same track already in memory — reset to start, queue activation
    if (morphEngine.isTrackLoaded() && morphEngine.loadedTrackId() == trackId)
    {
        morphEngine.reset();  // readHead = 0, always restart from beginning
        pendingPreviewStart.store (true);
        startStandalonePreview();
        previewStatus.store (PreviewStatus::Ready);
        return;
    }

    previewStatus.store (PreviewStatus::Loading);
    juce::String token = authToken;

    // Load + decode on background thread, then queue quantized activation
    std::thread ([this, trackId, demoHash, token, sourceBPM, semitones]()
    {
        bool ok = morphEngine.loadTrack (trackId, demoHash, token);

        juce::MessageManager::callAsync ([this, ok, sourceBPM, semitones]()
        {
            if (!ok)
            {
                previewStatus.store (PreviewStatus::Failed);
                return;
            }

            previewSourceBPM.store (sourceBPM);
            previewSemitones.store (semitones);
            pendingPreviewStart.store (true);
            startStandalonePreview();   // plays immediately when DAW is stopped
            previewStatus.store (PreviewStatus::Ready);
        });
    }).detach();
}

void MorphAudioProcessor::stopPreview()
{
    // Kill both paths immediately — standaloneActive=false makes MorphStandaloneSource
    // return silence on the very next callback (no drain, no 3-second fade).
    // The standalone device stays open and silenced; it's only closed in the destructor
    // or when a new device open is needed. This avoids close/drain latency entirely.
    standaloneActive.store (false);
    pendingPreviewStart.store (false);
    previewActive.store (false);
    previewStatus.store (PreviewStatus::Idle);
    morphEngine.reset();  // readHead=0, restart from beginning next play
}

//==============================================================================
// Export
//==============================================================================
juce::File MorphAudioProcessor::exportMorphedWAV (const juce::String& trackId,
                                                    const juce::String& demoHash,
                                                    double              sourceBPM,
                                                    double              targetBPM,
                                                    int                 targetKey)
{
    if (morphEngine.loadedTrackId() != trackId)
    {
        if (!morphEngine.loadTrack (trackId, demoHash, authToken))
            return {};
    }

    // Apply morph params with correct source BPM before offline render
    morphEngine.setMorphParams (sourceBPM, targetBPM, targetKey);
    return morphEngine.exportWAV (trackId, targetBPM, targetKey);
}

//==============================================================================
// Waveform
//==============================================================================
juce::Array<float> MorphAudioProcessor::getWaveformForLoadedTrack (int numPoints) const
{
    return morphEngine.getWaveformData (numPoints);
}

//==============================================================================
// Project key
//==============================================================================

// Decode a key-signature value encoded as (sharpsOrFlats * 2 + (minor ? 1 : 0))
// into a key string like "Bmin", "Cmaj", etc.
// sharpsOrFlats ranges from -7 (7 flats) to +7 (7 sharps).
// Returns empty string for out-of-range values.
juce::String MorphAudioProcessor::decodeHostKey (int encoded) noexcept
{
    // encoded = sharps*2 + (minor ? 1 : 0)
    // recover sharps and mode
    bool isMinor = (encoded % 2 != 0);
    if (encoded < 0 && isMinor) encoded -= 1;  // round toward zero for negatives
    int sharps = encoded / 2;

    if (sharps < -7 || sharps > 7) return {};

    // Cycle of fifths, index = sharps + 7  (0 = Cb/Ab, 7 = C/A, 14 = C#/A#)
    static const char* kMajor[] = {
        "Cbmaj","Gbmaj","Dbmaj","Abmaj","Ebmaj","Bbmaj","Fmaj",
        "Cmaj","Gmaj","Dmaj","Amaj","Emaj","Bmaj","F#maj","C#maj"
    };
    static const char* kMinor[] = {
        "Abmin","Ebmin","Bbmin","Fmin","Cmin","Gmin","Dmin",
        "Amin","Emin","Bmin","F#min","C#min","G#min","D#min","A#min"
    };

    int idx = sharps + 7;
    return isMinor ? kMinor[idx] : kMajor[idx];
}

void MorphAudioProcessor::setProjectKey (const juce::String& k)
{
    const juce::ScopedLock sl (projKeyLock);
    projectKey = k;
    // If user clears key manually, allow BRAIN to re-detect on next audio chunk
    if (k.isEmpty())
    {
        brainKeyConfirmed.store (false);
        brainAttempts.store (0);
        analysisWritePos = 0;
    }
}

juce::String MorphAudioProcessor::getProjectKey() const
{
    const juce::ScopedLock sl (projKeyLock);
    return projectKey;
}

void MorphAudioProcessor::resetAnalysis()
{
    // Clear project key → resets brainKeyConfirmed + brainAttempts + analysisWritePos
    setProjectKey ({});
    // Also clear the last detected result so UI shows scanning state immediately
    {
        const juce::ScopedLock sl (analysisLock);
        detectedTrackKey = "?";
        detectedTrackBPM = 120.0;
    }
}

//==============================================================================
// State serialisation
//==============================================================================
void MorphAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("MorphState");
    state.setProperty ("authToken",    authToken,    nullptr);
    state.setProperty ("refreshToken", refreshToken, nullptr);
    state.setProperty ("loggedIn",     loggedIn.load(), nullptr);

    juce::MemoryOutputStream out (destData, true);
    state.writeToStream (out);
}

void MorphAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, static_cast<size_t> (sizeInBytes), false);
    juce::ValueTree state = juce::ValueTree::readFromStream (in);

    if (state.isValid())
    {
        authToken    = state.getProperty ("authToken",    "").toString();
        refreshToken = state.getProperty ("refreshToken", "").toString();
        bool wasLoggedIn = state.getProperty ("loggedIn", false);
        loggedIn.store (wasLoggedIn && authToken.isNotEmpty());

        if (authToken.isNotEmpty())
            persistTokens (authToken, refreshToken);
    }
}

//==============================================================================
// Standalone preview device
//==============================================================================
void MorphAudioProcessor::startStandalonePreview()
{
    // Must be called on the message thread
    if (!standaloneDeviceOpen)
    {
        standaloneSource = std::make_unique<MorphStandaloneSource> (*this);
        standalonePlayer.setSource (standaloneSource.get());

        juce::String err = standaloneDevice.initialise (0, 2, nullptr, true);
        if (err.isEmpty())
        {
            standaloneDevice.addAudioCallback (&standalonePlayer);
            standaloneDeviceOpen = true;
        }
    }
    standaloneActive.store (true);
}

void MorphAudioProcessor::stopStandalonePreview()
{
    standaloneActive.store (false);
    if (standaloneDeviceOpen)
    {
        standalonePlayer.setSource (nullptr);
        standaloneDevice.removeAudioCallback (&standalonePlayer);
        standaloneDevice.closeAudioDevice();
        standaloneSource.reset();
        standaloneDeviceOpen = false;
    }
}

void MorphAudioProcessor::setStandaloneActive (bool b)
{
    if (b && !standaloneDeviceOpen)
        startStandalonePreview();
    else
        standaloneActive.store (b);
}

//==============================================================================
// Plugin entry points
//==============================================================================
juce::AudioProcessorEditor* MorphAudioProcessor::createEditor()
{
    return new MorphAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphAudioProcessor();
}

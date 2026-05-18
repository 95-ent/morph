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
                                         juce::MidiBuffer& /*midiMessages*/)
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

    // Always accumulate mono audio for BRAIN analysis.
    // Track mode uses detected BPM+key directly; Project mode uses detected key to
    // auto-populate projectKey (BPM still comes from the DAW playhead).
    if (buffer.getNumChannels() > 0)
    {
        const int numSamples = buffer.getNumSamples();
        const int numCh      = buffer.getNumChannels();
        const int bufSize    = static_cast<int> (analysisBuffer.size());

        for (int i = 0; i < numSamples && analysisWritePos < bufSize; ++i)
        {
            float mono = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                mono += buffer.getReadPointer (ch)[i];
            analysisBuffer[analysisWritePos++] = mono / static_cast<float> (numCh);
        }

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
    if (referenceMode.load() == ReferenceMode::kTrack)
    {
        const juce::ScopedLock sl (analysisLock);
        return detectedTrackBPM;
    }
    // readCurrentBPM() hits the live playhead — Logic provides project tempo
    // even when transport is stopped, so this works without processBlock running.
    return readCurrentBPM();
}

juce::String MorphAudioProcessor::getDetectedKey() const
{
    const juce::ScopedLock sl (analysisLock);
    return detectedTrackKey;
}

//==============================================================================
// BRAIN Analysis — same pipeline as detect-bpm-key.py
//==============================================================================
void MorphAudioProcessor::maybeScheduleAnalysis() noexcept
{
    // 30-second cooldown between analyses; don't run if one is already in flight
    if (analysisInProgress.load()) return;
    if ((pluginRunTimeSec - lastAnalysisTime) < 30.0) return;

    lastAnalysisTime = pluginRunTimeSec;
    analysisInProgress.store (true);

    // Take a snapshot of the buffer (audio thread → background thread handoff)
    std::vector<float> snapshot = analysisBuffer;
    double sr = sampleRateCache;

    // Reset write position so audio continues accumulating
    analysisWritePos = 0;

    std::thread ([this, buf = std::move (snapshot), sr]() mutable
    {
        runBrainAnalysis (std::move (buf), sr);
    }).detach();
}

void MorphAudioProcessor::runBrainAnalysis (std::vector<float> monoBuffer, double sr)
{
    // 1 — C++ offline detection (no Python required, works for every user)
    {
        auto local = localDetectBpmAndKey (monoBuffer.data(), (int) monoBuffer.size(), sr);
        if (local.bpm >= 50.0 && local.bpm <= 220.0)
        {
            const juce::ScopedLock sl (analysisLock);
            detectedTrackBPM = local.bpm;
            detectedTrackKey = local.key;
        }
        if (local.key.isNotEmpty() && getProjectKey().isEmpty())
            setProjectKey (local.key);
    }

    // 2 — Write buffer to a temp WAV file (for Python refinement if available)
    juce::File tmpWav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("morph_brain_analysis.wav");

    {
        juce::WavAudioFormat wavFmt;
        std::unique_ptr<juce::FileOutputStream> fos (tmpWav.createOutputStream());
        if (!fos || fos->failedToOpen())
        {
            analysisInProgress.store (false);
            return;
        }
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wavFmt.createWriterFor (fos.get(), sr, 1, 16, {}, 0));
        if (!writer)
        {
            analysisInProgress.store (false);
            return;
        }
        fos.release(); // writer owns the stream

        // Write in chunks
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
    } // writer destructor flushes + closes

    // 2 — Find Python executable
    juce::String pythonPath;
    for (auto& candidate : { "/opt/homebrew/bin/python3.11",
                              "/opt/homebrew/bin/python3",
                              "/usr/local/bin/python3",
                              "/usr/bin/python3" })
    {
        if (juce::File (candidate).existsAsFile())
        {
            pythonPath = candidate;
            break;
        }
    }
    if (pythonPath.isEmpty())
    {
        analysisInProgress.store (false);
        return;
    }

    // 3 — Script path (same directory as detect-bpm-key.py in Water repo)
    juce::String scriptPath =
        "/Users/Sebastien Graux/GRAUX Dropbox/Sébastien Graux"
        "/GRAUX_SYSTEM/02_Tech/library/scripts/detect-bpm-key.py";

    if (!juce::File (scriptPath).existsAsFile())
    {
        analysisInProgress.store (false);
        return;
    }

    // 4 — Call subprocess: python3 detect-bpm-key.py /tmp/morph_brain_analysis.wav
    juce::ChildProcess proc;
    juce::StringArray args { pythonPath, scriptPath, tmpWav.getFullPathName() };
    if (!proc.start (args))
    {
        analysisInProgress.store (false);
        return;
    }

    proc.waitForProcessToFinish (30000); // 30s timeout
    juce::String output = proc.readAllProcessOutput().trim();

    // 5 — Parse JSON: {"bpm": 93, "key": "Dmin", "confidence": 0.85, "method": "..."}
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

        // Auto-populate projectKey only if not yet set — prevents key drift
        // when BRAIN re-analyses subsequent audio chunks during continuous playback.
        if (key.isNotEmpty() && key != "?" && getProjectKey().isEmpty())
            setProjectKey (key);
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
void MorphAudioProcessor::setProjectKey (const juce::String& k)
{
    const juce::ScopedLock sl (projKeyLock);
    projectKey = k;
}

juce::String MorphAudioProcessor::getProjectKey() const
{
    const juce::ScopedLock sl (projKeyLock);
    return projectKey;
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

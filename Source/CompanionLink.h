#pragma once
#include <JuceHeader.h>
#include <memory>

//==============================================================================
// CompanionLink — plugin-side TCP client for WaterMorphHelper.
//
// On macOS the AU runs inside Logic Pro's sandboxed XPC process and cannot
// initiate a native NSDraggingSession directly.  CompanionLink connects to
// WaterMorphHelper (running outside the sandbox) via TCP localhost:59812 and
// asks it to perform the drag on our behalf.
//
// On Windows VST3 hosts (FL, Ableton, Cubase) the plugin is not sandboxed so
// drag is handled natively; the companion is still used for the library web UI
// and transport sync.
//
// Thread safety: all public methods are safe to call from the message thread.
//==============================================================================
class CompanionLink
{
public:
    static CompanionLink& get();

    //==========================================================================
    // Try to connect to the companion (fire-and-forget at plugin startup).
    // The companion is registered as a LaunchAgent / autostart entry and starts
    // automatically at login — the plugin never needs to launch it.
    //==========================================================================
    void ensureRunning();

    //==========================================================================
    // Broadcast DAW sync state (BPM, key, mode, timeSecs) to the companion.
    // timeSecs = ppqPosition / bpm * 60 — current playhead position in seconds.
    // Fire-and-forget — no ack expected. Safe to call from the message thread.
    //==========================================================================
    // tsNum / tsDen = project time signature (e.g. 4/4) — lets the companion
    // bar-align morph exports to the DAW grid. Appended after the existing
    // positional fields so older companions ignore them harmlessly.
    void sendSync (double bpm, const juce::String& key, const juce::String& mode,
                   double timeSecs = 0.0, int tsNum = 4, int tsDen = 4);

    //==========================================================================
    // Notify the companion of a transport play/stop event with the current
    // playhead position. Companion injects transport() into the web player.
    //==========================================================================
    void sendTransport (bool playing, double timeSecs, double ppq = 0.0);

    //==========================================================================
    // Ask the companion to bring its window to front.
    // Falls back gracefully if companion is not running.
    //==========================================================================
    void requestShowWindow();

    //==========================================================================
    bool isConnected() const noexcept;

private:
    CompanionLink()  = default;
    ~CompanionLink();

    std::unique_ptr<juce::StreamingSocket> socket;

    // Throttle reconnect attempts — tryConnect() blocks for kConnectTimeoutMs,
    // so we cap retries to once every kReconnectThrottleMs to avoid UI freeze.
    int64_t lastConnectAttemptMs_ { 0 };
    static constexpr int kConnectTimeoutMs    =  200;   // was 3000 — prevents UI freeze
    static constexpr int kReconnectThrottleMs = 1000;   // retry at most every 1 s

    bool tryConnect();
    void disconnect();

    JUCE_DECLARE_NON_COPYABLE (CompanionLink)
};

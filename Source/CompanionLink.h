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
    // Ask the companion to drag filePath to wherever the user drops it.
    // Returns true if the command was sent successfully.
    //==========================================================================
    bool requestDrag (const juce::String& filePath);

    //==========================================================================
    // Broadcast DAW sync state (BPM, key, mode, timeSecs) to the companion.
    // timeSecs = ppqPosition / bpm * 60 — current playhead position in seconds.
    // Fire-and-forget — no ack expected. Safe to call from the message thread.
    //==========================================================================
    void sendSync (double bpm, const juce::String& key, const juce::String& mode,
                   double timeSecs = 0.0);

    //==========================================================================
    // Notify the companion of a transport play/stop event with the current
    // playhead position. Companion injects transport() into the web player.
    //==========================================================================
    void sendTransport (bool playing, double timeSecs, double ppq = 0.0);

    //==========================================================================
    bool isConnected() const noexcept;

private:
    CompanionLink()  = default;
    ~CompanionLink();

    std::unique_ptr<juce::StreamingSocket> socket;

    bool tryConnect();
    void disconnect();

    JUCE_DECLARE_NON_COPYABLE (CompanionLink)
};

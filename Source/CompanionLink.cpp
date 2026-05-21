#include "CompanionLink.h"

static constexpr int kPort = 59812;

//==============================================================================
CompanionLink& CompanionLink::get()
{
    static CompanionLink instance;
    return instance;
}

CompanionLink::~CompanionLink()
{
    disconnect();
}

//==============================================================================
bool CompanionLink::tryConnect()
{
    // Throttle reconnect attempts to prevent blocking the message thread.
    // kConnectTimeoutMs is only 200ms but we still cap to once every 5s.
    const int64_t now = juce::Time::currentTimeMillis();
    if (now - lastConnectAttemptMs_ < kReconnectThrottleMs)
        return false;
    lastConnectAttemptMs_ = now;

    disconnect();
    socket = std::make_unique<juce::StreamingSocket>();

    if (! socket->connect ("127.0.0.1", kPort, kConnectTimeoutMs))
    {
        socket.reset();
        return false;
    }

    // Wait for READY\n handshake
    if (socket->waitUntilReady (true, kConnectTimeoutMs) != 1)
    {
        socket.reset();
        return false;
    }

    char buf[32] = {};
    if (socket->read (buf, sizeof (buf) - 1, false) <= 0
        || juce::String (buf).trim() != "READY")
    {
        socket.reset();
        return false;
    }

    // Reset throttle on success so a reconnect after disconnect is instant
    lastConnectAttemptMs_ = 0;
    return true;
}

void CompanionLink::disconnect()
{
    if (socket)
    {
        socket->close();
        socket.reset();
    }
}

//==============================================================================
void CompanionLink::ensureRunning()
{
    // The companion is managed by a LaunchAgent / autostart entry and starts
    // automatically at login — the plugin never needs to launch it.
    // We just try to connect; if the socket isn't ready we'll retry on
    // the next requestDrag() or sendTransport() call.
    if (isConnected()) return;
    tryConnect();
}

//==============================================================================
bool CompanionLink::requestDrag (const juce::String& filePath)
{
    if (! isConnected()) tryConnect();
    if (! isConnected()) return false;

    const juce::String cmd = "DRAG " + filePath + "\n";
    if (socket->write (cmd.toRawUTF8(), (int) cmd.getNumBytesAsUTF8()) <= 0)
    {
        disconnect();
        return false;
    }

    if (socket->waitUntilReady (true, kConnectTimeoutMs) != 1)
        return false;

    char ack[16] = {};
    socket->read (ack, sizeof (ack) - 1, false);
    return juce::String (ack).trim() == "OK";
}

//==============================================================================
void CompanionLink::sendSync (double bpm, const juce::String& key,
                               const juce::String& mode, double timeSecs)
{
    if (! isConnected()) tryConnect();
    if (! isConnected()) return;

    const juce::String msg = "SYNC " + juce::String (bpm, 2)
                           + " " + (key.isEmpty() ? "?" : key)
                           + " " + mode
                           + " " + juce::String (timeSecs, 3) + "\n";
    if (socket->write (msg.toRawUTF8(), (int) msg.getNumBytesAsUTF8()) <= 0)
        disconnect();
}

void CompanionLink::sendTransport (bool playing, double timeSecs, double ppq)
{
    if (! isConnected()) tryConnect();
    if (! isConnected()) return;

    const juce::String msg = "TRANSPORT "
                           + juce::String (playing ? "playing" : "stopped")
                           + " " + juce::String (timeSecs, 3)
                           + " " + juce::String (ppq, 4) + "\n";
    if (socket->write (msg.toRawUTF8(), (int) msg.getNumBytesAsUTF8()) <= 0)
        disconnect();
}

//==============================================================================
void CompanionLink::requestShowWindow()
{
    if (! isConnected()) tryConnect();
    if (! isConnected())
    {
        // Companion not running — launch it via registered watermorph:// protocol.
        // The companion registered itself as the handler at install time.
        juce::URL ("watermorph://show").launchInDefaultBrowser();
        return;
    }

    const juce::String cmd = "SHOW_WINDOW\n";
    if (socket->write (cmd.toRawUTF8(), (int) cmd.getNumBytesAsUTF8()) <= 0)
        disconnect();
}

//==============================================================================
bool CompanionLink::isConnected() const noexcept
{
    return socket != nullptr && socket->isConnected();
}

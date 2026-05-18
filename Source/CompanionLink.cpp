#include "CompanionLink.h"

static constexpr int kPort      = 59812;
static constexpr int kTimeoutMs = 3000;

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
    disconnect();
    socket = std::make_unique<juce::StreamingSocket>();

    if (! socket->connect ("127.0.0.1", kPort, kTimeoutMs))
    {
        socket.reset();
        return false;
    }

    // Wait for READY\n handshake
    if (socket->waitUntilReady (true, kTimeoutMs) != 1)
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

    if (socket->waitUntilReady (true, kTimeoutMs) != 1)
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
bool CompanionLink::isConnected() const noexcept
{
    return socket != nullptr && socket->isConnected();
}

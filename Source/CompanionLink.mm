#import <Cocoa/Cocoa.h>
#include "CompanionLink.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static constexpr uint16_t kTCPPort  = 59812;
static constexpr int      kTimeoutMs = 3000;

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
juce::File CompanionLink::helperExecutable()
{
    // Primary location: bundled inside the installed AU component.
    juce::File au ("~/Library/Audio/Plug-Ins/Components/Morph.component");
    juce::File bundled = au.getChildFile ("Contents/Helpers/WaterMorphHelper.app"
                                          "/Contents/MacOS/WaterMorphHelper");
    if (bundled.existsAsFile())
        return bundled;

    // Dev fallback: next to the Companion source folder.
    juce::File dev = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                         .getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory()
                         .getChildFile ("Companion/WaterMorphHelper.app"
                                        "/Contents/MacOS/WaterMorphHelper");
    return dev;
}

//==============================================================================
bool CompanionLink::tryConnect()
{
    disconnect();

    int fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons (kTCPPort);
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

    if (::connect (fd, (struct sockaddr*) &addr, sizeof (addr)) < 0)
    {
        close (fd);
        return false;
    }

    // Wait for READY\n
    char buf[32] = {};
    ssize_t n = read (fd, buf, sizeof (buf) - 1);
    if (n <= 0 || juce::String (buf).trim() != "READY")
    {
        close (fd);
        return false;
    }

    sockFd = fd;
    return true;
}

void CompanionLink::disconnect()
{
    if (sockFd >= 0)
    {
        close (sockFd);
        sockFd = -1;
    }
}

//==============================================================================
void CompanionLink::ensureRunning()
{
    // The companion is managed by a LaunchAgent (installed by build.sh) and
    // starts automatically at login — the plugin never needs to launch it.
    // We just try to connect; if the socket isn't there yet we'll retry on
    // the next requestDrag() call.
    if (isConnected()) return;
    tryConnect();
}

//==============================================================================
bool CompanionLink::requestDrag (const juce::String& filePath)
{
    // Re-connect if the connection was lost.
    if (!isConnected())
        tryConnect();

    if (!isConnected()) return false;

    juce::String cmd = "DRAG " + filePath + "\n";
    const char* raw  = cmd.toRawUTF8();
    ssize_t     sent = write (sockFd, raw, strlen (raw));

    if (sent <= 0)
    {
        disconnect();
        return false;
    }

    // Read acknowledgement
    char ack[16] = {};
    read (sockFd, ack, sizeof (ack) - 1);
    return juce::String (ack).trim() == "OK";
}

//==============================================================================
void CompanionLink::sendSync (double bpm, const juce::String& key, const juce::String& mode, double timeSecs)
{
    if (!isConnected()) tryConnect();
    if (!isConnected()) return;

    juce::String msg = "SYNC " + juce::String (bpm, 2) + " " + (key.isEmpty() ? "?" : key) + " " + mode
                     + " " + juce::String (timeSecs, 3) + "\n";
    const char* raw  = msg.toRawUTF8();
    if (write (sockFd, raw, strlen (raw)) <= 0)
        disconnect();
}

void CompanionLink::sendTransport (bool playing, double timeSecs, double ppq)
{
    if (!isConnected()) tryConnect();
    if (!isConnected()) return;

    juce::String msg = "TRANSPORT " + juce::String (playing ? "playing" : "stopped")
                     + " " + juce::String (timeSecs, 3)
                     + " " + juce::String (ppq, 4) + "\n";
    const char* raw  = msg.toRawUTF8();
    if (write (sockFd, raw, strlen (raw)) <= 0)
        disconnect();
}


// WaterMorphHelper.m
// Water Morph companion process.
//
// Runs as a background-only macOS process (no dock icon, no menu bar).
// Listens on Unix socket /tmp/water-morph.sock.
// Receives DRAG commands from the sandboxed AU plugin and executes
// them via NSDraggingSession — which requires being outside Logic's sandbox.
//
// Build:  see build.sh
// Install: Morph.component/Contents/Helpers/WaterMorphHelper.app

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static NSString* const kSocketPath = @"/tmp/water-morph.sock";

// ---------------------------------------------------------------------------
// Drag-capable view — lives in a tiny transparent window.
// ---------------------------------------------------------------------------
@interface WMDragView : NSView <NSDraggingSource>
@end

@implementation WMDragView

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
        sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return NSDragOperationCopy | NSDragOperationGeneric | NSDragOperationMove;
}

- (void)draggingSession:(NSDraggingSession*)session
        endedAtPoint:(NSPoint)screenPoint
        operation:(NSDragOperation)operation {}

- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (void)startDragWithFile:(NSString*)filePath
{
    if (![[NSFileManager defaultManager] fileExistsAtPath:filePath]) return;

    NSURL* fileURL = [NSURL fileURLWithPath:filePath];
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:fileURL];

    // Use the WAV file icon as drag image
    NSImage* icon = [[NSWorkspace sharedWorkspace]
        iconForContentType:[UTType typeWithFilenameExtension:@"wav"]];
    [icon setSize:NSMakeSize(32, 32)];
    NSRect dragFrame = NSMakeRect(16, 16, 32, 32);
    [item setDraggingFrame:dragFrame contents:icon];

    // Synthesise a mouse-down event at the view's centre so
    // beginDraggingSession: has a valid event to anchor the drag.
    NSPoint viewCentre = NSMakePoint(NSMidX(self.bounds), NSMidY(self.bounds));
    NSEvent* fakeDown = [NSEvent
        mouseEventWithType:NSEventTypeLeftMouseDown
                  location:viewCentre
             modifierFlags:0
                 timestamp:[[NSProcessInfo processInfo] systemUptime]
              windowNumber:self.window.windowNumber
                   context:nil
               eventNumber:0
                clickCount:1
                  pressure:1.0];

    NSDraggingSession* session = [self beginDraggingSessionWithItems:@[item]
                                                               event:fakeDown
                                                              source:self];
    session.animatesToStartingPositionsOnCancelOrFail = NO;
}

@end

// ---------------------------------------------------------------------------
// App delegate — owns the drag window and the socket server.
// ---------------------------------------------------------------------------
@interface WMAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow*   dragWindow;
@property (nonatomic, strong) WMDragView* dragView;
@end

@implementation WMAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    // Tiny transparent floating window — always in front, never focused.
    self.dragWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 64, 64)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.dragWindow.backgroundColor       = [NSColor clearColor];
    self.dragWindow.opaque                = NO;
    self.dragWindow.level                 = NSFloatingWindowLevel;
    self.dragWindow.ignoresMouseEvents    = NO;
    self.dragWindow.hidesOnDeactivate     = NO;
    self.dragWindow.collectionBehavior    =
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary;

    self.dragView = [[WMDragView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    self.dragWindow.contentView = self.dragView;
    [self.dragWindow orderFront:nil];

    // Socket server runs on a dedicated background thread.
    [NSThread detachNewThreadSelector:@selector(runSocketServer)
                             toTarget:self
                           withObject:nil];
}

// Called from socket thread — always dispatches drag onto the main thread.
- (void)performDragWithFile:(NSString*)filePath
{
    dispatch_async(dispatch_get_main_queue(), ^{
        // Centre the drag window on the current cursor so the drag
        // appears to originate from the pointer position.
        NSPoint mouse = [NSEvent mouseLocation];
        NSRect  frame = NSMakeRect(mouse.x - 32, mouse.y - 32, 64, 64);
        [self.dragWindow setFrame:frame display:YES];
        [self.dragWindow makeKeyAndOrderFront:nil];
        [self.dragView startDragWithFile:filePath];
    });
}

// ---------------------------------------------------------------------------
// Unix socket server — accepts multiple clients, one thread per connection.
// ---------------------------------------------------------------------------
- (void)runSocketServer
{
    // Remove stale socket left by a previous run.
    unlink(kSocketPath.UTF8String);

    int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0) { NSLog(@"[WMHelper] socket() failed"); return; }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, kSocketPath.UTF8String, sizeof(addr.sun_path));

    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        NSLog(@"[WMHelper] bind() failed");
        close(serverFd);
        return;
    }

    chmod(kSocketPath.UTF8String, 0600);
    listen(serverFd, 8);
    NSLog(@"[WMHelper] listening on %@", kSocketPath);

    while (YES) {
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd < 0) continue;
        [NSThread detachNewThreadSelector:@selector(handleClient:)
                                 toTarget:self
                               withObject:@(clientFd)];
    }
    close(serverFd);
}

- (void)handleClient:(NSNumber*)fdNum
{
    int fd = fdNum.intValue;

    // Announce readiness.
    write(fd, "READY\n", 6);

    char         buf[2048] = {};
    ssize_t      n;
    NSMutableString* pending = [NSMutableString string];

    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        [pending appendString:[NSString stringWithUTF8String:buf]];

        NSRange nl;
        while ((nl = [pending rangeOfString:@"\n"]).location != NSNotFound) {
            NSString* line = [[pending substringToIndex:nl.location]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            [pending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];

            if ([line isEqualToString:@"PING"]) {
                write(fd, "PONG\n", 5);

            } else if ([line hasPrefix:@"DRAG "]) {
                // "DRAG /absolute/path/to/file.wav"
                NSString* filePath = [[line substringFromIndex:5]
                    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                [self performDragWithFile:filePath];
                write(fd, "OK\n", 3);
            }
        }
    }

    close(fd);
}

@end

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, const char* argv[])
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        WMAppDelegate* delegate = [WMAppDelegate new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The host's windows for embedded plugin views on macOS, and the Cocoa event loop for processes
// that have none of their own (a plugin worker).
#include "Window.h"
#include <kick_clap/Host.h>
#import <AppKit/AppKit.h>
#include <algorithm>

@interface KickClapWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) kick::clap::detail::Window* owner;
@property(nonatomic, assign) bool resizing;
@end

@implementation KickClapWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    [sender orderOut:nil];
    if (_owner && _owner->closing) _owner->closing();
    return NO; // the host destroys the plugin's view, then the window
}
- (void)windowDidResize:(NSNotification*)notification
{
    if (!_owner || !_owner->resized || _resizing) return;
    NSWindow* window = notification.object;
    const NSSize size = [window contentRectForFrameRect:window.frame].size;
    uint32_t width = uint32_t(std::max(1.0, size.width)), height = uint32_t(std::max(1.0, size.height));
    const uint32_t asked = width, askedHeight = height;
    _owner->resized(width, height);
    if (width != asked || height != askedHeight) { _resizing = true; [window setContentSize:NSMakeSize(width, height)]; _resizing = false; }
}
@end

namespace kick::clap::detail {
namespace {
// A process without an application object (a plugin worker) gets one, without a Dock icon.
void startApplication()
{
    if (NSApp) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
}
}

class CocoaWindow final : public Window {
public:
    CocoaWindow(const std::string& title, uint32_t width, uint32_t height, bool resizable)
    {
        startApplication();
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (resizable) style |= NSWindowStyleMaskResizable;
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, std::max(1u, width), std::max(1u, height))
                                             styleMask:style backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        delegate = [KickClapWindowDelegate new];
        delegate.owner = this;
        window.delegate = delegate;
        setTitle(title);
        [window center];
    }
    ~CocoaWindow() override
    {
        window.delegate = nil;
        [window orderOut:nil];
        [window close];
    }
    void* view() noexcept override { return (__bridge void*)window.contentView; }
    void setTitle(const std::string& title) override { NSString* text = [NSString stringWithUTF8String:title.c_str()]; window.title = text ? text : @""; }
    void setSize(uint32_t width, uint32_t height) override
    {
        delegate.resizing = true;
        [window setContentSize:NSMakeSize(std::max(1u, width), std::max(1u, height))];
        delegate.resizing = false;
    }
    void setResizable(bool resizable) override
    {
        window.styleMask = resizable ? (window.styleMask | NSWindowStyleMaskResizable) : (window.styleMask & ~NSWindowStyleMaskResizable);
    }
    void show() override { [NSApp activateIgnoringOtherApps:YES]; [window makeKeyAndOrderFront:nil]; }
    void hide() override { [window orderOut:nil]; }

private:
    NSWindow* window = nil;
    KickClapWindowDelegate* delegate = nil;
};

std::unique_ptr<Window> Window::create(const std::string& title, uint32_t width, uint32_t height, bool resizable)
{
    @autoreleasepool { return std::make_unique<CocoaWindow>(title, width, height, resizable); }
}
const char* Window::api() noexcept { return "cocoa"; }
void startWindows() { startApplication(); }
}

namespace kick::clap {
namespace {
bool readable = false;
void wakeOnRead(CFFileDescriptorRef, CFOptionFlags, void*)
{
    readable = true;
    // Ends the wait for an event below.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
                                     windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
}
}

bool runWindowEvents(int descriptor, int milliseconds)
{
    if (!NSApp) return false; // no window was ever made here
    @autoreleasepool {
        static CFFileDescriptorRef watched = nullptr;
        static int watchedDescriptor = -1;
        if (descriptor >= 0 && descriptor != watchedDescriptor) {
            if (watched) { CFFileDescriptorInvalidate(watched); CFRelease(watched); }
            watched = CFFileDescriptorCreate(nullptr, descriptor, false, wakeOnRead, nullptr);
            CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(nullptr, watched, 0);
            CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
            watchedDescriptor = descriptor;
        }
        readable = false;
        if (watched) CFFileDescriptorEnableCallBacks(watched, kCFFileDescriptorReadCallBack);
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:milliseconds / 1000.0];
        while (!readable && until.timeIntervalSinceNow > 0) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!event) break;
            if (event.type != NSEventTypeApplicationDefined) [NSApp sendEvent:event];
        }
    }
    return true;
}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace kick::clap::detail {
// A titled window of the host's own, holding a plugin's embedded view (CLAP gui, not floating).
// Main thread. Made only where the host draws such windows: macOS (Cocoa) so far.
class Window {
public:
    // Null where this platform has none.
    static std::unique_ptr<Window> create(const std::string& title, uint32_t width, uint32_t height, bool resizable);
    // The CLAP window API whose views fit in it ("cocoa"), or null where there is no window.
    static const char* api() noexcept;
    virtual ~Window() = default;
    virtual void* view() noexcept = 0;  // the parent to give the plugin (an NSView on macOS)
    virtual void setTitle(const std::string&) = 0;
    virtual void setSize(uint32_t width, uint32_t height) = 0; // the content, in points
    virtual void setResizable(bool) = 0;
    virtual void show() = 0;
    virtual void hide() = 0;
    // The person closed it: the window has hidden itself; the host destroys the plugin's view later.
    std::function<void()> closing;
    // The person resized it to width × height: the host asks the plugin and may change them.
    std::function<void(uint32_t& width, uint32_t& height)> resized;
};
// Before a plugin makes a window of its own: the process's application object (macOS).
void startWindows();
}

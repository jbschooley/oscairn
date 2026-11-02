// SPDX-License-Identifier: AGPL-3.0-or-later
// Platforms where the host has no windows of its own yet: only plugins that float their own
// window show one (Linux's X11 embedding is not built).
#include "Window.h"
#include <kick_clap/Host.h>

namespace kick::clap::detail {
std::unique_ptr<Window> Window::create(const std::string&, uint32_t, uint32_t, bool) { return nullptr; }
const char* Window::api() noexcept { return nullptr; }
void startWindows() {}
}

namespace kick::clap {
bool runWindowEvents(int, int) { return false; }
}

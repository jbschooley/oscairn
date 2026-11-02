// SPDX-License-Identifier: AGPL-3.0-or-later
// oscairn-plugin-worker FD [MEMORY_LIMIT]: hosts one plugin for an engine connected on socket FD (RFC 0010).
#include "Worker.h"
#include "oscairn/plugin_host.h"
#include <algorithm>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OSCAIRN_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define OSCAIRN_SANITIZED 1
#endif
#endif

int main(int argc, char** argv)
{
    if(argc != 2 && argc != 3) return 2;
    std::signal(SIGPIPE, SIG_IGN);
#if defined(__linux__) && !defined(OSCAIRN_SANITIZED)
    // The engine's supervisor enforces the memory limit on resident memory; Linux also enforces
    // address space. That runs well above resident memory (a native engine's worker reserves a
    // gigabyte it never touches), so four times the limit: a backstop for allocations faster than
    // the supervisor.
    if(argc == 3) if(const uint64_t limit = std::strtoull(argv[2], nullptr, 10))
    {
        const rlim_t space = rlim_t(std::min<uint64_t>(limit, RLIM_INFINITY / 4) * 4);
        rlimit r{space, space}; setrlimit(RLIMIT_AS, &r);
    }
#endif
    // RFC 0025: locked at instrument and above; a committed plugin's worker after the apps in the
    // out-of-memory order, since RFC 0010 restarts it.
    if(const auto missed = oscairn::applyMemoryPolicy(300); !missed.empty() && std::getenv("OSCAIRN_RELIABILITY"))
        std::fprintf(stderr, "oscairn-plugin-worker: %s\n", missed.c_str());
    return oscairn::worker::run(std::atoi(argv[1]));
}

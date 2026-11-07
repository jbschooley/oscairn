// SPDX-License-Identifier: AGPL-3.0-or-later
// Package repositories on the web (RFC 0031) through JUCE's URL streams: on macOS, the system's
// NSURLSession; on Linux, libcurl (JUCE_USE_CURL), which verifies certificates against the
// system's CA bundle. Every byte fetched is checked by its hash and signature in the core.
#include "WebTransport.h"
#include "oscairn/repository.h"
#include <juce_core/juce_core.h>
#include <stdexcept>

namespace oscairn::host
{
    void useWebTransport()
    {
        repository::setTransport([](const std::string& url, uint64_t from, const std::function<bool(int, const char*, size_t)>& sink) {
            int status = 0;
            auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                               .withConnectionTimeoutMs(15000)
                               .withNumRedirectsToFollow(5) // release downloads redirect to another host
                               .withStatusCode(&status)
                               .withExtraHeaders(from ? "Range: bytes=" + juce::String(juce::int64(from)) + "-" : juce::String());
            const auto stream = juce::URL(juce::String::fromUTF8(url.c_str())).createInputStream(options);
            if(!stream) throw std::runtime_error("cannot reach " + url);
            char buffer[1 << 16];
            for(;;)
            {
                const auto n = stream->read(buffer, sizeof buffer);
                if(n <= 0 || !sink(status, buffer, size_t(n))) break;
            }
            return status;
        });
    }
}

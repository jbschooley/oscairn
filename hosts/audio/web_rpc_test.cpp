// SPDX-License-Identifier: AGPL-3.0-or-later
// A control session on stdin and stdout, a JSON-RPC request per line, that fetches package
// repositories from the web as the engine does (WebTransport); no audio device. For
// control-repository-web (core/control/tests/repository.py).
#include "WebTransport.h"
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include <juce_events/juce_events.h>
#include <iostream>

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    oscairn::host::useWebTransport();
    oscairn::Session session(48000, 64);
    oscairn::JsonRpc rpc(session);
    for(std::string line; std::getline(std::cin, line);) std::cout << rpc.dispatch(line).value_or("") << '\n' << std::flush;
}

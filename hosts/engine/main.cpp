// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../audio/AudioEngine.h"
#include "oscairn/local_socket.h"
#include "EngineProcess.h"
#include <juce_events/juce_events.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace
{
    volatile std::sig_atomic_t stopping=0;
    struct Pump : juce::Timer
    {
        oscairn::LocalServer& server;
        int result=0;
        explicit Pump(oscairn::LocalServer& s):server(s){startTimer(5);}
        void timerCallback()override
        {
            if(stopping){juce::MessageManager::getInstance()->stopDispatchLoop();return;}
            try
            {
                const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                server.poll(static_cast<uint64_t>(now));
            }
            catch(const std::exception& e){std::cerr<<"oscairn-engine: "<<e.what()<<'\n';result=1;juce::MessageManager::getInstance()->stopDispatchLoop();}
        }
    };
}
int main(int argc,char** argv)
{
    std::string plugins=OSCAIRN_PLUGIN_DIR,socket;
    bool managed=false;
    for(int i=1;i<argc;++i)
    {
        const std::string arg=argv[i];
        if(arg=="--managed"){managed=true;continue;}
        if(arg=="--help"){std::cout<<"oscairn-engine --socket /private/directory/control.sock [--plugins DIR]\n";return 0;}
        if((arg=="--socket" || arg=="--plugins") && i+1<argc){(arg=="--socket"?socket:plugins)=argv[++i];continue;}
        std::cerr<<"Unknown or incomplete argument: "<<arg<<'\n';return 2;
    }
    if(socket.empty()){std::cerr<<"--socket is required; its parent must be a private directory owned by this user\n";return 2;}
    std::signal(SIGINT,[](int){stopping=1;});std::signal(SIGTERM,[](int){stopping=1;});
    juce::ScopedJuceInitialiser_GUI initialise; // JUCE event loop initialization; no GUI modules linked
    try
    {
        std::unique_ptr<oscairn::host::EngineLease> lease;
        if(managed)lease=std::make_unique<oscairn::host::EngineLease>(socket);
        oscairn::host::AudioEngine audio(plugins);
        oscairn::LocalServer server(audio.control());server.listen(socket);
        Pump pump(server);
        std::cout<<"oscairn-engine: listening on "<<socket<<std::endl;
        // On macOS runDispatchLoop() calls [NSApp run], but this console host
        // has no NSApplication. The bounded loop pumps the native run loop too,
        // so JUCE timers and device callbacks stay serviced without a GUI app.
        while(!stopping && juce::MessageManager::getInstance()->runDispatchLoopUntil(50)) {}
        return pump.result;
    }
    catch(const std::exception& e){std::cerr<<"oscairn-engine: "<<e.what()<<'\n';return 1;}
}

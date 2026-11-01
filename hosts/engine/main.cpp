// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../audio/AudioEngine.h"
#include "oscairn/local_socket.h"
#include "EngineProcess.h"
#include "oscairn/plugin_host.h"
#include <juce_events/juce_events.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
    volatile std::sig_atomic_t stopping=0;
    struct Pump : juce::Timer
    {
        oscairn::LocalServer& server;
        bool managed;
        int result=0;
        Pump(oscairn::LocalServer& s,bool m):server(s),managed(m){startTimer(5);}
        void timerCallback()override
        {
            if(stopping){juce::MessageManager::getInstance()->stopDispatchLoop();return;}
            try
            {
                const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                server.poll(static_cast<uint64_t>(now));
            }
            catch(const std::exception& e){std::cerr<<"oscairn-engine: "<<e.what()<<'\n';result=1;juce::MessageManager::getInstance()->stopDispatchLoop();return;}
            // A managed engine stops when its UIs have quit normally (crashes leave it running),
            // checked after reading this tick's requests, so a window just opening keeps it.
            if(managed && server.released()){stopping=1;juce::MessageManager::getInstance()->stopDispatchLoop();}
        }
    };
}
int main(int argc,char** argv)
{
    std::string plugins=OSCAIRN_PLUGIN_DIR,socket;
    bool managed=false;
    oscairn::host::AudioConfiguration audioConfig;
    try { for(int i=1;i<argc;++i)
    {
        const std::string arg=argv[i];
        if(oscairn::host::AudioConfiguration::isOption(arg) && i+1<argc)
        {
            audioConfig.setOption(arg,argv[++i]);continue;
        }
        if(arg=="--managed"){managed=true;continue;}
        if(arg=="--help"){std::cout<<"oscairn-engine --socket /private/directory/control.sock [--plugins DIR]\n  [--input-device NAME] [--output-device NAME]\n  [--audio-inputs none|0,1,...] [--audio-outputs 0,1,...]\nChannel indices are zero-based; list order defines logical graph channels.\n";return 0;}
        if((arg=="--socket" || arg=="--plugins") && i+1<argc){(arg=="--socket"?socket:plugins)=argv[++i];continue;}
        std::cerr<<"Unknown or incomplete argument: "<<arg<<'\n';return 2;
    }
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
    if(socket.empty()){std::cerr<<"--socket is required; its parent must be a private directory owned by this user\n";return 2;}
    std::signal(SIGINT,[](int){stopping=1;});std::signal(SIGTERM,[](int){stopping=1;});
    juce::ScopedJuceInitialiser_GUI initialize; // JUCE event loop initialization; no GUI modules linked
    // RFC 0010: every plugin in a worker process, the worker found beside this program (in the app)
    // or where it was built. OSCAIRN_PLUGIN_WORKER overrides it; set but empty, plugins run in the engine.
    if(!std::getenv("OSCAIRN_PLUGIN_WORKER"))
    {
        auto worker=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("oscairn-plugin-worker");
        if(!worker.existsAsFile())worker=juce::File(OSCAIRN_PLUGIN_WORKER_PATH);
        auto policy=oscairn::placementPolicy();
        if(worker.existsAsFile())policy.worker=worker.getFullPathName().toStdString();
        else std::cerr<<"oscairn-engine: no plugin worker found; plugins run in the engine\n";
        oscairn::setPlacementPolicy(policy);
    }
    try
    {
        std::unique_ptr<oscairn::host::EngineLease> lease;
        if(managed)lease=std::make_unique<oscairn::host::EngineLease>(socket);
        oscairn::host::AudioEngine audio(plugins,std::move(audioConfig));
        oscairn::LocalServer server(audio.control());server.listen(socket);
        Pump pump(server,managed);
        std::cout<<"oscairn-engine: listening on "<<socket<<std::endl;
        // On macOS runDispatchLoop() calls [NSApp run], but this console host
        // has no NSApplication. The bounded loop pumps the native run loop too,
        // so JUCE timers and device callbacks stay serviced without a GUI app.
        while(!stopping && juce::MessageManager::getInstance()->runDispatchLoopUntil(50)) {}
        return pump.result;
    }
    catch(const std::exception& e){std::cerr<<"oscairn-engine: "<<e.what()<<'\n';return 1;}
}

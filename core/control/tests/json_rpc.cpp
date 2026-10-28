// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include "AllocationGuard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Fixture
    {
        oscairn::Session session{48000,512};
        oscairn::JsonRpc rpc{session};
        oscairn::NodeId synth;
        std::array<float,512> left{}, right{};
        explicit Fixture( const char* path )
        {
            synth = session.add("synth",path);
            session.connectNotes(oscairn::graphInput,synth);
            session.connectAudio(synth,oscairn::graphOutput);
            session.commit();
        }
        void render( uint32_t frames = 512 )
        {
            session.service();
            float* out[]{left.data(),right.data()};
            forbidAllocation = true;
            const bool ok = session.process(out,frames);
            forbidAllocation = false;
            if( !ok ) throw std::runtime_error("render failed");
        }
        void call( const std::string& method, const std::string& params )
        {
            const auto response = rpc.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method + "\",\"params\":" + params + '}');
            if( !response || response->find("\"error\"") != std::string::npos )
                throw std::runtime_error(response.value_or("No response"));
        }
    };
    void equivalence( const char* path )
    {
        Fixture wire(path), direct(path);
        double energy = 0;
        for( unsigned i = 0; i < 500; ++i )
        {
            if( i == 0 )
            {
                wire.call("transport.set", R"({"playing":true,"tempo":137})");
                direct.session.setPlaying(true); direct.session.setTempo(137);
            }
            if( i == 1 )
            {
                wire.call("notes.send", R"({"on":true,"key":57,"velocity":0.73,"channel":2})");
                direct.session.note(true,57,0.73,2);
            }
            if( i == 20 )
            {
                wire.call("params.set", R"({"values":[{"node":"synth","param":1,"value":0.82},{"node":1,"param":2,"value":0.4}]})");
                direct.session.setParameter(direct.synth,1,0.82);
                direct.session.setParameter(direct.synth,2,0.4);
            }
            if( i == 45 )
            {
                wire.call("notes.send", R"({"on":false,"key":57,"channel":2})");
                direct.session.note(false,57,0,2);
            }
            const uint32_t frames = i % 2 ? 37 : 512;
            wire.render(frames); direct.render(frames);
            if( !std::equal(wire.left.begin(),wire.left.begin()+frames,direct.left.begin()) ||
                !std::equal(wire.right.begin(),wire.right.begin()+frames,direct.right.begin()) )
                throw std::runtime_error("JSON and typed controls rendered different audio");
            for( uint32_t j = 0; j < frames; ++j ) energy += wire.left[j]*wire.left[j];
        }
        if( energy < 1 || std::abs(wire.left[36]) > 1e-6f ) throw std::runtime_error("Missing note or unreleased voice");
        std::cout << "ok: JSON and typed control render identical audio without render allocations\n";
    }
}
int main( int argc, char** argv )
{
    if( argc < 2 ) return 2;
    try
    {
        if( argc == 2 ) { equivalence(argv[1]); return 0; }
        // Test-only line driver. Blank output represents no JSON-RPC response.
        Fixture f(argv[1]);
        for( std::string line; std::getline(std::cin,line); )
        {
            if( line == "!render" || line == "!drain" )
            {
                const auto blocks = line == "!drain" ? (oscairn::Session::commandCapacity + kick::clap::Host::maxEvents) / kick::clap::Host::maxBlockEvents + 2 : 1;
                for( uint32_t i = 0; i < blocks; ++i ) { f.render(); f.session.service(); }
                std::cout << "{}\n";
            }
            else if( line.starts_with("!poll ") )
            {
                const auto events = f.rpc.notifications(std::stoull(line.substr(6)));
                std::cout << '[';
                for(size_t i=0;i<events.size();++i) { if(i)std::cout<<','; std::cout<<events[i]; }
                std::cout << "]\n";
            }
            else std::cout << f.rpc.dispatch(line).value_or("") << '\n';
            std::cout.flush();
        }
    }
    catch( const std::exception& e ) { std::cerr << e.what() << '\n'; return 1; }
}

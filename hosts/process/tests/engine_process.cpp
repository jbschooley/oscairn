// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineProcess.h"
#include "oscairn/session.h"
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    using namespace oscairn;
    using namespace oscairn::host;
    void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    struct Directory
    {
        char path[64]="/tmp/oscairn-process-XXXXXX";
        Directory(){check(mkdtemp(path)!=nullptr,"temporary directory");}
        ~Directory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
    };
    volatile std::sig_atomic_t stopping=0;
    int helper(const std::string& path,const std::string& plugins)
    {
        std::signal(SIGTERM,[](int){stopping=1;});
        EngineLease lease(path);
        {std::ofstream pid(path+".pid");pid<<getpid();}
        if(std::filesystem::exists(plugins+"/stall"))
        {
            std::signal(SIGTERM,SIG_IGN); // model an unresponsive device-open operation
            for(;;)::pause();
        }
        Session session(48000,64);
        const auto node=session.add("synth",plugins+"/oscairn-control-fixture.clap");
        session.connectAudio(node,graphOutput);session.connectNotes(graphInput,node);session.commit();
        LocalServer server(session.control());server.listen(path);
        std::array<float,64> left{},right{};float* outputs[]{left.data(),right.data()};
        while(!stopping)
        {
            const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            server.poll(static_cast<uint64_t>(now));session.service();check(session.process(outputs,64),"fake helper render");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    pid_t pid(const std::string& socket)
    {
        for(int i=0;i<100;++i)
        {
            std::ifstream file(socket+".pid");pid_t result=-1;if(file>>result)return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("helper pid missing");
    }
    struct Child
    {
        pid_t value=-1;
        ~Child(){if(value>0){::kill(value,SIGTERM);while(::waitpid(value,nullptr,0)<0 && errno==EINTR){}}}
        void crash(){check(::kill(value,SIGKILL)==0,"kill owned helper");while(::waitpid(value,nullptr,0)<0 && errno==EINTR){}value=-1;}
    };
    void policy(const char* executable,const char* plugins)
    {
        Directory directory;EngineProcess process(executable,plugins,directory.path);
        {
            EngineLease lease(process.socketPath());
            check(process.running(),"lifetime lock indicates running engine");
            bool refused=false;try{EngineLease other(process.socketPath());}catch(const std::runtime_error&){refused=true;}
            check(refused,"duplicate engine lease refused");
            LocalClient client;bool timeout=false;
            try{process.connectOrStart(client,100);}catch(const std::runtime_error&){timeout=true;}
            check(timeout && !std::filesystem::exists(process.socketPath()+".pid"),"a starting engine is not duplicated");
        }
        check(!process.running(),"released lease permits recovery");
        {std::ofstream file(process.socketPath());file<<"keep";}
        bool refused=false;try{EngineLease lease(process.socketPath());}catch(const std::runtime_error&){refused=true;}
        check(refused && std::filesystem::is_regular_file(process.socketPath()),"stale cleanup preserves ordinary files");
        ::unlink(process.socketPath().c_str());
        const auto target=std::string(directory.path)+"/target";{std::ofstream file(target);file<<"keep";}
        std::filesystem::create_symlink(target,process.socketPath());
        refused=false;try{EngineLease lease(process.socketPath());}catch(const std::runtime_error&){refused=true;}
        check(refused && std::filesystem::is_symlink(process.socketPath()),"stale cleanup preserves symlinks");
        Directory stalledPlugins;
        {std::ofstream marker(std::string(stalledPlugins.path)+"/stall");marker<<"stall during startup";}
        EngineProcess stalled(executable,stalledPlugins.path,directory.path);
        LocalClient client;bool timedOut=false;
        try{stalled.connectOrStart(client,150);}catch(const std::runtime_error&){timedOut=true;}
        check(timedOut && !stalled.running(),"timed-out startup stops only its own child and releases the lease");
        std::cout<<"ok: engine lease, startup timeout and unrelated endpoint preservation\n";
    }
    int lifecycle(const char* executable,const char* plugins)
    {
        Directory directory;
        // Probe the platform permission explicitly; child startup errors cannot skip.
        Session probeSession(48000,64);
        try{LocalServer probe(probeSession.control());probe.listen(std::string(directory.path)+"/probe.sock");}
        catch(const LocalServer::BindError& e)
        {
            if((e.code().value()==EPERM || e.code().value()==EACCES) && !std::getenv("OSCAIRN_REQUIRE_SOCKET_BIND"))return 77;
            throw;
        }
        std::string endpoint;
        Child child;
        {
            EngineProcess first(executable,plugins,directory.path),second(executable,plugins,directory.path);
            endpoint=first.socketPath();check(endpoint==second.socketPath(),"launch identity stable");
            LocalClient a,b;std::exception_ptr failureA,failureB;
            std::thread launchA([&]{try{first.connectOrStart(a);}catch(...){failureA=std::current_exception();}});
            std::thread launchB([&]{try{second.connectOrStart(b);}catch(...){failureB=std::current_exception();}});
            launchA.join();launchB.join();
            child.value=pid(endpoint);
            if(failureA)std::rethrow_exception(failureA);if(failureB)std::rethrow_exception(failureB);
            a.call("transport.set",R"({"tempo":143,"playing":true})");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(b.call("transport.get").find("143")!=std::string::npos,"concurrent launchers share the running session");
        }
        // Launcher and both clients are gone. The helper must remain alive.
        LocalClient client;client.connect(endpoint);
        check(client.call("transport.get").find("143")!=std::string::npos,"UI lifetime does not own engine lifetime");
        EngineProcess reopened(executable,plugins,directory.path);reopened.connectOrStart(client);
        check(child.value==pid(endpoint),"reopening reuses helper process");
        child.crash();client.disconnect();
        check(std::filesystem::exists(endpoint),"abrupt helper death leaves stale socket");
        check(!reopened.running(),"helper death offers recovery");
        reopened.connectOrStart(client);child.value=pid(endpoint);
        check(client.call("transport.get").find("120")!=std::string::npos,"explicit recovery starts fresh session");
        client.disconnect();
        {
            bool refused=false;try{EngineLease other(endpoint);}catch(const std::runtime_error&){refused=true;}
            check(refused,"live helper cannot be replaced");
        }
        // A listener without our lifetime lock must also be preserved.
        const auto foreign=std::string(directory.path)+"/foreign.sock";
        LocalServer server(probeSession.control());server.listen(foreign);
        bool refused=false;try{EngineLease other(foreign);}catch(const std::runtime_error&){refused=true;}
        check(refused && std::filesystem::exists(foreign),"reachable unmanaged server preserved");
        std::cout<<"ok: concurrent startup, detach, reattach, helper crash and explicit recovery\n";
        return 0;
    }
}
int main(int argc,char** argv)
{
    try
    {
        if(argc==6 && std::string_view(argv[1])=="--socket" && std::string_view(argv[3])=="--plugins" && std::string_view(argv[5])=="--managed")
            return helper(argv[2],argv[4]);
        if(argc!=3)return 2;
        if(std::string_view(argv[2])=="--policy"){policy(argv[0],argv[1]);return 0;}
        if(std::string_view(argv[2])=="--lifecycle")return lifecycle(argv[0],argv[1]);
        return 2;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

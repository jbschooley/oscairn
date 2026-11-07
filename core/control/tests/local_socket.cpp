// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/local_socket.h"
#include "oscairn/session.h"
#include "oscairn/remote_control.h"
#include "oscairn/wav_file.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <system_error>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
    using namespace oscairn;
    void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    std::array<int,2> pair()
    {
        std::array<int,2> fds;check(socketpair(AF_UNIX,SOCK_STREAM,0,fds.data())==0,"socketpair");
        for(auto fd:fds)
        {
            fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
        }
        return fds;
    }
    ssize_t send(int fd,std::string_view data)
    {
#ifdef MSG_NOSIGNAL
        return ::send(fd,data.data(),data.size(),MSG_NOSIGNAL);
#else
        return ::send(fd,data.data(),data.size(),0);
#endif
    }
    std::string read(int fd)
    {
        std::string result;std::array<char,8192> buffer;
        for(;;){const auto n=recv(fd,buffer.data(),buffer.size(),0);if(n<=0)break;result.append(buffer.data(),n);}
        return result;
    }
    struct Fixture
    {
        Session session{48000,64};LocalServer server{session};
        std::array<float,64> left{},right{};
        explicit Fixture(const char* plugin)
        {
            const auto node=session.add("synth",plugin);
            session.connectNotes(graphInput,node);session.connectAudio(node,graphOutput);session.commit();
        }
        void tick()
        {
            const auto time=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            server.poll(static_cast<uint64_t>(time));session.service();
            float* out[]{left.data(),right.data()};
            forbidAllocation=true;const bool ok=session.process(out,64);forbidAllocation=false;check(ok,"render");
        }
    };
    struct TemporaryDirectory
    {
#ifdef __APPLE__
        char path[64]="/private/tmp/oscairn-socket-XXXXXX";
#else
        char path[64]="/tmp/oscairn-socket-XXXXXX";
#endif
        TemporaryDirectory(){check(mkdtemp(path)!=nullptr,"temporary directory");}
        ~TemporaryDirectory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
        std::string child(const std::string& name)const{return std::string(path)+'/'+name;}
    };
    template<class F> void clientWhileRendering(Fixture& fixture,F&& action)
    {
        std::atomic<bool> done=false;std::exception_ptr failure;
        std::thread client([&]{try{action();}catch(...){failure=std::current_exception();}done=true;});
        while(!done){fixture.tick();std::this_thread::yield();}
        client.join();if(failure)std::rethrow_exception(failure);
    }
    void framing(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        const std::string request=R"({"jsonrpc":"2.0","id":"🎹","method":"transport.set","params":{"tempo":139}})";
        check(send(fd[1],request.substr(0,32))==32,"send fragment");f.tick();
        check(f.session.transportState().tempo==120 && read(fd[1]).empty(),"partial request not executed");
        auto tail=request.substr(32)+"\r\n{broken\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"transport.get\"}\n";
        check(send(fd[1],tail)==static_cast<ssize_t>(tail.size()),"send coalesced lines");f.tick();
        const auto replies=read(fd[1]);
        check(replies.find("🎹")!=std::string::npos && replies.find("-32700")!=std::string::npos && replies.find("\"id\":2")!=std::string::npos,"framing preserves IDs and parse errors isolate a line");
        check(f.session.transportState().tempo==139,"complete request applied");
        ::close(fd[1]);f.tick();check(f.server.clients()==0,"disconnect collected");

        fd=pair();f.server.adopt(fd[0]);
        std::string oversized(65538,' ');size_t written=0;
        for(int i=0;i<100 && f.server.clients();++i)
        {
            const auto n=send(fd[1],std::string_view(oversized).substr(written));if(n>0)written+=n;f.tick();
        }
        check(f.server.clients()==0,"oversized unterminated line disconnects");::close(fd[1]);
        fd=pair();f.server.adopt(fd[0]);
        const std::string half=R"({"jsonrpc":"2.0","id":3,"method":"transport.get"})" "\n";
        send(fd[1],half);shutdown(fd[1],SHUT_WR);f.tick();
        check(read(fd[1]).find("\"id\":3")!=std::string::npos,"write half-close drains the response");::close(fd[1]);
    }
    void slowReader(const char* plugin)
    {
        // Notifications pile up while a client is busy with calls; overflow becomes one resync, not a disconnect.
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);LocalClient client;client.adopt(fd[1]);
        clientWhileRendering(f,[&]{client.call("subscribe",R"({"topics":["params.changed","transport"]})");});
        const auto synth=f.session.nodeId("synth");uint64_t now=1;
        float* out[]{f.left.data(),f.right.data()};
        for(int i=0;i<300;++i){check(f.session.setParameter(synth,1,(i%2)?.2:.8),"parameter change");check(f.session.process(out,64),"render");f.session.service();f.server.poll(now+=20000000);client.poll();}
        const auto notices=client.takeNotifications();
        check(notices.size()<=128 && std::any_of(notices.begin(),notices.end(),[](const auto& n){return n.find("session.resync")!=std::string::npos;}),"overflow becomes resync");
        clientWhileRendering(f,[&]{check(client.call("transport.get").find("\"tempo\"")!=std::string::npos,"replies continue after overflow");});
    }
    // A managed engine's lifetime: it may stop once every UI that keeps it has quit normally;
    // one that leaves without releasing (a crash) keeps it until a UI attaches again, and a
    // client that never attached (a background job) does not count.
    void lifetime(const char* plugin)
    {
        Fixture f(plugin);
        auto connect = [&] { auto fd = pair(); f.server.adopt(fd[0]); auto c = std::make_unique<LocalClient>(); c->adopt(fd[1]); return c; };
        auto settle = [&] { for(int i = 0; i < 4; ++i) f.tick(); };
        check(!f.server.released(), "nothing attached: not released");
        auto desktop = connect(), kick = connect(), job = connect();
        clientWhileRendering(f, [&] { desktop->call("engine.attach"); kick->call("engine.attach"); job->call("transport.get"); });
        clientWhileRendering(f, [&] { desktop->call("engine.release"); });
        desktop.reset(); settle();
        check(!f.server.released(), "another window still keeps it");
        job.reset(); settle();
        check(!f.server.released(), "a background job's connection is not a window");
        kick.reset(); settle(); // the KICK window crashed: it left without releasing
        check(!f.server.released(), "a crash keeps the engine for the UI to come back to");
        auto back = connect();
        clientWhileRendering(f, [&] { back->call("engine.attach"); back->call("engine.release"); });
        back.reset(); settle();
        check(f.server.released(), "the last window quit normally: released");
        // Two windows; one crashes, the other quits normally: the user quit the app.
        Fixture g(plugin);
        auto join = [&] { auto fd = pair(); g.server.adopt(fd[0]); auto c = std::make_unique<LocalClient>(); c->adopt(fd[1]); return c; };
        auto first = join(), second = join();
        clientWhileRendering(g, [&] { first->call("engine.attach"); second->call("engine.attach"); });
        first.reset(); for(int i = 0; i < 4; ++i) g.tick();
        check(!g.server.released(), "one window crashed, the other still open");
        clientWhileRendering(g, [&] { second->call("engine.release"); });
        second.reset(); for(int i = 0; i < 4; ++i) g.tick();
        check(g.server.released(), "and the last one open quits normally: released");
    }
    void clientAndReconnect(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);
            check(client.call("session.hello").find("\"protocolVersion\":1")!=std::string::npos,"hello over socket");
            client.call("subscribe",R"({"topics":["transport","params.changed"]})");
            client.call("params.set",R"({"values":[{"node":"synth","param":1,"value":0.7}]})");
            client.call("notes.send",R"({"on":true,"key":60})");
            client.call("transport.get");
            check(!client.takeNotifications().empty(),"notifications interleaved with replies");
        });
        for(int i=0;i<100;++i){f.tick();check(f.left[0]>.69f,"audio continues when controller disconnects");}
        fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);
            check(client.call("params.get",R"({"values":[{"node":"synth","param":1}]})").find("0.7")!=std::string::npos,"reconnected client sees existing state");
            client.call("notes.send",R"({"on":false,"key":60})");
        });
        f.tick();check(f.left[0]==0,"reconnected controller releases held note");
    }
    void pressure(const char* plugin)
    {
        Fixture f(plugin);auto slow=pair(),good=pair();
        int small=1024;setsockopt(slow[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small));
        f.server.adopt(slow[0]);f.server.adopt(good[0]);
        std::string burst;
        for(int i=0;i<16;++i)burst+=R"({"jsonrpc":"2.0","id":1,"method":"session.hello"})" "\n";
        for(int i=0;i<1000 && f.server.clients()==2;++i){send(slow[1],burst);f.tick();}
        check(f.server.clients()==1,"slow reader disconnected at bounded output limit");
        send(good[1],R"({"jsonrpc":"2.0","id":2,"method":"transport.get"})" "\n");f.tick();
        check(read(good[1]).find("\"id\":2")!=std::string::npos,"slow reader does not block another client");
        ::close(slow[1]);::close(good[1]);f.tick();
        std::vector<int> clients;
        for(size_t i=0;i<LocalServer::maxClients;++i){auto fd=pair();check(f.server.adopt(fd[0]),"accept within limit");clients.push_back(fd[1]);}
        auto excess=pair();check(!f.server.adopt(excess[0]),"client count bounded");::close(excess[1]);
        for(auto fd:clients)::close(fd);
        f.tick();check(f.server.clients()==0,"all connections cleaned");
    }
    void controllerCrash(const char* plugin,const char* executable,const std::string& socketPath={})
    {
        Fixture f(plugin);std::array<int,2> fd{-1,-1};
        if(socketPath.empty()){fd=pair();f.server.adopt(fd[0]);}
        else f.server.listen(socketPath);
        posix_spawn_file_actions_t actions;
        check(posix_spawn_file_actions_init(&actions)==0,"spawn actions");
        // Reserve an inherited descriptor explicitly; the engine endpoint is CLOEXEC.
        if(socketPath.empty())
        {
            check(posix_spawn_file_actions_adddup2(&actions,fd[1],64)==0,"spawn socket descriptor");
            if(fd[1]!=64)check(posix_spawn_file_actions_addclose(&actions,fd[1])==0,"spawn close original");
        }
        std::string mode=socketPath.empty()?"--crash-client":"--crash-client-socket";
        std::string endpoint=socketPath.empty()?"64":socketPath;
        char* args[]{const_cast<char*>(executable),mode.data(),endpoint.data(),nullptr};pid_t pid=-1;
        const int result=posix_spawn(&pid,executable,&actions,nullptr,args,environ);
        posix_spawn_file_actions_destroy(&actions);if(fd[1]>=0)::close(fd[1]);
        check(result==0,"spawn separate controller process");
        struct Reap
        {
            pid_t pid;
            ~Reap(){if(pid>0){::kill(pid,SIGKILL);while(::waitpid(pid,nullptr,0)<0 && errno==EINTR){}}}
        } child{pid};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!f.session.transportState().playing && std::chrono::steady_clock::now()<deadline)
        {f.tick();std::this_thread::yield();}
        check(f.session.transportState().playing,"child controller completed edits");
        check(::kill(pid,SIGKILL)==0,"kill controller process");int status=0;
        check(::waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL,"controller died abruptly");child.pid=-1;
        for(int i=0;i<100;++i){f.tick();check(f.left[0]>.82f,"audio survives controller process crash");}
        check(f.server.clients()==0,"crashed connection collected");
        if(socketPath.empty()){fd=pair();f.server.adopt(fd[0]);}
        clientWhileRendering(f,[&]{
            LocalClient client;
            if(socketPath.empty())client.adopt(fd[1]);else client.connect(socketPath);
            RemoteControl remote(client);
            check(remote.parameter(remote.nodeId("synth"),1)==.83,"new controller recovers existing parameter state");
            check(remote.transportState().playing,"new controller recovers transport state");
            remote.note(false,60,0);
        });
        f.tick();check(f.left[0]==0,"new controller can release note after crash");
    }
    void clientFailures()
    {
        auto fd=pair();LocalClient client;client.adopt(fd[0]);
        bool timeout=false;try{client.call("session.hello","{}",10);}catch(const std::runtime_error&){timeout=true;}
        check(timeout && !client.connected(),"timeout closes uncertain connection without replay");::close(fd[1]);
        fd=pair();client.adopt(fd[0]);
        std::thread peer([&]{
            while(read(fd[1]).empty())std::this_thread::yield();
            send(fd[1],R"({"jsonrpc":"2.0","id":999,"result":{}})" "\n");::close(fd[1]);
        });
        bool mismatch=false;try{client.call("session.hello");}catch(const std::runtime_error&){mismatch=true;}
        peer.join();check(mismatch && !client.connected(),"mismatched response closes connection");
        fd=pair();client.adopt(fd[0]);
        std::thread finalReply([&]{
            while(read(fd[1]).empty())std::this_thread::yield();
            // IDs are monotonic across reconnects; failed calls are never reused.
            send(fd[1],R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})" "\n");::close(fd[1]);
        });
        std::string reply;std::exception_ptr failure;
        try{reply=client.call("session.hello");}catch(...){failure=std::current_exception();}
        finalReply.join();if(failure)std::rethrow_exception(failure);
        check(reply.find("\"ok\":true")!=std::string::npos,"final response survives simultaneous EOF");
        {
            // Requests sent without waiting: a later call's wait leaves their replies queued, in
            // order, for takeNotifications, and more of them than the notification bound is no overflow.
            auto sockets=pair();LocalClient pipelined;pipelined.adopt(sockets[0]);
            std::thread peer([&]{
                std::string got;
                while(std::count(got.begin(),got.end(),'\n')<201){got+=read(sockets[1]);std::this_thread::yield();}
                std::string replies;for(int id=1;id<=201;++id)replies+="{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(id)+",\"result\":{\"n\":"+std::to_string(id)+"}}\n";
                for(size_t sent=0;sent<replies.size();){const auto n=send(sockets[1],std::string_view(replies).substr(sent));if(n>0)sent+=size_t(n);else std::this_thread::yield();}
            });
            for(int i=0;i<200;++i)check(pipelined.send("transport.get")==uint64_t(i+1),"a request sent without waiting has the next ID");
            const auto last=pipelined.call("session.hello");peer.join();
            const auto queued=pipelined.takeNotifications();
            check(last.find("\"n\":201")!=std::string::npos && queued.size()==200 && queued.front().find("\"n\":1}")!=std::string::npos && queued.back().find("\"n\":200}")!=std::string::npos && pipelined.connected(),
                  "replies not waited for stay queued in order, beyond the notification bound");
            ::close(sockets[1]);
        }
        for(const auto* invalid : {
            R"({"jsonrpc":"1.0","id":1,"result":{}})",
            R"({"jsonrpc":"2.0","id":1,"result":{},"error":{}})",
            R"({"jsonrpc":"2.0","id":1,"id":1,"result":{}})",
            R"({"jsonrpc":"2.0","method":17})"})
        {
            auto sockets=pair();LocalClient fresh;fresh.adopt(sockets[0]);
            send(sockets[1],std::string(invalid)+'\n');
            bool rejected=false;try{fresh.poll();}catch(const std::runtime_error&){rejected=true;}
            ::close(sockets[1]);check(rejected && !fresh.connected(),"invalid server envelope closes connection");
        }
    }
    void typedClient(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        Control::SessionStatus host{"Test device",{{MidiEndpoint::Kind::Port,3,"Test MIDI",2}},48000,.25,64,7,{{2,"Input 3",true}},{{4,"Output 5",true},{5,"Output 6",false}},false};
        host.audioUnderruns=3;f.session.setHostStatus(host);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            const auto synth=remote.nodeId("synth");
            check(remote.hasParameter(synth,1) && remote.parameters(synth).size()==1,"remote metadata snapshot");
            // RFC 0025's memory classes over the socket; with no CPU budget nothing is measured or refused.
            remote.setBudget({0,0,1000,100,300,50});
            remote.setAlwaysAvailable({{"oscairn.library",20}});
            check(remote.launchApp({"oscairn.setlist",30}).admitted,"a launch crosses the socket");
            auto memory=remote.memoryState();
            check(memory.device==1000 && memory.floor==300 && memory.margin==50 && memory.running.size()==1 && memory.running[0].app=="oscairn.setlist" &&
                  memory.running[0].reserve==30 && memory.alwaysAvailable.size()==1 && memory.alwaysAvailable[0].reserve==20,"the memory classes cross the socket");
            remote.appClosed("oscairn.setlist");
            check(remote.memoryState().running.empty(),"and an app closing");
            remote.setBudget({});
            check(remote.sessionStatus().audioDevice=="Test device" && remote.sessionStatus().midiInputs==std::vector<MidiEndpoint>{{MidiEndpoint::Kind::Port,3,"Test MIDI",2}},"host status crosses socket");
            check(remote.sessionStatus().audioInputs[0].deviceChannel==2 && !remote.sessionStatus().audioReady && !remote.sessionStatus().audioOutputs[1].available,"device mapping and unavailable channels cross socket");
            check(remote.sessionStatus().audioUnderruns==3 && remote.sessionStatus().deadlineMisses>=7,"the device's underruns cross the socket, apart from deadline misses");
            const auto port=remote.graphState().topology.nodes[0].outputs[0].id;
            const AudioConnection route{{synth,port,0},{graphOutput,0,1},1,.5};
            const auto initial=remote.graphState().topology.audioRoutes.size();
            remote.connectAudio(route);
            const auto routes=remote.graphState().topology.audioRoutes;
            check(routes.size()==initial+1 && std::find(routes.begin(),routes.end(),route)!=routes.end(),"explicit remote route round trip");
            remote.disconnectAudio({route.from,route.to,route.channels});
            check(remote.graphState().topology.audioRoutes.size()==initial,"remote disconnect ignores gain");
            check(remote.graphState().topology.audioLayout.outputs[0].channels==2,"external port metadata crosses socket");
            check(remote.parameterText(synth,1,.5)=="0.500","remote plugin formatting");
            bool ignored=true;try{remote.ignoreMidiInputs({"Test MIDI"});remote.ignoreMidiInputs({});}catch(const std::exception&){ignored=false;}
            check(ignored,"MIDI inputs to ignore cross socket");
            remote.parameterGesture(synth,1,true);remote.setParameter(synth,1,.63);remote.parameterGesture(synth,1,false);
            check(remote.parameter(synth,1)==.63,"accepted value updates local cache");
            remote.setPlaying(true);remote.setTempo(131);remote.note(true,60,1);
            for(int i=0;i<10;++i){client.call("transport.get");remote.service();}
            check(remote.parameter(synth,1)==.63,"notification cache matches accepted value");
            // Graph edits use the same interface and refresh names/metadata.
            const auto effect=remote.add("effect",plugin);
            remote.disconnectAudio(synth,graphOutput);remote.connectAudio(synth,effect);remote.connectAudio(effect,graphOutput);remote.commit();
            check(remote.hasNode(effect) && !remote.graphState().staged,"remote graph snapshot after commit");
            remote.remove(effect);remote.connectAudio(synth,graphOutput);remote.commit();
            check(!remote.hasNode(effect),"remote removal refreshes cache");
            bool bad=false;try{remote.setParameter(synth,999,1);}catch(const std::invalid_argument&){bad=true;}
            check(bad,"remote invalid parameter follows typed error contract");
        });
        for(int i=0;i<10;++i)f.tick();
        check(f.left[0]>.62f,"typed remote client exit leaves audio running");
    }
    // An adapter given another's parameter info (the desktop's refresh, twice a second, on a fresh
    // connection) lists only the plugins whose info changed since, and reads every value as it is now.
    void knownParameters(const char* plugin)
    {
        Fixture f(plugin);
        std::vector<NodeId> effects;for(int i=0;i<5;++i)effects.push_back(f.session.add("effect"+std::to_string(i),plugin));f.session.commit();
        const auto synth=f.session.nodeId("synth");
        auto connect=[&]{auto fd=pair();f.server.adopt(fd[0]);auto c=std::make_unique<LocalClient>();c->adopt(fd[1]);return c;};
        auto started=[](LocalClient& c){const auto id=c.send("transport.get");c.wait(id);return id-1;}; // the requests its start sent
        auto first=connect(),second=connect(),third=connect();
        RemoteControl::KnownParameters known;uint64_t all=0,given=0,replaced=0;
        clientWhileRendering(f,[&]{
            RemoteControl full(*first);all=started(*first);known=full.knownParameters();
            RemoteControl seeded(*second,known);given=started(*second);
            check(known.size()==6 && seeded.knownParameters().size()==6 && seeded.parameters(effects[2]).size()==1 && seeded.parameters(effects[2])[0].name=="Value","each plugin's parameters known from what was given");
            full.replace(effects[0],plugin,"org.oscairn.test.control");
            check(full.knownParameters().at(effects[0]).first!=known.at(effects[0]).first && full.knownParameters().at(effects[1]).first==known.at(effects[1]).first,"a plugin replaced has new parameter info, the others the same");
        });
        std::cout<<"an adapter's start: "<<all<<" requests reading 6 plugins' parameters, "<<given<<" given their info\n";
        check(given+6==all,"given their info, no plugin's parameters are listed");
        check(f.session.setParameter(synth,1,.25),"a value changed after the info was read");
        for(int i=0;i<10;++i)f.tick(); // the plugin takes it and sends it back as its own
        clientWhileRendering(f,[&]{
            RemoteControl again(*third,known);replaced=started(*third);
            check(again.parameter(synth,1)==.25,"a value changed since shows, its plugin's info unchanged");
            check(again.parameters(effects[0]).size()==1,"and the plugin replaced is listed again");
        });
        check(replaced==given+1,"only the plugin replaced is listed");
    }
    // A graph another client changed is read again without waiting: service() returns at once,
    // and the cache shows the change, the node and its parameters, once the read's replies are in.
    void graphReadLater(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair(),other=pair();f.server.adopt(fd[0]);f.server.adopt(other[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            LocalClient second;second.adopt(other[1]);RemoteControl editor(second);
            const auto effect=editor.add("effect",plugin);editor.commit();
            const auto before=client.waits().count;bool read=false;
            for(int i=0;i<2000 && !read;++i){read=remote.service();if(!read)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(client.waits().count==before,"a graph changed elsewhere is read without waiting");
            check(read && remote.hasNode(effect) && remote.hasParameter(effect,1) && remote.parameters(effect).size()==remote.parameters(remote.nodeId("synth")).size(),"and shows the node and its parameters once read");
        });
    }
    // A value the engine pushes while a graph read is out, after the read's graph, is newer than
    // what the read has: the cache shows it once the read is in, not the read's older value. The
    // server is polled by hand here, so the push lands between the read's replies.
    void pushedDuringRead(const char* plugin)
    {
        Fixture f(plugin);
        const auto knob=f.session.addMacro("Knob");f.session.commit();
        auto fd=pair(),other=pair();f.server.adopt(fd[0]);f.server.adopt(other[0]);
        LocalClient client,second;std::unique_ptr<RemoteControl> remote;
        clientWhileRendering(f,[&]{client.adopt(fd[1]);remote=std::make_unique<RemoteControl>(client);second.adopt(other[1]);second.call("session.hello");});
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // past the pushes' 60-a-second pace
        second.send("controls.name","{\"control\":"+std::to_string(knob)+",\"name\":\"Renamed\"}");second.send("graph.commit");
        for(int i=0;i<10 && !remote->reading();++i){f.tick();remote->service();} // the graph.changed, and the read sent
        check(remote->reading(),"a graph read out");
        for(int i=0;i<20 && remote->reading();++i)
        {
            if(i==0)second.send("controls.set","{\"control\":"+std::to_string(knob)+",\"value\":0.75}"); // served after the read's graph.get, pushed among its next replies
            f.tick();remote->service();
        }
        double value=-1;for(const auto& m:remote->graphState().macros)if(m.id==knob)value=m.value;
        check(!remote->reading() && value==.75,"the read in keeps the value pushed after its graph");
    }
    // A mute sent after a graph read's graph.get, and answered before the engine's push of it
    // (pushes go at most 60 times a second), still shows once that older read is in.
    void editOverOlderRead(const char* plugin)
    {
        Fixture f(plugin);const auto part=f.session.addPart("Lead");f.session.commit();
        auto fd=pair(),other=pair();f.server.adopt(fd[0]);f.server.adopt(other[0]);
        LocalClient client,second;std::unique_ptr<RemoteControl> remote;
        clientWhileRendering(f,[&]{client.adopt(fd[1]);remote=std::make_unique<RemoteControl>(client);second.adopt(other[1]);second.call("session.hello");});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        remote->setPartLevel(part,.5,false);remote->commit();
        for(int i=0;i<3;++i){f.tick();remote->service();} // its push sent: the next waits its turn
        second.send("parts.name","{\"part\":"+std::to_string(part)+",\"name\":\"Renamed\"}");second.send("graph.commit");
        for(int i=0;i<10 && !remote->reading();++i){f.tick();remote->service();}
        check(remote->reading(),"a graph read out, its graph.get not yet answered");
        remote->setPartLevel(part,.5,true);remote->commit();
        bool read=false;for(int i=0;i<20 && !read;++i){f.tick();read=remote->service();}
        bool muted=false;for(const auto& p:remote->graphState().parts)if(p.id==part)muted=p.mute;
        check(read && muted,"the read in shows the mute sent after its graph");
    }
    // Connecting reads its snapshots a few at once: on a server polled every 5 ms, as the engine
    // is, it takes five polls here (the hello; the rest and the graph; the parameters; the values;
    // and the one before the hello is sent), where it took nine, one per request.
    void connectPipelined(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        std::atomic<bool> done=false;int polls=0;
        std::thread connecting([&]{LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);done=true;});
        while(!done && polls<100){f.tick();++polls;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        connecting.join();
        check(polls<=6,("connecting takes a few polls, not one per request: "+std::to_string(polls)).c_str());
    }
    // The views' polls go without waiting: service() gives each its reply, one of a kind out at
    // a time; a read forgotten is never given, and the next goes at once.
    void fetches(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            remote.add("effect",plugin);remote.commit();
            std::optional<Control::History> history;std::optional<std::vector<PartMeter>> meters;bool window=true;
            const auto before=client.waits().count;
            check(remote.fetchHistory([&](auto h){history=h;}) && !remote.fetchHistory([](auto){}),"a read sent, and one of a kind at a time");
            check(remote.fetchPartMeters([&](auto m){meters=m;}) && remote.fetchPluginWindow(remote.nodeId("synth"),[&](bool w){window=w;}),"other reads beside it");
            for(int i=0;i<2000 && !(history && meters && !window);++i){remote.service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(client.waits().count==before,"sent and taken without waiting");
            check(history && !history->undo.empty() && history->undo==remote.history().undo && meters && !window,"each given its reply");
            bool given=false;remote.fetchHistory([&](auto){given=true;});remote.forgetFetches();
            bool next=false;check(remote.fetchHistory([&](auto){next=true;}),"a read after forgetting goes at once");
            for(int i=0;i<2000 && !next;++i){remote.service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(next && !given,"and the forgotten one's reply is never given");
        });
    }
    void effectClient(const char* synth,const char* delay)
    {
        Fixture f(synth);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            check(!remote.effectResources(remote.nodeId("synth")),"unsupported resources cross socket");
            const auto first=remote.add("delay",delay),second=remote.addEffectContext("second",first);
            remote.connectAudio(remote.nodeId("synth"),first);remote.connectAudio(first,second);remote.connectAudio(second,graphOutput);remote.commit();
            const auto r=remote.effectResources(second);check(r && r->prepared==2 && r->capacity==16,"remote shared engine accounting");
            check(remote.setParameter(second,2,.7),"remote independent parameters");
            remote.remove(first);remote.commit();check(remote.hasNode(second),"remote child survives parent removal");
        });
    }
    void soundClient(const char* plugin)
    {
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            const auto node=remote.nodeId("synth");check(remote.sounds(node).capacity==8,"native capacity crosses socket");
            const auto id=remote.prepareSound(node,"Remote dark",{{1,.1},{2,.3}});
            check(remote.selectSound(node,id,2),"remote channel selection accepted");
            bool selected=false;
            for(int i=0;i<20 && !selected;++i)for(const auto& c:remote.sounds(node).contexts)if(c.id==id)selected=c.selectedChannels==4 && c.name=="Remote dark";
            check(selected && !remote.discardSound(node,id),"remote selection and busy retirement");
            check(remote.selectSound(node,0,2),"remote switch back");bool retired=false;
            for(int i=0;i<20 && !retired;++i)retired=remote.discardSound(node,id);
            check(retired,"remote retires silent outgoing context");
            const auto defaults=remote.prepareSound(node,"Defaults",{});check(remote.discardSound(node,defaults),"empty patch uses defaults");
            TemporaryDirectory saved;const auto file=(std::filesystem::path(saved.path)/"performance").string();
            remote.savePerformance(file);remote.loadPerformance(file);
            check(remote.nodeId("synth")!=node && !remote.hasNode(node),"remote Performance reload invalidates old node IDs");
            check(remote.sounds(remote.nodeId("synth")).contexts.size()==1,"remote restored sound list");
        });
    }
    void instanceClient(const char* plugin)
    {
        TemporaryDirectory directory;const auto state=(std::filesystem::path(directory.path)/"sound").string();
        auto raw=loadPlugin(plugin,"org.kick.test.synth",48000,64);raw->setParameter(7,.25);raw->saveState(state);
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
            const auto node=remote.nodeId("synth");check(remote.sounds(node).mode=="instances","remote hosted capability");
            const auto id=remote.prepareSound(node,"Saved",{},state);
            check(remote.selectSound(node,id),"remote prepared state selection");
            bool selected=false;for(int i=0;i<40 && !selected;++i)for(const auto& c:remote.sounds(node).contexts)if(c.id==id)selected=c.selectedChannels==65535;
            check(selected,"remote applied hosted selection");
            check(remote.supportsState(node),"remote state capability");
            const auto presets=remote.presets(node);
            check(presets.size()==1 && presets[0].name=="Synth only" && presets[0].uri.rfind("clap-preset:",0)==0 && remote.properties(node).empty(),
                  "the synth fixture's one CLAP preset reaches the remote client, not the others' presets; no properties");
            const auto exported=directory.child("exported");remote.setParameter(node,7,.75);remote.saveState(node,exported);
            auto restored=loadPlugin(plugin,"org.kick.test.synth",48000,64);restored->loadState(exported);
            check(restored->parameterValue(7)==.75,"remote export includes pending parameter edit on selected instance");
            bool refused=false;try{remote.saveState(node,exported);}catch(const std::exception&){refused=true;}check(refused,"remote export cannot overwrite state");
            check(remote.selectSound(node,0),"remote default selection");
            bool retired=false;for(int i=0;i<40 && !retired;++i)retired=remote.discardSound(node,id,true);
            check(retired,"remote explicit hosted retirement");
        });
    }
    void propertyClient(const char* plugin)
    {
        TemporaryDirectory directory;const auto sample=directory.child("sample.wav");
        writeWavFile(sample,std::vector<float>(48000,.25f),1,48000);
        Fixture f(plugin);auto fd=pair();f.server.adopt(fd[0]);
        clientWhileRendering(f,[&]{
            LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);const auto node=remote.nodeId("synth");
            const std::string uri="http://lv2plug.in/plugins/eg-sampler#sample";
            const auto properties=remote.properties(node);bool found=false;
            for(const auto& p:properties)if(p.uri==uri)found=p.type==PropertyType::Path && p.writable;
            check(found,"remote LV2 property metadata");check(!remote.setProperty(node,uri,std::string(sample)),"remote rejects mismatched type");
            check(remote.setProperty(node,uri,FilePath{sample}),"remote accepts sample path");
            bool confirmed=false;for(int i=0;i<2000 && !confirmed;++i){
                const auto value=remote.propertyValue(node,uri);confirmed=value && std::holds_alternative<FilePath>(*value) && std::get<FilePath>(*value).value==sample;
                if(!confirmed)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }check(confirmed,"remote plugin-confirmed typed sample readback");
        });
    }
    void pathPolicy(const char* plugin)
    {
        TemporaryDirectory directory;Fixture f(plugin);
        auto rejected=[&](const std::string& path)
        {
            bool refused=false;
            try{LocalServer server(f.session);server.listen(path);}
            catch(const LocalServer::BindError&){throw std::runtime_error("Invalid endpoint reached bind instead of failing validation");}
            catch(const std::exception&){refused=true;}
            check(refused,"invalid endpoint must be refused");
        };
        rejected("relative.sock");rejected("/"+std::string(200,'a'));
        rejected(directory.child(std::string("bad\0name",8)));
        rejected(directory.child("missing/control.sock"));
        const auto occupied=directory.child("existing");
        {std::ofstream file(occupied);file<<"preserve me";check(file.good(),"can create ordinary file in private directory");}
        rejected(occupied);
        std::ifstream file(occupied);std::string contents;std::getline(file,contents);check(contents=="preserve me","existing file preserved");
        const auto link=directory.child("link");
        std::filesystem::create_symlink(occupied,link);rejected(link);
        check(std::filesystem::is_symlink(link),"existing symlink preserved");
        const auto alias=directory.child("alias");std::filesystem::create_directory_symlink(directory.path,alias);
        rejected(alias+"/control.sock");
        check(chmod(directory.path,0755)==0,"make test directory nonprivate");
        rejected(directory.child("control.sock"));
        check(chmod(directory.path,0700)==0,"restore private directory");
        std::cout<<"ok: invalid socket paths, private-directory policy, existing-file preservation and filesystem writes\n";
    }
    int named(const char* plugin,const char* executable)
    {
        TemporaryDirectory directory;
        const std::string path=directory.child("control.sock");
        // Establish ordinary filesystem access before diagnosing a bind denial.
        {std::ofstream probe(directory.child("write-check"));probe<<"writable";check(probe.good(),"socket directory must be writable");}
        Fixture f(plugin);
        try{f.server.listen(path);}
        catch(const LocalServer::BindError& e)
        {
            if((e.code().value()==EPERM || e.code().value()==EACCES) && !std::getenv("OSCAIRN_REQUIRE_SOCKET_BIND"))
            {std::cout<<"SKIP: bind denied after path ownership/privacy checks and a successful file write: "<<e.what()
                      <<". Check sandbox/socket permissions. OSCAIRN_REQUIRE_SOCKET_BIND=1 makes this a failure.\n";return 77;}
            throw;
        }
        struct stat info{};check(lstat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600,"socket user-only permissions");
        bool refused=false;try{LocalServer other(f.session);other.listen(path);}catch(const std::runtime_error&){refused=true;}
        check(refused && std::filesystem::exists(path),"live endpoint never replaced");
        clientWhileRendering(f,[&]{LocalClient client;client.connect(path);check(client.call("session.hello").find("protocolVersion")!=std::string::npos,"pathname connect and accept");});
        const std::string temporary=directory.child("temporary.sock");
        {LocalServer other(f.session);other.listen(temporary);}
        check(!std::filesystem::exists(temporary),"server removes its own endpoint");
        {LocalServer other(f.session);other.listen(temporary);::unlink(temporary.c_str());std::ofstream file(temporary);file<<"replacement";}
        check(std::filesystem::is_regular_file(temporary),"cleanup preserves replacement file");
        controllerCrash(plugin,executable,directory.child("crash.sock"));
        std::cout<<"ok: pathname listener, permissions, connect, cleanup and process crash/reconnect\n";return 0;
    }
}
// RFC 0011 parts and wires through the remote client and JSON-RPC.
void partsClient(const char* plugin,const char* delay)
{
    Fixture f(plugin);auto fd=pair(),fd2=pair();f.server.adopt(fd[0]);f.server.adopt(fd2[0]);
    const auto fx=f.session.add("fx",delay);f.session.commit();
    clientWhileRendering(f,[&]{
        LocalClient client;client.adopt(fd[1]);RemoteControl remote(client);
        const auto synth=remote.nodeId("synth");
        auto owned=[&](WireOwner o){size_t n=0;for(const auto& w:remote.graphState().wires)n+=w.owner==o;return n;};
        check(owned(WireOwner::User)==2 && owned(WireOwner::Part)==0,"the fixture's connections are user wires");
        const auto part=remote.addPart("Keys");
        remote.insertIntoPart(part,0,synth);remote.insertIntoPart(part,1,fx);
        auto g=remote.graphState();
        check(g.parts.size()==1 && g.parts[0].name=="Keys" && g.parts[0].chain==std::vector<NodeId>{synth,fx},"a part built over the socket");
        check(owned(WireOwner::Part)==3 && owned(WireOwner::User)==2,"its three wires, and the user wires kept");
        uint32_t out=0,in=0;
        for(const auto& n:g.topology.nodes){if(n.id==synth)out=n.outputs.front().id;if(n.id==fx)in=n.inputs.front().id;}
        Wire w;w.from=synth;w.fromPort=out;w.to=fx;w.toPort=in;w.channels={{0,1}};
        const auto id=remote.connectPorts(w);
        remote.setMix(id,.5,.25,true,false);
        bool found=false;
        for(const auto& x:remote.graphState().wires)if(x.id==id)
            found=x.owner==WireOwner::User && x.channels==w.channels && x.level==.5 && x.pan==.25 && x.mute && !x.solo && x.connected;
        check(found,"a user wire with explicit channels and its Mix settings read back");
        const auto room=remote.addPart("Room",{PartInput::Kind::Bus});
        remote.setSend(part,room,.25);remote.setPartOutput(part,room);
        auto withBus=remote.graphState().parts;
        check(withBus.size()==2 && withBus[0].bus==room && withBus[0].sends.size()==1 && withBus[0].sends[0].level==.25 && withBus[1].input.kind==PartInput::Kind::Bus,"a bus, a send and an output through it over the socket");
        remote.setSend(part,room,std::nullopt);remote.setPartOutput(part,0);remote.removePart(room);
        const auto verse=remote.addScene("Verse");remote.setPartLevel(part,.5,true);const auto chorus=remote.addScene("Chorus");
        remote.selectScene(verse);
        check(remote.graphState().scene==verse,"a scene selected shows at once, sent without waiting");
        // The engine's graph.changed is read without waiting: the fader the verse restores shows once it is in.
        for(int i=0;i<500 && !(remote.graphState().parts[0].level==1 && !remote.graphState().parts[0].mute);++i){remote.service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(remote.graphState().scene==verse && remote.graphState().scenes.size()==2 && remote.graphState().parts[0].level==1 && !remote.graphState().parts[0].mute,"scenes over the socket: the verse restores the part's fader");
        remote.removeScene(chorus);remote.removeScene(verse);
        const auto knob=remote.addMacro("Super");MacroTarget level;level.kind=MacroTarget::Kind::PartLevel;level.part=part;level.start=1;level.end=0;level.curve=MacroTarget::Curve::SCurve;
        remote.setMacroTargets(knob,{level});remote.setMacro(knob,.25);
        check(remote.graphState().macros.size()==1 && remote.graphState().macros[0].targets[0].curve==MacroTarget::Curve::SCurve && std::abs(remote.graphState().parts[0].level-(1-.25*.25*(3-2*.25)))<1e-9,"a control over the socket drives the part's level along its curve");
        remote.removeMacro(knob);
        NoteZone split;split.highKey=59;split.transpose=12;
        const std::vector<DeviceRule> devices{{DeviceRule::Kind::BuiltIn},{DeviceRule::Kind::UsbNamed,DeviceRule::Match::Regex,0,"^Key\\w+"},{DeviceRule::Kind::Port,DeviceRule::Match::Contains,5}};
        remote.setPartInput(part,{PartInput::Kind::Notes,0,{split},devices});
        const auto zoned=remote.graphState().parts[0].input.zones;
        check(zoned.size()==1 && zoned[0].highKey==59 && zoned[0].transpose==12,"a part's zone set and read back over the socket");
        check(remote.graphState().parts[0].input.devices==devices,"a part's device selector set and read back over the socket");
        {PartInput limited=remote.graphState().parts[0].input;limited.maxNotes=6;limited.reserve=2;limited.priority=9;remote.setPartInput(part,limited);}
        check(remote.graphState().parts[0].input.maxNotes==6,"a part's note limit set and read back over the socket");
        check(remote.graphState().parts[0].input.reserve==2 && remote.graphState().parts[0].input.priority==9,"its reserve and priority too");
        // Splits over the socket: a split at the end, a lane's Mix and zones, a third lane.
        const auto sp=remote.addSplit(part,2);
        Lane lane;lane.zones={split};lane.level=.5;lane.mute=true;remote.setLane(part,sp,1,lane);
        check(remote.addLane(part,sp)==2,"a third lane");
        g=remote.graphState();
        check(g.parts[0].chain.size()==3 && g.parts[0].chain[2]==(splitMark|sp) && g.parts[0].splits.size()==1 && g.parts[0].splits[0].lanes.size()==3,"a split and its lanes read back over the socket");
        const auto& l=g.parts[0].splits[0].lanes[1];
        check(l.level==.5 && l.mute && l.zones.size()==1 && l.zones[0].highKey==59,"a lane's Mix and zones read back over the socket");
        check(std::any_of(g.wires.begin(),g.wires.end(),[&](const Wire& w){return w.split==sp && w.lane==1 && w.mute;}),"the lane's join wire carries its Mix");
        // Undo over the socket. (The edits above are staged; committing makes them their own step.)
        remote.commit();
        const auto oldName=remote.graphState().parts[0].name;
        remote.setPartName(part,"Renamed");remote.commit();
        check(remote.history().undo=="Rename Renamed","the edit is named over the socket");
        check(remote.undo() && remote.graphState().parts[0].name==oldName && remote.history().redo=="Rename Renamed","undone over the socket");
        check(remote.redo() && remote.graphState().parts[0].name=="Renamed","redone over the socket");
        remote.undo();
        remote.setPartSolo(part,true);
        check(remote.graphState().parts[0].solo,"a part soloed over the socket");
        const auto meters=remote.partMeters();
        check(meters.size()==1 && meters[0].part==part && meters[0].peak>=0,"the parts' meters over the socket");
        const auto wireMeters=remote.wireMeters();
        check(!wireMeters.empty() && wireMeters[0].peak>=0,"the wires' meters over the socket");
        remote.setNodeEnabled(synth,false);remote.commit();
        check(remote.graphState().disabled.count(synth)==1,"a plugin switched off over the socket");
        remote.setNodeEnabled(synth,true);remote.commit();
        const auto firstWire=remote.graphState().wires.front().id;
        remote.setWireEnabled(firstWire,false);
        bool off=false;for(const auto& w:remote.graphState().wires)if(w.id==firstWire)off=!w.enabled;
        check(off && remote.graphState().disabled.empty(),"a wire switched off over the socket, the plugin on again");
        remote.setWireEnabled(firstWire,true);remote.commit();
        remote.setPartSolo(part,false);
        remote.removeSplit(part,sp);
        check(remote.graphState().parts[0].chain==std::vector<NodeId>{synth,fx} && remote.graphState().parts[0].splits.empty(),"the split removed over the socket");
        // A part's fader and pan moved elsewhere (another client, a scene's glide, a controller)
        // reach this client as controls.changed alone: the wires the Mix rows show follow them.
        {
            for(int quiet=0;quiet<50;){remote.service();quiet=remote.reading()?0:quiet+1;std::this_thread::sleep_for(std::chrono::milliseconds(1));} // the reads its own edits pushed
            LocalClient other;other.adopt(fd2[1]);
            other.call("parts.level","{\"part\":"+std::to_string(part)+",\"level\":0.25,\"mute\":false}");
            other.call("parts.pan","{\"part\":"+std::to_string(part)+",\"pan\":-0.5}");
            bool read=false;
            for(int i=0;i<2000 && !(remote.graphState().parts[0].level==.25 && remote.graphState().parts[0].pan==-.5);++i){read=remote.service() || read;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(!read && remote.graphState().parts[0].level==.25 && remote.graphState().parts[0].pan==-.5,"a fader and pan moved elsewhere arrive with no graph read");
            Wire w;for(const auto& x:remote.graphState().wires)if(x.owner==WireOwner::Part && x.part==part && x.fader)w=x;
            check(w.fader && std::abs(w.level-.25)<1e-12 && w.pan==-.5 && !w.mute,"and the part's fader wire, as its Mix row shows it, follows them");
        }
        bool refused=false;try{remote.insertIntoPart(99,0,synth);}catch(const std::exception&){refused=true;}
        check(refused,"an unknown part is refused over the socket");
        remote.movePartNode(part,1,0);remote.removeFromPart(part,fx);remote.disconnectWire(id);
        check(owned(WireOwner::User)==2,"part edits and wire removal over the socket keep the other user wires");
        remote.connectAudio(synth,fx);remote.connectAudio(fx,graphOutput);
        remote.remove(fx,UserWires::Reconnect);remote.removePart(part);remote.commit();
        size_t direct=0;for(const auto& x:remote.graphState().wires)direct+=x.from==synth && x.to==graphOutput && x.owner==WireOwner::User;
        check(direct==2 && !remote.hasNode(fx) && remote.graphState().parts.empty(),"removing a node reconnects its user wires");
    });
    check(f.session.parts().empty() && f.session.wires().size()==3,"the session agrees with the client");
}

int main(int argc,char** argv)
{
    if(argc<2)return 2;
    try
    {
        if(std::string_view(argv[1])=="--crash-client" || std::string_view(argv[1])=="--crash-client-socket")
        {
            if(argc!=3)return 2;
            LocalClient client;
            if(std::string_view(argv[1])=="--crash-client")client.adopt(std::stoi(argv[2]));else client.connect(argv[2]);
            RemoteControl remote(client);
            remote.setParameter(remote.nodeId("synth"),1,.83);remote.note(true,60,1);remote.setPlaying(true);
            for(;;)::pause(); // parent deliberately kills this process
        }
        if(argc==3 && std::string_view(argv[2])=="--properties"){propertyClient(argv[1]);return 0;}
        if(argc==3 && std::string_view(argv[2])=="--instances"){instanceClient(argv[1]);return 0;}
        if(argc==4 && std::string_view(argv[2])=="--effects"){effectClient(argv[1],argv[3]);return 0;}
        if(argc==3 && std::string_view(argv[2])=="--sounds"){soundClient(argv[1]);return 0;}
        if(argc==4 && std::string_view(argv[2])=="--parts"){partsClient(argv[1],argv[3]);return 0;}
        if(argc==3 && std::string_view(argv[2])=="--path-policy"){pathPolicy(argv[1]);return 0;}
        if(argc==3 && std::string_view(argv[2])=="--named")return named(argv[1],argv[0]);
        if(argc!=2)return 2;
        framing(argv[1]);lifetime(argv[1]);slowReader(argv[1]);clientAndReconnect(argv[1]);pressure(argv[1]);clientFailures();typedClient(argv[1]);knownParameters(argv[1]);graphReadLater(argv[1]);pushedDuringRead(argv[1]);editOverOlderRead(argv[1]);connectPipelined(argv[1]);fetches(argv[1]);controllerCrash(argv[1],argv[0]);
        std::cout<<"ok: real Unix streams, framing, backpressure, notifications, reconnect, continuing audio and client failures\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/local_socket.h"
#include "oscairn/json_rpc.h"
#include "sheredom/json.h"
#include "JsonInput.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace oscairn
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        [[noreturn]] void systemError(const char* operation)
        { throw std::system_error(errno,std::generic_category(),operation); }
        bool again() { return errno == EAGAIN || errno == EWOULDBLOCK; }
        void configure(int fd)
        {
            const int status=fcntl(fd,F_GETFL), flags=fcntl(fd,F_GETFD);
            if(status<0 || flags<0 || fcntl(fd,F_SETFL,status|O_NONBLOCK)<0 || fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0)
                systemError("configure socket");
#ifdef SO_NOSIGPIPE
            int yes=1;
            if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))<0)systemError("SO_NOSIGPIPE");
#endif
        }
        sockaddr_un address(const std::string& path)
        {
            sockaddr_un result{};
            if(path.empty() || path.front()!='/' || path.find('\0')!=std::string::npos || path.size()>=sizeof(result.sun_path))
                throw std::invalid_argument("Unix socket requires an absolute path that fits sun_path");
            result.sun_family=AF_UNIX;
            std::memcpy(result.sun_path,path.c_str(),path.size()+1);
            return result;
        }
        ssize_t sendBytes(int fd,const char* p,size_t n)
        {
#ifdef MSG_NOSIGNAL
            return ::send(fd,p,n,MSG_NOSIGNAL);
#else
            return ::send(fd,p,n,0);
#endif
        }
        void waitFor(int fd,short events,Clock::time_point deadline)
        {
            for(;;)
            {
                const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
                if(left<0)throw std::runtime_error("Local RPC timed out; outcome may be unknown");
                pollfd p{fd,events,0};
                const auto result=::poll(&p,1,static_cast<int>(std::min<int64_t>(left+1,60000)));
                if(result<0 && errno==EINTR)continue;
                if(result<0)systemError("poll");
                if(result==0)continue;
                // Read buffered data before treating a simultaneous hangup as EOF.
                if(p.revents&events)return;
                if(p.revents&(POLLERR|POLLHUP|POLLNVAL))throw std::runtime_error("Local RPC disconnected");
            }
        }
        struct Stream
        {
            int fd=-1;
            std::string input,output;
            size_t sent=0;
            bool eof=false;
            explicit Stream(int socket):fd(socket)
            {
                try { configure(fd); } catch(...) { ::close(fd); fd=-1; throw; }
            }
            ~Stream(){if(fd>=0)::close(fd);}
            void close(){if(fd>=0)::close(fd);fd=-1;input.clear();output.clear();sent=0;}
            void queue(std::string_view line)
            {
                if(line.size()+1>LocalServer::maxQueuedBytes-(output.size()-sent))
                    throw std::runtime_error("Local RPC output queue full");
                if(sent){output.erase(0,sent);sent=0;}
                output.append(line);output+='\n';
            }
            void flush()
            {
                size_t budget=65536;
                while(fd>=0 && sent<output.size() && budget)
                {
                    const auto n=sendBytes(fd,output.data()+sent,std::min(budget,output.size()-sent));
                    if(n<0 && errno==EINTR)continue;
                    if(n<0 && again())return;
                    if(n<=0)throw std::runtime_error("Local RPC write disconnected");
                    sent+=static_cast<size_t>(n);budget-=static_cast<size_t>(n);
                }
                if(sent==output.size()){output.clear();sent=0;}
            }
            // Reads at most one chunk; limit includes the newline delimiter. Callers
            // consume complete lines before requesting more bytes.
            bool read(size_t limit)
            {
                if(eof)return false;
                if(input.size()>=limit)throw std::runtime_error("Local RPC message too large");
                std::array<char,4096> buffer;
                ssize_t n;
                do { n=::recv(fd,buffer.data(),std::min(buffer.size(),limit-input.size()),0); } while(n<0 && errno==EINTR);
                if(n<0 && again())return false;
                if(n<0)systemError("recv");
                if(n==0){eof=true;return false;}
                input.append(buffer.data(),static_cast<size_t>(n));return true;
            }
            bool line(std::string& result)
            {
                const auto newline=input.find('\n');
                if(newline==std::string::npos)return false;
                result=input.substr(0,newline);input.erase(0,newline+1);return true;
            }
        };
        using detail::quote;
        using detail::member;
        using Document=std::unique_ptr<json_value_s,decltype(&std::free)>;
        Document frame(std::string_view line)
        {
            size_t depth=0;bool quoted=false,escaped=false;
            for(char c:line)
                if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
                else if(c=='"')quoted=true;
                else if(c=='[' || c=='{'){if(++depth>JsonRpc::maxDepth)throw std::runtime_error("Local RPC reply nesting limit");}
                else if((c==']' || c=='}') && depth)--depth;
            Document doc(json_parse(line.data(),line.size()),&std::free);
            if(!doc || doc->type!=json_type_object)throw std::runtime_error("Malformed local RPC response");
            const auto* version=member(doc.get(),"jsonrpc");
            if(!version || version->type!=json_type_string)throw std::runtime_error("Missing RPC version");
            const auto* text=static_cast<const json_string_s*>(version->payload);
            if(std::string_view(text->string,text->string_size)!="2.0")throw std::runtime_error("Unsupported RPC version");
            const auto* object=static_cast<const json_object_s*>(doc->payload);
            std::set<std::string_view> names;
            for(auto* a=object->start;a;a=a->next)
                if(!names.emplace(a->name->string,a->name->string_size).second)
                    throw std::runtime_error("Duplicate RPC envelope field");
            const auto* method=member(doc.get(),"method");
            const auto* result=member(doc.get(),"result");const auto* error=member(doc.get(),"error");
            if(member(doc.get(),"id"))
            {
                if(method || (result!=nullptr)==(error!=nullptr))throw std::runtime_error("Malformed RPC response envelope");
            }
            else if(!method || method->type!=json_type_string || result || error)
                throw std::runtime_error("Malformed RPC notification envelope");
            return doc;
        }
    }

    struct LocalServer::Impl
    {
        Control& control;
        int listener=-1;
        std::string path;
        dev_t device=0;ino_t inode=0;
        struct Peer { Stream stream; JsonRpc rpc; Peer(int fd,Control& c):stream(fd),rpc(c){} };
        std::vector<std::unique_ptr<Peer>> peers;
        explicit Impl(Control& c):control(c){}
        ~Impl()
        {
            if(listener>=0)::close(listener);
            struct stat info{};
            if(!path.empty() && lstat(path.c_str(),&info)==0 && S_ISSOCK(info.st_mode) && info.st_dev==device && info.st_ino==inode)
                ::unlink(path.c_str());
        }
    };
    LocalServer::LocalServer(Control& c):impl_(std::make_unique<Impl>(c)){}
    LocalServer::~LocalServer()=default;
    size_t LocalServer::clients()const{return impl_->peers.size();}
    void LocalServer::listen(const std::string& path)
    {
        auto& s=*impl_;
        if(s.listener>=0 || !s.path.empty())throw std::logic_error("Local server already bound");
        const auto addr=address(path);
        struct stat parent{},existing{};
        const auto directory=std::filesystem::path(path).parent_path().string();
        if(lstat(directory.c_str(),&parent)<0)systemError("socket parent directory");
        if(!S_ISDIR(parent.st_mode) || parent.st_uid!=geteuid() || (parent.st_mode&077)!=0)
            throw std::runtime_error("Socket parent must be a private directory owned by this user: " + directory);
        if(lstat(path.c_str(),&existing)==0)throw std::runtime_error("Socket path already exists; refusing to replace it: " + path);
        if(errno!=ENOENT)systemError("socket path");
        const int fd=::socket(AF_UNIX,SOCK_STREAM,0);
        if(fd<0)systemError("socket");
        try
        {
            configure(fd);
            if(::bind(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0)throw BindError(errno,path);
            s.path=path;
            if(lstat(path.c_str(),&existing)<0)systemError("stat bound socket");
            s.device=existing.st_dev;s.inode=existing.st_ino;
            if(::chmod(path.c_str(),0600)<0 || ::listen(fd,static_cast<int>(maxClients))<0)systemError("listen Unix socket");
            s.listener=fd;
        }
        catch(...){::close(fd);throw;}
    }
    bool LocalServer::adopt(int fd)
    {
        if(impl_->peers.size()>=maxClients){::close(fd);return false;}
        impl_->peers.push_back(std::make_unique<Impl::Peer>(fd,impl_->control));return true;
    }
    void LocalServer::poll(uint64_t now)
    {
        auto& s=*impl_;
        if(s.listener>=0)
            for(size_t i=0;i<maxClients;++i)
            {
                const auto fd=::accept(s.listener,nullptr,nullptr);
                if(fd<0 && errno==EINTR)continue;
                if(fd<0 && again())break;
                if(fd<0)systemError("accept");
                adopt(fd);
            }
        for(auto& peer:s.peers)
        {
            auto& stream=peer->stream;
            try
            {
                size_t requests=0,reads=0;
                std::string line;
                while(requests<maxRequestsPerTick)
                {
                    if(stream.line(line))
                    {
                        if(line.size()>JsonRpc::maxMessageBytes)throw std::runtime_error("Request too large");
                        if(auto reply=peer->rpc.dispatch(line))stream.queue(*reply);
                        ++requests;
                    }
                    else if(reads++>=16 || !stream.read(JsonRpc::maxMessageBytes+1))break;
                }
                // End of a write-half: finish queued complete requests/responses first.
                if(!stream.eof)
                    for(auto& notice:peer->rpc.notifications(now))stream.queue(notice);
                stream.flush();
                if(stream.eof && stream.input.find('\n')==std::string::npos && stream.output.empty())stream.close();
            }
            catch(const std::exception&){stream.close();} // failure isolates this client
        }
        std::erase_if(s.peers,[](const auto& peer){return peer->stream.fd<0;});
    }

    struct LocalClient::Impl
    {
        std::unique_ptr<Stream> stream;
        uint64_t nextId=1;
        std::deque<std::string> messages;
        size_t messageBytes=0;
        void receive()
        {
            if(!stream)throw std::runtime_error("Local RPC is not connected");
            std::string line;
            size_t reads=0;
            for(;;)
            {
                if(stream->line(line))
                {
                    const bool notification=!member(frame(line).get(),"id");
                    if(messages.size()>=128 || line.size()>LocalServer::maxQueuedBytes-messageBytes)
                    {
                        // A reader that falls behind loses intermediate state, not its connection:
                        // queued notifications collapse into one resync; replies are never dropped.
                        static const std::string resync=R"({"jsonrpc":"2.0","method":"session.resync","params":{"topic":"all"}})";
                        std::erase_if(messages,[&](const std::string& m){const bool drop=!member(frame(m).get(),"id");if(drop)messageBytes-=m.size();return drop;});
                        messageBytes+=resync.size();messages.push_back(resync);
                        if(notification)continue;
                        if(messages.size()>=128 || line.size()>LocalServer::maxQueuedBytes-messageBytes)
                            throw std::runtime_error("Local RPC receive queue full");
                    }
                    messageBytes+=line.size();messages.push_back(std::move(line));
                }
                else if(reads++>=16 || !stream->read(LocalServer::maxQueuedBytes))break;
            }
            if(stream->eof && messages.empty())throw std::runtime_error("Local RPC disconnected");
        }
    };
    LocalClient::LocalClient():impl_(std::make_unique<Impl>()){}
    LocalClient::~LocalClient()=default;
    void LocalClient::disconnect()noexcept{impl_->stream.reset();impl_->messages.clear();impl_->messageBytes=0;}
    bool LocalClient::connected()const{return impl_->stream && !impl_->stream->eof;}
    void LocalClient::adopt(int fd){disconnect();impl_->stream=std::make_unique<Stream>(fd);}
    void LocalClient::connect(const std::string& path,unsigned timeoutMs)
    {
        disconnect();const auto addr=address(path);
        const int fd=::socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)systemError("socket");
        adopt(fd);
        try
        {
            if(::connect(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0)
            {
                if(errno!=EINPROGRESS && !again())systemError("connect Unix socket");
                waitFor(fd,POLLOUT,Clock::now()+std::chrono::milliseconds(timeoutMs));
                int error=0;socklen_t size=sizeof(error);
                if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)systemError("connect status");
                if(error){errno=error;systemError("connect Unix socket");}
            }
        }
        catch(...){disconnect();throw;}
    }
    void LocalClient::poll()
    {
        try{impl_->receive();}catch(...){disconnect();throw;}
    }
    std::vector<std::string> LocalClient::takeNotifications()
    {
        std::vector<std::string> result;
        while(!impl_->messages.empty())
        {
            result.push_back(std::move(impl_->messages.front()));impl_->messages.pop_front();
        }
        impl_->messageBytes=0;return result;
    }
    std::string LocalClient::call(std::string_view method,std::string_view params,unsigned timeoutMs)
    {
        if(!impl_->stream)throw std::runtime_error("Local RPC is not connected");
        // Bound and frame params here; the server performs full JSON validation.
        if(params.size()>JsonRpc::maxMessageBytes || params.find('\n')!=std::string_view::npos || params.find('\r')!=std::string_view::npos)
            throw std::invalid_argument("Local RPC params must be a bounded single-line JSON object");
        // The server performs full JSON validation; this transport never rewrites params.
        const auto id=std::to_string(impl_->nextId++);
        const auto request="{\"jsonrpc\":\"2.0\",\"id\":"+id+",\"method\":"+quote(method)+",\"params\":"+std::string(params)+'}';
        if(request.size()>JsonRpc::maxMessageBytes)throw std::invalid_argument("Local RPC request too large");
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
        try
        {
            auto& stream=*impl_->stream;stream.queue(request);
            while(!stream.output.empty()){stream.flush();if(!stream.output.empty())waitFor(stream.fd,POLLOUT,deadline);}
            for(;;)
            {
                impl_->receive();
                for(auto it=impl_->messages.begin();it!=impl_->messages.end();++it)
                {
                    auto doc=frame(*it);
                    if(const auto* responseId=member(doc.get(),"id"))
                    {
                        if(responseId->type!=json_type_number)throw std::runtime_error("Unexpected RPC response ID");
                        const auto* n=static_cast<const json_number_s*>(responseId->payload);
                        if(std::string_view(n->number,n->number_size)!=id)throw std::runtime_error("Mismatched RPC response ID");
                        auto result=std::move(*it);impl_->messageBytes-=result.size();impl_->messages.erase(it);return result;
                    }
                    if(!member(doc.get(),"method"))throw std::runtime_error("Malformed local RPC notification");
                }
                waitFor(stream.fd,POLLIN,deadline);
            }
        }
        catch(...){disconnect();throw;}
    }
}

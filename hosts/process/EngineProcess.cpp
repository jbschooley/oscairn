// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineProcess.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace oscairn::host
{
    namespace
    {
        namespace fs=std::filesystem;
        using Clock=std::chrono::steady_clock;
        [[noreturn]] void error(const std::string& operation)
        {throw std::system_error(errno,std::generic_category(),operation);}
        struct Descriptor
        {
            int fd=-1;
            explicit Descriptor(int value=-1):fd(value){}
            ~Descriptor(){if(fd>=0)::close(fd);}
            int release(){const auto value=fd;fd=-1;return value;}
            Descriptor(const Descriptor&)=delete;
        };
        void privateDirectory(const std::string& path,bool create)
        {
            if(!fs::path(path).is_absolute())throw std::invalid_argument("Engine runtime directory must be absolute");
            if(create && ::mkdir(path.c_str(),0700)<0 && errno!=EEXIST)error("create engine runtime directory "+path);
            struct stat info{};
            if(::lstat(path.c_str(),&info)<0)error("engine runtime directory "+path);
            if(!S_ISDIR(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&077))
                throw std::runtime_error("Engine runtime directory must be private and owned by this user: "+path);
        }
        int privateFile(const std::string& path,int flags)
        {
            Descriptor file(::open(path.c_str(),flags|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));
            if(file.fd<0)error("open engine file "+path);
            struct stat info{};
            if(::fstat(file.fd,&info)<0)error("stat engine file "+path);
            if(!S_ISREG(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&077) || info.st_nlink!=1)
                throw std::runtime_error("Engine lock/log must be a private regular file: "+path);
            return file.release();
        }
        bool lock(int fd)
        {
            if(::flock(fd,LOCK_EX|LOCK_NB)==0)return true;
            if(errno==EWOULDBLOCK || errno==EAGAIN)return false;
            error("lock engine file");
        }
        bool connect(LocalClient& client,const std::string& path)
        {
            try{client.connect(path,100);return true;}
            catch(const std::system_error& e)
            {
                if(e.code().value()==ENOENT || e.code().value()==ECONNREFUSED)return false;
                throw;
            }
        }
        std::string defaultDirectory()
        {
            if(const auto* dir=std::getenv("OSCAIRN_RUNTIME_DIR");dir && *dir)return dir;
#ifdef __linux__
            if(const auto* dir=std::getenv("XDG_RUNTIME_DIR");dir && *dir)return std::string(dir)+"/oscairn";
#endif
            return "/tmp/oscairn-"+std::to_string(geteuid());
        }
        std::string identity(const std::string& plugins)
        {
            // Stable across launches/platform libraries, unlike std::hash.
            uint64_t hash=14695981039346656037ull;
            for(unsigned char c:plugins){hash^=c;hash*=1099511628211ull;}
            std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return out.str();
        }
    }
    EngineLease::EngineLease(const std::string& path)
    {
        privateDirectory(fs::path(path).parent_path().string(),false);
        Descriptor lease(privateFile(path+".lock",O_RDWR));
        if(!lock(lease.fd))throw std::runtime_error("Engine already running or starting: "+path);
        struct stat before{};
        if(::lstat(path.c_str(),&before)==0)
        {
            if(!S_ISSOCK(before.st_mode))throw std::runtime_error("Refusing to remove a non-socket endpoint: "+path);
            LocalClient probe;
            if(connect(probe,path))throw std::runtime_error("A server already owns this endpoint: "+path);
            struct stat after{};
            if(::lstat(path.c_str(),&after)==0)
            {
                if(before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || !S_ISSOCK(after.st_mode))
                    throw std::runtime_error("Engine endpoint changed during startup: "+path);
                if(::unlink(path.c_str())<0)error("remove stale engine socket "+path);
            }
            else if(errno!=ENOENT)error("recheck stale engine socket "+path);
        }
        else if(errno!=ENOENT)error("check engine socket "+path);
        const auto owner=std::to_string(getpid())+'\n';
        if(::ftruncate(lease.fd,0)<0 || ::write(lease.fd,owner.data(),owner.size())!=static_cast<ssize_t>(owner.size()))
            error("record engine pid");
        lock_=lease.release();
    }
    EngineLease::~EngineLease(){if(lock_>=0)::close(lock_);}
    struct EngineProcess::Impl
    {
        std::string executable,plugins,socket,log;
        AudioConfiguration audio;
        pid_t child=-1;
        void reap(){if(child>0 && ::waitpid(child,nullptr,WNOHANG)==child)child=-1;}
        void stopFailedStartup()
        {
            if(child<=0)return;
            // Only our unreaped child can be stopped, never a PID read from disk.
            const auto state=::waitpid(child,nullptr,WNOHANG);
            if(state!=0){child=-1;return;}
            ::kill(child,SIGTERM);
            for(int i=0;i<20;++i)
            {
                if(::waitpid(child,nullptr,WNOHANG)!=0){child=-1;return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::kill(child,SIGKILL);
            while(::waitpid(child,nullptr,0)<0 && errno==EINTR){}
            child=-1;
        }
        void spawn()
        {
            reap();
            Descriptor output(privateFile(log,O_WRONLY|O_APPEND));
            posix_spawn_file_actions_t actions;
            int result=posix_spawn_file_actions_init(&actions);
            if(result)throw std::system_error(result,std::generic_category(),"initialize engine launch");
            struct Actions{posix_spawn_file_actions_t* value;~Actions(){posix_spawn_file_actions_destroy(value);}}cleanup{&actions};
            auto checked=[](int code){if(code)throw std::system_error(code,std::generic_category(),"configure engine launch");};
            checked(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
            checked(posix_spawn_file_actions_adddup2(&actions,output.fd,STDOUT_FILENO));
            checked(posix_spawn_file_actions_adddup2(&actions,output.fd,STDERR_FILENO));
            posix_spawnattr_t attributes;
            checked(posix_spawnattr_init(&attributes));
            struct Attributes{posix_spawnattr_t* value;~Attributes(){posix_spawnattr_destroy(value);}}attributeCleanup{&attributes};
            checked(posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP));
            checked(posix_spawnattr_setpgroup(&attributes,0));
            std::vector<std::string> argStrings = {
                executable,
                "--socket", socket,
                "--plugins", plugins,
                "--managed"
            };
            const auto audioArgs=audio.arguments();
            argStrings.insert(argStrings.end(),audioArgs.begin(),audioArgs.end());
            std::vector<char*> args;
            for(auto& str : argStrings) args.push_back(str.data());
            args.push_back(nullptr);
            checked(::posix_spawn(&child,executable.c_str(),&actions,&attributes,args.data(),environ));
        }
    };
    EngineProcess::EngineProcess(std::string executable,std::string plugins,std::string directory,const AudioConfiguration& audio):impl_(std::make_unique<Impl>())
    {
        auto& s=*impl_;
        s.executable=fs::absolute(executable).string();s.plugins=fs::canonical(plugins).string();
        audio.validate();s.audio=audio;
        if(directory.empty())directory=defaultDirectory();
        privateDirectory(directory,true);
        std::string key=std::to_string(s.plugins.size())+":"+s.plugins;
        for(const auto& arg:audio.arguments())key+=std::to_string(arg.size())+":"+arg;
        s.socket=directory+"/engine-"+identity(key)+".sock";
        s.log=s.socket+".log";
    }
    EngineProcess::~EngineProcess(){impl_->reap();}
    const std::string& EngineProcess::socketPath()const{return impl_->socket;}
    const std::string& EngineProcess::logPath()const{return impl_->log;}
    bool EngineProcess::running()
    {
        impl_->reap();Descriptor lease(privateFile(impl_->socket+".lock",O_RDWR));return !lock(lease.fd);
    }
    void EngineProcess::connectOrStart(LocalClient& client,unsigned timeoutMs)
    {
        auto& s=*impl_;s.reap();
        if(connect(client,s.socket))return;
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
        Descriptor startup(privateFile(s.socket+".launch",O_RDWR));
        bool launched=false;
        try
        {
            while(Clock::now()<deadline)
            {
                if(connect(client,s.socket))return;
                if(lock(startup.fd))
                {
                    // Another launcher may have completed while we acquired the lock.
                    if(connect(client,s.socket))return;
                    if(!launched && !running()){s.spawn();launched=true;}
                    if(launched && s.child>0)
                    {
                        const auto reaped=::waitpid(s.child,nullptr,WNOHANG);
                        if(reaped==s.child){s.child=-1;throw std::runtime_error("Audio engine exited during startup. See "+s.log);}
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if(connect(client,s.socket))return;
            throw std::runtime_error("Audio engine did not become ready. See "+s.log);
        }
        catch(...){if(launched)s.stopFailedStartup();throw;}
    }
}

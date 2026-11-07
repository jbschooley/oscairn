// SPDX-License-Identifier: AGPL-3.0-or-later
// Installing from package repositories (RFC 0031): each job reads, fetches, checks and unpacks on
// a thread of its own, so the control thread never waits on a drive or the network; the control
// thread moves what it unpacked into place when the job is next asked about.
#include "oscairn/session.h"
#include "oscairn/package.h"
#include "oscairn/repository.h"
#include "JsonInput.h"
#include "sheredom/json.h"
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace oscairn {
namespace {
namespace fs=std::filesystem;
fs::path seenFile(){return dataHome()/"repositories.json";}
std::mutex seenMutex; // the file of serials seen, written by job threads

// The highest serial seen of each repository, so an older root (from a stale or hostile mirror)
// is refused: {"format":"oscairn.repositories","version":1,"seen":{"<repository>":<serial>}}.
std::map<std::string,uint64_t> readSeen(){
    std::map<std::string,uint64_t> out;
    std::error_code e;
    if(!fs::is_regular_file(seenFile(),e) || fs::file_size(seenFile(),e)>1024*1024)return out;
    std::ifstream in(seenFile(),std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    if(!detail::jsonPreflight(bytes,4).empty())return out;
    const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
    const auto* seen=detail::member(doc.get(),"seen");
    if(seen && seen->type==json_type_object)for(auto* m=static_cast<const json_object_s*>(seen->payload)->start;m;m=m->next)
        if(m->value->type==json_type_number){const auto* n=static_cast<const json_number_s*>(m->value->payload);try{out[std::string(m->name->string,m->name->string_size)]=std::stoull(std::string(n->number,n->number_size));}catch(const std::exception&){}}
    return out;
}
// Refuses a root older than one seen, and remembers this one.
void see(const std::string& repository,uint64_t serial){
    std::lock_guard lock(seenMutex);
    auto seen=readSeen();
    if(seen.count(repository) && serial<seen[repository])
        throw std::runtime_error("not a usable repository: its index (serial "+std::to_string(serial)+") is older than one this device has seen ("+std::to_string(seen[repository])+"); a mirror may be out of date or replaying an old index");
    seen[repository]=serial;
    std::string out="{\"format\":\"oscairn.repositories\",\"version\":1,\"seen\":{";
    bool first=true;for(const auto& [id,n]:seen){out+=(first?"":",")+detail::quote(id)+":"+std::to_string(n);first=false;}
    out+="}}\n";
    fs::create_directories(dataHome());
    const auto partial=dataHome()/".repositories.json.partial";
    {std::ofstream o(partial,std::ios::binary|std::ios::trunc);o<<out;if(!o)throw std::runtime_error("cannot write repositories.json");}
    fs::rename(partial,seenFile());
}
// A repository on the web (https:// or http://), or in a folder.
std::unique_ptr<repository::Source> sourceFor(const std::string& source,const std::atomic<bool>* cancel){
    if(source.empty() || source.size()>4096)throw std::invalid_argument("a repository is a folder holding repository.json, or its https:// address");
    if(source.rfind("https://",0)==0 || source.rfind("http://",0)==0)return repository::web(source,cancel);
    return repository::folder(source);
}
fs::path kept(){return dataHome()/"Repositories";} // each repository's index, read last
fs::path scratch(const char* suffix){std::random_device random;return dataHome()/(".install-"+std::to_string(random())+std::to_string(random())+suffix);}
}

struct Session::PackageWork {
    std::mutex mutex;
    uint64_t id=0;           // set before the thread starts
    PackageJob job;          // guarded by mutex
    bool unpacked=false;     // the thread is done and left packages, each in its folder, to move into place
    std::vector<std::pair<Package,fs::path>> packages; // the asset libraries it needs first, then the package
    bool acceptUnsignedCode=false;
    std::atomic<bool> cancel=false; // set when the session goes: a download stops
    std::thread thread;
    ~PackageWork(){cancel=true;if(thread.joinable())thread.join();for(const auto& [p,into]:packages){std::error_code e;fs::remove_all(into,e);}}
};

namespace {
// The trust store's keys as the job needs them, read on the control thread before it starts.
void keys(const std::vector<Control::TrustedKey>& all,std::vector<std::string>& trusted,std::vector<std::string>& revoked){
    for(const auto& k:all){
        if(k.revoked)revoked.push_back(k.key);
        else if(std::find(k.scope.begin(),k.scope.end(),"repositories")!=k.scope.end())trusted.push_back(k.key);
    }
}
}

uint64_t Session::listRepository(const std::string& source){
    if(packageWork_ && packageJob(packageWork_->id).state=="running")throw std::logic_error("a package job is running; wait for it to finish");
    std::vector<std::string> trusted,revoked;keys(trustedKeys(),trusted,revoked);
    auto work=std::make_shared<PackageWork>();
    work->id=work->job.id=++packageJobs_;work->job.state="running";
    auto* w=work.get();
    packageWork_.reset(); // the last job's thread is done; joined here
    w->thread=std::thread([w,source,trusted,revoked]{
        PackageJob out;
        try{
            auto from=sourceFor(source,&w->cancel);
            const auto index=repository::open(*from,trusted,revoked,out.rootKey,kept());
            see(index.repository,index.serial);
            out.repository=index.repository;out.name=index.name;out.serial=index.serial;
            for(const auto& p:index.packages){
                RepositoryPackage listed{p.id,p.name,p.summary,p.license,p.publisherName,p.publisherKey,p.tags,{}};
                for(const auto& v:p.versions)listed.versions.push_back({v.version,v.channel,v.yanked,repository::incompatible(v,OSCAIRN_VERSION,platform()),v.size,v.installed});
                out.packages.push_back(std::move(listed));
            }
            out.state="done";
        }catch(const std::exception& e){out.state="failed";out.error=e.what();}
        out.id=w->id;std::lock_guard lock(w->mutex);w->job=std::move(out);
    });
    packageWork_=std::move(work);
    return packageJobs_;
}

uint64_t Session::installFromRepository(const std::string& source,const std::string& id,const std::string& version,bool acceptUnsignedCode){
    if(packageWork_ && packageJob(packageWork_->id).state=="running")throw std::logic_error("a package job is running; wait for it to finish");
    if(id.empty())throw std::invalid_argument("which package?");
    std::vector<std::string> trusted,revoked;keys(trustedKeys(),trusted,revoked);
    auto work=std::make_shared<PackageWork>();
    work->id=work->job.id=++packageJobs_;work->job.state="running";work->acceptUnsignedCode=acceptUnsignedCode;
    auto* w=work.get();
    packageWork_.reset();
    const auto assetRoot=assets_.root(); // read on the control thread; the job reads which libraries are installed from its own Store
    w->thread=std::thread([w,source,id,version,trusted,revoked,assetRoot]{
        std::string rootKey,error,chosen,repositoryId,name;uint64_t serial=0;
        std::vector<std::pair<Package,fs::path>> unpacked; // the package, then each asset library it needs
        try{
            auto from=sourceFor(source,&w->cancel);
            const auto index=repository::open(*from,trusted,revoked,rootKey,kept());
            see(index.repository,index.serial);
            repositoryId=index.repository;name=index.name;serial=index.serial;
            // A release: fetched (or read in place), checked against the signed index and unpacked.
            auto unpack=[&](const repository::Package& package,const repository::Version& pick){
                // The package file: read in place from a folder, else fetched into the data folder,
                // where a download cut short stays to be resumed; either way it must be the size and
                // SHA-256 the signed index gives.
                const auto blob=repository::blobPath(pick.archiveSha256);
                auto file=from->local(blob);
                fs::path download;
                if(file.empty()){download=dataHome()/(".install-"+pick.archiveSha256+".partial");from->fetch(pick.archiveSha256,pick.archiveSize,download);file=download;}
                struct Gone{fs::path p;~Gone(){std::error_code e;if(!p.empty())fs::remove(p,e);}} gone{download}; // whole: used or refused; one cut short is kept to resume
                std::error_code e;
                if(!fs::is_regular_file(file,e) || fs::file_size(file,e)!=pick.archiveSize || assets::sha256File(file)!=pick.archiveSha256)
                    throw std::runtime_error("refused: the package file for "+package.id+" "+pick.version+" is missing, or not the one the index names (changed or damaged): "+blob);
                unpacked.push_back({Package{},scratch("")});
                auto& [got,into]=unpacked.back();
                got=readPackage(file,into); // every hash and signature in it, and room for it
                // It is the release the index names, from the publisher it names.
                if(got.manifestSha256!=pick.manifestSha256 || got.id!=package.id)
                    throw std::runtime_error("refused: the package file for "+package.id+" "+pick.version+" is not the release the index names");
                if(got.signature.key.empty() || signing::keyId(got.signature.key)!=package.publisherKey)
                    throw std::runtime_error("refused: "+package.id+" "+pick.version+" is not signed by its publisher's key "+package.publisherKey);
            };
            // The newest stable version this device can install that is not yanked, and at least `least`.
            auto newest=[&](const repository::Package& package,const std::string& least){
                const repository::Version* pick=nullptr;
                for(const auto& v:package.versions)
                    if(v.yanked.empty() && (v.channel.empty() || v.channel=="stable") && repository::incompatible(v,OSCAIRN_VERSION,platform()).empty() &&
                       (least.empty() || !repository::older(v.version,least)) && (!pick || repository::older(pick->version,v.version)))pick=&v;
                return pick;
            };
            const auto package=std::find_if(index.packages.begin(),index.packages.end(),[&](const auto& p){return p.id==id;});
            if(package==index.packages.end())throw std::runtime_error("no package "+id+" in "+index.name);
            // The version named (a yanked one too: a lock that names it still gets it), else the newest.
            const repository::Version* pick=version.empty()?newest(*package,{}):nullptr;
            if(!version.empty())for(const auto& v:package->versions)if(v.version==version)pick=&v;
            if(!pick && version.empty())for(const auto& v:package->versions)if(!pick || repository::older(pick->version,v.version))pick=&v; // none fits: say why the newest does not
            if(!pick)throw std::runtime_error(version.empty()?"no version of "+id+" to install":"no version "+version+" of "+id);
            if(const auto why=repository::incompatible(*pick,OSCAIRN_VERSION,platform());!why.empty())throw std::runtime_error(id+" "+pick->version+" cannot be installed here: "+why);
            chosen=pick->version;
            unpack(*package,*pick);
            // The asset libraries it needs that this device lacks, or has too old, from the same repository.
            const auto needs=unpacked.front().first.assetDependencies;
            const assets::Store installed(assetRoot);
            for(const auto& d:needs){
                if(!installed.libraryRoot(d.id,d.version).empty())continue;
                const auto library=std::find_if(index.packages.begin(),index.packages.end(),[&](const auto& p){return p.id==d.id;});
                const auto* release=library==index.packages.end()?nullptr:newest(*library,d.version);
                if(!release)throw std::runtime_error(id+" "+chosen+" needs the asset library "+d.id+" "+d.version+" or later, which "+index.name+" does not have");
                unpack(*library,*release);
                if(unpacked.back().first.kind!="asset")throw std::runtime_error("refused: "+d.id+" is not an asset library");
            }
        }catch(const std::exception& x){error=x.what();for(const auto& [p,into]:unpacked){std::error_code e;fs::remove_all(into,e);}unpacked.clear();}
        std::lock_guard lock(w->mutex);
        w->job.rootKey=rootKey;w->job.repository=repositoryId;w->job.name=name;w->job.serial=serial;w->job.package=id;w->job.version=chosen;
        if(!error.empty()){w->job.state="failed";w->job.error=error;return;}
        std::rotate(unpacked.begin(),unpacked.begin()+1,unpacked.end()); // the libraries first, so the package finds them installed
        w->packages=std::move(unpacked);w->unpacked=true;
    });
    packageWork_=std::move(work);
    return packageJobs_;
}

Control::PackageJob Session::packageJob(uint64_t id){
    if(!packageWork_ || packageWork_->id!=id)throw std::invalid_argument("no package job "+std::to_string(id));
    auto& w=*packageWork_;
    bool unpacked=false;
    {std::lock_guard lock(w.mutex);unpacked=w.unpacked;w.unpacked=false;}
    if(unpacked){
        // Moved into place here, on the control thread, which owns the library and the store: renames, not copies.
        if(w.thread.joinable())w.thread.join();
        auto packages=std::move(w.packages);w.packages.clear();
        PackageJob done;done.state="done";
        // Each asset library it needs, then the package; installUnpacked removes each folder either way.
        for(auto& [package,into]:packages){
            if(done.state=="failed"){std::error_code e;fs::remove_all(into,e);continue;}
            try{
                const auto installed=installUnpacked(package,into,true,{},w.acceptUnsignedCode,true);
                done.installed.added+=installed.added;done.installed.skipped+=installed.skipped;
                done.installed.notes.insert(done.installed.notes.end(),installed.notes.begin(),installed.notes.end());
                done.installed.code.insert(done.installed.code.end(),installed.code.begin(),installed.code.end());
            }catch(const std::exception& e){done.state="failed";done.error=e.what();}
        }
        std::lock_guard lock(w.mutex);w.job.state=done.state;w.job.error=done.error;w.job.installed=done.installed;
    }
    std::lock_guard lock(w.mutex);
    return w.job;
}
}

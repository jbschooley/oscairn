// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../../plugin-host/src/StateFiles.h"
#include "oscairn/wav_file.h"
#include <csignal>
#include <iostream>
#include <sys/resource.h>

int main(){try{
    namespace sf=oscairn::state_files;
    sf::TemporaryDirectory directory;
    struct rlimit saved{};
    if(getrlimit(RLIMIT_FSIZE,&saved))return 1;
    const auto handler=std::signal(SIGXFSZ,SIG_IGN);
    auto limit=saved;limit.rlim_cur=0;
    if(setrlimit(RLIMIT_FSIZE,&limit))return 1;
    bool stateFailed=false,wavFailed=false;
    try{sf::Bundle bundle((directory.path/"snapshot").string());sf::write(bundle.temporary.path/"state","abc",3);bundle.publish();}
    catch(const std::runtime_error&){stateFailed=true;}
    try{oscairn::writeWavFile(directory.path/"test.wav",{.1f,.2f},1,48000);}
    catch(const std::runtime_error&){wavFailed=true;}
    const bool restored=setrlimit(RLIMIT_FSIZE,&saved)==0;std::signal(SIGXFSZ,handler);
    if(!restored || !stateFailed || !wavFailed || std::filesystem::exists(directory.path/"snapshot"))return 1;
    sf::write(directory.path/"good","abc",3);
    if(sf::read(directory.path/"good",3).size()!=3)return 1;
    std::cout<<"ok: buffered write failures are reported before publishing state\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

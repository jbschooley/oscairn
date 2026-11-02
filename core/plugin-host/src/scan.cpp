// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/discovery.h"
#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define OSCAIRN_SCAN_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OSCAIRN_SCAN_SANITIZED 1
#endif
int main(int argc,char** argv) {
    try {
        if(argc!=3 || (std::string(argv[1])!="--worker" && std::string(argv[1])!="--presets"))return 2;
#if defined(__linux__) && !defined(OSCAIRN_SCAN_SANITIZED)
        // Third-party code runs here, as in kick-clap-scan: cap its memory so a runaway scan or
        // preset provider cannot starve the device. macOS does not enforce address-space limits;
        // there only the helper's timeout applies.
        const rlimit cap{1ull<<30,1ull<<30};
        if(setrlimit(RLIMIT_AS,&cap)!=0){std::cerr<<"cannot limit scanner memory\n";return 3;}
#endif
        const auto result=kick::clap::encodeScan(oscairn::inspectPlugin(argv[2],std::string(argv[1])=="--presets"));
        std::cout.write(result.data(),result.size());return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

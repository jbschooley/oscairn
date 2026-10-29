// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/discovery.h"
#include <iostream>
int main(int argc,char** argv) {
    try {
        if(argc!=3 || std::string(argv[1])!="--worker")return 2;
        const auto result=kick::clap::encodeScan(oscairn::inspectPlugin(argv[2]));
        std::cout.write(result.data(),result.size());return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

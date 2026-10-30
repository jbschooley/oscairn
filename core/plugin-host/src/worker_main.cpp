// SPDX-License-Identifier: AGPL-3.0-or-later
// oscairn-plugin-worker FD: hosts one plugin for an engine connected on socket FD (RFC 0010).
#include "Worker.h"
#include <csignal>
#include <cstdlib>

int main(int argc, char** argv)
{
    if(argc != 2) return 2;
    std::signal(SIGPIPE, SIG_IGN);
    return oscairn::worker::run(std::atoi(argv[1]));
}

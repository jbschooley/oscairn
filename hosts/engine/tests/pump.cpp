// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The engine's control socket as the engine serves it (hosts/engine/Pump.h on JUCE's message
// thread, with the session serviced on a 5 ms timer as AudioEngine does), without an audio
// device: a request is answered as soon as it arrives, not at the next tick; what one client
// changes reaches the others at once; and an idle engine does not spin. It prints what it measured.
#include "../Pump.h"
#include "oscairn/session.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

using namespace oscairn;
using Clock = std::chrono::steady_clock;

namespace
{
    double cpuSeconds()
    {
        rusage use{}; getrusage(RUSAGE_SELF, &use);
        return double(use.ru_utime.tv_sec + use.ru_stime.tv_sec) + double(use.ru_utime.tv_usec + use.ru_stime.tv_usec) / 1e6;
    }
    // The session serviced every 5 ms on the message thread, then what changed sent, as AudioEngine's timer does.
    struct Service : juce::Timer
    {
        Session& session; host::Pump& pump;
        Service(Session& s, host::Pump& p) : session(s), pump(p) { startTimer(5); }
        ~Service() override { stopTimer(); }
        void timerCallback() override { session.service(); pump.poll(); }
    };
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    char folder[] = "/tmp/oscairn-pump-XXXXXX";
    if(!::mkdtemp(folder)) return 2;
    const std::string path = std::string(folder) + "/control.sock";
    {
        Session session(48000, 128);
        LocalServer server(session);
        server.listen(path);
        volatile std::sig_atomic_t stopping = 0;
        host::Pump pump(server, false, stopping);
        Service service(session, pump);
        std::atomic<bool> done = false;
        std::vector<double> trips; double pushed = -1, idle = -1;
        std::thread client([&] {
            try
            {
                LocalClient a, b;
                a.connect(path); b.connect(path);
                b.call("subscribe", R"({"topics":["controls.changed"]})");
                // Round trips, spaced so they fall anywhere in the service tick.
                std::mt19937 random(7);
                for(int i = 0; i < 200; ++i)
                {
                    const auto began = Clock::now();
                    a.call("transport.get");
                    trips.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
                    std::this_thread::sleep_for(std::chrono::microseconds(500 + random() % 5000));
                }
                // What a changes, b hears: from a's request to b's notification.
                b.poll(); b.takeNotifications();
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // past the 60 Hz interval
                const auto began = Clock::now();
                a.send("master.set", R"({"level":0.25,"pan":0})");
                for(bool heard = false; !heard && Clock::now() - began < std::chrono::seconds(2);)
                {
                    b.poll();
                    for(const auto& m : b.takeNotifications()) heard = heard || m.find("\"level\":0.25") != std::string::npos;
                    if(heard) pushed = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                    else std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                // Idle, with both clients connected: what the process spends.
                const auto cpu = cpuSeconds(); const auto wall = Clock::now();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                idle = 100 * (cpuSeconds() - cpu) / std::chrono::duration<double>(Clock::now() - wall).count();
            }
            catch(const std::exception& e) { std::fprintf(stderr, "FAIL: client: %s\n", e.what()); ++failures; }
            done = true;
        });
        while(!done) juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        client.join();
        if(!trips.empty())
        {
            std::sort(trips.begin(), trips.end());
            const auto median = trips[trips.size() / 2], worst = trips.back();
            std::printf("round trip: median %.3f ms, worst %.3f ms over %zu requests\n", median, worst, trips.size());
            check(median < 1, "a request is answered as soon as it arrives, not at the next 5 ms tick: median " + std::to_string(median) + " ms");
        }
        std::printf("another client's change heard after %.3f ms\n", pushed);
        check(pushed >= 0 && pushed < 2, "what one client changes reaches the others at once: " + std::to_string(pushed) + " ms");
        std::printf("idle: %.1f%% CPU\n", idle);
        check(idle >= 0 && idle < 50, "an idle engine does not spin: " + std::to_string(idle) + "% CPU");
        check(session.graphState().masterLevel == .25, "the change applied");
    }
    std::filesystem::remove_all(folder);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}

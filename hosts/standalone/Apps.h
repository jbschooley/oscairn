// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <functional>
#include <map>
#include <string>

namespace oscairn::standalone
{
    /// Actions apps offer each other (RFC 0022), inside the desktop app: "pick a Performance",
    /// "add this Performance to a setlist". An app asks by the action's name and never knows
    /// which app answers, so any app offering the action can replace another. Message thread.
    class Actions
    {
    public:
        using Answer = std::function<void(const std::string& result)>; // empty: canceled
        using Handler = std::function<void(const std::string& argument, Answer answer)>;

        // shortcut: one app per action, the first to offer it; the choice between two is RFC 0022 step 3.
        void offer(const std::string& action, const std::string& app, Handler handler) { offers_.try_emplace(action, Offer{app, std::move(handler)}); }
        bool offered(const std::string& action) const { return offers_.count(action) != 0; }
        // Asks whoever offers `action`; `answer` is called once, maybe later. False when no app offers it.
        bool request(const std::string& action, const std::string& argument, Answer answer) const
        {
            const auto it = offers_.find(action);
            if(it == offers_.end()) return false;
            it->second.handler(argument, std::move(answer));
            return true;
        }

    private:
        struct Offer { std::string app; Handler handler; };
        std::map<std::string, Offer> offers_;
    };

    // The actions Oscairn's own apps offer.
    inline constexpr const char* pickPerformance = "oscairn.pick.performance"; // gives back a library ID
    inline constexpr const char* sendPerformance = "oscairn.send.performance"; // takes one; gives back what was done
    inline constexpr const char* pickSound = "oscairn.pick.sound";             // gives back a library ID
}

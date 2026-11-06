// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/physical_controls.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace oscairn
{
    class Session;

    // RFC 0011's instant controls: the controller-mapping service in the engine. A MIDI input
    // thread maps a controller's message (ControllerMapping, the same as the app ran) against a
    // snapshot the control thread compiles from the Session, under a lock only the MIDI threads
    // and the control thread take, never the audio thread. A part's level or mute it plays (on its
    // own or as a control's target) goes straight to the part's live gain (Graph::setLiveGain),
    // which the audio thread reads at its next block, and a plugin's parameter to the graph's
    // queue for its next block (Graph::queueParameter); everything it plays is also queued for
    // the control thread, which applies it to the Session's model (and selects scenes, morphs
    // and moves faders that are not live), woken at once.
    class ControllerInput
    {
    public:
        explicit ControllerInput(Session& session) : session_(session) {}

        // Control thread.
        void set(std::vector<ControlGroup> groups, std::vector<Control::ControllerPort> ports);
        void view(const Control::ControlView& view);
        Control::ControllerState state() const;
        void setWake(std::function<void()> wake) { std::lock_guard lock(lock_); wake_ = std::move(wake); }
        // Applies what the MIDI threads played to the model, then compiles the snapshot again
        // where what it reads has changed.
        void service();
        // After the Session sent its waiting parameter values to the plugins: a parameter a MIDI
        // thread moved since service() took the moves goes to the graph again, so the older value
        // just sent is not the last the plugin hears.
        void resend();

        // A MIDI input thread: a message from the input numbered `source` (the session status's
        // midiInputs). Any number of threads; never called on the audio thread.
        void midi(uint8_t source, const uint8_t* bytes, size_t size) noexcept;

    private:
        using Key = std::tuple<int, NodeId, uint32_t, uint32_t>; // an assignment's kind, node, param, id
        static Key key(const Assignment& a) { return {int(a.kind), a.node, a.param, a.id}; }
        struct Played { Assignment target; double value = 0; };
        // MIDI thread, under the lock.
        double value(const Assignment& target) const;
        void play(const ControllerMapping::Action& action);
        void setFader(uint32_t part, std::optional<double> level, std::optional<bool> mute);
        void send(const Key& parameter);
        // Control thread.
        void apply(const Played& played);
        void refresh(bool rebuild);

        Session& session_;
        mutable std::mutex lock_;
        // Under the lock: what the app sent, the snapshot, and the MIDI threads' state.
        std::vector<ControlGroup> groups_;
        std::vector<Control::ControllerPort> ports_;
        std::map<uint8_t, std::vector<std::pair<std::string, uint32_t>>> bindings_; // by source: each profile's id and port it is
        AssignmentSource source_;
        uint32_t part_ = 0;
        ScreenPage screen_;
        GroupPositions positions_;
        ControllerMapping mapping_;
        std::map<Key, double> values_;          // each target's value, 0 to 1
        std::set<Key> unapplied_;               // targets a MIDI thread set since the control thread last took the moves: newer than the model
        std::vector<Macro> macros_;             // the controls, with their values and bases
        std::map<std::tuple<int, NodeId, uint32_t, uint32_t>, double> written_; // as Session::written_
        uint32_t mappingScene_ = 0, scene_ = 0;
        std::map<uint32_t, uint8_t> slots_;     // each live part's gain slot
        struct Range { double minimum = 0, maximum = 0; bool stepped = false; };
        std::map<Key, Range> ranges_;           // each writable parameter's, for its plain value
        std::vector<Played> played_;            // for the control thread, in order
        uint64_t moves_ = 0;                    // counted, so the control thread knows whether more came
        std::string touched_;
        uint64_t touches_ = 0;
        std::function<void()> wake_;
        // Control thread only: what the snapshot was compiled from.
        uint64_t sequence_ = ~0ull, revision_ = ~0ull;
        bool dirty_ = true;
        std::vector<MidiEndpoint> inputs_;
        Control::LiveValues liveSeen_;           // the model's, as the snapshot read them: a fader moved on screen changes them
    };
}

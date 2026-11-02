// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Desktop controls for what was reachable only through the control API, against a real engine
// session over the control socket, without an audio device or any MIDI device. With SNAPSHOTS,
// each new view is written to SNAPSHOTS-<name>.png.
// - A part's MIDI out on the Part page, and clock out beside the transport (RFC 0012).
//   oscairn_desktop_gaps_test PLUGIN_DIR SCANNER THEMES [SNAPSHOTS]
#include "../DesktopShell.h"
#include "../PartsView.h"
#include "oscairn/local_socket.h"
#include "oscairn/session.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 4) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-gaps-" + std::to_string(::getpid()) + ".sock");
    // A data folder of its own, so the user's library and keys are never read or written.
    const auto data = juce::File::createTempFile("oscairn-gaps-data");
    ::setenv("OSCAIRN_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
    {
        auto session = std::make_unique<Session>(48000, 128);
        // Endpoints described to the session only: no device is opened.
        const MidiEndpoint modx{MidiEndpoint::Kind::Usb, 1, "MODX M"};
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, 3, "KeyLab 61"}}; status.midiOutputs = {modx};
        session->setHostStatus(status);
        auto server = std::make_unique<LocalServer>(*session);
        server->listen(socket.string());
        std::atomic<bool> running = true;
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            while(running)
            {
                server->poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session->service();
                session->process(nullptr, 0, out, 2, 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server.reset(); session.reset();
        });
        {
            standalone::Engine engine(argv[1], socket.string());
            const auto configuration = juce::File::createTempFile("oscairn-gaps-config");
            standalone::Theme theme(configuration, juce::File(argv[3]));
            juce::LookAndFeel::setDefaultLookAndFeel(&theme);
            theme.apply("oscairn-dark");
            auto settle = [] { for(int i = 0; i < 30; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); };
            auto wait = [](auto done) { for(int i = 0; i < 1000 && !done(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); return done(); };
            auto snapshot = [&](juce::Component& c, const juce::String& name) {
                if(argc < 5) return;
                const juce::File out{juce::String(argv[4]) + "-" + name + ".png"};
                out.deleteFile();
                juce::FileOutputStream stream(out);
                juce::PNGImageFormat().writeImageToStream(c.createComponentSnapshot(c.getLocalBounds()), stream);
            };
            const auto plugins = standalone::bundledPlugins(argv[1], argv[2]);
            uint32_t lead = 0;
            engine.edit([&](Control& c) {
                lead = standalone::addPart(c, "Lead", false);
                for(const auto& p : plugins) if(p.name == "Basic Synth") standalone::insertPlugin(c, lead, 0, p);
            });
            check(wait([&] { return engine.graph().parts.size() == 1; }) && engine.editError().empty(), "a part to send from: " + engine.editError());
            auto midiOut = [&] { for(const auto& p : engine.graph().parts) if(p.id == lead) return p.midiOut; return PartMidiOut{}; };

            // MIDI out on the Part page (RFC 0012 step 1): its outputs, then its channel.
            {
                standalone::PartsView view(engine, argv[2]);
                standalone::setDesktopMode(view, 1); view.setEmbedded(true); view.setSize(1180, 820);
                view.showPart(lead);
                juce::ComboBox* channel = nullptr;
                juce::TextButton* outputs = nullptr;
                auto channelBox = [&] { channel = nullptr; for(auto* c : view.getChildren()) if(auto* b = dynamic_cast<juce::ComboBox*>(c); b && b->getName() == "MIDI out channel") channel = b; return channel != nullptr; };
                auto outputsButton = [&] { outputs = nullptr; for(auto* c : view.getChildren()) if(auto* b = dynamic_cast<juce::TextButton*>(c); b && b->getTooltip().contains("MIDI outputs")) outputs = b; return outputs != nullptr; };
                check(channelBox() && outputsButton() && outputs->getButtonText() == "Off" && !channel->isEnabled(), "the Part page shows MIDI out, off");
                engine.edit([&](Control& c) { standalone::setMidiOut(c, lead, {standalone::toggled({}, standalone::ruleFor(modx)), 0}); });
                check(wait([&] { view.refresh(); return outputsButton() && outputs->getButtonText() == "MODX M"; }), "an output chosen shows by name");
                check(channelBox() && channel->isEnabled() && channel->getSelectedId() == 1, "and its channel can be set, each message's own at first");
                if(channel) channel->setSelectedId(4, juce::sendNotificationSync);
                check(wait([&] { return midiOut().channel == 3; }) && midiOut().devices.size() == 1, "the channel menu sets channel 3");
                view.refresh(); settle();
                snapshot(view, "part-midi-out");
            }
            // Clock out beside the transport (RFC 0012 step 2): cabling, kept by the engine.
            {
                standalone::SequencerPane pane(engine);
                standalone::setDesktopMode(pane, 1); pane.setSize(1100, 760);
                pane.refresh();
                check(pane.clockOut().isVisible() && pane.clockOut().getButtonText() == "Clock out: Off", "clock out shows beside Follow MIDI clock, off: " + pane.clockOut().getButtonText().toStdString());
                engine.setClockOutputs({standalone::ruleFor(modx)});
                check(wait([&] { pane.refresh(); return pane.clockOut().getButtonText() == "Clock out: MODX M"; }), "an output chosen for clock shows by name");
                settle();
                snapshot(pane, "clock-out");
                engine.setClockOutputs({});
            }
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            configuration.deleteRecursively();
        }
        running = false;
        control.join();
    }
    std::filesystem::remove(socket);
    data.deleteRecursively();
    if(!failures) std::puts("ok: the desktop's controls for what the control API had first");
    return failures ? 1 : 0;
}

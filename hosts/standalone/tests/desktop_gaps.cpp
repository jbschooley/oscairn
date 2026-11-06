// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Desktop controls for what was reachable only through the control API, against a real engine
// session over the control socket, without an audio device or any MIDI device. With SNAPSHOTS,
// each new view is written to SNAPSHOTS-<name>.png.
// - A part's MIDI out on the Part page, and clock out beside the transport (RFC 0012).
// - Signing an export, its signer, and the keys in Settings (RFC 0006).
// - Choosing a controller's profile (RFC 0024).
// - Asking before an app opens (RFC 0025).
//   oscairn_desktop_gaps_test PLUGIN_DIR SCANNER THEMES [SNAPSHOTS]
#include "../DesktopShell.h"
#include "../Launcher.h"
#include "../LibraryPane.h"
#include "../Panel.h"
#include "../PartsView.h"
#include "../Settings.h"
#include "../Setlist.h"
#include "EngineProcess.h"
#include "oscairn/local_socket.h"
#include "oscairn/package.h"
#include "oscairn/session.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace oscairn;

namespace
{
    // Between the Engine and the server: passes every byte on and counts the requests by
    // method, so a test can say which round trips a view or an edit made. drop() closes every
    // connection, as an engine timing out does.
    class SocketTap
    {
    public:
        SocketTap(std::string listenPath, std::string serverPath) : path_(std::move(listenPath)), server_(std::move(serverPath))
        {
            ::unlink(path_.c_str());
            listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            const auto address = addressOf(path_);
            if(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0 || ::listen(listener_, 16) < 0) throw std::runtime_error("tap listen");
            thread_ = std::thread([this] { run(); });
        }
        ~SocketTap()
        {
            running_ = false; thread_.join();
            for(auto& p : pairs_) { ::close(p.client); ::close(p.server); }
            ::close(listener_); ::unlink(path_.c_str());
        }
        int count(const std::string& method) const { std::lock_guard lock(mutex_); const auto at = counts_.find(method); return at == counts_.end() ? 0 : at->second; }
        void reset() { std::lock_guard lock(mutex_); counts_.clear(); kept_.clear(); pushed_.reset(); ++epoch_; }
        // The requests on connections open since before reset(), leaving out the new ones a job opens.
        int countKept(const std::string& method) const { std::lock_guard lock(mutex_); const auto at = kept_.find(method); return at == kept_.end() ? 0 : at->second; }
        // When the engine's first controls.changed since reset() passed, if one has.
        std::optional<std::chrono::steady_clock::time_point> pushed() const { std::lock_guard lock(mutex_); return pushed_; }
        // Answer a method with an error from here on, as the engine refuses it; empty passes it on again.
        void refuse(const std::string& method, const std::string& message) { std::lock_guard lock(mutex_); if(message.empty()) refusals_.erase(method); else refusals_[method] = message; }
        // Hold a method's requests, and every request after one, until stall(method, false): its
        // reply waits, as an engine busy with it does, and nothing overtakes it.
        void stall(const std::string& method, bool on) { std::lock_guard lock(mutex_); if(on) stalls_.insert(method); else stalls_.erase(method); }
        void drop() { dropping_ = true; while(dropping_) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

    private:
        struct Pair { int client, server; std::string sent, held; int epoch = 0; };
        static sockaddr_un addressOf(const std::string& path)
        {
            sockaddr_un a{}; a.sun_family = AF_UNIX; std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str()); return a;
        }
        static void quiet(int fd) { int yes = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes); }
        static bool writeAll(int to, const std::string& bytes)
        {
            for(size_t done = 0; done < bytes.size();) { const auto w = ::write(to, bytes.data() + done, bytes.size() - done); if(w <= 0) return false; done += size_t(w); }
            return true;
        }
        static std::string field(const std::string& line, const std::string& key, char end)
        {
            const auto at = line.find(key);
            if(at == std::string::npos) return {};
            const auto stop = line.find(end, at + key.size());
            return stop == std::string::npos ? std::string() : line.substr(at + key.size(), stop - at - key.size());
        }
        bool pass(int from, int to) // false once either side is gone
        {
            char buffer[65536];
            const auto n = ::read(from, buffer, sizeof buffer);
            if(n > 0 && std::string_view(buffer, size_t(n)).find("\"method\":\"controls.changed\"") != std::string_view::npos)
                { std::lock_guard lock(mutex_); if(!pushed_) pushed_ = std::chrono::steady_clock::now(); }
            return n > 0 && writeAll(to, std::string(buffer, size_t(n)));
        }
        // The client's requests, by line: counted, and passed on, or answered with the error
        // refuse() set for the method.
        bool forward(Pair& p)
        {
            char buffer[65536];
            const auto n = ::read(p.client, buffer, sizeof buffer);
            if(n <= 0) return false;
            p.sent.append(buffer, size_t(n));
            for(size_t end; (end = p.sent.find('\n')) != std::string::npos; p.sent.erase(0, end + 1))
            {
                const auto line = p.sent.substr(0, end + 1);
                const auto method = field(line, "\"method\":\"", '"'), id = field(line, "\"id\":", ',');
                std::string refusal;
                bool hold = false;
                { std::lock_guard lock(mutex_); ++counts_[method]; if(p.epoch < epoch_) ++kept_[method]; if(const auto at = refusals_.find(method); at != refusals_.end()) refusal = at->second; hold = !p.held.empty() || stalls_.count(method); }
                if(hold) { p.held += line; continue; }
                const bool refused = !refusal.empty() && !id.empty();
                if(!writeAll(refused ? p.client : p.server, refused ? "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32005,\"message\":\"" + refusal + "\"}}\n" : line)) return false;
            }
            return true;
        }
        void run()
        {
            while(running_)
            {
                if(dropping_) { for(auto& p : pairs_) { ::close(p.client); ::close(p.server); } pairs_.clear(); dropping_ = false; }
                if(std::lock_guard lock(mutex_); stalls_.empty()) for(auto& p : pairs_) if(!p.held.empty()) { writeAll(p.server, p.held); p.held.clear(); }
                std::vector<pollfd> fds{{listener_, POLLIN, 0}};
                for(const auto& p : pairs_) { fds.push_back({p.client, POLLIN, 0}); fds.push_back({p.server, POLLIN, 0}); }
                if(::poll(fds.data(), nfds_t(fds.size()), 5) <= 0) continue;
                const size_t polled = pairs_.size(); // a connection accepted now is polled next time
                if(fds[0].revents & POLLIN)
                {
                    const int client = ::accept(listener_, nullptr, nullptr);
                    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
                    const auto address = addressOf(server_);
                    if(client >= 0 && server >= 0 && ::connect(server, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0) { quiet(client); quiet(server); { std::lock_guard lock(mutex_); pairs_.push_back({client, server, {}, {}, epoch_}); } }
                    else { if(client >= 0) ::close(client); if(server >= 0) ::close(server); }
                }
                std::vector<size_t> gone;
                for(size_t i = 0; i < polled; ++i)
                {
                    auto& p = pairs_[i];
                    bool alive = true;
                    if(fds[1 + 2 * i].revents & (POLLIN | POLLHUP | POLLERR)) alive = forward(p);
                    if(alive && (fds[2 + 2 * i].revents & (POLLIN | POLLHUP | POLLERR))) alive = pass(p.server, p.client);
                    if(!alive) gone.push_back(i);
                }
                for(auto i = gone.rbegin(); i != gone.rend(); ++i) { ::close(pairs_[*i].client); ::close(pairs_[*i].server); pairs_.erase(pairs_.begin() + std::ptrdiff_t(*i)); }
            }
        }
        std::string path_, server_;
        int listener_ = -1;
        std::vector<Pair> pairs_;
        std::atomic<bool> running_ = true, dropping_ = false;
        mutable std::mutex mutex_;
        std::map<std::string, int> counts_, kept_;
        int epoch_ = 0;
        std::map<std::string, std::string> refusals_; // method: the error it is answered with
        std::set<std::string> stalls_;
        std::optional<std::chrono::steady_clock::time_point> pushed_;
        std::thread thread_;
    };

    // An engine session of its own, served on a socket from a thread, as the engine process does.
    struct Served
    {
        std::unique_ptr<Session> session = std::make_unique<Session>(48000, 128);
        std::unique_ptr<LocalServer> server = std::make_unique<LocalServer>(*session);
        std::atomic<bool> running = true;
        std::thread thread;
        Served(const std::string& path, const std::function<void(Session&)>& setUp = {})
        {
            if(setUp) setUp(*session);
            server->listen(path);
            thread = std::thread([this] {
                std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
                while(running)
                {
                    server->poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                    session->service(); session->process(nullptr, 0, out, 2, 128);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
        ~Served() { running = false; thread.join(); server.reset(); session.reset(); }
    };
}

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
        std::atomic<bool> running = true, holdRequests = false, jobHeld = false, forcedHold = false;
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            while(running)
            {
                server->poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session->service();
                session->process(nullptr, 0, out, 2, 128);
                // The test leaves a job in flight: while its own connection shows, stop
                // answering, so its calls wait, as a long engine call does. A job asked behind
                // a refresh still gets by: the hold lasts two seconds and then serves one tick,
                // which is longer than any call's wait.
                const auto since = std::chrono::steady_clock::now();
                while(running && (forcedHold.load() || (holdRequests.load() && server->clients() >= 2 && std::chrono::steady_clock::now() - since < std::chrono::milliseconds(2000))))
                    { jobHeld = true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
                jobHeld = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server.reset(); session.reset();
        });
        // The Engine reaches the server through the tap, which counts its requests.
        const auto tapPath = socket.string() + "-tap";
        SocketTap tap(tapPath, socket.string());
        {
            standalone::Engine engine(argv[1], tapPath);
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
            // Signing on export (RFC 0006): off by default; a key made from the export's choices,
            // chosen, signs the archive, whose signer the install dialog then names.
            {
                standalone::ExportChoices choices(engine, true);
                standalone::setDesktopMode(choices, 1);
                check(choices.key().empty() && choices.signWith.getSelectedId() == 1 && choices.signWith.getNumItems() == 2 && choices.signWith.getItemText(1) == "Make a key...",
                      "signing is off at first, with Make a key... offered");
                Control::SigningKey made;
                check(engine.makeSigningKey("stage", made).empty() && made.id.size() == 16, "a key made");
                choices.chooseKey("stage");
                check(choices.key() == "stage" && choices.signWith.getNumItems() == 3 && choices.signWith.getText() == "Sign with stage", "listed, and chosen once made");
                snapshot(choices, "export-sign");
                auto exportTo = [&](const char* name, const std::string& key) {
                    const auto file = data.getChildFile(name);
                    standalone::SoundAction out; out.kind = standalone::SoundAction::ExportPerformance; out.directory = file.getFullPathName().toStdString(); out.signKey = key;
                    engine.soundAction(std::move(out));
                    Control::Archive archive;
                    check(wait([&] { return !engine.soundBusy(); }) && file.existsAsFile() && engine.archiveContents(file.getFullPathName().toStdString(), archive).empty(), std::string("exported: ") + engine.soundMessage());
                    return archive;
                };
                const auto plain = exportTo("Plain.olib", {}), signedArchive = exportTo("Signed.olib", choices.key());
                check(plain.signer.status == "unsigned" && standalone::describeSigner(plain.signer) == "Not signed", "unsigned unless a key is chosen");
                check(signedArchive.signer.status == "unknown" && signedArchive.signer.key == made.id && engine.soundMessage().find("Signed with stage") != std::string::npos,
                      "signed with the key chosen: " + signedArchive.signer.status + " " + engine.soundMessage());
                const auto said = standalone::describeSigner(signedArchive.signer);
                check(said.contains(standalone::fingerprint(made.id)) && said.contains("not a key you trust"), "the install dialog names its signer by fingerprint: " + said.toStdString());
                // A large archive's install dialog: the theme's background, the items scrolling in a
                // viewport between the options and the buttons, every row full height.
                {
                    Control::Archive big = signedArchive;
                    big.items.clear();
                    for(int i = 0; i < 40; ++i) { Control::LibraryItem item; item.id = "item-" + std::to_string(i); item.kind = "sound"; item.name = "Sound " + std::to_string(i); big.items.push_back(item); }
                    Control::Signer build; build.name = "Synth"; build.version = "1.0"; build.platform = "macos-arm64"; build.status = "unsigned";
                    big.code = {build};
                    standalone::InstallChoices install(big);
                    standalone::setDesktopMode(install, 1); install.resized();
                    const auto view = install.view.getBoundsInParent(), go = install.go.getBoundsInParent(), cancel = install.cancel.getBoundsInParent(), code = install.code.getBoundsInParent();
                    bool inList = install.items.size() == 40, fullRows = true;
                    for(auto* b : install.items) { inList = inList && b->getParentComponent() == &install.list; fullRows = fullRows && b->getHeight() == 30; }
                    check(install.getHeight() <= 630 && install.view.getViewedComponent() == &install.list && inList && fullRows && install.list.getHeight() == 26 + 40 * 30 && view.getHeight() < install.list.getHeight(),
                          "the items scroll in a viewport, every row full height");
                    check(install.getLocalBounds().contains(go) && install.getLocalBounds().contains(cancel) && view.getBottom() <= code.getY() && code.getBottom() <= go.getY() && install.go.getButtonText() == "Install anyway",
                          "the warning and the buttons stay in view below the items");
                    const auto image = install.createComponentSnapshot(install.getLocalBounds());
                    check(image.getPixelAt(2, 2) == standalone::ink(standalone::Ink::Background), "the dialog paints the theme's background");
                    snapshot(install, "install-many");
                }
                // An archive of both kinds, mixed: its Performances under one heading, then its
                // Sounds under another, in the archive's order, no kind after a name; still all ticked.
                {
                    Control::Archive mixed = signedArchive;
                    mixed.items.clear(); mixed.code.clear();
                    for(const auto& [kind, name] : std::vector<std::pair<std::string, std::string>>{{"sound", "Bass"}, {"performance", "Set A"}, {"sound", "Keys"}, {"performance", "Set B"}, {"sound", "Pad"}})
                    {
                        Control::LibraryItem item; item.id = name; item.kind = kind; item.name = name; mixed.items.push_back(item);
                    }
                    standalone::InstallChoices install(mixed);
                    standalone::setDesktopMode(install, 1); install.resized();
                    std::vector<std::string> shown, focus;
                    bool ticked = true, idsInOrder = true;
                    for(auto* c : install.list.getChildren())
                    {
                        if(auto* l = dynamic_cast<juce::Label*>(c)) shown.push_back("# " + l->getText().toStdString());
                        if(auto* b = dynamic_cast<juce::ToggleButton*>(c)) { shown.push_back(b->getButtonText().toStdString()); ticked = ticked && b->getToggleState(); }
                    }
                    for(auto* c : install.createFocusTraverser()->getAllComponents(&install))
                        if(c->getParentComponent() == &install.list) focus.push_back(dynamic_cast<juce::Label*>(c) ? "# " + dynamic_cast<juce::Label*>(c)->getText().toStdString() : dynamic_cast<juce::Button*>(c)->getButtonText().toStdString());
                    const std::vector<std::string> want{"# Performances", "Set A", "Set B", "# Sounds", "Bass", "Keys", "Pad"};
                    for(size_t i = 0; i < install.ids.size() && i < 5; ++i) idsInOrder = idsInOrder && install.ids[i] == std::vector<std::string>{"Set A", "Set B", "Bass", "Keys", "Pad"}[i];
                    check(shown == want && ticked && install.ids.size() == 5 && idsInOrder, "the install items under a heading for each kind, Performances first, no suffixes");
                    std::vector<juce::Component*> keys, toggles(install.items.begin(), install.items.end());
                    for(auto* c : juce::KeyboardFocusTraverser().getAllComponents(&install)) if(c->getParentComponent() == &install.list) keys.push_back(c);
                    check(focus == want && keys == toggles, "Tab goes down the items in order, and a screen reader reads each heading before its items");
                    snapshot(install, "install-mixed");
                }
                // A Performance with many parts and scenes to import from: the dialog no taller than
                // the screen, the headings and toggles scrolling in a viewport, the buttons below it.
                {
                    Control::PerformanceContents held;
                    for(uint32_t i = 1; i <= 30; ++i) held.parts.push_back({i, "Part " + std::to_string(i), i == 30});
                    for(uint32_t i = 1; i <= 20; ++i) held.scenes.push_back({i, "Scene " + std::to_string(i)});
                    standalone::ImportChoices choose(held, 600);
                    standalone::setDesktopMode(choose, 1); choose.resized();
                    const auto view = choose.view.getBoundsInParent(), import = choose.import.getBoundsInParent(), cancel = choose.cancel.getBoundsInParent();
                    bool inList = choose.partsHeading.getParentComponent() == &choose.list && choose.scenesHeading.getParentComponent() == &choose.list;
                    for(auto* b : choose.parts) inList = inList && b->getParentComponent() == &choose.list;
                    for(auto* b : choose.scenes) inList = inList && b->getParentComponent() == &choose.list;
                    check(choose.getHeight() <= 600 && choose.view.getViewedComponent() == &choose.list && inList && choose.list.getHeight() == 26 + 30 * 30 + 30 + 20 * 30 && view.getHeight() < choose.list.getHeight(),
                          "the import list scrolls in a viewport, the dialog within the screen: " + std::to_string(choose.getHeight()));
                    check(choose.getLocalBounds().contains(import) && choose.getLocalBounds().contains(cancel) && import.getBottom() <= 600 && !view.isEmpty() && view.getBottom() <= import.getY() && choose.import.isVisible() && choose.cancel.isVisible(),
                          "the import buttons stay on the screen, below the list");
                    // Tab goes down the toggles in order, then Cancel and Import; a screen reader
                    // reads each heading before its toggles.
                    std::vector<juce::Component*> keys, read{&choose.partsHeading};
                    for(auto* b : choose.parts) { keys.push_back(b); read.push_back(b); }
                    read.push_back(&choose.scenesHeading);
                    for(auto* b : choose.scenes) { keys.push_back(b); read.push_back(b); }
                    for(auto* b : {&choose.cancel, &choose.import}) { keys.push_back(b); read.push_back(b); }
                    auto mine = [&](std::vector<juce::Component*> all) { std::erase_if(all, [&](auto* c) { return std::find(read.begin(), read.end(), c) == read.end(); }); return all; };
                    check(mine(juce::KeyboardFocusTraverser().getAllComponents(&choose)) == keys && mine(choose.createFocusTraverser()->getAllComponents(&choose)) == read,
                          "the import toggles and buttons take focus in order, each heading read before its toggles");
                    Control::PerformanceContents few; few.parts = {{1, "Lead", false}, {2, "Pad", false}};
                    standalone::ImportChoices small(few, 600);
                    check(small.getHeight() == 2 * 30 + 100 && !small.scenesHeading.isVisible(), "a short import list is as tall as it needs");
                    for(const auto* name : {"import-many", "import-many-scrolled"})
                    {
                        if(argc < 5) break;
                        juce::Image image(juce::Image::ARGB, choose.getWidth(), choose.getHeight(), true);
                        juce::Graphics g(image); g.fillAll(standalone::ink(standalone::Ink::Background)); // the dialog's background
                        choose.paintEntireComponent(g, true);
                        const juce::File out{juce::String(argv[4]) + "-" + name + ".png"};
                        out.deleteFile(); juce::FileOutputStream stream(out); juce::PNGImageFormat().writeImageToStream(image, stream);
                        choose.view.setViewPosition(0, choose.scenesHeading.getY() - 120); // the last parts and the Scenes heading
                    }
                }
                // The keys in Settings: names and fingerprints, never a private key.
                standalone::Settings settings(engine);
                standalone::setDesktopMode(settings, 1); settings.setSize(640, 560);
                check(settings.keys().size() == 1 && settings.keys()[0].name == "stage" && settings.keys()[0].key.size() == 64 && standalone::fingerprint(made.id).length() == 19,
                      "Settings lists the key by name and fingerprint");
                snapshot(settings, "settings-keys");
            }
            // Choosing a controller's profile (RFC 0024): two profiles fit the SMK37's port, stock
            // and custom firmware; the first by id until the musician chooses. Inputs are names
            // only: no MIDI device is opened.
            {
                auto profiles = standalone::controllerProfiles();
                std::string stock; for(const auto& [id, json] : standalone::builtInProfiles()) if(id == "smk37-elite") stock = json;
                profiles.push_back(standalone::Profile::parse(juce::String(stock).replace("\"smk37-elite\"", "\"smk37-elite-custom\"").replace("(stock firmware)", "(custom firmware)").toStdString()));
                const std::vector<std::string> inputs{"IAC Bus 1", "SMK-37 Elite -Private", "SMK-37 Elite -Master", "SMK-37 Elite -MCP"};
                standalone::ProfileChoices kept;
                standalone::Settings settings(engine);
                standalone::setDesktopMode(settings, 1); settings.setSize(760, 640);
                settings.setControllers({profiles, [&] { return inputs; }, [&] { return kept; }, [&](const std::string& port, const std::vector<std::string>& ids) { kept[port] = ids; }});
                const auto& rows = settings.ports();
                auto profileName = [&](size_t row) { return rows.size() > row ? profiles[rows[row].chosen].name : std::string(); };
                check(rows.size() == 3 && rows[1].port == "SMK-37 Elite -Master" && rows[1].profiles.size() == 2 && profileName(1) == "SMK37 Elite (stock firmware)" && settings.profileBox(1)->isEnabled(),
                      "the SMK37's ports listed; two profiles fit -Master, the first by id in use");
                check(rows.size() == 3 && rows[0].note == "Ignored: a duplicate of another port" && rows[2].note == "Mackie Control" && rows[1].note == "Its controls join",
                      "the duplicate port shown as ignored");
                snapshot(settings, "settings-controllers");
                if(auto* box = settings.profileBox(1)) box->setSelectedId(2, juce::sendNotificationSync);
                check(kept["SMK-37 Elite -Master"] == std::vector<std::string>{"smk37-elite-custom"}, "choosing the custom profile keeps it for the port's name");
                check(wait([&] { return profileName(1) == "SMK37 Elite (custom firmware)"; }), "and it is in use");
                // Identical controllers told apart by channel: each profile that fits, together.
                if(auto* box = settings.profileBox(1)) box->setSelectedId(3, juce::sendNotificationSync);
                check(kept["SMK-37 Elite -Master"] == std::vector<std::string>{"smk37-elite", "smk37-elite-custom"}, "each of them, by MIDI channel, keeps both for the port's name");
                check(wait([&] { return rows.size() > 1 && rows[1].all; }), "and the port shows both in use");
            }
            // Asking before an app opens (RFC 0025): with memory classes, a launch past what is
            // free is refused with the reason; the prepared next entry in the way can go.
            {
                check(engine.launchApp({"oscairn.library", 120ull << 20}).admitted, "without memory classes every app opens");
                engine.appClosed("oscairn.library");
                // The protocol takes a reservation of at most 4 GiB, so a small device.
                constexpr uint64_t MiB = 1ull << 20;
                // The synth measured at 100 MiB, read when the budget is set, so the next song holds memory.
                // Its ID, version and build digest (Session::pluginKey): the graph over the socket does not carry the version.
                std::string key;
                for(const auto& p : plugins) if(p.name == "Basic Synth") key = "org.oscairn.basic-synth@0.1.0 " + bundleDigest(p.path);
                data.getChildFile("measurements.json").replaceWithText("{\"plugins\":{\"" + key + "\":{\"cpu\":0.01,\"rest\":0.01,\"voice\":0,\"voices\":0,\"memory\":" + std::to_string(100 * MiB) + "}}}");
                engine.edit([](Control& c) { c.setBudget({.9, 0, 3072 * MiB, 256 * MiB, 0, 64 * MiB}); });
                check(engine.launchApp({"oscairn.library", 120ull << 20}).admitted, "the Library fits");
                std::string path;
                engine.edit([&](Control& c) {
                    c.savePerformanceToLibrary("Next song", true);
                    Control::LibraryQuery q; q.text = "Next song";
                    for(const auto& item : c.libraryQuery(q).items) path = item.path;
                });
                Control::MemoryState before, after;
                engine.edit([&](Control& c) { before = c.memoryState(); c.preparePerformance(path); after = c.memoryState(); });
                const auto next = after.committed - before.committed;
                check(!path.empty() && after.nextCommitted && next > 0, "the next song prepared ahead, committed: " + std::to_string(after.committed) + "/" + std::to_string(before.committed) + " " + engine.editError());
                // Just too big beside the next song, and small enough without it.
                const uint64_t reserve = after.device - after.reserved - after.margin - after.committed + next / 2;
                std::unique_ptr<standalone::LaunchNotice> notice;
                bool retried = false;
                auto show = [&](std::unique_ptr<standalone::LaunchNotice> n) { notice = std::move(n); };
                check(!standalone::askToLaunch(engine, {"oscairn.groovebox", reserve}, "Groovebox", [&] { retried = true; }, show) && notice && notice->letGo.isVisible(),
                      "refused, with Let the next song go offered");
                if(notice)
                {
                    standalone::setDesktopMode(*notice, 2); notice->setSize(1024, 600);
                    check(notice->message().contains("letting the next entry"), "it says why: " + notice->message().toStdString());
                    snapshot(*notice, "launch-next-song");
                    notice->letGo.onClick();
                    check(wait([&] { return retried && !notice; }), "letting it go closes the notice and tries again");
                }
                Control::MemoryState now;
                engine.edit([&](Control& c) { now = c.memoryState(); });
                check(!now.nextPrepared && std::any_of(now.running.begin(), now.running.end(), [](const auto& a) { return a.app == "oscairn.groovebox"; }), "the next song let go, the Groovebox admitted");
                check(!standalone::askToLaunch(engine, {"oscairn.setlist", 3500 * MiB}, "Setlist", {}, show) && notice && !notice->letGo.isVisible() &&
                      notice->message().contains("close") && notice->message().contains("Library"), "refused past what closing frees, saying what to close: " + (notice ? notice->message().toStdString() : std::string()));
                if(notice) { standalone::setDesktopMode(*notice, 1); notice->setSize(1024, 600); snapshot(*notice, "launch-refused"); notice->ok.onClick(); wait([&] { return !notice; }); }
                engine.appClosed("oscairn.library");
                engine.edit([&](Control& c) { now = c.memoryState(); });
                check(std::none_of(now.running.begin(), now.running.end(), [](const auto& a) { return a.app == "oscairn.library"; }), "a closed app's reservation is let go");
                engine.edit([](Control& c) { c.setBudget({}); });
            }
            // A refresh in flight (finding: the periodic sound refresh every ~30 ticks) must not
            // make the engine busy: it reads on its own connection and makes only short calls,
            // so Record, Save and the library work while it runs.
            {
                holdRequests = true;
                standalone::SoundAction tick; tick.kind = standalone::SoundAction::Refresh;
                engine.soundAction(std::move(tick)); // the periodic refresh, or this one, next
                check(wait([&] { return jobHeld.load(); }), "a refresh job's own connection is in flight");
                check(engine.controlReady(), "a refresh in flight leaves the controls ready");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load(); }), "the refresh finished");
            }
            // The polled reads (meters, undo names, the open Performance's name) come from the
            // Engine's cache while a job holds the engine's thread, so the UI thread neither
            // waits on the socket nor drops the connection over a stalled request.
            {
                std::string probe;
                engine.edit([&](Control& c) {
                    c.savePerformanceToLibrary("Cache probe", true);
                    Control::LibraryQuery q; q.text = "Cache probe";
                    for(const auto& item : c.libraryQuery(q).items) probe = item.path;
                });
                check(wait([&] { return !probe.empty() && engine.editError().empty(); }), "a Performance to prepare: " + engine.editError());
                check(wait([&] { return engine.performanceInfo().has_value() && !engine.history().undo.empty(); }), "the reads are primed");
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = probe;
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare job holds the engine's thread");
                const auto began = std::chrono::steady_clock::now();
                const auto info = engine.performanceInfo(); const auto undo = engine.history().undo; const auto meters = engine.meters();
                const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
                check(info && !undo.empty(), "the reads answer from the cache: " + std::to_string(meters.size()));
                check(engine.connected(), "and no call waits on the stalled socket");
                check(waited < 100000, "at once, not after a round trip: " + std::to_string(waited) + " us");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the prepare finished, the engine answers again");
                engine.edit([](Control& c) { c.addMacro("Cache probe control"); });
                check(wait([&] { return engine.history().undo.find("Cache probe control") != std::string::npos && engine.connected(); }), "the cache follows the engine again once free");
            }
            // A fader dragged sends 60 edits a second: the edits leave the meters' cache alone,
            // so the views polling between them still read the engine at most twenty times a second.
            {
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                const auto node = engine.graph().names.at(synth);
                check(wait([&] { return !engine.soundBusy(); }), "the engine is free for a drag");
                engine.meters(); engine.wireMeters(); engine.history();
                tap.reset();
                const auto began = juce::Time::getMillisecondCounter();
                for(int i = 0; i < 60; ++i) { engine.setParam(node, 4, i % 3); engine.meters(); engine.wireMeters(); engine.history(); }
                const int allowed = int((juce::Time::getMillisecondCounter() - began) / 50) + 1;
                // Sent without waiting: the tap has them once its thread has read them.
                check(wait([&] { return tap.count("params.set") == 60; }), "the drag's 60 edits sent: " + std::to_string(tap.count("params.set")));
                check(tap.count("parts.meters") <= allowed && tap.count("wires.meters") <= allowed && tap.count("edit.history") <= allowed,
                      "the meters and undo names read at their own pace, not per edit: " + std::to_string(tap.count("parts.meters")) + " " + std::to_string(tap.count("wires.meters")) + " " +
                      std::to_string(tap.count("edit.history")) + " of at most " + std::to_string(allowed));
                // The periodic refresh reads on its own connection and changes nothing: a note
                // or a fader move asked while it runs goes at once, not after it.
                check(wait([&] { return !engine.jobRunning(); }), "no job before the refresh");
                standalone::SoundAction tick; tick.kind = standalone::SoundAction::Refresh;
                engine.soundAction(std::move(tick));
                check(engine.jobRunning(), "a refresh runs");
                tap.reset();
                engine.setParam(node, 4, 1); engine.note(true, 60, 1.f); engine.note(false, 60, 0);
                check(engine.jobRunning() && tap.count("notes.send") == 2 && wait([&] { return tap.count("params.set") == 1; }), "a fader move and a note go at once during a refresh: " + std::to_string(tap.count("params.set")) + " " + std::to_string(tap.count("notes.send")));
                // The overrides of the physical controls are read at most four times a second, as Play and a controller ask.
                check(wait([&] { return !engine.jobRunning(); }), "the refresh finished");
                engine.assignments(); tap.reset();
                const auto asked = juce::Time::getMillisecondCounter();
                for(int i = 0; i < 20; ++i) engine.assignments();
                const int most = int((juce::Time::getMillisecondCounter() - asked) / 250) + 1;
                check(tap.count("assignments.get") <= most, "the assignments read at their own pace: " + std::to_string(tap.count("assignments.get")) + " of at most " + std::to_string(most));
            }
            // A fader or a mute (RFC 0011's instant controls) shows in the Engine's state at once,
            // before the engine answers, and the edit never waits on the socket: a stalled reply
            // does not hold it, and a drag reads no graph back.
            {
                check(wait([&] { return !engine.jobRunning() && !engine.soundBusy(); }), "the engine is free for the faders");
                auto part = [&] { for(const auto& p : engine.graph().parts) if(p.id == lead) return p; return Part{}; };
                auto level = [&](double v, bool m) { engine.edit([&, v, m](Control& c) { c.setPartLevel(lead, v, m); c.commit(); }); };
                auto micros = [](auto since) { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count(); };
                tap.reset(); tap.stall("parts.level", true);
                auto began = std::chrono::steady_clock::now();
                level(.3, false);
                const auto edited = micros(began);
                const auto shown = part();
                const auto read = micros(began);
                check(edited < 20000 && engine.connected(), "a level edit with its reply stalled returns at once: " + std::to_string(edited) + " us");
                check(std::abs(shown.level - .3) < 1e-9 && [&] { for(const auto& f : engine.liveValues().parts) if(f.part == lead) return std::abs(f.level - .3) < 1e-9; return false; }(), "and the state shows it before any reply, " + std::to_string(read) + " us after the edit");
                began = std::chrono::steady_clock::now();
                level(.3, true);
                check(micros(began) < 20000 && part().mute, "a mute shows at once too, its reply stalled: " + std::to_string(micros(began)) + " us");
                tap.stall("parts.level", false);
                settle();
                check(engine.connected() && part().mute && std::abs(part().level - .3) < 1e-9 && engine.editError().empty(), "and stays after the engine's answers and pushes");
                check(tap.countKept("graph.get") <= 1, "a mute reads the graph once, for its wires, and a level not at all: " + std::to_string(tap.countKept("graph.get")));
                level(1, false); settle();
                tap.reset(); began = std::chrono::steady_clock::now();
                level(.4, false);
                const bool moved = std::abs(part().level - .4) < 1e-9; const auto state = micros(began);
                check(moved && wait([&] { return tap.pushed().has_value(); }), "the engine pushes the level it took");
                if(const auto at = tap.pushed()) std::printf("instant controls: the state %lld us after the edit, the engine's push %lld us\n", (long long)state,
                                                             (long long)std::chrono::duration_cast<std::chrono::microseconds>(*at - began).count());
                settle(); tap.reset();
                for(int i = 0; i < 60; ++i) { level(i / 60.0, false); juce::MessageManager::getInstance()->runDispatchLoopUntil(1); }
                settle();
                check(tap.count("parts.level") == 60 && tap.countKept("graph.get") == 0, "a drag of 60 moves reads no graph: " + std::to_string(tap.count("parts.level")) + " " + std::to_string(tap.countKept("graph.get")));
                check(std::abs(part().level - 59 / 60.0) < 1e-9 && engine.connected(), "and ends where the drag left it: " + std::to_string(part().level));
                // A refusal is said, and the engine's value shown again.
                tap.refuse("parts.level", "no such fader");
                level(.5, false);
                check(std::abs(part().level - .5) < 1e-9, "a refused edit shows until the engine answers");
                check(wait([&] { return engine.editError() == "no such fader"; }) && wait([&] { return std::abs(part().level - 59 / 60.0) < 1e-9; }), "then its refusal is said and the engine's level shown: " + engine.editError());
                tap.refuse("parts.level", "");
                level(1, false); settle();
                // A window showing the part (the desktop's parts list) redraws a mute edited
                // elsewhere at once, not at its next timer (every 200 ms): drawn off screen.
                standalone::DesktopShell shell(engine, argv[2], standalone::InputMode::Mouse);
                shell.setSize(1280, 800); shell.showTab(standalone::DesktopShell::Tab::Mixer); settle(); // Edit: the parts list shows
                auto drawn = [&] { return shell.createComponentSnapshot(shell.getLocalBounds()); };
                auto same = [](const juce::Image& a, const juce::Image& b) {
                    for(int y = 0; y < a.getHeight(); ++y) for(int x = 0; x < a.getWidth(); ++x) if(a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
                    return true;
                };
                for(const bool muted : {true, false, true, false})
                {
                    const auto before = drawn();
                    level(1, muted);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
                    check(!same(before, drawn()), std::string("the parts list shows the part ") + (muted ? "muted" : "unmuted") + " within a frame");
                }
                settle();
            }
            // The Library reads the library again after a job that may change it, not after the
            // periodic refresh, which changes nothing.
            {
                standalone::Actions actions;
                tap.reset();
                standalone::LibraryPane pane(engine, actions, standalone::LibraryPane::Mode::Play);
                check(wait([&] { return tap.count("library.query") > 0 && !engine.jobRunning(); }), "the Library read once");
                holdRequests = true;
                standalone::SoundAction tick; tick.kind = standalone::SoundAction::Refresh;
                engine.soundAction(std::move(tick));
                check(wait([&] { return jobHeld.load(); }), "a refresh in flight");
                juce::MessageManager::getInstance()->runDispatchLoopUntil(400); // the pane looks at least once meanwhile
                holdRequests = false;
                check(wait([&] { return !engine.jobRunning() && !jobHeld.load(); }), "the refresh finished");
                tap.reset();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
                check(tap.count("library.query") == 0, "the Library is not read again after a refresh: " + std::to_string(tap.count("library.query")));
            }
            // A Parts view not on screen (the one under Play, a background app's) polls nothing:
            // on the Mixer, shown, it would read the meters twenty times a second.
            {
                standalone::PartsView view(engine, argv[2]);
                view.setSize(1024, 600); view.showPart(standalone::PartsView::mixerPage);
                check(!view.isShowing(), "the view is not on screen");
                tap.reset();
                for(int i = 0; i < 50; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                check(tap.count("parts.meters") == 0, "a hidden Mixer reads no meters: " + std::to_string(tap.count("parts.meters")));
            }
            // An action asked while a job runs is refused, and said: a dropped Save must not
            // look saved.
            {
                // The Performance app's toolbar Save shows it would be refused, and why; every
                // Save also asks Engine::refusal before it sends, and alerts with its reason.
                standalone::DesktopShell shell(engine, argv[2], standalone::InputMode::Mouse);
                shell.onSave = [] {}; shell.setSize(1280, 800);
                juce::TextButton* saveButton = nullptr;
                for(auto* c : shell.getChildren()) if(auto* b = dynamic_cast<juce::TextButton*>(c); b && b->getButtonText() == "Save") saveButton = b;
                check(saveButton && wait([&] { return saveButton->isEnabled(); }), "the toolbar's Save is offered while the engine is free");
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare of a missing folder holds the engine's thread");
                check(engine.refusal().find("busy") != std::string::npos, "a Save asked now is refused as busy: " + engine.refusal());
                standalone::SoundAction save; save.kind = standalone::SoundAction::SaveToLibrary;
                engine.soundAction(std::move(save));
                check(engine.soundMessage().find("busy") != std::string::npos, "a Save dropped while busy says so: " + engine.soundMessage());
                check(saveButton && wait([&] { return !saveButton->isEnabled() && saveButton->getTooltip().contains("busy"); }),
                      "the toolbar's Save shows it is refused, and why: " + (saveButton ? saveButton->getTooltip().toStdString() : std::string()));
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
                check(saveButton && wait([&] { return saveButton->isEnabled(); }), "and Save is offered again");
            }
            // Exporting over a file the musician chose to replace: the old archive stays when the
            // export is refused, and goes only once the new one is written.
            {
                const auto file = data.getChildFile("Again.olib");
                file.replaceWithText("last week's");
                standalone::SoundAction out; out.kind = standalone::SoundAction::ExportPerformance; out.directory = file.getFullPathName().toStdString();
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a job holds the engine's thread");
                engine.soundAction(out);
                check(engine.soundMessage().find("busy") != std::string::npos && file.loadFileAsString() == "last week's", "an export refused while busy keeps the old file: " + engine.soundMessage());
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
                engine.soundAction(out);
                Control::Archive archive;
                check(wait([&] { return !engine.soundBusy(); }) && engine.archiveContents(file.getFullPathName().toStdString(), archive).empty() && !archive.items.empty(),
                      "an export over the file replaces it once written: " + engine.soundMessage());
                juce::MemoryBlock written; file.loadFileAsData(written);
                out.signKey = "no such key"; // the engine's export fails
                engine.soundAction(out);
                check(wait([&] { return !engine.soundBusy(); }) && !engine.soundMessage().empty() && engine.soundMessage().find("exported") == std::string::npos && [&] { juce::MemoryBlock now; file.loadFileAsData(now); return now == written; }(),
                      "a failed export keeps the archive it would have replaced: " + engine.soundMessage());
                check(data.findChildFiles(juce::File::findDirectories, false, ".oscairn-export*").isEmpty(), "and leaves nothing beside it");
            }
            // Quitting right after an export over a file: written within the quit's wait, it
            // replaces the file rather than stay in the hidden folder beside it.
            {
                const auto file = data.getChildFile("Quit.olib");
                file.replaceWithText("last week's");
                {
                    standalone::Engine quitting(argv[1], tapPath);
                    wait([&] { return !quitting.jobRunning(); });
                    standalone::SoundAction out; out.kind = standalone::SoundAction::ExportPerformance; out.directory = file.getFullPathName().toStdString();
                    quitting.soundAction(std::move(out));
                    check(quitting.jobRunning(), "the export runs as the window quits");
                }
                Control::Archive archive;
                check(file.loadFileAsString() != "last week's" && wait([&] { return engine.controlReady(); }) && engine.archiveContents(file.getFullPathName().toStdString(), archive).empty() && !archive.items.empty(),
                      "an export done within the quit's wait replaces the file");
                check(data.findChildFiles(juce::File::findDirectories, false, ".oscairn-export*").isEmpty(), "and leaves nothing beside it on quit");
            }
            // A view rebuilt while a job runs asks whether each plugin has a window: one not yet
            // read answers no at once, rather than wait on the stalled socket.
            {
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a job holds the engine's thread");
                tap.reset();
                const auto began = std::chrono::steady_clock::now();
                const bool window = engine.hasPluginWindow(987654);
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
                check(!window && tap.count("window.info") == 0 && waited < 100 && engine.connected(), "a plugin window not yet read is not asked for while busy: " + std::to_string(waited) + " ms");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
            }
            // The library's edits, its archives and making a key wait up to 30 s on a call: while a
            // job holds the engine they are refused at once, saying it is busy.
            {
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a job holds the engine's thread");
                tap.reset();
                const auto began = std::chrono::steady_clock::now();
                std::string copied; Control::Archive archive; Control::SigningKey key;
                const std::vector<std::string> said{engine.editLibraryItem("perf-x", {}), engine.copyLibraryItem("perf-x", copied), engine.deleteLibraryItem("perf-x"),
                                                    engine.uninstallLibrary("lib-x"), engine.archiveContents("/nothing.olib", archive), engine.makeSigningKey("busy", key)};
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
                check(std::all_of(said.begin(), said.end(), [](const std::string& e) { return e.find("busy") != std::string::npos; }), "each is refused as busy: " + said[0]);
                int asked = 0;
                for(const char* m : {"library.edit", "library.copy", "library.delete", "library.uninstall", "library.contents", "keys.make"}) asked += tap.count(m);
                check(asked == 0 && waited < 100 && engine.connected(), "and none waits on the engine: " + std::to_string(asked) + " calls, " + std::to_string(waited) + " ms");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
            }
            // While a job holds the engine's thread it cannot answer a launch ask either: the
            // shell admits at once, rather than freeze for the call's timeout and drop.
            {
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare of a missing folder holds the engine's thread");
                const auto began = std::chrono::steady_clock::now();
                const bool opened = standalone::askToLaunch(engine, {"oscairn.library", 120ull << 20}, "Library", {}, {});
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
                check(opened && engine.connected(), "an app opens at once while a job runs");
                check(waited < 900, "the ask did not wait on the stalled socket: " + std::to_string(waited) + " ms");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
                engine.appClosed("oscairn.library");
            }
            // An editor asks for every value of a stepped parameter on every rebuild: the texts
            // are cached per value, so they answer while the next refresh round trips.
            {
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                const auto node = engine.graph().names.at(synth);
                check(synth != 0, "the Lead's Basic Synth node");
                check(wait([&] { return !engine.soundBusy(); }), "the engine is free to read texts");
                // Asked without waiting: each shows once its reply has come.
                std::string saw, detuned, square;
                wait([&] { saw = engine.paramText(node, 4, 0); detuned = engine.paramText(node, 4, 1); square = engine.paramText(node, 4, 2); return saw == "Saw" && detuned == "Detuned saws" && square == "Square"; });
                check(saw == "Saw" && detuned == "Detuned saws" && square == "Square", "the oscillator names read, every value kept");
                wait([&] { return !engine.jobRunning(); }); // a refresh held too would time out: the job held is the one asked
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a job runs while the editor rebuilds");
                check(engine.paramText(node, 4, 0) == saw && engine.paramText(node, 4, 1) == detuned && engine.paramText(node, 4, 2) == square,
                      "every value's text answers from the cache while the engine works");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the job finished");
                // A continuous knob dragged asks for a new value's text on every move: a
                // parameter keeps a few hundred, then starts again, so the first is asked anew.
                // One out at a time, the latest value asked next: each value asked once its turn comes.
                tap.reset();
                for(int i = 1; i <= 300; ++i) { engine.paramText(node, 4, i / 1000.0); wait([&] { return tap.count("params.text") >= i; }); }
                settle();
                tap.reset();
                engine.paramText(node, 4, 0);
                check(wait([&] { return tap.count("params.text") >= 1; }) && tap.count("params.text") == 1, "a parameter's texts are capped, the oldest asked for again: " + std::to_string(tap.count("params.text")));
            }
            // A setlist marks entries "not in the library" once the mixed Sound+Performance read
            // of 1000 items passes them: entries are Performances, and the read asks for that
            // kind alone.
            {
                std::string lastId;
                engine.edit([&](Control& c) { lastId = c.savePerformanceToLibrary("Z last", true); });
                check(wait([&] { return !engine.soundBusy() && !lastId.empty(); }), "a Performance saved as the library's Z last");
                const auto lib = juce::File(::getenv("OSCAIRN_DATA_HOME"));
                const auto writeItem = [&](const std::string& kind, const std::string& folder, const std::string& id, const std::string& name) {
                    const auto at = lib.getChildFile(kind).getChildFile(folder);
                    at.createDirectory();
                    at.getChildFile(std::string(kind == "Sounds" ? "sound" : "performance") + ".json").replaceWithText(
                        "{\"format\":\"oscairn." + std::string(kind == "Sounds" ? "sound" : "performance") + "\",\"version\":" + std::string(kind == "Sounds" ? "8" : "13") +
                        ",\"info\":{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"description\":\"\",\"tags\":[],\"color\":\"\",\"revision\":\"r\",\"parent\":\"\",\"hash\":\"h\",\"origin\":null}}");
                };
                for(int i = 0; i < 999; ++i) writeItem("Sounds", "Sound " + std::to_string(i), "sound-" + std::to_string(i), "Sound " + std::to_string(i));
                writeItem("Performances", "Z dummy.oscairn", "perf-dummy", "Z dummy"); // sorted just before Z last: past the 1000-item mixed read
                engine.edit([](Control& c) { c.libraryRescan(); });
                check(wait([&] { const auto all = engine.library({}); return all && all->total >= 1003; }), "the library read holds 1001 staged items");
                standalone::Actions actions;
                standalone::SetlistPane pane(engine, actions, data.getChildFile("Setlists-staged"));
                const std::string setlistName = "Staged";
                standalone::Setlist staged; staged.name = setlistName; staged.entries = {{lastId, "Z last", {}}, {"perf-gone", "Gone", {}}};
                standalone::writeSetlist(data.getChildFile("Setlists-staged"), staged);
                pane.open(standalone::setlistFile(data.getChildFile("Setlists-staged"), setlistName));
                pane.go(0);
                check(pane.current() == 0 && !pane.message().contains("no longer in the library"), "the last Performance opens: " + pane.message().toStdString());
                check(wait([&] { return !engine.soundBusy(); }), "it finished opening");
                pane.go(1);
                check(pane.current() == 0 && pane.message().contains("no longer in the library"), "a gone entry is still marked: " + pane.message().toStdString());
                check(wait([&] { return !engine.soundBusy(); }), "the staged setlist is done opening");
            }
            // A reconnect to an engine still running (a timeout, another window) keeps its Capture
            // buffer: Capture's length goes again only to an engine without it, a fresh one.
            {
                auto reconnect = [&] {
                    tap.drop();
                    engine.edit([](Control& c) { c.history(); }); // the next call finds the connection gone
                    const bool dropped = wait([&] { return !engine.connected(); });
                    return dropped && wait([&] { return engine.connected(); });
                };
                auto retro = [&] { double seconds = -1; wait([&] { return !engine.jobRunning(); }); engine.edit([&](Control& c) { seconds = c.recordRetroSeconds(); }); return seconds; };
                check(wait([&] { return !engine.jobRunning(); }) && engine.setCapture(4).empty() && retro() == 4, "Capture keeps 4 seconds");
                tap.reset();
                check(reconnect(), "the client reconnects after a drop");
                check(tap.count("record.retro") == 0 && retro() == 4, "the running engine's buffer is kept, not started again: " + std::to_string(tap.count("record.retro")));
                engine.edit([](Control& c) { c.recordRetro(0); }); // as a fresh engine has it
                tap.reset();
                check(reconnect(), "the client reconnects again");
                check(tap.count("record.retro") == 1 && retro() == 4, "an engine without Capture gets its length again: " + std::to_string(retro()));
                check(engine.setCapture(0).empty(), "Capture off again");
            }
            // A crash ends the desktop's every control with it; Home and the Performance app now
            // show the Panel's notice (and Restart where a restart is possible), and a setlist
            // entry does not pretend to be playing while the engine is gone.
            {
                // Stall the server from this thread: the engine's next periodic refresh cannot
                // finish its handshake, and its timeout marks the engine disconnected.
                engine.edit([](Control& c) { c.addMacro("Drop probe"); }); // an undo name to read
                check(wait([&] { return !engine.meters().empty() && !engine.history().undo.empty() && engine.performanceInfo(); }), "the meters, undo names and Performance read before the drop: " + std::to_string(engine.meters().size()) + " " + engine.history().undo);
                forcedHold = true;
                check(wait([&] { return engine.jobRunning(); }), "the next refresh starts against the stalled server");
                check(wait([&] { return !engine.connected(); }), "its handshake times out; the engine reads as disconnected");
                standalone::SoundAction save; save.kind = standalone::SoundAction::SaveToLibrary;
                engine.soundAction(std::move(save));
                check(engine.refusal() == "The engine is not running." && engine.soundMessage() == engine.refusal(), "a Save while disconnected says the engine is not running: " + engine.soundMessage());
                check(engine.meters().empty() && engine.wireMeters().empty() && engine.history().undo.empty() && !engine.performanceInfo() && engine.takes().empty(),
                      "nothing read before the drop shows while disconnected");
                const auto folder = data.getChildFile("Setlists-staged");
                standalone::Actions actions;
                standalone::SetlistPane pane(engine, actions, folder);
                check(pane.setlist().entries.size() == 2 && pane.current() == -1, "the staged setlist is open with nothing playing");
                pane.go(0);
                check(pane.current() == -1 && pane.message().contains("not answering"), "a setlist entry says it did not open: " + pane.message().toStdString());
                pane.step(1);
                check(pane.current() == -1, "and stepping does not move on from an entry that never opened");
                standalone::Launcher launcher(engine, {}, {});
                standalone::setDesktopMode(launcher, 1); launcher.setSize(900, 600); launcher.resized();
                launcher.recovery().update();
                check(launcher.recovery().isVisible() && launcher.recovery().notice().contains("Reconnecting"), "Home shows the disconnect notice");
                check(!launcher.recovery().restartShown(), "no Restart while the engine may come back (this engine is not managed)");
                // A device's Panel shows the same notice.
                {
                    standalone::Panel panel(engine, standalone::Layout::parse(R"({"name": "Notice test", "size": [1024, 600], "controls": [{"type": "screen", "rect": [0, 0, 1024, 600]}]})"));
                    panel.setSize(1024, 600);
                    standalone::EngineRecovery* strip = nullptr;
                    check(wait([&] { for(auto* c : panel.getChildren()) if(auto* r = dynamic_cast<standalone::EngineRecovery*>(c)) strip = r; return strip && strip->isVisible(); }) && strip->notice().contains("Reconnecting"),
                          "a Panel shows the engine's notice with EngineRecovery");
                }
                // As after a managed engine's crash: Restart is a real button, in front of the
                // view's own controls at its place, on Home and in the Performance app.
                standalone::DesktopShell shell(engine, argv[2], standalone::InputMode::Mouse);
                shell.setSize(1280, 800); shell.setVisible(true); launcher.setVisible(true);
                for(auto* recovery : {&launcher.recovery(), &shell.recovery()})
                {
                    auto* view = recovery == &shell.recovery() ? static_cast<juce::Component*>(&shell) : &launcher;
                    recovery->show(true, true);
                    bool restarted = false;
                    recovery->onRestart = [&] { restarted = true; };
                    auto& button = recovery->restartButton();
                    auto* hit = view->getComponentAt(view->getLocalPoint(&button, button.getLocalBounds().getCentre()));
                    check(recovery->isVisible() && button.isVisible() && hit == &button,
                          "Restart is what a click at its place reaches: " + std::string(hit ? typeid(*hit).name() : "nothing"));
                    check(button.getWantsKeyboardFocus() && button.isAccessible(), "and it is reachable by keyboard and accessibility: " + std::to_string(button.getWantsKeyboardFocus()));
                    button.triggerClick();
                    check(wait([&] { return restarted; }), "a click on it restarts the engine");
                    recovery->onRestart = {};
                }
                forcedHold = false;
                for(int i = 0; i < 600 && !engine.connected(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); // the engine answers again
                check(engine.connected(), "the client reconnects");
                launcher.recovery().update();
                check(!launcher.recovery().isVisible(), "the notice goes when the engine is back");
            }
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            configuration.deleteRecursively();
        }
        running = false;
        control.join();
    }
    auto wait = [](auto done) { for(int i = 0; i < 1000 && !done(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); return done(); };
    // A managed engine restarted: Restart starts it off the message thread, which goes on
    // drawing (and says so) while the engine comes up. The engine here is a session served by
    // this test where the managed engine's socket is, in a runtime folder of the test's own;
    // the program started is a stand-in that only waits.
    {
        const std::string runtime = "/tmp/oscairn-gaps-run-" + std::to_string(::getpid());
        std::filesystem::remove_all(runtime);
        check(::mkdir(runtime.c_str(), 0700) == 0, "a private runtime folder");
        ::setenv("OSCAIRN_RUNTIME_DIR", runtime.c_str(), 1);
        const auto helper = runtime + "/stand-in";
        juce::File(helper).replaceWithText("#!/bin/sh\nsleep 3\n", false, false, "\n");
        ::chmod(helper.c_str(), 0700);
        const auto path = host::EngineProcess(helper, argv[1]).socketPath();
        auto served = std::make_unique<Served>(path);
        {
            standalone::Engine managed(argv[1], {}, helper);
            check(managed.connected(), "the managed engine is reached");
            served.reset(); // as the engine stops
            check(wait([&] { return managed.canRestart(); }), "a stopped managed engine offers Restart");
            standalone::EngineRecovery strip(managed);
            const auto began = std::chrono::steady_clock::now();
            strip.restartButton().onClick(); // as a click, at once
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
            check(waited < 1000 && strip.isVisible() && strip.notice().contains("Starting") && !strip.restartShown(),
                  "Restart does not hold the message thread, and says the engine is starting: " + std::to_string(waited) + " ms, " + strip.notice().toStdString());
            served = std::make_unique<Served>(path); // the engine comes up
            check(wait([&] { return managed.connected(); }), "the restarted engine is used once it answers");
            strip.update();
            check(!strip.isVisible(), "and the notice goes");
        }
        served.reset();
        std::filesystem::remove_all(runtime);
        ::unsetenv("OSCAIRN_RUNTIME_DIR");
    }
    // A plugin's presets still being listed (the engine answers at once and lists them meanwhile):
    // the picker says so, and a later refresh shows the presets and drops the message.
    {
        const auto path = socket.string() + "-presets", tapped = path + "-tap";
        Served served(path, [&](Session& s) {
            const auto synth = s.add("synth", (std::filesystem::path(argv[1]) / "oscairn-basic-synth.clap").string());
            s.connectNotes(graphInput, synth); s.connectAudio(synth, graphOutput); s.commit();
        });
        SocketTap tap(tapped, path);
        tap.refuse("presets.list", "Presets are being listed; ask again");
        {
            standalone::Engine engine(argv[1], tapped);
            check(wait([&] { return engine.soundPicker().presetProblem.find("being listed") != std::string::npos && engine.soundMessage().find("being listed") != std::string::npos; }),
                  "the picker says the presets are being listed: " + engine.soundPicker().presetProblem + " / " + engine.soundMessage());
            check(engine.connected(), "and the engine is still connected");
            tap.refuse("presets.list", {});
            check(wait([&] { return engine.soundPicker().presetProblem.find("being listed") == std::string::npos && engine.soundMessage().find("being listed") == std::string::npos; }),
                  "a later refresh lists them and the message goes: " + engine.soundPicker().presetProblem + " / " + engine.soundMessage());
        }
        std::filesystem::remove(path);
    }
    std::filesystem::remove(socket);
    data.deleteRecursively();
    if(!failures) std::puts("ok: the desktop's controls for what the control API had first");
    return failures ? 1 : 0;
}

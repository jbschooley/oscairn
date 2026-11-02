// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Profile.h"
#include "ProfileData.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <thread>

namespace oscairn::standalone
{
    namespace
    {
        constexpr int maxProfileBytes = 64 * 1024;
        void require(bool ok, const std::string& why) { if(!ok) throw std::invalid_argument("profile: " + why); }
        const juce::DynamicObject& object(const juce::var& v, const std::string& what)
        {
            const auto* o = v.getDynamicObject(); require(o != nullptr, what + " must be an object"); return *o;
        }
        void fields(const juce::DynamicObject& o, const juce::StringArray& allowed, const std::string& what)
        {
            for(const auto& p : o.getProperties()) require(allowed.contains(p.name.toString()), "unknown field " + p.name.toString().toStdString() + " in " + what);
        }
        std::string text(const juce::DynamicObject& o, const char* key, bool required = true)
        {
            const auto v = o.getProperty(key);
            if(v.isVoid() && !required) return {};
            require(v.isString() && v.toString().length() <= 256 && !v.toString().containsChar(0), std::string(key) + " must be a short string");
            return v.toString().toStdString();
        }
        int count(const juce::DynamicObject& o, const char* key, int high)
        {
            const auto v = o.getProperty(key);
            if(v.isVoid()) return 0;
            require(v.isInt() && int(v) >= 0 && int(v) <= high, std::string(key) + " must be a whole number from 0 to " + std::to_string(high));
            return int(v);
        }
        bool flag(const juce::DynamicObject& o, const char* key)
        {
            const auto v = o.getProperty(key);
            if(v.isVoid()) return false;
            require(v.isBool(), std::string(key) + " must be true or false");
            return bool(v);
        }
        std::vector<std::string> names(const juce::DynamicObject& o, const char* key)
        {
            std::vector<std::string> out;
            const auto v = o.getProperty(key);
            if(v.isVoid()) return out;
            require(v.isArray() && v.size() <= 64, std::string(key) + " must be a list");
            for(const auto& e : *v.getArray()) { require(e.isString() && e.toString().isNotEmpty() && e.toString().length() <= 128, std::string(key) + " holds names"); out.push_back(e.toString().toStdString()); }
            return out;
        }
        const juce::StringArray controlKinds{"keys", "pads", "encoders", "knobs", "faders", "buttons", "pedal", "footswitches", "expression", "jog", "touchstrip", "wheels"};
        const juce::StringArray controlRoles{"notes", "screen", "assign", "fixed"};
        const juce::StringArray banks{"assign", "filter", "ampEG", "eq", "levels", "effects"};
        const juce::StringArray surfaces{"desktop", "panel", "compact"};
        const juce::StringArray reliabilities{"desktop", "instrument", "pass-through"};
        const juce::StringArray sections{"platform", "compute", "audio", "controls", "screens", "midi", "cv", "network", "power"};
        const juce::StringArray standardControllers{"pitchBend", "modulation", "breath", "foot", "expression", "sustain", "portamento", "sostenuto", "soft"};
        const juce::StringArray encodingNames{"absolute", "twosComplement", "binaryOffset", "signMagnitude"};
        std::vector<int> numbers(const juce::DynamicObject& o, const char* key, int high)
        {
            std::vector<int> out;
            const auto v = o.getProperty(key);
            if(v.isVoid()) return out;
            require(v.isArray() && v.size() <= 1024, std::string(key) + " must be a list");
            for(const auto& e : *v.getArray()) { require(e.isInt() && int(e) >= 0 && int(e) <= high, std::string(key) + " holds numbers from 0 to " + std::to_string(high)); out.push_back(int(e)); }
            return out;
        }
    }

    bool Profile::offers(const std::string& app) const { return std::find(apps.begin(), apps.end(), app) != apps.end(); }

    // shortcut: RFC 0025's illustrative figures, not measured; measure each app on each profile and read them from manifests (step 8).
    int Profile::appMemory(const std::string& app)
    {
        static const std::map<std::string, int> own{{"oscairn.performance", 150}, {"oscairn.library", 120}, {"oscairn.setlist", 60}, {"oscairn.groovebox", 200}};
        const auto it = own.find(app);
        return it == own.end() ? 0 : it->second;
    }

    Profile Profile::parse(const std::string& json)
    {
        require(json.size() <= maxProfileBytes, "larger than 64 KiB");
        juce::var value;
        require(juce::JSON::parse(juce::String::fromUTF8(json.data(), int(json.size())), value).wasOk(), "not valid JSON");
        const auto& root = object(value, "a profile");
        fields(root, {"format", "version", "id", "name", "platform", "compute", "audio", "controls", "screens", "midi", "cv", "network", "power", "reliability", "apps", "alwaysAvailable", "estimated"}, "the profile");
        require(text(root, "format") == "oscairn.profile", "format must be oscairn.profile");
        require(root.getProperty("version").isInt() && int(root.getProperty("version")) == 2, "version must be 2");
        Profile p;
        // A controller's profile (it names ports) holds its controls and ports only.
        if(const auto m = root.getProperty("midi"); m.getDynamicObject() && m.getDynamicObject()->hasProperty("ports"))
            for(const auto* key : {"platform", "compute", "audio", "screens", "cv", "network", "power", "reliability", "apps", "alwaysAvailable"})
                require(!root.hasProperty(key), std::string("a controller's profile has no ") + key);
        const bool controller = root.getProperty("midi").getDynamicObject() && root.getProperty("midi").getDynamicObject()->hasProperty("ports");
        p.id = text(root, "id"); p.name = text(root, "name");
        require(!p.id.empty() && std::all_of(p.id.begin(), p.id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'; }), "id holds letters, digits, dots and hyphens");
        if(const auto v = root.getProperty("platform"); !v.isVoid())
        {
            const auto& o = object(v, "platform"); fields(o, {"os", "cpu", "runtime", "board"}, "platform");
            p.platform = {text(o, "os"), text(o, "cpu"), text(o, "runtime", false), text(o, "board", false)};
        }
        if(const auto v = root.getProperty("compute"); !v.isVoid())
        {
            const auto& o = object(v, "compute"); fields(o, {"cores", "realtimeCores", "memory", "storage", "osMemory", "performanceMemory"}, "compute");
            p.compute = {count(o, "cores", 1024), count(o, "realtimeCores", 1024), count(o, "memory", 1 << 24), count(o, "storage", 1 << 30),
                         count(o, "osMemory", 1 << 24), count(o, "performanceMemory", 1 << 24)};
            require(p.compute.realtimeCores <= p.compute.cores, "more realtime cores than cores");
            require(o.hasProperty("osMemory") == o.hasProperty("performanceMemory"), "osMemory and performanceMemory come together");
            require(p.compute.osMemory || !o.hasProperty("osMemory"), "osMemory is more than 0");
        }
        if(const auto v = root.getProperty("audio"); !v.isVoid())
        {
            const auto& o = object(v, "audio"); fields(o, {"inputs", "outputs", "rates", "block", "headphones"}, "audio");
            p.audio.inputs = count(o, "inputs", 256); p.audio.outputs = count(o, "outputs", 256);
            p.audio.block = count(o, "block", 8192); p.audio.headphones = flag(o, "headphones");
            if(p.audio.block) require(p.audio.block >= 16, "block is at least 16 frames");
            if(const auto r = o.getProperty("rates"); !r.isVoid())
            {
                require(r.isArray(), "rates must be a list");
                for(const auto& e : *r.getArray()) { require(e.isInt() && int(e) >= 8000 && int(e) <= 384000, "rates are sample rates in Hz"); p.audio.rates.push_back(int(e)); }
            }
        }
        if(const auto v = root.getProperty("controls"); !v.isVoid())
        {
            require(v.isArray() && v.size() <= 64, "controls must be a list");
            for(const auto& e : *v.getArray())
            {
                const auto& o = object(e, "a control"); fields(o, {"kind", "count", "sensing", "role", "rows", "paging", "modes", "functions", "target", "midi"}, "a control");
                Control c{text(o, "kind"), count(o, "count", 1024), text(o, "sensing", false), text(o, "role")};
                require(controlKinds.contains(juce::String(c.kind)), "unknown control kind " + c.kind);
                require(c.count > 0, "a control's count is at least 1");
                require(controlRoles.contains(juce::String(c.role)), "a control's role is notes, screen, assign or fixed");
                require((c.role == "notes") == (c.kind == "keys" || c.kind == "pads"), "keys and pads, and only they, play notes");
                if(const auto r = o.getProperty("rows"); !r.isVoid())
                {
                    require(r.isArray() && r.size() > 0 && r.size() <= 16, "rows must be a list of 1 to 16");
                    for(const auto& row : *r.getArray())
                    {
                        const auto& ro = object(row, "a row"); fields(ro, {"name", "bank"}, "a row");
                        c.rows.push_back({text(ro, "name"), text(ro, "bank")});
                        require(banks.contains(juce::String(c.rows.back().bank)), "a row's bank is assign, filter, ampEG, eq, levels or effects");
                    }
                }
                c.paging = text(o, "paging", false);
                require(c.paging.empty() || c.paging == "button" || c.paging == "screen", "paging is button or screen");
                c.modes = names(o, "modes");
                for(const auto& m : c.modes) require(m == "part" || m == "element" || m == "audio", "a fader mode is part, element or audio");
                c.functions = names(o, "functions");
                c.target = text(o, "target", false);
                const bool knobs = c.kind == "knobs" || c.kind == "encoders", levers = c.kind == "faders" || c.kind == "buttons";
                const bool assign = c.role == "assign";
                // Knobs play banks by rows or pages; other kinds of assign group may page banks, pass a function through, or wait to be assigned.
                require(!assign || !knobs || c.rows.empty() != c.paging.empty(), "an assign group of " + c.kind + " has rows or paging, not both");
                require((assign && !levers) || (c.rows.empty() && c.paging.empty()), "only an assign group of knobs, wheels or pedals has rows or paging");
                require(c.rows.empty() || c.paging.empty(), "a group has rows or paging, not both");
                require(c.modes.empty() || c.kind == "faders", "only faders have modes");
                if(c.role == "fixed") require(c.functions.empty() || int(c.functions.size()) == c.count, "a fixed group's functions are one per control");
                else if(!c.functions.empty())
                {
                    require(assign && !knobs && !levers && c.rows.empty() && c.paging.empty() && int(c.functions.size()) == c.count,
                            "an assign group of wheels, pedals, a ribbon, footswitches or a jog has a function per control, and no rows or paging");
                    for(const auto& f : c.functions) require(standardControllers.contains(juce::String(f)), "an assign group's function is a standard controller: " + f);
                }
                if(!c.target.empty())
                    require(assign && ((knobs && c.target == "banks") || (c.kind == "faders" && c.target == "partLevels") || (c.kind == "buttons" && (c.target == "partMutes" || c.target == "scenes"))),
                            "target is banks for knobs, partLevels for faders, partMutes or scenes for buttons, in an assign group");
                if(const auto m = o.getProperty("midi"); !m.isVoid())
                {
                    require(controller, "only a controller's controls say how they talk MIDI");
                    const auto& mo = object(m, "a control's midi"); fields(mo, {"port", "channel", "cc", "notes", "pitchBend", "encoding", "button", "feedback"}, "a control's midi");
                    auto& midi = c.midi;
                    midi.port = mo.hasProperty("port") ? count(mo, "port", 16) : 1;
                    if(mo.getProperty("channel").isArray())
                    {
                        midi.channels = numbers(mo, "channel", 16);
                        require(int(midi.channels.size()) == c.count, "channel is one, or one per control");
                        midi.channel = midi.channels.front();
                    }
                    else midi.channel = count(mo, "channel", 16);
                    require(midi.channel >= 1 && std::all_of(midi.channels.begin(), midi.channels.end(), [](int n) { return n >= 1; }), "a control's MIDI channel is 1 to 16");
                    midi.cc = numbers(mo, "cc", 127); midi.notes = numbers(mo, "notes", 127); midi.pitchBend = flag(mo, "pitchBend");
                    const int kinds = int(!midi.cc.empty()) + int(!midi.notes.empty()) + int(midi.pitchBend);
                    require(kinds == 1 || (kinds == 0 && c.kind == "keys"), "a control's midi has one of cc, notes or pitchBend (keys only a channel)");
                    const auto perRow = c.count * int(std::max<size_t>(1, c.rows.size()));
                    require(midi.cc.empty() || int(midi.cc.size()) == c.count || (!c.rows.empty() && int(midi.cc.size()) == perRow), "cc has a number per control, or per control per row");
                    require(midi.notes.empty() || int(midi.notes.size()) == c.count, "notes has a number per control");
                    require(!midi.pitchBend || (c.kind == "wheels" && c.count == 1), "pitchBend is a group of one wheel");
                    if(const auto encoding = mo.getProperty("encoding"); !encoding.isVoid())
                    {
                        require(!midi.cc.empty(), "only CCs have an encoding");
                        // A name, or {increment, decrement}: the values for one step up and one down.
                        auto one = [&](const juce::var& e) {
                            if(e.isString())
                            {
                                require(encodingNames.contains(e.toString()), "an encoding is absolute, twosComplement, binaryOffset, signMagnitude or {increment, decrement}");
                                midi.encodings.push_back(e.toString().toStdString()); midi.steps.push_back({0, 0}); return;
                            }
                            const auto& so = object(e, "an encoding"); fields(so, {"increment", "decrement"}, "an encoding");
                            require(so.hasProperty("increment") && so.hasProperty("decrement"), "an encoding's steps have an increment and a decrement");
                            const int up = count(so, "increment", 127), down = count(so, "decrement", 127);
                            require(up != down, "an encoding's increment and decrement differ");
                            midi.encodings.push_back("steps"); midi.steps.push_back({up, down});
                        };
                        if(encoding.isArray()) { require(encoding.size() <= 1024, "encoding must be a list"); for(const auto& e : *encoding.getArray()) one(e); }
                        else for(int k = 0; k < c.count; ++k) one(encoding);
                        require(int(midi.encodings.size()) == c.count, "encoding is one, or one per control");
                    }
                    if(mo.hasProperty("button")) { midi.button = text(mo, "button"); require(c.kind == "buttons" && (midi.button == "momentary" || midi.button == "latch"), "a button is momentary or latch"); }
                    midi.feedback = flag(mo, "feedback");
                    require(!midi.feedback || (!midi.cc.empty() && std::all_of(midi.encodings.begin(), midi.encodings.end(), [](const std::string& e) { return e == "absolute"; })),
                            "feedback is for controls sending absolute CCs");
                }
                p.controls.push_back(c);
            }
        }
        if(const auto v = root.getProperty("screens"); !v.isVoid())
        {
            require(v.isArray() && v.size() <= 8, "screens must be a list");
            for(const auto& e : *v.getArray())
            {
                const auto& o = object(e, "a screen"); fields(o, {"width", "height", "touch", "surface", "layout"}, "a screen");
                Screen s{count(o, "width", 16384), count(o, "height", 16384), flag(o, "touch"), text(o, "surface"), text(o, "layout")};
                require(s.width > 0 && s.height > 0, "a screen has a size");
                require(surfaces.contains(juce::String(s.surface)), "surface is desktop, panel or compact");
                p.screens.push_back(s);
            }
        }
        if(const auto v = root.getProperty("midi"); !v.isVoid())
        {
            const auto& o = object(v, "midi"); fields(o, controller ? juce::StringArray{"ports"} : juce::StringArray{"din", "usbDevice", "usbHost"}, controller ? "a controller's midi" : "midi");
            p.midi = {count(o, "din", 64), flag(o, "usbDevice"), flag(o, "usbHost"), {}};
            if(controller)
            {
                const auto ports = o.getProperty("ports");
                require(ports.isArray() && ports.size() >= 1 && ports.size() <= 16, "ports must be a list of 1 to 16");
                for(const auto& e : *ports.getArray())
                {
                    const auto& po = object(e, "a port"); fields(po, {"name", "index", "protocol", "ignore", "display"}, "a port");
                    Port port{text(po, "name"), po.hasProperty("index") ? count(po, "index", 64) : 1, po.hasProperty("protocol") ? text(po, "protocol") : std::string("midi"), flag(po, "ignore"), flag(po, "display")};
                    require(!port.name.empty() && port.index >= 1, "a port has a name and an index from 1");
                    require(port.protocol == "midi" || port.protocol == "mackie", "a port's protocol is midi or mackie");
                    require(!port.ignore || !po.hasProperty("protocol"), "an ignored port has no protocol");
                    require(!port.display || port.protocol == "mackie", "only a Mackie port has a display");
                    p.midi.ports.push_back(port);
                }
            }
        }
        // Each control's MIDI on a midi port of its controller, and no message from two controls.
        std::set<std::tuple<int, int, int, int>> messages; // port, channel, kind (0 cc, 1 note, 2 pitch bend), number
        for(const auto& c : p.controls)
        {
            if(!c.midi.channel) continue;
            require(c.midi.port >= 1 && c.midi.port <= int(p.midi.ports.size()) && p.midi.ports[size_t(c.midi.port - 1)].protocol == "midi" && !p.midi.ports[size_t(c.midi.port - 1)].ignore,
                    "a control's port is one of the controller's midi ports, not an ignored one");
            auto add = [&](int kind, int number, size_t k) { require(messages.insert({c.midi.port, c.midi.channelOf(k % size_t(c.count)), kind, number}).second, "two controls send the same message"); };
            for(size_t k = 0; k < c.midi.cc.size(); ++k) add(0, c.midi.cc[k], k);
            if(c.role != "notes") for(size_t k = 0; k < c.midi.notes.size(); ++k) add(1, c.midi.notes[k], k);
            if(c.midi.pitchBend) add(2, 0, 0);
        }
        if(const auto v = root.getProperty("cv"); !v.isVoid())
        {
            const auto& o = object(v, "cv"); fields(o, {"inputs", "outputs"}, "cv");
            p.cv = {count(o, "inputs", 256), count(o, "outputs", 256)};
        }
        if(const auto v = root.getProperty("network"); !v.isVoid())
        {
            const auto& o = object(v, "network"); fields(o, {"ethernet", "wifi"}, "network");
            p.network = {flag(o, "ethernet"), flag(o, "wifi")};
        }
        if(const auto v = root.getProperty("power"); !v.isVoid())
        {
            const auto& o = object(v, "power"); fields(o, {"battery"}, "power");
            p.battery = flag(o, "battery");
        }
        if(!controller)
        {
            p.reliability = text(root, "reliability");
            require(reliabilities.contains(juce::String(p.reliability)), "reliability is desktop, instrument or pass-through");
        }
        else p.reliability.clear();
        p.apps = names(root, "apps");
        if(root.hasProperty("alwaysAvailable"))
        {
            p.alwaysAvailable = names(root, "alwaysAvailable");
            for(const auto& a : p.alwaysAvailable) require(p.offers(a), "alwaysAvailable names an app in apps: " + a);
        }
        else for(const auto* a : {"oscairn.performance", "oscairn.library"}) if(p.offers(a)) p.alwaysAvailable.push_back(a);
        // RFC 0024: the OS, the floor and the always-available apps' reservations must fit its memory.
        if(p.memoryClasses())
        {
            int64_t need = int64_t(p.compute.osMemory) + p.compute.performanceMemory;
            for(const auto& a : p.alwaysAvailable) need += appMemory(a);
            require(need <= p.compute.memory, "osMemory, performanceMemory and the always-available apps' reservations (" + std::to_string(need) +
                                              " MiB) exceed memory (" + std::to_string(p.compute.memory) + " MiB)");
        }
        p.estimated = names(root, "estimated");
        for(const auto& s : p.estimated) require(sections.contains(juce::String(s)), "estimated names a section: " + s);
        return p;
    }

    Profile Profile::desktop()
    {
        Profile p;
        p.id = "desktop"; p.name = "This computer";
#if JUCE_MAC
        p.platform.os = "macos";
#elif JUCE_WINDOWS
        p.platform.os = "windows";
#else
        p.platform.os = "linux";
#endif
#if JUCE_ARM
        p.platform.cpu = "arm64";
#else
        p.platform.cpu = "x86_64";
#endif
        p.compute.cores = int(std::max(1u, std::thread::hardware_concurrency()));
        p.compute.memory = juce::SystemStats::getMemorySizeInMegabytes();
        // The main display, if there is one yet (a test or a headless run may have none).
        Screen s;
        if(const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            s.width = display->userArea.getWidth(); s.height = display->userArea.getHeight();
            s.touch = juce::Desktop::getInstance().getMainMouseSource().isTouch();
        }
        else { s.width = 1440; s.height = 900; }
        p.screens.push_back(s);
        p.reliability = "desktop";
        p.apps = {"oscairn.performance", "oscairn.groovebox", "oscairn.library", "oscairn.setlist", "oscairn.recordings"};
        p.alwaysAvailable = {"oscairn.performance", "oscairn.library"};
        return p;
    }

    std::vector<std::pair<std::string, std::string>> builtInProfiles()
    {
        std::vector<std::pair<std::string, std::string>> out;
        for(int i = 0; i < ProfileData::namedResourceListSize; ++i)
        {
            int size = 0;
            const auto* data = ProfileData::getNamedResource(ProfileData::namedResourceList[i], size);
            const auto file = juce::String(ProfileData::getNamedResourceOriginalFilename(ProfileData::namedResourceList[i]));
            if(data && file.endsWith(".json")) out.push_back({file.upToLastOccurrenceOf(".json", false, false).toStdString(), std::string(data, size_t(size))});
        }
        return out;
    }

    namespace
    {
        Profile readProfile(const std::string& nameOrFile)
        {
            for(const auto& [id, json] : builtInProfiles()) if(id == nameOrFile) return Profile::parse(json);
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(nameOrFile));
            require(file.existsAsFile(), "no profile named " + nameOrFile + " and no such file");
            require(file.getSize() <= maxProfileBytes, "larger than 64 KiB");
            return Profile::parse(file.loadFileAsString().toStdString());
        }
    }

    Profile loadProfile(const std::string& nameOrFile)
    {
        if(nameOrFile.empty() || nameOrFile == "desktop") return Profile::desktop();
        auto p = readProfile(nameOrFile);
        require(!p.controller(), p.id + " is a controller's profile: it joins the device's when connected");
        return p;
    }

    std::vector<Profile> controllerProfiles(const std::vector<std::string>& files)
    {
        std::vector<Profile> out;
        for(const auto& [id, json] : builtInProfiles()) if(auto p = Profile::parse(json); p.controller()) out.push_back(std::move(p));
        for(const auto& f : files)
        {
            auto p = readProfile(f);
            require(p.controller(), p.id + " is not a controller's profile (it names no ports)");
            std::erase_if(out, [&](const Profile& q) { return q.id == p.id; }); // a file replaces the built-in of its id
            out.push_back(std::move(p));
        }
        std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) { return a.id < b.id; });
        return out;
    }

    namespace
    {
        // The input a port is: the Nth whose name contains the port's, or none (inputs.size()).
        size_t inputOf(const Profile::Port& port, const std::vector<std::string>& inputs)
        {
            int seen = 0;
            for(size_t i = 0; i < inputs.size(); ++i)
                if(juce::String(inputs[i]).containsIgnoreCase(juce::String(port.name)) && ++seen == port.index) return i;
            return inputs.size();
        }
    }

    std::vector<PortMatch> matchPorts(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs, const std::string& protocol)
    {
        std::vector<PortMatch> out;
        for(size_t c = 0; c < controllers.size(); ++c)
            for(size_t k = 0; k < controllers[c].midi.ports.size(); ++k)
            {
                const auto& port = controllers[c].midi.ports[k];
                if(port.protocol != protocol || port.ignore) continue;
                // An input is one controller's port at most: the first profile to claim it.
                if(const auto i = inputOf(port, inputs); i < inputs.size() && std::none_of(out.begin(), out.end(), [&](const PortMatch& m) { return m.input == i; }))
                    out.push_back({i, c, int(k + 1)});
            }
        return out;
    }

    std::vector<std::string> ignoredInputs(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs)
    {
        std::vector<std::string> out;
        for(const auto& c : controllers)
            for(const auto& port : c.midi.ports)
                if(const auto i = inputOf(port, inputs); port.ignore && i < inputs.size()) out.push_back(inputs[i]);
        return out;
    }
}

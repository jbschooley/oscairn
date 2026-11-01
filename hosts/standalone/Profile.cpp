// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Profile.h"
#include "ProfileData.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <stdexcept>
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
        const juce::StringArray surfaces{"desktop", "panel", "compact"};
        const juce::StringArray reliabilities{"desktop", "instrument", "pass-through"};
        const juce::StringArray sections{"platform", "compute", "audio", "controls", "screens", "midi", "cv", "network", "power"};
    }

    bool Profile::offers(const std::string& app) const { return std::find(apps.begin(), apps.end(), app) != apps.end(); }

    Profile Profile::parse(const std::string& json)
    {
        require(json.size() <= maxProfileBytes, "larger than 64 KiB");
        juce::var value;
        require(juce::JSON::parse(juce::String::fromUTF8(json.data(), int(json.size())), value).wasOk(), "not valid JSON");
        const auto& root = object(value, "a profile");
        fields(root, {"format", "version", "id", "name", "platform", "compute", "audio", "controls", "screens", "midi", "cv", "network", "power", "reliability", "apps", "estimated"}, "the profile");
        require(text(root, "format") == "oscairn.profile", "format must be oscairn.profile");
        require(root.getProperty("version").isInt() && int(root.getProperty("version")) == 1, "version must be 1");
        Profile p;
        p.id = text(root, "id"); p.name = text(root, "name");
        require(!p.id.empty() && std::all_of(p.id.begin(), p.id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'; }), "id holds letters, digits, dots and hyphens");
        if(const auto v = root.getProperty("platform"); !v.isVoid())
        {
            const auto& o = object(v, "platform"); fields(o, {"os", "cpu", "runtime", "board"}, "platform");
            p.platform = {text(o, "os"), text(o, "cpu"), text(o, "runtime", false), text(o, "board", false)};
        }
        if(const auto v = root.getProperty("compute"); !v.isVoid())
        {
            const auto& o = object(v, "compute"); fields(o, {"cores", "realtimeCores", "memory", "storage"}, "compute");
            p.compute = {count(o, "cores", 1024), count(o, "realtimeCores", 1024), count(o, "memory", 1 << 24), count(o, "storage", 1 << 30)};
            require(p.compute.realtimeCores <= p.compute.cores, "more realtime cores than cores");
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
                const auto& o = object(e, "a control"); fields(o, {"kind", "count", "sensing"}, "a control");
                Control c{text(o, "kind"), count(o, "count", 1024), text(o, "sensing", false)};
                require(controlKinds.contains(juce::String(c.kind)), "unknown control kind " + c.kind);
                require(c.count > 0, "a control's count is at least 1");
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
            const auto& o = object(v, "midi"); fields(o, {"din", "usbDevice", "usbHost"}, "midi");
            p.midi = {count(o, "din", 64), flag(o, "usbDevice"), flag(o, "usbHost")};
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
        p.reliability = text(root, "reliability");
        require(reliabilities.contains(juce::String(p.reliability)), "reliability is desktop, instrument or pass-through");
        p.apps = names(root, "apps");
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
        p.apps = {"oscairn.groovebox", "oscairn.library", "oscairn.setlist"};
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

    Profile loadProfile(const std::string& nameOrFile)
    {
        if(nameOrFile.empty() || nameOrFile == "desktop") return Profile::desktop();
        for(const auto& [id, json] : builtInProfiles()) if(id == nameOrFile) return Profile::parse(json);
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(nameOrFile));
        require(file.existsAsFile(), "no profile named " + nameOrFile + " and no such file");
        require(file.getSize() <= maxProfileBytes, "larger than 64 KiB");
        return Profile::parse(file.loadFileAsString().toStdString());
    }
}

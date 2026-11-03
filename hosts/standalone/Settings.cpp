// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Settings.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
    }

    juce::String fingerprint(const std::string& id)
    {
        juce::String out;
        for(size_t i = 0; i < id.size(); i += 4) out << (i ? " " : "") << text(id.substr(i, 4));
        return out;
    }

    void askToMakeKey(Engine& engine, juce::Component* over, std::function<void(const std::string&)> made)
    {
        auto* w = new juce::AlertWindow("Make a signing key", "Its name, which only you see: letters, digits, '.', '-' and '_'. The private key stays on this device.", juce::MessageBoxIconType::NoIcon, over);
        w->addTextEditor("name", "", "Name");
        w->addButton("Make", 1, juce::KeyPress(juce::KeyPress::returnKey)); w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([&engine, w, then = std::move(made)](int result) {
            if(result != 1) return;
            Control::SigningKey key;
            if(const auto error = engine.makeSigningKey(w->getTextEditorContents("name").trim().toStdString(), key); !error.empty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "No key made", text(error));
                return;
            }
            if(then) then(key.name);
        }), true);
    }

    Settings::Settings(Engine& engine) : engine_(engine)
    {
        setTitle("Settings");
        makeKey_.setTooltip("A new key to sign exports with; its private half never leaves this device");
        makeKey_.onClick = [this] { askToMakeKey(engine_, this, [safe = juce::Component::SafePointer<Settings>(this)](const std::string&) { if(safe) safe->refresh(); }); };
        addAndMakeVisible(makeKey_);
        refresh();
    }

    void Settings::setControllers(ControllerPorts ports)
    {
        controllers_ = std::move(ports);
        refresh();
    }

    void Settings::readPorts()
    {
        ports_.clear(); boxes_.clear();
        if(!controllers_.inputs) return;
        const auto inputs = controllers_.inputs();
        const auto choices = controllers_.choices ? controllers_.choices() : ProfileChoices{};
        const auto& profiles = controllers_.profiles;
        for(size_t i = 0; i < inputs.size(); ++i)
        {
            PortRow row{inputs[i], profilesFor(profiles, inputs, i), profileOf(profiles, inputs, i, choices), {}};
            if(row.profiles.empty()) continue;
            row.all = row.profiles.size() > 1 && profilesOf(profiles, inputs, i, choices).size() == row.profiles.size();
            // What the profile in use does with this port: its port whose name the input carries.
            for(const auto& p : profiles[row.chosen].midi.ports)
                if(juce::String(inputs[i]).containsIgnoreCase(juce::String(p.name)))
                    row.note = p.ignore ? "Ignored: a duplicate of another port" : p.protocol == "mackie" ? "Mackie Control" : "Its controls join";
            auto box = std::make_unique<juce::ComboBox>("Profile for " + text(inputs[i]));
            for(size_t k = 0; k < row.profiles.size(); ++k) box->addItem(text(profiles[row.profiles[k]].name), int(k) + 1);
            // Identical controllers on one port name, each set to its own channels, a profile each.
            if(row.profiles.size() > 1) box->addItem("Each of these, by MIDI channel", int(row.profiles.size()) + 1);
            box->setSelectedId(row.all ? int(row.profiles.size()) + 1 : int(std::find(row.profiles.begin(), row.profiles.end(), row.chosen) - row.profiles.begin()) + 1, juce::dontSendNotification);
            box->setEnabled(row.profiles.size() > 1);
            box->setTooltip(row.profiles.size() > 1 ? "Several profiles fit this port: choose the one its firmware matches, or each for identical controllers on their own channels" : "The only profile that fits this port");
            box->onChange = [this, port = inputs[i], candidates = row.profiles, raw = box.get()] {
                const auto k = size_t(raw->getSelectedId() - 1);
                if(k > candidates.size() || !controllers_.choose) return;
                std::vector<std::string> ids;
                for(size_t c = 0; c < candidates.size(); ++c) if(c == k || k == candidates.size()) ids.push_back(controllers_.profiles[candidates[c]].id);
                controllers_.choose(port, ids);
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Settings>(this)] { if(safe) safe->refresh(); }); // not while its box calls
            };
            addAndMakeVisible(*box);
            ports_.push_back(std::move(row)); boxes_.push_back(std::move(box));
        }
    }

    void Settings::refresh()
    {
        keys_ = engine_.signingKeys();
        readPorts();
        resized();
        repaint();
    }

    void Settings::resized()
    {
        const int m = mode(), row = m == 2 ? 48 : 36, control = m == 2 ? 44 : 30;
        auto area = getLocalBounds().reduced(24);
        area.removeFromTop(int(textSize(TextRole::Title, m)) + 8 + int(textSize(TextRole::Small, m)) + 4 + 12); // heading and its line
        keysArea_ = area.removeFromTop(row * std::max<int>(1, int(keys_.size())));
        area.removeFromTop(12);
        makeKey_.setBounds(area.removeFromTop(control).removeFromLeft(160));
        // The controllers: a heading and its line, then a row per port.
        area.removeFromTop(28);
        portsHeading_ = area.removeFromTop(int(textSize(TextRole::Title, m)) + 8 + int(textSize(TextRole::Small, m)) + 4 + 12);
        portsArea_ = area.removeFromTop(row * std::max<int>(1, int(ports_.size())));
        for(size_t i = 0; i < boxes_.size(); ++i)
        {
            auto r = portsArea_.withHeight(row).translated(0, int(i) * row).reduced(16, 0);
            r.removeFromLeft(r.getWidth() * 2 / 5);
            const int width = std::min(300, r.getWidth() * 3 / 5);
            boxes_[i]->setBounds(r.removeFromLeft(width).withSizeKeepingCentre(width, control));
        }
    }

    void Settings::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = mode(), row = m == 2 ? 48 : 36;
        const juce::Font title(juce::FontOptions(textSize(TextRole::Title, m), juce::Font::bold)), body(juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)),
                         small(juce::FontOptions(textSize(TextRole::Small, m)));
        auto area = getLocalBounds().reduced(24);
        g.setColour(ink(Ink::Text)); drawInked(g, "Signing keys", area.removeFromTop(int(title.getHeight()) + 8), title);
        g.setColour(ink(Ink::Muted)); drawInked(g, "Exports signed with one of these show others they came from you. Each is shown by its fingerprint.", area.removeFromTop(int(small.getHeight()) + 4), small);
        drawSection(g, keysArea_, ink(Ink::Surface));
        if(keys_.empty()) { g.setColour(ink(Ink::Muted)); drawInked(g, "No keys yet", keysArea_.reduced(16, 0), small); }
        for(size_t i = 0; i < keys_.size(); ++i)
        {
            auto r = keysArea_.withHeight(row).translated(0, int(i) * row);
            if(i) { g.setColour(ink(Ink::Border).withAlpha(.35f)); g.fillRect(r.removeFromTop(1).reduced(12, 0)); }
            r = r.reduced(16, 0);
            g.setColour(ink(Ink::Text)); drawInked(g, text(keys_[i].name), r.removeFromLeft(r.getWidth() / 2), body);
            g.setColour(ink(Ink::Muted)); drawInked(g, fingerprint(keys_[i].id), r, small, juce::Justification::right);
        }
        auto heading = portsHeading_;
        g.setColour(ink(Ink::Text)); drawInked(g, "MIDI controllers", heading.removeFromTop(int(title.getHeight()) + 8), title);
        g.setColour(ink(Ink::Muted)); drawInked(g, "Each port a controller's profile fits. Where several fit, choose the one its firmware matches; it is kept for the port.", heading.removeFromTop(int(small.getHeight()) + 4), small);
        drawSection(g, portsArea_, ink(Ink::Surface));
        if(ports_.empty()) { g.setColour(ink(Ink::Muted)); drawInked(g, "No controller with a profile is connected", portsArea_.reduced(16, 0), small); }
        for(size_t i = 0; i < ports_.size(); ++i)
        {
            auto r = portsArea_.withHeight(row).translated(0, int(i) * row);
            if(i) { g.setColour(ink(Ink::Border).withAlpha(.35f)); g.fillRect(r.removeFromTop(1).reduced(12, 0)); }
            r = r.reduced(16, 0);
            g.setColour(ink(Ink::Text)); drawInked(g, text(ports_[i].port), r.removeFromLeft(r.getWidth() * 2 / 5).withTrimmedRight(12), body);
            r.removeFromLeft(std::min(300, r.getWidth() * 3 / 5) + 12); // its profile's box
            g.setColour(ports_[i].note.startsWith("Ignored") ? ink(Ink::Danger) : ink(Ink::Muted)); drawInked(g, ports_[i].note, r, small, juce::Justification::right);
        }
    }
}

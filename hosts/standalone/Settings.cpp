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

    void Settings::refresh()
    {
        keys_ = engine_.signingKeys();
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
    }
}

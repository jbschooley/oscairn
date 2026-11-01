// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Launcher.h"

namespace oscairn::standalone
{
    namespace
    {
        constexpr int barHeight = 40; // the top bar (RFC 0030)
        // The top bar's line under it and its background, shared by Home and the apps' frames.
        void drawBar(juce::Graphics& g, juce::Rectangle<int> bar)
        {
            g.setColour(ink(Ink::Background)); g.fillRect(bar);
            g.setColour(ink(Ink::Border).withAlpha(.35f)); g.fillRect(bar.removeFromBottom(1));
        }
    }

    Launcher::Launcher(Engine& engine, std::vector<App> apps) : engine_(engine), apps_(std::move(apps)), states_(apps_.size())
    {
        setTitle("Home");
        timerCallback();
        startTimer(1000);
    }

    void Launcher::timerCallback()
    {
        for(size_t i = 0; i < apps_.size(); ++i) states_[i] = apps_[i].state ? apps_[i].state() : juce::String();
        if(const auto info = engine_.performanceInfo()) context_ = juce::String::fromUTF8(info->name.empty() ? "Untitled" : info->name.c_str());
        repaint();
    }

    void Launcher::open(int app)
    {
        if(app < 0 || app >= int(apps_.size()) || !apps_[size_t(app)].open) return;
        juce::MessageManager::callAsync([open = apps_[size_t(app)].open] { open(); });
    }

    void Launcher::resized()
    {
        // Tiles in as many columns as fit at about 240 pixels, all of one size.
        const bool touch = mode() == 2 || desktopMode(*this) == 0;
        const int gap = touch ? 14 : 12, tileHeight = touch ? 150 : 132;
        auto area = getLocalBounds().withTrimmedTop(barHeight).reduced(gap * 2, gap * 2);
        const int columns = std::clamp(area.getWidth() / 240, 1, std::max(1, int(apps_.size())));
        const int width = (area.getWidth() - gap * (columns - 1)) / columns;
        tiles_.clear();
        for(size_t i = 0; i < apps_.size(); ++i)
        {
            const int column = int(i) % columns, row = int(i) / columns;
            tiles_.push_back({area.getX() + column * (width + gap), area.getY() + row * (tileHeight + gap), width, tileHeight});
        }
        repaint();
    }

    void Launcher::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = mode();
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m), title = textSize(TextRole::Title, m);
        // The top bar: where (Home), and the open Performance.
        auto bar = getLocalBounds().removeFromTop(barHeight);
        drawBar(g, bar);
        bar = bar.reduced(16, 0);
        g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(body, juce::Font::bold));
        g.drawText("Home", bar, juce::Justification::centredLeft);
        g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small));
        g.drawText(context_, bar, juce::Justification::centredRight, true);
        for(size_t i = 0; i < tiles_.size(); ++i)
        {
            const auto r = tiles_[i];
            drawSection(g, r, ink(Ink::Surface));
            auto inside = r.reduced(16);
            const int iconSide = 48;
            const auto well = inside.removeFromTop(iconSide).removeFromLeft(iconSide).toFloat();
            g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(well, std::min(12.f, themeData().radius + 4));
            drawIcon(g, apps_[i].icon, well.reduced(12), ink(Ink::Text));
            auto state = inside.removeFromBottom(int(small) + 4);
            g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(title, juce::Font::bold));
            g.drawText(apps_[i].name, inside.removeFromBottom(int(title) + 6), juce::Justification::bottomLeft, true);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small));
            g.drawText(states_[i], state, juce::Justification::centredLeft, true);
        }
    }

    void Launcher::mouseUp(const juce::MouseEvent& e)
    {
        if(e.mouseWasDraggedSinceMouseDown()) return;
        for(size_t i = 0; i < tiles_.size(); ++i) if(tiles_[i].contains(e.getPosition())) { open(int(i)); return; }
    }

    AppFrame::AppFrame(const juce::String& name, juce::Component& app, bool home, std::function<void()> goHome) : name_(name), app_(app)
    {
        home_.setTooltip("Home");
        home_.onClick = [goHome] { if(goHome) juce::MessageManager::callAsync(goHome); }; // going Home may replace this frame
        addChildComponent(home_);
        home_.setVisible(home);
        addAndMakeVisible(app_);
    }

    void AppFrame::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        auto bar = getLocalBounds().removeFromTop(barHeight);
        drawBar(g, bar);
        bar.removeFromLeft(home_.isVisible() ? home_.getRight() + 4 : 16);
        g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this))), juce::Font::bold));
        g.drawText(name_, bar, juce::Justification::centredLeft);
    }

    void AppFrame::resized()
    {
        home_.setBounds(6, 3, 36, barHeight - 6);
        app_.setBounds(getLocalBounds().withTrimmedTop(barHeight));
    }
}

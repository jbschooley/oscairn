// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Launcher.h"

namespace oscairn::standalone
{
    namespace
    {
        // Opening an app may replace the component whose button asked: after the click is done.
        std::function<void()> later(std::function<void()> f) { return f ? [f] { juce::MessageManager::callAsync(f); } : std::function<void()>{}; }
    }

    TopBar::TopBar(Engine& engine, const juce::String& where, Shell shell) : engine_(engine), where_(where), shell_(std::move(shell))
    {
        // The bar's icons stand alone, without a button's fill; All notes off in the danger color.
        for(auto* b : std::initializer_list<IconButton*>{&home_, &back_, &apps_, &more_, &notesOff_}) b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        auto wire = [this](IconButton& b, const std::function<void()>& f, const char* tip) {
            b.setTooltip(tip); b.onClick = later(f); addChildComponent(b); b.setVisible(f != nullptr);
        };
        wire(home_, shell_.home, "Home: the apps");
        wire(back_, shell_.back, "Back");
        wire(apps_, shell_.apps, "The running apps");
        more_.setTooltip("Everything else this app does");
        more_.onClick = [this] {
            juce::PopupMenu menu;
            for(size_t i = 3; i < actions_.size(); ++i) menu.addItem(int(i) + 1, actions_[i].name);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more_), [actions = actions_](int chosen) { if(chosen > 0 && actions[size_t(chosen - 1)].run) actions[size_t(chosen - 1)].run(); }); // as shown: the bar may change or go meanwhile
        };
        addChildComponent(more_);
        notesOff_.setTooltip("All notes off: hold for half a second");
        notesOff_.getProperties().set("danger", true);
        notesOff_.onHeld = shell_.notesOff;
        addChildComponent(notesOff_);
        notesOff_.setVisible(shell_.notesOff != nullptr);
        if(shell_.record) { record_ = std::make_unique<RecordButtons>(engine_); addAndMakeVisible(*record_); }
        timerCallback();
        startTimerHz(4);
    }

    void TopBar::setActions(std::vector<Action> actions)
    {
        actions_ = std::move(actions);
        buttons_.clear();
        for(size_t i = 0; i < actions_.size() && i < 3; ++i)
        {
            auto* b = buttons_.add(new juce::TextButton(actions_[i].name));
            b->onClick = later(actions_[i].run);
            if(actions_[i].main) { b->setColour(juce::TextButton::buttonColourId, ink(Ink::Accent)); b->setColour(juce::TextButton::textColourOffId, ink(Ink::Accent).contrasting()); }
            addAndMakeVisible(b);
        }
        more_.setVisible(actions_.size() > 3);
        resized();
    }

    void TopBar::timerCallback()
    {
        // The open Performance and its scene; the tempo and the engine's load. Nothing while
        // hidden, as in an app in the background; the constructor's call, before the timer, fills it.
        if(!isShowing() && isTimerRunning()) return;
        if(const auto info = engine_.performanceInfo()) context_ = juce::String::fromUTF8(info->name.empty() ? "Untitled" : info->name.c_str());
        const auto& graph = engine_.graph();
        scene_.clear();
        for(const auto& s : graph.scenes) if(s.id == graph.scene) scene_ = juce::String::fromUTF8(s.name.c_str());
        const auto status = engine_.status();
        status_ = juce::String(status.tempo, 1) + " BPM   " + juce::String(juce::roundToInt(status.load * 100)) + "%";
        load_ = status.load >= .9 ? ink(Ink::Danger) : status.load >= .75 ? meterYellow : ink(Ink::Muted);
        resized();
        repaint();
    }

    void TopBar::resized()
    {
        const bool touch = mode() == 2 || desktopMode(*this) == 0;
        const int side = touch ? 44 : 32, top = (getHeight() - side) / 2;
        auto r = getLocalBounds().reduced(6, 0);
        for(auto* b : {&home_, &back_, &apps_}) if(b->isVisible()) { b->setBounds(r.removeFromLeft(side).withY(top).withHeight(side)); r.removeFromLeft(2); }
        markArea_ = mark_ ? r.removeFromLeft(side).withY(top).withHeight(side) : juce::Rectangle<int>();
        if(notesOff_.isVisible()) { notesOff_.setBounds(r.removeFromRight(side).withY(top).withHeight(side)); r.removeFromRight(6); }
        // The status, at the right, as wide as it needs.
        const juce::Font small{juce::FontOptions(textSize(TextRole::Small, mode()))};
        statusArea_ = r.removeFromRight(juce::GlyphArrangement::getStringWidthInt(small, status_) + 16);
        if(record_) { record_->setBounds(r.removeFromRight(record_->preferredWidth()).withY(top).withHeight(side)); r.removeFromRight(6); } // beside the tempo
        if(more_.isVisible()) { more_.setBounds(r.removeFromRight(side).withY(top).withHeight(side)); }
        for(int i = buttons_.size(); --i >= 0;)
        {
            auto* b = buttons_[i];
            const int w = juce::GlyphArrangement::getStringWidthInt(small, b->getButtonText()) + (touch ? 36 : 28);
            r.removeFromRight(6);
            b->setBounds(r.removeFromRight(w).withSizeKeepingCentre(w, touch ? 44 : 28));
        }
        // Where, then the Performance: the trail shortens first.
        const juce::Font body{juce::FontOptions(textSize(TextRole::Body, mode()), juce::Font::bold)};
        r.removeFromLeft(8);
        const int whereWidth = std::min(juce::GlyphArrangement::getStringWidthInt(body, where_) + 12, std::max(60, r.getWidth() / 3));
        r.removeFromLeft(whereWidth);
        const int contextWidth = std::min(r.getWidth() - 8, juce::GlyphArrangement::getStringWidthInt(small, context_ + "  ·  " + scene_) + 28);
        contextArea_ = r.removeFromLeft(std::max(0, contextWidth)).withSizeKeepingCentre(std::max(0, contextWidth), touch ? 36 : 26);
    }

    void TopBar::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        g.setColour(ink(Ink::Border).withAlpha(.35f)); g.fillRect(getLocalBounds().removeFromBottom(1));
        const int m = mode();
        const juce::Font body{juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)}, small{juce::FontOptions(textSize(TextRole::Small, m))};
        // Where.
        auto r = getLocalBounds().reduced(6, 0);
        for(auto* b : {&home_, &back_, &apps_}) if(b->isVisible()) r.setLeft(b->getRight() + 2);
        if(!markArea_.isEmpty())
        {
            // In one color, as the bar's icons: it follows the theme.
            drawMonoMark(g, markArea_.toFloat().withSizeKeepingCentre(markArea_.getHeight() * .7f, markArea_.getHeight() * .7f), ink(Ink::Text));
            r.setLeft(markArea_.getRight());
        }
        g.setColour(ink(Ink::Text));
        drawInked(g, where_, r.withTrimmedLeft(8).withRight(contextArea_.getX() - 4), body);
        // The Performance and scene, a pill to tap.
        if(!contextArea_.isEmpty())
        {
            g.setColour(ink(Ink::Surface)); g.fillRoundedRectangle(contextArea_.toFloat(), buttonRadius(float(contextArea_.getHeight())));
            g.setColour(ink(Ink::Text));
            drawInked(g, context_ + (scene_.isEmpty() ? juce::String() : juce::String::fromUTF8("  ·  ") + scene_), contextArea_.reduced(12, 0), small);
        }
        // The tempo and the engine's load.
        g.setColour(load_);
        drawInked(g, status_, statusArea_.reduced(8, 0), small, juce::Justification::right);
    }

    void TopBar::mouseUp(const juce::MouseEvent& e)
    {
        if(!contextArea_.contains(e.getPosition()) || !shell_.save) return;
        juce::PopupMenu menu;
        menu.addItem(1, "Save");
        menu.addItem(2, "Save As New...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(contextArea_)), [save = shell_.save](int chosen) { if(chosen > 0) save(chosen == 2); });
    }

    EngineRecovery::EngineRecovery(Engine& engine) : engine_(engine)
    {
        onRestart = [this] { engine_.restart(); update(); };
        restart_.setTooltip("Start the engine again, with a fresh session");
        restart_.onClick = [this] { if(onRestart) onRestart(); };
        addChildComponent(restart_);
    }
    void EngineRecovery::update()
    {
        show(!engine_.connected(), engine_.canRestart());
        if(engine_.restarting() && isVisible()) notice_ = "Starting the engine...";
    }
    void EngineRecovery::show(bool disconnected, bool canRestart)
    {
        notice_ = !disconnected ? juce::String()
                : canRestart ? "Engine stopped. Restart opens a fresh session." : "Engine disconnected. Reconnecting...";
        restart_.setVisible(disconnected && canRestart);
        setVisible(disconnected);
        if(disconnected) toFront(false); // over the view's own controls, which would take Restart's clicks
        repaint();
    }
    void EngineRecovery::resized()
    {
        const int side = desktopMode(*this) == 2 ? 40 : 28;
        restart_.setBounds(getLocalBounds().removeFromRight(170).reduced(8, std::max(0, (getHeight() - side) / 2)));
    }
    void EngineRecovery::paint(juce::Graphics& g)
    {
        g.setColour(ink(Ink::Danger));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
        const int mode = std::max(1, desktopMode(*this));
        g.setColour(ink(Ink::Danger).contrasting());
        drawInked(g, notice_, getLocalBounds().reduced(12, 0).withRight(restart_.isVisible() ? restart_.getX() - 8 : getRight() - 8),
                 juce::Font(juce::FontOptions(textSize(TextRole::Small, mode))), juce::Justification::centredLeft);
    }

    Launcher::Launcher(Engine& engine, std::vector<App> apps, TopBar::Shell shell) : engine_(engine), apps_(std::move(apps)), states_(apps_.size()), recovery_(engine), bar_(engine, "Home", [&] {
        shell.home = {}; shell.back = {}; return shell; }()) // Home is here; nothing to go back to
    {
        setTitle("Home");
        bar_.showMark();
        addAndMakeVisible(bar_);
        addChildComponent(recovery_); // the engine's notice and Restart, when it is gone
        timerCallback();
        startTimer(1000);
    }

    void Launcher::timerCallback()
    {
        for(size_t i = 0; i < apps_.size(); ++i) states_[i] = apps_[i].state ? apps_[i].state() : juce::String();
        recovery_.update();
        repaint();
    }

    void Launcher::open(int app)
    {
        if(app < 0 || app >= int(apps_.size()) || !apps_[size_t(app)].open) return;
        juce::MessageManager::callAsync([open = apps_[size_t(app)].open] { open(); });
    }

    void Launcher::resized()
    {
        bar_.setBounds(getLocalBounds().removeFromTop(TopBar::heightFor(*this)));
        recovery_.setBounds(getLocalBounds().withTrimmedTop(TopBar::heightFor(*this)).removeFromTop(36).reduced(8, 2));
        // Tiles in as many columns as fit at about 240 pixels, all of one size.
        const bool touch = mode() == 2 || desktopMode(*this) == 0;
        const int gap = touch ? 14 : 12, tileHeight = touch ? 150 : 132;
        auto area = getLocalBounds().withTrimmedTop(TopBar::heightFor(*this)).reduced(gap * 2, gap * 2);
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
        const float small = textSize(TextRole::Small, m), title = textSize(TextRole::Title, m);
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
            g.setColour(ink(Ink::Text)); drawInked(g, apps_[i].name, inside.removeFromBottom(int(title) + 6), juce::Font(juce::FontOptions(title, juce::Font::bold)));
            g.setColour(ink(Ink::Muted)); drawInked(g, states_[i], state, juce::Font(juce::FontOptions(small)));
        }
    }

    void Launcher::mouseUp(const juce::MouseEvent& e)
    {
        if(e.mouseWasDraggedSinceMouseDown()) return;
        for(size_t i = 0; i < tiles_.size(); ++i) if(tiles_[i].contains(e.getPosition())) { open(int(i)); return; }
    }

    AppFrame::AppFrame(Engine& engine, const juce::String& name, juce::Component& app, TopBar::Shell shell) : app_(app), bar_(engine, name, std::move(shell))
    {
        addAndMakeVisible(bar_);
        addAndMakeVisible(app_);
    }

    void AppFrame::resized()
    {
        bar_.setBounds(getLocalBounds().removeFromTop(TopBar::heightFor(*this)));
        app_.setBounds(getLocalBounds().withTrimmedTop(TopBar::heightFor(*this)));
    }

    LaunchNotice::LaunchNotice(const juce::String& name, const Control::Launch& refused) : title_(name + " did not open")
    {
        // The engine names apps by ID: Oscairn's own by their names here.
        auto reason = juce::String::fromUTF8(refused.reason.c_str());
        for(const auto& [id, shown] : {std::pair{"oscairn.performance", "Performance"}, {"oscairn.library", "Library"}, {"oscairn.setlist", "Setlist"}, {"oscairn.groovebox", "Groovebox"}, {"oscairn.recordings", "Recordings"}}) reason = reason.replace(id, shown);
        message_ = "It " + reason + ".";
        setTitle(title_);
        ok.setButtonText(refused.offerNext ? "Cancel" : "OK");
        addAndMakeVisible(ok);
        addChildComponent(letGo);
        letGo.setVisible(refused.offerNext);
        letGo.setColour(juce::TextButton::buttonColourId, ink(Ink::Accent)); letGo.setColour(juce::TextButton::textColourOffId, ink(Ink::Accent).contrasting());
        letGo.setTooltip("The next song then opens when you go to it, not at once");
    }

    void LaunchNotice::resized()
    {
        const int m = std::max(1, desktopMode(*this)), control = m == 2 ? 44 : 30;
        card_ = getLocalBounds().withSizeKeepingCentre(std::min(560, getWidth() - 32), m == 2 ? 230 : 200);
        auto bar = card_.reduced(20).removeFromBottom(control);
        ok.setBounds(bar.removeFromRight(110)); bar.removeFromRight(8);
        if(letGo.isVisible()) letGo.setBounds(bar.removeFromRight(200));
    }

    void LaunchNotice::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background).withAlpha(.7f));
        drawSection(g, card_, ink(Ink::Surface));
        const int m = std::max(1, desktopMode(*this));
        const juce::Font title(juce::FontOptions(textSize(TextRole::Title, m), juce::Font::bold)), body(juce::FontOptions(textSize(TextRole::Body, m)));
        auto r = card_.reduced(20);
        g.setColour(ink(Ink::Text)); drawInked(g, title_, r.removeFromTop(int(title.getHeight()) + 10), title);
        r.removeFromTop(6);
        // The message, a line at a time, broken between words to fit.
        g.setColour(ink(Ink::Muted));
        juce::String line;
        auto flush = [&] { drawInked(g, line, r.removeFromTop(int(body.getHeight()) + 6), body); line.clear(); };
        for(const auto& word : juce::StringArray::fromTokens(message_, " ", ""))
        {
            const auto longer = line.isEmpty() ? word : line + " " + word;
            if(line.isNotEmpty() && juce::GlyphArrangement::getStringWidthInt(body, longer) > r.getWidth()) { flush(); line = word; }
            else line = longer;
        }
        if(line.isNotEmpty()) flush();
    }

    bool askToLaunch(Engine& engine, const Control::AppMemory& app, const juce::String& name, std::function<void()> retry, std::function<void(std::unique_ptr<LaunchNotice>)> show)
    {
        const auto launch = engine.launchApp(app);
        if(launch.admitted) return true;
        auto notice = std::make_unique<LaunchNotice>(name, launch);
        notice->ok.onClick = [show] { juce::MessageManager::callAsync([show] { show(nullptr); }); }; // after the click is done with it
        notice->letGo.onClick = [&engine, app, retry, show] {
            engine.launchApp(app, true);
            // Opening asks again: admitted now, it is running; refused still, it says why.
            juce::MessageManager::callAsync([show, retry] { show(nullptr); if(retry) retry(); });
        };
        show(std::move(notice));
        return false;
    }

    AppSwitcher::AppSwitcher(Engine& engine, std::vector<Running> running, std::function<void(int)> open, std::function<void(int)> close, TopBar::Shell shell)
        : running_(std::move(running)), open_(std::move(open)), close_(std::move(close)), bar_(engine, "Running apps", [&] { shell.apps = {}; return shell; }())
    {
        addAndMakeVisible(bar_);
    }

    void AppSwitcher::resized()
    {
        bar_.setBounds(getLocalBounds().removeFromTop(TopBar::heightFor(*this)));
        auto area = getLocalBounds().withTrimmedTop(TopBar::heightFor(*this)).reduced(24);
        cards_.clear(); closes_.clear();
        const int width = std::min(260, (area.getWidth() - 16 * 3) / 4), height = 120;
        for(size_t i = 0; i < running_.size(); ++i)
        {
            const juce::Rectangle<int> card{area.getX() + int(i % 4) * (width + 16), area.getY() + int(i / 4) * (height + 16), width, height};
            cards_.push_back(card);
            closes_.push_back(card.withTrimmedTop(card.getHeight() - 52).reduced(12, 6).removeFromRight(90));
        }
        repaint();
    }

    void AppSwitcher::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = std::max(1, desktopMode(*this));
        if(running_.empty())
        {
            g.setColour(ink(Ink::Muted));
            drawInked(g, "No apps running. Open one from Home.", getLocalBounds().withTrimmedTop(TopBar::heightFor(*this)), juce::Font(juce::FontOptions(textSize(TextRole::Body, m))), juce::Justification::horizontallyCentred);
        }
        for(size_t i = 0; i < cards_.size(); ++i)
        {
            const auto r = cards_[i];
            drawSection(g, r, ink(Ink::Surface));
            auto top = r.reduced(14).removeFromTop(32);
            drawIcon(g, running_[i].icon, top.removeFromLeft(28).toFloat().reduced(2), ink(Ink::Text));
            top.removeFromLeft(10);
            g.setColour(ink(Ink::Text)); drawInked(g, running_[i].name, top, juce::Font(juce::FontOptions(textSize(TextRole::Title, m), juce::Font::bold)));
            const auto c = closes_[i].toFloat();
            g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(c, buttonRadius(c.getHeight()));
            g.setColour(ink(Ink::Text)); drawInked(g, "Close", closes_[i], juce::Font(juce::FontOptions(textSize(TextRole::Button, m))), juce::Justification::horizontallyCentred);
        }
    }

    void AppSwitcher::mouseUp(const juce::MouseEvent& e)
    {
        if(e.mouseWasDraggedSinceMouseDown()) return;
        for(size_t i = 0; i < cards_.size(); ++i)
        {
            if(closes_[i].contains(e.getPosition())) { if(close_) juce::MessageManager::callAsync([f = close_, i] { f(int(i)); }); return; }
            if(cards_[i].contains(e.getPosition())) { if(open_) juce::MessageManager::callAsync([f = open_, i] { f(int(i)); }); return; }
        }
    }
}

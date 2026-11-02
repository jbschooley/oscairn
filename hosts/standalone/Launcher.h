// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Icons.h"
#include "Recording.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// The top bar (RFC 0030), one in every app: Home, Back and the running apps where the
    /// product has no buttons for them, where the musician is, the open Performance and its
    /// scene (a tap: Save), the app's own actions and its menu, the tempo and the engine's load,
    /// and All notes off (held half a second, so a brush does not cut a chord).
    class TopBar : public juce::Component, private juce::Timer
    {
    public:
        // 48 pixels with 44-pixel targets on a touch screen or a device's panel, 36 with a mouse.
        static int heightFor(const juce::Component& c) { return desktopMode(c) == 1 ? 36 : 48; }
        struct Action { juce::String name; std::function<void()> run; bool main = false; };
        // What the shell does for the bar's buttons; a button without one is not shown.
        struct Shell
        {
            std::function<void()> home, back, apps, notesOff;
            std::function<void(bool asNew)> save;
            bool record = false; // Record and Capture beside the tempo (RFC 0020), showing a take recording
        };
        TopBar(Engine& engine, const juce::String& where, Shell shell);
        void setActions(std::vector<Action> actions); // the first main one in the accent; past three, in ⋮
        const juce::String& context() const { return context_; }
        void showMark() { mark_ = true; resized(); } // Oscairn's mark before where: Home's header

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        // Held, not tapped: notes off only after half a second.
        struct HoldButton : IconButton
        {
            using IconButton::IconButton;
            std::function<void()> onHeld;
            void mouseUp(const juce::MouseEvent& e) override
            {
                IconButton::mouseUp(e);
                if(e.getLengthOfMousePress() >= 500 && contains(e.getPosition()) && onHeld) onHeld();
            }
        };
        void timerCallback() override;
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        juce::String where_;
        Shell shell_;
        juce::String context_, scene_, status_;
        juce::Colour load_;
        juce::Rectangle<int> contextArea_, statusArea_, markArea_;
        bool mark_ = false;
        IconButton home_{IconButton::Glyph::Home, "Home"}, back_{IconButton::Glyph::Back, "Back"}, apps_{IconButton::Glyph::Apps, "Running apps"},
                   more_{IconButton::Glyph::More, "More"};
        HoldButton notesOff_{IconButton::Glyph::Panic, "All notes off"};
        std::unique_ptr<RecordButtons> record_;
        std::vector<Action> actions_;
        juce::OwnedArray<juce::TextButton> buttons_;
    };

    /// The engine's disconnect notice and its Restart button (RFC 0030's desktop, which has no
    /// Panel to show them): a strip across the top of a view, shown while the engine is gone,
    /// offering a restart when one is possible. Tapping the button restarts the managed engine.
    class EngineRecovery : public juce::Component
    {
    public:
        explicit EngineRecovery(Engine& engine) : engine_(engine) {}
        void update();                              // says what is wrong, and whether Restart shows
        const juce::String& notice() const { return notice_; }
        bool restartShown() const { return restart_; }
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
    private:
        Engine& engine_;
        juce::String notice_;
        bool restart_ = false;
        juce::Rectangle<int> button_;
    };

    /// Home (RFC 0030): the product's apps as tiles under the top bar, each with a line of its
    /// state. In the design the launcher is an app answering oscairn.home, replaceable; until
    /// apps are packages (RFC 0022 step 2) this one is built in.
    class Launcher : public juce::Component, private juce::Timer
    {
    public:
        struct App
        {
            juce::String name;
            const char* icon;                     // a Lucide icon the app carries (Icons.h)
            std::function<juce::String()> state;  // a line of the app's state, or empty
            std::function<void()> open;
        };
        Launcher(Engine& engine, std::vector<App> apps, TopBar::Shell shell = {});
        int apps() const { return int(apps_.size()); }
        EngineRecovery& recovery() { return recovery_; } // the engine's disconnect notice (RFC 0030)
        const juce::String& name(int app) const { return apps_[size_t(app)].name; }
        void open(int app); // after this click is done with the launcher: opening may replace it

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        std::vector<App> apps_;
        std::vector<juce::Rectangle<int>> tiles_;
        std::vector<juce::String> states_;
        EngineRecovery recovery_;
        TopBar bar_;
    };

    /// An app on a device's screen (RFC 0030): the top bar over the app.
    class AppFrame : public juce::Component
    {
    public:
        AppFrame(Engine& engine, const juce::String& name, juce::Component& app, TopBar::Shell shell);
        TopBar& bar() { return bar_; }
        juce::Component& app() { return app_; }
        void resized() override;

    private:
        juce::Component& app_;
        TopBar bar_;
    };

    /// Why an app did not open (RFC 0025), over the screen: what it needs and what closing would
    /// free, as the engine said it; Let the next song go when only the prepared next setlist entry
    /// is in the way.
    class LaunchNotice : public juce::Component
    {
    public:
        LaunchNotice(const juce::String& name, const Control::Launch& refused);
        const juce::String& message() const { return message_; }
        juce::TextButton letGo{"Let the next song go"}, ok{"OK"};
        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        juce::String title_, message_;
        juce::Rectangle<int> card_;
    };
    /// Asks the engine before an app opens (RFC 0025): true when it is admitted. Refused: false,
    /// and `show` gets a notice to put over the screen (and nothing once it is done with); its Let
    /// the next song go asks again, letting that entry go, then runs `retry`.
    bool askToLaunch(Engine& engine, const Control::AppMemory& app, const juce::String& name, std::function<void()> retry, std::function<void(std::unique_ptr<LaunchNotice>)> show);

    /// The running apps (RFC 0030), on a device's screen: each kept with its state; a tap brings
    /// one forward, Close lets it go.
    class AppSwitcher : public juce::Component
    {
    public:
        struct Running { juce::String name; const char* icon; };
        AppSwitcher(Engine& engine, std::vector<Running> running, std::function<void(int)> open, std::function<void(int)> close, TopBar::Shell shell);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        std::vector<Running> running_;
        std::function<void(int)> open_, close_;
        std::vector<juce::Rectangle<int>> cards_, closes_;
        TopBar bar_;
    };
}

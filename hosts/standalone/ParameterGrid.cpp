// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterGrid.h"
#include "Theme.h"

#include <clap/ext/params.h>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <map>

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int maxListed = 64; // more values than this get a slider (filter types run to dozens)
        int values(const Engine::ParamInfo& p) { return p.stepped ? int(std::lround(p.maximum - p.minimum)) + 1 : 0; }
        // "Operator 12" -> ("Operator", 12); a name without a trailing number has none.
        std::pair<std::string, int> numbered(const std::string& name)
        {
            auto end = name.size();
            while(end && std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
            if(end == name.size() || end == 0 || name[end - 1] != ' ') return {name, -1};
            return {name.substr(0, end - 1), std::stoi(name.substr(end))};
        }

        /// A page in the list down the left: its name, and a fill when it is the one shown.
        /// A page in the list down the left, or a tab across the top of a page: its name, and a
        /// fill with an accent mark (left, or under a tab) when it is the one shown.
        class PageButton : public juce::Button
        {
        public:
            PageButton(const juce::String& name, bool tab) : juce::Button(name), tab_(tab) {}
            bool shown = false;
            void paintButton(juce::Graphics& g, bool hover, bool) override
            {
                const auto r = getLocalBounds().toFloat();
                if(shown || hover) { g.setColour(shown ? ink(Ink::Raised) : ink(Ink::Raised).withAlpha(.5f)); g.fillRoundedRectangle(r, std::min(themeData().radius, r.getHeight() / 2)); }
                if(shown) { g.setColour(ink(Ink::Accent)); g.fillRect(tab_ ? r.withTop(r.getBottom() - 2.f).reduced(themeData().radius, 0) : r.withWidth(3.f)); }
                g.setColour(shown ? ink(Ink::Text) : ink(Ink::Muted));
                g.setFont(juce::FontOptions(textSize(tab_ ? TextRole::Button : TextRole::Body, std::max(1, desktopMode(*this))), shown ? juce::Font::bold : juce::Font::plain));
                g.drawText(getName(), tab_ ? r : r.withTrimmedLeft(12), tab_ ? juce::Justification::centred : juce::Justification::centredLeft, true);
            }
        private:
            bool tab_;
        };
        constexpr size_t tabbedAbove = 24; // a page with more parameters than this gets tabs
        constexpr int maxChoices = 4;      // up to this many values: buttons side by side
        bool onOff(const juce::String& a, const juce::String& b) { return a.equalsIgnoreCase("off") && b.equalsIgnoreCase("on"); }
        std::string level(const std::string& module, size_t n) // the module path's nth level, or ""
        {
            size_t from = 0;
            for(size_t i = 0; i < n; ++i) { const auto slash = module.find('/', from); if(slash == std::string::npos) return {}; from = slash + 1; }
            return module.substr(from, module.find('/', from) - from);
        }
        std::string after(const std::string& module, size_t n) // the path below its first n levels
        {
            size_t from = 0;
            for(size_t i = 0; i < n; ++i) { const auto slash = module.find('/', from); if(slash == std::string::npos) return {}; from = slash + 1; }
            return module.substr(from);
        }
    }

    EnvelopeShape envelopeOf(const std::vector<std::string>& names)
    {
        auto find = [&](const char* name) {
            for(size_t i = 0; i < names.size(); ++i) if(juce::String(names[i]).equalsIgnoreCase(name)) return int(i);
            return -1;
        };
        auto either = [&](std::initializer_list<const char*> options) { for(const auto* o : options) if(const int i = find(o); i >= 0) return i; return -1; };
        EnvelopeShape e;
        // DX7: four rates and four levels; it starts from level 4 and holds at level 3.
        const int r1 = find("Rate 1"), r2 = find("Rate 2"), r3 = find("Rate 3"), r4 = find("Rate 4");
        const int l1 = find("Level 1"), l2 = find("Level 2"), l3 = find("Level 3"), l4 = find("Level 4");
        if(std::min({r1, r2, r3, r4, l1, l2, l3, l4}) >= 0)
        {
            e.start = l4;
            e.points = {{r1, l1, 0, true}, {r2, l2, 0, true}, {r3, l3, 0, true, true}, {r4, l4, 0, true}};
            return e;
        }
        // FM-X: a time and a level for each stage, from a hold (or initial) level.
        const int attackTime = find("Attack Time"), releaseTime = find("Release Time");
        if(attackTime >= 0 && releaseTime >= 0)
        {
            e.start = either({"Release (Hold) Level", "Hold Level", "Initial Level"});
            if(const int hold = find("Hold Time"); hold >= 0) e.points.push_back({hold, e.start});
            e.points.push_back({attackTime, find("Attack Level"), 1});
            for(const auto& [t, l] : {std::pair{"Decay 1 Time", "Decay 1 Level"}, std::pair{"Decay 2 Time", "Decay 2 Level"}, std::pair{"Decay Time", "Decay Level"}})
                if(const int time = find(t); time >= 0) e.points.push_back({time, find(l), e.points.back().fixed});
            e.points.back().sustain = true;
            e.points.push_back({releaseTime, either({"Release Level", "Release (Hold) Level"}), 0});
            if(e.points.back().level < 0 && e.start >= 0) e.points.back().level = e.start;
            return e;
        }
        // ADSR, with an optional delay and hold: from silence to full, down to the sustain level.
        const int attack = find("Attack"), decay = find("Decay"), sustain = find("Sustain"), release = find("Release");
        if(attack >= 0 && release >= 0 && (decay >= 0 || sustain >= 0))
        {
            if(const int delay = find("Delay"); delay >= 0) e.points.push_back({delay, -1, 0});
            e.points.push_back({attack, -1, 1});
            if(const int hold = find("Hold"); hold >= 0) e.points.push_back({hold, -1, 1});
            if(decay >= 0) e.points.push_back({decay, sustain, sustain >= 0 ? 0.0 : 1.0});
            if(decay < 0 && sustain >= 0) e.points.back().level = sustain;
            e.points.back().sustain = true;
            e.points.push_back({release, -1, 0});
        }
        return e;
    }

    LfoShape lfoOf(const std::vector<std::string>& names)
    {
        auto either = [&](std::initializer_list<const char*> options) {
            for(const auto* o : options) for(size_t i = 0; i < names.size(); ++i) if(juce::String(names[i]).equalsIgnoreCase(o)) return int(i);
            return -1;
        };
        LfoShape l;
        l.wave = either({"Wave", "Waveform", "Type", "Shape"}); l.speed = either({"Speed", "Rate", "Frequency"});
        l.delay = either({"Delay"}); l.fade = either({"Fade In", "Attack"}); l.phase = either({"Phase"});
        for(int n = 1; n <= 64; ++n) { const auto name = "User Step " + std::to_string(n); const int i = either({name.c_str()}); if(i < 0) break; l.steps.push_back(i); }
        return l;
    }

    Wave waveNamed(const juce::String& name)
    {
        const auto t = name.toLowerCase();
        if(t.contains("sin")) return Wave::Sine;
        if(t.contains("tri")) return Wave::Triangle;
        if(t.contains("square") || t.contains("pulse")) return Wave::Square;
        if(t.contains("saw")) return t.contains("down") ? Wave::SawDown : Wave::SawUp;
        if(t.contains("sample") || t.contains("s&h") || t.contains("noise") || t.contains("random")) return Wave::Random;
        if(t.contains("user") || t.contains("step")) return Wave::Steps;
        return Wave::None;
    }

    FilterShape filterOf(const std::vector<std::string>& names)
    {
        auto find = [&](std::initializer_list<const char*> options) {
            for(const auto* o : options) for(size_t i = 0; i < names.size(); ++i) if(juce::String(names[i]).equalsIgnoreCase(o)) return int(i);
            return -1;
        };
        return {find({"Cutoff", "Frequency"}), find({"Resonance", "Q"}), find({"Type", "Filter Type"})};
    }

    FilterResponse filterNamed(const juce::String& name)
    {
        using K = FilterResponse::Kind;
        const auto t = name.toUpperCase().removeCharacters(" ");
        FilterResponse r;
        if(t == "OFF" || t == "THRU") { r.kind = K::None; return r; }
        const bool lp = t.contains("LP"), hp = t.contains("HP"), bp = t.contains("BP"), notch = t.contains("BEF") || t.contains("NOTCH") || t.startsWith("N");
        r.kind = bp || (lp && hp) ? K::BandPass : notch ? K::Notch : hp ? K::HighPass : lp ? K::LowPass : K::Unknown;
        r.poles = t.contains("24") ? 4 : t.contains("18") ? 3 : t.contains("12") ? 2 : t.contains("6") ? 1 : 2;
        return r;
    }

    std::vector<int> linkedOrder(const EnvelopeShape& e)
    {
        std::vector<int> order;
        auto add = [&](int i) { if(i >= 0 && std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i); };
        const bool rates = !e.points.empty() && e.points.front().rate;
        add(e.start);
        if(rates) { for(const auto& p : e.points) add(p.time); for(const auto& p : e.points) add(p.level); }
        else for(const auto& p : e.points) { add(p.time); add(p.level); }
        if(rates) { order.erase(std::find(order.begin(), order.end(), e.start)); order.push_back(e.start); } // level 4 last, under rate 4
        return order;
    }
    std::vector<int> linkedOrder(const LfoShape& l)
    {
        std::vector<int> order;
        for(const int i : {l.wave, l.speed, l.delay, l.fade, l.phase}) if(i >= 0) order.push_back(i);
        return order;
    }
    std::vector<int> linkedOrder(const FilterShape& f)
    {
        std::vector<int> order;
        for(const int i : {f.type, f.cutoff, f.resonance}) if(i >= 0) order.push_back(i);
        return order;
    }

    /// A filter's response across the audio range: its cutoff (across) and resonance (the peak)
    /// for the type its value text names. A preview: its tiles edit it.
    class ParameterGrid::Filter : public juce::Component
    {
    public:
        Filter(ParameterGrid& grid, FilterShape shape, std::vector<size_t> params) : grid_(grid), shape_(shape), params_(std::move(params)) {}
        void paint(juce::Graphics& g) override
        {
            const int mode = std::max(1, desktopMode(*this));
            juce::String name = "Filter";
            FilterResponse response;
            if(shape_.type >= 0)
            {
                const auto& type = grid_.params_[params_[size_t(shape_.type)]];
                const auto text = juce::String::fromUTF8(grid_.engine_.paramText(grid_.name_, type.id, grid_.engine_.param(grid_.name_, type.id)).c_str());
                name << juce::String::fromUTF8("  ·  ") << text; response = filterNamed(text);
            }
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold));
            g.drawText(name, getLocalBounds().reduced(10, 8).removeFromTop(16), juce::Justification::topLeft);
            const auto area = getLocalBounds().toFloat().reduced(14, 10).withTrimmedTop(18);
            g.setColour(ink(Ink::Border).withAlpha(.35f)); g.drawRect(area, 1.f);
            if(response.kind == FilterResponse::Kind::Unknown)
            {
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode)));
                g.drawText("Drawn by the plugin", area, juce::Justification::centred);
                return;
            }
            // A resonant two-pole section, raised to the type's order: across 20 Hz to 20 kHz on a
            // log scale, the cutoff at its parameter's place, the top at +24 dB.
            const double cutoff = std::pow(1000.0, grid_.normal(params_[size_t(shape_.cutoff)])) * 20, q = .707 + 8 * grid_.normal(params_[size_t(shape_.resonance)]);
            juce::Path line, fill;
            const int n = int(area.getWidth());
            for(int i = 0; i <= n; ++i)
            {
                const double f = std::pow(1000.0, double(i) / n) * 20, x = f / cutoff, d = std::sqrt((1 - x * x) * (1 - x * x) + (x / q) * (x / q));
                double gain = 1;
                switch(response.kind)
                {
                case FilterResponse::Kind::LowPass: gain = 1 / d; break;
                case FilterResponse::Kind::HighPass: gain = x * x / d; break;
                case FilterResponse::Kind::BandPass: gain = (x / q) / d; break;
                case FilterResponse::Kind::Notch: gain = std::abs(1 - x * x) / d; break;
                default: break;
                }
                const double db = std::clamp(20 * std::log10(std::max(1e-6, std::pow(gain, response.poles / 2.0))), -48.0, 24.0);
                const float px = area.getX() + float(i), py = area.getY() + float((24 - db) / 72) * area.getHeight();
                if(i == 0) { line.startNewSubPath(px, py); fill.startNewSubPath(px, area.getBottom()); }
                else line.lineTo(px, py);
                fill.lineTo(px, py);
            }
            fill.lineTo(area.getRight(), area.getBottom()); fill.closeSubPath();
            g.setColour(ink(Ink::Accent).withAlpha(.14f)); g.fillPath(fill);
            g.setColour(ink(Ink::Accent)); g.strokePath(line, juce::PathStrokeType(2.f));
        }
    private:
        ParameterGrid& grid_;
        FilterShape shape_;
        std::vector<size_t> params_;
    };

    /// An LFO's waveform over a few cycles (more when faster), after its delay and fading in, as
    /// its parameters set it. A preview: its tiles edit it.
    class ParameterGrid::Lfo : public juce::Component
    {
    public:
        Lfo(ParameterGrid& grid, LfoShape shape, std::vector<size_t> params) : grid_(grid), shape_(std::move(shape)), params_(std::move(params)) {}
        void paint(juce::Graphics& g) override
        {
            const int mode = std::max(1, desktopMode(*this));
            const auto& wave = grid_.params_[params_[size_t(shape_.wave)]];
            const auto name = juce::String::fromUTF8(grid_.engine_.paramText(grid_.name_, wave.id, grid_.engine_.param(grid_.name_, wave.id)).c_str());
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold));
            g.drawText("LFO  " + juce::String::fromUTF8("·") + "  " + name, getLocalBounds().reduced(10, 8).removeFromTop(16), juce::Justification::topLeft);
            const auto area = getLocalBounds().toFloat().reduced(14, 10).withTrimmedTop(18);
            g.setColour(ink(Ink::Border).withAlpha(.35f)); g.drawHorizontalLine(int(area.getCentreY()), area.getX(), area.getRight());
            const auto kind = waveNamed(name);
            if(kind == Wave::None || (kind == Wave::Steps && shape_.steps.empty()))
            {
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode)));
                g.drawText("Drawn by the plugin", area, juce::Justification::centred);
                return;
            }
            auto value = [&](int index) { return index >= 0 ? grid_.normal(params_[size_t(index)]) : 0.0; };
            const double cycles = 1 + 4 * value(shape_.speed), phase = value(shape_.phase);
            const double delay = .25 * value(shape_.delay), fade = .25 * value(shape_.fade);
            juce::Path line;
            const int n = int(area.getWidth());
            for(int i = 0; i <= n; ++i)
            {
                const double x = double(i) / n;
                double y = 0;
                if(x >= delay)
                {
                    const double t = std::fmod((x - delay) / (1 - delay) * cycles + phase, 1.0);
                    switch(kind)
                    {
                    case Wave::Sine: y = std::sin(t * juce::MathConstants<double>::twoPi); break;
                    case Wave::Triangle: y = t < .25 ? t * 4 : t < .75 ? 2 - t * 4 : t * 4 - 4; break;
                    case Wave::Square: y = t < .5 ? 1 : -1; break;
                    case Wave::SawUp: y = t * 2 - 1; break;
                    case Wave::SawDown: y = 1 - t * 2; break;
                    case Wave::Random: { const int step = int((x - delay) / (1 - delay) * cycles * 2); y = std::sin(step * 12.9898) * 43758.5453; y = (y - std::floor(y)) * 2 - 1; break; } // the same steps each time
                    case Wave::Steps: { const auto s = size_t(t * double(shape_.steps.size())); y = value(shape_.steps[std::min(s, shape_.steps.size() - 1)]) * 2 - 1; break; }
                    case Wave::None: break;
                    }
                    if(fade > 0) y *= std::min(1.0, (x - delay) / fade);
                }
                const float px = area.getX() + float(i), py = area.getCentreY() - float(y) * area.getHeight() / 2;
                if(i == 0) line.startNewSubPath(px, py); else line.lineTo(px, py);
            }
            g.setColour(ink(Ink::Accent)); g.strokePath(line, juce::PathStrokeType(2.f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        }
    private:
        ParameterGrid& grid_;
        LfoShape shape_;
        std::vector<size_t> params_;
    };

    /// An envelope's curve, drawn from its parameters; drag a point to change the time (across)
    /// and the level (up and down) it is reached at. The parameters keep their own tiles.
    class ParameterGrid::Envelope : public juce::Component
    {
    public:
        // shape's indices are into `params`, the grid's parameter indices of its group.
        Envelope(ParameterGrid& grid, EnvelopeShape shape, std::vector<size_t> params) : grid_(grid), shape_(std::move(shape)), params_(std::move(params)) {}
        void paint(juce::Graphics& g) override
        {
            layout();
            const int mode = std::max(1, desktopMode(*this));
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold));
            g.drawText("Envelope", getLocalBounds().reduced(10, 8).removeFromTop(16), juce::Justification::topLeft);
            // A faint baseline and top, the shape filled under its line, and a handle per point.
            g.setColour(ink(Ink::Border).withAlpha(.35f));
            g.drawHorizontalLine(area_.getBottom(), area_.getX(), area_.getRight());
            g.drawHorizontalLine(area_.getY(), area_.getX(), area_.getRight());
            juce::Path line; line.startNewSubPath(start_);
            for(const auto& h : handles_) { line.lineTo(h.at); if(h.holdTo > 0) line.lineTo(h.holdTo, h.at.y); }
            juce::Path fill(line); fill.lineTo(line.getCurrentPosition().x, float(area_.getBottom())); fill.lineTo(start_.x, float(area_.getBottom())); fill.closeSubPath();
            g.setColour(ink(Ink::Accent).withAlpha(.14f)); g.fillPath(fill);
            g.setColour(ink(Ink::Accent)); g.strokePath(line, juce::PathStrokeType(2.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            for(size_t i = 0; i < handles_.size(); ++i)
            {
                const float r = int(i) == dragging_ ? 6.f : 4.5f;
                g.setColour(juce::Colours::white); g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(handles_[i].at));
                g.setColour(ink(Ink::Accent)); g.drawEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(handles_[i].at), 1.5f);
            }
        }
        void mouseDown(const juce::MouseEvent& e) override
        {
            layout(); dragging_ = -1;
            float best = 14;
            for(size_t i = 0; i < handles_.size(); ++i) if(const float d = handles_[i].at.getDistanceFrom(e.position); d < best) { best = d; dragging_ = int(i); }
            if(dragging_ < 0) return;
            for(const int p : touched(dragging_)) grid_.engine_.paramGesture(grid_.name_, grid_.params_[params_[size_t(p)]].id, true);
            repaint();
        }
        void mouseDrag(const juce::MouseEvent& e) override
        {
            if(dragging_ < 0) return;
            const auto& point = shape_.points[size_t(dragging_)];
            const auto& h = handles_[size_t(dragging_)];
            if(point.time >= 0)
            {
                const double across = std::clamp((e.position.x - h.from) / segment_, 0.f, 1.f);
                grid_.setNormal(params_[size_t(point.time)], point.rate ? 1 - across : across);
            }
            if(point.level >= 0) grid_.setNormal(params_[size_t(point.level)], std::clamp((area_.getBottom() - e.position.y) / area_.getHeight(), 0.f, 1.f));
            repaint();
        }
        void mouseUp(const juce::MouseEvent&) override
        {
            if(dragging_ >= 0) for(const int p : touched(dragging_)) grid_.engine_.paramGesture(grid_.name_, grid_.params_[params_[size_t(p)]].id, false);
            dragging_ = -1; repaint();
        }
    private:
        struct Handle { juce::Point<float> at; float from = 0, holdTo = 0; }; // from: where its segment starts; holdTo: the sustain's end
        std::vector<int> touched(int i) const { std::vector<int> p; const auto& pt = shape_.points[size_t(i)]; if(pt.time >= 0) p.push_back(pt.time); if(pt.level >= 0) p.push_back(pt.level); return p; }
        float level(int param, double fixed) const { const double v = param >= 0 ? grid_.normal(params_[size_t(param)]) : fixed; return area_.getBottom() - float(v) * area_.getHeight(); }
        void layout()
        {
            // Each timed segment has the same room, scaled by its time; the sustain a fixed width.
            area_ = getLocalBounds().toFloat().reduced(14, 10).withTrimmedTop(18);
            const float hold = area_.getWidth() * .14f;
            segment_ = (area_.getWidth() - hold) / float(std::max<size_t>(1, shape_.points.size()));
            start_ = {area_.getX(), level(shape_.start, shape_.fixedStart)};
            handles_.clear();
            float x = start_.x;
            for(const auto& p : shape_.points)
            {
                const double t = p.time >= 0 ? grid_.normal(params_[size_t(p.time)]) : 0;
                Handle h; h.from = x;
                x += float(p.rate ? 1 - t : t) * segment_;
                h.at = {x, level(p.level, p.fixed)};
                if(p.sustain) { x += hold; h.holdTo = x; }
                handles_.push_back(h);
            }
        }
        ParameterGrid& grid_;
        EnvelopeShape shape_;
        std::vector<size_t> params_;
        juce::Rectangle<float> area_;
        juce::Point<float> start_;
        float segment_ = 1;
        std::vector<Handle> handles_;
        int dragging_ = -1;
    };

    double ParameterGrid::normal(size_t param) const
    {
        const auto& p = params_[param];
        return p.maximum > p.minimum ? std::clamp((engine_.param(name_, p.id) - p.minimum) / (p.maximum - p.minimum), 0.0, 1.0) : 0.0;
    }
    void ParameterGrid::setNormal(size_t param, double value)
    {
        const auto& p = params_[param];
        double v = p.minimum + value * (p.maximum - p.minimum);
        if(p.stepped) v = std::round(v);
        engine_.setParam(name_, p.id, v);
    }

    /// A few values side by side (Mono | Poly), the one set lifted out of the track, like the
    /// desktop's tabs; choosing one sets it.
    class ParameterGrid::Choices : public juce::Component
    {
    public:
        explicit Choices(juce::StringArray names) : names_(std::move(names)) {}
        std::function<void(int)> onChoose;
        int chosen = 0;
        void paint(juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat().reduced(1);
            const float radius = buttonRadius(r.getHeight());
            g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(r, radius);
            drawEdge(g, r, radius);
            const float w = r.getWidth() / float(names_.size());
            g.setFont(juce::FontOptions(textSize(TextRole::Button, std::max(1, desktopMode(*this)))));
            for(int i = 0; i < names_.size(); ++i)
            {
                const auto cell = juce::Rectangle<float>(r.getX() + w * float(i), r.getY(), w, r.getHeight());
                if(i == chosen) { g.setColour(ink(Ink::Accent)); g.fillRoundedRectangle(cell.reduced(2), std::max(0.f, radius - 2)); }
                g.setColour(i == chosen ? ink(Ink::Accent).contrasting() : ink(Ink::Muted));
                g.drawFittedText(names_[i], cell.reduced(4, 0).toNearestInt(), juce::Justification::centred, 1, .8f);
            }
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if(!getLocalBounds().contains(e.getPosition()) || names_.isEmpty()) return;
            const int i = std::clamp(int(e.position.x / (float(getWidth()) / float(names_.size()))), 0, names_.size() - 1);
            if(onChoose) onChoose(i);
        }
    private:
        juce::StringArray names_;
    };

    /// The scrolling area: the tiles' backgrounds and the headings, under their controls.
    class ParameterGrid::Content : public juce::Component
    {
    public:
        explicit Content(ParameterGrid& grid) : grid_(grid) {}
        void paint(juce::Graphics& g) override
        {
            const int mode = std::max(1, desktopMode(*this));
            // Linked tiles share a card per row, a line between neighbours.
            for(const auto& l : grid_.links_) drawSection(g, l, ink(Ink::Surface));
            for(const auto& t : grid_.tiles_)
            {
                if(!t.linked) { drawSection(g, t.bounds, ink(Ink::Surface)); continue; }
                g.setColour(ink(Ink::Border).withAlpha(.6f));
                for(const auto& l : grid_.links_)
                {
                    if(!l.contains(t.bounds)) continue;
                    if(t.bounds.getRight() < l.getRight()) g.fillRect(t.bounds.getRight() - 1, t.bounds.getY() + 8, 1, t.bounds.getHeight() - 16); // to its right
                    if(t.bounds.getY() > l.getY()) g.fillRect(t.bounds.getX() + 8, t.bounds.getY(), t.bounds.getWidth() - 16, 1);                  // above, in a wrapped group
                }
            }
            for(const auto& p : grid_.panels_) drawSection(g, p, ink(Ink::Surface));
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold));
            for(const auto& h : grid_.headings_) g.drawText(h.text, h.bounds, juce::Justification::bottomLeft, true);
        }
    private:
        ParameterGrid& grid_;
    };

    ParameterGrid::ParameterGrid(Engine& engine, NodeId node, std::string name) : engine_(engine), node_(node), name_(std::move(name)), content_(std::make_unique<Content>(*this))
    {
        try { params_ = engine_.parameters(node_); } catch(const std::exception&) {}
        std::erase_if(params_, [](const Engine::ParamInfo& p) { return (p.flags & CLAP_PARAM_IS_HIDDEN) != 0; });
        // A page per top-level module, in the order the plugin lists them.
        std::map<std::string, size_t> index;
        for(size_t i = 0; i < params_.size(); ++i)
        {
            const auto& m = params_[i].module;
            const auto top = m.empty() ? std::string("Main") : m.substr(0, m.find('/'));
            auto [at, added] = index.try_emplace(top, pages_.size());
            if(added) pages_.push_back({top, {}});
            pages_[at->second].params.push_back(i);
        }
        // The plugin's order, except that numbered pages of one kind sit together in number
        // order (Operator 1 to 8), where the first of them came.
        std::map<std::string, size_t> family;
        for(size_t i = 0; i < pages_.size(); ++i) family.try_emplace(numbered(pages_[i].name).first, i);
        std::stable_sort(pages_.begin(), pages_.end(), [&](const Page& a, const Page& b) {
            const auto x = numbered(a.name), y = numbered(b.name);
            const auto fx = family.at(x.first), fy = family.at(y.first);
            return fx != fy ? fx < fy : x.second < y.second;
        });
        if(pages_.size() > 1)
            for(size_t i = 0; i < pages_.size(); ++i)
            {
                auto b = std::make_unique<PageButton>(text(pages_[i].name), false);
                b->onClick = [this, i] { showPage(i); };
                addAndMakeVisible(*b); pageButtons_.push_back(std::move(b));
            }
        viewport_.setViewedComponent(content_.get(), false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        search_.setTextToShowWhenEmpty("Search parameters", ink(Ink::Muted));
        search_.setJustification(juce::Justification::centredLeft); search_.setIndents(10, 0);
        search_.setTooltip("Find a parameter on any page by its name or module");
        search_.onTextChange = [this] { tab_ = 0; viewport_.setViewPosition(0, 0); refresh(); };
        search_.onEscapeKey = [this] { search_.clear(); };
        if(params_.size() > tabbedAbove) addAndMakeVisible(search_);
        startTimer(60);
    }

    ParameterGrid::~ParameterGrid() { stopTimer(); }

    void ParameterGrid::showPage(size_t page)
    {
        if(page >= pages_.size()) return;
        page_ = page; tab_ = 0; viewport_.setViewPosition(0, 0);
        if(search_.isEmpty()) refresh(); else search_.clear(); // clearing the search refreshes
    }

    void ParameterGrid::showTab(size_t tab)
    {
        if(tab >= tabNames_.size()) return;
        tab_ = tab; viewport_.setViewPosition(0, 0);
        refresh();
    }

    std::vector<std::string> ParameterGrid::tabsFor(size_t page) const
    {
        std::vector<std::string> names;
        if(page >= pages_.size() || pages_[page].params.size() <= tabbedAbove) return names;
        for(const auto i : pages_[page].params)
            if(const auto name = level(params_[i].module, 1); std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        if(names.size() < 2) names.clear();
        return names;
    }

    void ParameterGrid::resized() { refresh(); }

    void ParameterGrid::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        if(pages_.empty())
        {
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this)))));
            g.drawText(text(name_) + " has no parameters to show", getLocalBounds(), juce::Justification::centred);
        }
    }

    void ParameterGrid::refresh()
    {
        const int mode = std::max(1, desktopMode(*this));
        // The tabs for this page (none while searching), remade only when they change.
        const auto names = search_.isEmpty() ? tabsFor(page_) : std::vector<std::string>{};
        if(names != tabNames_)
        {
            tabNames_ = names; tabButtons_.clear();
            for(size_t i = 0; i < tabNames_.size(); ++i)
            {
                auto b = std::make_unique<PageButton>(tabNames_[i].empty() ? juce::String("General") : text(tabNames_[i]), true);
                b->onClick = [this, i] { showTab(i); };
                addAndMakeVisible(*b); tabButtons_.push_back(std::move(b));
            }
        }
        auto area = getLocalBounds();
        const int height = mode == 2 ? 44 : 30;
        if(!pageButtons_.empty() || search_.isVisible())
        {
            // The search and the pages down the left, the page shown marked like a selected row.
            auto list = area.removeFromLeft(mode == 2 ? 200 : 170);
            area.removeFromLeft(sectionGap(8));
            if(search_.isVisible()) { search_.setFont(juce::FontOptions(textSize(TextRole::Body, mode))); search_.setBounds(list.removeFromTop(height)); list.removeFromTop(8); }
            for(size_t i = 0; i < pageButtons_.size(); ++i)
            {
                auto& b = static_cast<PageButton&>(*pageButtons_[i]);
                b.shown = i == page_ && search_.isEmpty(); b.repaint();
                b.setBounds(list.removeFromTop(height)); list.removeFromTop(2);
            }
        }
        if(!tabButtons_.empty())
        {
            auto strip = area.removeFromTop(height);
            area.removeFromTop(8);
            const juce::Font font(juce::FontOptions(textSize(TextRole::Button, mode), juce::Font::bold));
            for(size_t i = 0; i < tabButtons_.size(); ++i)
            {
                auto& b = static_cast<PageButton&>(*tabButtons_[i]);
                b.shown = i == tab_; b.repaint();
                b.setBounds(strip.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(font, b.getName()) + 28)); strip.removeFromLeft(2);
            }
        }
        viewport_.setBounds(area);
        content_->setSize(area.getWidth() - viewport_.getScrollBarThickness(), content_->getHeight());
        build();
    }

    void ParameterGrid::build()
    {
        controls_.clear(); tiles_.clear(); headings_.clear(); panels_.clear(); links_.clear(); linkedRuns_ = 0;
        if(pages_.empty()) return;
        const int mode = std::max(1, desktopMode(*this)), width = content_->getWidth();
        const int gap = sectionGap(6), columns = std::clamp(width / 210, 2, 6);
        const int tileWidth = (width - gap * (columns - 1)) / columns, tileHeight = mode == 2 ? 78 : 64;
        // What shows, each group under its heading: a search's matches from every page by their
        // whole module path; else the page's (or its tab's) parameters by the rest of the path.
        std::vector<std::pair<std::string, std::vector<size_t>>> groups;
        auto add = [&](const std::string& heading, size_t i) {
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == heading; });
            if(it == groups.end()) { groups.push_back({heading, {}}); it = groups.end() - 1; }
            it->second.push_back(i);
        };
        if(!search_.isEmpty())
        {
            const auto query = search_.getText().trim().toLowerCase();
            for(size_t i = 0; i < params_.size(); ++i)
                if(text(params_[i].name).toLowerCase().contains(query) || text(params_[i].module).toLowerCase().contains(query))
                    add(params_[i].module.empty() ? std::string("Main") : params_[i].module, i);
        }
        else
            for(const auto i : pages_[page_].params)
            {
                const auto& m = params_[i].module;
                if(tabNames_.empty()) add(after(m, 1), i);
                else if(level(m, 1) == tabNames_[tab_]) add(after(m, 2), i);
            }
        // A range's high end is drawn by its low end's tile; the rest by the plugin's priorities,
        // numbered first, then the others in its order.
        for(auto& [heading, members] : groups)
        {
            std::erase_if(members, [&](size_t i) { return std::any_of(params_.begin(), params_.end(), [&](const Engine::ParamInfo& q) { return q.widget == OSCAIRN_WIDGET_RANGE && q.rangeHigh == params_[i].id && &q != &params_[i]; }); });
            std::stable_sort(members.begin(), members.end(), [&](size_t a, size_t b) {
                const auto pa = params_[a].priority ? params_[a].priority : UINT32_MAX, pb = params_[b].priority ? params_[b].priority : UINT32_MAX;
                return pa < pb;
            });
        }
        std::erase_if(groups, [](const auto& g) { return g.second.empty(); });
        // Knobs want room: a page that has any gets taller tiles.
        const bool knobs = std::any_of(groups.begin(), groups.end(), [&](const auto& g) {
            return std::any_of(g.second.begin(), g.second.end(), [&](size_t i) { return params_[i].widget == OSCAIRN_WIDGET_KNOB; });
        });
        const int height = knobs ? tileHeight + (mode == 2 ? 36 : 30) : tileHeight;
        int y = 0;
        for(const auto& [heading, members] : groups)
        {
            if(!heading.empty()) { headings_.push_back({text(heading).replace("/", juce::String::fromUTF8(" › ")), {0, y, width, 28}}); y += 30; }
            // An LFO or an envelope among them: drawn first, across the row.
            {
                std::vector<std::string> names;
                for(const auto i : members) names.push_back(params_[i].name);
                if(auto shape = filterOf(names); !shape.empty())
                {
                    const juce::Rectangle<int> bounds(0, y, width, mode == 2 ? 160 : 130);
                    auto view = std::make_unique<Filter>(*this, shape, members);
                    view->setBounds(bounds); content_->addAndMakeVisible(*view); controls_.push_back(std::move(view));
                    panels_.push_back(bounds);
                    y += bounds.getHeight() + gap;
                }
                if(auto shape = lfoOf(names); !shape.empty())
                {
                    const juce::Rectangle<int> bounds(0, y, width, mode == 2 ? 140 : 110);
                    auto view = std::make_unique<Lfo>(*this, std::move(shape), members);
                    view->setBounds(bounds); content_->addAndMakeVisible(*view); controls_.push_back(std::move(view));
                    panels_.push_back(bounds);
                    y += bounds.getHeight() + gap;
                }
                if(auto shape = envelopeOf(names); !shape.empty())
                {
                    const juce::Rectangle<int> bounds(0, y, width, mode == 2 ? 180 : 150);
                    auto view = std::make_unique<Envelope>(*this, std::move(shape), members);
                    view->setBounds(bounds); content_->addAndMakeVisible(*view); controls_.push_back(std::move(view));
                    panels_.push_back(bounds);
                    y += bounds.getHeight() + gap;
                }
            }
            // Runs: a linked group's tiles together (the plugin's groups, then a recognised
            // envelope, LFO or filter), where its first tile would be; other tiles alone.
            std::vector<std::vector<size_t>> runs;
            {
                std::vector<std::vector<size_t>> linked;
                std::map<uint32_t, size_t> byGroup;
                for(const auto i : members)
                    if(const auto group = params_[i].group)
                    {
                        auto [at, added] = byGroup.try_emplace(group, linked.size());
                        if(added) linked.push_back({});
                        linked[at->second].push_back(i);
                    }
                std::vector<std::string> names;
                for(const auto i : members) names.push_back(params_[i].name);
                auto recognised = [&](const std::vector<int>& order) {
                    std::vector<size_t> run;
                    for(const int k : order) if(params_[members[size_t(k)]].group == 0) run.push_back(members[size_t(k)]);
                    if(run.size() > 1) linked.push_back(run);
                };
                if(auto e = envelopeOf(names); !e.empty()) recognised(linkedOrder(e));
                if(auto l = lfoOf(names); !l.empty()) recognised(linkedOrder(l));
                if(auto f = filterOf(names); !f.empty()) recognised(linkedOrder(f));
                std::vector<bool> done(params_.size());
                for(const auto i : members)
                {
                    if(done[i]) continue;
                    const auto run = std::find_if(linked.begin(), linked.end(), [&](const auto& r) { return std::find(r.begin(), r.end(), i) != r.end(); });
                    std::vector<size_t> next;
                    for(const auto j : run == linked.end() ? std::vector<size_t>{i} : *run) if(!done[j]) { next.push_back(j); done[j] = true; }
                    if(!next.empty()) runs.push_back(next);
                }
            }
            int column = 0;
            std::vector<std::pair<size_t, juce::Rectangle<int>>> placed; // a tile's param and its cell
            std::vector<bool> inLink; // per placed tile
            for(const auto& run : runs)
            {
                const bool group = run.size() > 1;
                // A group that does not fit what is left of the row starts the next one; the tile
                // before it widens to the row's end, so the row has no hole.
                if(column && (column == columns || (group && column + int(run.size()) > columns)))
                {
                    if(column < columns && !inLink.back()) placed.back().second.setRight(width);
                    column = 0; y += height + gap;
                }
                juce::Rectangle<int> card;
                for(size_t k = 0; k < run.size(); ++k)
                {
                    // A group wider than the screen goes on in the next row, its rows touching.
                    if(column == columns) { column = 0; y += height + (group ? 0 : gap); }
                    juce::Rectangle<int> cell(column * (tileWidth + gap), y, tileWidth, height);
                    if(group)
                    {
                        if(k + 1 < run.size() && column + 1 < columns) cell.setWidth(tileWidth + gap); // touches the next
                        if(k + 1 == run.size() && run.size() > size_t(columns)) { cell.setRight(width); column = columns - 1; } // a clean rectangle; nothing after it in the row
                        card = card.isEmpty() ? cell : card.getUnion(cell);
                    }
                    placed.push_back({run[k], cell}); inLink.push_back(group);
                    ++column;
                }
                if(group) { links_.push_back(card); ++linkedRuns_; }
            }
            for(const auto& [param, cell] : placed)
            {
                Tile tile{param, cell};
                tile.linked = std::any_of(links_.begin(), links_.end(), [&](const juce::Rectangle<int>& l) { return l.contains(cell); });
                const auto& p = params_[tile.param];
                auto inner = tile.bounds.reduced(10, 8);
                auto top = inner.removeFromTop(18);
                auto name = std::make_unique<juce::Label>(juce::String(), text(p.name));
                name->setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold)); name->setColour(juce::Label::textColourId, ink(Ink::Muted));
                name->setBorderSize({}); name->setMinimumHorizontalScale(1.f); name->setTooltip(text(p.name));
                auto value = std::make_unique<juce::Label>();
                value->setFont(juce::FontOptions(textSize(TextRole::Small, mode))); value->setJustificationType(juce::Justification::centredRight); value->setBorderSize({});
                value->setBounds(top.removeFromRight(std::min(90, top.getWidth() / 2))); name->setBounds(top);
                tile.value = value.get();
                content_->addAndMakeVisible(*name); content_->addAndMakeVisible(*value);
                controls_.push_back(std::move(name)); controls_.push_back(std::move(value));
                inner.removeFromTop(4);
                const auto id = p.id; const auto node = name_;
                const int count = values(p);
                if((p.flags & CLAP_PARAM_IS_READONLY) == 0)
                {
                    // The control: the plugin's hint where it gave one (oscairn.ui, checked by the
                    // host), else by its values: a few as buttons (Off and On a switch), up to 64
                    // as a list, the rest a slider.
                    enum class Control { Buttons, Switch, List, Slider, Knob, Range } control;
                    juce::StringArray names;
                    auto named = [&] { if(names.isEmpty()) for(int v = 0; v < count; ++v) names.add(text(engine_.paramText(node, id, p.minimum + v))); };
                    switch(p.widget)
                    {
                    case OSCAIRN_WIDGET_KNOB: control = Control::Knob; break;
                    case OSCAIRN_WIDGET_SLIDER: control = Control::Slider; break;
                    case OSCAIRN_WIDGET_BUTTONS: control = count <= 8 ? Control::Buttons : count <= maxListed ? Control::List : Control::Slider; break;
                    case OSCAIRN_WIDGET_SWITCH: control = Control::Switch; break;
                    case OSCAIRN_WIDGET_LIST: control = count <= maxListed ? Control::List : Control::Slider; break;
                    case OSCAIRN_WIDGET_RANGE: control = Control::Range; break;
                    default:
                        if(count >= 2 && count <= maxChoices) named();
                        control = count >= 2 && count <= maxChoices && !(count == 2 && onOff(names[0], names[1])) ? Control::Buttons
                                : count == 2 ? Control::Switch : count > 2 && count <= maxListed ? Control::List : Control::Slider;
                    }
                    if(control == Control::Buttons) named();
                    const auto high = std::find_if(params_.begin(), params_.end(), [&](const Engine::ParamInfo& q) { return q.id == p.rangeHigh; });
                    if(control == Control::Range && high == params_.end()) control = Control::Slider;
                    if(control == Control::Range)
                    {
                        // One two-sided slider sets both ends (the high end has no tile of its own).
                        auto s = std::make_unique<juce::Slider>(juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox);
                        s->setRange(std::min(p.minimum, high->minimum), std::max(p.maximum, high->maximum), p.stepped ? 1.0 : 0.0);
                        const auto highId = high->id;
                        s->onValueChange = [this, raw = s.get(), node, id, highId] { engine_.setParam(node, id, raw->getMinValue()); engine_.setParam(node, highId, raw->getMaxValue()); };
                        s->onDragStart = [this, node, id, highId] { engine_.paramGesture(node, id, true); engine_.paramGesture(node, highId, true); };
                        s->onDragEnd = [this, node, id, highId] { engine_.paramGesture(node, id, false); engine_.paramGesture(node, highId, false); };
                        s->setBounds(inner.withHeight(std::min(inner.getHeight(), 24)));
                        tile.range = s.get(); tile.high = size_t(high - params_.begin());
                        content_->addAndMakeVisible(*s); controls_.push_back(std::move(s));
                    }
                    else if(control == Control::Knob)
                    {
                        auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
                        s->setRange(p.minimum, p.maximum, p.stepped ? 1.0 : 0.0);
                        s->setDoubleClickReturnValue(true, p.defaultValue);
                        s->onValueChange = [this, raw = s.get(), node, id] { engine_.setParam(node, id, raw->getValue()); };
                        s->onDragStart = [this, node, id] { engine_.paramGesture(node, id, true); };
                        s->onDragEnd = [this, node, id] { engine_.paramGesture(node, id, false); };
                        const int side = std::min(inner.getHeight(), 64);
                        s->setBounds(inner.withSizeKeepingCentre(side, side));
                        tile.slider = s.get(); content_->addAndMakeVisible(*s); controls_.push_back(std::move(s));
                    }
                    else if(control == Control::Buttons)
                    {
                        auto choices = std::make_unique<Choices>(names);
                        choices->onChoose = [this, node, id, p](int v) { engine_.setParam(node, id, p.minimum + v); };
                        choices->setBounds(inner.withSizeKeepingCentre(inner.getWidth(), std::min(inner.getHeight(), 28)));
                        tile.choices = choices.get(); content_->addAndMakeVisible(*choices); controls_.push_back(std::move(choices));
                    }
                    else if(control == Control::Switch)
                    {
                        auto b = std::make_unique<juce::TextButton>();
                        b->setClickingTogglesState(true);
                        b->onClick = [this, raw = b.get(), node, id, p] { engine_.setParam(node, id, raw->getToggleState() ? p.maximum : p.minimum); };
                        b->setBounds(inner.withHeight(std::min(inner.getHeight(), 28)).withWidth(std::min(inner.getWidth(), 110)));
                        tile.toggle = b.get(); content_->addAndMakeVisible(*b); controls_.push_back(std::move(b));
                    }
                    else if(control == Control::List)
                    {
                        auto list = std::make_unique<juce::ComboBox>(text(p.name));
                        for(int v = 0; v < count; ++v) list->addItem(text(engine_.paramText(node, id, p.minimum + v)), v + 1);
                        list->onChange = [this, raw = list.get(), node, id, p] { if(raw->getSelectedId() > 0) engine_.setParam(node, id, p.minimum + raw->getSelectedId() - 1); };
                        list->setBounds(inner.withHeight(std::min(inner.getHeight(), 28)));
                        tile.list = list.get(); content_->addAndMakeVisible(*list); controls_.push_back(std::move(list));
                    }
                    else
                    {
                        auto s = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
                        s->setRange(p.minimum, p.maximum, p.stepped ? 1.0 : 0.0);
                        s->setDoubleClickReturnValue(true, p.defaultValue);
                        // Centred on zero (pan, detune): a symmetric range whose default is zero.
                        if(p.minimum < 0 && p.maximum > 0 && std::abs(p.minimum + p.maximum) < 1e-9 * (p.maximum - p.minimum) && p.defaultValue == 0) s->getProperties().set("bipolar", true);
                        s->onValueChange = [this, raw = s.get(), node, id] { engine_.setParam(node, id, raw->getValue()); };
                        s->onDragStart = [this, node, id] { engine_.paramGesture(node, id, true); };
                        s->onDragEnd = [this, node, id] { engine_.paramGesture(node, id, false); };
                        s->setBounds(inner.withHeight(std::min(inner.getHeight(), 24)));
                        tile.slider = s.get(); content_->addAndMakeVisible(*s); controls_.push_back(std::move(s));
                    }
                }
                tiles_.push_back(tile);
            }
            y += height + gap + 6;
        }
        content_->setSize(width, std::max(y, 1));
        stale_ = !engine_.controlReady(); // the engine answers value texts only when it is free
        for(auto& t : tiles_) show(t, engine_.param(name_, params_[t.param].id));
        content_->repaint();
    }

    void ParameterGrid::show(Tile& t, double value)
    {
        const auto& p = params_[t.param];
        t.shown = value;
        const auto shown = text(engine_.paramText(name_, p.id, value));
        t.value->setText(t.list || t.toggle || t.choices ? juce::String() : shown, juce::dontSendNotification);
        if(t.choices) { t.choices->chosen = int(std::lround(value - p.minimum)); t.choices->repaint(); }
        if(t.slider && !t.slider->isMouseButtonDown()) t.slider->setValue(value, juce::dontSendNotification);
        if(t.range)
        {
            const auto& high = params_[t.high];
            t.shownHigh = engine_.param(name_, high.id);
            t.value->setText(shown + juce::String::fromUTF8(" – ") + text(engine_.paramText(name_, high.id, t.shownHigh)), juce::dontSendNotification);
            if(!t.range->isMouseButtonDown()) t.range->setMinAndMaxValues(value, t.shownHigh, juce::dontSendNotification);
        }
        if(t.list) t.list->setSelectedId(int(std::lround(value - p.minimum)) + 1, juce::dontSendNotification);
        if(t.toggle) { t.toggle->setToggleState(value > p.minimum, juce::dontSendNotification); t.toggle->setButtonText(shown); }
    }

    void ParameterGrid::timerCallback()
    {
        if(stale_ && engine_.controlReady()) { refresh(); return; }
        if(!panels_.empty()) content_->repaint(); // envelopes follow their parameters too
        // Values change from automation, scenes, controls and other clients: follow them.
        for(auto& t : tiles_)
            if(const double v = engine_.param(name_, params_[t.param].id); v != t.shown || (t.range && engine_.param(name_, params_[t.high].id) != t.shownHigh)) show(t, v);
    }
}

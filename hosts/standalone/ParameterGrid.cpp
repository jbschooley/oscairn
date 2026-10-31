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
        constexpr int maxListed = 32; // more values than this get a slider
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
        class PageButton : public juce::Button
        {
        public:
            explicit PageButton(const juce::String& name) : juce::Button(name) {}
            bool shown = false;
            void paintButton(juce::Graphics& g, bool hover, bool) override
            {
                const auto r = getLocalBounds().toFloat();
                if(shown || hover) { g.setColour(shown ? ink(Ink::Raised) : ink(Ink::Raised).withAlpha(.5f)); g.fillRoundedRectangle(r, std::min(themeData().radius, r.getHeight() / 2)); }
                if(shown) { g.setColour(ink(Ink::Accent)); g.fillRect(r.withWidth(3.f)); }
                g.setColour(shown ? ink(Ink::Text) : ink(Ink::Muted));
                g.setFont(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this))), shown ? juce::Font::bold : juce::Font::plain));
                g.drawText(getName(), r.withTrimmedLeft(12), juce::Justification::centredLeft, true);
            }
        };
    }

    /// The scrolling area: the tiles' backgrounds and the headings, under their controls.
    class ParameterGrid::Content : public juce::Component
    {
    public:
        explicit Content(ParameterGrid& grid) : grid_(grid) {}
        void paint(juce::Graphics& g) override
        {
            const int mode = std::max(1, desktopMode(*this));
            for(const auto& t : grid_.tiles_) drawSection(g, t.bounds, ink(Ink::Surface));
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold));
            for(const auto& h : grid_.headings_) g.drawText(h.text.toUpperCase(), h.bounds, juce::Justification::bottomLeft, true);
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
                auto b = std::make_unique<PageButton>(text(pages_[i].name));
                b->onClick = [this, i] { showPage(i); };
                addAndMakeVisible(*b); pageButtons_.push_back(std::move(b));
            }
        viewport_.setViewedComponent(content_.get(), false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        startTimer(60);
    }

    ParameterGrid::~ParameterGrid() { stopTimer(); }

    void ParameterGrid::showPage(size_t page)
    {
        if(page >= pages_.size()) return;
        page_ = page; viewport_.setViewPosition(0, 0);
        build(); resized();
    }

    void ParameterGrid::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        if(pages_.empty())
        {
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this)))));
            g.drawText(text(name_) + " has no parameters to show", getLocalBounds(), juce::Justification::centred);
        }
    }

    void ParameterGrid::resized()
    {
        const int mode = std::max(1, desktopMode(*this));
        auto area = getLocalBounds();
        if(!pageButtons_.empty())
        {
            // The pages down the left, the one shown marked like a selected row.
            auto list = area.removeFromLeft(mode == 2 ? 200 : 170);
            area.removeFromLeft(sectionGap(8));
            const int height = mode == 2 ? 44 : 30;
            for(size_t i = 0; i < pageButtons_.size(); ++i)
            {
                auto& b = static_cast<PageButton&>(*pageButtons_[i]);
                b.shown = i == page_; b.repaint();
                b.setBounds(list.removeFromTop(height)); list.removeFromTop(2);
            }
        }
        viewport_.setBounds(area);
        const int width = area.getWidth() - viewport_.getScrollBarThickness();
        if(width != content_->getWidth() || tiles_.empty()) { content_->setSize(width, content_->getHeight()); build(); }
    }

    void ParameterGrid::build()
    {
        controls_.clear(); tiles_.clear(); headings_.clear();
        if(pages_.empty()) return;
        const int mode = std::max(1, desktopMode(*this)), width = content_->getWidth();
        const int gap = sectionGap(6), columns = std::clamp(width / 210, 2, 6);
        const int tileWidth = (width - gap * (columns - 1)) / columns, tileHeight = mode == 2 ? 78 : 64;
        // The page's parameters by the rest of their module path, each group under its heading.
        std::vector<std::pair<std::string, std::vector<size_t>>> groups;
        for(const auto i : pages_[page_].params)
        {
            const auto& m = params_[i].module;
            const auto slash = m.find('/');
            const auto rest = slash == std::string::npos ? std::string() : m.substr(slash + 1);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == rest; });
            if(it == groups.end()) { groups.push_back({rest, {}}); it = groups.end() - 1; }
            it->second.push_back(i);
        }
        int y = 0;
        for(const auto& [heading, members] : groups)
        {
            if(!heading.empty()) { headings_.push_back({text(heading), {0, y, width, 28}}); y += 30; }
            for(size_t k = 0; k < members.size(); ++k)
            {
                const auto column = int(k % size_t(columns));
                if(k && !column) y += tileHeight + gap;
                Tile tile{members[k], {column * (tileWidth + gap), y, tileWidth, tileHeight}};
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
                    if(count == 2)
                    {
                        auto b = std::make_unique<juce::TextButton>();
                        b->setClickingTogglesState(true);
                        b->onClick = [this, raw = b.get(), node, id, p] { engine_.setParam(node, id, raw->getToggleState() ? p.maximum : p.minimum); };
                        b->setBounds(inner.withHeight(std::min(inner.getHeight(), 28)).withWidth(std::min(inner.getWidth(), 110)));
                        tile.toggle = b.get(); content_->addAndMakeVisible(*b); controls_.push_back(std::move(b));
                    }
                    else if(count > 2 && count <= maxListed)
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
            y += tileHeight + gap + 6;
        }
        content_->setSize(width, std::max(y, 1));
        for(auto& t : tiles_) show(t, engine_.param(name_, params_[t.param].id));
        content_->repaint();
    }

    void ParameterGrid::show(Tile& t, double value)
    {
        const auto& p = params_[t.param];
        t.shown = value;
        const auto shown = text(engine_.paramText(name_, p.id, value));
        t.value->setText(t.list || t.toggle ? juce::String() : shown, juce::dontSendNotification);
        if(t.slider && !t.slider->isMouseButtonDown()) t.slider->setValue(value, juce::dontSendNotification);
        if(t.list) t.list->setSelectedId(int(std::lround(value - p.minimum)) + 1, juce::dontSendNotification);
        if(t.toggle) { t.toggle->setToggleState(value > p.minimum, juce::dontSendNotification); t.toggle->setButtonText(shown); }
    }

    void ParameterGrid::timerCallback()
    {
        // Values change from automation, scenes, controls and other clients: follow them.
        for(auto& t : tiles_)
            if(const double v = engine_.param(name_, params_[t.param].id); v != t.shown) show(t, v);
    }
}

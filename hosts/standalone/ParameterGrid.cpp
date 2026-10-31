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
        controls_.clear(); tiles_.clear(); headings_.clear();
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
        int y = 0;
        for(const auto& [heading, members] : groups)
        {
            if(!heading.empty()) { headings_.push_back({text(heading).replace("/", juce::String::fromUTF8(" › ")), {0, y, width, 28}}); y += 30; }
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
        stale_ = !engine_.controlReady(); // the engine answers value texts only when it is free
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
        if(stale_ && engine_.controlReady()) { refresh(); return; }
        // Values change from automation, scenes, controls and other clients: follow them.
        for(auto& t : tiles_)
            if(const double v = engine_.param(name_, params_[t.param].id); v != t.shown) show(t, v);
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CompactEditor.h"
#include "PartsModel.h"
#include "Theme.h"

#include <clap/ext/params.h>
#include <cmath>

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        void collect(const std::vector<PartBox>& chain, std::vector<NodeId>& nodes)
        {
            for(const auto& box : chain)
            {
                if(box.split) { for(const auto& lane : box.lanes) collect(lane.boxes, nodes); }
                else if(box.node && std::find(nodes.begin(), nodes.end(), box.node) == nodes.end()) nodes.push_back(box.node);
            }
        }
    }

    CompactEditor::CompactEditor(Engine& engine) : engine_(engine)
    {
        plugins_ = plugins();
        startTimer(100);
    }

    CompactEditor::~CompactEditor() { stopTimer(); }

    std::vector<CompactEditor::Plugin> CompactEditor::plugins() const
    {
        const auto graph = engine_.graph();
        const auto parts = readParts(graph);
        std::vector<NodeId> nodes;
        for(const auto* rows : {&parts.parts, &parts.buses})
            for(const auto& part : *rows) collect(part.chain, nodes);
        std::vector<Plugin> out;
        for(const auto node : nodes)
            if(const auto name = graph.names.find(node); name != graph.names.end()) out.push_back({node, name->second});
        return out;
    }

    void CompactEditor::edit(NodeId node, const std::string& name)
    {
        plugin_ = {node, name};
        params_.clear();
        try { params_ = editorParameters(engine_.parameters(node)); } catch(const std::exception&) {}
        pages_ = editorPages(params_);
        level_ = Level::Pages; chosen_ = 0;
        if(pages_.size() == 1) open(0);
        repaint();
    }

    void CompactEditor::open(size_t page)
    {
        if(page >= pages_.size()) return;
        page_ = page; first_ = 0;
        shown_ = pages_[page].params;
        byPriority(params_, shown_);
        level_ = Level::Parameters;
        repaint();
    }

    const Engine::ParamInfo* CompactEditor::onEncoder(int encoder) const
    {
        const auto at = first_ + size_t(encoder);
        return level_ == Level::Parameters && encoder >= 0 && encoder < encoders && at < shown_.size() ? &params_[shown_[at]] : nullptr;
    }

    std::vector<std::string> CompactEditor::listShown() const
    {
        std::vector<std::string> items;
        if(level_ == Level::Plugins) for(const auto& p : plugins_) items.push_back(p.name);
        if(level_ == Level::Pages) for(const auto& p : pages_) items.push_back(p.name);
        return items;
    }

    std::vector<std::string> CompactEditor::encoderNames() const
    {
        std::vector<std::string> names;
        for(int e = 0; e < encoders; ++e) if(const auto* p = onEncoder(e)) names.push_back(p->name);
        return names;
    }

    std::string CompactEditor::encoderParam(int encoder, uint32_t& id) const
    {
        const auto* p = onEncoder(encoder);
        if(!p) return {};
        id = p->id;
        return plugin_.name;
    }

    void CompactEditor::turn(int encoder, int steps)
    {
        if(level_ != Level::Parameters)
        {
            const auto count = listShown().size();
            if(count) chosen_ = size_t(std::clamp<long>(long(chosen_) + steps, 0, long(count) - 1));
            repaint();
            return;
        }
        const auto* p = onEncoder(encoder);
        if(!p || (p->flags & CLAP_PARAM_IS_READONLY)) return;
        // A value per detent when stepped, else a hundredth of the range.
        const double step = p->stepped ? 1.0 : (p->maximum - p->minimum) / 100;
        engine_.setParam(plugin_.name, p->id, std::clamp(engine_.param(plugin_.name, p->id) + step * steps, p->minimum, p->maximum));
        repaint();
    }

    void CompactEditor::press(int encoder)
    {
        if(encoder == 0)
        {
            // Back: from a page to its plugin's pages (or the plugins, when it has one page).
            if(level_ == Level::Parameters && pages_.size() > 1) { level_ = Level::Pages; chosen_ = page_; }
            else if(level_ != Level::Plugins) { level_ = Level::Plugins; plugins_ = plugins(); chosen_ = 0; }
        }
        else if(encoder == encoders - 1)
        {
            if(level_ == Level::Plugins && chosen_ < plugins_.size()) { const auto p = plugins_[chosen_]; edit(p.node, p.name); }
            else if(level_ == Level::Pages) open(chosen_);
            else if(level_ == Level::Parameters && shown_.size() > size_t(encoders)) first_ = first_ + encoders < shown_.size() ? first_ + encoders : 0; // the next four, then the first again
        }
        repaint();
    }

    void CompactEditor::timerCallback()
    {
        if(level_ == Level::Plugins)
        {
            auto now = plugins();
            const bool same = now.size() == plugins_.size() && std::equal(now.begin(), now.end(), plugins_.begin(), [](const Plugin& a, const Plugin& b) { return a.node == b.node && a.name == b.name; });
            if(!same) { plugins_ = std::move(now); chosen_ = std::min(chosen_, plugins_.empty() ? 0 : plugins_.size() - 1); }
        }
        else if(++ticks_ % 5 == 0)
            try
            {
                // A mode can show other controls: the same page again, or the pages when it is gone.
                if(auto now = editorParameters(engine_.parameters(plugin_.node)); !sameParameters(now, params_))
                {
                    const auto shown = level_ == Level::Parameters ? pages_[page_].name : std::string();
                    const auto first = first_;
                    params_ = std::move(now); pages_ = editorPages(params_);
                    level_ = Level::Pages; chosen_ = 0;
                    for(size_t i = 0; i < pages_.size(); ++i) if(pages_[i].name == shown) { open(i); first_ = first < shown_.size() ? first : 0; }
                    if(level_ == Level::Pages && pages_.size() == 1) open(0);
                }
            }
            catch(const std::exception&) {}
        repaint(); // values follow the engine
    }

    void CompactEditor::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        auto area = getLocalBounds();
        const float scale = float(getHeight()) / 240.f; // drawn for 320 × 240
        auto font = [&](float size, bool bold = false) { return juce::Font(juce::FontOptions(size * scale * themeData().fontScale, bold ? juce::Font::bold : juce::Font::plain)); };
        const int pad = juce::roundToInt(8 * scale);

        // The title: where the lists are, and on a page which of its parameters are shown.
        auto header = area.removeFromTop(juce::roundToInt(30 * scale));
        g.setColour(ink(Ink::Surface)); g.fillRect(header);
        juce::String title = level_ == Level::Plugins ? juce::String("Plugins") : text(plugin_.name);
        if(level_ == Level::Parameters && pages_.size() > 1) title << juce::String::fromUTF8(" › ") << text(pages_[page_].name);
        auto titleArea = header.reduced(pad, 0);
        if(level_ == Level::Parameters && shown_.size() > size_t(encoders))
        {
            const auto last = std::min(first_ + encoders, shown_.size());
            const auto count = juce::String(int(first_ + 1)) + juce::String::fromUTF8("–") + juce::String(int(last)) + " of " + juce::String(int(shown_.size()));
            g.setColour(ink(Ink::Muted));
            drawInked(g, count, titleArea.removeFromRight(juce::roundToInt(76 * scale)), font(12), juce::Justification::right);
        }
        g.setColour(ink(Ink::Text));
        drawInked(g, title, titleArea, font(14, true));

        // What pressing encoders 1 and 4 does, under the knobs they are.
        auto footer = area.removeFromBottom(juce::roundToInt(22 * scale));
        const bool back = level_ != Level::Plugins;
        const juce::String forward = level_ == Level::Parameters ? (shown_.size() > size_t(encoders) ? "More" : "") : listShown().empty() ? "" : "Open";
        g.setColour(ink(Ink::Muted));
        const int quarter = footer.getWidth() / encoders;
        if(back) drawInked(g, "Back", footer.withWidth(quarter), font(11), juce::Justification::horizontallyCentred);
        if(forward.isNotEmpty()) drawInked(g, forward, footer.withTrimmedLeft(footer.getWidth() - quarter), font(11), juce::Justification::horizontallyCentred);

        if(level_ != Level::Parameters)
        {
            const auto items = listShown();
            if(items.empty())
            {
                g.setColour(ink(Ink::Muted));
                drawInked(g, level_ == Level::Plugins ? "No plugins" : "No parameters", area, font(13), juce::Justification::horizontallyCentred);
                return;
            }
            // A row per item, scrolled to keep the choice in view.
            const int row = juce::roundToInt(26 * scale), rows = std::max(1, area.getHeight() / row);
            const size_t top = chosen_ >= size_t(rows) ? chosen_ - size_t(rows) + 1 : 0;
            for(size_t i = top; i < items.size() && i < top + size_t(rows); ++i)
            {
                const auto r = area.removeFromTop(row);
                if(i == chosen_) { g.setColour(ink(Ink::Accent)); g.fillRoundedRectangle(r.reduced(pad / 2, 1).toFloat(), themeData().radius); }
                g.setColour(i == chosen_ ? ink(Ink::Accent).contrasting() : ink(Ink::Text));
                drawInked(g, text(items[i]), r.reduced(pad, 0), font(13, i == chosen_));
            }
            return;
        }

        // A column per encoder: its parameter's name, value and position, as the knob below it.
        const int width = area.getWidth() / encoders;
        for(int e = 0; e < encoders; ++e)
        {
            auto cell = juce::Rectangle<int>(area.getX() + e * width, area.getY(), width, area.getHeight()).reduced(pad / 2);
            const auto* p = onEncoder(e);
            drawSection(g, cell, p ? ink(Ink::Surface) : ink(Ink::Background));
            if(!p) continue;
            cell = cell.reduced(pad / 2);
            const double value = engine_.param(plugin_.name, p->id);
            const float normal = p->maximum > p->minimum ? float(std::clamp((value - p->minimum) / (p->maximum - p->minimum), 0.0, 1.0)) : 0.f;
            // The name on up to three lines, then the value, then a bar for where it sits.
            auto name = cell.removeFromTop(juce::roundToInt(48 * scale));
            g.setColour(ink(Ink::Muted)); g.setFont(font(11, true));
            g.drawFittedText(text(p->name), name, juce::Justification::centredTop, 3, .85f);
            auto bar = cell.removeFromBottom(juce::roundToInt(6 * scale)).toFloat();
            g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(bar, bar.getHeight() / 2);
            g.setColour((p->flags & CLAP_PARAM_IS_READONLY) ? ink(Ink::Muted) : ink(Ink::Accent));
            g.fillRoundedRectangle(bar.withWidth(std::max(bar.getHeight(), bar.getWidth() * normal)), bar.getHeight() / 2);
            g.setColour(ink(Ink::Text));
            drawInked(g, text(engine_.paramText(plugin_.name, p->id, value)), cell, font(13, true), juce::Justification::horizontallyCentred);
        }
    }
}

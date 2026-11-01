// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ZoneEditor.h"
#include "Theme.h"
#include "oscairn/pads.h"

#include <cmath>

namespace oscairn::standalone
{
    namespace
    {
        constexpr int whiteKeys = 75; // in MIDI notes 0 to 127
        int whitesBefore(int key) { int n = 0; for(int k = 0; k < key; ++k) n += !ZoneEditor::black(k); return n; }
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
    }

    void ZoneEditor::set(int lowKey, int highKey, int lowVelocity, int highVelocity)
    {
        if(drag_ != Drag::None) return;
        lowKey_ = lowKey; highKey_ = highKey; lowVelocity_ = lowVelocity; highVelocity_ = highVelocity;
        repaint();
    }

    juce::Rectangle<float> ZoneEditor::keys() const
    {
        auto r = getLocalBounds().toFloat().withTrimmedTop(18);
        return onVelocities ? r.withTrimmedBottom(26) : r;
    }
    juce::Rectangle<float> ZoneEditor::velocities() const { return getLocalBounds().toFloat().removeFromBottom(18).withTrimmedTop(4); }

    juce::Rectangle<float> ZoneEditor::keyBounds(int key) const
    {
        const auto area = keys();
        const float w = area.getWidth() / whiteKeys;
        if(!black(key)) return {area.getX() + whitesBefore(key) * w, area.getY(), w, area.getHeight()};
        return {area.getX() + whitesBefore(key + 1) * w - w * .3f, area.getY(), w * .6f, area.getHeight() * .6f};
    }

    int ZoneEditor::keyAt(float x) const
    {
        const auto area = keys();
        const float w = area.getWidth() / whiteKeys;
        const int white = std::clamp(int((x - area.getX()) / w), 0, whiteKeys - 1);
        for(int k = 0, n = 0; k < 128; ++k) if(!black(k) && n++ == white) return k;
        return 127;
    }

    int ZoneEditor::velocityAt(float x) const
    {
        const auto area = velocities();
        return std::clamp(int(std::lround((x - area.getX()) / area.getWidth() * 127)), 0, 127);
    }

    void ZoneEditor::paint(juce::Graphics& g)
    {
        const int mode = std::max(1, desktopMode(*this));
        const auto small = textSize(TextRole::Small, mode);
        // The range in words above the keys.
        g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(small, juce::Font::bold));
        juce::String range = lowKey_ == 0 && highKey_ == 127 ? juce::String("All keys") : text(noteName(lowKey_)) + juce::String::fromUTF8(" – ") + text(noteName(highKey_));
        if(onVelocities) range << juce::String::fromUTF8("   ·   velocity ") << lowVelocity_ << juce::String::fromUTF8("–") << highVelocity_;
        g.drawText(range, getLocalBounds().removeFromTop(16), juce::Justification::centredLeft);
        // White keys, then black ones over them; keys outside the range dimmed.
        const auto zone = keyBounds(lowKey_).getUnion(keyBounds(highKey_)).withY(keys().getY()).withHeight(keys().getHeight());
        for(const bool blacks : {false, true})
            for(int k = 0; k < 128; ++k)
            {
                if(black(k) != blacks) continue;
                const auto r = keyBounds(k);
                const bool in = k >= lowKey_ && k <= highKey_;
                const auto color = blacks ? juce::Colour(0xff1a1d22) : juce::Colour(0xffe9edf2);
                g.setColour(in ? color : color.interpolatedWith(ink(Ink::Background), .6f));
                g.fillRect(blacks ? r : r.reduced(.5f, 0));
                // Each C named, from the key's left edge so the first stays clear of the frame.
                if(k % 12 == 0 && !blacks) { g.setColour(juce::Colours::black.withAlpha(.45f)); g.setFont(juce::FontOptions(9.f)); g.drawText(text(noteName(k)), r.withTrimmedTop(r.getHeight() - 12).withTrimmedLeft(2).withWidth(std::max(r.getWidth(), 24.f)), juce::Justification::centredLeft, false); }
            }
        g.setColour(ink(Ink::Accent)); g.drawRect(zone, 2.f);
        for(const float x : {zone.getX(), zone.getRight()}) g.fillRect(juce::Rectangle<float>(4, zone.getHeight() + 6).withCentre({x, zone.getCentreY()}));
        if(!onVelocities) return;
        // The velocities: a track with the range lit, a handle at each end.
        const auto v = velocities();
        g.setColour(ink(Ink::Border).withAlpha(.45f)); g.fillRoundedRectangle(v.withSizeKeepingCentre(v.getWidth(), 6), 3);
        const float a = v.getX() + v.getWidth() * lowVelocity_ / 127.f, b = v.getX() + v.getWidth() * highVelocity_ / 127.f;
        g.setColour(ink(Ink::Accent)); g.fillRoundedRectangle(juce::Rectangle<float>(a, v.getCentreY() - 3, b - a, 6), 3);
        for(const float x : {a, b}) g.fillRect(juce::Rectangle<float>(4, v.getHeight()).withCentre({x, v.getCentreY()}));
    }

    void ZoneEditor::mouseDown(const juce::MouseEvent& e)
    {
        dragVelocity_ = onVelocities && e.position.y >= velocities().getY() - 2;
        const float low = dragVelocity_ ? velocities().getX() + velocities().getWidth() * lowVelocity_ / 127.f : keyBounds(lowKey_).getX();
        const float high = dragVelocity_ ? velocities().getX() + velocities().getWidth() * highVelocity_ / 127.f : keyBounds(highKey_).getRight();
        const float x = e.position.x;
        if(std::abs(x - low) <= 8 && std::abs(x - low) <= std::abs(x - high)) drag_ = Drag::Low;
        else if(std::abs(x - high) <= 8) drag_ = Drag::High;
        else if(x > low && x < high) drag_ = Drag::Both;
        else drag_ = x < low ? Drag::Low : Drag::High;
        grabbed_ = dragVelocity_ ? velocityAt(x) : keyAt(x);
        startLow_ = dragVelocity_ ? lowVelocity_ : lowKey_; startHigh_ = dragVelocity_ ? highVelocity_ : highKey_;
        mouseDrag(e);
    }

    void ZoneEditor::mouseDrag(const juce::MouseEvent& e)
    {
        if(drag_ == Drag::None) return;
        const int at = dragVelocity_ ? velocityAt(e.position.x) : keyAt(e.position.x);
        int low = startLow_, high = startHigh_;
        if(drag_ == Drag::Low) low = std::min(at, high);
        else if(drag_ == Drag::High) high = std::max(at, low);
        else { const int shift = std::clamp(at - grabbed_, -startLow_, 127 - startHigh_); low += shift; high += shift; }
        (dragVelocity_ ? lowVelocity_ : lowKey_) = low; (dragVelocity_ ? highVelocity_ : highKey_) = high;
        repaint();
    }

    void ZoneEditor::mouseUp(const juce::MouseEvent&)
    {
        if(drag_ == Drag::None) return;
        drag_ = Drag::None;
        if(dragVelocity_) { if(onVelocities) onVelocities(lowVelocity_, highVelocity_); }
        else if(onKeys) onKeys(lowKey_, highKey_);
    }

    void ZoneEditor::mouseDoubleClick(const juce::MouseEvent& e)
    {
        drag_ = Drag::None;
        if(onVelocities && e.position.y >= velocities().getY() - 2) { lowVelocity_ = 0; highVelocity_ = 127; onVelocities(0, 127); }
        else { lowKey_ = 0; highKey_ = 127; if(onKeys) onKeys(0, 127); }
        repaint();
    }
}

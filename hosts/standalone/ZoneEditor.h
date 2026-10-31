// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace oscairn::standalone
{
    /// A key range over the whole MIDI keyboard, and a velocity range under it when there is
    /// one (RFC 0019's key and velocity map). Drag an end of a range to move it, its middle to
    /// move the whole range, or anywhere else to bring the nearer end there; double-click for
    /// the full range. A change is reported when the drag ends, so it is one edit.
    class ZoneEditor : public juce::Component
    {
    public:
        std::function<void(int low, int high)> onKeys, onVelocities; // no onVelocities: keys only
        void set(int lowKey, int highKey, int lowVelocity = 0, int highVelocity = 127); // ignored while dragging

        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;

        static bool black(int key) { const int n = key % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

    private:
        juce::Rectangle<float> keys() const;
        juce::Rectangle<float> velocities() const;
        juce::Rectangle<float> keyBounds(int key) const;
        int keyAt(float x) const;
        int velocityAt(float x) const;

        int lowKey_ = 0, highKey_ = 127, lowVelocity_ = 0, highVelocity_ = 127;
        enum class Drag { None, Low, High, Both };
        Drag drag_ = Drag::None;
        bool dragVelocity_ = false;
        int grabbed_ = 0, startLow_ = 0, startHigh_ = 0; // where a whole-range drag began
    };
}

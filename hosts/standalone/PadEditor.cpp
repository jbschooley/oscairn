// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PadEditor.h"
#include "Theme.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String name(int note) { return juce::String::fromUTF8(noteName(note).c_str()) + " (" + juce::String(note) + ")"; }
    }

    PadEditor::PadEditor(Engine& engine) : engine_(engine), map_(engine.padMap())
    {
        for(auto* c : std::initializer_list<juce::Component*>{&title_, &padLabel_, &noteLabel_, &channelLabel_, &velocityLabel_, &pressureLabel_, &fillLabel_, &done_, &noteDown_, &noteUp_,
                                                             &octaveDown_, &octaveUp_, &setAll_, &note_, &channel_, &pressure_, &fillKind_, &root_, &scale_, &first_, &fixedVelocity_, &inKey_,
                                                             &velocity_, &fillBank_, &fillAll_})
            addAndMakeVisible(c);
        title_.setText("Pads", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(22.0f * themeData().fontScale, juce::Font::bold));
        noteLabel_.setText("Note", juce::dontSendNotification);
        channelLabel_.setText("MIDI channel", juce::dontSendNotification);
        velocityLabel_.setText("Velocity", juce::dontSendNotification);
        pressureLabel_.setText("Pressure", juce::dontSendNotification);
        fillLabel_.setText("Fill", juce::dontSendNotification);
        note_.setJustificationType(juce::Justification::centred);
        note_.setFont(juce::FontOptions(20.0f * themeData().fontScale, juce::Font::bold));

        for(size_t b = 0; b < banks_.size(); ++b)
        {
            addAndMakeVisible(banks_[b]);
            banks_[b].setButtonText(juce::String::charToString(juce::juce_wchar('A' + b)));
            banks_[b].setTooltip("Bank " + banks_[b].getButtonText());
            banks_[b].setClickingTogglesState(false);
            banks_[b].onClick = [this, b] { map_.bank = uint8_t(b); apply(); showPad(); repaint(); };
        }
        auto move = [this](int by) { selected().note = uint8_t(juce::jlimit(0, 127, selected().note + by)); apply(); showPad(); repaint(); };
        noteDown_.onClick = [move] { move(-1); };
        noteUp_.onClick = [move] { move(1); };
        octaveDown_.onClick = [move] { move(-12); };
        octaveUp_.onClick = [move] { move(12); };

        channel_.setName("Pad MIDI channel");
        channel_.addItem("Follow the track", 1);
        for(int c = 1; c <= 16; ++c) channel_.addItem("MIDI " + juce::String(c), c + 1);
        channel_.onChange = [this] { selected().channel = int8_t(channel_.getSelectedId() - 2); apply(); };

        velocity_.setName("Pad fixed velocity");
        velocity_.setRange(1, 127, 1);
        velocity_.setValue(100, juce::dontSendNotification);
        velocity_.setSliderStyle(juce::Slider::LinearHorizontal);
        velocity_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 24);
        fixedVelocity_.setTooltip("Off: as hard as the pad is played. On: always this velocity.");
        fixedVelocity_.onClick = [this] { selected().velocity = fixedVelocity_.getToggleState() ? velocity_.getValue() / 127.0 : 0; apply(); showPad(); };
        velocity_.onValueChange = [this] { if(fixedVelocity_.getToggleState()) { selected().velocity = velocity_.getValue() / 127.0; apply(); } };

        pressure_.setName("Pad pressure");
        pressure_.addItem("Poly aftertouch", 1);
        pressure_.addItem("Channel pressure", 2);
        pressure_.addItem("Off", 3);
        pressure_.onChange = [this] {
            const int id = pressure_.getSelectedId();
            selected().pressure = id == 1 ? PadAssignment::Pressure::Poly : id == 2 ? PadAssignment::Pressure::Channel : PadAssignment::Pressure::None;
            apply();
        };
        setAll_.setTooltip("Give every pad in this bank the selected pad's channel, velocity and pressure; notes stay.");
        setAll_.onClick = [this] {
            const auto from = selected();
            for(auto& a : map_.assignments[map_.bank]) { a.channel = from.channel; a.velocity = from.velocity; a.pressure = from.pressure; }
            apply();
        };

        fillKind_.setName("Fill with");
        fillKind_.addItem("Chromatic", 1);
        fillKind_.addItem("Drum kit (General MIDI)", 2);
        fillKind_.addItem("Rows continue", 3);
        fillKind_.addItem("Rows up a fourth", 4);
        fillKind_.addItem("Rows up a third", 5);
        fillKind_.setSelectedId(1, juce::dontSendNotification);
        first_.setName("First pad's note");
        for(int n = 0; n < 128; ++n) first_.addItem("From " + name(n), n + 1);
        first_.setSelectedId(37, juce::dontSendNotification);
        root_.setName("Key");
        for(int r = 0; r < 12; ++r) root_.addItem(juce::String::fromUTF8(noteName(60 + r).c_str()).dropLastCharacters(1), r + 1);
        root_.setSelectedId(1, juce::dontSendNotification);
        scale_.setName("Scale");
        for(size_t s = 0; s < padScales().size(); ++s) scale_.addItem(padScales()[s].first, int(s) + 1);
        scale_.setSelectedId(1, juce::dontSendNotification);
        inKey_.setToggleState(true, juce::dontSendNotification);
        fillKind_.onChange = [this] {
            const bool layout = fillKind_.getSelectedId() >= 3;
            first_.setVisible(fillKind_.getSelectedId() == 1);
            root_.setVisible(layout); scale_.setVisible(layout); inKey_.setVisible(layout);
        };
        fillKind_.onChange();
        fillBank_.onClick = [this] { fill(false); };
        fillAll_.onClick = [this] { fill(true); };
        done_.onClick = [this] { if(onDone) onDone(); };
        showPad();
    }

    void PadEditor::apply() { engine_.setPadMap(map_); }

    void PadEditor::showPad()
    {
        const auto& a = selected();
        padLabel_.setText("Pad " + juce::String(selected_ + 1) + ", bank " + juce::String::charToString(juce::juce_wchar('A' + map_.bank)), juce::dontSendNotification);
        note_.setText(name(a.note), juce::dontSendNotification);
        channel_.setSelectedId(a.channel + 2, juce::dontSendNotification);
        fixedVelocity_.setToggleState(a.velocity > 0, juce::dontSendNotification);
        if(a.velocity > 0) velocity_.setValue(std::round(a.velocity * 127), juce::dontSendNotification);
        velocity_.setEnabled(a.velocity > 0);
        pressure_.setSelectedId(a.pressure == PadAssignment::Pressure::Poly ? 1 : a.pressure == PadAssignment::Pressure::Channel ? 2 : 3, juce::dontSendNotification);
        for(size_t b = 0; b < banks_.size(); ++b) banks_[b].setColour(juce::TextButton::buttonColourId, b == map_.bank ? ink(Ink::Accent) : ink(Ink::Raised));
    }

    void PadEditor::fill(bool everyBank)
    {
        PadFill f;
        f.kind = PadFill::Kind(fillKind_.getSelectedId() - 1);
        f.first = uint8_t(first_.getSelectedId() - 1);
        f.root = uint8_t(root_.getSelectedId() - 1);
        f.scale = size_t(scale_.getSelectedId() - 1);
        f.inKey = inKey_.getToggleState();
        fillPads(map_, everyBank ? -1 : map_.bank, f);
        apply(); showPad(); repaint();
    }

    juce::Rectangle<float> PadEditor::grid() const
    {
        auto area = getLocalBounds().toFloat().reduced(16).withTrimmedTop(96);
        const auto side = std::min(area.getWidth() * .5f, area.getHeight());
        return area.removeFromLeft(side).removeFromTop(side);
    }

    juce::Rectangle<float> PadEditor::padBounds(int pad) const
    {
        const auto g = grid();
        const auto cell = g.getWidth() / float(PadMap::columns);
        const int row = pad / int(PadMap::columns), column = pad % int(PadMap::columns);
        return {g.getX() + column * cell + 4, g.getBottom() - (row + 1) * cell + 4, cell - 8, cell - 8};
    }

    int PadEditor::padAt(juce::Point<float> p) const
    {
        for(int i = 0; i < int(PadMap::pads); ++i) if(padBounds(i).contains(p)) return i;
        return -1;
    }

    void PadEditor::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        for(int i = 0; i < int(PadMap::pads); ++i)
        {
            const auto r = padBounds(i);
            const auto& a = map_.assignments[map_.bank][size_t(i)];
            const bool chosen = i == selected_;
            g.setColour(i == held_ ? ink(Ink::Positive) : chosen ? ink(Ink::Accent) : ink(Ink::Raised));
            g.fillRoundedRectangle(r, themeData().radius);
            g.setColour(ink(Ink::Border));
            g.drawRoundedRectangle(r, themeData().radius, 1.5f);
            const auto text = chosen || i == held_ ? ink(Ink::Accent).contrasting() : ink(Ink::Text);
            g.setColour(text.withAlpha(.7f));
            g.setFont(juce::FontOptions(13.0f * themeData().fontScale));
            g.drawText(juce::String(i + 1), r.reduced(8), juce::Justification::topLeft);
            if(a.channel >= 0) g.drawText("ch " + juce::String(a.channel + 1), r.reduced(8), juce::Justification::topRight);
            g.setColour(text);
            g.setFont(juce::FontOptions(22.0f * themeData().fontScale, juce::Font::bold));
            g.drawText(juce::String::fromUTF8(noteName(a.note).c_str()), r, juce::Justification::centred);
        }
    }

    void PadEditor::resized()
    {
        auto area = getLocalBounds().reduced(16);
        auto top = area.removeFromTop(40);
        done_.setBounds(top.removeFromRight(120).reduced(2));
        title_.setBounds(top);
        auto bankRow = area.removeFromTop(48);
        const auto g = grid();
        auto banks = bankRow.withWidth(int(g.getWidth()));
        const int w = banks.getWidth() / int(banks_.size());
        for(auto& b : banks_) b.setBounds(banks.removeFromLeft(w).reduced(3));

        auto side = area.withTrimmedLeft(int(g.getWidth()) + 24);
        const int rowHeight = 36;
        auto row = [&] { return side.removeFromTop(rowHeight + 6).withTrimmedBottom(6); };
        padLabel_.setBounds(row());
        noteLabel_.setBounds(row());
        auto notes = row();
        octaveDown_.setBounds(notes.removeFromLeft(56).reduced(2));
        noteDown_.setBounds(notes.removeFromLeft(44).reduced(2));
        octaveUp_.setBounds(notes.removeFromRight(56).reduced(2));
        noteUp_.setBounds(notes.removeFromRight(44).reduced(2));
        note_.setBounds(notes);
        auto labelled = [&](juce::Label& label, juce::Component& control) { auto r = row(); label.setBounds(r.removeFromLeft(120)); control.setBounds(r); };
        labelled(channelLabel_, channel_);
        auto v = row(); velocityLabel_.setBounds(v.removeFromLeft(120)); fixedVelocity_.setBounds(v.removeFromLeft(150)); velocity_.setBounds(v);
        labelled(pressureLabel_, pressure_);
        setAll_.setBounds(row());
        side.removeFromTop(12);
        labelled(fillLabel_, fillKind_);
        auto options = row();
        first_.setBounds(options);
        root_.setBounds(options.removeFromLeft(90).reduced(0, 0));
        inKey_.setBounds(options.removeFromRight(100));
        scale_.setBounds(options.reduced(4, 0));
        auto buttons = row();
        fillBank_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(2));
        fillAll_.setBounds(buttons.reduced(2));
    }

    void PadEditor::mouseDown(const juce::MouseEvent& e)
    {
        const int pad = padAt(e.position);
        if(pad < 0) return;
        selected_ = pad; held_ = pad;
        engine_.pad(pad, true, .8f); // hear it
        showPad(); repaint();
    }

    void PadEditor::mouseUp(const juce::MouseEvent&)
    {
        if(held_ >= 0) engine_.pad(held_, false, 0);
        held_ = -1; repaint();
    }
}

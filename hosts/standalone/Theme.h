// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
namespace oscairn::standalone {
enum class Ink { Background, Surface, Raised, Border, Text, Muted, Accent, Positive, Danger, Count };
struct ThemeData {
    juce::String id="oscairn-dark",name="Oscairn Dark",font="Default Sans Serif";
    std::array<juce::Colour,9> colours{{juce::Colour(0xff15181d),juce::Colour(0xff20252d),juce::Colour(0xff2c333d),juce::Colour(0xff526070),juce::Colour(0xffedf1f5),juce::Colour(0xffa3aebb),juce::Colour(0xffffa45b),juce::Colour(0xff5ad9a4),juce::Colour(0xffff8d87)}};
    float fontScale=1,radius=6;
    static ThemeData parse(const juce::String&);
    static ThemeData light();
};
const ThemeData& themeData();
juce::Colour ink(Ink);
// Per-user Oscairn appearance, shared across layouts, windows and UI processes.
class Theme final : public juce::LookAndFeel_V4, private juce::Timer {
public:
    explicit Theme(juce::File root = {});
    ~Theme() override;
    std::vector<std::pair<juce::String,juce::String>> installed() const;
    void apply(const juce::String& id);
    void install(const juce::File& source);
    juce::File directory() const {return root_.getChildFile("themes");}
    const juce::String& error() const {return error_;}
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&,int) override;
    void drawButtonBackground(juce::Graphics&,juce::Button&,const juce::Colour&,bool,bool) override;
    void drawRotarySlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider&) override;
private:
    void set(const ThemeData&);
    ThemeData read(const juce::String& id) const;
    void reload();
    void timerCallback() override;
    juce::File root_,selection_;
    juce::String lastSelection_,error_;
};
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
namespace oscairn::standalone {
enum class Ink { Background, Surface, Raised, Border, Text, Muted, Accent, Positive, Danger, Count };
// Font files a theme carries, as paths relative to its folder. Empty only for the emergency theme.
struct ThemeFont { juce::String regular, bold, license; };
// How controls are drawn (theme format 3): a fixed set of choices the host draws, never code.
struct ThemeStyle {
    enum class Buttons { Rounded, Pill } buttons=Buttons::Pill;          // the theme's radius, or fully round ends
    enum class Edges { None, Hairline, Outlined } edges=Edges::Hairline;  // outlines on controls and cards
    enum class Depth { Flat, Soft } depth=Depth::Soft;                    // a small shadow under buttons and thumbs
    enum class Sliders { Filled, Line, Cap } sliders=Sliders::Line;       // an accent thumb, a thin track with a light round thumb, or with a square cap
    enum class Tabs { Folder, Segmented } tabs=Tabs::Segmented;
    enum class Marks { Square, Round } marks=Marks::Square;                // mute and solo
    enum class Sections { Separate, Joined } sections=Sections::Separate; // gaps between cards, or edge to edge with a line
};
int sectionGap(int separate);                    // the gap between sections: `separate`, or 0 when they join
// A section (a card, a tile, a mixer strip): rounded with its edge when sections are separate;
// square when they join, its edge on the shared pixel so neighbours draw one line between them.
void drawSection(juce::Graphics&, juce::Rectangle<int>, juce::Colour fill);
// Wires in the routing views (theme format 4): enabled audio and note wires, disabled ones,
// draining ones (seamless changes, RFC 0021) and ones that no longer fit their plugin's ports.
struct ThemeWires {
    juce::Colour audio{0xff8cc2ff},notes{0xffc29cff},disabled{0xff6b7480},draining{0xffff9a45},unconnected{0xffff6b63};
};
struct ThemeData {
    juce::String id="oscairn-dark",name="Oscairn Dark";
    ThemeFont font;
    std::array<juce::Colour,9> colours{{juce::Colour(0xff15181d),juce::Colour(0xff20252d),juce::Colour(0xff2c333d),juce::Colour(0xff526070),juce::Colour(0xffedf1f5),juce::Colour(0xffa3aebb),juce::Colour(0xffffa45b),juce::Colour(0xff5ad9a4),juce::Colour(0xffff8d87)}};
    float fontScale=1,radius=6;
    ThemeStyle style;
    ThemeWires wires;
    static ThemeData parse(const juce::String&);
};
const ThemeData& themeData();
juce::Colour ink(Ink);
bool darkTheme();                                // a dark background, for choices colours alone can't make
float fieldRadius(float height);                 // dropdowns, text boxes, value boxes
float buttonRadius(float height);
// An outline in the theme's edge style, or nothing; focus is drawn whatever the style.
void drawEdge(juce::Graphics&,juce::Rectangle<float>,float radius,bool focus=false);
// The desktop's type scale (RFC 0019): text sizes fixed by role, not by the box they sit in. A
// component inside the desktop window has its input mode; 0 elsewhere (a device's panel).
enum class TextRole { Small, Body, Button, Title };
int desktopMode(const juce::Component&);        // 0: not the desktop, 1: keyboard and mouse, 2: touch
void setDesktopMode(juce::Component&, int mode);
float textSize(TextRole, int mode);              // in pixels, with the theme's font scale
// Per-user Oscairn appearance, shared across layouts, windows and UI processes. A theme is a
// folder holding theme.json and its fonts. Built-in themes are the same folders, shipped beside
// the app; instrument builders can replace them and name the default in themes/default.
class Theme final : public juce::LookAndFeel_V4, private juce::Timer {
public:
    explicit Theme(juce::File root = {}, juce::File builtIn = {});
    ~Theme() override;
    std::vector<std::pair<juce::String,juce::String>> installed() const;
    void apply(const juce::String& id);
    void install(const juce::File& source); // a theme folder or its theme.json
    juce::File directory() const {return root_.getChildFile("themes");}
    juce::File configuration() const {return root_;} // the per-user Oscairn configuration folder
    const juce::String& error() const {return error_;}
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&,int) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void drawComboBox(juce::Graphics&,int,int,bool,int,int,int,int,juce::ComboBox&) override;
    void fillTextEditorBackground(juce::Graphics&,int,int,juce::TextEditor&) override;
    void drawLabel(juce::Graphics&,juce::Label&) override;
    void drawLinearSlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider::SliderStyle,juce::Slider&) override;
    int getSliderThumbRadius(juce::Slider&) override;
    void drawTickBox(juce::Graphics&,juce::Component&,float,float,float,float,bool,bool,bool,bool) override;
    void drawTextEditorOutline(juce::Graphics&,int,int,juce::TextEditor&) override;
    void drawButtonBackground(juce::Graphics&,juce::Button&,const juce::Colour&,bool,bool) override;
    void drawRotarySlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider&) override;
private:
    struct Loaded { ThemeData data; juce::Typeface::Ptr regular,bold; };
    Loaded load(const juce::File& folder) const;
    juce::File find(const juce::String& id) const;
    juce::String defaultId() const;
    void set(const Loaded&);
    void reload();
    void timerCallback() override;
    juce::File root_,builtIn_,selection_;
    juce::Typeface::Ptr regular_,bold_;
    juce::String lastSelection_,error_;
};
}

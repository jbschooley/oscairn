// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Theme.h"
#include <cmath>
#include <stdexcept>
namespace oscairn::standalone {
namespace {
ThemeData current;
const std::array<juce::String,9> names{"background","surface","raised","border","text","muted","accent","positive","danger"};
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void fields(const juce::DynamicObject& object,const juce::StringArray& allowed) {
    for(const auto& item:object.getProperties())require(allowed.contains(item.name.toString()),"Unknown theme field");
}
void write(const juce::File& destination,const juce::String& text) {
    require(destination.getParentDirectory().createDirectory().wasOk(),"Cannot create theme directory");
    juce::TemporaryFile temporary(destination);
    require(temporary.getFile().replaceWithText(text,false,false,nullptr) && temporary.overwriteTargetFileWithTemporary(),"Cannot save appearance file");
}
}
const ThemeData& themeData(){return current;}
juce::Colour ink(Ink role){return current.colours[static_cast<size_t>(role)];}
ThemeData ThemeData::light() {
    ThemeData t;t.id="oscairn-light";t.name="Oscairn Light";
    const std::array<juce::uint32,9> colours{0xffe9edf2,0xfff7f9fc,0xffdce3eb,0xff6a7888,0xff172332,0xff4f6073,0xff964600,0xff006b49,0xffb12831};
    for(size_t i=0;i<colours.size();++i)t.colours[i]=juce::Colour(colours[i]);return t;
}
ThemeData ThemeData::parse(const juce::String& json) {
    require(json.getNumBytesAsUTF8()<=16384,"Theme exceeds 16 KiB");
    juce::var value;require(juce::JSON::parse(json,value).wasOk(),"Invalid theme JSON");
    const auto* object=value.getDynamicObject();require(object,"Theme must be an object");
    fields(*object,{"version","id","name","font","fontScale","radius","colours"});
    require(value["version"].isInt() && int(value["version"])==1,"Unsupported theme version");
    ThemeData t;t.id=value["id"].toString();t.name=value["name"].toString();
    require(value["id"].isString() && t.id.length()>0 && t.id.length()<=64 && t.id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-"),"Theme ID must contain lowercase letters, digits or hyphens");
    require(value["name"].isString() && t.name.isNotEmpty() && t.name.length()<=80,"Theme needs a short name");
    if(object->hasProperty("font")){require(value["font"].isString(),"Invalid theme font");t.font=value["font"].toString();require(t.font.isNotEmpty() && t.font.length()<=80,"Invalid theme font");}
    auto number=[&](const char* key,float fallback,float low,float high) {
        if(!object->hasProperty(key))return fallback;const auto v=value[key];
        require(v.isInt() || v.isDouble(),"Theme metric must be a number");const double n=v;
        require(std::isfinite(n) && n>=low && n<=high,"Theme metric is out of range");return static_cast<float>(n);
    };
    t.fontScale=number("fontScale",1,.8f,1.4f);t.radius=number("radius",6,0,16);
    const auto* colours=value["colours"].getDynamicObject();require(colours,"Theme needs semantic colours");
    juce::StringArray allowed;for(const auto& name:names)allowed.add(name);fields(*colours,allowed);
    for(size_t i=0;i<names.size();++i){const auto colour=colours->getProperty(names[i]);const auto text=colour.toString();
        require(colour.isString() && text.length()==7 && text[0]=='#' && text.substring(1).containsOnly("0123456789abcdefABCDEF"),"Theme colours must be #RRGGBB");
        t.colours[i]=juce::Colour(0xff000000u|static_cast<juce::uint32>(text.substring(1).getHexValue32()));}
    auto luminance=[](juce::Colour c){auto linear=[](double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};return .2126*linear(c.getFloatRed())+.7152*linear(c.getFloatGreen())+.0722*linear(c.getFloatBlue());};
    for(auto background:{Ink::Background,Ink::Surface,Ink::Raised}) {
        const auto a=luminance(t.colours[static_cast<size_t>(Ink::Text)]),b=luminance(t.colours[static_cast<size_t>(background)]);
        require((std::max(a,b)+.05)/(std::min(a,b)+.05)>=4.5,"Theme text contrast must be at least 4.5:1");
    }
    return t;
}
Theme::Theme(juce::File root) {
    if(root==juce::File()) {
        const auto env=juce::SystemStats::getEnvironmentVariable("OSCAIRN_CONFIG_HOME",{});
        if(env.isNotEmpty())root=juce::File(env);
#if JUCE_MAC
        else root=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Application Support/Oscairn");
#elif JUCE_LINUX
        else {const auto xdg=juce::SystemStats::getEnvironmentVariable("XDG_CONFIG_HOME",{});root=(xdg.isNotEmpty()?juce::File(xdg):juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".config")).getChildFile("Oscairn");}
#else
        else root=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Oscairn");
#endif
    }
    root_=root;selection_=root_.getChildFile("appearance.txt");
    set(ThemeData{});reload();juce::LookAndFeel::setDefaultLookAndFeel(this);startTimer(1000);
}
Theme::~Theme(){stopTimer();juce::LookAndFeel::setDefaultLookAndFeel(nullptr);}
void Theme::set(const ThemeData& data) {
    current=data;
    // JUCE on Linux draws no text for a family that is not installed (macOS substitutes one), so a
    // missing or placeholder name such as "Default Sans Serif" uses the platform default font.
    setDefaultSansSerifTypefaceName(juce::Font::findAllTypefaceNames().contains(data.font)?data.font:juce::String());
    setColour(juce::ResizableWindow::backgroundColourId,ink(Ink::Background));
    setColour(juce::Label::textColourId,ink(Ink::Text));
    setColour(juce::TextButton::buttonColourId,ink(Ink::Raised));setColour(juce::TextButton::buttonOnColourId,ink(Ink::Accent));
    setColour(juce::TextButton::textColourOffId,ink(Ink::Text));setColour(juce::TextButton::textColourOnId,ink(Ink::Accent).contrasting());
    setColour(juce::ToggleButton::textColourId,ink(Ink::Text));setColour(juce::ToggleButton::tickColourId,ink(Ink::Accent));
    for(int id:std::initializer_list<int>{juce::ComboBox::backgroundColourId,juce::TextEditor::backgroundColourId,juce::ListBox::backgroundColourId,juce::PopupMenu::backgroundColourId})setColour(id,ink(Ink::Surface));
    for(int id:std::initializer_list<int>{juce::ComboBox::textColourId,juce::TextEditor::textColourId,juce::ListBox::textColourId,juce::PopupMenu::textColourId})setColour(id,ink(Ink::Text));
    setColour(juce::ComboBox::outlineColourId,ink(Ink::Border));setColour(juce::ComboBox::arrowColourId,ink(Ink::Text));
    setColour(juce::TextEditor::outlineColourId,ink(Ink::Border));setColour(juce::TextEditor::focusedOutlineColourId,ink(Ink::Accent));
    setColour(juce::PopupMenu::highlightedBackgroundColourId,ink(Ink::Accent));setColour(juce::PopupMenu::highlightedTextColourId,ink(Ink::Accent).contrasting());
    setColour(juce::Slider::rotarySliderFillColourId,ink(Ink::Accent));setColour(juce::Slider::rotarySliderOutlineColourId,ink(Ink::Border));
    setColour(juce::ScrollBar::thumbColourId,ink(Ink::Accent));setColour(juce::ScrollBar::trackColourId,ink(Ink::Surface));
    setColour(juce::Slider::backgroundColourId,ink(Ink::Border));
    setColour(juce::Slider::trackColourId,ink(Ink::Accent));setColour(juce::Slider::thumbColourId,ink(Ink::Accent));
    setColour(juce::Slider::textBoxTextColourId,ink(Ink::Text));setColour(juce::Slider::textBoxBackgroundColourId,ink(Ink::Surface));setColour(juce::Slider::textBoxOutlineColourId,ink(Ink::Border));
    setColour(juce::AlertWindow::backgroundColourId,ink(Ink::Surface));setColour(juce::AlertWindow::textColourId,ink(Ink::Text));
    auto refreshText=[&](auto&& self,juce::Component& component)->void {
        if(auto* editor=dynamic_cast<juce::TextEditor*>(&component))editor->applyColourToAllText(ink(Ink::Text));
        for(auto* child:component.getChildren())self(self,*child);
    };
    for(int i=0;i<juce::Desktop::getInstance().getNumComponents();++i){auto* component=juce::Desktop::getInstance().getComponent(i);component->sendLookAndFeelChange();refreshText(refreshText,*component);}

}
ThemeData Theme::read(const juce::String& id) const {
    require(id.isNotEmpty() && id.length()<=64 && id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-"),"Invalid theme ID");
    require(directory().getChildFile(id+".json").getSize()<=16384,"Theme exceeds 16 KiB");
    const auto data=id=="oscairn-dark"?ThemeData{}:id=="oscairn-light"?ThemeData::light():ThemeData::parse(directory().getChildFile(id+".json").loadFileAsString());
    require(data.id==id,"Theme ID mismatch");return data;
}
void Theme::apply(const juce::String& id) {
    const auto data=read(id);write(selection_,id);lastSelection_=id;set(data);error_.clear();
}
void Theme::install(const juce::File& source) {
    require(source.getSize()<=16384,"Theme exceeds 16 KiB");const auto text=source.loadFileAsString();const auto data=ThemeData::parse(text);
    require(data.id!="oscairn-dark" && data.id!="oscairn-light","Built-in theme IDs are reserved");
    const auto destination=directory().getChildFile(data.id+".json");
    require(!destination.exists() || destination.loadFileAsString()==text,"That theme ID is already installed; use a new ID for a different theme");
    write(destination,text);apply(data.id);
}
std::vector<std::pair<juce::String,juce::String>> Theme::installed() const {
    std::vector<std::pair<juce::String,juce::String>> result{{"oscairn-dark","Oscairn Dark"},{"oscairn-light","Oscairn Light"}};
    for(const auto& file:directory().findChildFiles(juce::File::findFiles,false,"*.json"))try{require(file.getSize()<=16384,"Theme exceeds 16 KiB");const auto data=ThemeData::parse(file.loadFileAsString());if(file.getFileNameWithoutExtension()==data.id)result.emplace_back(data.id,data.name);}catch(const std::exception&){}
    return result;
}
void Theme::reload() {
    const auto id=selection_.loadFileAsString().trim();if(id==lastSelection_)return;
    lastSelection_=id;
    try{set(read(id.isEmpty()?"oscairn-dark":id));error_.clear();}catch(const std::exception& e){error_=e.what();set(ThemeData{});}
}
void Theme::timerCallback(){reload();}
juce::Font Theme::getLabelFont(juce::Label& label){return juce::FontOptions(juce::jlimit(12.f,28.f,label.getHeight()*.7f)*current.fontScale);}
juce::Font Theme::getTextButtonFont(juce::TextButton&,int height){return juce::FontOptions(juce::jlimit(13.f,22.f,height*.36f)*current.fontScale);}
void Theme::drawButtonBackground(juce::Graphics& g,juce::Button& button,const juce::Colour& colour,bool hover,bool down) {
    const auto bounds=button.getLocalBounds().toFloat().reduced(1);
    g.setColour(colour.withMultipliedAlpha(button.isEnabled()?1.f:.4f).brighter(down?.15f:hover?.07f:0));
    g.fillRoundedRectangle(bounds,current.radius);g.setColour(ink(button.hasKeyboardFocus(true)?Ink::Accent:Ink::Border));g.drawRoundedRectangle(bounds,current.radius,button.hasKeyboardFocus(true)?2.f:1.f);
}
void Theme::drawRotarySlider(juce::Graphics& g,int x,int y,int width,int height,float position,float start,float end,juce::Slider& slider) {
    if(!slider.getProperties()["endless"]){LookAndFeel_V4::drawRotarySlider(g,x,y,width,height,position,start,end,slider);return;}
    const auto bounds=juce::Rectangle<int>(x,y,width,height).toFloat().reduced(4);const auto radius=std::min(bounds.getWidth(),bounds.getHeight())/2,centreX=bounds.getCentreX(),centreY=bounds.getCentreY();
    const int on=slider.isEnabled()?juce::roundToInt(position*16):0;
    for(int i=0;i<16;++i){const auto at=juce::Point<float>(centreX,centreY).getPointOnCircumference(radius*.9f,start+(end-start)*(i+.5f)/16);g.setColour(ink(i<on?Ink::Accent:Ink::Border));g.fillEllipse(juce::Rectangle<float>(radius*.14f,radius*.14f).withCentre(at));}
    const auto knob=juce::Rectangle<float>(radius*1.4f,radius*1.4f).withCentre({centreX,centreY});
    g.setColour(ink(Ink::Surface));g.fillEllipse(knob);g.setColour(ink(Ink::Border));g.drawEllipse(knob,radius*.05f);
}
}

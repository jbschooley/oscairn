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
constexpr juce::int64 maxThemeBytes=16384,maxFontBytes=4<<20,maxLicenseBytes=64<<10;
bool validId(const juce::String& id){return id.isNotEmpty() && id.length()<=64 && id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-");}
// A file named by a theme: a relative path inside its folder, no links, within a size limit.
juce::File inside(const juce::File& folder,const juce::String& relative,juce::int64 limit) {
    require(relative.isNotEmpty() && relative.length()<=128 && relative.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/"),"Theme file paths must be relative names inside the theme");
    for(const auto& part:juce::StringArray::fromTokens(relative,"/",""))require(part.isNotEmpty() && part!="." && part!="..","Theme file paths must stay inside the theme");
    const auto file=folder.getChildFile(relative);
    for(auto at=file;at!=folder;at=at.getParentDirectory()){require(!at.isSymbolicLink(),"Theme files must not be links");if(at.getParentDirectory()==at)break;}
    require(file.existsAsFile(),"Theme file is missing");require(file.getSize()<=limit,"Theme file is too large");
    return file;
}
// JUCE dereferences a failed font parse, so check the font container first: signature, a table
// directory inside the file and the tables a usable font needs. Not a full font validator.
bool fontContainer(const juce::MemoryBlock& data) {
    const auto* b=static_cast<const juce::uint8*>(data.getData());const auto size=data.getSize();
    auto u32=[&](size_t at){return juce::uint32(b[at])<<24|juce::uint32(b[at+1])<<16|juce::uint32(b[at+2])<<8|b[at+3];};
    if(size<12)return false;
    const auto signature=u32(0);if(signature!=0x00010000u && signature!=0x74727565u && signature!=0x4f54544fu)return false; // 1.0, 'true', 'OTTO'
    const size_t tables=size_t(b[4])<<8|b[5];if(tables<1 || tables>64 || 12+16*tables>size)return false;
    juce::StringArray tags;
    for(size_t i=0;i<tables;++i){const auto at=12+16*i;
        if(uint64_t(u32(at+8))+u32(at+12)>size)return false;
        tags.add(juce::String::fromUTF8(reinterpret_cast<const char*>(b+at),4));}
    for(const auto* tag:{"cmap","head","hhea","hmtx","maxp","name"})if(!tags.contains(tag))return false;
    return (tags.contains("glyf") && tags.contains("loca")) || tags.contains("CFF ") || tags.contains("CFF2");
}
juce::Typeface::Ptr typeface(const juce::File& file) {
    require(file.hasFileExtension("ttf;otf"),"Theme fonts must be .ttf or .otf files");
    juce::MemoryBlock data;require(file.loadFileAsData(data),"Cannot read theme font");
    require(fontContainer(data),"Theme font is not a TrueType or OpenType font");
    auto face=juce::Typeface::createSystemTypefaceFor(data.getData(),data.getSize());
    require(face!=nullptr && face->getNominalGlyphForCodepoint('A').has_value(),"Theme font cannot be used");
    return face;
}
void write(const juce::File& destination,const juce::String& text) {
    require(destination.getParentDirectory().createDirectory().wasOk(),"Cannot create theme directory");
    juce::TemporaryFile temporary(destination);
    require(temporary.getFile().replaceWithText(text,false,false,nullptr) && temporary.overwriteTargetFileWithTemporary(),"Cannot save appearance file");
}
}
const ThemeData& themeData(){return current;}
juce::Colour ink(Ink role){return current.colours[static_cast<size_t>(role)];}
ThemeData ThemeData::parse(const juce::String& json) {
    require(json.getNumBytesAsUTF8()<=maxThemeBytes,"Theme exceeds 16 KiB");
    juce::var value;require(juce::JSON::parse(json,value).wasOk(),"Invalid theme JSON");
    const auto* object=value.getDynamicObject();require(object,"Theme must be an object");
    fields(*object,{"version","id","name","font","fontScale","radius","colours"});
    require(value["version"].isInt() && int(value["version"])==2,"Unsupported theme version");
    ThemeData t;t.id=value["id"].toString();t.name=value["name"].toString();
    require(value["id"].isString() && validId(t.id),"Theme ID must contain lowercase letters, digits or hyphens");
    require(value["name"].isString() && t.name.isNotEmpty() && t.name.length()<=80,"Theme needs a short name");
    const auto* font=value["font"].getDynamicObject();require(font,"Theme needs its font files");fields(*font,{"regular","bold","license"});
    auto path=[&](const char* key,bool required){const auto v=font->getProperty(key);if(!required && v.isVoid())return juce::String();require(v.isString(),"Theme font paths must be strings");return v.toString();};
    t.font={path("regular",true),path("bold",false),path("license",true)};
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
Theme::Theme(juce::File root,juce::File builtIn) {
    if(builtIn==juce::File()) {
        // Beside the app (macOS bundle, build tree) or installed under share/ (Linux devices).
        const auto binary=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        builtIn=binary.getChildFile("themes").isDirectory()?binary.getChildFile("themes"):binary.getSiblingFile("share").getChildFile("oscairn/themes");
    }
    builtIn_=builtIn;
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
    set(Loaded{});reload();juce::LookAndFeel::setDefaultLookAndFeel(this);startTimer(1000);
}
Theme::~Theme(){stopTimer();juce::LookAndFeel::setDefaultLookAndFeel(nullptr);}
void Theme::set(const Loaded& loaded) {
    current=loaded.data;regular_=loaded.regular;bold_=loaded.bold;
    // Without theme fonts (the emergency theme) text uses the platform default font.
    setDefaultSansSerifTypefaceName({});juce::Typeface::clearTypefaceCache();
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
Theme::Loaded Theme::load(const juce::File& folder) const {
    const auto file=inside(folder,"theme.json",maxThemeBytes);
    Loaded result{ThemeData::parse(file.loadFileAsString()),nullptr,nullptr};
    require(folder.getFileName()==result.data.id,"Theme folder must be named after its ID");
    const auto& font=result.data.font;
    result.regular=typeface(inside(folder,font.regular,maxFontBytes));
    if(font.bold.isNotEmpty())result.bold=typeface(inside(folder,font.bold,maxFontBytes));
    inside(folder,font.license,maxLicenseBytes);
    return result;
}
juce::File Theme::find(const juce::String& id) const {
    require(validId(id),"Invalid theme ID");
    return builtIn_.getChildFile(id).isDirectory()?builtIn_.getChildFile(id):directory().getChildFile(id);
}
juce::String Theme::defaultId() const {
    const auto id=builtIn_.getChildFile("default").loadFileAsString().trim();
    return validId(id) && builtIn_.getChildFile(id).isDirectory()?id:juce::String("oscairn-dark");
}
void Theme::apply(const juce::String& id) {
    const auto loaded=load(find(id));write(selection_,id);lastSelection_=id;set(loaded);error_.clear();
}
void Theme::install(const juce::File& source) {
    const auto folder=source.isDirectory()?source:source.getParentDirectory();
    require(source.isDirectory() || source.getFileName()=="theme.json","Choose a theme folder or its theme.json");
    const auto loaded=load(folder);const auto& id=loaded.data.id;const auto& font=loaded.data.font;
    require(!builtIn_.getChildFile(id).isDirectory(),"Built-in theme IDs are reserved");
    juce::StringArray files{"theme.json",font.regular,font.license};if(font.bold.isNotEmpty())files.add(font.bold);
    const auto destination=directory().getChildFile(id);
    if(destination.exists()) {
        for(const auto& name:files)require(destination.getChildFile(name).existsAsFile() && destination.getChildFile(name).hasIdenticalContentTo(folder.getChildFile(name)),"That theme ID is already installed; use a new ID for a different theme");
    } else {
        // Copy only the files the theme names, into a staging folder renamed into place.
        const auto staging=directory().getNonexistentChildFile("."+id+"-install","",false);
        require(staging.createDirectory().wasOk(),"Cannot create theme directory");
        try {
            for(const auto& name:files){const auto target=staging.getChildFile(name);require(target.getParentDirectory().createDirectory().wasOk() && folder.getChildFile(name).copyFileTo(target),"Cannot copy theme files");}
            require(staging.moveFileTo(destination),"Cannot install theme");
        } catch(...) { staging.deleteRecursively(); throw; }
    }
    apply(id);
}
std::vector<std::pair<juce::String,juce::String>> Theme::installed() const {
    std::vector<std::pair<juce::String,juce::String>> result;
    for(const auto& folder:{builtIn_,directory()})
        for(const auto& theme:folder.findChildFiles(juce::File::findDirectories,false))
            try{const auto data=ThemeData::parse(inside(theme,"theme.json",maxThemeBytes).loadFileAsString());
                if(theme.getFileName()==data.id && (folder==builtIn_ || !builtIn_.getChildFile(data.id).isDirectory()))result.emplace_back(data.id,data.name);}catch(const std::exception&){}
    return result;
}
void Theme::reload() {
    const auto id=selection_.loadFileAsString().trim();if(id==lastSelection_ && lastSelection_.isNotEmpty())return;
    lastSelection_=id;
    try{set(load(find(id.isEmpty()?defaultId():id)));error_.clear();return;}catch(const std::exception& e){error_=e.what();}
    // The selected theme is unusable: use the default theme, or the emergency theme without fonts.
    try{set(load(find(defaultId())));}catch(const std::exception&){set(Loaded{});}
}
void Theme::timerCallback(){reload();}
juce::Typeface::Ptr Theme::getTypefaceForFont(const juce::Font& font) {
    if(regular_ && font.getTypefaceName()==juce::Font::getDefaultSansSerifFontName())return font.isBold() && bold_?bold_:regular_;
    return LookAndFeel_V4::getTypefaceForFont(font);
}
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

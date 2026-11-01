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
int sectionGap(int separate){return current.style.sections==ThemeStyle::Sections::Joined?0:separate;}
void drawSection(juce::Graphics& g,juce::Rectangle<int> r,juce::Colour fill) {
    if(current.style.sections==ThemeStyle::Sections::Joined) {
        g.setColour(fill);g.fillRect(r);
        g.setColour(ink(Ink::Border).withAlpha(current.style.edges==ThemeStyle::Edges::Hairline?.6f:1.f));
        g.drawRect(r.expanded(1,1).withTrimmedRight(1).withTrimmedBottom(1),1);
        return;
    }
    const float radius=current.radius+2;g.setColour(fill);g.fillRoundedRectangle(r.toFloat(),radius);drawEdge(g,r.toFloat().reduced(.5f),radius);
}
bool darkTheme(){return ink(Ink::Background).getPerceivedBrightness()<.5f;}
float buttonRadius(float height){return current.style.buttons==ThemeStyle::Buttons::Pill?height/2:current.radius;}
// Pill themes round single-line fields like their buttons; a taller field (several lines) keeps the radius plus 3.
float fieldRadius(float height){if(current.style.buttons!=ThemeStyle::Buttons::Pill)return std::min(height/2,current.radius);return height<=56?height/2:current.radius+3;}
void drawEdge(juce::Graphics& g,juce::Rectangle<float> r,float radius,bool focus) {
    if(focus){g.setColour(ink(Ink::Accent));g.drawRoundedRectangle(r,radius,2.f);return;}
    if(current.style.edges==ThemeStyle::Edges::None)return;
    g.setColour(ink(Ink::Border).withAlpha(current.style.edges==ThemeStyle::Edges::Hairline?.6f:1.f));g.drawRoundedRectangle(r,radius,1.f);
}
ThemeData ThemeData::parse(const juce::String& json) {
    require(json.getNumBytesAsUTF8()<=maxThemeBytes,"Theme exceeds 16 KiB");
    juce::var value;require(juce::JSON::parse(json,value).wasOk(),"Invalid theme JSON");
    const auto* object=value.getDynamicObject();require(object,"Theme must be an object");
    fields(*object,{"version","id","name","font","fontScale","radius","colours","style","wires"});
    require(value["version"].isInt() && int(value["version"])==4,"Unsupported theme version");
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
    auto hex=[](const juce::var& colour){const auto text=colour.toString();
        require(colour.isString() && text.length()==7 && text[0]=='#' && text.substring(1).containsOnly("0123456789abcdefABCDEF"),"Theme colours must be #RRGGBB");
        return juce::Colour(0xff000000u|static_cast<juce::uint32>(text.substring(1).getHexValue32()));};
    for(size_t i=0;i<names.size();++i)t.colours[i]=hex(colours->getProperty(names[i]));
    const auto* wires=value["wires"].getDynamicObject();require(wires,"Theme needs its wire colours");
    fields(*wires,{"audio","notes","disabled","draining","unconnected"});
    t.wires={hex(wires->getProperty("audio")),hex(wires->getProperty("notes")),hex(wires->getProperty("disabled")),hex(wires->getProperty("draining")),hex(wires->getProperty("unconnected"))};
    const auto* style=value["style"].getDynamicObject();require(style,"Theme needs a style");
    fields(*style,{"buttons","edges","depth","sliders","tabs","marks","sections"});
    auto choice=[&](const char* key,const juce::StringArray& options){const auto v=style->getProperty(key);const int i=options.indexOf(v.toString());require(v.isString() && i>=0,"Theme style choice is not one of its options");return i;};
    t.style.buttons=ThemeStyle::Buttons(choice("buttons",{"rounded","pill"}));
    t.style.edges=ThemeStyle::Edges(choice("edges",{"none","hairline","outlined"}));
    t.style.depth=ThemeStyle::Depth(choice("depth",{"flat","soft"}));
    t.style.sliders=ThemeStyle::Sliders(choice("sliders",{"filled","line","cap"}));
    t.style.tabs=ThemeStyle::Tabs(choice("tabs",{"folder","segmented"}));
    t.style.marks=ThemeStyle::Marks(choice("marks",{"square","round"}));
    t.style.sections=ThemeStyle::Sections(choice("sections",{"separate","joined"}));
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
int desktopMode(const juce::Component& c) {
    for(auto* at=&c;at;at=at->getParentComponent())if(const auto* v=at->getProperties().getVarPointer("oscairnDesktop"))return int(*v);
    return 0;
}
void setDesktopMode(juce::Component& c,int mode){c.getProperties().set("oscairnDesktop",mode);}
float textSize(TextRole role,int mode) {
    const bool touch=mode==2;
    const float size=role==TextRole::Small?(touch?13.f:12.f):role==TextRole::Button?(touch?15.f:13.f):role==TextRole::Title?(touch?20.f:18.f):(touch?16.f:14.f);
    return size*current.fontScale;
}
// On the desktop a label keeps the size its code gives it and buttons and menus the type scale;
// on a device's panel, text follows its box, which the panel scales as a whole.
juce::Font Theme::getLabelFont(juce::Label& label){return desktopMode(label)?label.getFont():juce::Font(juce::FontOptions(juce::jlimit(12.f,28.f,label.getHeight()*.7f)*current.fontScale));}
juce::Font Theme::getTextButtonFont(juce::TextButton& b,int height){const int mode=desktopMode(b);return juce::FontOptions(mode?textSize(TextRole::Button,mode):juce::jlimit(13.f,22.f,height*.36f)*current.fontScale);}
juce::Font Theme::getComboBoxFont(juce::ComboBox& box){const int mode=desktopMode(box);return mode?juce::Font(juce::FontOptions(textSize(TextRole::Body,mode))):LookAndFeel_V4::getComboBoxFont(box);}
// Dropdowns and text boxes share the buttons' corners and outline.
void Theme::drawComboBox(juce::Graphics& g,int width,int height,bool,int,int,int,int,juce::ComboBox& box) {
    const auto bounds=juce::Rectangle<float>(0,0,float(width),float(height)).reduced(1);const float radius=fieldRadius(bounds.getHeight());
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId).withMultipliedAlpha(box.isEnabled()?1.f:.4f));g.fillRoundedRectangle(bounds,radius);
    drawEdge(g,bounds,radius,box.hasKeyboardFocus(true));
    const auto arrow=juce::Rectangle<float>(float(width-height),0,float(height),float(height)).withSizeKeepingCentre(height*.3f,height*.15f);
    juce::Path p;p.startNewSubPath(arrow.getX(),arrow.getY());p.lineTo(arrow.getCentreX(),arrow.getBottom());p.lineTo(arrow.getRight(),arrow.getY());
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(box.isEnabled()?1.f:.4f));
    g.strokePath(p,juce::PathStrokeType(1.6f,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
}
// Linear sliders: the Line style draws a thin track and a light thumb; a slider centred on zero
// (pan, transpose: the property "bipolar") fills from its centre in either style.
int Theme::getSliderThumbRadius(juce::Slider& slider){return current.style.sliders!=ThemeStyle::Sliders::Filled?8:LookAndFeel_V4::getSliderThumbRadius(slider);}
void Theme::drawLinearSlider(juce::Graphics& g,int x,int y,int width,int height,float position,float minimum,float maximum,juce::Slider::SliderStyle style,juce::Slider& slider) {
    const bool horizontal=style==juce::Slider::LinearHorizontal,line=current.style.sliders!=ThemeStyle::Sliders::Filled,bipolar=slider.getProperties()["bipolar"];
    if((!horizontal && style!=juce::Slider::LinearVertical) || (!line && !bipolar)){LookAndFeel_V4::drawLinearSlider(g,x,y,width,height,position,minimum,maximum,style,slider);return;}
    const auto area=juce::Rectangle<int>(x,y,width,height).toFloat();
    const float track=line?(current.style.sliders==ThemeStyle::Sliders::Cap?3.f:4.f):juce::jmin(6.f,(horizontal?area.getHeight():area.getWidth())*.25f);
    const auto centre=area.getCentre();
    const auto at=horizontal?juce::Point<float>(position,centre.y):juce::Point<float>(centre.x,position);
    const auto start=horizontal?juce::Point<float>(area.getX()+track,centre.y):juce::Point<float>(centre.x,area.getBottom()-track);
    const auto end=horizontal?juce::Point<float>(area.getRight()-track,centre.y):juce::Point<float>(centre.x,area.getY()+track);
    const float alpha=slider.isEnabled()?1.f:.5f;
    auto stroke=[&](juce::Point<float> a,juce::Point<float> b,juce::Colour colour){juce::Path p;p.startNewSubPath(a);p.lineTo(b);g.setColour(colour);g.strokePath(p,juce::PathStrokeType(track,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));};
    stroke(start,end,(line?ink(Ink::Border).withAlpha(.55f):slider.findColour(juce::Slider::backgroundColourId)).withMultipliedAlpha(alpha));
    stroke(bipolar?centre:start,at,slider.findColour(juce::Slider::trackColourId).withMultipliedAlpha(alpha));
    if(bipolar){g.setColour(ink(Ink::Muted));if(horizontal)g.fillRect(centre.x-.5f,centre.y-track-1,1.f,track*2+2);else g.fillRect(centre.x-track-1,centre.y-.5f,track*2+2,1.f);}
    const float radius=float(getSliderThumbRadius(slider));
    // A cap is a short bar across the track; a line thumb is a disc.
    const bool cap=current.style.sliders==ThemeStyle::Sliders::Cap;
    const auto thumb=cap?(horizontal?juce::Rectangle<float>(radius*1.25f,radius*2.25f):juce::Rectangle<float>(radius*2.75f,radius*1.25f)).withCentre(at):juce::Rectangle<float>(radius*2,radius*2).withCentre(at);
    const float corner=cap?std::min(2.f,current.radius):radius;
    if(line) {
        if(current.style.depth==ThemeStyle::Depth::Soft){g.setColour(juce::Colours::black.withAlpha(darkTheme()?.45f:.18f));g.fillRoundedRectangle(thumb.translated(0,1),corner);}
        g.setColour(juce::Colours::white.withMultipliedAlpha(alpha));g.fillRoundedRectangle(thumb,corner);
        g.setColour(ink(Ink::Border).withAlpha(.8f));g.drawRoundedRectangle(thumb.reduced(.5f),corner,1.f);
    } else {g.setColour(slider.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(alpha));g.fillEllipse(thumb.reduced(radius/2));}
}
// Checkboxes share the fields' corners and edge.
void Theme::drawTickBox(juce::Graphics& g,juce::Component& c,float x,float y,float w,float h,bool ticked,bool enabled,bool,bool) {
    const auto box=juce::Rectangle<float>(x,y,w,h).reduced(1);const float radius=std::min(4.f,box.getHeight()/4);
    g.setColour((ticked?ink(Ink::Accent):ink(Ink::Surface)).withMultipliedAlpha(enabled?1.f:.5f));g.fillRoundedRectangle(box,radius);
    if(!ticked){g.setColour(ink(Ink::Border).withAlpha(enabled?1.f:.5f));g.drawRoundedRectangle(box,radius,1.f);return;}
    juce::Path tick;tick.startNewSubPath(box.getX()+box.getWidth()*.25f,box.getCentreY());tick.lineTo(box.getX()+box.getWidth()*.43f,box.getBottom()-box.getHeight()*.27f);tick.lineTo(box.getRight()-box.getWidth()*.22f,box.getY()+box.getHeight()*.28f);
    g.setColour(ink(Ink::Accent).contrasting().withMultipliedAlpha(enabled?1.f:.5f));g.strokePath(tick,juce::PathStrokeType(std::max(1.5f,w*.12f),juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
    juce::ignoreUnused(c);
}
// A label with a background or outline (a slider's value box) gets the same corners.
void Theme::drawLabel(juce::Graphics& g,juce::Label& label) {
    const auto bounds=label.getLocalBounds().toFloat().reduced(.5f);const float radius=fieldRadius(bounds.getHeight());
    g.setColour(label.findColour(juce::Label::backgroundColourId));g.fillRoundedRectangle(bounds,radius);
    if(!label.isBeingEdited()) {
        const auto font=getLabelFont(label);
        g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(label.isEnabled()?1.f:.5f));g.setFont(font);
        const auto area=getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
        g.drawFittedText(label.getText(),area,label.getJustificationType(),juce::jmax(1,int(float(area.getHeight())/font.getHeight())),label.getMinimumHorizontalScale());
    }
    if(!label.findColour(juce::Label::outlineColourId).isTransparent())drawEdge(g,bounds,radius);
}
void Theme::fillTextEditorBackground(juce::Graphics& g,int width,int height,juce::TextEditor& editor) {
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(juce::Rectangle<float>(0,0,float(width),float(height)).reduced(1),fieldRadius(float(height)-2));
}
void Theme::drawTextEditorOutline(juce::Graphics& g,int width,int height,juce::TextEditor& editor) {
    if(!editor.isEnabled())return;
    const auto bounds=juce::Rectangle<float>(0,0,float(width),float(height)).reduced(1);
    drawEdge(g,bounds,fieldRadius(bounds.getHeight()),editor.hasKeyboardFocus(true) && !editor.isReadOnly());
}
void Theme::drawButtonBackground(juce::Graphics& g,juce::Button& button,const juce::Colour& colour,bool hover,bool down) {
    const auto bounds=button.getLocalBounds().toFloat().reduced(1);const float radius=buttonRadius(bounds.getHeight());
    if(current.style.depth==ThemeStyle::Depth::Soft && button.isEnabled()){g.setColour(juce::Colours::black.withAlpha(darkTheme()?.35f:.12f));g.fillRoundedRectangle(bounds.translated(0,1),radius);}
    g.setColour(colour.withMultipliedAlpha(button.isEnabled()?1.f:.4f).brighter(down?.15f:hover?.07f:0));
    g.fillRoundedRectangle(bounds,radius);drawEdge(g,bounds,radius,button.hasKeyboardFocus(true));
}
void Theme::drawRotarySlider(juce::Graphics& g,int x,int y,int width,int height,float position,float start,float end,juce::Slider& slider) {
    if(!slider.getProperties()["endless"]) {
        // A thin track arc, the value lit along it in the accent (from the centre when the range
        // is centred on zero), and a disc with a pointer, like the sliders' thumbs.
        const auto bounds=juce::Rectangle<int>(x,y,width,height).toFloat().reduced(2);
        const float radius=std::min(bounds.getWidth(),bounds.getHeight())/2,line=std::max(2.f,radius*.14f);
        const auto centre=bounds.getCentre();const float angle=start+position*(end-start);
        auto arc=[&](float from,float to,juce::Colour colour){juce::Path p;p.addCentredArc(centre.x,centre.y,radius-line/2,radius-line/2,0,from,to,true);g.setColour(colour);g.strokePath(p,juce::PathStrokeType(line,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));};
        const float alpha=slider.isEnabled()?1.f:.5f;
        arc(start,end,ink(Ink::Border).withAlpha(.55f*alpha));
        const bool bipolar=slider.getProperties()["bipolar"];const float from=bipolar?(start+end)/2:start;
        if(std::abs(angle-from)>.001f)arc(std::min(from,angle),std::max(from,angle),slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(alpha));
        const auto disc=juce::Rectangle<float>(radius*1.3f,radius*1.3f).withCentre(centre);
        if(current.style.depth==ThemeStyle::Depth::Soft){g.setColour(juce::Colours::black.withAlpha(darkTheme()?.4f:.15f));g.fillEllipse(disc.translated(0,1));}
        g.setColour(current.style.sliders==ThemeStyle::Sliders::Filled?ink(Ink::Raised):juce::Colours::white.withMultipliedAlpha(alpha));g.fillEllipse(disc);
        g.setColour(ink(Ink::Border).withAlpha(.8f));g.drawEllipse(disc.reduced(.5f),1.f);
        const auto tip=centre.getPointOnCircumference(radius*.55f,angle),root=centre.getPointOnCircumference(radius*.15f,angle);
        g.setColour(current.style.sliders==ThemeStyle::Sliders::Filled?ink(Ink::Text):ink(Ink::Background));
        g.drawLine({root,tip},std::max(1.5f,radius*.1f));
        return;
    }
    const auto bounds=juce::Rectangle<int>(x,y,width,height).toFloat().reduced(4);const auto radius=std::min(bounds.getWidth(),bounds.getHeight())/2,centreX=bounds.getCentreX(),centreY=bounds.getCentreY();
    const int on=slider.isEnabled()?juce::roundToInt(position*16):0;
    for(int i=0;i<16;++i){const auto at=juce::Point<float>(centreX,centreY).getPointOnCircumference(radius*.9f,start+(end-start)*(i+.5f)/16);g.setColour(ink(i<on?Ink::Accent:Ink::Border));g.fillEllipse(juce::Rectangle<float>(radius*.14f,radius*.14f).withCentre(at));}
    const auto knob=juce::Rectangle<float>(radius*1.4f,radius*1.4f).withCentre({centreX,centreY});
    g.setColour(ink(Ink::Surface));g.fillEllipse(knob);g.setColour(ink(Ink::Border));g.drawEllipse(knob,radius*.05f);
}
}

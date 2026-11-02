// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../Theme.h"
#include "../Icons.h"
#include "../Splash.h"
#include <algorithm>
#include <iostream>
using namespace oscairn::standalone;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
juce::String face(Theme& theme,bool bold=false){
    const auto f=theme.getTypefaceForFont(juce::Font(juce::FontOptions(14.f,bold?juce::Font::bold:juce::Font::plain)));
    return f?f->getName()+"/"+f->getStyle():juce::String();
}
// The splash at a size, drawn with the current theme: something besides its background where the
// mark goes (the top part), written as name.png into the snapshot folder when one is given.
juce::File snapshots;
void splash(const juce::String& name,int w,int h,const juce::String& status={}){
    Splash s;s.setSize(w,h);if(status.isNotEmpty())s.setStatus(status);
    const auto image=s.createComponentSnapshot(s.getLocalBounds(),true,1.f);
    const auto background=image.getPixelAt(0,0);int marked=0;
    for(int y=0;y<h*45/100;++y)for(int x=0;x<w;++x)marked+=image.getPixelAt(x,y)!=background;
    check(marked>w*h/200,"the splash draws its mark");
    if(snapshots==juce::File())return;
    const auto file=snapshots.getChildFile(name+".png");file.deleteFile();
    juce::FileOutputStream out(file);check(out.openedOk() && juce::PNGImageFormat().writeImageToStream(image,out),"write a splash snapshot");
}
int main(int argc,char** argv){try{
    check(argc==3 || argc==4,"need built-in theme folder and example theme folder (and a folder for splash snapshots)");
    if(argc==4){snapshots=juce::File(argv[3]);check(snapshots.createDirectory().wasOk(),"create the snapshot folder");}
    const juce::File builtIn(argv[1]),example(argv[2]);const auto json=example.getChildFile("theme.json").loadFileAsString();
    // The splash: its image and colors, each optional, and refused when unsafe or unreadable.
    {const auto s=ThemeData::parse(json).splash;check(s.image=="splash.svg" && s.background==juce::Colour(0xff0a101b) && s.ink==juce::Colour(0xfff0f5fc),"the splash fields parsed");
     const auto none=ThemeData::parse(builtIn.getChildFile("oscairn-dark/theme.json").loadFileAsString()).splash;check(none.image.isEmpty() && !none.background && !none.ink,"without splash fields, the built-in splash");
     const auto colors=ThemeData::parse(json.replace("\"image\": \"splash.svg\",","")).splash;check(colors.image.isEmpty() && colors.background,"splash colors without an image");}
    for(auto broken:{json.replace("\"image\": \"splash.svg\"","\"image\": \"splash.gif\""),json.replace("\"image\": \"splash.svg\"","\"image\": 3"),
                     json.replace("\"ink\": \"#F0F5FC\"","\"ink\": \"#0A101B\""),json.replace("\"background\": \"#0A101B\"","\"background\": \"navy\""),
                     json.replace("\"ink\": \"#F0F5FC\"","\"ink\": \"#F0F5FC\", \"script\": \"x\""),json.upToFirstOccurrenceOf(",\n  \"splash\"",false,false)+",\n  \"splash\": 3\n}"}){
        bool rejected=false;try{ThemeData::parse(broken);}catch(const std::exception&){rejected=true;}check(rejected,"invalid splash fields rejected");
    }
    const auto theme=ThemeData::parse(json);check(theme.id=="midnight" && theme.radius==8 && theme.font.regular=="fonts/Inter-Regular.ttf","valid theme parsed");
    check(theme.style.buttons==ThemeStyle::Buttons::Pill && theme.style.tabs==ThemeStyle::Tabs::Segmented && theme.style.sliders==ThemeStyle::Sliders::Line,"its style parsed");
    const auto classic=ThemeData::parse(json.replace("\"pill\"","\"rounded\"").replace("\"hairline\"","\"outlined\"").replace("\"soft\"","\"flat\"").replace("\"line\"","\"filled\"").replace("\"segmented\"","\"folder\"").replace("\"square\"","\"round\"").replace("\"separate\"","\"joined\""));
    check(classic.style.buttons==ThemeStyle::Buttons::Rounded && classic.style.edges==ThemeStyle::Edges::Outlined && classic.style.depth==ThemeStyle::Depth::Flat &&
          classic.style.sliders==ThemeStyle::Sliders::Filled && classic.style.tabs==ThemeStyle::Tabs::Folder && classic.style.marks==ThemeStyle::Marks::Round && classic.style.sections==ThemeStyle::Sections::Joined,"every style choice parsed");
    check(ThemeData::parse(json.replace("\"line\"","\"cap\"")).style.sliders==ThemeStyle::Sliders::Cap,"slider caps parsed");
    check(theme.style.rows==ThemeStyle::Rows::Dot && ThemeData::parse(json.replace("\"dot\"","\"fill\"")).style.rows==ThemeStyle::Rows::Fill,"row color placement parsed");
    for(auto broken:{json.replace("\"midnight\"","\"../escape\""),json.replace("\"version\": 7","\"version\": 6"),json.replace("\"fontScale\": 1.0","\"fontScale\": 100"),
                     json.replace("#F0F5FC","#101724"),json.replace("#7CCEFF","red"),json.replace("\"radius\": 8","\"script\": \"evil\""),
                     json.replace("\"license\": \"fonts/OFL.txt\"","\"licenses\": \"fonts/OFL.txt\""),json.replace("\"regular\": \"fonts/Inter-Regular.ttf\",",""),
                     json.replace("\"tabs\": \"segmented\"","\"tabs\": \"ribbon\""),json.replace("\"marks\": \"square\"","\"marks\": \"square\", \"shader\": \"x\""),
                     json.replace("\"buttons\": \"pill\",",""),json.replace("\"rows\": \"dot\"","\"rows\": \"stripe\"")}){
        bool rejected=false;try{ThemeData::parse(broken);}catch(const std::exception&){rejected=true;}check(rejected,"invalid or unreadable theme rejected");
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("oscairn-theme-test","",false);
    {Theme fresh(root,builtIn);
        check(themeData().id=="oscairn-dark" && fresh.error().isEmpty(),"built-in default theme applied on first launch");
        check(face(fresh)=="Inter/Regular" && face(fresh,true)=="Inter/Bold","theme fonts used for regular and bold text");
        const auto list=fresh.installed();
        check(list.size()==4 && std::all_of(list.begin(),list.end(),[](const auto& t){return t.first.startsWith("oscairn-");}),"built-in themes listed from their folders");
        // The mark is embedded with the icons, each version an SVG JUCE reads.
        for(const char* version:{"-on-dark.svg","-on-light.svg","-mono.svg"}){
            const auto file=juce::String(markName)+version;const char* found=nullptr;int size=0;
            for(int i=0;i<IconData::namedResourceListSize;++i)if(file==IconData::getNamedResourceOriginalFilename(IconData::namedResourceList[i]))found=IconData::getNamedResource(IconData::namedResourceList[i],size);
            check(found!=nullptr && juce::Drawable::createFromSVGString(juce::String::fromUTF8(found,size).replace("currentColor","#000000"))!=nullptr,"the built-in mark loads from BinaryData");
        }
        // Each built-in theme's splash: the built-in mark in the theme's colors, in a window and on a panel.
        for(const auto& t:list){fresh.apply(t.first);check(splashImage()==nullptr,"built-in themes use the built-in mark");splash(t.first+"-desktop",420,300);splash(t.first+"-panel",1280,800);}
        fresh.apply("oscairn-dark");splash("oscairn-dark-error",420,300,"Could not start the engine: the helper exited");}
    {Theme installed(root,builtIn);installed.install(example.getChildFile("theme.json"));check(themeData().id=="midnight","install applies theme");
        const auto copy=installed.directory().getChildFile("midnight");
        check(copy.getChildFile("theme.json").loadFileAsString()==json && copy.getChildFile("fonts/Inter-Regular.ttf").hasIdenticalContentTo(example.getChildFile("fonts/Inter-Regular.ttf")),"install copies the theme and its fonts");
        installed.install(example);check(installed.installed().size()==5,"reinstall is idempotent");
        check(face(installed)=="Inter/Regular","installed theme uses its own font");
        check(splashImage()!=nullptr && copy.getChildFile("splash.svg").hasIdenticalContentTo(example.getChildFile("splash.svg")),"install copies and loads the theme's splash image");
        splash("midnight-desktop",420,300);splash("midnight-panel",1280,800);
        auto refused=[&](const juce::File& folder){bool r=false;try{installed.install(folder);}catch(const std::exception&){r=true;}return r && themeData().id=="midnight";};
        auto variant=[&](const juce::String& id,const juce::String& text){
            const auto folder=root.getChildFile("sources").getChildFile(id);check(example.copyDirectoryTo(folder),"copy example");
            check(folder.getChildFile("theme.json").replaceWithText(text.replace("\"midnight\"","\""+id+"\"")),"write variant");return folder;};
        check(refused(variant("oscairn-dark",json)),"built-in theme IDs are reserved");
        check(refused(variant("escape",json.replace("fonts/Inter-Regular.ttf","../midnight/fonts/Inter-Regular.ttf"))),"font paths cannot leave the theme");
        {const auto fake=variant("fake-font",json);check(fake.getChildFile("fonts/Inter-Regular.ttf").replaceWithText("not a font"),"write fake font");check(refused(fake),"unreadable fonts are rejected");}
        {const auto linked=variant("linked-font",json);const auto font=linked.getChildFile("fonts/Inter-Regular.ttf");font.deleteFile();
            check(example.getChildFile("fonts/Inter-Regular.ttf").createSymbolicLink(font,true),"create link");check(refused(linked),"linked font files are rejected");}
        check(refused(variant("no-license",json.replace("fonts/OFL.txt","fonts/missing.txt"))),"a theme must carry its font license");
        check(refused(variant("no-splash",json.replace("\"splash.svg\"","\"missing.svg\""))),"a splash image must be in the theme");
        check(refused(variant("escape-splash",json.replace("\"splash.svg\"","\"../midnight/splash.svg\""))),"splash image paths cannot leave the theme");
        {const auto fake=variant("fake-splash",json);check(fake.getChildFile("splash.svg").replaceWithText("not an image"),"write fake splash");check(refused(fake),"unreadable splash images are rejected");}
        installed.apply("oscairn-light");check(themeData().id=="oscairn-light" && splashImage()==nullptr,"built-in selection applied");
        bool refusedId=false;try{installed.apply("../escape");}catch(const std::exception&){refusedId=true;}check(refusedId && themeData().id=="oscairn-light","invalid selection preserves active theme");}
    const auto selection=root.getChildFile("appearance.txt");selection.setLastModificationTime(juce::Time(1000000000));const auto saved=selection.getLastModificationTime();
    {Theme reopened(root,builtIn);check(themeData().id=="oscairn-light","selection persists across UI restart");check(selection.getLastModificationTime()==saved,"loading does not rewrite preferences");}
    selection.replaceWithText("missing-theme");
    {Theme broken(root,builtIn);check(themeData().id=="oscairn-dark" && broken.error().isNotEmpty() && face(broken)=="Inter/Regular","missing theme falls back to the default theme with an error");}
    {const auto custom=root.getChildFile("builder");check(builtIn.copyDirectoryTo(custom) && custom.getChildFile("default").replaceWithText("oscairn-light\n"),"copy built-ins");
        selection.deleteFile();Theme builder(root,custom);check(themeData().id=="oscairn-light","the built-in default file chooses the first theme");}
    {Theme emergency(root,root.getChildFile("no-built-in-themes"));
        check(themeData().id=="oscairn-dark" && face(emergency).isNotEmpty(),"without built-in themes the emergency theme uses the platform font");}
    root.deleteRecursively();
    std::cout<<"ok: theme folders, bundled fonts, path-safe IDs, bounded files, readable text, installation and persistence\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

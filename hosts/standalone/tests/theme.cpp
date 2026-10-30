// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../Theme.h"
#include <iostream>
using namespace oscairn::standalone;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
juce::String face(Theme& theme,bool bold=false){
    const auto f=theme.getTypefaceForFont(juce::Font(juce::FontOptions(14.f,bold?juce::Font::bold:juce::Font::plain)));
    return f?f->getName()+"/"+f->getStyle():juce::String();
}
int main(int argc,char** argv){try{
    check(argc==3,"need built-in theme folder and example theme folder");
    const juce::File builtIn(argv[1]),example(argv[2]);const auto json=example.getChildFile("theme.json").loadFileAsString();
    const auto theme=ThemeData::parse(json);check(theme.id=="midnight" && theme.radius==8 && theme.font.regular=="fonts/Inter-Regular.ttf","valid theme parsed");
    for(auto broken:{json.replace("\"midnight\"","\"../escape\""),json.replace("\"version\": 2","\"version\": 1"),json.replace("\"fontScale\": 1.0","\"fontScale\": 100"),
                     json.replace("#F0F5FC","#101724"),json.replace("#7CCEFF","red"),json.replace("\"radius\": 8","\"script\": \"evil\""),
                     json.replace("\"license\": \"fonts/OFL.txt\"","\"licence\": \"fonts/OFL.txt\""),json.replace("\"regular\": \"fonts/Inter-Regular.ttf\",","")}){
        bool rejected=false;try{ThemeData::parse(broken);}catch(const std::exception&){rejected=true;}check(rejected,"invalid or unreadable theme rejected");
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("oscairn-theme-test","",false);
    {Theme fresh(root,builtIn);
        check(themeData().id=="oscairn-dark" && fresh.error().isEmpty(),"built-in default theme applied on first launch");
        check(face(fresh)=="Inter/Regular" && face(fresh,true)=="Inter/Bold","theme fonts used for regular and bold text");
        const auto list=fresh.installed();
        check(list.size()==2 && list[0].first.startsWith("oscairn-") && list[1].first.startsWith("oscairn-"),"built-in themes listed from their folders");}
    {Theme installed(root,builtIn);installed.install(example.getChildFile("theme.json"));check(themeData().id=="midnight","install applies theme");
        const auto copy=installed.directory().getChildFile("midnight");
        check(copy.getChildFile("theme.json").loadFileAsString()==json && copy.getChildFile("fonts/Inter-Regular.ttf").hasIdenticalContentTo(example.getChildFile("fonts/Inter-Regular.ttf")),"install copies the theme and its fonts");
        installed.install(example);check(installed.installed().size()==3,"reinstall is idempotent");
        check(face(installed)=="Inter/Regular","installed theme uses its own font");
        auto refused=[&](const juce::File& folder){bool r=false;try{installed.install(folder);}catch(const std::exception&){r=true;}return r && themeData().id=="midnight";};
        auto variant=[&](const juce::String& id,const juce::String& text){
            const auto folder=root.getChildFile("sources").getChildFile(id);check(example.copyDirectoryTo(folder),"copy example");
            check(folder.getChildFile("theme.json").replaceWithText(text.replace("\"midnight\"","\""+id+"\"")),"write variant");return folder;};
        check(refused(variant("oscairn-dark",json)),"built-in theme IDs are reserved");
        check(refused(variant("escape",json.replace("fonts/Inter-Regular.ttf","../midnight/fonts/Inter-Regular.ttf"))),"font paths cannot leave the theme");
        {const auto fake=variant("fake-font",json);check(fake.getChildFile("fonts/Inter-Regular.ttf").replaceWithText("not a font"),"write fake font");check(refused(fake),"unreadable fonts are rejected");}
        {const auto linked=variant("linked-font",json);const auto font=linked.getChildFile("fonts/Inter-Regular.ttf");font.deleteFile();
            check(example.getChildFile("fonts/Inter-Regular.ttf").createSymbolicLink(font,true),"create link");check(refused(linked),"linked font files are rejected");}
        check(refused(variant("no-license",json.replace("fonts/OFL.txt","fonts/missing.txt"))),"a theme must carry its font licence");
        installed.apply("oscairn-light");check(themeData().id=="oscairn-light","built-in selection applied");
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

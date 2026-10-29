// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../Theme.h"
#include <iostream>
using namespace oscairn::standalone;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    check(argc==2,"need example theme");const auto json=juce::File(argv[1]).loadFileAsString();
    const auto theme=ThemeData::parse(json);check(theme.id=="midnight" && theme.radius==8,"valid theme parsed");
    for(auto broken:{json.replace("\"midnight\"","\"../escape\""),json.replace("\"version\": 1","\"version\": 2"),json.replace("\"fontScale\": 1.0","\"fontScale\": 100"),json.replace("#F0F5FC","#101724"),json.replace("#7CCEFF","red"),json.replace("\"radius\": 8","\"script\": \"evil\"")}){
        bool rejected=false;try{ThemeData::parse(broken);}catch(const std::exception&){rejected=true;}check(rejected,"invalid or unreadable theme rejected");
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("oscairn-theme-test", "", false);
    {Theme installed(root);installed.install(juce::File(argv[1]));check(themeData().id=="midnight","install applies theme");
        check(installed.directory().getChildFile("midnight.json").loadFileAsString()==json,"install copies original data");
        installed.install(juce::File(argv[1]));check(installed.installed().size()==3,"reinstall is idempotent");
        installed.apply("oscairn-light");check(themeData().id=="oscairn-light","built-in selection applied");
        bool refused=false;try{installed.apply("../escape");}catch(const std::exception&){refused=true;}check(refused && themeData().id=="oscairn-light","invalid selection preserves active theme");}
    const auto selection=root.getChildFile("appearance.txt");selection.setLastModificationTime(juce::Time(1000000000));const auto saved=selection.getLastModificationTime();
    {Theme reopened(root);check(themeData().id=="oscairn-light","selection persists across UI restart");check(selection.getLastModificationTime()==saved,"loading does not rewrite preferences");}
    selection.replaceWithText("missing-theme");
    {Theme broken(root);check(themeData().id=="oscairn-dark" && broken.error().isNotEmpty(),"missing theme falls back with an error");}
    root.deleteRecursively();
    std::cout<<"ok: theme schema, path-safe IDs, bounded metrics, readable text, installation and persistence\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

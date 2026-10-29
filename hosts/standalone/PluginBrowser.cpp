// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginBrowser.h"
#include <algorithm>
namespace oscairn::standalone {
namespace {
juce::String format(const kick::clap::Descriptor& p){return std::filesystem::path(p.path).extension()==".lv2"?"LV2":"CLAP";}
}
PluginBrowser::PluginBrowser(Engine& engine,Theme& theme,const juce::File& plugins):engine_(engine),theme_(theme) {
    for(auto* c:std::initializer_list<juce::Component*>{&title_,&loaded_,&hint_,&target_,&appearance_,&presets_,&search_,&parameterSearch_,&messages_,&scanButton_,&folderButton_,&loadButton_,&presetButton_,&themeButton_,&list_,&viewport_})addAndMakeVisible(c);
    title_.setText("Plugins",juce::dontSendNotification);title_.setFont(juce::FontOptions(24,juce::Font::bold));
    target_.addItem("Instrument",1);target_.addItem("Effect",2);target_.setSelectedId(1);target_.setName("Plugin slot");
    target_.onChange=[this]{filter();editorKey_.clear();editor();};
    appearance_.setName("System theme");themes();appearance_.onChange=[this]{const auto i=appearance_.getSelectedItemIndex();if(i>=0 && size_t(i)<themes_.size())try{theme_.apply(themes_[size_t(i)].first);}catch(const std::exception& e){messages_.setText(e.what());}};
    themeButton_.onClick=[this]{chooser_=std::make_unique<juce::FileChooser>("Install an Oscairn theme",juce::File(),"*.json");chooser_->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe=juce::Component::SafePointer<PluginBrowser>(this)](const auto& fc){if(!safe || fc.getResult()==juce::File())return;try{safe->theme_.install(fc.getResult());safe->themes();safe->messages_.setText("Theme installed and applied throughout Oscairn.");}catch(const std::exception& e){safe->messages_.setText(e.what());}});};
    search_.setTextToShowWhenEmpty("Search name, maker or format",ink(Ink::Muted));search_.setName("Search plugins");search_.onTextChange=[this]{filter();};
    parameterSearch_.setTextToShowWhenEmpty("Search parameters",ink(Ink::Muted));parameterSearch_.setName("Search parameters");parameterSearch_.onTextChange=[this]{editorKey_.clear();editor();};
    messages_.setMultiLine(true);messages_.setReadOnly(true);messages_.setName("Scan and load status");
    hint_.setText("Replacement stops this slot's notes/tails and removes its prepared sounds. Failed loads keep the current plugin.",juce::dontSendNotification);
    hint_.setTooltip(hint_.getText());
    viewport_.setViewedComponent(&parameters_,false);list_.setRowHeight(54);
    sources_=theme.directory().getParentDirectory().getChildFile("plugin-folders.txt");
    roots_=pluginSearchPaths();roots_.emplace_back(plugins.getFullPathName().toStdString());
    for(const auto& path:juce::StringArray::fromLines(sources_.loadFileAsString()))if(path.isNotEmpty())roots_.emplace_back(path.toStdString());
    scanButton_.onClick=[this]{if(scan_.valid()){cancelled_=true;scanButton_.setEnabled(false);}else scan();};
    folderButton_.onClick=[this]{chooser_=std::make_unique<juce::FileChooser>("Add a plugin folder");chooser_->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories,[safe=juce::Component::SafePointer<PluginBrowser>(this)](const auto& fc){if(!safe || fc.getResult()==juce::File())return;const auto path=fc.getResult().getFullPathName();safe->roots_.emplace_back(path.toStdString());safe->sources_.getParentDirectory().createDirectory();auto paths=juce::StringArray::fromLines(safe->sources_.loadFileAsString());paths.addIfNotAlreadyThere(path);if(!safe->sources_.replaceWithText(paths.joinIntoString("\n")))safe->messages_.setText("Folder added for this session; could not save folder settings.");safe->scan();});};
    loadButton_.onClick=[this]{const auto row=list_.getSelectedRow();if(row<0 || size_t(row)>=visible_.size())return;const auto& plugin=catalog_.plugins[visible_[size_t(row)]];engine_.replacePlugin(slot(),plugin.path,plugin.id);};
    presetButton_.onClick=[this]{const auto it=engine_.editors().find(slot());const auto i=presets_.getSelectedItemIndex();if(it==engine_.editors().end() || i<0 || size_t(i)>=it->second.presets.size())return;const auto& p=it->second;engine_.replacePlugin(slot(),p.path,p.id,p.presets[size_t(i)].uri);};
    setSize(1060,760);startTimer(100);scan();
}
PluginBrowser::~PluginBrowser(){stopTimer();cancelled_=true;if(scan_.valid())scan_.wait();viewport_.setViewedComponent(nullptr,false);}
void PluginBrowser::themes(){themes_=theme_.installed();appearance_.clear(juce::dontSendNotification);for(size_t i=0;i<themes_.size();++i){appearance_.addItem(themes_[i].second,int(i)+1);if(themes_[i].first==themeData().id)appearance_.setSelectedId(int(i)+1,juce::dontSendNotification);}}
void PluginBrowser::scan() {
    if(scan_.valid())return;cancelled_=false;scanButton_.setButtonText("Cancel scan");folderButton_.setEnabled(false);messages_.setText("Scanning in isolated workers. Playback continues.");
    const auto helper=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("oscairn-plugin-scan").getFullPathName().toStdString();
    scan_=std::async(std::launch::async,[this,roots=roots_,helper]{return scanPlugins(roots,helper,&cancelled_);});
}
void PluginBrowser::filter() {
    visible_.clear();const auto query=search_.getText().trim();
    for(size_t i=0;i<catalog_.plugins.size();++i){const auto& p=catalog_.plugins[i];const auto kind=slot()=="synth"?"instrument":"audio-effect";
        if(std::find(p.features.begin(),p.features.end(),kind)==p.features.end())continue;
        if((juce::String(p.name)+" "+p.vendor+" "+format(p)).containsIgnoreCase(query))visible_.push_back(i);}
    list_.deselectAllRows();list_.updateContent();list_.repaint();selectedRowsChanged(-1);
}
void PluginBrowser::paintListBoxItem(int row,juce::Graphics& g,int w,int h,bool selected) {
    if(row<0 || size_t(row)>=visible_.size())return;const auto& p=catalog_.plugins[visible_[size_t(row)]];
    g.fillAll(ink(selected?Ink::Raised:Ink::Surface));if(selected){g.setColour(ink(Ink::Accent));g.fillRect(0,0,3,h);}
    g.setColour(ink(Ink::Text));g.setFont(juce::FontOptions(16*themeData().fontScale,juce::Font::bold));g.drawText(p.name,12,3,w-24,25,juce::Justification::centredLeft);
    g.setColour(ink(Ink::Muted));g.setFont(juce::FontOptions(13*themeData().fontScale));g.drawText(format(p)+" | "+p.vendor,12,28,w-24,21,juce::Justification::centredLeft);
}
juce::String PluginBrowser::getNameForRow(int row){return row>=0 && size_t(row)<visible_.size()?juce::String(catalog_.plugins[visible_[size_t(row)]].name):juce::String();}
void PluginBrowser::lookAndFeelChanged(){search_.setTextToShowWhenEmpty("Search name, maker or format",ink(Ink::Muted));parameterSearch_.setTextToShowWhenEmpty("Search parameters",ink(Ink::Muted));}
void PluginBrowser::selectedRowsChanged(int row){loadButton_.setEnabled(row>=0 && engine_.connected() && !engine_.soundBusy() && engine_.canPrepareSounds());loadButton_.setButtonText(slot()=="synth"?"Replace instrument":"Replace effect");if(row>=0 && size_t(row)<visible_.size()){const auto& p=catalog_.plugins[visible_[size_t(row)]];list_.setTooltip(juce::String(p.id)+"\n"+p.path);}}
void PluginBrowser::editor() {
    const auto it=engine_.editors().find(slot());if(it==engine_.editors().end())return;
    const auto& plugin=it->second;const auto key=slot()+plugin.path+plugin.id+parameterSearch_.getText().toStdString();if(key==editorKey_)return;editorKey_=key;
    rows_.clear();loaded_.setText("Loaded: "+juce::String(plugin.name),juce::dontSendNotification);
    presets_.clear(juce::dontSendNotification);for(size_t i=0;i<plugin.presets.size();++i)presets_.addItem(plugin.presets[i].name,int(i)+1);presets_.setTextWhenNothingSelected("Choose a preset");presets_.setName("Plugin preset");
    for(const auto& p:plugin.parameters) {
        if(!juce::String(p.name).containsIgnoreCase(parameterSearch_.getText().trim()))continue;
        Row row;row.info=p;row.label=std::make_unique<juce::Label>();row.label->setText(p.name,juce::dontSendNotification);
        row.slider=std::make_unique<juce::Slider>();auto* slider=row.slider.get();slider->setName(p.name);slider->setSliderStyle(juce::Slider::LinearHorizontal);slider->setTextBoxStyle(juce::Slider::TextBoxRight,true,110,28);
        if(p.maximum>p.minimum)slider->setRange(p.minimum,p.maximum,p.stepped?1:0);
        slider->setDoubleClickReturnValue(true,p.defaultValue);slider->setValue(engine_.param(slot(),p.id),juce::dontSendNotification);
        const auto node=slot();slider->textFromValueFunction=[this,node,id=p.id](double v){return juce::String(engine_.paramText(node,id,v));};
        slider->updateText();
        slider->onValueChange=[this,node,id=p.id,slider]{engine_.setParam(node,id,slider->getValue());};
        slider->onDragStart=[this,node,id=p.id]{engine_.paramGesture(node,id,true);};slider->onDragEnd=[this,node,id=p.id]{engine_.paramGesture(node,id,false);};
        parameters_.addAndMakeVisible(*row.label);parameters_.addAndMakeVisible(*row.slider);rows_.push_back(std::move(row));
    }
    resized();
}
void PluginBrowser::timerCallback() {
    if(scan_.valid() && scan_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try{catalog_=scan_.get();std::sort(catalog_.plugins.begin(),catalog_.plugins.end(),[](const auto& a,const auto& b){return a.name<b.name;});
            juce::String message=cancelled_?"Scan cancelled. ":"Scan complete. ";message+=juce::String(catalog_.plugins.size())+" plugins; "+juce::String(catalog_.failures.size())+" scan issues.";
            for(const auto& failure:catalog_.failures)message+="\n"+juce::String(failure.path)+": "+failure.message;messages_.setText(message);filter();
        }catch(const std::exception& e){messages_.setText(e.what());}
        scanButton_.setButtonText("Scan plugins");scanButton_.setEnabled(true);folderButton_.setEnabled(true);
    }
    if(appearance_.getSelectedItemIndex()<0 || themes_[size_t(appearance_.getSelectedItemIndex())].first!=themeData().id)themes();
    editor();const bool enabled=engine_.connected() && !engine_.soundBusy();
    for(auto& row:rows_){row.slider->setEnabled(enabled && !(row.info.flags&CLAP_PARAM_IS_READONLY) && row.info.maximum>row.info.minimum);if(!row.slider->isMouseButtonDown())row.slider->setValue(engine_.param(slot(),row.info.id),juce::dontSendNotification);if(engine_.controlReady())row.slider->updateText();}
    selectedRowsChanged(list_.getSelectedRow());presetButton_.setEnabled(enabled && engine_.canPrepareSounds() && presets_.getSelectedId()>0);
    if(lastError_!=engine_.soundMessage()){lastError_=engine_.soundMessage();if(!lastError_.empty())messages_.setText(lastError_);}
    target_.setEnabled(enabled);repaint();
}
void PluginBrowser::paint(juce::Graphics& g){g.fillAll(ink(Ink::Background));}
void PluginBrowser::resized() {
    auto area=getLocalBounds().reduced(16);auto header=area.removeFromTop(36);title_.setBounds(header.removeFromLeft(150));themeButton_.setBounds(header.removeFromRight(150));header.removeFromRight(8);appearance_.setBounds(header.removeFromRight(200));area.removeFromTop(12);
    auto footer=area.removeFromBottom(106);hint_.setBounds(footer.removeFromTop(30));messages_.setBounds(footer.reduced(0,4));area.removeFromBottom(8);
    auto left=area.removeFromLeft(area.getWidth()*42/100);area.removeFromLeft(16);auto right=area;
    auto bar=left.removeFromTop(34);target_.setBounds(bar.removeFromLeft(130));bar.removeFromLeft(8);search_.setBounds(bar);left.removeFromTop(8);
    auto actions=left.removeFromBottom(34);loadButton_.setBounds(actions.removeFromRight(160));actions.removeFromRight(8);scanButton_.setBounds(actions.removeFromLeft(110));actions.removeFromLeft(8);folderButton_.setBounds(actions);left.removeFromBottom(8);list_.setBounds(left);
    loaded_.setBounds(right.removeFromTop(34));right.removeFromTop(8);auto presets=right.removeFromTop(34);presetButton_.setBounds(presets.removeFromRight(160));presets.removeFromRight(8);presets_.setBounds(presets);right.removeFromTop(8);parameterSearch_.setBounds(right.removeFromTop(32));right.removeFromTop(8);viewport_.setBounds(right);
    const auto width=std::max(100,right.getWidth()-16);parameters_.setSize(width,int(rows_.size())*64);
    for(size_t i=0;i<rows_.size();++i){rows_[i].label->setBounds(0,int(i)*64,width,24);rows_[i].slider->setBounds(0,int(i)*64+24,width,34);}
}
}

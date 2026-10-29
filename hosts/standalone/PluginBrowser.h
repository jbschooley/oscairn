// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Theme.h"
#include <oscairn/discovery.h>
namespace oscairn::standalone {
class PluginBrowser final : public juce::Component, private juce::ListBoxModel, private juce::Timer {
public:
    PluginBrowser(Engine&,Theme&,const juce::File& bundledPlugins);
    ~PluginBrowser() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void lookAndFeelChanged() override;
private:
    int getNumRows() override {return static_cast<int>(visible_.size());}
    void paintListBoxItem(int,juce::Graphics&,int,int,bool) override;
    void selectedRowsChanged(int) override;
    juce::String getNameForRow(int row) override;
    void timerCallback() override;
    void scan();
    void filter();
    void editor();
    void themes();
    std::string slot() const {return target_.getSelectedId()==2?"delay":"synth";}
    struct Row {Control::ParamInfo info;std::unique_ptr<juce::Label> label;std::unique_ptr<juce::Slider> slider;};
    Engine& engine_;Theme& theme_;
    std::vector<std::filesystem::path> roots_;
    juce::File sources_;
    std::future<kick::clap::ScanResult> scan_;
    std::atomic<bool> cancelled_{false};
    kick::clap::ScanResult catalog_;
    std::vector<size_t> visible_;
    std::vector<std::pair<juce::String,juce::String>> themes_;
    juce::Label title_,loaded_,hint_;
    juce::ComboBox target_,appearance_,presets_;
    juce::TextEditor search_,parameterSearch_,messages_;
    juce::TextButton scanButton_{"Scan plugins"},folderButton_{"Add folder..."},loadButton_{"Replace instrument"},presetButton_{"Reload with preset"},themeButton_{"Install theme..."};
    juce::ListBox list_{"Available plugins",this};
    juce::Viewport viewport_;
    juce::Component parameters_;
    std::vector<Row> rows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::string editorKey_,lastError_;
};
}

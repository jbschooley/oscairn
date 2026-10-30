// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SoundPickerModel.h"
#include "../../plugins/step-sequencer/Parameters.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace oscairn::standalone
{
    SoundPickerState readSoundPicker(Control& control)
    {
        SoundPickerState result;
        try{result.node = control.nodeId("synth");}catch(const std::invalid_argument&){return result;}
        const auto graph = control.graphState();
        for(const auto& node : graph.topology.nodes)
            if(node.id == result.node)
            {
                result.pluginId = node.plugin.id;
                result.instrument = node.plugin.name;
            }
        result.sounds = control.sounds(result.node);
        if(result.pluginId == "org.oscairn.basic-synth")
        {
            result.choices = {
                {"Warm", {}, {{1, .15}, {2, .7}, {3, .5}}},
                {"Bright", {}, {{1, .9}, {2, .7}, {3, .5}}},
                {"Pressure swell", {}, {{1, .05}, {2, .7}, {3, 1.0}}},
                {"Detuned saws", {}, {{1, .65}, {2, .5}, {3, .5}, {4, 1}, {5, 7}}},
                {"Square vibrato", {}, {{1, .8}, {2, .4}, {3, .5}, {4, 2}, {6, 18}, {7, 70}}}
            };
            for(const auto& node : graph.topology.nodes)
                if(node.plugin.id == "org.oscairn.step-sequencer" && graph.names.at(node.id) == "sequencer")
                {
                    result.canToggleBrightnessLocks.fill(true);
                    for(const auto& mapping : graph.topology.mappings)
                        if(mapping.source == node.id && mapping.lane < result.brightnessLocks.size())
                        {
                            result.brightnessLocks[mapping.lane] = mapping.target == result.node && mapping.param == 1;
                            result.canToggleBrightnessLocks[mapping.lane] = result.brightnessLocks[mapping.lane];
                        }
                }
        }
        else if(result.pluginId == "org.oscairn.sampler")
            result.importsSamples = true;
        else if(result.sounds.capacity)
        {
            result.choices.push_back({"Default", {}, {}});
            for(const auto& preset : control.presets(result.node))
                result.choices.push_back({preset.name, preset.uri, {}});
        }
        return result;
    }

    SoundChoice importSound(const std::string& path)
    {
        const std::filesystem::path file(path);
        auto extension = file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        SoundChoice choice;
        choice.name = file.stem().string();
        if(choice.name.size() > 63)
        {
            size_t end = 63; // cut at a character boundary: names must stay valid UTF-8
            while(end > 0 && (static_cast<unsigned char>(choice.name[end]) & 0xC0) == 0x80) --end;
            choice.name.resize(end);
        }
        choice.directory = file.parent_path().string();
        // SFZ's default 1 ms release ticks when a sustained sample stops; give raw WAVs a short fade.
        if(extension == ".wav")
            choice.sfz = "<region> ampeg_release=0.03 sample=" + file.filename().string() + "\n";
        else if(extension == ".sfz")
        {
            std::error_code error;
            if(std::filesystem::file_size(file, error) > 64 * 1024 || error) throw std::runtime_error("That SFZ file is too large or unreadable.");
            std::ifstream in(file, std::ios::binary);std::ostringstream text;text << in.rdbuf();
            if(!in) throw std::runtime_error("Could not read that SFZ file.");
            choice.sfz = text.str();
        }
        else throw std::runtime_error("Choose a .wav sample or an .sfz patch.");
        return choice;
    }

    void applySoundAction(Control& control, const SoundAction& action)
    {
        if(action.kind == SoundAction::Refresh) return;
        if(action.kind == SoundAction::SavePerformance){control.savePerformance(action.directory);return;}
        if(action.kind == SoundAction::LoadPerformance){control.loadPerformance(action.directory);return;}
        if(action.kind==SoundAction::Replace) {
            const auto graph=control.graphState();
            const auto found=std::find_if(graph.topology.nodes.begin(),graph.topology.nodes.end(),[&](const auto& n){return n.id==action.node && n.plugin.id==action.pluginId;});
            if(found==graph.topology.nodes.end())throw std::runtime_error("The loaded plugin changed. Choose again.");
            control.replace(action.node,action.replacementPath,action.replacementId,action.choice.preset);return;
        }
        const auto current = readSoundPicker(control);
        if(current.node != action.node || current.pluginId != action.pluginId)
            throw std::runtime_error("The instrument changed. Choose a sound again.");
        if(action.kind == SoundAction::BrightnessLocks)
        {
            if(action.track>=current.canToggleBrightnessLocks.size() || !current.canToggleBrightnessLocks[action.track] || control.graphState().staged)
                throw std::runtime_error("Finish other routing edits before changing brightness locks.");
            const auto sequencer = control.nodeId("sequencer");
            if(action.enableLocks){
                if(!control.setParameter(sequencer,oscairn::sequencer::lockScope(action.track),1))
                    throw std::runtime_error("Could not set track lock scope. Try again.");
                control.mapParameter(sequencer,action.track,current.node,1);
            }
            else control.unmapParameter(sequencer,action.track);
            control.commit();
            return;
        }
        if(action.kind == SoundAction::Prepare)
        {
            const auto choice = action.importPath.empty() ? action.choice : importSound(action.importPath);
            control.prepareSound(current.node, choice.name, choice.parameters, {}, choice.preset, choice.sfz, choice.directory);
            return; // Preparation never changes the playing sound.
        }
        const auto found = std::find_if(current.sounds.contexts.begin(), current.sounds.contexts.end(),
            [&](const auto& sound) { return sound.id == action.context; });
        if(found == current.sounds.contexts.end())
            throw std::runtime_error("That sound is no longer prepared.");
        if(action.kind == SoundAction::Select)
        {
            if(!control.selectSound(current.node, action.context, action.channel))
                throw std::runtime_error("Sound change was refused. Try again.");
        }
        else
        {
            if(found->selectedChannels || found->activeVoices || action.context == 0 ||
               (!found->idle && !(action.cutTail && current.sounds.mode == "instances")))
                throw std::runtime_error("Keep this sound until it is unselected and its notes and tails have finished.");
            if(!control.discardSound(current.node, action.context, action.cutTail))
                throw std::runtime_error("Sound is still in use. Try again after its notes finish.");
        }
    }
}

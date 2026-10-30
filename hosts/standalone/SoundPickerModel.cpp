// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SoundPickerModel.h"
#include "../../plugins/step-sequencer/Parameters.h"
#include <algorithm>
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
        else if(result.sounds.capacity)
        {
            result.choices.push_back({"Default", {}, {}});
            for(const auto& preset : control.presets(result.node))
                result.choices.push_back({preset.name, preset.uri, {}});
        }
        return result;
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
            control.prepareSound(current.node, action.choice.name, action.choice.parameters, {}, action.choice.preset);
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

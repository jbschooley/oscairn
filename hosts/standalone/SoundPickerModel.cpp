// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SoundPickerModel.h"
#include <algorithm>
#include <stdexcept>

namespace oscairn::standalone
{
    SoundPickerState readSoundPicker(Control& control)
    {
        SoundPickerState result;
        result.node = control.nodeId("synth");
        const auto graph = control.graphState();
        for(const auto& node : graph.topology.nodes)
            if(node.id == result.node)
            {
                result.pluginId = node.plugin.id;
                result.instrument = node.plugin.name;
            }
        result.sounds = control.sounds(result.node);
        if(result.pluginId == "org.oscairn.basic-synth")
            result.choices = {
                {"Warm", {}, {{1, .15}, {2, .7}, {3, .5}}},
                {"Bright", {}, {{1, .9}, {2, .7}, {3, .5}}},
                {"Pressure swell", {}, {{1, .05}, {2, .7}, {3, 1.0}}}
            };
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
        const auto current = readSoundPicker(control);
        if(current.node != action.node || current.pluginId != action.pluginId)
            throw std::runtime_error("The instrument changed. Choose a sound again.");
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
            if(!control.selectSound(current.node, action.context, -1))
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

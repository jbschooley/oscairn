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
        if(result.pluginId == "org.oscairn.sampler")
            result.importsSamples = true;
        else if(result.sounds.capacity)
        {
            // The plugin's defaults, and its presets (CLAP preset discovery or LV2), prepared in a
            // native context or a spare instance (RFC 0005).
            result.choices.push_back({"Default", {}, {}});
            try
            {
                for(const auto& preset : control.presets(result.node))
                    result.choices.push_back({preset.name, preset.uri, {}});
            }
            catch(const std::runtime_error& e) { result.presetProblem = e.what(); }
        }
        if(result.pluginId == "org.oscairn.basic-synth")
        {
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
        return result;
    }

    SoundChoice importSound(Control& control, const std::string& path)
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
        // SFZ's default 1 ms release ticks when a sustained sample stops; give raw WAVs a short fade.
        // A single WAV is copied into the user library, so tidying the original folder cannot break it.
        if(extension == ".wav")
        {
            const auto asset = control.importAsset(path);
            const std::filesystem::path stored(asset.path);
            choice.sfz = "<region> ampeg_release=0.03 sample=" + stored.filename().string() + "\n";
            choice.library = asset.library; choice.directory = stored.parent_path().generic_string();
        }
        else if(extension == ".sfz")
        {
            std::error_code error;
            if(std::filesystem::file_size(file, error) > 64 * 1024 || error) throw std::runtime_error("That SFZ file is too large or unreadable.");
            std::ifstream in(file, std::ios::binary);std::ostringstream text;text << in.rdbuf();
            if(!in) throw std::runtime_error("Could not read that SFZ file.");
            choice.sfz = text.str();
            // An SFZ's folder is used in place as (or inside) a registered library.
            const auto folder = control.registerAssetFolder(file.parent_path().string());
            choice.library = folder.library; choice.directory = folder.directory;
        }
        else throw std::runtime_error("Choose a .wav sample or an .sfz patch.");
        return choice;
    }

    std::string applySoundAction(Control& control, const SoundAction& action)
    {
        if(action.kind == SoundAction::Refresh) return {};
        if(action.kind == SoundAction::SavePerformance){control.savePerformance(action.directory);return {};}
        // A change that would not fit beside the outgoing Performance is a cut (RFC 0025); say so.
        constexpr const char* cut = "Opened at once: it and the last Performance ringing out would not fit this device together.";
        if(action.kind == SoundAction::LoadPerformance){
            const bool seamless = control.estimate(action.directory).seamless;
            control.loadPerformance(action.directory);
            return seamless ? std::string() : cut;
        }
        if(action.kind == SoundAction::SaveToLibrary){control.savePerformanceToLibrary(action.choice.name,action.asNew);return {};}
        if(action.kind == SoundAction::LoadFromLibrary){
            Control::LibraryQuery one; one.id = action.libraryId; one.kind = "performance";
            bool seamless = true; std::string path;
            for(const auto& item : control.libraryQuery(one).items) { seamless = control.estimate(item.path).seamless; path = item.path; }
            // Prepared first while this one plays, as the Setlist prepares ahead (RFC 0021), so the
            // open only publishes it; one that would not fit beside it opens as a cut instead.
            if(seamless && !path.empty()) control.preparePerformance(path);
            control.loadPerformanceFromLibrary(action.libraryId);
            return seamless ? std::string() : cut;
        }
        // The next one, loaded while this one plays (RFC 0021), so opening it only publishes it.
        if(action.kind == SoundAction::PrepareFromLibrary){
            Control::LibraryQuery one; one.id = action.libraryId; one.kind = "performance";
            for(const auto& item : control.libraryQuery(one).items)
                if(!control.preparePerformance(item.path)) return "Not prepared ahead: beside this Performance it would not fit this device. It opens when you go to it.";
            return {};
        }
        if(action.kind == SoundAction::SaveSound){control.savePartAsSound(action.part,action.choice.name,action.asNew);return {};}
        if(action.kind == SoundAction::LoadSound){control.loadSound(action.part,action.libraryId);return {};}
        if(action.kind == SoundAction::PartFromSound){control.partFromSound(action.libraryId);return {};}
        if(action.kind == SoundAction::ImportPerformance){control.importPerformance(action.directory,action.parts,action.scenes);return {};}
        if(action.kind == SoundAction::AuditionStart){control.auditionStart(action.libraryId,action.part);return {};}
        if(action.kind == SoundAction::AuditionStop){control.auditionStop(action.asNew);return {};}
        // What could not go, after what went.
        auto withNotes = [](std::string done, const std::vector<std::string>& notes) { for(const auto& n : notes) done += " " + n + "."; return done; };
        const bool sign = !action.signKey.empty();
        const Control::ExportSigning signing{action.signKey, sign, sign}; // the items and the whole archive
        const std::string signedNote = sign ? " Signed with " + action.signKey + "." : "";
        if(action.kind == SoundAction::ExportPerformance && !action.libraryId.empty()) return withNotes("Exported." + signedNote, control.exportLibraryItem(action.libraryId,action.directory,action.asNew,signing));
        if(action.kind == SoundAction::ExportPerformance) return withNotes("Performance exported." + signedNote, control.exportPerformance(action.directory,action.asNew,signing));
        if(action.kind == SoundAction::InstallArchive){
            const auto r=control.installArchive(action.directory,action.asNew,action.items,action.acceptCode);
            std::string message="Installed "+std::to_string(r.added)+(r.skipped?", "+std::to_string(r.skipped)+" already here":"")+".";
            for(const auto& n:r.notes)message+=" "+n+".";
            return message;
        }
        if(action.kind == SoundAction::AddPluginBuild){control.addPluginBuild(action.format,action.pluginId,action.version,action.platform,action.directory);return "Added the "+action.platform+" build.";}
        if(action.kind==SoundAction::Replace) {
            const auto graph=control.graphState();
            const auto found=std::find_if(graph.topology.nodes.begin(),graph.topology.nodes.end(),[&](const auto& n){return n.id==action.node && n.plugin.id==action.pluginId;});
            if(found==graph.topology.nodes.end())throw std::runtime_error("The loaded plugin changed. Choose again.");
            control.replace(action.node,action.replacementPath,action.replacementId,action.choice.preset);return {};
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
            return {};
        }
        if(action.kind == SoundAction::Prepare)
        {
            const auto choice = action.importPath.empty() ? action.choice : importSound(control, action.importPath);
            control.prepareSound(current.node, choice.name, choice.parameters, {}, choice.preset, choice.sfz, choice.library, choice.directory);
            return {}; // Preparation never changes the playing sound.
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
        return {};
    }
}

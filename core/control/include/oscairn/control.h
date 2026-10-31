// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"
#include "oscairn/parts.h"
#include "oscairn/pads.h"
#include "oscairn/transport.h"

#include <string>
#include <map>
#include <vector>

namespace oscairn
{
    /// In-process control boundary. All calls belong to one control/main thread;
    /// audio and hardware input do not call this interface. No device or plugin handles
    /// escape it. The socket/JSON adapter will implement the same control operations.
    class Control
    {
    public:
        virtual ~Control() = default;

        struct ParamInfo
        {
            uint32_t    id = 0;
            std::string name;
            double      minimum = 0, maximum = 1, defaultValue = 0;
            bool        stepped = false;
            uint32_t    flags = 0;
        };
        struct TransportState
        {
            bool   playing = false;
            double tempo = 120.0;
            double beat = 0.0;
        };
        struct DeviceChannel { uint32_t deviceChannel=0; std::string name; bool available=false; };
        struct SessionStatus
        {
            std::string audioDevice;
            std::vector<MidiEndpoint> midiInputs; // the host's MIDI inputs, for device selectors
            double sampleRate = 0, cpuLoad = 0;
            uint32_t bufferSize = 0;
            uint64_t deadlineMisses = 0;
            std::vector<DeviceChannel> audioInputs, audioOutputs; // logical order -> device channel
            bool audioReady=true;
            uint64_t graphEventDrops=0,midiEventDrops=0;
            // Plugins that have failed and are silent: a worker that crashed, hung or was given up.
            struct PluginFailure { NodeId node=0; std::string name, reason; bool operator==(const PluginFailure&) const = default; };
            std::vector<PluginFailure> failures;
        };
        virtual SessionStatus sessionStatus() const = 0;
        struct GraphState
        {
            Graph::Description topology;
            std::map<NodeId,std::string> names;
            uint64_t revision = 0;
            bool staged = false;
            std::vector<Part> parts; // RFC 0011: the part views' model, staged with the graph
            std::vector<Wire> wires;
            std::vector<Scene> scenes;
            std::vector<SceneParameter> sceneParameters;
            uint32_t scene = 0; // the scene last selected or stored, or 0
            std::vector<Macro> macros;
        };
        enum class ChangeKind { Value, GestureBegin, GestureEnd, Graph, Resync };
        struct Change
        {
            uint64_t sequence = 0;
            ChangeKind kind = ChangeKind::Value;
            NodeId node = 0;
            uint32_t param = 0;
            double value = 0;
            bool plugin = false;
        };
        struct Changes { uint64_t latest = 0; bool lost = false; std::vector<Change> events; };

        // Control thread, engine-local paths. Performance save includes all prepared sounds
        // in a NEW folder; load validates before replacing the current graph.
        virtual void savePerformance(const std::string& directory) = 0;
        virtual void loadPerformance(const std::string& directory) = 0;
        // Per-node export saves the selected channel-0 instance of a hosted pool.
        virtual bool supportsState(NodeId node) const = 0;
        virtual void saveState(NodeId node,const std::string& directory) = 0;
        virtual std::vector<PresetInfo> presets(NodeId node) = 0;
        virtual std::vector<PropertyInfo> properties(NodeId node) const = 0;
        // Acceptance queues a request; propertyValue is last plugin-confirmed state.
        virtual bool setProperty(NodeId node,const std::string& uri,const PropertyValue& value) = 0;
        virtual bool requestProperty(NodeId node,const std::string& uri) = 0;
        virtual std::optional<PropertyValue> propertyValue(NodeId node,const std::string& uri) const = 0;

        struct SoundInfo { uint32_t id;std::string name;uint32_t selectedChannels,activeVoices;bool idle; };
        struct Sounds { uint32_t capacity;std::vector<SoundInfo> contexts;std::string mode; };
        virtual Sounds sounds(NodeId node) const = 0;
        // sfz patches name files in directory of asset library (see the asset calls below).
        virtual uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state={},const std::string& preset={},const std::string& sfz={},const std::string& library={},const std::string& directory={}) = 0;
        virtual bool selectSound(NodeId node,uint32_t id,int16_t channel=-1) = 0;
        virtual bool discardSound(NodeId node,uint32_t id,bool cutTail=false) = 0;

        // Sound assets (docs/rfcs/0009): libraries of files that sounds use without copying them.
        struct AssetFolder { std::string library,directory; };      // a folder's place in a library
        struct AssetLibrary { std::string id,name,path;size_t files; };
        // The library containing folder (registering folder as a new library if none does).
        virtual AssetFolder registerAssetFolder(const std::string& folder) = 0;
        // Copies one file into the user library and returns its reference.
        virtual kick::clap::AssetReference importAsset(const std::string& file) = 0;
        virtual std::vector<AssetLibrary> assetLibraries() const = 0;

        virtual GraphState graphState() const = 0;
        virtual NodeId add( const std::string& name, const std::string& path, const std::string& pluginId = {} ) = 0;
        virtual NodeId addEffectContext(const std::string& name,NodeId source) = 0;
        virtual std::optional<oscairn_effect_resources_t> effectResources(NodeId node) const = 0;
        virtual void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset = {}) = 0;
        virtual void remove( NodeId node ) = 0;
        virtual void connectAudio( NodeId from, NodeId to ) = 0;
        virtual void connectAudio( const AudioConnection& ) = 0;
        virtual void disconnectAudio( const AudioConnection& ) = 0;
        virtual void connectNotes( NodeId from, NodeId to ) = 0;
        virtual void disconnectAudio( NodeId from, NodeId to ) = 0;
        virtual void disconnectNotes( NodeId from, NodeId to ) = 0;
        // RFC 0011 parts and wires (parts.h). Part edits change only their part's wires.
        virtual uint32_t addPart( const std::string& name, PartInput input = {}, std::optional<uint32_t> output = {} ) = 0;
        virtual void removePart( uint32_t part ) = 0;
        virtual void insertIntoPart( uint32_t part, size_t index, NodeId node, ChainPlace place = {} ) = 0;
        virtual void removeFromPart( uint32_t part, NodeId node ) = 0; // wherever it is in the part
        virtual void movePartNode( uint32_t part, size_t from, size_t to, ChainPlace place = {} ) = 0; // a node or split entry
        // Splits (parts.h): a new one has two empty lanes. Kinds other than copy take their Split
        // (and, for left/right and mid/side, Join) node, already added; the split sets their mode
        // and removes them with itself. Removing one keeps its first lane's chain in its place;
        // other lanes' nodes leave the part. Removing the last lane removes the split. setLane
        // sets a lane's zones and Mix, not its chain.
        virtual uint32_t addSplit( uint32_t part, size_t index, ChainPlace place = {}, Split::Kind kind = Split::Kind::Copy, NodeId splitter = 0, NodeId joiner = 0 ) = 0;
        virtual void removeSplit( uint32_t part, uint32_t split ) = 0;
        virtual uint32_t addLane( uint32_t part, uint32_t split ) = 0; // its index
        virtual void removeLane( uint32_t part, uint32_t split, uint32_t lane ) = 0;
        virtual void setLane( uint32_t part, uint32_t split, uint32_t lane, const Lane& settings ) = 0;
        virtual void setPartInput( uint32_t part, const PartInput& input ) = 0;
        virtual void setPartOutput( uint32_t part, uint32_t bus, std::optional<uint32_t> port = {} ) = 0; // through a bus, or (bus 0) to a port
        virtual void setSend( uint32_t part, uint32_t bus, std::optional<double> level ) = 0;            // without a level, removes the send
        virtual void setPartLevel( uint32_t part, double level, bool mute ) = 0; // its fader, post-fader sends included
        virtual void setPartName( uint32_t part, const std::string& name ) = 0;
        virtual void setPartSolo( uint32_t part, bool solo ) = 0;
        virtual void setPartPan( uint32_t part, double pan ) = 0; // -1 left .. 1 right
        // Undo (RFC 0011): edits to parts, wires, splits, Mix settings, scenes, controls and pads,
        // nodes added, removed or replaced, parameter mappings, and parameter values set through
        // this interface. Playing, switching scenes, moving controls and choosing sounds are not
        // edits. An edit is everything up to a commit, or one change that needs none; repeated
        // changes to the same thing within a moment are one edit. The labels say what undo and
        // redo would do; empty: nothing to undo or redo. Opening a Performance clears them. The
        // history keeps at most 8 removed or replaced plugins alive; older steps go.
        struct History { std::string undo, redo; };
        virtual History history() const = 0;
        virtual bool undo() = 0; // false: nothing to undo
        virtual bool redo() = 0;
        // Each part's and bus's recent peak after its fader, held and falling, for the Mixer.
        virtual std::vector<PartMeter> partMeters() const = 0;
        virtual uint32_t connectPorts( const Wire& wire ) = 0; // a user wire; returns its ID
        virtual void disconnectWire( uint32_t wire ) = 0;
        virtual void setMix( uint32_t wire, double level, double pan, bool mute, bool solo ) = 0;
        virtual void setZones( uint32_t wire, const std::vector<NoteZone>& zones ) = 0; // a note wire's zones

        // Scenes (parts.h). Adding or storing captures the current part levels and mutes and the
        // scene parameters' values; selecting applies them at once and commits.
        virtual uint32_t addScene( const std::string& name ) = 0;
        virtual void storeScene( uint32_t scene ) = 0;
        virtual void selectScene( uint32_t scene ) = 0;
        virtual void removeScene( uint32_t scene ) = 0;
        virtual void setSceneName( uint32_t scene, const std::string& name ) = 0;
        // Which parameters scenes control. A parameter added takes its current value in every scene.
        virtual void setSceneParameters( const std::vector<SceneParameter>& parameters ) = 0;

        // The control tree (parts.h). Setting a control's value sets its targets, and their
        // targets in turn; an assignment that would loop is refused.
        virtual uint32_t addMacro( const std::string& name ) = 0;
        virtual void removeMacro( uint32_t macro ) = 0;
        virtual void setMacroName( uint32_t macro, const std::string& name ) = 0;
        // A target newly relative takes its current value as base; one already relative keeps its base.
        virtual void setMacroTargets( uint32_t macro, const std::vector<MacroTarget>& targets ) = 0;
        virtual void setMacro( uint32_t macro, double value ) = 0;
        virtual void remove( NodeId node, UserWires userWires ) = 0;
        virtual void mapParameter( NodeId from, uint32_t lane, NodeId to, uint32_t param ) = 0;
        virtual void unmapParameter( NodeId from, uint32_t lane ) = 0;
        virtual void commit() = 0;
        /// Bounded history, independent cursor for every consumer. lost requires resync.
        virtual uint64_t changeSequence() const = 0;
        virtual Changes changesAfter( uint64_t sequence ) const = 0;

        virtual NodeId nodeId( const std::string& name ) const = 0;
        virtual bool hasNode( NodeId node ) const = 0;
        virtual bool hasParameter( NodeId node, uint32_t param ) const = 0;
        virtual std::vector<ParamInfo> parameters( NodeId node ) const = 0;
        virtual ParamInfo parameterInfo( NodeId node, uint32_t param ) const = 0;
        virtual double parameter( NodeId node, uint32_t param ) const = 0;
        virtual std::string parameterText( NodeId node, uint32_t param, double value ) const = 0;
        /// Accepted values coalesce until service(), then retry if the plugin is busy.
        /// False means the bounded pending-command store refused this new command.
        virtual bool setParameter( NodeId node, uint32_t param, double value ) = 0;
        virtual bool parameterGesture( NodeId node, uint32_t param, bool begin ) = 0;

        virtual void setPlaying( bool playing ) = 0;
        virtual void setTempo( double bpm ) = 0;
        virtual void setClockSource( Transport::ClockSource source ) = 0;
        virtual Transport::ClockSource clockSource() const = 0;
        /// Last rendered block's transport. Fields are atomic telemetry, not a transaction.
        virtual TransportState transportState() const = 0;

        /// UI notes at the next block boundary; channel is zero-based. Physical MIDI
        /// bypasses this queue and enters Session::process with sample offsets.
        /// False means overflow; the session discards queued UI notes and broadcasts
        /// release on the next block. This may release other voices on shared destinations.
        virtual bool note( bool on, int key, double velocity, int channel = 0 ) = 0;

        // Pads (pads.h): the Performance's pad map, and playing a pad through it. A pad press
        // plays its assigned note, on its channel or else `channel`; its release ends the note
        // it started, even if the map or bank changed meanwhile.
        virtual PadMap padMap() const = 0;
        virtual void setPadMap( const PadMap& map ) = 0;
        virtual bool pad( uint32_t pad, bool on, double velocity, int channel = 0 ) = 0;
        virtual bool padPressure( uint32_t pad, double pressure ) = 0; // 0..1, as the pad's assignment sends it
    };
}

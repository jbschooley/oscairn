// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/physical_controls.h"
#include "oscairn/remote_control.h"
#include "../audio/AudioConfiguration.h"
#include "SoundPickerModel.h"
#include <juce_events/juce_events.h>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <future>
#include <deque>
#include <functional>

namespace oscairn::host { class AudioEngine; class EngineProcess; }
namespace oscairn::standalone
{
    std::string tidyNumber(const std::string& valueText); // "0.0100000" -> "0.01"; other text unchanged
    /// UI facade over either an in-process Control or a remote engine's cached Control.
    class Engine : private juce::Timer
    {
    public:
        /// helperExecutable selects managed mode; empty helper + empty socket is in-process.
        explicit Engine(const std::string& pluginDir, const std::string& socketPath = {}, const std::string& helperExecutable = {}, const host::AudioConfiguration& audio = {});
        ~Engine() override;
        using ParamInfo = Control::ParamInfo;
        bool hasParam(const std::string& node,uint32_t param)const;
        ParamInfo paramInfo(const std::string& node,uint32_t param)const;
        double param(const std::string& node,uint32_t param)const;
        std::string paramText(const std::string& node,uint32_t param,double value)const;
        void setParam(const std::string& node,uint32_t param,double value);
        void paramGesture(const std::string& node,uint32_t param,bool begin);
        void setPlaying(bool playing);
        void setTempo(double bpm);
        void setFollowMidiClock(bool follow);
        bool followsMidiClock()const;
        // Clock out (RFC 0012 step 2): the MIDI outputs the transport's clock goes to; cabling, not saved with a Performance.
        void setClockOutputs(std::vector<DeviceRule> outputs);
        std::vector<DeviceRule> clockOutputs()const;
        void note(bool on,int key,float velocity);
        // Pads play through the Performance's pad map (pads.h), on the selected track's MIDI
        // channel unless a pad has its own. The map is cached here and refreshed after a load.
        void pad(int index,bool on,float velocity);
        void padPressure(int index,float pressure);
        const PadMap& padMap() const {return padMap_;}
        void setPadMap(const PadMap& map);
        std::function<void()> openPads;
        // The Parts views (RFC 0011): the graph with its parts, and edits made through the control
        // API in order; a failed edit is reported in editError().
        Control::GraphState graph() const {return control_->graphState();}
        std::vector<Control::ParamInfo> parameters(NodeId node) const {return control_->parameters(node);}
        // The plugin's own window (RFC 0019), which its worker shows beside the app's; the title
        // names the node and its part. Read at most four times a second per node and kept while
        // the engine is busy, as the views ask for it on every rebuild.
        bool hasPluginWindow(NodeId node) const;
        void openPluginWindow(NodeId node,std::string title){command([this,node,title]{control_->showPluginWindow(node,true,title);});}
        // Meters, the routing wires' meters, and the undo/redo names: read at most twenty and
        // four times a second (the views poll faster) and kept while the engine is busy, so the
        // message thread does not sit in socket waits between paints.
        std::vector<PartMeter> meters() const;
        std::vector<WireMeter> wireMeters() const;
        Control::History history() const;
        void edit(std::function<void(Control&)> change);
        const std::string& editError() const {return editError_;}
        // MIDI inputs the engine leaves out (a controller's duplicate ports), sent again after a restart.
        void ignoreMidiInputs(std::vector<std::string> names);
        const std::string& pluginDir() const {return pluginDir_;}
        std::function<void()> openParts;
        std::function<void()> goHome, goBack; // the shell's (RFC 0030), for a panel's buttons
        // Controls (RFC 0011's control tree) by name, for knobs bound as "control:NAME". The
        // values are read once per timer tick.
        bool hasControl(const std::string& name) const;
        double controlValue(const std::string& name) const;
        void setControl(const std::string& name,double value);
        const std::vector<Macro>& controls() const {return macros_;}
        void setControl(uint32_t id,double value);
        // Physical controls (RFC 0011): the Performance's overrides (the last read while the
        // engine is busy), and the device's control moved last ("knob:2"), counted so each move
        // shows, for learn.
        const std::vector<AssignmentOverride>& assignments() const;
        void touched(const std::string& control){touched_=control;++touches_;}
        const std::string& lastTouched() const {return touched_;}
        uint32_t touches() const {return touches_;}
        // The device's control groups, from its profile (RFC 0024); none on the desktop.
        void setControlGroups(std::vector<ControlGroup> groups){groups_=std::move(groups);}
        const std::vector<ControlGroup>& controlGroups() const {return groups_;}
        // Where each group is (page, row or mode) and the part the knobs follow, as Play shows
        // them, for the controller-mapping service (Controllers.h) to play the same.
        GroupPositions& positions(){return positions_;}
        void setKnobPart(uint32_t part){knobPart_=part;}
        uint32_t knobPart() const {return knobPart_;}
        void selectTrack(uint32_t track){sequenceTrack_=std::min(track,1u);}
        // The row selected on screen (RFC 0019): the parameters a device's knobs bound "row:N"
        // play, in order, and how many such knobs the device has and on which edge of its
        // screen they sit (none on the desktop). Main thread.
        struct RowParam { std::string node; uint32_t param = 0; std::string name; uint32_t control = 0; }; // control: a control of the tree (RFC 0011) instead of a parameter
        enum class Edge { Bottom, Top, Left, Right };
        void setRow(std::vector<RowParam> row) { row_ = std::move(row); }
        const std::vector<RowParam>& row() const { return row_; }
        void setRowKnobs(int count, Edge edge) { rowKnobs_ = count; rowEdge_ = edge; }
        int rowKnobs() const { return rowKnobs_; }
        Edge rowEdge() const { return rowEdge_; }
        // The generated editor page on screen (RFC 0011's screen knobs, RFC 0019): its parameters
        // by priority, which a device's screen knobs play; none while Play shows, when they play
        // its knob row. An editor shows `page` in place of the one it showed last (`published`),
        // and an empty page withdraws its own. Main thread.
        const ScreenPage& screenPage() const { return screenPage_; }
        void showScreenPage(ScreenPage& published, ScreenPage page)
        {
            if(page == published) return;
            if(screenPage_ == published) screenPage_ = {};
            published = std::move(page);
            if(!published.params.empty()) screenPage_ = published;
        }
        int trackChannel()const;
        bool connected()const{return failure_.empty();}
        bool canRestart()const{return recoveryAvailable_;}
        void restart();
        struct Status
        {
            bool playing=false;
            double tempo=120;
            int step=-1;
            std::string audioDevice;
            std::vector<MidiEndpoint> midiInputs;
            std::vector<Control::SessionStatus::PluginFailure> failures; // plugins that failed and are silent
            double load=0; // the audio thread's share of each block's time, 0..1
            std::vector<MidiEndpoint> midiOutputs; // for parts' MIDI out and clock out (RFC 0012)
        };
        Status status()const;
        const SoundPickerState& soundPicker() const { return soundPicker_; }
        const std::string& soundMessage() const { return soundMessage_; }
        // A refresh reads on its own connection and makes only short calls, so the UI's calls
        // wait on nothing long: it does not make the engine busy.
        bool controlReady() const { return !soundJob_.valid() || soundJobKind_ == SoundAction::Refresh; }
        // Any job, including a refresh.
        bool jobRunning() const { return soundJob_.valid(); }
        // Preparing the next Performance ahead is not busy: anything asked meanwhile waits for it, or replaces it while it waits.
        bool soundBusy() const { return std::any_of(pendingSounds_.begin(),pendingSounds_.end(),[](const SoundAction& a){return a.kind!=SoundAction::PrepareFromLibrary;}) || (soundJob_.valid() && soundJobKind_!=SoundAction::Refresh && soundJobKind_!=SoundAction::PrepareFromLibrary); }
        // Why a load, save or export asked now would be refused (the engine busy or gone), or empty.
        std::string refusal() const { return !connected() ? "The engine is not running." : soundBusy() ? "The engine is busy; try again in a moment." : std::string(); }
        bool canPrepareSounds() const { return !socketPath_.empty(); }
        void soundAction(SoundAction action);
        std::function<void()> openBrowser;
        // The library (RFC 0021); nothing while the engine is busy loading or saving, so ask again.
        std::optional<Control::LibraryResult> library(const Control::LibraryQuery& query) const;
        std::optional<Control::LibraryItem> libraryItem(const std::string& id) const;
        std::optional<Control::PerformanceInfo> performanceInfo() const;
        std::optional<Control::Audition> audition() const; // nothing while the engine is busy
        std::optional<Control::PerformanceContents> performanceContents(const std::string& directory) const;
        // Libraries (RFC 0021): an archive's contents (empty error when read), the installed
        // libraries, and deleting, editing or copying an item or uninstalling a library (the
        // error, or empty; a copy's ID into copied).
        std::string archiveContents(const std::string& file, Control::Archive& out) const;
        std::vector<Control::InstalledLibrary> libraries() const;
        std::string deleteLibraryItem(const std::string& id);
        std::string editLibraryItem(const std::string& id, const Control::PerformanceInfo& info);
        std::string copyLibraryItem(const std::string& id, std::string& copied);
        std::string uninstallLibrary(const std::string& id);
        // The plugin store (RFC 0006): what is installed and what uses it (nothing while the
        // engine is busy), and removing a plugin's builds (the error, or empty). Another
        // platform's build is added by soundAction (AddPluginBuild).
        std::vector<Control::InstalledPlugin> installedPlugins() const;
        // Signing keys (RFC 0006): by name, ID (its fingerprint) and public key, never the private
        // key; none while the engine is busy. Making one gives the error, or empty.
        std::vector<Control::SigningKey> signingKeys() const;
        std::string makeSigningKey(const std::string& name, Control::SigningKey& made);
        // Apps asked for before they open, and their closing (RFC 0025). Admitted when the engine
        // cannot be asked, so the shell stays usable without it.
        Control::Launch launchApp(const Control::AppMemory& app, bool dropNext = false);
        void appClosed(const std::string& app);
        std::string removePlugin(const Control::InstalledPlugin& plugin);
        std::string pluginId(const std::string& node) const;
        void replacePlugin(const std::string& node,const std::string& path,const std::string& id,const std::string& preset={});
        // Recording (RFC 0020): the takes, this run's and those found in recordings/, read at most
        // four times a second and kept while the engine is busy; the take recording, or 0. The
        // rest give the error, or empty. setCapture's length is sent again after a restart.
        const std::vector<Control::Take>& takes() const;
        void refreshTakes() { takesRead_ = 0; }
        uint32_t recordingTake() const;
        std::string record(bool on);          // Main and the notes, until stopped
        std::string capture(double seconds);  // the last seconds of the retroactive buffer, as a take
        std::string setCapture(double seconds);
        double captureSeconds() const { return captureSeconds_; }
        std::string deleteTake(uint32_t take);
        std::string renameTake(uint32_t take, const std::string& name);
        std::string playTake(uint32_t take);  // 0 stops
        struct Editor { std::string name,id,path;std::vector<ParamInfo> parameters;std::vector<PresetInfo> presets; };
        const std::map<std::string,Editor>& editors() const {return editors_;}

    private:
        void timerCallback()override;
        void attach();
        template<class F> void command(F&& f)
        {
            if(!failure_.empty() || (soundJob_.valid() && soundJobKind_==SoundAction::Replace))return;
            // Wait behind a job that may change the graph; a refresh changes nothing, so notes and edits go at once.
            if(!controlReady()) { pendingControls_.emplace_back(std::forward<F>(f)); return; }
            // The polled reads above keep their pace through edits: a fader dragged sends 60 a
            // second, and the undo names and the open Performance may trail an edit by 250 ms.
            try{f();}catch(const std::exception& e)
            {
                if(client_ && !client_->connected())failed(e.what());
                else std::fprintf(stderr,"oscairn: control command: %s\n",e.what());
            }
        }
        void failed(const std::string& message);
        void refreshSounds(SoundAction action = {});
        void finishSounds();
        struct SoundResult { std::map<std::string,Editor> editors;SoundPickerState state; std::string error, message; bool disconnected = false; };
        std::future<SoundResult> soundJob_;
        SoundAction::Kind soundJobKind_=SoundAction::Refresh;
        std::deque<SoundAction> pendingSounds_; // after the job running, in order
        SoundPickerState soundPicker_;
        std::map<std::string,Editor> editors_;
        std::string soundMessage_;
        std::string exportTarget_, exportWritten_; // an export replacing a file: the file, and where it is written first
        uint32_t soundTicks_ = 0;
        std::deque<std::function<void()>> pendingControls_;
        std::unique_ptr<host::AudioEngine> audio_;
        std::unique_ptr<host::EngineProcess> process_;
        std::unique_ptr<LocalClient> client_;
        std::unique_ptr<RemoteControl> remote_;
        Control* control_=nullptr;
        uint32_t sequenceTrack_=0;
        std::vector<RowParam> row_;
        ScreenPage screenPage_;
        std::string touched_;
        uint32_t touches_=0;
        std::vector<ControlGroup> groups_;
        GroupPositions positions_;
        uint32_t knobPart_=0;
        int rowKnobs_=0;
        Edge rowEdge_=Edge::Bottom;
        std::array<uint16_t,128> padChannels_{};
        PadMap padMap_;
        std::string editError_, pluginDir_;
        std::vector<std::string> ignoredInputs_;
        std::vector<Macro> macros_;
        void refreshPads();
        std::string socketPath_,failure_;
        uint32_t reconnectTicks_=0;
        bool recoveryAvailable_=false;
        mutable std::vector<Control::Take> takes_;
        mutable uint32_t takesRead_=0;
        mutable std::vector<PartMeter> meters_; mutable uint32_t metersRead_=0;
        mutable std::vector<WireMeter> wireMeters_; mutable uint32_t wireMetersRead_=0;
        mutable Control::History history_; mutable uint32_t historyRead_=0;
        mutable std::optional<Control::PerformanceInfo> performance_; mutable uint32_t performanceRead_=0;
        mutable std::map<NodeId,bool> windowCache_; mutable uint32_t windowsRead_=0;
        double captureSeconds_=0;
        mutable std::map<std::pair<std::string,uint32_t>,std::map<uint64_t,std::string>> textCache_;
        mutable std::vector<AssignmentOverride> assignments_;
    };
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/unavailable_plugin.h"
#include "StateFiles.h"
#include <cstring>
#include <map>
#include <sstream>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr size_t maxFiles = 4096;
        constexpr uint64_t maxBytes = 512ull << 20; // the saved state kept in memory, at most

        class Unavailable final : public PluginHost
        {
        public:
            explicit Unavailable(UnavailablePlugin p) : p_(std::move(p))
            {
                for(const auto& q : p_.parameters) values_[q.id] = q.defaultValue;
            }
            void service() override {}
            // The saved snapshot, kept as it was, every file, and written back unchanged.
            void loadSnapshot(const std::string& directory) override
            {
                files_.clear();
                uint64_t bytes = 0;
                for(const auto& e : fs::recursive_directory_iterator(directory))
                {
                    if(e.is_symlink() || (!e.is_directory() && !e.is_regular_file())) throw std::runtime_error("Unsupported file in a plugin's saved state");
                    if(!e.is_regular_file()) continue;
                    if(files_.size() >= maxFiles) throw std::runtime_error("Too many files in a plugin's saved state");
                    auto data = state_files::read(e.path(), size_t(maxBytes - bytes));
                    bytes += data.size();
                    files_[fs::relative(e.path(), directory).generic_string()] = std::move(data);
                }
                // A parameter snapshot's values, shown as they were saved.
                if(const auto it = files_.find("snapshot"); it != files_.end())
                {
                    std::istringstream in(std::string(it->second.begin(), it->second.end())); in.imbue(std::locale::classic());
                    std::string kind; unsigned version = 0; clap_id id; double value;
                    if(in >> kind >> version && kind == "parameters") while(in >> id >> value) if(values_.count(id)) values_[id] = value;
                }
            }
            void saveSnapshot(const std::string& directory) override
            {
                state_files::Bundle bundle(directory);
                for(const auto& [path, data] : files_)
                {
                    const auto to = bundle.temporary.path / fs::path(path);
                    fs::create_directories(to.parent_path());
                    state_files::write(to, data.data(), data.size());
                }
                bundle.publish();
            }
            const kick::clap::Descriptor& descriptor() const noexcept override { return p_.descriptor; }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return p_.parameters; }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return p_.inputs; }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return p_.outputs; }
            uint32_t noteInputPorts() const noexcept override { return p_.noteInputs; }
            uint32_t noteOutputPorts() const noexcept override { return p_.noteOutputs; }
            void panic() noexcept override {}
            // Values a scene or control sets are kept (control thread only), so they save as set.
            double parameterValue(clap_id id) const noexcept override { const auto it = values_.find(id); return it == values_.end() ? 0 : it->second; }
            std::string parameterText(clap_id, double value) const override { std::ostringstream o; o.imbue(std::locale::classic()); o << value; return o.str(); }
            bool setParameter(clap_id id, double value) noexcept override { const auto it = values_.find(id); if(it == values_.end()) return false; it->second = value; return true; }
            bool parameterGesture(clap_id, bool) noexcept override { return true; }
            bool popOutput(kick::clap::Event&) noexcept override { return false; }
            uint64_t outputEventDrops() const noexcept override { return 0; }
            const kick::clap::Event* outputEvents(uint32_t& count) const noexcept override { count = 0; return nullptr; }
            kick::clap::Statistics statistics() const noexcept override { return {}; }
            std::string failure() const override { return p_.reason; }
            bool asleep() const noexcept override { return true; } // silent: a change never waits on it
            bool process(const float* const*, uint32_t, float* const* outputs, uint32_t no, uint32_t frames, const kick::clap::Event*, uint32_t, const clap_event_transport_t*) noexcept override
            {
                for(uint32_t c = 0; c < no; ++c) if(outputs[c]) std::memset(outputs[c], 0, frames * sizeof(float));
                return true;
            }

        private:
            UnavailablePlugin p_;
            std::map<clap_id, double> values_;
            std::map<std::string, std::vector<uint8_t>> files_;
        };
    }

    std::shared_ptr<PluginHost> unavailablePlugin(UnavailablePlugin plugin) { return std::make_shared<Unavailable>(std::move(plugin)); }
}

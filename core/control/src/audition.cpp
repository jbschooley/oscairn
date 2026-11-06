// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Audition (RFC 0021): a library item tried without keeping it. What it replaces is kept as the
// undo history keeps a removed plugin: the model before it, holding the plugins themselves, so
// stopping puts back the same instances with their state and prepared sounds, not copies.
#include "oscairn/session.h"

namespace oscairn
{
    void Session::auditionStart(const std::string& id, uint32_t part)
    {
        if(audition_) auditionStop(false);
        const auto* item = library().find(id);
        if(!item) throw std::invalid_argument("no such library item");
        const auto found = *item;
        if(found.kind == "sound") findPart(part); // throws for one that is not there
        Auditioning a;
        a.what = {found.id, found.kind, found.kind == "sound" ? part : 0};
        record(); // an edit waiting for its step comes first
        a.before = std::make_shared<const Model>(model());
        a.undo = undo_; a.redo = redo_;
        a.info = info_; a.tempo = tempo_.load(); a.clock = clockSource_.load(); a.scene = scene_; a.mappingScene = mappingScene_;
        if(found.kind == "sound") loadSoundInto(part, found.path); // as loading it, but not marked used
        else
        {
            // This Performance stays loaded while the other plays: both must fit (RFC 0025).
            if(budget_.cpu > 0)
                if(const auto e = estimate(found.path); e.fits && !e.seamless)
                    throw std::runtime_error("auditioning " + found.name + " needs room for it and the open Performance together, more than this device allows");
            // As opening it, but not marked used, and taking over none of the instances set aside,
            // which stopping puts back as they were (RFC 0021).
            struct Keeping { bool& keep; ~Keeping() { keep = false; } } keeping{keepInstances_ = true};
            loadPerformance(found.path);
        }
        audition_ = std::move(a);
    }

    void Session::auditionStop(bool keep)
    {
        if(!audition_) return;
        auto a = std::move(*audition_);
        audition_.reset();
        if(keep) { library().used(a.what.id); return; } // kept: it was used; what was set aside goes (a Performance's is already ringing out)
        // The tried plugins ring out where they are, as the last Performance's do after a change:
        // their sound stays, their notes drain, nothing new reaches them.
        std::set<NodeId> tried;
        for(const auto& [n, name] : nodes_) if(!a.before->nodes.count(n)) tried.insert(n);
        for(const auto n : tried)
        {
            nodes_.erase(n); leaving_.insert(n); graph_.setRetiring(n, true);
            std::erase_if(observed_, [&](const auto& entry) { return entry.first.first == n; });
        }
        for(const auto& e : graph_.describe().notes) if(tried.count(e.second)) graph_.setDraining(e.first, e.second, true);
        // What was set aside sounds again: out of the leaving ones if it was still ringing.
        for(const auto& [n, named] : a.before->nodes) if(leaving_.erase(n)) graph_.setRetiring(n, a.before->disabled.count(n) != 0);
        apply(model(), *a.before);
        undo_ = std::move(a.undo); redo_ = std::move(a.redo); mergeable_ = false;
        observed_.clear();
        for(const auto& [n, name] : nodes_) for(const auto& p : graph_.host(n).parameters()) observed_[{n, p.id}] = graph_.host(n).parameterValue(p.id);
        if(a.what.kind == "performance")
        {
            info_ = a.info; tempo_.store(a.tempo); clockSource_.store(a.clock);
            scene_ = a.scene; mappingScene_ = a.mappingScene; transition_.reset(); pendingScene_.reset();
            rungOut_ = disabled_; // as opening it: what is off has rung out
            syncWires(); commit();
        }
        ++revision_; publish(ChangeKind::Resync);
    }
}

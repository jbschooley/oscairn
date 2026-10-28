// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/local_socket.h"
#include <memory>

namespace oscairn::host
{
    /// Lifetime lock for an engine started by the desktop. Under this lock only,
    /// a refused connection permits removing a stale socket, never another file.
    class EngineLease
    {
    public:
        explicit EngineLease(const std::string& socketPath);
        ~EngineLease();
        EngineLease(const EngineLease&)=delete;
        EngineLease& operator=(const EngineLease&)=delete;
    private:
        int lock_=-1;
    };

    /// Desktop-owned launcher, not audio ownership. Destruction leaves a running
    /// helper alive. All operations belong to one control thread.
    class EngineProcess
    {
    public:
        EngineProcess(std::string executable,std::string pluginDirectory,std::string runtimeDirectory={});
        ~EngineProcess();
        EngineProcess(const EngineProcess&)=delete;
        EngineProcess& operator=(const EngineProcess&)=delete;
        /// Attach first; serialize startup and wait up to timeoutMs for readiness.
        /// Never replaces a reachable server or replays mutations. On startup failure,
        /// stops only its own newly spawned child; an existing helper is left alone.
        void connectOrStart(LocalClient&,unsigned timeoutMs=5000);
        bool running();
        const std::string& socketPath()const;
        const std::string& logPath()const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

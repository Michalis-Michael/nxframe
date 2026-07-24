/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * GUI-owned sender process launcher. It starts the existing NxFrame CLI as a
 * separate process, so the web control plane does not enter the real-time
 * capture/encode/mux path.
 */

#pragma once

#include "utils/cpu_profile_helper.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

class SenderProcessManager {
public:
    SenderProcessManager(std::string nxframeExecutable,
                         std::string cpuProfileConfig);
    ~SenderProcessManager();

    nlohmann::json status(const std::string& channel);

    bool start(const std::string& channel,
               int decklinkDevice,
               const std::string& presetPath,
               const std::string& cpuProfile,
               nlohmann::json& response,
               std::string* error = nullptr);

    bool startReceiver(const std::string& channel,
                       int decklinkDevice,
                       const std::string& presetPath,
                       const std::string& cpuProfile,
                       nlohmann::json& response,
                       std::string* error = nullptr);

    bool updateReceiverRoute(const std::string& channel,
                             const std::vector<int>& route,
                             nlohmann::json& response,
                             std::string* error = nullptr);

    bool stop(const std::string& channel,
              nlohmann::json& response,
              std::string* error = nullptr);

    void reap();
    void shutdownAll();
    bool anyRunning();

    bool available() const { return !nxframeExecutable_.empty(); }
    bool hasCpuProfile(const std::string& profileName) const;
    nlohmann::json cpuProfiles() const;
    const std::string& cpuProfileWarning() const { return cpuProfileWarning_; }
    const std::string& executable() const { return nxframeExecutable_; }

private:
    struct ProcessInfo {
        pid_t pid = -1;
        int decklinkDevice = -1;
        std::string role;
        bool running = false;
        bool stopping = false;
        int lastExitCode = 0;
        bool hasExitCode = false;
        std::chrono::steady_clock::time_point startedAt{};
    };

    nlohmann::json statusFor(const std::string& channel, const ProcessInfo* info) const;
    bool buildTransportUrl(const std::string& presetPath,
                           std::string& transportUrl,
                           std::string* error) const;
    bool buildReceiverInputUrl(const std::string& presetPath,
                               std::string& inputUrl,
                               std::string* error) const;
    bool prepareCpuProfile(const std::string& cpuProfile,
                           bool& firstWorker,
                           std::string* error);
    bool launch(const std::string& channel,
                int decklinkDevice,
                const std::string& role,
                std::vector<std::string> args,
                bool firstWorker,
                nlohmann::json& response,
                std::string* error);
    nlohmann::json receiverStateFor(pid_t pid) const;
    static std::string receiverControlPath(pid_t pid);
    static std::string receiverStatePath(pid_t pid);
    bool hasRunningProcesses() const;
    void restoreCpuProfileIfIdle();

    std::string nxframeExecutable_;
    std::string cpuProfileConfig_;
    std::vector<nxframe::CpuProfileSpec> cpuProfiles_;
    std::string cpuProfileWarning_;
    nxframe::CpuProfileGuard cpuProfileGuard_;
    std::string activeCpuProfile_;
    std::map<std::string, ProcessInfo> processes_;
};

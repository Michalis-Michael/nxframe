/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * License / EULA notice:
 * This file is part of NxFrame. Use, redistribution, and modification are
 * governed by the project license and any written EULA or commercial license
 * agreement supplied with the project. If no separate written agreement is
 * supplied, the GPL-3.0-or-later terms apply.
 *
 * Description:
 * Linux CPU frequency profile helper. It applies optional, named CPU profiles
 * from JSON before live sender startup and restores the original machine state
 * when the sender exits.
 */

#pragma once

#include <string>
#include <vector>

namespace nxframe {

struct CpuProfileSpec {
    std::string name;
    std::string description;
    long minFrequencyKHz = 0; // 0 means auto / do not force a minimum.
    long maxFrequencyKHz = 0; // 0 means auto / do not force a maximum.
    std::string governor;     // Empty means keep the current governor.
    bool restoreOnExit = true;
};

struct CpuProfileCpuState {
    std::string name;
    std::string cpufreqDir;
    std::string scalingMinPath;
    std::string scalingMaxPath;
    std::string scalingGovernorPath;
    std::string originalMin;
    std::string originalMax;
    std::string originalGovernor;
    bool hasScalingMin = false;
    bool hasScalingMax = false;
    bool hasScalingGovernor = false;
};

class CpuProfileGuard {
public:
    CpuProfileGuard() = default;
    ~CpuProfileGuard();

    CpuProfileGuard(const CpuProfileGuard&) = delete;
    CpuProfileGuard& operator=(const CpuProfileGuard&) = delete;

    bool applyProfile(const std::string& profileName,
                      const std::string& configPath,
                      std::string* error);

    void restore(bool force = false);
    bool active() const { return applied_; }
    const CpuProfileSpec& spec() const { return spec_; }
    const std::string& resolvedConfigPath() const { return resolvedConfigPath_; }
    size_t cpuCount() const { return states_.size(); }

private:
    CpuProfileSpec spec_;
    std::string resolvedConfigPath_;
    std::vector<CpuProfileCpuState> states_;
    bool applied_ = false;
    bool restored_ = false;
};

// Load and validate every named profile without applying it. This is used by
// the GUI to present the same profile catalogue consumed by the CLI helper.
bool loadCpuProfileSpecs(const std::string& configPath,
                         std::vector<CpuProfileSpec>& profiles,
                         std::string* resolvedConfigPath = nullptr,
                         std::string* error = nullptr);

} // namespace nxframe

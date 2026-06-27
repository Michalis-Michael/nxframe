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
 * Linux CPU frequency profile helper implementation. Profiles are read from a
 * JSON file and applied through the standard cpufreq sysfs interface.
 */

#include "utils/cpu_profile_helper.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nxframe {
namespace {

using json = nlohmann::json;

const char* kCpuSysfsRoot = "/sys/devices/system/cpu";

bool fileExistsLocal(const std::string& path)
{
    struct stat st;
    return (::stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

bool dirExistsLocal(const std::string& path)
{
    struct stat st;
    return (::stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

std::string joinPathLocal(const std::string& a, const std::string& b)
{
    if (a.empty()) {
        return b;
    }
    if (a.back() == '/') {
        return a + b;
    }
    return a + "/" + b;
}

std::string getExeDirLocal()
{
#if defined(__linux__)
    char path[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        return {};
    }
    path[n] = '\0';
    std::string full(path);
    const auto pos = full.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }
    return full.substr(0, pos);
#else
    return {};
#endif
}

std::string trimLocal(const std::string& s)
{
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool readTextFile(const std::string& path, std::string& out)
{
    std::ifstream f(path.c_str());
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool readOneLine(const std::string& path, std::string& out)
{
    std::ifstream f(path.c_str());
    if (!f) {
        return false;
    }
    std::getline(f, out);
    out = trimLocal(out);
    return true;
}

bool writeOneLine(const std::string& path, const std::string& value, std::string* error)
{
    std::ofstream f(path.c_str());
    if (!f) {
        if (error) {
            *error = "cannot open '" + path + "' for writing: " + std::strerror(errno) +
                     ". CPU profiles usually require root or adjusted sysfs permissions.";
        }
        return false;
    }
    f << value << '\n';
    if (!f) {
        if (error) {
            *error = "failed to write '" + value + "' to '" + path + "'";
        }
        return false;
    }
    return true;
}

bool readLongFile(const std::string& path, long& out)
{
    std::string text;
    if (!readOneLine(path, text)) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str()) {
        return false;
    }
    out = value;
    return true;
}

bool parseCpuNumber(const std::string& name, int& out)
{
    if (name.size() <= 3 || name.compare(0, 3, "cpu") != 0) {
        return false;
    }
    for (size_t i = 3; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    char* end = nullptr;
    const long v = std::strtol(name.c_str() + 3, &end, 10);
    if (end == name.c_str() + 3 || v < 0 || v > 4096) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

std::vector<std::string> enumerateCpuDirs()
{
    std::vector<std::pair<int, std::string> > numbered;
    DIR* dir = ::opendir(kCpuSysfsRoot);
    if (!dir) {
        return {};
    }

    while (dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        int index = 0;
        if (!parseCpuNumber(name, index)) {
            continue;
        }
        const std::string cpufreq = joinPathLocal(joinPathLocal(kCpuSysfsRoot, name), "cpufreq");
        if (dirExistsLocal(cpufreq)) {
            numbered.push_back(std::make_pair(index, name));
        }
    }
    ::closedir(dir);

    std::sort(numbered.begin(), numbered.end(),
              [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                  return a.first < b.first;
              });

    std::vector<std::string> result;
    for (const auto& item : numbered) {
        result.push_back(item.second);
    }
    return result;
}

bool resolveCpuProfileConfig(const std::string& explicitPath,
                             std::string& resolved,
                             std::string* error)
{
    if (!explicitPath.empty()) {
        if (fileExistsLocal(explicitPath)) {
            resolved = explicitPath;
            return true;
        }
        if (error) {
            *error = "CPU profile config not found: " + explicitPath;
        }
        return false;
    }

    std::vector<std::string> candidates;
    candidates.push_back("config/cpu_profiles.json");
    candidates.push_back("config/cpu_profile.json");
    candidates.push_back("cpu_profiles.json");
    candidates.push_back("cpu_profile.json");
    candidates.push_back("../config/cpu_profiles.json");
    candidates.push_back("../config/cpu_profile.json");

    const std::string exeDir = getExeDirLocal();
    if (!exeDir.empty()) {
        candidates.push_back(joinPathLocal(exeDir, "config/cpu_profiles.json"));
        candidates.push_back(joinPathLocal(exeDir, "config/cpu_profile.json"));
        candidates.push_back(joinPathLocal(exeDir, "../config/cpu_profiles.json"));
        candidates.push_back(joinPathLocal(exeDir, "../config/cpu_profile.json"));
        candidates.push_back(joinPathLocal(exeDir, "../../config/cpu_profiles.json"));
        candidates.push_back(joinPathLocal(exeDir, "../../config/cpu_profile.json"));
    }

    candidates.push_back("/usr/local/share/nxframe/config/cpu_profiles.json");
    candidates.push_back("/usr/local/share/nxframe/config/cpu_profile.json");
    candidates.push_back("/usr/share/nxframe/config/cpu_profiles.json");
    candidates.push_back("/usr/share/nxframe/config/cpu_profile.json");

    for (const auto& candidate : candidates) {
        if (fileExistsLocal(candidate)) {
            resolved = candidate;
            return true;
        }
    }

    if (error) {
        *error = "CPU profile config not found. Create config/cpu_profiles.json or pass --cpu-profile-config <path>.";
    }
    return false;
}

bool getNumber(const json& obj, const std::vector<std::string>& keys, double& out)
{
    for (const auto& key : keys) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number()) {
            out = it->get<double>();
            return true;
        }
    }
    return false;
}

bool getString(const json& obj, const std::vector<std::string>& keys, std::string& out)
{
    for (const auto& key : keys) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_string()) {
            out = it->get<std::string>();
            return true;
        }
    }
    return false;
}

bool getBool(const json& obj, const std::vector<std::string>& keys, bool& out)
{
    for (const auto& key : keys) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_boolean()) {
            out = it->get<bool>();
            return true;
        }
    }
    return false;
}

bool parseFrequencyString(const std::string& raw, long& outKHz, std::string* error)
{
    std::string s = trimLocal(raw);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    double multiplier = 1000000.0; // default for plain strings is GHz, same as numeric small values.
    const auto eraseUnit = [&](const std::string& unit, double m) -> bool {
        if (s.size() >= unit.size() && s.compare(s.size() - unit.size(), unit.size(), unit) == 0) {
            s = trimLocal(s.substr(0, s.size() - unit.size()));
            multiplier = m;
            return true;
        }
        return false;
    };

    eraseUnit("ghz", 1000000.0) || eraseUnit("g", 1000000.0) ||
    eraseUnit("mhz", 1000.0) || eraseUnit("m", 1000.0) ||
    eraseUnit("khz", 1.0) || eraseUnit("k", 1.0);

    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0' || value < 0.0) {
        if (error) {
            *error = "invalid frequency value '" + raw + "'";
        }
        return false;
    }

    outKHz = static_cast<long>(value * multiplier + 0.5);
    return true;
}

long frequencyNumberToKHz(double value)
{
    // 0 means auto. Small values are GHz (3.8 -> 3800000 kHz).
    // Typical MHz values are 800..10000. Large values are already kHz.
    if (value <= 0.0) {
        return 0;
    }
    if (value <= 20.0) {
        return static_cast<long>(value * 1000000.0 + 0.5);
    }
    if (value < 100000.0) {
        return static_cast<long>(value * 1000.0 + 0.5);
    }
    return static_cast<long>(value + 0.5);
}

bool readFrequencyField(const json& profile,
                        const std::string& baseName,
                        long& outKHz,
                        std::string* error)
{
    const std::vector<std::string> genericKeys = {
        baseName,
        baseName + "_frequency",
        baseName + "Frequency",
        baseName + "_freq",
        baseName + "-frequency"
    };

    std::string s;
    if (getString(profile, genericKeys, s)) {
        return parseFrequencyString(s, outKHz, error);
    }

    double value = 0.0;
    if (getNumber(profile, {baseName, baseName + "_frequency", baseName + "_freq", baseName + "-frequency"}, value)) {
        outKHz = frequencyNumberToKHz(value);
        return true;
    }
    if (getNumber(profile, {baseName + "_frequency_ghz", baseName + "_ghz", baseName + "-frequency-ghz"}, value)) {
        outKHz = static_cast<long>(value * 1000000.0 + 0.5);
        return true;
    }
    if (getNumber(profile, {baseName + "_frequency_mhz", baseName + "_mhz", baseName + "-frequency-mhz"}, value)) {
        outKHz = static_cast<long>(value * 1000.0 + 0.5);
        return true;
    }
    if (getNumber(profile, {baseName + "_frequency_khz", baseName + "_khz", baseName + "-frequency-khz"}, value)) {
        outKHz = static_cast<long>(value + 0.5);
        return true;
    }

    outKHz = 0;
    return true;
}

bool parseProfile(const json& root,
                  const std::string& profileName,
                  CpuProfileSpec& out,
                  std::string* error)
{
    const json* profiles = &root;
    auto profilesIt = root.find("profiles");
    if (profilesIt != root.end() && profilesIt->is_object()) {
        profiles = &(*profilesIt);
    }

    auto profileIt = profiles->find(profileName);
    if (profileIt == profiles->end() || !profileIt->is_object()) {
        if (error) {
            *error = "CPU profile '" + profileName + "' not found in config.";
        }
        return false;
    }

    const json& profile = *profileIt;
    CpuProfileSpec spec;
    spec.name = profileName;

    getString(profile, {"description", "comment"}, spec.description);
    getString(profile, {"governor", "scaling_governor"}, spec.governor);
    getBool(profile, {"restore_on_exit", "restoreOnExit"}, spec.restoreOnExit);

    if (!readFrequencyField(profile, "min", spec.minFrequencyKHz, error)) {
        return false;
    }
    if (!readFrequencyField(profile, "max", spec.maxFrequencyKHz, error)) {
        return false;
    }

    if (spec.minFrequencyKHz > 0 && spec.maxFrequencyKHz > 0 && spec.minFrequencyKHz > spec.maxFrequencyKHz) {
        if (error) {
            *error = "CPU profile '" + profileName + "' has min frequency above max frequency.";
        }
        return false;
    }

    if (spec.minFrequencyKHz <= 0 && spec.maxFrequencyKHz <= 0 && spec.governor.empty()) {
        if (error) {
            *error = "CPU profile '" + profileName + "' does not set max/min frequency or governor.";
        }
        return false;
    }

    out = spec;
    return true;
}

std::string longToString(long value)
{
    return std::to_string(value);
}

bool captureCpuStates(std::vector<CpuProfileCpuState>& states, std::string* error)
{
    const std::vector<std::string> cpus = enumerateCpuDirs();
    if (cpus.empty()) {
        if (error) {
            *error = "no cpufreq-capable CPUs found under /sys/devices/system/cpu. Is CPU frequency scaling enabled?";
        }
        return false;
    }

    for (const auto& cpu : cpus) {
        CpuProfileCpuState state;
        state.name = cpu;
        state.cpufreqDir = joinPathLocal(joinPathLocal(kCpuSysfsRoot, cpu), "cpufreq");
        state.scalingMinPath = joinPathLocal(state.cpufreqDir, "scaling_min_freq");
        state.scalingMaxPath = joinPathLocal(state.cpufreqDir, "scaling_max_freq");
        state.scalingGovernorPath = joinPathLocal(state.cpufreqDir, "scaling_governor");

        state.hasScalingMin = readOneLine(state.scalingMinPath, state.originalMin);
        state.hasScalingMax = readOneLine(state.scalingMaxPath, state.originalMax);
        state.hasScalingGovernor = readOneLine(state.scalingGovernorPath, state.originalGovernor);

        if (!state.hasScalingMin && !state.hasScalingMax && !state.hasScalingGovernor) {
            continue;
        }
        states.push_back(state);
    }

    if (states.empty()) {
        if (error) {
            *error = "CPU cpufreq directories exist but no scaling_min_freq, scaling_max_freq, or scaling_governor files could be read.";
        }
        return false;
    }

    return true;
}

bool applyState(const CpuProfileSpec& spec,
                const CpuProfileCpuState& state,
                std::string* error)
{
    if (!spec.governor.empty() && state.hasScalingGovernor) {
        if (!writeOneLine(state.scalingGovernorPath, spec.governor, error)) {
            return false;
        }
    }

    long currentMin = 0;
    const bool haveCurrentMin = readLongFile(state.scalingMinPath, currentMin);

    if (spec.maxFrequencyKHz > 0 && state.hasScalingMax) {
        // If the current minimum is higher than the requested maximum, lower the
        // minimum first. With min_frequency = 0 this uses the CPU policy minimum,
        // preserving the user's "auto minimum" intent as much as possible.
        if (haveCurrentMin && currentMin > spec.maxFrequencyKHz && state.hasScalingMin) {
            long minBeforeMax = spec.minFrequencyKHz;
            if (minBeforeMax <= 0 || minBeforeMax > spec.maxFrequencyKHz) {
                long cpuInfoMin = 0;
                if (!readLongFile(joinPathLocal(state.cpufreqDir, "cpuinfo_min_freq"), cpuInfoMin) || cpuInfoMin <= 0) {
                    minBeforeMax = spec.maxFrequencyKHz;
                } else {
                    minBeforeMax = std::min(cpuInfoMin, spec.maxFrequencyKHz);
                }
            }
            if (!writeOneLine(state.scalingMinPath, longToString(minBeforeMax), error)) {
                return false;
            }
        }

        if (!writeOneLine(state.scalingMaxPath, longToString(spec.maxFrequencyKHz), error)) {
            return false;
        }
    }

    if (spec.minFrequencyKHz > 0 && state.hasScalingMin) {
        if (!writeOneLine(state.scalingMinPath, longToString(spec.minFrequencyKHz), error)) {
            return false;
        }
    }

    return true;
}

} // namespace

CpuProfileGuard::~CpuProfileGuard()
{
    restore();
}

bool CpuProfileGuard::applyProfile(const std::string& profileName,
                                   const std::string& configPath,
                                   std::string* error)
{
    if (profileName.empty()) {
        return true;
    }

#if !defined(__linux__)
    if (error) {
        *error = "CPU profiles are currently implemented only for Linux cpufreq sysfs.";
    }
    return false;
#else
    if (!resolveCpuProfileConfig(configPath, resolvedConfigPath_, error)) {
        return false;
    }

    std::string text;
    if (!readTextFile(resolvedConfigPath_, text)) {
        if (error) {
            *error = "failed to read CPU profile config: " + resolvedConfigPath_;
        }
        return false;
    }

    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& e) {
        if (error) {
            *error = std::string("failed to parse CPU profile config: ") + e.what();
        }
        return false;
    }

    if (!parseProfile(root, profileName, spec_, error)) {
        return false;
    }

    if (!captureCpuStates(states_, error)) {
        return false;
    }

    for (const auto& state : states_) {
        if (!applyState(spec_, state, error)) {
            restore();
            return false;
        }
    }

    applied_ = true;
    restored_ = false;

    std::cout << "[CPU] Applied profile '" << spec_.name << "' from " << resolvedConfigPath_;
    if (spec_.maxFrequencyKHz > 0) {
        std::cout << " max=" << (spec_.maxFrequencyKHz / 1000.0) << " MHz";
    }
    if (spec_.minFrequencyKHz > 0) {
        std::cout << " min=" << (spec_.minFrequencyKHz / 1000.0) << " MHz";
    } else {
        std::cout << " min=auto";
    }
    if (!spec_.governor.empty()) {
        std::cout << " governor=" << spec_.governor;
    }
    std::cout << " CPUs=" << states_.size() << "\n";

    return true;
#endif
}

void CpuProfileGuard::restore()
{
    if (!applied_ || restored_ || !spec_.restoreOnExit) {
        return;
    }

    bool ok = true;
    std::string error;
    for (const auto& state : states_) {
        // Restore the upper limit first so the original minimum is accepted even
        // if the profile lowered max_frequency below the original minimum.
        if (state.hasScalingMax && !state.originalMax.empty()) {
            if (!writeOneLine(state.scalingMaxPath, state.originalMax, &error)) {
                ok = false;
            }
        }
        if (state.hasScalingMin && !state.originalMin.empty()) {
            if (!writeOneLine(state.scalingMinPath, state.originalMin, &error)) {
                ok = false;
            }
        }
        if (state.hasScalingGovernor && !state.originalGovernor.empty()) {
            if (!writeOneLine(state.scalingGovernorPath, state.originalGovernor, &error)) {
                ok = false;
            }
        }
    }

    restored_ = true;
    applied_ = false;

    if (ok) {
        std::cout << "[CPU] Restored CPU frequency settings to previous state.\n";
    } else {
        std::cerr << "[CPU] Warning: CPU profile restore was incomplete: " << error << "\n";
    }
}

} // namespace nxframe

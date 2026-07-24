/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/sender_process_manager.h"

#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

bool isValidChannel(const std::string& channel)
{
    return channel == "sdi1" || channel == "sdi2" || channel == "sdi3" || channel == "sdi4";
}

int normalizedExitCode(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::string canonicalFilePath(const std::string& path)
{
    char resolved[PATH_MAX]{};
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

std::string humanizeIdentifier(const std::string& value)
{
    std::string result;
    bool capitalize = true;
    for (unsigned char c : value) {
        if (c == '_' || c == '-') {
            result.push_back(' ');
            capitalize = true;
            continue;
        }
        result.push_back(capitalize ? static_cast<char>(std::toupper(c)) : static_cast<char>(c));
        capitalize = false;
    }
    return result.empty() ? value : result;
}

bool readJsonFile(const std::string& path, json& value)
{
    std::ifstream input(path);
    if (!input) return false;
    try {
        input >> value;
        return value.is_object();
    } catch (...) {
        return false;
    }
}

} // namespace

SenderProcessManager::SenderProcessManager(std::string nxframeExecutable,
                                           std::string cpuProfileConfig)
    : nxframeExecutable_(std::move(nxframeExecutable)),
      cpuProfileConfig_(std::move(cpuProfileConfig))
{
    std::string resolved;
    if (!nxframe::loadCpuProfileSpecs(cpuProfileConfig_, cpuProfiles_, &resolved, &cpuProfileWarning_)) {
        cpuProfiles_.clear();
    } else {
        cpuProfileConfig_ = resolved;
        cpuProfileWarning_.clear();
    }
}

SenderProcessManager::~SenderProcessManager()
{
    shutdownAll();
}

std::string SenderProcessManager::receiverControlPath(pid_t pid)
{
    return "/tmp/nxframe_receiver_control_" + std::to_string(static_cast<long long>(pid)) + ".json";
}

std::string SenderProcessManager::receiverStatePath(pid_t pid)
{
    return "/tmp/nxframe_receiver_state_" + std::to_string(static_cast<long long>(pid)) + ".json";
}

nlohmann::json SenderProcessManager::receiverStateFor(pid_t pid) const
{
    json state;
    return pid > 0 && readJsonFile(receiverStatePath(pid), state) ? state : json::object();
}

nlohmann::json SenderProcessManager::statusFor(const std::string& channel,
                                               const ProcessInfo* info) const
{
    json result = {
        {"ok", true},
        {"channel", channel},
        {"available", available()},
        {"running", info && info->running},
        {"stopping", info && info->stopping},
        {"role", info ? info->role : std::string()}
    };
    if (info && info->running) {
        result["pid"] = static_cast<long long>(info->pid);
        result["decklink_device"] = info->decklinkDevice;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - info->startedAt).count();
        result["uptime_seconds"] = seconds;
        if (info->role == "receiver") {
            result["receiver_state"] = receiverStateFor(info->pid);
        }
    }
    if (info && info->hasExitCode) result["last_exit_code"] = info->lastExitCode;
    result["cpu_profile"] = activeCpuProfile_.empty() ? "system_default" : activeCpuProfile_;
    return result;
}

bool SenderProcessManager::hasRunningProcesses() const
{
    for (const auto& entry : processes_) if (entry.second.running) return true;
    return false;
}

void SenderProcessManager::restoreCpuProfileIfIdle()
{
    if (hasRunningProcesses()) return;
    cpuProfileGuard_.restore(true);
    activeCpuProfile_.clear();
}

bool SenderProcessManager::anyRunning()
{
    reap();
    return hasRunningProcesses();
}

bool SenderProcessManager::hasCpuProfile(const std::string& profileName) const
{
    if (profileName.empty() || profileName == "system_default") return true;
    for (const auto& profile : cpuProfiles_) if (profile.name == profileName) return true;
    return false;
}

nlohmann::json SenderProcessManager::cpuProfiles() const
{
    json result = json::array();
    result.push_back({
        {"id", "system_default"},
        {"name", "System default"},
        {"description", "Keep the operating system CPU frequency policy unchanged."}
    });
    for (const auto& profile : cpuProfiles_) {
        json item = {
            {"id", profile.name},
            {"name", humanizeIdentifier(profile.name)},
            {"description", profile.description},
            {"restore_on_exit", profile.restoreOnExit}
        };
        if (profile.minFrequencyKHz > 0) item["min_frequency_mhz"] = profile.minFrequencyKHz / 1000.0;
        if (profile.maxFrequencyKHz > 0) item["max_frequency_mhz"] = profile.maxFrequencyKHz / 1000.0;
        if (!profile.governor.empty()) item["governor"] = profile.governor;
        result.push_back(item);
    }
    return result;
}

void SenderProcessManager::reap()
{
    for (auto& entry : processes_) {
        ProcessInfo& info = entry.second;
        if (!info.running || info.pid <= 0) continue;
        int status = 0;
        const pid_t result = ::waitpid(info.pid, &status, WNOHANG);
        if (result == info.pid) {
            if (info.role == "receiver") {
                std::remove(receiverControlPath(info.pid).c_str());
                std::remove(receiverStatePath(info.pid).c_str());
            }
            info.running = false;
            info.stopping = false;
            info.hasExitCode = true;
            info.lastExitCode = normalizedExitCode(status);
            info.pid = -1;
        }
    }
    restoreCpuProfileIfIdle();
}

nlohmann::json SenderProcessManager::status(const std::string& channel)
{
    reap();
    const auto found = processes_.find(channel);
    return statusFor(channel, found == processes_.end() ? nullptr : &found->second);
}

bool SenderProcessManager::buildTransportUrl(const std::string& presetPath,
                                             std::string& transportUrl,
                                             std::string* error) const
{
    json preset;
    if (!readJsonFile(presetPath, preset)) {
        if (error) *error = "saved sender configuration is unavailable";
        return false;
    }
    if (!preset.contains("streaming") || !preset["streaming"].is_object()) {
        if (error) *error = "saved sender configuration has no streaming section";
        return false;
    }
    const json& streaming = preset["streaming"];
    const std::string protocol = streaming.value("protocol", std::string());
    const std::string address = streaming.value("address", std::string());
    const int port = streaming.value("port", 0);
    if ((protocol != "srt" && protocol != "udp" && protocol != "rtp") ||
        address.empty() || port < 1 || port > 65535) {
        if (error) *error = "saved sender transport is incomplete";
        return false;
    }
    transportUrl = protocol + "://" + address + ":" + std::to_string(port);
    return true;
}

bool SenderProcessManager::buildReceiverInputUrl(const std::string& presetPath,
                                                 std::string& inputUrl,
                                                 std::string* error) const
{
    json preset;
    if (!readJsonFile(presetPath, preset)) {
        if (error) *error = "saved receiver configuration is unavailable";
        return false;
    }
    if (!preset.contains("receiver_input") || !preset["receiver_input"].is_object()) {
        if (error) *error = "saved receiver configuration has no receiver_input section";
        return false;
    }
    const json& input = preset["receiver_input"];
    const std::string protocol = input.value("protocol", std::string());
    std::string address = input.value("address", std::string());
    const int port = input.value("port", 0);
    if ((protocol != "srt" && protocol != "udp" && protocol != "rtp") || port < 1 || port > 65535) {
        if (error) *error = "saved receiver transport is incomplete";
        return false;
    }
    if (address.empty() || address == "*") address = "0.0.0.0";
    inputUrl = protocol + "://" + address + ":" + std::to_string(port);
    return true;
}

bool SenderProcessManager::prepareCpuProfile(const std::string& cpuProfile,
                                             bool& firstWorker,
                                             std::string* error)
{
    const std::string requested =
        (cpuProfile.empty() || cpuProfile == "system_default") ? std::string() : cpuProfile;
    if (!hasCpuProfile(requested)) {
        if (error) *error = "CPU profile '" + cpuProfile + "' is not available";
        return false;
    }
    firstWorker = !hasRunningProcesses();
    if (!firstWorker && requested != activeCpuProfile_) {
        if (error) *error = "CPU profile cannot change while another SDI channel is running";
        return false;
    }
    if (firstWorker) {
        if (!requested.empty()) {
            std::string cpuError;
            if (!cpuProfileGuard_.applyProfile(requested, cpuProfileConfig_, &cpuError)) {
                if (error) *error = "cannot apply CPU profile '" + requested + "': " + cpuError;
                return false;
            }
        }
        activeCpuProfile_ = requested;
    }
    return true;
}

bool SenderProcessManager::launch(const std::string& channel,
                                  int decklinkDevice,
                                  const std::string& role,
                                  std::vector<std::string> args,
                                  bool firstWorker,
                                  nlohmann::json& response,
                                  std::string* error)
{
    int errorPipe[2] = {-1, -1};
    if (::pipe(errorPipe) != 0) {
        if (error) *error = std::string("cannot create launcher pipe: ") + std::strerror(errno);
        if (firstWorker) restoreCpuProfileIfIdle();
        return false;
    }
    (void)::fcntl(errorPipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int savedErrno = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        if (error) *error = std::string("cannot start NxFrame: ") + std::strerror(savedErrno);
        if (firstWorker) restoreCpuProfileIfIdle();
        return false;
    }
    if (pid == 0) {
        ::close(errorPipe[0]);
        ::setpgid(0, 0);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) argv.push_back(&arg[0]);
        argv.push_back(nullptr);
        if (nxframeExecutable_.find('/') == std::string::npos) ::execvp(nxframeExecutable_.c_str(), argv.data());
        else ::execv(nxframeExecutable_.c_str(), argv.data());
        const int execError = errno;
        (void)::write(errorPipe[1], &execError, sizeof(execError));
        ::_exit(127);
    }

    ::close(errorPipe[1]);
    (void)::setpgid(pid, pid);
    int execError = 0;
    const ssize_t readBytes = ::read(errorPipe[0], &execError, sizeof(execError));
    ::close(errorPipe[0]);
    if (readBytes > 0) {
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        if (error) *error = std::string("cannot execute NxFrame: ") + std::strerror(execError);
        if (firstWorker) restoreCpuProfileIfIdle();
        return false;
    }

    ProcessInfo& info = processes_[channel];
    info.pid = pid;
    info.decklinkDevice = decklinkDevice;
    info.role = role;
    info.running = true;
    info.stopping = false;
    info.hasExitCode = false;
    info.startedAt = std::chrono::steady_clock::now();
    response = statusFor(channel, &info);
    response["message"] = role == "receiver" ? "receiver started" : "streaming started";
    return true;
}

bool SenderProcessManager::start(const std::string& channel,
                                 int decklinkDevice,
                                 const std::string& presetPath,
                                 const std::string& cpuProfile,
                                 nlohmann::json& response,
                                 std::string* error)
{
    reap();
    if (!isValidChannel(channel)) { if (error) *error = "invalid SDI channel"; return false; }
    if (!available()) { if (error) *error = "NxFrame executable is not configured; start NxFrameWeb with --nxframe-executable"; return false; }
    if (processes_[channel].running) { if (error) *error = channel + " is already running"; return false; }
    for (const auto& entry : processes_) {
        if (entry.second.running && entry.second.decklinkDevice == decklinkDevice) {
            if (error) *error = "DeckLink device " + std::to_string(decklinkDevice) + " is already used by " + entry.first;
            return false;
        }
    }
    bool firstWorker = false;
    if (!prepareCpuProfile(cpuProfile, firstWorker, error)) return false;
    std::string url;
    if (!buildTransportUrl(presetPath, url, error)) { if (firstWorker) restoreCpuProfileIfIdle(); return false; }
    const std::string preset = canonicalFilePath(presetPath);
    return launch(channel, decklinkDevice, "sender", {
        nxframeExecutable_, "send", "decklink", std::to_string(decklinkDevice),
        "to", url, "encoder", "preset", preset
    }, firstWorker, response, error);
}

bool SenderProcessManager::startReceiver(const std::string& channel,
                                         int decklinkDevice,
                                         const std::string& presetPath,
                                         const std::string& cpuProfile,
                                         nlohmann::json& response,
                                         std::string* error)
{
    reap();
    if (!isValidChannel(channel)) { if (error) *error = "invalid SDI channel"; return false; }
    if (!available()) { if (error) *error = "NxFrame executable is not configured; start NxFrameWeb with --nxframe-executable"; return false; }
    if (processes_[channel].running) { if (error) *error = channel + " is already running"; return false; }
    for (const auto& entry : processes_) {
        if (entry.second.running && entry.second.decklinkDevice == decklinkDevice) {
            if (error) *error = "DeckLink device " + std::to_string(decklinkDevice) + " is already used by " + entry.first;
            return false;
        }
    }
    bool firstWorker = false;
    if (!prepareCpuProfile(cpuProfile, firstWorker, error)) return false;
    std::string inputUrl;
    if (!buildReceiverInputUrl(presetPath, inputUrl, error)) { if (firstWorker) restoreCpuProfileIfIdle(); return false; }
    const std::string preset = canonicalFilePath(presetPath);
    return launch(channel, decklinkDevice, "receiver", {
        nxframeExecutable_, "--receiver-preset", preset,
        "play", inputUrl, "to", "decklink", std::to_string(decklinkDevice)
    }, firstWorker, response, error);
}

bool SenderProcessManager::updateReceiverRoute(const std::string& channel,
                                               const std::vector<int>& route,
                                               nlohmann::json& response,
                                               std::string* error)
{
    reap();
    const auto found = processes_.find(channel);
    if (found == processes_.end() || !found->second.running || found->second.pid <= 0 || found->second.role != "receiver") {
        if (error) *error = channel + " receiver is not running";
        return false;
    }
    json command = {{"audio_pair_route", route}};
    const std::string path = receiverControlPath(found->second.pid);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) { if (error) *error = "cannot write receiver routing command"; return false; }
        output << command.dump(2) << '\n';
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) *error = std::string("cannot publish receiver routing command: ") + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    response = statusFor(channel, &found->second);
    response["message"] = "audio routing updated";
    return true;
}

bool SenderProcessManager::stop(const std::string& channel,
                                nlohmann::json& response,
                                std::string* error)
{
    reap();
    const auto found = processes_.find(channel);
    if (found == processes_.end() || !found->second.running || found->second.pid <= 0) {
        if (error) *error = channel + " is not running";
        return false;
    }
    ProcessInfo& info = found->second;
    if (::kill(-info.pid, SIGINT) != 0 && errno != ESRCH) {
        if (error) *error = std::string("cannot stop NxFrame: ") + std::strerror(errno);
        return false;
    }
    info.stopping = true;
    response = statusFor(channel, &info);
    response["message"] = "stop requested";
    return true;
}

void SenderProcessManager::shutdownAll()
{
    reap();
    for (auto& entry : processes_) {
        ProcessInfo& info = entry.second;
        if (info.running && info.pid > 0) {
            (void)::kill(-info.pid, SIGINT);
            info.stopping = true;
        }
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        reap();
        if (!hasRunningProcesses()) { restoreCpuProfileIfIdle(); return; }
        ::usleep(50000);
    }
    for (auto& entry : processes_) {
        ProcessInfo& info = entry.second;
        if (info.running && info.pid > 0) {
            (void)::kill(-info.pid, SIGKILL);
            int status = 0;
            (void)::waitpid(info.pid, &status, 0);
            if (info.role == "receiver") {
                std::remove(receiverControlPath(info.pid).c_str());
                std::remove(receiverStatePath(info.pid).c_str());
            }
            info.running = false;
            info.stopping = false;
            info.hasExitCode = true;
            info.lastExitCode = normalizedExitCode(status);
            info.pid = -1;
        }
    }
    restoreCpuProfileIfIdle();
}

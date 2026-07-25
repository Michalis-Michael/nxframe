/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/http_server.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> gShutdownRequested{false};

void handleSignal(int)
{
    gShutdownRequested.store(true, std::memory_order_release);
}

bool directoryExists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool executableFileExists(const std::string& path)
{
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

std::string canonicalFilePath(const std::string& path)
{
    char resolved[PATH_MAX]{};
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

std::string executableDirectory()
{
    char path[PATH_MAX]{};
    const ssize_t count = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (count <= 0) return {};
    path[count] = '\0';
    const std::string full(path);
    const std::size_t slash = full.find_last_of('/');
    return slash == std::string::npos ? std::string() : full.substr(0, slash);
}

std::string joinPath(const std::string& left, const std::string& right)
{
    if (left.empty()) return right;
    return left.back() == '/' ? left + right : left + "/" + right;
}

std::string resolveExistingDirectory(const std::string& requested,
                                     const std::vector<std::string>& candidates)
{
    if (!requested.empty() && directoryExists(requested)) return requested;
    for (const auto& candidate : candidates) {
        if (directoryExists(candidate)) return candidate;
    }
    return {};
}

std::string resolveWebRoot(const std::string& requested)
{
    const std::string exe = executableDirectory();
    return resolveExistingDirectory(requested, {
        "gui/static",
        "../gui/static",
        joinPath(exe, "gui"),
        joinPath(exe, "../share/nxframe/gui"),
        joinPath(exe, "../../share/nxframe/gui")
    });
}

std::string resolveEncoderPresetRoot(const std::string& requested)
{
    const std::string exe = executableDirectory();
    return resolveExistingDirectory(requested, {
        "gui/gui_encoder_presets",
        "../gui/gui_encoder_presets",
        joinPath(exe, "encoder_presets"),
        joinPath(exe, "../share/nxframe/gui/encoder_presets"),
        joinPath(exe, "../../share/nxframe/gui/encoder_presets")
    });
}

std::string resolveNxFrameExecutable(const std::string& requested)
{
    if (!requested.empty()) return executableFileExists(requested) ? canonicalFilePath(requested) : std::string();
    const std::string exe = executableDirectory();
    const std::vector<std::string> candidates = {
        "build/NxFrame",
        "./NxFrame",
        "../build/NxFrame",
        joinPath(exe, "NxFrame"),
        joinPath(exe, "../build/NxFrame"),
        joinPath(exe, "../../build/NxFrame"),
        "/usr/local/bin/NxFrame",
        "/usr/bin/NxFrame"
    };
    for (const auto& candidate : candidates) {
        if (executableFileExists(candidate)) return canonicalFilePath(candidate);
    }
    return {};
}

void printUsage(const char* executable)
{
    std::cout << "Usage: " << executable << " [options]\n\n"
              << "Options:\n"
              << "  --bind <IPv4>                Address to listen on (default 127.0.0.1)\n"
              << "  --port <1-65535>              HTTP port (default 8080)\n"
              << "  --config <path>               Appliance JSON configuration (default config/system.json)\n"
              << "  --web-root <path>             Dashboard static asset directory\n"
              << "  --encoder-presets <path>      Protected GUI encoder-template directory\n"
              << "  --channel-config-root <path>  Per-SDI generated preset directory\n"
              << "  --nxframe-executable <path>   NxFrame CLI executable used for Start Streaming\n"
              << "  --cpu-profile-config <path>  CPU profile definitions (default config/cpu_profiles.json)\n"
              << "  --help                        Show this help\n";
}

bool parsePort(const std::string& value, std::uint16_t& port)
{
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed < 1 || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    WebServerOptions options;
    std::string requestedWebRoot;
    std::string requestedEncoderPresetRoot;
    std::string requestedNxFrameExecutable;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "[GUI] " << name << " requires a value.\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--bind") {
            const char* value = requireValue("--bind");
            if (!value) return 1;
            options.bindAddress = value;
        } else if (arg == "--port") {
            const char* value = requireValue("--port");
            if (!value || !parsePort(value, options.port)) {
                std::cerr << "[GUI] --port must be between 1 and 65535.\n";
                return 1;
            }
        } else if (arg == "--config") {
            const char* value = requireValue("--config");
            if (!value) return 1;
            options.configPath = value;
        } else if (arg == "--web-root") {
            const char* value = requireValue("--web-root");
            if (!value) return 1;
            requestedWebRoot = value;
        } else if (arg == "--encoder-presets") {
            const char* value = requireValue("--encoder-presets");
            if (!value) return 1;
            requestedEncoderPresetRoot = value;
        } else if (arg == "--channel-config-root") {
            const char* value = requireValue("--channel-config-root");
            if (!value) return 1;
            options.channelConfigRoot = value;
        } else if (arg == "--nxframe-executable") {
            const char* value = requireValue("--nxframe-executable");
            if (!value) return 1;
            requestedNxFrameExecutable = value;
        } else if (arg == "--cpu-profile-config") {
            const char* value = requireValue("--cpu-profile-config");
            if (!value) return 1;
            options.cpuProfileConfig = value;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "[GUI] Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    options.webRoot = resolveWebRoot(requestedWebRoot);
    if (options.webRoot.empty()) {
        std::cerr << "[GUI] Cannot locate dashboard assets. Use --web-root <path>.\n";
        return 1;
    }

    options.encoderPresetRoot = resolveEncoderPresetRoot(requestedEncoderPresetRoot);
    if (options.encoderPresetRoot.empty()) {
        std::cerr << "[GUI] Cannot locate GUI encoder templates. Use --encoder-presets <path>.\n";
        return 1;
    }

    options.nxframeExecutable = resolveNxFrameExecutable(requestedNxFrameExecutable);
    if (!requestedNxFrameExecutable.empty() && options.nxframeExecutable.empty()) {
        std::cerr << "[GUI] NxFrame executable is not available or executable: "
                  << requestedNxFrameExecutable << "\n";
        return 1;
    }
    if (options.nxframeExecutable.empty()) {
        std::cerr << "[GUI] Warning: NxFrame executable not found; Start Streaming will be unavailable. "
                  << "Use --nxframe-executable <path>.\n";
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    return runWebServer(options, gShutdownRequested);
}

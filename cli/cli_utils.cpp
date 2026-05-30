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
 * Command-line utility implementation. This file contains reusable CLI helpers for usage text, strict numeric parsing, filesystem checks, and preset path resolution.
 */

#include "cli/cli_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <limits.h>

// Keep command help centralized so sender/receiver syntax changes are visible in one place.
void printUsage()
{
    std::cout << "Usage:\n";
    std::cout << "  send decklink <deviceIndex> to <host>:<port>|srt://<host>:<port>|udp://<host>:<port> encoder preset <preset_name> [--copy] [--allow-test-fallback] [--timing] [--timing-verbose] [--ts-debug] [--ts-capture <file.ts>]\n";
    std::cout << "  send test to <host>:<port>|srt://<host>:<port>|udp://<host>:<port> encoder preset <preset_name> [--copy] [--timing] [--timing-verbose] [--ts-debug] [--ts-capture <file.ts>]\n";
    std::cout << "  play srt://<ip>:<port>|udp://<ip>:<port> to test [--receiver-preset <preset>] [--packed-audio-channels <n>] [--max-audio-pairs <n>] [--audio-route <csv>] [--timing] [--timing-verbose]\n";
    std::cout << "  play srt://<ip>:<port>|udp://<ip>:<port> to decklink <deviceIndex> [--receiver-preset <preset>] [--packed-audio-channels <n>] [--max-audio-pairs <n>] [--audio-route <csv>] [--timing] [--timing-verbose]\n";
    std::cout << "\nNotes:\n";
    std::cout << "  --copy forces the legacy memcpy video path (debug/fallback). Default is zero-copy.\n";
    std::cout << "  --allow-test-fallback lets decklink input fall back to the internal test signal if DeckLink init fails.\n";
    std::cout << "  --timing enables low-overhead per-stage timing summaries once per second.\n";
    std::cout << "  --timing-verbose enables more detailed stage timing breakdowns.\n";
    std::cout << "  --ts-debug enables muxer timestamp debug output for the first packets.\n";
    std::cout << "  --ts-capture <file.ts> saves the muxed MPEG-TS output locally on the sender side.\n";
}

bool fileExists(const std::string& path)
{
    struct stat st;
    return (::stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path)
{
    struct stat st;
    return (::stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

std::string trimCopy(const std::string& value)
{
    const char* ws = " \t\r\n";
    const size_t b = value.find_first_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = value.find_last_not_of(ws);
    return value.substr(b, e - b + 1);
}

bool parseIntStrict(const std::string& text,
                    int minValue,
                    int maxValue,
                    int& out,
                    std::string* error)
{
    const std::string s = trimCopy(text);
    if (s.empty()) {
        if (error) *error = "empty value";
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        if (error) *error = "not an integer: '" + text + "'";
        return false;
    }
    if (value < minValue || value > maxValue) {
        if (error) {
            *error = "value " + std::to_string(value) + " outside allowed range " +
                     std::to_string(minValue) + ".." + std::to_string(maxValue);
        }
        return false;
    }

    out = static_cast<int>(value);
    return true;
}

std::string getCwd()
{
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) {
        return std::string(buf);
    }
    return {};
}

std::string getExeDir()
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

std::string joinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) {
        return b;
    }
    if (a.back() == '/') {
        return a + b;
    }
    return a + "/" + b;
}

// Depth-limited preset search keeps short preset names convenient without scanning the whole filesystem.
std::string findPresetRecursive(const std::string& rootDir,
                                const std::string& filename,
                                int depthLeft)
{
    if (depthLeft < 0 || !dirExists(rootDir)) {
        return {};
    }

    const std::string direct = joinPath(rootDir, filename);
    if (fileExists(direct)) {
        return direct;
    }

    DIR* dir = ::opendir(rootDir.c_str());
    if (!dir) {
        return {};
    }

    std::vector<std::string> childDirs;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.empty() || name == "." || name == "..") {
            continue;
        }
        const std::string child = joinPath(rootDir, name);
        if (dirExists(child)) {
            childDirs.push_back(child);
        }
    }
    ::closedir(dir);

    std::sort(childDirs.begin(), childDirs.end());

    for (const auto& child : childDirs) {
        const std::string found = findPresetRecursive(child, filename, depthLeft - 1);
        if (!found.empty()) {
            return found;
        }
    }

    return {};
}

// Resolve either an explicit JSON path or a short preset name from common install/build locations.
std::string resolvePresetPath(const std::string& presetArg)
{
    if (presetArg.find('/') != std::string::npos) {
        std::string p = presetArg;
        if (p.size() < 5 || p.substr(p.size() - 5) != ".json") {
            p += ".json";
        }
        if (fileExists(p)) {
            return p;
        }
        return {};
    }

    const std::string nameJson = presetArg + ".json";

    // Short preset names are resolved by scanning the whole preset tree.
    // This keeps existing commands stable and allows future folders such as
    // preset/05_customer_profiles/foo.json without code changes.
    std::vector<std::string> roots;

    auto addRoot = [&](const std::string& root) {
        if (!root.empty()) {
            roots.push_back(root);
        }
    };

    addRoot("preset");
    addRoot("presets");
    addRoot("encoders/preset");
    addRoot("encoders/presets");
    addRoot("../preset");
    addRoot("../presets");
    addRoot("../encoders/preset");
    addRoot("../encoders/presets");

    const std::string exeDir = getExeDir();
    if (!exeDir.empty()) {
        addRoot(joinPath(exeDir, "preset"));
        addRoot(joinPath(exeDir, "presets"));
        addRoot(joinPath(exeDir, "encoders/preset"));
        addRoot(joinPath(exeDir, "encoders/presets"));
        addRoot(joinPath(exeDir, "../preset"));
        addRoot(joinPath(exeDir, "../presets"));
        addRoot(joinPath(exeDir, "../encoders/preset"));
        addRoot(joinPath(exeDir, "../encoders/presets"));
        addRoot(joinPath(exeDir, "../../preset"));
        addRoot(joinPath(exeDir, "../../presets"));
        addRoot(joinPath(exeDir, "../../encoders/preset"));
        addRoot(joinPath(exeDir, "../../encoders/presets"));
    }

    // Depth 6 is intentionally conservative: enough for organized profiles,
    // customer folders, and nested experiments, while avoiding unbounded scans.
    for (const auto& root : roots) {
        const std::string found = findPresetRecursive(root, nameJson, 6);
        if (!found.empty()) {
            return found;
        }
    }

    return {};
}

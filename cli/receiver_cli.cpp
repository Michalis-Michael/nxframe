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
 * Receiver CLI preset and audio-routing implementation. This file validates receiver-side JSON options and maps them into Receiver::Config.
 */

#include "cli/receiver_cli.h"

#include "cli/cli_utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Receiver presets are intentionally small; keep validation strict so bad routing does not reach the live playout path.
bool jsonIntStrict(const json& obj,
                   const char* key,
                   int minValue,
                   int maxValue,
                   int& out,
                   std::string* error)
{
    if (!obj.contains(key)) {
        return true;
    }

    if (!obj[key].is_number_integer()) {
        if (error) *error = std::string("'") + key + "' must be an integer";
        return false;
    }

    const int value = obj[key].get<int>();
    if (value < minValue || value > maxValue) {
        if (error) {
            *error = std::string("'") + key + "' value " + std::to_string(value) +
                     " outside allowed range " + std::to_string(minValue) + ".." +
                     std::to_string(maxValue);
        }
        return false;
    }

    out = value;
    return true;
}

} // namespace

// Parse comma-separated one-based input-pair routing used by the receiver audio mapper.
bool parseRouteCsv(const std::string& csv, std::vector<int>& route, std::string* error)
{
    route.clear();

    std::stringstream ss(csv);
    std::string item;
    size_t index = 0;
    while (std::getline(ss, item, ',')) {
        ++index;
        item = trimCopy(item);
        if (item.empty()) {
            continue;
        }

        int value = 0;
        std::string parseError;
        if (!parseIntStrict(item, 0, 64, value, &parseError)) {
            if (error) {
                *error = "invalid --audio-route item #" + std::to_string(index) +
                         ": " + parseError;
            }
            route.clear();
            return false;
        }
        route.push_back(value);
    }

    return true;
}

// Load optional receiver audio settings from either a receiver_audio section or a flat object for compatibility.
bool loadReceiverPreset(const std::string& presetPath,
                        ReceiverCliOptions& opt,
                        std::string* error)
{
    std::ifstream file(presetPath);
    if (!file) {
        if (error) *error = "failed to open file";
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        if (error) *error = std::string("JSON parse error: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("JSON read error: ") + e.what();
        return false;
    }

    const json* section = nullptr;
    if (j.contains("receiver_audio") && j["receiver_audio"].is_object()) {
        section = &j["receiver_audio"];
    } else if (j.is_object()) {
        section = &j;
    } else {
        if (error) *error = "receiver preset root must be a JSON object";
        return false;
    }

    if (!jsonIntStrict(*section, "packed_audio_channels", 2, 64, opt.packedAudioChannels, error)) {
        return false;
    }
    if (!jsonIntStrict(*section, "max_audio_pairs", 1, 32, opt.maxAudioPairs, error)) {
        return false;
    }

    if (section->contains("audio_pair_route")) {
        if (!(*section)["audio_pair_route"].is_array()) {
            if (error) *error = "'audio_pair_route' must be an array";
            return false;
        }

        opt.audioRoute.clear();
        size_t index = 0;
        for (const auto& v : (*section)["audio_pair_route"]) {
            ++index;
            if (!v.is_number_integer()) {
                if (error) {
                    *error = "'audio_pair_route' item #" + std::to_string(index) +
                             " must be an integer";
                }
                return false;
            }
            const int route = v.get<int>();
            if (route < 0 || route > 64) {
                if (error) {
                    *error = "'audio_pair_route' item #" + std::to_string(index) +
                             " outside allowed range 0..64";
                }
                return false;
            }
            opt.audioRoute.push_back(route);
        }
    }

    return true;
}

std::string routeToString(const std::vector<int>& route)
{
    if (route.empty()) {
        return "sequential";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < route.size(); ++i) {
        if (i) oss << ',';
        oss << route[i];
    }
    return oss.str();
}

// Apply validated CLI/preset values to the runtime receiver configuration.
void applyReceiverCliOptions(Receiver::Config& cfg, const ReceiverCliOptions& opt)
{
    cfg.packed_audio_channels = std::max(2, opt.packedAudioChannels);
    cfg.max_audio_pairs = std::max(1, opt.maxAudioPairs);
    cfg.audio_pair_route = opt.audioRoute;
}

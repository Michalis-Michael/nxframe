/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/receiver_config.h"

#include "config/preset_validator.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <utility>

namespace {

using json = nlohmann::json;

bool validChannel(const std::string& channel)
{
    return channel == "sdi1" || channel == "sdi2" || channel == "sdi3" || channel == "sdi4";
}

bool makeDirectories(const std::string& path, std::string* error)
{
    if (path.empty()) return true;
    std::string current;
    if (path.front() == '/') current = "/";
    std::size_t begin = path.front() == '/' ? 1u : 0u;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::string part = path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current.push_back('/');
            current += part;
            struct stat st{};
            if (::stat(current.c_str(), &st) != 0) {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    if (error) *error = "cannot create channel configuration directory: " + std::string(std::strerror(errno));
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                if (error) *error = current + " is not a directory";
                return false;
            }
        }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return true;
}

bool isIntegerInRange(const json& object,
                      const char* key,
                      int minimum,
                      int maximum,
                      int& output,
                      std::string* error)
{
    if (!object.contains(key) || !object[key].is_number_integer()) {
        if (error) *error = std::string(key) + " must be an integer";
        return false;
    }
    output = object[key].get<int>();
    if (output < minimum || output > maximum) {
        if (error) *error = std::string(key) + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum);
        return false;
    }
    return true;
}

bool isString(const json& object, const char* key, std::string& output, std::string* error)
{
    if (!object.contains(key) || !object[key].is_string()) {
        if (error) *error = std::string(key) + " must be a string";
        return false;
    }
    output = object[key].get<std::string>();
    return true;
}

bool wildcardAddress(const std::string& address)
{
    return address.empty() || address == "*" || address == "0.0.0.0";
}

json defaultSettings()
{
    return {
        {"configuration_name", "Receiver"},
        {"input", {
            {"protocol", "srt"},
            {"mode", "listener"},
            {"address", "0.0.0.0"},
            {"port", 5000},
            {"latency", 250},
            {"streamid", ""},
            {"passphrase", ""},
            {"pbkeylen", 0},
            {"interface", ""}
        }},
        {"output", {
            {"packed_audio_channels", 16}
        }},
        {"audio_pair_route", json::array({1, 2, 3, 4, 5, 6, 7, 8})}
    };
}

} // namespace

ReceiverConfigStore::ReceiverConfigStore(std::string channelRoot)
    : channelRoot_(std::move(channelRoot))
{
}

std::string ReceiverConfigStore::channelPath(const std::string& channel) const
{
    return channelRoot_ + "/" + channel + ".json";
}

nlohmann::json ReceiverConfigStore::loadChannel(const std::string& channel,
                                                std::string* warning) const
{
    json result = {
        {"exists", false},
        {"settings", defaultSettings()}
    };
    if (!validChannel(channel)) {
        if (warning) *warning = "invalid SDI channel";
        return result;
    }

    std::ifstream input(channelPath(channel));
    if (!input) return result;

    json preset;
    try {
        input >> preset;
    } catch (const std::exception& ex) {
        if (warning) *warning = std::string("cannot read saved receiver configuration: ") + ex.what();
        return result;
    }
    if (preset.value("role", std::string()) != "receiver") return result;

    json settings = defaultSettings();
    settings["configuration_name"] = preset.value("configuration_name", std::string("Receiver"));
    if (preset.contains("receiver_input") && preset["receiver_input"].is_object()) {
        settings["input"] = preset["receiver_input"];
    }
    if (preset.contains("receiver_audio") && preset["receiver_audio"].is_object()) {
        const json& audio = preset["receiver_audio"];
        settings["output"]["packed_audio_channels"] = audio.value("packed_audio_channels", 16);
        if (audio.contains("audio_pair_route") && audio["audio_pair_route"].is_array()) {
            settings["audio_pair_route"] = audio["audio_pair_route"];
        }
    }
    result["exists"] = true;
    result["settings"] = settings;
    return result;
}

bool ReceiverConfigStore::buildPreset(const std::string& channel,
                                      const nlohmann::json& request,
                                      nlohmann::json& preset,
                                      nlohmann::json& response,
                                      std::string* error) const
{
    if (!validChannel(channel)) {
        if (error) *error = "invalid SDI channel";
        return false;
    }
    if (!request.is_object()) {
        if (error) *error = "receiver request must be an object";
        return false;
    }

    const json& input = request.value("input", json::object());
    const json& output = request.value("output", json::object());
    if (!input.is_object() || !output.is_object()) {
        if (error) *error = "input and output must be objects";
        return false;
    }

    std::string protocol;
    std::string mode;
    std::string address;
    std::string streamid;
    std::string passphrase;
    std::string interfaceName;
    int port = 0;
    int latency = 0;
    int pbkeylen = 0;
    int packedChannels = 0;

    if (!isString(input, "protocol", protocol, error) ||
        !isString(input, "mode", mode, error) ||
        !isString(input, "address", address, error) ||
        !isString(input, "streamid", streamid, error) ||
        !isString(input, "passphrase", passphrase, error) ||
        !isString(input, "interface", interfaceName, error) ||
        !isIntegerInRange(input, "port", 1, 65535, port, error) ||
        !isIntegerInRange(input, "latency", 20, 30000, latency, error) ||
        !isIntegerInRange(input, "pbkeylen", 0, 32, pbkeylen, error) ||
        !isIntegerInRange(output, "packed_audio_channels", 2, 16, packedChannels, error)) {
        return false;
    }

    if (protocol != "srt" && protocol != "udp" && protocol != "rtp") {
        if (error) *error = "protocol must be srt, udp, or rtp";
        return false;
    }
    if (packedChannels != 2 && packedChannels != 4 && packedChannels != 8 && packedChannels != 16) {
        if (error) *error = "packed_audio_channels must be 2, 4, 8, or 16";
        return false;
    }
    if (protocol == "srt") {
        if (mode != "caller" && mode != "listener" && mode != "rendezvous") {
            if (error) *error = "SRT mode must be caller, listener, or rendezvous";
            return false;
        }
        if ((mode == "caller" || mode == "rendezvous") && wildcardAddress(address)) {
            if (error) *error = "SRT caller and rendezvous modes require a remote address";
            return false;
        }
        if (!passphrase.empty() && (passphrase.size() < 10 || passphrase.size() > 79)) {
            if (error) *error = "SRT passphrase must be empty or 10-79 characters";
            return false;
        }
        if (pbkeylen != 0 && pbkeylen != 16 && pbkeylen != 24 && pbkeylen != 32) {
            if (error) *error = "SRT AES key length must be 0, 16, 24, or 32 bytes";
            return false;
        }
    } else {
        mode = "listener";
        latency = 120;
        streamid.clear();
        passphrase.clear();
        pbkeylen = 0;
        if (address.empty()) address = "0.0.0.0";
    }

    const int outputPairs = packedChannels / 2;
    json route = request.value("audio_pair_route", json::array());
    if (!route.is_array()) {
        if (error) *error = "audio_pair_route must be an array";
        return false;
    }
    json normalizedRoute = json::array();
    for (int index = 0; index < outputPairs; ++index) {
        int value = index + 1;
        if (static_cast<std::size_t>(index) < route.size()) {
            if (!route[index].is_number_integer()) {
                if (error) *error = "audio route items must be integers";
                return false;
            }
            value = route[index].get<int>();
        }
        if (value < 0 || value > 64) {
            if (error) *error = "audio route values must be between 0 and 64";
            return false;
        }
        normalizedRoute.push_back(value);
    }

    const std::string configurationName = request.value("configuration_name", std::string("Receiver"));
    if (configurationName.empty() || configurationName.size() > 96) {
        if (error) *error = "configuration name must contain 1-96 characters";
        return false;
    }

    preset = {
        {"channel", channel},
        {"role", "receiver"},
        {"configuration_name", configurationName},
        {"receiver_input", {
            {"protocol", protocol},
            {"mode", mode},
            {"address", address},
            {"port", port},
            {"latency", latency},
            {"streamid", streamid},
            {"passphrase", passphrase},
            {"pbkeylen", pbkeylen},
            {"interface", interfaceName}
        }},
        {"receiver_audio", {
            {"packed_audio_channels", packedChannels},
            {"max_audio_pairs", outputPairs},
            {"audio_pair_route", normalizedRoute}
        }}
    };

    const PresetValidator::Result validation =
        PresetValidator::validateJson(preset, PresetValidator::Kind::Receiver);
    if (!validation.ok()) {
        if (error) *error = validation.errors.empty() ? "receiver preset validation failed" : validation.errors.front();
        return false;
    }

    response = {
        {"ok", true},
        {"channel", channel},
        {"configuration_name", configurationName},
        {"settings", {
            {"configuration_name", configurationName},
            {"input", preset["receiver_input"]},
            {"output", {{"packed_audio_channels", packedChannels}}},
            {"audio_pair_route", normalizedRoute}
        }}
    };
    return true;
}

bool ReceiverConfigStore::validateChannelRequest(const std::string& channel,
                                                 const nlohmann::json& request,
                                                 nlohmann::json& response,
                                                 std::string* error) const
{
    json preset;
    return buildPreset(channel, request, preset, response, error);
}

bool ReceiverConfigStore::saveChannel(const std::string& channel,
                                      const nlohmann::json& request,
                                      nlohmann::json& response,
                                      std::string* error) const
{
    json preset;
    if (!buildPreset(channel, request, preset, response, error)) return false;
    if (!makeDirectories(channelRoot_, error)) return false;

    const std::string path = channelPath(channel);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            if (error) *error = "cannot open receiver configuration for writing";
            return false;
        }
        output << preset.dump(2) << '\n';
        if (!output.good()) {
            if (error) *error = "cannot write receiver configuration";
            output.close();
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) *error = std::string("cannot replace receiver configuration: ") + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    response["message"] = "receiver configuration saved";
    return true;
}

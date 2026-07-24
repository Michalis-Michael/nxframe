/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/admin_config.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

using json = nlohmann::json;

bool isValidIpv4(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    in_addr addr{};
    return ::inet_pton(AF_INET, value.c_str(), &addr) == 1;
}

bool isValidOptionalIpv4(const std::string& value)
{
    return value.empty() || isValidIpv4(value);
}

bool isSafeProfileId(const std::string& value)
{
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool requireStringField(const json& object,
                        const char* field,
                        const std::string& path,
                        bool required,
                        std::string* error)
{
    if (!object.contains(field)) {
        if (!required) return true;
        if (error) *error = path + "." + field + " is required";
        return false;
    }
    if (!object[field].is_string()) {
        if (error) *error = path + "." + field + " must be a string";
        return false;
    }
    return true;
}

bool ensureParentDirectories(const std::string& filePath, std::string* error)
{
    const std::size_t slash = filePath.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return true;
    }

    const std::string parent = filePath.substr(0, slash);
    std::string current;
    if (!parent.empty() && parent[0] == '/') {
        current = "/";
    }

    std::size_t begin = (parent.empty() || parent[0] != '/') ? 0 : 1;
    while (begin <= parent.size()) {
        const std::size_t end = parent.find('/', begin);
        const std::string part = parent.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') {
                current += '/';
            }
            current += part;
            struct stat st{};
            if (::stat(current.c_str(), &st) != 0) {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    if (error) {
                        *error = "cannot create directory '" + current + "': " + std::strerror(errno);
                    }
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                if (error) {
                    *error = "path component is not a directory: '" + current + "'";
                }
                return false;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool validateNetworkRole(const json& network, const char* role, std::string* error)
{
    const std::string path = std::string("network.") + role;
    if (!network.contains(role) || !network[role].is_object()) {
        if (error) *error = path + " must be an object";
        return false;
    }

    const json& entry = network[role];
    if (!requireStringField(entry, "interface", path, true, error) ||
        !requireStringField(entry, "mode", path, false, error) ||
        !requireStringField(entry, "address", path, false, error) ||
        !requireStringField(entry, "netmask", path, false, error) ||
        !requireStringField(entry, "gateway", path, false, error)) {
        return false;
    }

    const std::string mode = entry.value("mode", "dhcp");
    if (mode != "dhcp" && mode != "static") {
        if (error) *error = path + ".mode must be 'dhcp' or 'static'";
        return false;
    }

    if (mode == "static") {
        const std::string address = entry.value("address", std::string());
        const std::string netmask = entry.value("netmask", std::string());
        const std::string gateway = entry.value("gateway", std::string());
        if (!isValidIpv4(address)) {
            if (error) *error = path + ".address is not a valid IPv4 address";
            return false;
        }
        if (!isValidIpv4(netmask)) {
            if (error) *error = path + ".netmask is not a valid IPv4 netmask";
            return false;
        }
        if (!isValidOptionalIpv4(gateway)) {
            if (error) *error = path + ".gateway is not a valid IPv4 address";
            return false;
        }
    }

    if (entry.contains("dns")) {
        if (!entry["dns"].is_array()) {
            if (error) *error = path + ".dns must be an array";
            return false;
        }
        for (const auto& value : entry["dns"]) {
            if (!value.is_string() || !isValidIpv4(value.get<std::string>())) {
                if (error) *error = path + ".dns contains an invalid IPv4 address";
                return false;
            }
        }
    }
    return true;
}

} // namespace

AdminConfigStore::AdminConfigStore(std::string path)
    : path_(std::move(path))
{
}

nlohmann::json AdminConfigStore::load(std::string* warning) const
{
    std::ifstream input(path_);
    if (!input.is_open()) {
        if (warning) {
            *warning = "configuration file does not exist yet; defaults are active";
        }
        return json();
    }

    try {
        json config;
        input >> config;
        std::string validationError;
        if (!validate(config, &validationError)) {
            if (warning) {
                *warning = "configuration validation failed: " + validationError;
            }
            return json();
        }
        return config;
    } catch (const std::exception& ex) {
        if (warning) {
            *warning = std::string("cannot parse configuration: ") + ex.what();
        }
        return json();
    }
}

bool AdminConfigStore::save(const nlohmann::json& config, std::string* error) const
{
    std::string validationError;
    if (!validate(config, &validationError)) {
        if (error) *error = validationError;
        return false;
    }

    if (!ensureParentDirectories(path_, error)) {
        return false;
    }

    const std::string temporary = path_ + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            if (error) *error = "cannot open temporary configuration file for writing";
            return false;
        }
        output << config.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            if (error) *error = "failed while writing temporary configuration file";
            output.close();
            std::remove(temporary.c_str());
            return false;
        }
    }

    if (::rename(temporary.c_str(), path_.c_str()) != 0) {
        if (error) *error = "cannot replace configuration file: " + std::string(std::strerror(errno));
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

nlohmann::json AdminConfigStore::makeDefault(const std::string& deviceName,
                                              const std::string& controlInterface,
                                              const std::string& streamingInterface)
{
    json config = {
        {"device_name", deviceName.empty() ? "NxFrame" : deviceName},
        {"network", {
            {"control", {
                {"interface", controlInterface},
                {"mode", "dhcp"},
                {"address", ""},
                {"netmask", "255.255.255.0"},
                {"gateway", ""},
                {"dns", json::array()}
            }},
            {"streaming", {
                {"interface", streamingInterface},
                {"mode", "static"},
                {"address", "192.168.10.25"},
                {"netmask", "255.255.255.0"},
                {"gateway", ""},
                {"dns", json::array()},
                {"multicast_ttl", 16}
            }}
        }},
        {"cpu", {
            {"profile", "system_default"}
        }},
        {"sdi_ports", json::array()}
    };

    for (int i = 0; i < 4; ++i) {
        config["sdi_ports"].push_back({
            {"id", "sdi" + std::to_string(i + 1)},
            {"name", "SDI " + std::to_string(i + 1)},
            {"decklink_device", i},
            {"role", "disabled"}
        });
    }
    return config;
}

bool AdminConfigStore::validate(const nlohmann::json& config, std::string* error)
{
    if (!config.is_object()) {
        if (error) *error = "configuration root must be an object";
        return false;
    }

    if (!config.contains("device_name") || !config["device_name"].is_string()) {
        if (error) *error = "device_name must be a string";
        return false;
    }
    const std::string deviceName = config["device_name"].get<std::string>();
    if (deviceName.empty() || deviceName.size() > 64) {
        if (error) *error = "device_name must contain 1 to 64 characters";
        return false;
    }

    if (!config.contains("network") || !config["network"].is_object()) {
        if (error) *error = "network must be an object";
        return false;
    }
    if (!validateNetworkRole(config["network"], "control", error) ||
        !validateNetworkRole(config["network"], "streaming", error)) {
        return false;
    }

    if (config.contains("cpu")) {
        if (!config["cpu"].is_object()) {
            if (error) *error = "cpu must be an object";
            return false;
        }
        if (!config["cpu"].contains("profile") || !config["cpu"]["profile"].is_string()) {
            if (error) *error = "cpu.profile must be a string";
            return false;
        }
        if (!isSafeProfileId(config["cpu"]["profile"].get<std::string>())) {
            if (error) *error = "cpu.profile contains unsupported characters";
            return false;
        }
    }

    if (!config.contains("sdi_ports") || !config["sdi_ports"].is_array()) {
        if (error) *error = "sdi_ports must be an array";
        return false;
    }
    if (config["sdi_ports"].size() != 4) {
        if (error) *error = "sdi_ports must contain exactly four ports";
        return false;
    }

    for (std::size_t i = 0; i < config["sdi_ports"].size(); ++i) {
        const auto& port = config["sdi_ports"][i];
        if (!port.is_object()) {
            if (error) *error = "each sdi_ports entry must be an object";
            return false;
        }
        const std::string role = port.value("role", std::string());
        if (role != "disabled" && role != "sender" && role != "receiver") {
            if (error) *error = "SDI role must be disabled, sender, or receiver";
            return false;
        }
        if (!port.contains("decklink_device") || !port["decklink_device"].is_number_integer()) {
            if (error) *error = "each SDI port needs an integer decklink_device";
            return false;
        }
        const int device = port["decklink_device"].get<int>();
        if (device < 0 || device > 128) {
            if (error) *error = "decklink_device must be between 0 and 128";
            return false;
        }
    }

    return true;
}

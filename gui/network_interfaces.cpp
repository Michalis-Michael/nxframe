/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/network_interfaces.h"

#include <arpa/inet.h>
#include <fstream>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <map>
#include <net/if.h>
#include <sstream>
#include <string>

namespace {

using json = nlohmann::json;

std::string addressToString(const sockaddr* address)
{
    if (!address || address->sa_family != AF_INET) {
        return {};
    }
    char text[INET_ADDRSTRLEN]{};
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    if (!::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) {
        return {};
    }
    return text;
}

std::string readLine(const std::string& path)
{
    std::ifstream input(path);
    std::string line;
    if (input.is_open()) {
        std::getline(input, line);
    }
    return line;
}

std::string macToString(const sockaddr* address)
{
    if (!address || address->sa_family != AF_PACKET) {
        return {};
    }
    const auto* packet = reinterpret_cast<const sockaddr_ll*>(address);
    if (packet->sll_halen < 6) {
        return {};
    }
    std::ostringstream out;
    out << std::hex;
    for (int i = 0; i < 6; ++i) {
        if (i) out << ':';
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned int>(packet->sll_addr[i]);
    }
    return out.str();
}

} // namespace

nlohmann::json enumerateNetworkInterfaces()
{
    std::map<std::string, json> interfaces;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return json::array();
    }

    for (ifaddrs* item = list; item; item = item->ifa_next) {
        if (!item->ifa_name || !item->ifa_addr) {
            continue;
        }
        const std::string name(item->ifa_name);
        if (name == "lo") {
            continue;
        }

        json& entry = interfaces[name];
        entry["name"] = name;
        entry["up"] = (item->ifa_flags & IFF_UP) != 0;
        entry["running"] = (item->ifa_flags & IFF_RUNNING) != 0;
        entry["loopback"] = (item->ifa_flags & IFF_LOOPBACK) != 0;

        if (item->ifa_addr->sa_family == AF_INET) {
            const std::string address = addressToString(item->ifa_addr);
            if (!address.empty()) entry["address"] = address;
            const std::string netmask = addressToString(item->ifa_netmask);
            if (!netmask.empty()) entry["netmask"] = netmask;
        } else if (item->ifa_addr->sa_family == AF_PACKET) {
            const std::string mac = macToString(item->ifa_addr);
            if (!mac.empty()) entry["mac"] = mac;
        }
    }
    ::freeifaddrs(list);

    json result = json::array();
    for (auto& pair : interfaces) {
        json& entry = pair.second;
        const std::string base = "/sys/class/net/" + pair.first + "/";
        const std::string operstate = readLine(base + "operstate");
        if (!operstate.empty()) entry["operstate"] = operstate;
        const std::string speed = readLine(base + "speed");
        if (!speed.empty() && speed != "-1") {
            try {
                entry["speed_mbps"] = std::stoi(speed);
            } catch (...) {
                // Some virtual drivers expose non-numeric speed values.
            }
        }
        result.push_back(entry);
    }
    return result;
}

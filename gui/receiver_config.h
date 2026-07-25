/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Receiver GUI configuration persistence. The browser submits operator-facing
 * transport and audio-routing settings; this class validates them and writes a
 * complete receiver preset consumable by the existing NxFrame CLI.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>

class ReceiverConfigStore {
public:
    explicit ReceiverConfigStore(std::string channelRoot);

    nlohmann::json loadChannel(const std::string& channel,
                               std::string* warning = nullptr) const;

    bool validateChannelRequest(const std::string& channel,
                                const nlohmann::json& request,
                                nlohmann::json& response,
                                std::string* error = nullptr) const;

    bool saveChannel(const std::string& channel,
                     const nlohmann::json& request,
                     nlohmann::json& response,
                     std::string* error = nullptr) const;

    std::string channelPath(const std::string& channel) const;

private:
    bool buildPreset(const std::string& channel,
                     const nlohmann::json& request,
                     nlohmann::json& preset,
                     nlohmann::json& response,
                     std::string* error) const;

    std::string channelRoot_;
};

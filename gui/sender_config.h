/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Protected GUI encoder-template loading and per-SDI sender configuration
 * generation. The browser submits only approved operator fields; this class
 * derives dependent encoder/transport values and writes a complete preset.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>

class SenderConfigStore {
public:
    SenderConfigStore(std::string templateRoot, std::string channelRoot);

    nlohmann::json listTemplates(std::string* warning = nullptr) const;
    nlohmann::json loadChannel(const std::string& channel, std::string* warning = nullptr) const;

    bool validateChannelRequest(const std::string& channel,
                                const nlohmann::json& request,
                                nlohmann::json& response,
                                std::string* error = nullptr) const;

    bool saveChannel(const std::string& channel,
                     const nlohmann::json& request,
                     nlohmann::json& response,
                     std::string* error = nullptr) const;

    const std::string& templateRoot() const { return templateRoot_; }
    const std::string& channelRoot() const { return channelRoot_; }
    std::string channelPath(const std::string& channel) const;

private:
    bool buildChannelPreset(const std::string& channel,
                            const nlohmann::json& request,
                            nlohmann::json& preset,
                            nlohmann::json& response,
                            std::string* error) const;

    bool loadTemplate(const std::string& templateId,
                      nlohmann::json& preset,
                      nlohmann::json* metadata,
                      std::string* error) const;

    std::string templateRoot_;
    std::string channelRoot_;
};

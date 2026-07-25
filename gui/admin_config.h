/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Persistent JSON configuration storage and validation for the web control plane.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>

class AdminConfigStore {
public:
    explicit AdminConfigStore(std::string path);

    nlohmann::json load(std::string* warning = nullptr) const;
    bool save(const nlohmann::json& config, std::string* error = nullptr) const;

    static nlohmann::json makeDefault(const std::string& deviceName,
                                      const std::string& controlInterface,
                                      const std::string& streamingInterface);
    static bool validate(const nlohmann::json& config, std::string* error = nullptr);

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

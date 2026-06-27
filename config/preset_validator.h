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
 * Preset validation declarations for sender and receiver JSON configuration files.
 */

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class PresetValidator {
public:
    struct Result {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        bool ok() const { return errors.empty(); }
    };

    enum class Kind {
        Sender,
        Receiver
    };

    static Result validateFile(const std::string& path, Kind kind);
    static Result validateJson(const nlohmann::json& root, Kind kind);
    static void printResult(const Result& result, const std::string& path, Kind kind);

private:
    PresetValidator() = delete;
};

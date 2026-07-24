/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "gui/admin_config.h"
#include "gui/sender_config.h"
#include "gui/receiver_config.h"

#include <atomic>
#include <cstdint>
#include <string>

struct WebServerOptions {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string webRoot;
    std::string configPath = "config/system.json";
    std::string encoderPresetRoot = "gui/gui_encoder_presets";
    std::string channelConfigRoot = "config/channels";
    std::string nxframeExecutable;
    std::string cpuProfileConfig = "config/cpu_profiles.json";
};

int runWebServer(const WebServerOptions& options, std::atomic<bool>& shutdownRequested);

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
 * Receiver CLI option declarations for audio routing, packed audio configuration, and preset loading.
 */

#pragma once

#include <string>
#include <vector>

#include "receiver/receiver.h"

struct ReceiverCliOptions {
    std::string presetPath;

    bool hasInputConfig = false;
    std::string inputProtocol = "srt";
    std::string inputMode = "listener";
    std::string inputAddress = "0.0.0.0";
    int inputPort = 5000;
    int srtLatency = 120;
    std::string srtStreamId;
    std::string srtPassphrase;
    int srtPbKeyLen = 0;
    std::string inputInterface;
    int packedAudioChannels = 16;
    int maxAudioPairs = 8;
    std::vector<int> audioRoute;
};

bool parseRouteCsv(const std::string& csv, std::vector<int>& route, std::string* error);
bool loadReceiverPreset(const std::string& presetPath,
                        ReceiverCliOptions& opt,
                        std::string* error);

std::string routeToString(const std::vector<int>& route);
void applyReceiverCliOptions(Receiver::Config& cfg, const ReceiverCliOptions& opt);

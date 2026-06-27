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
 * Transport URL declarations for SRT, UDP and RTP command-line addressing.
 */

#pragma once

#include <string>

#include "receiver/receiver.h"

enum class TransportScheme {
    SRT,
    UDP,
    RTP
};

struct TransportUrl {
    TransportScheme scheme = TransportScheme::SRT;
    std::string host;
    int port = 0;
    bool valid = false;
    bool explicitScheme = false;
};

TransportUrl parseTransportUrl(const std::string& url);
bool isListenerAddress(const std::string& host);
bool isIPv4MulticastAddress(const std::string& host);
void configureReceiverTransport(Receiver::Config& cfg, const TransportUrl& src);
std::string receiverTransportModeString(const Receiver::Config& cfg);

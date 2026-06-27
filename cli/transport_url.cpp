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
 * Transport URL parser and receiver transport mapper. This file converts user-facing SRT/UDP/RTP URLs into Receiver::Config transport settings.
 */

#include "cli/transport_url.h"

#include "cli/cli_utils.h"
#include "receiver/srt_input.h"

#include <arpa/inet.h>
#include <cstdint>

// Accepted forms are srt://host:port, udp://host:port, rtp://host:port, or host:port with SRT as the default.
TransportUrl parseTransportUrl(const std::string& url)
{
    TransportUrl out;
    std::string s = url;

    const std::string srtPrefix = "srt://";
    const std::string udpPrefix = "udp://";
    const std::string rtpPrefix = "rtp://";
    if (s.compare(0, srtPrefix.size(), srtPrefix) == 0) {
        out.scheme = TransportScheme::SRT;
        out.explicitScheme = true;
        s = s.substr(srtPrefix.size());
    } else if (s.compare(0, udpPrefix.size(), udpPrefix) == 0) {
        out.scheme = TransportScheme::UDP;
        out.explicitScheme = true;
        s = s.substr(udpPrefix.size());
    } else if (s.compare(0, rtpPrefix.size(), rtpPrefix) == 0) {
        out.scheme = TransportScheme::RTP;
        out.explicitScheme = true;
        s = s.substr(rtpPrefix.size());
    }

    const auto pos = s.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= s.size()) {
        return out;
    }

    out.host = s.substr(0, pos);
    std::string err;
    if (!parseIntStrict(s.substr(pos + 1), 1, 65535, out.port, &err)) {
        return out;
    }
    out.valid = !out.host.empty();
    return out;
}

// Special listener hosts let the same CLI grammar express SRT listener mode and UDP/RTP bind addresses.
bool isListenerAddress(const std::string& host)
{
    return host.empty() || host == "0.0.0.0" || host == "*" || host == "localhost-listen";
}

bool isIPv4MulticastAddress(const std::string& host)
{
    in_addr ipv4{};
    if (::inet_pton(AF_INET, host.c_str(), &ipv4) != 1) {
        return false;
    }
    const uint32_t addr = ntohl(ipv4.s_addr);
    return addr >= 0xE0000000u && addr <= 0xEFFFFFFFu;
}

// Convert the parsed user URL into the receiver transport configuration without opening sockets here.
void configureReceiverTransport(Receiver::Config& cfg, const TransportUrl& src)
{
    if (src.scheme == TransportScheme::UDP || src.scheme == TransportScheme::RTP) {
        cfg.transport = Receiver::Transport::UDP;
        cfg.udp.port = src.port;
        cfg.udp.rtp_depacketize = (src.scheme == TransportScheme::RTP);
        if (isListenerAddress(src.host) || isIPv4MulticastAddress(src.host)) {
            cfg.udp.bind_address = "0.0.0.0";
        } else {
            cfg.udp.bind_address = src.host;
        }
        if (isIPv4MulticastAddress(src.host)) {
            cfg.udp.multicast_group = src.host;
        }
        return;
    }

    cfg.transport = Receiver::Transport::SRT;
    cfg.srt.address = src.host;
    cfg.srt.port = src.port;
    cfg.srt.mode = isListenerAddress(src.host) ? SRTInput::Mode::Listener : SRTInput::Mode::Caller;
    if (cfg.srt.mode == SRTInput::Mode::Listener) {
        cfg.srt.bind_address = src.host.empty() || src.host == "*" ? "0.0.0.0" : src.host;
    }
}

std::string receiverTransportModeString(const Receiver::Config& cfg)
{
    if (cfg.transport == Receiver::Transport::UDP) {
        return cfg.udp.rtp_depacketize ? std::string("rtp-listener") : std::string("udp-listener");
    }
    return SRTInput::modeToString(cfg.srt.mode);
}

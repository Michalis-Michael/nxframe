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
 * Sender application runner. This file builds the SenderPipeline configuration from CLI values and starts the live contribution sender.
 */

#include "app/send_app.h"

#include "cli/cli_utils.h"
#include "cli/transport_url.h"
#include "config/preset_validator.h"
#include "output/output_manager.h"
#include "sender/sender_pipeline.h"

#include <iostream>

int runSendApp(const std::string& inputType,
               const std::string& cardInput,
               const std::string& transportUrl,
               const std::string& presetName,
               bool forceCopy,
               bool allowTestFallback,
               bool timingEnabled,
               bool timingVerbose,
               bool tsDebug,
               const std::string& tsCapturePath,
               std::atomic<bool>& shutdownRequested)
{
    // Validate the transport before constructing the sender pipeline. This keeps
    // invalid network destinations out of the muxer/transport layer.
    const TransportUrl dst = parseTransportUrl(transportUrl);
    if (!dst.valid) {
        std::cerr << "[Main] Error: Invalid transport address format. Expected <host>:<port>, srt://host:port, udp://host:port, or rtp://host:port.\n";
        return -1;
    }

    // UDP/RTP senders require a concrete destination. Listener-style addresses
    // are valid for receive-side sockets but would make sender output ambiguous.
    if ((dst.scheme == TransportScheme::UDP || dst.scheme == TransportScheme::RTP) && isListenerAddress(dst.host)) {
        std::cerr << "[Main] Error: UDP output needs a real destination address, not '"
                  << dst.host << "'. Use the receiver IP or a multicast group.\n";
        return -1;
    }

    const std::string presetFile = resolvePresetPath(presetName);
    if (presetFile.empty()) {
        std::cerr << "[Main] ERROR: Failed to locate preset '" << presetName << "'.\n";
        std::cerr << "[Main] CWD: " << getCwd() << "\n";
        std::cerr << "[Main] EXE DIR: " << getExeDir() << "\n";
        std::cerr << "[Main] Tip: pass a full path to the preset JSON.\n";
        return -1;
    }

    // Preset validation is performed before any device, encoder, muxer, or
    // transport resource is opened.
    const PresetValidator::Result presetValidation =
        PresetValidator::validateFile(presetFile, PresetValidator::Kind::Sender);
    PresetValidator::printResult(presetValidation, presetFile, PresetValidator::Kind::Sender);
    if (!presetValidation.ok()) {
        return -1;
    }

    std::cout << "[Main] Using preset file: " << presetFile << "\n";
    std::cout << "[Main] Video path: " << (forceCopy ? "COPY (legacy)" : "ZERO-COPY (default)") << "\n";
    if (timingEnabled) {
        std::cout << "[Main] Stage timing: ENABLED" << (timingVerbose ? " (verbose)" : "") << "\n";
    }
    if (tsDebug) {
        std::cout << "[Main] TS timestamp debug: ENABLED\n";
    }
    if (!tsCapturePath.empty()) {
        std::cout << "[Main] TS capture file: " << tsCapturePath << "\n";
    }

    // The app layer only translates user input into a pipeline configuration.
    // SenderPipeline owns the input, encoder workers, muxer, and transport.
    SenderPipeline::Config senderConfig;
    senderConfig.inputType = inputType;
    senderConfig.cardInput = cardInput;
    senderConfig.allowTestFallback = allowTestFallback;
    senderConfig.forceCopy = forceCopy;
    senderConfig.presetFile = presetFile;
    if (dst.scheme == TransportScheme::UDP) {
        senderConfig.transport = OutputManager::SenderTransport::UDP;
    } else if (dst.scheme == TransportScheme::RTP) {
        senderConfig.transport = OutputManager::SenderTransport::RTP;
    } else {
        senderConfig.transport = OutputManager::SenderTransport::SRT;
    }
    senderConfig.transportAddress = dst.host;
    senderConfig.transportPort = dst.port;
    senderConfig.tsDebug = tsDebug;
    senderConfig.tsCapturePath = tsCapturePath;
    senderConfig.externalStopFlag = &shutdownRequested;

    SenderPipeline pipeline;
    if (!pipeline.initialize(senderConfig)) {
        return shutdownRequested.load(std::memory_order_acquire) ? 0 : -1;
    }

    return pipeline.run();
}

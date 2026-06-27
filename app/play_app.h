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
 * Receiver application runner declarations. These runners start receiver test mode or receiver-to-DeckLink SDI playout.
 */

#pragma once

#include <atomic>
#include <string>

#include "cli/receiver_cli.h"

int runPlayTest(const std::string& inputUrl,
                const ReceiverCliOptions& options,
                std::atomic<bool>& shutdownRequested);

int runPlayDeckLink(const std::string& inputUrl,
                    int deviceIndex,
                    const ReceiverCliOptions& options,
                    std::atomic<bool>& shutdownRequested);

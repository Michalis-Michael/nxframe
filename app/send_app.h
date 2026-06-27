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
 * Sender application runner declaration. The sender runner validates transport and preset input before handing control to SenderPipeline.
 */

#pragma once

#include <atomic>
#include <string>

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
               const std::string& cpuProfileName,
               const std::string& cpuProfileConfigPath,
               std::atomic<bool>& shutdownRequested);

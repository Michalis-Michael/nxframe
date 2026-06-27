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
 * Application entry point declaration. The implementation parses global CLI options and dispatches the process into sender or receiver runtime modes.
 */

#pragma once

#include <atomic>

int runNxFrameApp(int argc, char* argv[], std::atomic<bool>& shutdownRequested);

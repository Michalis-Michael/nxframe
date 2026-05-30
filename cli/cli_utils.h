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
 * Command-line utility declarations shared by the sender and receiver application entry points.
 */

#pragma once

#include <string>

void printUsage();

bool fileExists(const std::string& path);
std::string trimCopy(const std::string& value);

bool parseIntStrict(const std::string& text,
                    int minValue,
                    int maxValue,
                    int& out,
                    std::string* error);

std::string getCwd();
std::string getExeDir();
std::string joinPath(const std::string& a, const std::string& b);
std::string resolvePresetPath(const std::string& presetArg);

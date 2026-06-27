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
 * Application entry dispatcher. This file owns top-level command parsing and routes validated commands to the sender or receiver app runners.
 */

#include "app/nxframe_app.h"

#include "app/play_app.h"
#include "app/send_app.h"
#include "cli/cli_utils.h"
#include "cli/receiver_cli.h"
#include "config/preset_validator.h"
#include "stage_timing.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

struct GlobalCliOptions {
    bool forceCopy = false;
    bool allowTestFallback = false;
    bool timingEnabled = false;
    bool timingVerbose = false;
    bool tsDebug = false;
    std::string tsCapturePath;
    std::string cpuProfileName;
    std::string cpuProfileConfigPath;
    ReceiverCliOptions receiverOptions;
    std::vector<std::string> positionalArgs;
};

// Parse options that affect the full process before command dispatch. Arguments
// that are part of the subcommand grammar are preserved in positionalArgs so the
// sender/play parsers see the same shape as the user entered.
bool parseGlobalOptions(int argc, char* argv[], GlobalCliOptions& out)
{
    out.positionalArgs.reserve(static_cast<size_t>(argc));
    out.positionalArgs.push_back(argc > 0 ? argv[0] : "NxFrame");

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--copy") {
            out.forceCopy = true;
        } else if (arg == "--allow-test-fallback") {
            out.allowTestFallback = true;
        } else if (arg == "--timing") {
            out.timingEnabled = true;
        } else if (arg == "--timing-verbose") {
            out.timingEnabled = true;
            out.timingVerbose = true;
        } else if (arg == "--ts-debug") {
            out.tsDebug = true;
        } else if (arg == "--ts-capture") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: --ts-capture requires a file path.\n";
                return false;
            }
            out.tsCapturePath = argv[++i];
        } else if (arg == "--cpu-profile" || arg == "-cpu_profile" || arg == "--cpu_profile" || arg == "-cpu-profile") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: " << arg << " requires a profile name.\n";
                return false;
            }
            out.cpuProfileName = argv[++i];
        } else if (arg.compare(0, 14, "--cpu-profile=") == 0) {
            out.cpuProfileName = arg.substr(14);
            if (out.cpuProfileName.empty()) {
                std::cerr << "[Main] Error: --cpu-profile requires a profile name.\n";
                return false;
            }
        } else if (arg.compare(0, 13, "-cpu_profile=") == 0) {
            out.cpuProfileName = arg.substr(13);
            if (out.cpuProfileName.empty()) {
                std::cerr << "[Main] Error: -cpu_profile requires a profile name.\n";
                return false;
            }
        } else if (arg == "--cpu-profile-config" || arg == "--cpu_profile_config") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: " << arg << " requires a JSON file path.\n";
                return false;
            }
            out.cpuProfileConfigPath = argv[++i];
        } else if (arg.compare(0, 21, "--cpu-profile-config=") == 0) {
            out.cpuProfileConfigPath = arg.substr(21);
            if (out.cpuProfileConfigPath.empty()) {
                std::cerr << "[Main] Error: --cpu-profile-config requires a JSON file path.\n";
                return false;
            }
        } else if (arg.compare(0, 21, "--cpu_profile_config=") == 0) {
            out.cpuProfileConfigPath = arg.substr(21);
            if (out.cpuProfileConfigPath.empty()) {
                std::cerr << "[Main] Error: --cpu_profile_config requires a JSON file path.\n";
                return false;
            }
        } else if (arg == "--receiver-preset") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: --receiver-preset requires a preset name or path.\n";
                return false;
            }
            out.receiverOptions.presetPath = argv[++i];
        } else if (arg == "--packed-audio-channels") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: --packed-audio-channels requires a value.\n";
                return false;
            }
            std::string err;
            if (!parseIntStrict(argv[++i], 2, 64, out.receiverOptions.packedAudioChannels, &err)) {
                std::cerr << "[Main] Error: invalid --packed-audio-channels: " << err << "\n";
                return false;
            }
        } else if (arg == "--max-audio-pairs") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: --max-audio-pairs requires a value.\n";
                return false;
            }
            std::string err;
            if (!parseIntStrict(argv[++i], 1, 32, out.receiverOptions.maxAudioPairs, &err)) {
                std::cerr << "[Main] Error: invalid --max-audio-pairs: " << err << "\n";
                return false;
            }
        } else if (arg == "--audio-route") {
            if (i + 1 >= argc) {
                std::cerr << "[Main] Error: --audio-route requires a CSV value.\n";
                return false;
            }
            std::string err;
            if (!parseRouteCsv(argv[++i], out.receiverOptions.audioRoute, &err)) {
                std::cerr << "[Main] Error: " << err << "\n";
                return false;
            }
        } else {
            out.positionalArgs.push_back(arg);
        }
    }

    if (!out.tsCapturePath.empty() && !out.tsDebug) {
        out.tsDebug = true;
    }

    return true;
}

bool loadReceiverCliPresetIfNeeded(ReceiverCliOptions& receiverOptions)
{
    if (receiverOptions.presetPath.empty()) {
        return true;
    }

    const std::string resolvedReceiverPreset = resolvePresetPath(receiverOptions.presetPath);
    const std::string receiverPresetFile = resolvedReceiverPreset.empty()
        ? receiverOptions.presetPath
        : resolvedReceiverPreset;

    const PresetValidator::Result presetValidation =
        PresetValidator::validateFile(receiverPresetFile, PresetValidator::Kind::Receiver);
    PresetValidator::printResult(presetValidation, receiverPresetFile, PresetValidator::Kind::Receiver);
    if (!presetValidation.ok()) {
        return false;
    }

    std::string err;
    if (!loadReceiverPreset(receiverPresetFile, receiverOptions, &err)) {
        std::cerr << "[Main] Error: Failed to load receiver preset '" << receiverOptions.presetPath
                  << "': " << err << "\n";
        return false;
    }

    std::cout << "[Main] Using receiver preset file: " << receiverPresetFile << "\n";
    return true;
}

// Receiver command grammar is intentionally kept at the app boundary. The
// receiver module receives normalized configuration rather than raw argv values.
int dispatchPlayCommand(const std::vector<std::string>& args,
                        const ReceiverCliOptions& receiverOptions,
                        std::atomic<bool>& shutdownRequested)
{
    if (args.size() >= 6 &&
        args[1] == "play" &&
        args[3] == "to" &&
        args[4] == "decklink")
    {
        if (args.size() != 6) {
            printUsage();
            return -1;
        }
        int deviceIndex = 0;
        std::string err;
        if (!parseIntStrict(args[5], 0, 128, deviceIndex, &err)) {
            std::cerr << "[Main] Error: invalid DeckLink device index: " << err << "\n";
            return -1;
        }
        return runPlayDeckLink(args[2], deviceIndex, receiverOptions, shutdownRequested);
    }

    if (args.size() >= 5 &&
        args[1] == "play" &&
        args[3] == "to" &&
        args[4] == "test")
    {
        if (args.size() != 5) {
            printUsage();
            return -1;
        }
        return runPlayTest(args[2], receiverOptions, shutdownRequested);
    }

    printUsage();
    return -1;
}

// Sender command parsing resolves the input type, destination, and preset name;
// all media ownership and worker-thread setup is delegated to SenderPipeline.
int dispatchSendCommand(const std::vector<std::string>& args,
                        const GlobalCliOptions& opt,
                        std::atomic<bool>& shutdownRequested)
{
    if (args.size() < 8) {
        printUsage();
        return -1;
    }

    std::string inputType = args[2];
    std::string cardInput;
    std::string transportUrl;
    std::string presetName;

    if (inputType == "decklink") {
        if (args.size() != 9) {
            printUsage();
            return -1;
        }
        cardInput = args[3];
        transportUrl = args[5];
        presetName = args[8];
    } else if (inputType == "test") {
        if (args.size() != 8) {
            printUsage();
            return -1;
        }
        transportUrl = args[4];
        presetName = args[7];
    } else {
        std::cerr << "[Main] Error: Unsupported input type.\n";
        printUsage();
        return -1;
    }

    return runSendApp(inputType,
                      cardInput,
                      transportUrl,
                      presetName,
                      opt.forceCopy,
                      opt.allowTestFallback,
                      opt.timingEnabled,
                      opt.timingVerbose,
                      opt.tsDebug,
                      opt.tsCapturePath,
                      opt.cpuProfileName,
                      opt.cpuProfileConfigPath,
                      shutdownRequested);
}

} // namespace

int runNxFrameApp(int argc, char* argv[], std::atomic<bool>& shutdownRequested)
{
    GlobalCliOptions options;
    if (!parseGlobalOptions(argc, argv, options)) {
        return -1;
    }

    stage_timing::set_enabled(options.timingEnabled, options.timingVerbose);

    // Receiver presets can alter live audio routing and packing. Load them
    // before dispatch so both test playout and DeckLink playout use identical
    // receiver configuration semantics.
    if (!loadReceiverCliPresetIfNeeded(options.receiverOptions)) {
        return -1;
    }

    const std::vector<std::string>& args = options.positionalArgs;
    if (args.size() < 2) {
        printUsage();
        return -1;
    }

    if (args[1] == "play") {
        return dispatchPlayCommand(args, options.receiverOptions, shutdownRequested);
    }

    if (args[1] == "send") {
        return dispatchSendCommand(args, options, shutdownRequested);
    }

    std::cerr << "[Main] Error: First argument must be 'send' or 'play'.\n";
    printUsage();
    return -1;
}

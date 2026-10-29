// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Entry point of the wasm build, replacing main.cpp (whose CLI parsing, IPC, debugger attach and
// Big Picture launcher have no browser equivalent). Usage, as argv from the page or from Node:
//
//   shadps4 <path to eboot.bin or game folder> [guest args...]
//
// The path is inside Emscripten's filesystem: under Node (tests) that is the host filesystem
// (NODERAWFS); in a browser the page mounts the game folder the user picked before calling main.

#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#include <fmt/ranges.h>
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "emulator.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: shadps4 <eboot.bin or game folder> [guest args...]\n";
        return 1;
    }
    Common::Log::Setup("shadps4.log");
    LOG_INFO(Debug, "Run: {}", fmt::join(std::span(argv, argc), " "));

    KeyManager::GetInstance()->LoadFromFile();
    EmulatorSettings.Load();
    UserSettings.Load();

    std::filesystem::path game(argv[1]);
    std::vector<std::string> game_args(argv + 2, argv + argc);
    if (!std::filesystem::exists(game)) {
        LOG_ERROR(Debug, "Game path not found: {}", game.string());
        return 1;
    }
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->Run(game, game_args);
    return 0;
}

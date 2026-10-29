// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wasm stand-in for sdl_window.cpp. The native window owns an SDL window with a Vulkan surface
// and pumps SDL events into the pad/keyboard/mouse libraries. In the browser the "window" is a
// <canvas> on the page: frames go out through web/web_display.h, and input comes in from the
// page through the exported shadps4_pad_* functions below (the page maps keyboard and the
// Gamepad API onto PS4 pad buttons/axes).

#include <atomic>
#include <chrono>
#include <thread>
#include <emscripten.h>
#include "common/logging/log.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Frontend {

namespace {
std::atomic<Input::GameControllers*> g_controllers{nullptr};
std::atomic<bool> g_quit{false};
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameControllers* controllers_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controllers{*controllers_} {
    window_info.type = WindowSystemType::Headless;
    g_controllers = controllers_;
    LOG_INFO(Frontend, "web window: {} ({}x{})", window_title, width, height);
    const std::string title{window_title};
    MAIN_THREAD_EM_ASM(
        {
            if (Module["shadps4OnTitle"]) {
                Module["shadps4OnTitle"](UTF8ToString($0));
            }
        },
        title.c_str());
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::SetIcon(std::span<const u8>) {}

void WindowSDL::WaitEvent() {
    // Called in a loop on the emulator's main thread for the life of the process. Input arrives
    // asynchronously via the exports below, so this only has to notice a quit request.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    if (g_quit) {
        is_open = false;
    }
}

void WindowSDL::InitTimers() {}
void WindowSDL::RequestKeyboard() {}
void WindowSDL::ReleaseKeyboard() {}
void WindowSDL::OnResize() {}
void WindowSDL::OnKeyboardMouseInput(const SDL_Event*) {}
void WindowSDL::OnGamepadEvent(const SDL_Event*) {}

void SetWindowIcon(SDL_Window*, const std::vector<u8>&) {}
void SetDefaultWindowIcon(SDL_Window*) {}

} // namespace Frontend

extern "C" {

// `button` is an OrbisPadButtonDataOffset bit (e.g. 0x4000 = Cross); see the page's key map.
EMSCRIPTEN_KEEPALIVE void shadps4_pad_button(int pad, unsigned button, int pressed) {
    if (auto* c = Frontend::g_controllers.load(); c && pad >= 0 && pad < 4) {
        (*c)[pad]->Button(static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(button), pressed);
    }
}

// `axis` is an Input::Axis value; `value` is 0..255 like the native SDL mapping produces.
EMSCRIPTEN_KEEPALIVE void shadps4_pad_axis(int pad, int axis, int value) {
    if (auto* c = Frontend::g_controllers.load(); c && pad >= 0 && pad < 4) {
        (*c)[pad]->Axis(static_cast<Input::Axis>(axis), value);
    }
}

EMSCRIPTEN_KEEPALIVE void shadps4_request_quit() {
    Frontend::g_quit = true;
}

} // extern "C"

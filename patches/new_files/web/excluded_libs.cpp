// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Definitions for the parts of shadPS4 the wasm build leaves out (emscripten/CMakeLists.txt,
// EXCLUDE_REGEX), so the code that refers to them still links:
//
//  - HLE libraries whose implementations need host facilities a browser doesn't offer:
//    networking/PSN (host sockets + epoll/kqueue, OpenSSL, protobuf, cpp-httplib), video/audio
//    decoding (FFmpeg), USB. Their RegisterLib does nothing, so every NID they would have
//    provided resolves to the generic unimplemented-import stub (returns 0, logged) - the same
//    thing that happens natively for the many functions shadPS4 hasn't written yet.
//  - The ImGui overlay renderer (Vulkan), the Vulkan pipeline cache and RenderDoc.
//
// Nothing here pretends to work: each is the "not available" answer.

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <pthread.h>
#include "core/loader/symbols_resolver.h"
#include "core/net/guest_net.h"
#include "imgui/imgui_layer.h"
#include "imgui/imgui_texture.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/cache_storage.h"
#include "video_core/renderdoc.h"

using Sym = Core::Loader::SymbolsResolver;

#define EXCLUDED_LIB(ns)                                                                           \
    namespace ns {                                                                                 \
    void RegisterLib(Sym*) {}                                                                      \
    }

// Media decoding (FFmpeg) and OpenAL.
EXCLUDED_LIB(Libraries::Ajm)
EXCLUDED_LIB(Libraries::AvPlayer)
EXCLUDED_LIB(Libraries::Videodec)
EXCLUDED_LIB(Libraries::Videodec2)
EXCLUDED_LIB(Libraries::Vdecsw)
EXCLUDED_LIB(Libraries::VdecCore)
EXCLUDED_LIB(Libraries::VideoCoreServer)
EXCLUDED_LIB(Libraries::Audio3dOpenAL)
EXCLUDED_LIB(Libraries::MusicPlayerService)
// Networking and PSN.
EXCLUDED_LIB(Libraries::Net)
EXCLUDED_LIB(Libraries::NetCtl)
EXCLUDED_LIB(Libraries::Http)
EXCLUDED_LIB(Libraries::Http2)
EXCLUDED_LIB(Libraries::Ssl2)
EXCLUDED_LIB(Libraries::Rudp)
EXCLUDED_LIB(Libraries::InvitationDialog)
EXCLUDED_LIB(Libraries::Np::NpAuth)
EXCLUDED_LIB(Libraries::Np::NpCommerce)
EXCLUDED_LIB(Libraries::Np::NpCommon)
EXCLUDED_LIB(Libraries::Np::NpManager)
EXCLUDED_LIB(Libraries::Np::NpMatching2)
EXCLUDED_LIB(Libraries::Np::NpPartner)
EXCLUDED_LIB(Libraries::Np::NpParty)
EXCLUDED_LIB(Libraries::Np::NpProfileDialog)
EXCLUDED_LIB(Libraries::Np::NpScore)
EXCLUDED_LIB(Libraries::Np::NpSignaling)
EXCLUDED_LIB(Libraries::Np::NpSnsFacebookDialog)
EXCLUDED_LIB(Libraries::Np::NpTrophy)
EXCLUDED_LIB(Libraries::Np::NpTus)
EXCLUDED_LIB(Libraries::Np::NpUtility)
EXCLUDED_LIB(Libraries::Np::NpWebApi)
EXCLUDED_LIB(Libraries::Np::NpWebApi2)

namespace Libraries::MusicPlayerService {
bool IsBgmPlaying() {
    return false;
}
} // namespace Libraries::MusicPlayerService

// Sockets can't be created, so the equeue's socket-readiness probes have nothing to report.
namespace Core::Net {
NetResult SocketBytesReadable(s32) {
    return NetResult::Fail(Host::Error::NotSock);
}
NetResult SocketSelect(std::span<SelectEntry>, s64) {
    return NetResult::Fail(Host::Error::NotSock);
}
} // namespace Core::Net

// ImGui overlay renderer (imgui/renderer/*, Vulkan). Layers are never drawn.
namespace ImGui {
void Layer::AddLayer(Layer*) {}
void Layer::RemoveLayer(Layer*) {}

namespace Core {
void AcquireGamepadInputCapture() {}
void ReleaseGamepadInputCapture() {}
bool IsGamepadInputCaptured() {
    return false;
}
bool MustKeepDrawing() {
    return false;
}
} // namespace Core

RefCountedTexture::RefCountedTexture(Core::TextureManager::Inner* inner_) : inner{inner_} {}
RefCountedTexture::RefCountedTexture() : inner{nullptr} {}
RefCountedTexture::RefCountedTexture(const RefCountedTexture&) : inner{nullptr} {}
RefCountedTexture::RefCountedTexture(RefCountedTexture&&) noexcept : inner{nullptr} {}
RefCountedTexture& RefCountedTexture::operator=(const RefCountedTexture&) {
    return *this;
}
RefCountedTexture& RefCountedTexture::operator=(RefCountedTexture&&) noexcept {
    return *this;
}
RefCountedTexture::~RefCountedTexture() = default;
RefCountedTexture RefCountedTexture::DecodePngTexture(std::vector<u8>) {
    return {};
}
RefCountedTexture RefCountedTexture::DecodePngFile(std::filesystem::path) {
    return {};
}
RefCountedTexture::Image RefCountedTexture::GetTexture() const {
    return {};
}
RefCountedTexture::operator bool() const {
    return false;
}
} // namespace ImGui

namespace shadNotifications {
void QueueNotification(std::string, float, position, stockIcons) {}
void QueueNotification(std::string, float, position, std::vector<u8>) {}
} // namespace shadNotifications

// Embedded fallback fonts (generated from imgui/renderer/fonts at build time natively). Left
// empty here to keep the download small; libSceFont then relies on the game's own fonts.
#define EMPTY_FONT(name)                                                                           \
    extern const unsigned int name##_compressed_size = 0;                                          \
    extern const unsigned char name##_compressed_data[] = {0};
EMPTY_FONT(imgui_font_notosans_regular)
EMPTY_FONT(imgui_font_notosansarabic_regular)
EMPTY_FONT(imgui_font_notosansthai_regular)
EMPTY_FONT(imgui_font_notosanssymbols2_regular)
EMPTY_FONT(imgui_font_notosanscjk_regular)
EMPTY_FONT(imgui_font_proggyvector_regular)

// Vulkan pipeline cache storage.
namespace Storage {
void DataBase::Open() {}
void DataBase::Close() {}
} // namespace Storage

// RenderDoc capture hooks.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
} // namespace VideoCore

// Emscripten's pthreads have no thread names; shadPS4 only uses them for logging and debugging.
extern "C" {
static thread_local char tls_thread_name[64];
int pthread_setname_np(pthread_t thread, const char* name) {
    if (pthread_equal(thread, pthread_self())) {
        std::strncpy(tls_thread_name, name, sizeof(tls_thread_name) - 1);
    }
    return 0;
}
int pthread_getname_np(pthread_t thread, char* name, size_t len) {
    if (len == 0) {
        return ERANGE;
    }
    std::strncpy(name, pthread_equal(thread, pthread_self()) ? tls_thread_name : "", len - 1);
    name[len - 1] = '\0';
    return 0;
}
}

// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// libSceNet and libSceNetCtl for the browser: a PS4 with its network cable pulled out.
//
// Upstream's implementations (core/libraries/net, core/libraries/network/netctl.cpp, on top of
// core/net) need host sockets, the host's routing tables and OpenSSL, none of which a page has,
// so the wasm build leaves them out (emscripten/CMakeLists.txt). Leaving the libraries out
// entirely isn't enough, though: games set up sockets at boot whether or not they'll ever go
// online, and the generic "unimplemented, returns 0" stub hands them socket 0 and a null errno
// pointer, which they then dereference. Here the parts that don't need a network work for real
// (errno, byte order, address text, epoll objects that time out) and everything that would touch
// the network fails the way it does on a console with no connection: ENETDOWN, and NetCtl
// reporting "disconnected".

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <arpa/inet.h>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/net/net_error.h"
#include "core/libraries/network/net_ctl_codes.h"
#include "core/loader/symbols_resolver.h"

namespace Libraries::Net {

namespace {

constexpr s32 OrbisAfInet = 2;
constexpr s32 OrbisAfInet6 = 28;

thread_local s32 net_errno = 0;

s32 Fail(s32 orbis_errno) {
    net_errno = orbis_errno;
    return ORBIS_NET_ERROR_BASE | orbis_errno;
}

// Epoll objects and resolvers exist (games create them up front); with no sockets to watch, an
// epoll wait only ends by timing out or being aborted.
struct Waitable {
    std::mutex mutex;
    std::condition_variable cv;
    bool aborted = false;
};
std::mutex objects_mutex;
std::unordered_map<s32, std::shared_ptr<Waitable>> objects;
std::atomic<s32> next_id{1};

s32 NewObject() {
    const s32 id = next_id++;
    std::scoped_lock lk{objects_mutex};
    objects.emplace(id, std::make_shared<Waitable>());
    return id;
}

std::shared_ptr<Waitable> Find(s32 id) {
    std::scoped_lock lk{objects_mutex};
    const auto it = objects.find(id);
    return it == objects.end() ? nullptr : it->second;
}

void LogOnce(const char* what) {
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> seen;
    std::scoped_lock lk{mutex};
    if (!seen[what]) {
        seen[what] = true;
        LOG_INFO(Lib_Net, "{}: no network in the browser build (ENETDOWN)", what);
    }
}

s32 NetDown(const char* what) {
    LogOnce(what);
    return Fail(ORBIS_NET_ENETDOWN);
}

} // namespace

s32 PS4_SYSV_ABI sceNetInit() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetTerm() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetPoolCreate(const char*, s32 size, s32) {
    if (size < 0) {
        return Fail(ORBIS_NET_EINVAL);
    }
    return next_id++;
}

s32 PS4_SYSV_ABI sceNetPoolDestroy(s32) {
    return ORBIS_OK;
}

s32* PS4_SYSV_ABI sceNetErrnoLoc() {
    return &net_errno;
}

u16 PS4_SYSV_ABI sceNetHtons(u16 v) {
    return std::byteswap(v);
}

u32 PS4_SYSV_ABI sceNetHtonl(u32 v) {
    return std::byteswap(v);
}

u64 PS4_SYSV_ABI sceNetHtonll(u64 v) {
    return std::byteswap(v);
}

const char* PS4_SYSV_ABI sceNetInetNtop(s32 af, const void* src, char* dst, u32 size) {
    if (src == nullptr || dst == nullptr) {
        Fail(ORBIS_NET_EFAULT);
        return nullptr;
    }
    const int host_af = af == OrbisAfInet ? AF_INET : af == OrbisAfInet6 ? AF_INET6 : -1;
    if (host_af < 0) {
        Fail(ORBIS_NET_EAFNOSUPPORT);
        return nullptr;
    }
    if (inet_ntop(host_af, src, dst, size) == nullptr) {
        Fail(ORBIS_NET_ENOSPC);
        return nullptr;
    }
    return dst;
}

s32 PS4_SYSV_ABI sceNetInetPton(s32 af, const char* src, void* dst) {
    if (src == nullptr || dst == nullptr) {
        return Fail(ORBIS_NET_EFAULT);
    }
    const int host_af = af == OrbisAfInet ? AF_INET : af == OrbisAfInet6 ? AF_INET6 : -1;
    if (host_af < 0) {
        return Fail(ORBIS_NET_EAFNOSUPPORT);
    }
    return inet_pton(host_af, src, dst) == 1 ? 1 : 0;
}

s32 PS4_SYSV_ABI sceNetSocket(const char*, s32, s32, s32) {
    return NetDown("sceNetSocket");
}

// Calls on a socket that can't exist.
s32 PS4_SYSV_ABI sceNetBadSocket() {
    return Fail(ORBIS_NET_EBADF);
}

s32 PS4_SYSV_ABI sceNetEpollCreate(const char*, s32) {
    return NewObject();
}

s32 PS4_SYSV_ABI sceNetEpollControl(s32 eid, s32, s32, void*) {
    if (!Find(eid)) {
        return Fail(ORBIS_NET_EBADF);
    }
    return Fail(ORBIS_NET_EBADF); // no socket id is valid
}

s32 PS4_SYSV_ABI sceNetEpollWait(s32 eid, void*, s32 maxevents, s32 timeout_us) {
    const auto object = Find(eid);
    if (!object) {
        return Fail(ORBIS_NET_EBADF);
    }
    if (maxevents <= 0) {
        return Fail(ORBIS_NET_EINVAL);
    }
    std::unique_lock lk{object->mutex};
    const auto aborted = [&] { return object->aborted; };
    if (timeout_us < 0) {
        object->cv.wait(lk, aborted);
    } else {
        object->cv.wait_for(lk, std::chrono::microseconds(timeout_us), aborted);
    }
    if (std::exchange(object->aborted, false)) {
        return Fail(ORBIS_NET_EINTR);
    }
    return 0;
}

s32 PS4_SYSV_ABI sceNetEpollAbort(s32 eid, s32) {
    const auto object = Find(eid);
    if (!object) {
        return Fail(ORBIS_NET_EBADF);
    }
    {
        std::scoped_lock lk{object->mutex};
        object->aborted = true;
    }
    object->cv.notify_all();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetEpollDestroy(s32 eid) {
    sceNetEpollAbort(eid, 0);
    std::scoped_lock lk{objects_mutex};
    return objects.erase(eid) ? ORBIS_OK : Fail(ORBIS_NET_EBADF);
}

s32 PS4_SYSV_ABI sceNetResolverCreate(const char*, s32, s32) {
    return NewObject();
}

s32 PS4_SYSV_ABI sceNetResolverStart(s32 rid, const char*, void*, s32, s32, s32) {
    if (!Find(rid)) {
        return Fail(ORBIS_NET_EBADF);
    }
    LogOnce("sceNetResolverStart");
    net_errno = ORBIS_NET_RESOLVER_ENODNS;
    return ORBIS_NET_ERROR_RESOLVER_ENODNS;
}

s32 PS4_SYSV_ABI sceNetResolverGetError(s32 rid, s32* result) {
    if (!Find(rid)) {
        return Fail(ORBIS_NET_EBADF);
    }
    if (result != nullptr) {
        *result = ORBIS_NET_RESOLVER_ENODNS;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetResolverDestroy(s32 rid) {
    std::scoped_lock lk{objects_mutex};
    return objects.erase(rid) ? ORBIS_OK : Fail(ORBIS_NET_EBADF);
}

s32 PS4_SYSV_ABI sceNetResolverAbort(s32 rid, s32) {
    return Find(rid) ? ORBIS_OK : Fail(ORBIS_NET_EBADF);
}

s32 PS4_SYSV_ABI sceNetGetMacAddress(u8* addr, s32) {
    if (addr == nullptr) {
        return Fail(ORBIS_NET_EFAULT);
    }
    // A locally administered address, as consoles without a real NIC report something.
    static constexpr u8 mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    std::memcpy(addr, mac, sizeof(mac));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetNoNetwork() {
    return NetDown("network query");
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Nlev7Lg8k3A", "libSceNet", 1, "libSceNet", sceNetInit);
    LIB_FUNCTION("cTGkc6-TBlI", "libSceNet", 1, "libSceNet", sceNetTerm);
    LIB_FUNCTION("dgJBaeJnGpo", "libSceNet", 1, "libSceNet", sceNetPoolCreate);
    LIB_FUNCTION("K7RlrTkI-mw", "libSceNet", 1, "libSceNet", sceNetPoolDestroy);
    LIB_FUNCTION("HQOwnfMGipQ", "libSceNet", 1, "libSceNet", sceNetErrnoLoc);
    LIB_FUNCTION("iWQWrwiSt8A", "libSceNet", 1, "libSceNet", sceNetHtons);
    LIB_FUNCTION("Rbvt+5Y2iEw", "libSceNet", 1, "libSceNet", sceNetHtons); // ntohs
    LIB_FUNCTION("9T2pDF2Ryqg", "libSceNet", 1, "libSceNet", sceNetHtonl);
    LIB_FUNCTION("pQGpHYopAIY", "libSceNet", 1, "libSceNet", sceNetHtonl); // ntohl
    LIB_FUNCTION("3CHi1K1wsCQ", "libSceNet", 1, "libSceNet", sceNetHtonll);
    LIB_FUNCTION("tOrRi-v3AOM", "libSceNet", 1, "libSceNet", sceNetHtonll); // ntohll
    LIB_FUNCTION("9vA2aW+CHuA", "libSceNet", 1, "libSceNet", sceNetInetNtop);
    LIB_FUNCTION("8Kcp5d-q1Uo", "libSceNet", 1, "libSceNet", sceNetInetPton);
    LIB_FUNCTION("Q4qBuN-c0ZM", "libSceNet", 1, "libSceNet", sceNetSocket);
    for (const char* nid : {"45ggEzakPJQ" /*SocketClose*/, "zJGf8xjFnQE" /*SocketAbort*/,
                            "bErx49PgxyY" /*Bind*/, "kOj1HiAGE54" /*Listen*/,
                            "OXXX4mUk3uk" /*Connect*/, "PIWqhn9oSxc" /*Accept*/,
                            "beRjXBn-z+o" /*Send*/, "gvD1greCu0A" /*Sendto*/,
                            "2eKbgcboJso" /*Sendmsg*/, "9wO9XrMsNhc" /*Recv*/,
                            "304ooNZxWDY" /*Recvfrom*/, "wvuUDv0jrMI" /*Recvmsg*/,
                            "2mKX2Spso7I" /*Setsockopt*/, "xphrZusl78E" /*Getsockopt*/,
                            "TSM6whtekok" /*Shutdown*/, "hoOAofhhRvE" /*Getsockname*/,
                            "TCkRD0DWNLg" /*Getpeername*/, "hLuXdjHnhiI" /*GetSockInfo*/,
                            "ghqRRVQxqKo" /*Ioctl*/}) {
        LIB_FUNCTION(nid, "libSceNet", 1, "libSceNet", sceNetBadSocket);
    }
    LIB_FUNCTION("SF47kB2MNTo", "libSceNet", 1, "libSceNet", sceNetEpollCreate);
    LIB_FUNCTION("ZVw46bsasAk", "libSceNet", 1, "libSceNet", sceNetEpollControl);
    LIB_FUNCTION("drjIbDbA7UQ", "libSceNet", 1, "libSceNet", sceNetEpollWait);
    LIB_FUNCTION("w21YgGGNtBk", "libSceNet", 1, "libSceNet", sceNetEpollAbort);
    LIB_FUNCTION("Inp1lfL+Jdw", "libSceNet", 1, "libSceNet", sceNetEpollDestroy);
    LIB_FUNCTION("C4UgDHHPvdw", "libSceNet", 1, "libSceNet", sceNetResolverCreate);
    LIB_FUNCTION("Nd91WaWmG2w", "libSceNet", 1, "libSceNet", sceNetResolverStart); // Ntoa
    LIB_FUNCTION("Apb4YDxKsRI", "libSceNet", 1, "libSceNet", sceNetResolverStart); // Aton
    LIB_FUNCTION("J5i3hiLJMPk", "libSceNet", 1, "libSceNet", sceNetResolverGetError);
    LIB_FUNCTION("kJlYH5uMAWI", "libSceNet", 1, "libSceNet", sceNetResolverDestroy);
    LIB_FUNCTION("AzqoBha7js4", "libSceNet", 1, "libSceNet", sceNetResolverAbort);
    LIB_FUNCTION("6Oc0bLsIYe0", "libSceNet", 1, "libSceNet", sceNetGetMacAddress);
    for (const char* nid : {"nCL0NyZsd5A" /*GetDnsInfo*/, "6Nx1hIQL9h8" /*GetRouteInfo*/,
                            "+S-2-jlpaBo" /*GetNameToIndex*/}) {
        LIB_FUNCTION(nid, "libSceNet", 1, "libSceNet", sceNetNoNetwork);
    }
}

} // namespace Libraries::Net

namespace Libraries::NetCtl {

s32 PS4_SYSV_ABI sceNetCtlInit() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetCtlTerm() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetCtlGetState(s32* state) {
    if (state == nullptr) {
        return ORBIS_NET_CTL_ERROR_INVALID_ADDR;
    }
    *state = ORBIS_NET_CTL_STATE_DISCONNECTED;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetCtlGetInfo(s32, void*) {
    return ORBIS_NET_CTL_ERROR_NOT_CONNECTED;
}

// Callbacks would only ever report a state change; there is none to report.
s32 PS4_SYSV_ABI sceNetCtlRegisterCallback(void*, void*, s32* cid) {
    if (cid != nullptr) {
        *cid = 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNetCtlNoOp() {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("gky0+oaNM4k", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlInit);
    LIB_FUNCTION("Z4wwCFiBELQ", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlTerm);
    LIB_FUNCTION("uBPlr0lbuiI", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlGetState);
    LIB_FUNCTION("obuxdTiwkF8", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlGetInfo);
    LIB_FUNCTION("JO4yuTuMoKI", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlGetInfo); // NatInfo
    LIB_FUNCTION("teuK4QnJTGg", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlGetInfo); // IfStat
    LIB_FUNCTION("UJ+Z7Q+4ck0", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlRegisterCallback);
    LIB_FUNCTION("Rqm2OnZMCz0", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlNoOp);
    LIB_FUNCTION("iQw3iQPhvUQ", "libSceNetCtl", 1, "libSceNetCtl", sceNetCtlNoOp);
}

} // namespace Libraries::NetCtl

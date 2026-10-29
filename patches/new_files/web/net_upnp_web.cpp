// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// UPnP port forwarding (core/libraries/net/net_upnp.cpp, built on miniupnpc) needs SSDP multicast
// discovery and raw TCP to the router - neither exists in a browser. This reports "no gateway",
// which is the same state the native client is in on a network without UPnP.

#include "core/libraries/net/net_upnp.h"

namespace Libraries::Net {

UPnPClient& UPnPClient::Instance() {
    static UPnPClient instance;
    return instance;
}

UPnPClient::~UPnPClient() = default;

void UPnPClient::Start() {
    m_started = true;
    m_done = true;
}

void UPnPClient::SetP2PFeaturesEnabled(bool enabled) {
    m_p2p_features_enabled = enabled;
}

bool UPnPClient::WaitReady(int) {
    return false;
}

bool UPnPClient::IsAvailable() const {
    return false;
}

u32 UPnPClient::GetExternalIp() const {
    return 0;
}

std::string UPnPClient::GetExternalIpString() const {
    return {};
}

u16 UPnPClient::GetExternalPort() const {
    return 0;
}

void UPnPClient::DiscoverThread() {}
void UPnPClient::AddMapping(u16) {}
void UPnPClient::RemoveMapping(u16) {}
void UPnPClient::MapPortAsync(u16) {}
void UPnPClient::UnmapPort(u16) {}

} // namespace Libraries::Net

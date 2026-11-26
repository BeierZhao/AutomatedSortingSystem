// Loads NetConfig.h if it exists, otherwise the committed example.
//
// The Wi-Fi name and password and the gateway address in that file are only
// defaults: values stored on the board by the serial HMI panel
// (src/common/Settings.h) take precedence.
#pragma once

#if __has_include("NetConfig.h")
#include "NetConfig.h"
#else
#warning "include/NetConfig.h not found, using NetConfig.example.h (placeholder Wi-Fi credentials)"
#include "NetConfig.example.h"
#endif

#include "ControllerApi.h"
#include "Settings.h"

// Call after settings::load().
inline NetworkConfig appNetworkConfig() {
    NetworkConfig net;
    net.ssid = settings::active().wifiSsid.c_str();
    net.password = settings::active().wifiPassword.c_str();
    net.useStaticIp = NET_USE_STATIC_IP;
    net.ip = IPAddress(NET_STATIC_IP);
    net.gateway = IPAddress(NET_GATEWAY_IP);
    net.subnet = IPAddress(NET_SUBNET_MASK);
    return net;
}

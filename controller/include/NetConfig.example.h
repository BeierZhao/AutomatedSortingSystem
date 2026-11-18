// Network settings. Copy this file to NetConfig.h (git-ignored) and fill in
// your own values. Without NetConfig.h the build uses this example as is.
#pragma once

#define NET_WIFI_SSID "your-ssid"
#define NET_WIFI_PASSWORD "your-password"

// Fixed address for the ESP32. Set NET_USE_STATIC_IP to 0 to use DHCP
// (then give the board a DHCP reservation on the router).
#define NET_USE_STATIC_IP 0
#define NET_STATIC_IP 192, 168, 1, 50
#define NET_GATEWAY_IP 192, 168, 1, 1
#define NET_SUBNET_MASK 255, 255, 255, 0

// Port of the /controller/... HTTP server on the ESP32.
#define NET_CONTROLLER_HTTP_PORT 80

// Receiver of the status (2.1) and alarm (2.2) reports.
// Ignition WebDev: port 8088, paths under /system/webdev/<project>/.
// Python gateway (ignition_bridge): port 8080, status path
// "/scada/report-current-status", alarm path "/scada/report-alarm".
#define NET_SCADA_HOST "192.168.1.10"
#define NET_SCADA_PORT 8088
#define NET_SCADA_STATUS_PATH "/system/webdev/SortingDemo/report_status"
#define NET_SCADA_ALARM_PATH "/scada/report-alarm"

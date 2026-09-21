// wifi_service.h — on-demand WiFi softAP + HTTP file server.
//
// Used only in "WiFi transfer" mode: the ESP32 brings up its own access point
// and serves the recordings over HTTP so the phone can download them at ~100x
// the Bluetooth SPP throughput. Bluetooth stays connected for control commands.
//
// The radio is shared with Bluetooth Classic, so WiFi and BT streaming must not
// run at full speed simultaneously — the app modes enforce this.
#pragma once

#include <Arduino.h>

// Bring up the softAP and start the HTTP server. Returns true on success.
// Idempotent: calling it while already active is a no-op that returns true.
bool wifiStart();

// Tear down the HTTP server and the softAP. Idempotent.
void wifiStop();

// True while the softAP is up.
bool wifiActive();

// Current AP IP address as a string (e.g. "192.168.4.1"), or "" when down.
String wifiIp();

// Configured AP SSID (from /config.txt or the compiled default).
const char *wifiSsid();

// FreeRTOS task entry point: services HTTP requests while WiFi is active.
void wifiTask(void *param);

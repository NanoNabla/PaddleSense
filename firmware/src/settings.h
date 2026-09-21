// settings.h — runtime-tunable storage settings loaded from /config.txt.
//
// The SD card may contain a plain-text /config.txt with "key=value" lines.
// It is parsed once at boot (after the card is mounted). Any key that is
// missing, malformed or out of range falls back to the compiled-in default
// from config.h, so a bad or absent file can never brick a recording session.
#pragma once

#include <Arduino.h>

struct Settings {
  // Write the text buffer to the card once it holds at least this many bytes.
  uint32_t writeThreshold;
  // ...or once this many milliseconds have elapsed since the last write,
  // whichever comes first. Bounds the write latency at low sample rates.
  uint32_t writeIntervalMs;
  // fsync() cadence. Bounds how much data is lost if power is cut.
  uint32_t flushIntervalMs;
  // WiFi softAP credentials for file-transfer mode (overridable via
  // /config.txt keys wifi_ssid / wifi_pass). Password must be >= 8 chars.
  char wifiSsid[33];
  char wifiPass[65];
};

// Populate `out` with the compiled-in defaults.
void settingsDefaults(Settings &out);

// Load /config.txt from the (already mounted) SD card into `out`. Missing or
// invalid keys keep their default value. Returns true if a config file was
// found and parsed, false if none was present (defaults are still applied).
bool settingsLoad(Settings &out);

// Log the effective settings over USB serial.
void settingsLog(const Settings &s);

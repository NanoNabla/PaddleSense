// wifi_service.cpp — on-demand WiFi softAP + HTTP file server.
//
// Endpoints (all plain HTTP, no auth — the AP is short-lived and local):
//   GET /files            -> "FILE <name> <size>\n" per recording, then "OK\n"
//   GET /files/<name>     -> 200 + Content-Length + raw bytes
//   GET /status           -> small JSON document
//
// The server runs on core 0 alongside the BT task. Only one of the two is ever
// doing bulk I/O at a time (enforced by the app's transfer mode).
#include "wifi_service.h"

#include "config.h"
#include "recorder.h"
#include "storage_task.h"
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

static WebServer s_server(WIFI_HTTP_PORT);
static bool s_active = false;
static bool s_routesReady = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Reject path traversal and require a bare file name (no separators).
static bool validName(const String &name) {
  if (name.length() == 0 || name.length() > 48) {
    return false;
  }
  if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 ||
      name.indexOf('\\') >= 0) {
    return false;
  }
  return true;
}

static void sendFileList() {
  File dir = SD.open(DATA_DIR);
  if (!dir || !dir.isDirectory()) {
    s_server.send(500, "text/plain", "ERR no_data_dir\n");
    return;
  }

  String body;
  body.reserve(512);
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char *name = entry.name();
      const char *base = strrchr(name, '/');
      base = base ? base + 1 : name;
      body += "FILE ";
      body += base;
      body += ' ';
      body += String((unsigned long)entry.size());
      body += '\n';
    }
    entry = dir.openNextFile();
  }
  dir.close();
  body += "OK\n";
  s_server.send(200, "text/plain", body);
}

static void sendFile(const String &name) {
  if (!validName(name)) {
    s_server.send(400, "text/plain", "ERR bad_name\n");
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name.c_str());

  File f = SD.open(path, FILE_READ);
  if (!f) {
    s_server.send(404, "text/plain", "ERR not_found\n");
    return;
  }
  if (f.isDirectory()) {
    f.close();
    s_server.send(400, "text/plain", "ERR is_dir\n");
    return;
  }

  // Content-Length lets the client show progress and verify the transfer.
  s_server.setContentLength(f.size());
  s_server.send(200, "application/octet-stream", "");

  WiFiClient client = s_server.client();
  static uint8_t chunk[WIFI_HTTP_CHUNK];
  while (f.available()) {
    const size_t n = f.read(chunk, sizeof(chunk));
    if (n == 0) {
      break;
    }
    if (client.write(chunk, n) != n) {
      break; // client vanished
    }
  }
  f.close();
}

static void sendStatus() {
  String body = "{";
  body += "\"recording\":";
  body += g_recorder.isRecording() ? "true" : "false";
  body += ",\"rate\":";
  body += String((unsigned)g_recorder.rateHz());
  body += ",\"files\":";
  body += String(storageFileCount());
  body += ",\"free_kb\":";
  body += String((unsigned long)storageFreeKb());
  body += ",\"dropped\":";
  body += String((unsigned long)g_recorder.ring().dropped());
  body += ",\"version\":\"";
  body += FW_VERSION;
  body += "\",\"ssid\":\"";
  body += wifiSsid();
  body += "\",\"ip\":\"";
  body += wifiIp();
  body += "\"}";
  s_server.send(200, "application/json", body);
}

static void registerRoutes() {
  if (s_routesReady) {
    return;
  }
  s_server.on("/files", HTTP_GET, sendFileList);
  s_server.on("/status", HTTP_GET, sendStatus);
  // Catch-all for /files/<name>; WebServer has no path parameters, so we parse
  // the URI ourselves.
  s_server.onNotFound([]() {
    const String uri = s_server.uri();
    if (uri.startsWith("/files/")) {
      sendFile(uri.substring(7));
    } else {
      s_server.send(404, "text/plain", "ERR not_found\n");
    }
  });
  s_routesReady = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool wifiStart() {
  if (s_active) {
    return true;
  }

  const Settings &cfg = storageSettings();

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(cfg.wifiSsid, cfg.wifiPass, WIFI_AP_CHANNEL, 0,
                   WIFI_AP_MAX_CLIENTS)) {
    Serial.println("[wifi] softAP start failed");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  registerRoutes();
  s_server.begin();
  s_active = true;

  Serial.printf("[wifi] AP '%s' up, http://%s/\n", cfg.wifiSsid,
                WiFi.softAPIP().toString().c_str());
  return true;
}

void wifiStop() {
  if (!s_active) {
    return;
  }
  s_server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_active = false;
  Serial.println("[wifi] AP down");
}

bool wifiActive() { return s_active; }

String wifiIp() {
  if (!s_active) {
    return String("");
  }
  return WiFi.softAPIP().toString();
}

const char *wifiSsid() { return storageSettings().wifiSsid; }

void wifiTask(void *param) {
  (void)param;
  for (;;) {
    if (s_active) {
      s_server.handleClient();
      vTaskDelay(1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

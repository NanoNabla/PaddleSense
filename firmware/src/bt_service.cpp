// bt_service.cpp — Bluetooth Classic SPP command protocol.
//
// Line-based ASCII protocol over RFCOMM. The phone is always the initiator and
// the ESP32 only ever replies, so a single request/response loop is sufficient
// (no background reader thread). File downloads stream raw bytes between a
// "BEGIN <size>" line and an "END <crc32>" line; the CRC is computed with the
// standard zlib polynomial so it matches java.util.zip.CRC32 on the phone.
#include "bt_service.h"

#include "config.h"
#include "recorder.h"
#include "storage_task.h"
#include "wifi_service.h"
#include <BluetoothSerial.h>
#include <SD.h>

static BluetoothSerial s_bt;
static volatile bool s_connected = false;

// ---------------------------------------------------------------------------
// CRC-32 (zlib / java.util.zip.CRC32 compatible)
// ---------------------------------------------------------------------------
static uint32_t s_crcTable[256];
static bool s_crcTableReady = false;

static void crcInitTable() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    s_crcTable[i] = c;
  }
  s_crcTableReady = true;
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc = s_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Line I/O
// ---------------------------------------------------------------------------
static void sendLine(const char *line) {
  s_bt.print(line);
  s_bt.print('\n');
}

static void sendError(const char *msg) {
  char buf[PROTO_LINE_MAX];
  snprintf(buf, sizeof(buf), "ERR %s", msg);
  sendLine(buf);
}

// Read one '\n'-terminated line into buf. Returns length, or -1 on timeout /
// disconnect. Blocks up to timeoutMs.
static int readLine(char *buf, size_t maxLen, uint32_t timeoutMs) {
  size_t len = 0;
  const uint32_t start = millis();
  for (;;) {
    if (!s_bt.connected()) {
      return -1;
    }
    if (s_bt.available()) {
      const int c = s_bt.read();
      if (c < 0) {
        continue;
      }
      if (c == '\n') {
        buf[len] = '\0';
        return (int)len;
      }
      if (c == '\r') {
        continue;
      }
      if (len + 1 < maxLen) {
        buf[len++] = (char)c;
      }
    } else {
      if (millis() - start > timeoutMs) {
        return -1;
      }
      vTaskDelay(1);
    }
  }
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void handleStatus() {
  char buf[PROTO_LINE_MAX];
  snprintf(
      buf, sizeof(buf),
      "STATUS recording=%d rate=%u files=%d free_kb=%u dropped=%u version=%s "
      "wifi=%d ip=%s ssid=%s",
      g_recorder.isRecording() ? 1 : 0, g_recorder.rateHz(), storageFileCount(),
      (unsigned)storageFreeKb(), (unsigned)g_recorder.ring().dropped(),
      FW_VERSION, wifiActive() ? 1 : 0, wifiActive() ? wifiIp().c_str() : "-",
      wifiActive() ? wifiSsid() : "-");
  sendLine(buf);
}

static void handleList() {
  File dir = SD.open(DATA_DIR);
  if (!dir || !dir.isDirectory()) {
    sendError("no_data_dir");
    return;
  }

  // First pass: count files.
  int count = 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory())
      count++;
    entry = dir.openNextFile();
  }

  char buf[PROTO_LINE_MAX];
  snprintf(buf, sizeof(buf), "FILES %d", count);
  sendLine(buf);

  // Second pass: emit name + size.
  dir.rewindDirectory();
  entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char *name = entry.name();
      // Some cores return the full path; strip any leading directory.
      const char *base = strrchr(name, '/');
      base = base ? base + 1 : name;
      snprintf(buf, sizeof(buf), "FILE %s %u", base, (unsigned)entry.size());
      sendLine(buf);
    }
    entry = dir.openNextFile();
  }
  dir.close();
  sendLine("OK");
}

// Stream a file as: BEGIN <size> \n <raw bytes> END <crc32> \n
static void handleGet(const char *name) {
  // Reject path traversal and require the expected prefix/suffix.
  if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
    sendError("bad_name");
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);

  File f = SD.open(path, FILE_READ);
  if (!f) {
    sendError("not_found");
    return;
  }
  if (f.isDirectory()) {
    f.close();
    sendError("is_dir");
    return;
  }

  const uint32_t size = f.size();
  char buf[64];
  snprintf(buf, sizeof(buf), "BEGIN %u", (unsigned)size);
  sendLine(buf);

  uint32_t crc = 0;
  static uint8_t chunk[PROTO_TX_CHUNK];
  uint32_t sent = 0;
  while (sent < size) {
    if (!s_bt.connected()) {
      f.close();
      return; // phone vanished; file stays on SD
    }
    const size_t want =
        (size - sent) > sizeof(chunk) ? sizeof(chunk) : (size - sent);
    const size_t got = f.read(chunk, want);
    if (got == 0) {
      break; // unexpected EOF
    }
    crc = crcUpdate(crc, chunk, got);
    s_bt.write(chunk, got);
    sent += got;
  }
  f.close();

  if (sent != size) {
    // We already sent BEGIN; signal failure with a distinct trailer.
    sendLine("END 0");
    return;
  }

  snprintf(buf, sizeof(buf), "END %08X", (unsigned)crc);
  sendLine(buf);
}

// Stop the current session (shared by STOP and the tail loop). Returns true if
// a session was actually stopped.
static bool doStopSession() {
  if (!g_recorder.isRecording()) {
    return false;
  }
  g_recorder.setState(RecState::Idle);
  storageStopSession();
  return true;
}

// Live-tail: stream the currently open recording file to the phone while it is
// still being written. Framing is BEGIN ? -> repeated (DATA <n> + n bytes) ->
// END <crc32>. While tailing, only STOP and ENDTAIL are honored; any other
// command line is ignored (a reply would corrupt the binary stream).
static void handleTail(const char *name) {
  if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
    sendError("bad_name");
    return;
  }

  // If the requested file is not the one currently open, fall back to a normal
  // (static) download.
  const bool isLive =
      storageIsOpen() && strcmp(name, storageCurrentName()) == 0;
  if (!isLive) {
    handleGet(name);
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);

  sendLine("BEGIN ?");

  uint32_t crc = 0;
  uint32_t sent = 0;
  bool aborted = false;
  bool stopped = false;
  static uint8_t chunk[PROTO_TX_CHUNK];
  char line[PROTO_LINE_MAX];

  for (;;) {
    if (!s_bt.connected()) {
      return; // phone vanished; file stays on SD
    }

    // Honor STOP / ENDTAIL from the phone. A short timeout lets a command that
    // arrives split across RFCOMM packets complete before we parse it.
    if (s_bt.available()) {
      const int n = readLine(line, sizeof(line), 50);
      if (n > 0) {
        if (strcasecmp(line, "STOP") == 0) {
          stopped = doStopSession();
        } else if (strcasecmp(line, "ENDTAIL") == 0) {
          aborted = true;
        }
        // Any other command is intentionally ignored while tailing.
      }
    }

    // Stream any newly written bytes.
    const uint32_t written = storageBytesWritten();
    if (written > sent) {
      File f = SD.open(path, FILE_READ);
      if (f) {
        f.seek(sent);
        while (sent < written) {
          const size_t want = (written - sent) > sizeof(chunk)
                                  ? sizeof(chunk)
                                  : (written - sent);
          const size_t got = f.read(chunk, want);
          if (got == 0) {
            break;
          }
          crc = crcUpdate(crc, chunk, got);
          char hdr[24];
          snprintf(hdr, sizeof(hdr), "DATA %u\n", (unsigned)got);
          s_bt.print(hdr);
          s_bt.write(chunk, got);
          sent += got;
        }
        f.close();
      }
    }

    // Done when the session has ended and all bytes have been sent.
    if (!storageIsOpen() && sent >= storageBytesWritten()) {
      break;
    }
    if (aborted) {
      break;
    }

    // Wait for the storage task to publish more bytes (or time out to re-check
    // the phone's control lines).
    SemaphoreHandle_t sem = storageFlushSemaphore();
    if (sem != nullptr) {
      xSemaphoreTake(sem, pdMS_TO_TICKS(200));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  if (aborted) {
    sendLine("END ABORT");
    return;
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "END %08X", (unsigned)crc);
  sendLine(buf);

  // If the phone asked us to stop during the tail, confirm it now (after the
  // binary stream is complete so the reply cannot be mistaken for payload).
  if (stopped) {
    sendLine("STOPPED");
  }
}

static void handleMode(const char *arg) {
  if (!arg || *arg == '\0' || strcasecmp(arg, "legacy") == 0) {
    wifiStop();
    sendLine("MODE legacy");
    return;
  }
  if (strcasecmp(arg, "wifi") == 0) {
    if (!wifiStart()) {
      sendError("wifi_failed");
      return;
    }
    char buf[PROTO_LINE_MAX];
    snprintf(buf, sizeof(buf), "MODE wifi ssid=%s ip=%s", wifiSsid(),
             wifiIp().c_str());
    sendLine(buf);
    return;
  }
  sendError("bad_mode");
}

static void handleDel(const char *name) {
  if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
    sendError("bad_name");
    return;
  }
  char path[64];
  snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);

  if (!SD.exists(path)) {
    sendError("not_found");
    return;
  }
  if (SD.remove(path)) {
    sendLine("DELETED");
  } else {
    sendError("delete_failed");
  }
}

static void handleStart() {
  if (g_recorder.isRecording()) {
    sendError("already_recording");
    return;
  }
  if (!storageStartSession()) {
    sendError("sd_not_ready");
    return;
  }
  g_recorder.beginSession();
  g_recorder.setState(RecState::Recording);
  char buf[PROTO_LINE_MAX];
  snprintf(buf, sizeof(buf), "STARTED %s", storageCurrentName());
  sendLine(buf);
}

static void handleStop() {
  if (!doStopSession()) {
    sendError("not_recording");
    return;
  }
  sendLine("STOPPED");
}

static void handleRate(const char *arg) {
  if (!arg || *arg == '\0') {
    sendError("missing_rate");
    return;
  }
  const long hz = atol(arg);
  if (hz <= 0) {
    sendError("bad_rate");
    return;
  }
  const uint16_t applied = g_recorder.setRateHz((uint16_t)hz);
  char buf[32];
  snprintf(buf, sizeof(buf), "RATE %u", applied);
  sendLine(buf);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
static void dispatch(char *line) {
  // Trim leading spaces.
  while (*line == ' ')
    line++;
  if (*line == '\0') {
    return;
  }

  // Split command and argument at the first space.
  char *arg = strchr(line, ' ');
  if (arg) {
    *arg = '\0';
    arg++;
    while (*arg == ' ')
      arg++;
  }

  if (strcasecmp(line, "PING") == 0) {
    sendLine("PONG");
  } else if (strcasecmp(line, "STATUS") == 0) {
    handleStatus();
  } else if (strcasecmp(line, "LIST") == 0) {
    handleList();
  } else if (strcasecmp(line, "GET") == 0) {
    handleGet(arg ? arg : "");
  } else if (strcasecmp(line, "TAIL") == 0) {
    handleTail(arg ? arg : "");
  } else if (strcasecmp(line, "MODE") == 0) {
    handleMode(arg);
  } else if (strcasecmp(line, "DEL") == 0) {
    handleDel(arg ? arg : "");
  } else if (strcasecmp(line, "START") == 0) {
    handleStart();
  } else if (strcasecmp(line, "STOP") == 0) {
    handleStop();
  } else if (strcasecmp(line, "RATE") == 0) {
    handleRate(arg);
  } else {
    sendError("unknown_command");
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool btInit() {
  crcInitTable();

  // Prefer the name configured in /config.txt (bt_name). When the SD card is
  // absent or the mount failed, settingsLoad() never ran and the field is
  // empty, so fall back to the compiled-in default.
  const char *name = storageSettings().btName;
  if (name[0] == '\0') {
    name = BT_DEVICE_NAME;
  }

  if (!s_bt.begin(name)) {
    Serial.println("[bt] begin failed");
    return false;
  }
  Serial.printf("[bt] SPP server '%s' ready\n", name);
  return true;
}

bool btConnected() { return s_connected; }

void btTask(void *param) {
  (void)param;

  char line[PROTO_LINE_MAX];

  for (;;) {
    if (!s_bt.hasClient()) {
      s_connected = false;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (!s_connected) {
      s_connected = true;
      Serial.println("[bt] client connected");
    }

    const int n = readLine(line, sizeof(line), 1000);
    if (n < 0) {
      if (!s_bt.hasClient()) {
        s_connected = false;
        Serial.println("[bt] client disconnected");
      }
      continue;
    }
    if (n == 0) {
      continue;
    }

    dispatch(line);
  }
}

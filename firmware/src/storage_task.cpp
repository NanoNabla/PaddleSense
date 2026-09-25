// storage_task.cpp — SD card writer.
//
// Consumes samples from the shared ring buffer and appends them to a CSV file
// under /data. Text is accumulated in a RAM buffer and written to the card
// only when the buffer reaches a size threshold OR a time interval elapses
// (whichever comes first), so there is never one SPI transaction per sample.
// A separate periodic flush() bounds the amount of data lost if power is cut.
// The threshold/interval values are runtime-tunable via /config.txt.
#include "storage_task.h"

#include "config.h"
#include "recorder.h"
#include "settings.h"
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <atomic>

static SPIClass s_spi(VSPI);
static File s_file;
static bool s_sdReady = false;
static bool s_fileOpen = false;
static uint32_t s_fileIndex = 0;
static uint32_t s_lastWriteMs = 0;
static uint32_t s_lastFlushMs = 0;
static char s_currentName[32] = {0};

// Live-tail support: total payload bytes durably written to the current file,
// and a binary semaphore given after every write+fsync round (and on close) so
// the BT tailer / HTTP server can be woken instead of busy-polling.
static std::atomic<uint32_t> s_bytesWritten{0};
static SemaphoreHandle_t s_flushSem = nullptr;

// Runtime-tunable storage settings (defaults, overridable via /config.txt).
static Settings s_settings;

// Text accumulation buffer. Sized to hold at least one full CSV line plus the
// write threshold so a single line always fits without splitting.
static char s_buf[SD_TEXT_BUF_MAX];
static size_t s_bufLen = 0;

static Preferences s_prefs;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void loadFileIndex() {
  s_prefs.begin(NVS_NAMESPACE, false);
  s_fileIndex = s_prefs.getUInt(NVS_KEY_FILE_INDEX, 0);
  s_prefs.end();
  Serial.printf("[storage] next file index = %u\n", s_fileIndex);
}

static void saveFileIndex() {
  s_prefs.begin(NVS_NAMESPACE, false);
  s_prefs.putUInt(NVS_KEY_FILE_INDEX, s_fileIndex);
  s_prefs.end();
}

static void flushBuffer(bool force) {
  if (!s_fileOpen || s_bufLen == 0) {
    return;
  }
  if (!force && s_bufLen < s_settings.writeThreshold) {
    return;
  }
  const size_t written = s_file.write((const uint8_t *)s_buf, s_bufLen);
  if (written != s_bufLen) {
    Serial.printf("[storage] short write %u/%u\n", (unsigned)written,
                  (unsigned)s_bufLen);
  }
  s_bytesWritten.fetch_add((uint32_t)written, std::memory_order_release);
  s_bufLen = 0;
}

static void appendToBuffer(const char *text, size_t len) {
  // The buffer is sized to hold at least one full line plus the threshold,
  // so a single line always fits; flush first if it would overflow.
  if (s_bufLen + len > sizeof(s_buf)) {
    flushBuffer(true);
  }
  if (len > sizeof(s_buf)) {
    // Pathological line; write directly.
    s_file.write((const uint8_t *)text, len);
    return;
  }
  memcpy(s_buf + s_bufLen, text, len);
  s_bufLen += len;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool storageInit() {
  if (s_flushSem == nullptr) {
    s_flushSem = xSemaphoreCreateBinary();
  }
  s_spi.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);

  if (!SD.begin(PIN_SD_CS, s_spi, 15000000)) {
    Serial.println("[storage] SD mount failed");
    s_sdReady = false;
    return false;
  }

  s_sdReady = true;
  if (!SD.exists(DATA_DIR)) {
    SD.mkdir(DATA_DIR);
  }

  // Load runtime settings from /config.txt (falls back to compiled defaults).
  settingsLoad(s_settings);
  settingsLog(s_settings);

  loadFileIndex();
  Serial.printf("[storage] SD ready, %u KB free\n", (unsigned)storageFreeKb());
  return true;
}

bool storageStartSession() {
  if (!s_sdReady) {
    return false;
  }
  if (s_fileOpen) {
    return true;
  }

  // Build the next file name: /data/ps_0001.csv
  snprintf(s_currentName, sizeof(s_currentName), "%s/%s%04u%s", DATA_DIR,
           FILE_PREFIX, (unsigned)(s_fileIndex + 1), FILE_SUFFIX);

  s_file = SD.open(s_currentName, FILE_WRITE);
  if (!s_file) {
    Serial.printf("[storage] cannot open %s\n", s_currentName);
    return false;
  }

  s_fileIndex++;
  saveFileIndex();

  s_bufLen = 0;
  s_bytesWritten.store(0, std::memory_order_release);
  s_lastWriteMs = millis();
  s_lastFlushMs = millis();
  s_fileOpen = true;

  // Header
  char header[128];
  int n =
      snprintf(header, sizeof(header),
               "# paddlesense v%s rate=%u arange=%dg grange=%ddps\n"
               "t_us,ax,ay,az,gx,gy,gz\n",
               FW_VERSION, g_recorder.rateHz(), ACCEL_RANGE_G, GYRO_RANGE_DPS);
  appendToBuffer(header, (size_t)n);
  flushBuffer(true);

  Serial.printf("[storage] recording to %s\n", s_currentName);
  return true;
}

void storageStopSession() {
  if (!s_fileOpen) {
    return;
  }

  // Drain anything still in the ring buffer before closing.
  Sample s;
  while (g_recorder.ring().pop(s)) {

    const float ax = (s.ax / ACCEL_LSB_PER_G) * 9.80665f;
    const float ay = (s.ay / ACCEL_LSB_PER_G) * 9.80665f;
    const float az = (s.az / ACCEL_LSB_PER_G) * 9.80665f;

    const float gx = s.gx / GYRO_LSB_PER_DPS;
    const float gy = s.gy / GYRO_LSB_PER_DPS;
    const float gz = s.gz / GYRO_LSB_PER_DPS;

    char line[96];
    int n = snprintf(line, sizeof(line), "%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                     (unsigned)s.tUs, ax, ay, az, gx, gy, gz);
    appendToBuffer(line, (size_t)n);
    g_recorder.countSample();
  }

  // Footer with integrity metadata.
  char footer[96];
  int n = snprintf(footer, sizeof(footer), "# samples=%u dropped=%u\n",
                   (unsigned)g_recorder.sessionSamples(),
                   (unsigned)g_recorder.ring().dropped());
  appendToBuffer(footer, (size_t)n);

  flushBuffer(true);
  s_file.flush();
  s_file.close();
  s_fileOpen = false;

  // Wake any tailer so it can observe the final size and finish.
  if (s_flushSem != nullptr) {
    xSemaphoreGive(s_flushSem);
  }

  Serial.printf("[storage] closed %s (%u samples, %u dropped)\n", s_currentName,
                (unsigned)g_recorder.sessionSamples(),
                (unsigned)g_recorder.ring().dropped());
}

bool storageIsOpen() { return s_fileOpen; }

int storageFileCount() {
  if (!s_sdReady) {
    return 0;
  }
  File dir = SD.open(DATA_DIR);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  int count = 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      count++;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

uint32_t storageFreeKb() {
  if (!s_sdReady) {
    return 0;
  }
  return (uint32_t)(SD.totalBytes() - SD.usedBytes()) / 1024u;
}

uint32_t storageBytesWritten() {
  return s_bytesWritten.load(std::memory_order_acquire);
}

const char *storageCurrentName() {
  if (!s_fileOpen) {
    return "";
  }
  // s_currentName is "/data/ps_0005.csv"; return just the base name.
  const char *base = strrchr(s_currentName, '/');
  return base ? base + 1 : s_currentName;
}

SemaphoreHandle_t storageFlushSemaphore() { return s_flushSem; }

const Settings &storageSettings() { return s_settings; }

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

void storageTask(void *param) {
  (void)param;

  for (;;) {
    bool didWork = false;

    if (s_fileOpen) {
      // Drain up to a bounded number of samples per iteration so the
      // task stays responsive to STOP requests.
      Sample s;
      int budget = 256;
      while (budget-- > 0 && g_recorder.ring().pop(s)) {
        char line[96];
        int n =
            snprintf(line, sizeof(line), "%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                     (unsigned)s.tUs, s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
        appendToBuffer(line, (size_t)n);
        g_recorder.countSample();
        didWork = true;
      }

      const uint32_t now = millis();

      // Write to the card when the buffer is full enough OR the write
      // interval has elapsed, whichever comes first. This keeps the number
      // of SPI transactions moderate even at high sample rates.
      if (s_bufLen >= s_settings.writeThreshold ||
          now - s_lastWriteMs >= s_settings.writeIntervalMs) {
        flushBuffer(true);
        s_lastWriteMs = now;
        // fsync immediately after each write so a freshly opened handle (used
        // by the BT tailer / HTTP server) sees the updated size right away.
        s_file.flush();
        s_lastFlushMs = now;
        if (s_flushSem != nullptr) {
          xSemaphoreGive(s_flushSem);
        }
      }

      // Safety-net fsync on its own cadence in case no write happened above
      // (e.g. very low sample rate) to bound data loss if power is cut.
      if (now - s_lastFlushMs >= s_settings.flushIntervalMs) {
        s_file.flush();
        s_lastFlushMs = now;
      }
    }

    // If there was nothing to do, sleep briefly to yield the CPU.
    vTaskDelay(didWork ? 1 : pdMS_TO_TICKS(20));
  }
}

// settings.cpp — /config.txt parser for runtime-tunable storage settings.
#include "settings.h"

#include "config.h"
#include <SD.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Clamp a value into [lo, hi].
static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

// Trim leading/trailing whitespace in place and return the trimmed pointer.
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                     end[-1] == '\n'))
    *--end = '\0';
  return s;
}

// Apply one "key=value" line to `out`. Unknown keys and malformed values are
// ignored (the default is kept). Returns true if a known key was applied.
static bool applyKey(Settings &out, const char *key, const char *value) {
  // Parse the value as an unsigned integer; reject anything non-numeric.
  char *endp = nullptr;
  const long v = strtol(value, &endp, 10);
  if (endp == value || v < 0) {
    return false;
  }
  const uint32_t uv = (uint32_t)v;

  if (strcasecmp(key, "write_threshold") == 0) {
    out.writeThreshold =
        clampU32(uv, SD_WRITE_THRESHOLD_MIN, SD_WRITE_THRESHOLD_MAX);
    return true;
  }
  if (strcasecmp(key, "write_interval_ms") == 0) {
    out.writeIntervalMs =
        clampU32(uv, SD_WRITE_INTERVAL_MS_MIN, SD_WRITE_INTERVAL_MS_MAX);
    return true;
  }
  if (strcasecmp(key, "flush_interval_ms") == 0) {
    out.flushIntervalMs =
        clampU32(uv, SD_FLUSH_INTERVAL_MS_MIN, SD_FLUSH_INTERVAL_MS_MAX);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void settingsDefaults(Settings &out) {
  out.writeThreshold = SD_WRITE_THRESHOLD_DEFAULT;
  out.writeIntervalMs = SD_WRITE_INTERVAL_MS_DEFAULT;
  out.flushIntervalMs = SD_FLUSH_INTERVAL_MS_DEFAULT;
}

bool settingsLoad(Settings &out) {
  settingsDefaults(out);

  if (!SD.exists(CONFIG_PATH)) {
    Serial.println("[settings] no /config.txt, using defaults");
    return false;
  }

  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[settings] cannot open /config.txt, using defaults");
    return false;
  }

  char line[128];
  size_t len = 0;
  int applied = 0;
  while (f.available()) {
    const int c = f.read();
    if (c < 0) {
      break;
    }
    if (c == '\n') {
      line[len] = '\0';
      len = 0;

      char *p = trim(line);
      if (*p == '\0' || *p == '#' || *p == ';') {
        continue; // blank line or comment
      }
      char *eq = strchr(p, '=');
      if (!eq) {
        continue; // not a key=value line
      }
      *eq = '\0';
      char *key = trim(p);
      char *value = trim(eq + 1);
      if (applyKey(out, key, value)) {
        applied++;
      } else {
        Serial.printf("[settings] ignoring '%s=%s'\n", key, value);
      }
    } else if (len + 1 < sizeof(line)) {
      line[len++] = (char)c;
    }
  }
  f.close();

  Serial.printf("[settings] loaded /config.txt (%d keys applied)\n", applied);
  return true;
}

void settingsLog(const Settings &s) {
  Serial.printf("[settings] write_threshold=%u write_interval_ms=%u "
                "flush_interval_ms=%u\n",
                (unsigned)s.writeThreshold, (unsigned)s.writeIntervalMs,
                (unsigned)s.flushIntervalMs);
}

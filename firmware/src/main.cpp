// main.cpp — paddle-meter firmware entry point.
//
// Brings up the sensor, SD card and Bluetooth SPP service, then creates the
// three FreeRTOS tasks that do the actual work:
//
//   sensorTask  (core 1, prio 5) — MPU-6050 -> ring buffer
//   storageTask (core 1, prio 3) — ring buffer -> SD card
//   btTask      (core 0, prio 2) — SPP command protocol
//
// The Arduino loop() only drives the status LED and periodic health logging.
#include "bt_service.h"
#include "config.h"
#include "recorder.h"
#include "sensor_task.h"
#include "storage_task.h"
#include <Arduino.h>

// Global recorder instance (declared extern in recorder.h).
Recorder g_recorder;

static bool s_sensorOk = false;
static bool s_sdOk = false;

// ---------------------------------------------------------------------------
// Status LED
//   solid        = recording
//   slow blink   = idle / ready
//   fast blink   = error (sensor or SD failure)
// ---------------------------------------------------------------------------
static void updateLed() {
  const uint32_t now = millis();

  if (!s_sensorOk || !s_sdOk) {
    // Fast blink: 100 ms on / 100 ms off
    digitalWrite(PIN_STATUS_LED, (now / 100) % 2);
    return;
  }
  if (g_recorder.isRecording()) {
    digitalWrite(PIN_STATUS_LED, HIGH);
    return;
  }
  // Slow blink: 1 s on / 1 s off
  digitalWrite(PIN_STATUS_LED, (now / 1000) % 2);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== paddle-meter ===");
  Serial.printf("firmware v%s\n", FW_VERSION);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Bring up peripherals. Failures are non-fatal: the device still boots and
  // reports the problem over Bluetooth / the LED.
  s_sensorOk = sensorInit();
  s_sdOk = storageInit();
  if (!s_sdOk) {
    g_recorder.setState(RecState::Error);
  }

  if (!btInit()) {
    Serial.println("[main] Bluetooth init failed");
  }

  // Create the worker tasks.
  xTaskCreatePinnedToCore(sensorTask, "sensor", SENSOR_TASK_STACK, nullptr,
                          SENSOR_TASK_PRIO, nullptr, SENSOR_TASK_CORE);

  xTaskCreatePinnedToCore(storageTask, "storage", STORAGE_TASK_STACK, nullptr,
                          STORAGE_TASK_PRIO, nullptr, STORAGE_TASK_CORE);

  xTaskCreatePinnedToCore(btTask, "bt", BT_TASK_STACK, nullptr, BT_TASK_PRIO,
                          nullptr, BT_TASK_CORE);

  Serial.println("[main] tasks started");
}

void loop() {
  updateLed();

  // Periodic health line on USB serial (not Bluetooth).
  static uint32_t lastLog = 0;
  const uint32_t now = millis();
  if (now - lastLog >= 5000) {
    lastLog = now;
    Serial.printf(
        "[main] rec=%d rate=%u ring=%u dropped=%u files=%d free=%uKB bt=%d "
        "heap=%u minheap=%u\n",
        g_recorder.isRecording() ? 1 : 0, g_recorder.rateHz(),
        (unsigned)g_recorder.ring().size(),
        (unsigned)g_recorder.ring().dropped(), storageFileCount(),
        (unsigned)storageFreeKb(), btConnected() ? 1 : 0,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)esp_get_minimum_free_heap_size());
  }

  delay(50);
}

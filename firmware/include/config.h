// config.h — compile-time configuration for the paddle-meter firmware.
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define FW_VERSION "1"
#define BT_DEVICE_NAME "paddle-meter"

// ---------------------------------------------------------------------------
// Pin map (see docs/architecture.md §1)
// ---------------------------------------------------------------------------
// MPU-6050 on I2C
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define MPU6050_ADDR 0x68

// SD card reader on VSPI
#define PIN_SD_CS 5
#define PIN_SPI_SCK 18
#define PIN_SPI_MISO 19
#define PIN_SPI_MOSI 23

// Status LED (onboard LED on most ESP32 dev boards)
#define PIN_STATUS_LED 2

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
#define DEFAULT_RATE_HZ 200
#define MIN_RATE_HZ 50
#define MAX_RATE_HZ 1000

// MPU-6050 full-scale ranges (must match the conversion constants below)
#define ACCEL_RANGE_G 4    // ±4 g
#define GYRO_RANGE_DPS 500 // ±500 deg/s

// Sensitivity scale factors from the MPU-6050 datasheet (LSB per unit)
#define ACCEL_LSB_PER_G 8192.0f // ±4 g  -> 8192 LSB/g
#define GYRO_LSB_PER_DPS 65.5f  // ±500 dps -> 65.5 LSB/(deg/s)

// DLPF configuration register value (44 Hz accel / 42 Hz gyro bandwidth)
#define MPU6050_DLPF_CFG 3

// ---------------------------------------------------------------------------
// Ring buffer / storage
// ---------------------------------------------------------------------------
#define RING_CAPACITY 1024 // samples (~24 KB)

// Static text accumulation buffer. Must hold at least one full CSV line plus
// the write threshold so a single line always fits without splitting.
#define SD_TEXT_BUF_MAX 32768 // 32 KB

// Defaults for the runtime-tunable storage settings. These can be overridden
// at boot by a /config.txt file on the SD card (see settings.h); the compiled
// values below are used for any key that is missing or invalid.
#define SD_WRITE_THRESHOLD_DEFAULT 24576  // write to SD at >= 24 KB
#define SD_WRITE_INTERVAL_MS_DEFAULT 2000 // ...or every 2 s, whichever first
#define SD_FLUSH_INTERVAL_MS_DEFAULT 2000 // fsync cadence (bounds data loss)

// Clamp ranges applied to values read from /config.txt.
#define SD_WRITE_THRESHOLD_MIN 512
#define SD_WRITE_THRESHOLD_MAX SD_TEXT_BUF_MAX
#define SD_WRITE_INTERVAL_MS_MIN 100
#define SD_WRITE_INTERVAL_MS_MAX 60000
#define SD_FLUSH_INTERVAL_MS_MIN 100
#define SD_FLUSH_INTERVAL_MS_MAX 60000

#define CONFIG_PATH "/config.txt"
#define DATA_DIR "/data"
#define FILE_PREFIX "pm_"
#define FILE_SUFFIX ".csv"

// ---------------------------------------------------------------------------
// Task configuration
// ---------------------------------------------------------------------------
#define SENSOR_TASK_CORE 1
#define SENSOR_TASK_PRIO 5
#define STORAGE_TASK_CORE 1
#define STORAGE_TASK_PRIO 3
#define BT_TASK_CORE 0
#define BT_TASK_PRIO 2

#define SENSOR_TASK_STACK 4096
#define STORAGE_TASK_STACK 8192
#define BT_TASK_STACK 8192

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------
#define PROTO_LINE_MAX 128  // max command line length
#define PROTO_TX_CHUNK 1024 // bytes per SPP write during file streaming
#define NVS_NAMESPACE "paddlemtr"
#define NVS_KEY_FILE_INDEX "file_idx"

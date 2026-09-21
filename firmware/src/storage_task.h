// storage_task.h — SD card writer task.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "settings.h"

// Mount the SD card and ensure the data directory exists.
bool storageInit();

// FreeRTOS task entry point. Drains the ring buffer into CSV files.
void storageTask(void *param);

// Open a new recording file. Returns true on success.
bool storageStartSession();

// Close the current recording file and write the footer.
void storageStopSession();

// True while a file is open for writing.
bool storageIsOpen();

// Number of recording files currently on the card.
int storageFileCount();

// Free space on the card in kilobytes (0 if unknown).
uint32_t storageFreeKb();

// ---------------------------------------------------------------------------
// Live-tail support
// ---------------------------------------------------------------------------

// Total number of payload bytes durably written to the current file (header +
// data + footer). Reset when a session starts. Used by the BT tailer and the
// WiFi HTTP server to know how much of the file is safe to read.
uint32_t storageBytesWritten();

// Base name of the currently open recording file (e.g. "ps_0005.csv"), or an
// empty string when no session is open.
const char *storageCurrentName();

// Binary semaphore given by the storage task after every write+fsync round and
// after the final close. Consumers (BT tailer) wait on it to be woken when new
// bytes become readable instead of busy-polling.
SemaphoreHandle_t storageFlushSemaphore();

// The effective runtime settings (defaults merged with /config.txt). Valid
// after storageInit().
const Settings &storageSettings();

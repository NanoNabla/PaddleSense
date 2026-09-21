// storage_task.h — SD card writer task.
#pragma once

#include <Arduino.h>

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

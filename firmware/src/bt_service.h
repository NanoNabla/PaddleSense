// bt_service.h — Bluetooth Classic SPP command service.
#pragma once

#include <Arduino.h>

// Start the SPP server and create the BT task.
bool btInit();

// FreeRTOS task entry point: accepts connections and processes commands.
void btTask(void *param);

// True while a phone is connected.
bool btConnected();

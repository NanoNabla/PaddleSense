// sensor_task.h — MPU-6050 sampling task.
#pragma once

#include <Arduino.h>

// Initialise the MPU-6050. Returns true on success.
bool sensorInit();

// FreeRTOS task entry point. Samples at g_recorder.rateHz() and pushes
// samples into the shared ring buffer.
void sensorTask(void *param);

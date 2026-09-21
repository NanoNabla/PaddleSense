// sensor_task.cpp — high-rate MPU-6050 sampling.
//
// Runs on core 1 at the highest application priority. Uses vTaskDelayUntil so
// the sampling period does not drift with execution time. Reads all six axes
// in a single 14-byte I2C burst (register 0x3B..0x48) to keep the bus
// transaction short and the sample coherent.
#include "sensor_task.h"

#include "config.h"
#include "recorder.h"
#include <Wire.h>

// MPU-6050 register map (subset)
static constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
static constexpr uint8_t REG_SMPLRT_DIV = 0x19;
static constexpr uint8_t REG_CONFIG = 0x1A;
static constexpr uint8_t REG_GYRO_CONFIG = 0x1B;
static constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
static constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
static constexpr uint8_t REG_WHO_AM_I = 0x75;

static void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

bool sensorInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

  const uint8_t who = readReg(REG_WHO_AM_I);
  if (who != 0x68 && who != 0x70 && who != 0x71 && who != 0x73) {
    Serial.printf("[sensor] unexpected WHO_AM_I=0x%02X\n", who);
    return false;
  }

  writeReg(REG_PWR_MGMT_1, 0x00); // wake up, internal 8 MHz oscillator
  delay(50);
  writeReg(REG_PWR_MGMT_1, 0x01); // PLL with X gyro reference (stable clock)
  delay(10);

  // DLPF: configures the digital low-pass filter (also sets gyro output rate).
  writeReg(REG_CONFIG, MPU6050_DLPF_CFG);

  // Sample rate divider: with DLPF enabled the gyro output rate is 1 kHz,
  // so divider = 1000/rate - 1. The task period is the real limiter; this
  // keeps the internal rate at or above the requested rate.
  const uint16_t rate = g_recorder.rateHz();
  uint16_t div = (rate >= 1000) ? 0 : (1000 / rate - 1);
  if (div > 255)
    div = 255;
  writeReg(REG_SMPLRT_DIV, (uint8_t)div);

  // Full-scale ranges: ±4 g and ±500 deg/s (see config.h).
  writeReg(REG_ACCEL_CONFIG, 0x08); // AFS_SEL = 1 -> ±4 g
  writeReg(REG_GYRO_CONFIG, 0x08);  // FS_SEL  = 1 -> ±500 deg/s

  Serial.printf("[sensor] MPU-6050 ready (WHO_AM_I=0x%02X, rate=%u Hz)\n", who,
                rate);
  return true;
}

// Read all 6 axes in one burst and convert to SI units.
static bool readSample(Sample &s) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)14) != 14) {
    return false;
  }

  uint8_t b[14];
  for (int i = 0; i < 14; i++) {
    b[i] = Wire.read();
  }

  const int16_t ax = (int16_t)((b[0] << 8) | b[1]);
  const int16_t ay = (int16_t)((b[2] << 8) | b[3]);
  const int16_t az = (int16_t)((b[4] << 8) | b[5]);
  // b[6],b[7] = temperature (unused)
  const int16_t gx = (int16_t)((b[8] << 8) | b[9]);
  const int16_t gy = (int16_t)((b[10] << 8) | b[11]);
  const int16_t gz = (int16_t)((b[12] << 8) | b[13]);

  constexpr float G = 9.80665f;
  s.tUs = micros();
  s.ax = (ax / ACCEL_LSB_PER_G) * G;
  s.ay = (ay / ACCEL_LSB_PER_G) * G;
  s.az = (az / ACCEL_LSB_PER_G) * G;
  s.gx = gx / GYRO_LSB_PER_DPS;
  s.gy = gy / GYRO_LSB_PER_DPS;
  s.gz = gz / GYRO_LSB_PER_DPS;
  return true;
}

void sensorTask(void *param) {
  (void)param;

  TickType_t lastWake = xTaskGetTickCount();
  uint16_t lastRate = 0;

  for (;;) {
    const uint16_t rate = g_recorder.rateHz();

    // Recompute the period when the rate changes.
    if (rate != lastRate) {
      lastRate = rate;
      Serial.printf("[sensor] sampling at %u Hz\n", rate);
    }

    if (g_recorder.isRecording()) {
      Sample s;
      if (readSample(s)) {
        g_recorder.ring().push(s);
      }
    }

    const TickType_t period = pdMS_TO_TICKS(1000) / rate;
    vTaskDelayUntil(&lastWake, period > 0 ? period : 1);
  }
}

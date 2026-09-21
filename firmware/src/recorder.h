// recorder.h — shared recorder state machine.
//
// Owns the global state that the sensor, storage and BT tasks all need to see:
// whether a recording session is active, the configured sample rate, and the
// ring buffer that connects the sensor to the storage task.
#pragma once

#include "Sample.h"
#include <Arduino.h>
#include <atomic>

enum class RecState : uint8_t {
  Idle = 0,      // not recording
  Recording = 1, // session active, samples flowing to SD
  Error = 2      // SD or sensor failure
};

class Recorder {
public:
  Recorder() : _state(RecState::Idle), _rateHz(DEFAULT_RATE_HZ) {}

  RecState state() const { return _state.load(std::memory_order_acquire); }
  bool isRecording() const { return state() == RecState::Recording; }

  void setState(RecState s) { _state.store(s, std::memory_order_release); }

  uint16_t rateHz() const { return _rateHz.load(std::memory_order_acquire); }

  // Clamp to the supported range and return the applied value.
  uint16_t setRateHz(uint16_t hz) {
    if (hz < MIN_RATE_HZ)
      hz = MIN_RATE_HZ;
    if (hz > MAX_RATE_HZ)
      hz = MAX_RATE_HZ;
    _rateHz.store(hz, std::memory_order_release);
    return hz;
  }

  SampleRing &ring() { return _ring; }

  // Session statistics, updated by the storage task.
  void beginSession() {
    _sessionSamples.store(0, std::memory_order_relaxed);
    _ring.resetDropped();
  }
  void countSample() {
    _sessionSamples.fetch_add(1, std::memory_order_relaxed);
  }
  uint32_t sessionSamples() const {
    return _sessionSamples.load(std::memory_order_relaxed);
  }

private:
  std::atomic<RecState> _state;
  std::atomic<uint16_t> _rateHz;
  std::atomic<uint32_t> _sessionSamples;
  SampleRing _ring;
};

// Single global instance shared across tasks.
extern Recorder g_recorder;

// Sample.h — sample data model and a lock-free single-producer/single-consumer
// ring buffer used to decouple the high-rate sensor task from SD card latency.
#pragma once

#include "config.h"
#include <Arduino.h>
#include <atomic>

// One measurement. Kept small (24 bytes) so the ring buffer stays cache
// friendly.
struct Sample {
  uint32_t tUs; // microseconds since boot (wraps after ~71 min)
  int16_t ax;     // m/s^2
  int16_t ay;
  int16_t az;
  int16_t gx; // deg/s
  int16_t gy;
  int16_t gz;
};

// Lock-free SPSC ring buffer.
//
//   producer: sensorTask  -> push()
//   consumer: storageTask -> pop()
//
// head/tail are monotonically increasing counters; the index is masked on use.
// This avoids the classic "full vs empty" ambiguity and needs no locks because
// there is exactly one producer and one consumer.
class SampleRing {
public:
  SampleRing() : _head(0), _tail(0), _dropped(0) {}

  // Producer side. Returns false if the buffer is full (sample dropped).
  bool push(const Sample &s) {
    const uint32_t head = _head.load(std::memory_order_relaxed);
    const uint32_t tail = _tail.load(std::memory_order_acquire);
    if (head - tail >= RING_CAPACITY) {
      _dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _buf[head & (RING_CAPACITY - 1)] = s;
    _head.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if the buffer is empty.
  bool pop(Sample &out) {
    const uint32_t tail = _tail.load(std::memory_order_relaxed);
    const uint32_t head = _head.load(std::memory_order_acquire);
    if (tail == head) {
      return false;
    }
    out = _buf[tail & (RING_CAPACITY - 1)];
    _tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  uint32_t size() const {
    return _head.load(std::memory_order_acquire) -
           _tail.load(std::memory_order_acquire);
  }

  uint32_t dropped() const { return _dropped.load(std::memory_order_relaxed); }

  void resetDropped() { _dropped.store(0, std::memory_order_relaxed); }

private:
  Sample _buf[RING_CAPACITY];
  std::atomic<uint32_t> _head;
  std::atomic<uint32_t> _tail;
  std::atomic<uint32_t> _dropped;
};

// RING_CAPACITY must be a power of two for the mask trick above.
static_assert((RING_CAPACITY & (RING_CAPACITY - 1)) == 0,
              "RING_CAPACITY must be a power of two");

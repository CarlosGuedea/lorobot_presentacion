#pragma once
#include <cstdint>

// ISR CHANGE y poll deben usar el mismo portMUX. Retiene un pulso completo
// durante esperas HTTP. Cada pulso exige LOW 100 ms y luego HIGH 100 ms.
class ButtonInput {
 public:
  void begin(bool high, uint32_t now) {
    currentHigh = high;
    armed = high;
    queued = false;
    highSince = lowSince = now;
  }
  __attribute__((always_inline)) inline void recordLevel(bool high, uint32_t now) {
    if (high == currentHigh) return;
    if (high) {
      if (armed && static_cast<uint32_t>(now - lowSince) >= 100) latch();
      highSince = now;
    } else {
      if (static_cast<uint32_t>(now - highSince) >= 100) armed = true;
      lowSince = now;
    }
    currentHigh = high;
  }
  bool poll(uint32_t now) {
    if (!currentHigh && armed && static_cast<uint32_t>(now - lowSince) >= 100) latch();
    if (currentHigh && static_cast<uint32_t>(now - highSince) >= 100) armed = true;
    if (!queued) return false;
    pressedAt = queuedAt;
    queued = false;
    return true;
  }
  uint32_t pressedMillis() const { return pressedAt; }
  __attribute__((always_inline)) inline void discardPending() {
    queued = false;
    armed = false;
  }
 private:
  __attribute__((always_inline)) inline void latch() {
    armed = false;
    if (!queued) { queuedAt = lowSince; queued = true; }
  }
  bool currentHigh = true;
  bool armed = false;
  bool queued = false;
  uint32_t highSince = 0;
  uint32_t lowSince = 0;
  uint32_t queuedAt = 0;
  uint32_t pressedAt = 0;
};

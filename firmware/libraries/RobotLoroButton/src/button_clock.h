#pragma once
#include <cstdint>

class ButtonClock {
 public:
  bool synchronize(uint64_t serverMs, uint32_t start, uint32_t end) {
    const uint32_t rtt = end - start;
    anchorMillis = start + rtt / 2;
    anchorServer = serverMs;
    error = (rtt + 1) / 2 + 2;
    valid = rtt <= 70 && serverMs > 1000000000000ULL;
    return valid;
  }
  bool ready(uint32_t now) const {
    return valid && static_cast<uint32_t>(now - anchorMillis) < 10000 && uncertainty(now) <= 40;
  }
  uint64_t timestamp(uint32_t at) const {
    return anchorServer + static_cast<int32_t>(at - anchorMillis);
  }
  uint32_t uncertainty(uint32_t at) const {
    const int64_t distance = static_cast<int32_t>(at - anchorMillis);
    const uint32_t age = distance < 0 ? -distance : distance;
    return error + (age + 4999) / 5000;
  }
  bool readyForPress(uint32_t at, uint32_t now) const {
    return ready(now) && static_cast<uint32_t>(now - at) < 10000 && uncertainty(at) <= 40;
  }
  // Variante laxa para registrar el evento de sensores: la camara ya no se
  // empareja por hora, basta un ancla reciente aunque el RTT sea alto.
  bool anchored() const { return anchorServer > 1000000000000ULL; }
  bool usableForEvent(uint32_t at, uint32_t now, uint32_t maxErrorMs) const {
    return anchored() && static_cast<uint32_t>(now - anchorMillis) < 10000
        && static_cast<uint32_t>(now - at) < 10000 && uncertainty(at) <= maxErrorMs;
  }
 private:
  bool valid = false;
  uint32_t anchorMillis = 0;
  uint64_t anchorServer = 0;
  uint32_t error = 0;
};

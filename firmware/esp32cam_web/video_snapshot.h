#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include "mjpeg_frame.h"

using JpegAllocator = void *(*)(size_t);

class JpegSnapshot {
 public:
  uint8_t *data = nullptr;
  size_t size = 0;
  JpegSnapshot() = default;
  JpegSnapshot(uint8_t *bytes, size_t length) : data(bytes), size(length) {}
  ~JpegSnapshot() { reset(); }
  JpegSnapshot(const JpegSnapshot &) = delete;
  JpegSnapshot &operator=(const JpegSnapshot &) = delete;
  JpegSnapshot(JpegSnapshot &&other) noexcept { *this = std::move(other); }
  JpegSnapshot &operator=(JpegSnapshot &&other) noexcept {
    if (this != &other) {
      reset(); data = other.data; size = other.size;
      other.data = nullptr; other.size = 0;
    }
    return *this;
  }
  explicit operator bool() const { return data != nullptr; }
  void reset() { free(data); data = nullptr; size = 0; }
};

inline JpegSnapshot copyJpeg(const uint8_t *data, size_t bytes, JpegAllocator allocate) {
  if (!data || bytes < 4 || bytes > MAX_VIDEO_FRAME_BYTES ||
      data[0] != 0xff || data[1] != 0xd8 || data[bytes-2] != 0xff || data[bytes-1] != 0xd9) return {};
  auto *copy = static_cast<uint8_t *>(allocate(bytes));
  if (!copy) return {};
  memcpy(copy, data, bytes);
  return JpegSnapshot(copy, bytes);
}

enum class SnapshotSource { Camera, Video, Unavailable };

// Sus metodos requieren mutex externo. Memoria acotada: 8 JPEG <=64KiB.
class VideoSnapshots {
 public:
  explicit VideoSnapshots(JpegAllocator allocate) : allocate(allocate) {}
  void begin(uint32_t now) {
    for (auto &entry : entries) entry.image.reset();
    next = 0; started = now; hasSession = true; active = true;
  }
  void end(uint32_t now) { ended = now; active = false; }
  SnapshotSource sourceAt(uint32_t pressedAt) const {
    if (!hasSession) return SnapshotSource::Camera;
    if (static_cast<int32_t>(pressedAt - started) < 0) return SnapshotSource::Unavailable;
    if (!active && static_cast<int32_t>(pressedAt - ended) > 0) return SnapshotSource::Camera;
    return SnapshotSource::Video;
  }
  bool store(const uint8_t *jpeg, size_t bytes, uint32_t capturedAt, uint32_t sentAt) {
    if (!active) return false;
    auto copy = copyJpeg(jpeg, bytes, allocate);
    if (!copy) return false;
    entries[next].image = std::move(copy);
    entries[next].capturedAt = capturedAt;
    entries[next].sentAt = sentAt;
    next = (next + 1) % 8;
    return true;
  }
  JpegSnapshot copyAt(uint32_t pressedAt) const {
    if (sourceAt(pressedAt) != SnapshotSource::Video) return {};
    const Entry *chosen = nullptr;
    uint32_t bestAge = UINT32_MAX;
    for (const auto &entry : entries) {
      if (!entry.image || static_cast<int32_t>(pressedAt - entry.sentAt) < 0 ||
          static_cast<uint32_t>(pressedAt - entry.capturedAt) > 1500) continue;
      const uint32_t age = pressedAt - entry.sentAt;
      if (age < bestAge) { chosen = &entry; bestAge = age; }
    }
    return chosen ? copyJpeg(chosen->image.data, chosen->image.size, allocate) : JpegSnapshot{};
  }
 private:
  struct Entry { JpegSnapshot image; uint32_t capturedAt = 0; uint32_t sentAt = 0; };
  Entry entries[8];
  JpegAllocator allocate;
  unsigned next = 0;
  bool hasSession = false;
  bool active = false;
  uint32_t started = 0;
  uint32_t ended = 0;
};

#pragma once
#include <cstddef>
#include <cstdio>

constexpr size_t MAX_VIDEO_FRAME_BYTES = 64 * 1024;
constexpr char MJPEG_CONTENT_TYPE[] = "multipart/x-mixed-replace;boundary=robotloro-frame";

inline size_t formatMjpegPart(char *output, size_t capacity, size_t bytes) {
  if (!bytes || bytes > MAX_VIDEO_FRAME_BYTES) return 0;
  const int n = std::snprintf(output, capacity,
      "\r\n--robotloro-frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      static_cast<unsigned>(bytes));
  return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}

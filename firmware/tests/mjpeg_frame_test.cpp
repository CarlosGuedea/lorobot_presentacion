#include "../esp32cam_web/mjpeg_frame.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
  char header[160];
  const size_t n = formatMjpegPart(header, sizeof(header), 12345);
  assert(n == strlen(header));
  assert(strstr(header, "\r\n--robotloro-frame\r\n") == header);
  assert(strstr(header, "Content-Type: image/jpeg\r\n"));
  assert(strstr(header, "Content-Length: 12345\r\n\r\n"));
  assert(strstr(MJPEG_CONTENT_TYPE, "boundary=robotloro-frame"));
  assert(formatMjpegPart(header, sizeof(header), 0) == 0);
  assert(formatMjpegPart(header, sizeof(header), MAX_VIDEO_FRAME_BYTES + 1) == 0);
  assert(formatMjpegPart(header, 5, 12345) == 0);
  assert(formatMjpegPart(header, sizeof(header), MAX_VIDEO_FRAME_BYTES) > 0);
  std::cout << "MJPEG: framing y limites correctos\n";
}

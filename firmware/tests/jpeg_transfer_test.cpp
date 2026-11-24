#include "../esp32cam_web/jpeg_transfer.h"
#include <cassert>
#include <algorithm>
#include <vector>
#include <iostream>

struct PartialClient {
  size_t capacity = 37;
  int stalls = 2;
  bool online = true;
  size_t largestRequest = 0;
  std::vector<uint8_t> received;
  bool connected() { return online; }
  size_t write(const uint8_t *data, size_t length) {
    largestRequest = std::max(largestRequest, length);
    if (stalls-- > 0) return 0;
    const size_t count = std::min(capacity, length);
    received.insert(received.end(), data, data + count);
    return count;
  }
};

int main() {
  std::vector<uint8_t> jpeg(5000);
  for (size_t i = 0; i < jpeg.size(); ++i) jpeg[i] = i % 251;
  uint32_t clock = 0;
  PartialClient client;
  auto now = [&]() { return clock; };
  auto pause = [&]() { ++clock; };
  assert(sendJpegFrame(client, jpeg.data(), jpeg.size(), now, pause, 1000) == jpeg.size());
  assert(client.received == jpeg); // No perder ni repetir bytes al reintentar.
  assert(client.largestRequest <= 1024);

  PartialClient blocked;
  blocked.capacity = 0;
  clock = 0;
  assert(sendJpegFrame(blocked, jpeg.data(), jpeg.size(), now, pause, 5) == 0);
  assert(clock == 5); // No quedarse bloqueado si la red deja de aceptar datos.

  PartialClient disconnected;
  disconnected.online = false;
  clock = 0;
  assert(sendJpegFrame(disconnected, jpeg.data(), jpeg.size(), now, pause, 5) == 0);
  assert(disconnected.received.empty());

  PartialClient wrap;
  wrap.capacity = 0;
  clock = UINT32_MAX - 2;
  assert(sendJpegFrame(wrap, jpeg.data(), jpeg.size(), now, pause, 5) == 0);
  assert(clock == 2); // millis() puede desbordarse.
  std::cout << "JPEG: escrituras parciales, pausas, desconexion y timeout correctos\n";
}

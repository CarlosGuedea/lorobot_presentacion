#pragma once
#include <cstddef>
#include <cstdint>

// write() puede aceptar menos bytes que los solicitados, incluso cero.
// Reanudar desde el offset confirmado y dar tiempo al stack TCP entre bloques.
template <class Client, class Clock, class Pause>
size_t sendJpegFrame(Client &client, const uint8_t *data, size_t length,
                     Clock now, Pause pause, uint32_t timeoutMs) {
  const uint32_t start = now();
  size_t sent = 0;
  while (sent < length && client.connected() &&
         static_cast<uint32_t>(now() - start) < timeoutMs) {
    const size_t remaining = length - sent;
    const size_t chunk = remaining < 1024 ? remaining : 1024;
    const size_t written = client.write(data + sent, chunk);
    if (written > chunk) break;
    sent += written;
    pause();
  }
  return sent;
}

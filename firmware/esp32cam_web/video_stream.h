#pragma once
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <cerrno>
#include "mjpeg_frame.h"
#include "video_state.h"

// El callback entrega una copia independiente, liberable con free(). Nunca
// conservar el framebuffer ni el mutex de camara durante un envio TCP lento.
using VideoFrameCopy = bool (*)(uint8_t **, size_t *, uint32_t *);
using VideoFrameSent = void (*)(const uint8_t *, size_t, uint32_t, uint32_t);
using VideoSessionChanged = void (*)(bool, uint32_t);
static httpd_handle_t videoServer = nullptr;
static VideoFrameCopy videoFrameCopy = nullptr;
static VideoFrameSent videoFrameSent = nullptr;
static VideoSessionChanged videoSessionChanged = nullptr;
static VideoRunState videoState;

class VideoSessionScope {
 public:
  VideoSessionScope() { if (videoSessionChanged) videoSessionChanged(true, millis()); }
  ~VideoSessionScope() { if (videoSessionChanged) videoSessionChanged(false, millis()); }
};

class CameraFrameLock {
 public:
  CameraFrameLock(SemaphoreHandle_t mutex, TickType_t wait) : handle(mutex) {
    acquired = handle && xSemaphoreTake(handle, wait) == pdTRUE;
  }
  ~CameraFrameLock() { if (acquired) xSemaphoreGive(handle); }
  explicit operator bool() const { return acquired; }
  CameraFrameLock(const CameraFrameLock &) = delete;
  CameraFrameLock &operator=(const CameraFrameLock &) = delete;
 private:
  SemaphoreHandle_t handle;
  bool acquired = false;
};

static esp_err_t videoStreamHandler(httpd_req_t *request) {
  VideoSessionScope session;
  Serial.println("[video] Cliente conectado; QVGA, maximo 5 fps.");
  esp_err_t result = httpd_resp_set_type(request, MJPEG_CONTENT_TYPE);
  if (result != ESP_OK) return result;
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  uint32_t frames = 0;
  while (videoState.running(WiFi.status() == WL_CONNECTED)) {
    char probe;
    const int received = recv(httpd_req_to_sockfd(request), &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
    uint8_t *jpeg = nullptr;
    size_t bytes = 0;
    uint32_t capturedAt = 0;
    if (!videoFrameCopy || !videoFrameCopy(&jpeg, &bytes, &capturedAt)) {
      // Priorizar el envio de una foto por boton sin bloquear loop.
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    char header[160];
    const size_t length = formatMjpegPart(header, sizeof(header), bytes);
    if (!length) { free(jpeg); result = ESP_FAIL; break; }
    result = httpd_resp_send_chunk(request, header, length);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, reinterpret_cast<char *>(jpeg), bytes);
    if (result == ESP_OK && videoFrameSent) videoFrameSent(jpeg, bytes, capturedAt, millis());
    free(jpeg);
    if (result != ESP_OK) break;
    if (++frames % 25 == 0) Serial.printf("[video] %lu frames enviados\n", static_cast<unsigned long>(frames));
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  Serial.printf("[video] Cliente cerrado; frames=%lu; resultado=%d\n",
                static_cast<unsigned long>(frames), result);
  // Retornar error fuerza cierre tambien si la red desaparecio.
  return ESP_FAIL;
}

inline bool startVideoServer(VideoFrameCopy copy, VideoFrameSent sent, VideoSessionChanged sessionChanged) {
  if (videoServer) return true;
  videoFrameCopy = copy;
  videoFrameSent = sent;
  videoSessionChanged = sessionChanged;
  videoState.start();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32769;
  config.max_open_sockets = 2;
  config.lru_purge_enable = true;
  config.send_wait_timeout = 2;
  config.recv_wait_timeout = 2;
  config.stack_size = 4096;
  if (httpd_start(&videoServer, &config) != ESP_OK) {
    videoState.stop();
    videoServer = nullptr;
    return false;
  }
  httpd_uri_t route = {};
  route.uri = "/stream";
  route.method = HTTP_GET;
  route.handler = videoStreamHandler;
  if (httpd_register_uri_handler(videoServer, &route) != ESP_OK) {
    videoState.stop();
    httpd_stop(videoServer);
    videoServer = nullptr;
    return false;
  }
  return true;
}

inline void stopVideoServer() {
  videoState.stop();
  if (videoServer) { httpd_stop(videoServer); videoServer = nullptr; }
}

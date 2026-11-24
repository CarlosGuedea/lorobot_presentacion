/* Proyecto Loro - AI Thinker ESP32-CAM / OV2640.
 * Wi-Fi configurado en wifi_config.h. GET /capture devuelve JPEG bajo demanda.
 * Placa Arduino IDE: AI Thinker ESP32-CAM. Monitor: 115200 baudios.
 * No comparte pines ni sketch con el ESP32 de sensores.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "wifi_config.h"
#include "jpeg_transfer.h"
#include <RobotLoroButton.h>
#include <esp_heap_caps.h>
#include "video_stream.h"
#include "video_snapshot.h"

constexpr char HOSTNAME[] = "esp32cam";
WebServer server(80);
bool cameraReady = false;
bool networkReady = false;
uint32_t lastReconnect = 0;
size_t lastFrameBytes = 0;
size_t lastSentBytes = 0;
uint32_t photosTaken = 0;
// La camara ya no usa boton fisico: sondea al backend por fotos pendientes.
LoroButtonClient buttonClient;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
bool cameraBusy = false; // protegido por stateMux
JpegSnapshot pendingImage;
String pendingTicket;
String pendingPressId;
uint32_t pendingPressedMillis = 0;
uint32_t lastUploadAttempt = 0;
uint32_t lastPendingPoll = 0;
bool firstUploadAttempt = false;
constexpr uint32_t PENDING_POLL_MS = 700;
SemaphoreHandle_t cameraFrameMutex = nullptr;
SemaphoreHandle_t videoHistoryMutex = nullptr;

void *allocateJpeg(size_t bytes) {
  return heap_caps_malloc(bytes, psramFound() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_8BIT);
}
VideoSnapshots videoSnapshots(allocateJpeg);

void onVideoSession(bool active, uint32_t at) {
  CameraFrameLock historyLock(videoHistoryMutex, portMAX_DELAY);
  if (active) videoSnapshots.begin(at);
  else videoSnapshots.end(at);
}

void onVideoFrameSent(const uint8_t *jpeg, size_t bytes, uint32_t capturedAt, uint32_t sentAt) {
  CameraFrameLock historyLock(videoHistoryMutex, pdMS_TO_TICKS(100));
  if (historyLock && !videoSnapshots.store(jpeg, bytes, capturedAt, sentAt)) {
    Serial.println("[video] No se pudo conservar el frame; historial anterior intacto.");
  }
}

bool copyVideoFrame(uint8_t **data, size_t *length, uint32_t *capturedAt) {
  if (!cameraReady) return false;
  CameraFrameLock frameLock(cameraFrameMutex, pdMS_TO_TICKS(10));
  if (!frameLock) return false;
  portENTER_CRITICAL(&stateMux);
  const bool busy = cameraBusy;
  portEXIT_CRITICAL(&stateMux);
  if (busy) return false;
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) return false;
  uint8_t *copy = nullptr;
  if (frame->format == PIXFORMAT_JPEG && frame->len > 0 && frame->len <= MAX_VIDEO_FRAME_BYTES) {
    copy = static_cast<uint8_t *>(allocateJpeg(frame->len));
    if (copy) {
      memcpy(copy, frame->buf, frame->len); *length = frame->len;
      *capturedAt = static_cast<uint32_t>(static_cast<uint64_t>(frame->timestamp.tv_sec) * 1000
                                        + frame->timestamp.tv_usec / 1000);
    }
  }
  esp_camera_fb_return(frame);
  *data = copy;
  return copy != nullptr;
}

void clearPendingFrame() {
  pendingImage.reset();
  pendingTicket = "";
  pendingPressId = "";
  portENTER_CRITICAL(&stateMux);
  cameraBusy = false;
  portEXIT_CRITICAL(&stateMux);
}

// Toma el JPEG para el evento indicado por el backend: fotograma del video
// si hay uno reciente para ese instante; si no, foto nueva de la OV2640.
void selectPendingPhoto(const char *eventId, uint64_t triggerMs, const char *cameraKey) {
  if (pendingImage || !cameraReady) return;
  const uint32_t now = millis();
  // Convertir la hora del servidor (triggerMs) a millis() locales con el reloj.
  const bool mapped = buttonClient.clock.ready(now);
  uint32_t pressedLocal = now;
  if (mapped) {
    const int64_t age = static_cast<int64_t>(buttonClient.clock.timestamp(now))
                        - static_cast<int64_t>(triggerMs);
    if (age < -2000 || age > 55000) {
      Serial.printf("[foto] Evento %s fuera de plazo (edad=%lld ms); se omite.\n",
                    eventId, static_cast<long long>(age));
      return;
    }
    pressedLocal = now - static_cast<uint32_t>(age < 0 ? 0 : age);
  } else {
    Serial.println("[foto] Reloj no sincronizado: se tomara una foto nueva.");
  }
  pendingPressedMillis = pressedLocal;
  pendingTicket = eventId;
  pendingPressId = (cameraKey && *cameraKey) ? String(cameraKey) : buttonClient.newPressId();
  portENTER_CRITICAL(&stateMux);
  cameraBusy = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("[foto] Evento %s pendiente; hora=%llu; seleccionando JPEG.\n",
                eventId, triggerMs);
  SnapshotSource source = SnapshotSource::Camera;
  if (mapped) {
    CameraFrameLock historyLock(videoHistoryMutex, pdMS_TO_TICKS(100));
    if (historyLock) {
      source = videoSnapshots.sourceAt(pressedLocal);
      if (source == SnapshotSource::Video) pendingImage = videoSnapshots.copyAt(pressedLocal);
      if (source == SnapshotSource::Unavailable) source = SnapshotSource::Camera;
    }
  }
  if (source == SnapshotSource::Video && pendingImage) {
    Serial.println("[foto] Fotograma del video seleccionado.");
  } else {
    if (source == SnapshotSource::Video)
      Serial.println("[foto] Sin fotograma reciente del video; se toma foto nueva.");
    CameraFrameLock frameLock(cameraFrameMutex, pdMS_TO_TICKS(1000));
    if (frameLock) {
      camera_fb_t *old = esp_camera_fb_get();
      if (old) esp_camera_fb_return(old);
      camera_fb_t *frame = esp_camera_fb_get();
      if (frame) {
        pendingImage = copyJpeg(frame->buf, frame->len, allocateJpeg);
        esp_camera_fb_return(frame);
      }
    }
  }
  if (!pendingImage) {
    Serial.println("[foto] No se pudo obtener un JPEG; se reintentara en el siguiente sondeo.");
    clearPendingFrame();
    return;
  }
  ++photosTaken;
  lastFrameBytes = pendingImage.size;
  lastSentBytes = 0;
  firstUploadAttempt = true;
  Serial.printf("[foto] Lista: %u bytes; evento=%s\n",
                static_cast<unsigned>(pendingImage.size), pendingTicket.c_str());
}

// Sondeo al backend: reemplaza al boton fisico en la camara. El ESP32 de
// sensores sigue registrando la pulsacion; aqui solo se atiende el pedido.
void pollPendingPhoto() {
  if (pendingImage || !cameraReady || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastPendingPoll) < PENDING_POLL_MS) return;
  lastPendingPoll = now;
  const String base = buttonClient.baseUrl();
  if (base.isEmpty()) return;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(1500);
  if (!http.begin(client, base + "/api/button-events/pending-photo")) return;
  const int code = http.GET();
  const String response = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("[foto] Sondeo pendiente HTTP=%d\n", code);
    return;
  }
  JsonDocument reply;
  if (deserializeJson(reply, response) || !reply["pending"].as<bool>()) return;
  const char *eventId = reply["event_id"].as<const char *>();
  const uint64_t triggerMs = reply["trigger_ms"].as<uint64_t>();
  if (!eventId || !*eventId || triggerMs == 0) return;
  selectPendingPhoto(eventId, triggerMs, reply["camera_key"].as<const char *>());
}

void uploadPendingPhoto() {
  if (!pendingImage) return;
  if (static_cast<uint32_t>(millis() - pendingPressedMillis) >= 55000) {
    Serial.println("[upload] Foto vencida sin confirmacion; no se tomara otra al reintentar.");
    clearPendingFrame();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (!firstUploadAttempt && static_cast<uint32_t>(millis() - lastUploadAttempt) < 5000) return;
  firstUploadAttempt = false;
  lastUploadAttempt = millis();
  const String base = buttonClient.baseUrl();
  if (base.isEmpty()) return;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(4000);
  if (!http.begin(client, base + "/api/button-events/" + pendingTicket + "/photo")) return;
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Press-Id", pendingPressId);
  const uint32_t start = millis();
  const int code = http.POST(pendingImage.data, pendingImage.size);
  const String response = http.getString();
  JsonDocument reply;
  const bool confirmed = (code == 200 || code == 201) && !deserializeJson(reply, response)
      && reply["saved"].as<bool>() && reply["event_id"].as<String>() == pendingTicket;
  Serial.printf("[upload] HTTP=%d; confirmado=%s; tiempo=%lu ms; evento=%s\n",
                code, confirmed ? "SI" : "NO", static_cast<unsigned long>(millis() - start),
                pendingTicket.c_str());
  http.end();
  if (confirmed) {
    lastSentBytes = pendingImage.size;
    clearPendingFrame();
    Serial.println("[upload] JPEG confirmado en MySQL.");
  } else if (code == 409 || code == 400 || code == 413 || code == 415) {
    Serial.println(response);
    clearPendingFrame();
  } else {
    Serial.println("[upload] Se conserva el mismo JPEG para reintentar.");
  }
}

const char HOME[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Proyecto Loro · Cámara</title><style>body{font:16px system-ui;max-width:800px;margin:40px auto;padding:20px}button{font:inherit;padding:12px 20px}img{display:block;max-width:100%;margin-top:20px}</style>
<h1>Proyecto Loro</h1><p>ESP32-CAM · Foto y video en vivo</p>
<button id="video">Iniciar video</button><p id="videoStatus" role="status">Video detenido. Se pausa mientras se envia una foto al backend.</p><img id="live" alt="Video en vivo" hidden>
<button id="take">Tomar foto</button><p id="status" role="status">No se ha solicitado una foto.</p><img id="photo" alt="Foto capturada" hidden>
<script>let streaming=false;const live=document.querySelector('#live'),video=document.querySelector('#video'),vs=document.querySelector('#videoStatus');video.onclick=()=>{streaming=!streaming;if(streaming){let u=new URL('/stream',location.href);u.port='81';u.searchParams.set('t',Date.now());live.src=u.href;live.hidden=false;video.textContent='Detener video';vs.textContent='Conectando al video…';}else{live.removeAttribute('src');live.hidden=true;video.textContent='Iniciar video';vs.textContent='Video detenido.';}};live.onload=()=>{if(streaming)vs.textContent='Video en vivo · 320x240 · hasta 5 fps';};live.onerror=()=>{if(streaming){streaming=false;live.removeAttribute('src');live.hidden=true;video.textContent='Iniciar video';vs.textContent='No se pudo recibir video. Revisa la conexion de la camara.';}};let url;document.querySelector('#take').onclick=async()=>{let b=document.querySelector('#take'),s=document.querySelector('#status');b.disabled=true;s.textContent='Tomando foto…';try{let r=await fetch('/capture',{cache:'no-store'});if(!r.ok)throw Error('No se pudo capturar la imagen');let next=URL.createObjectURL(await r.blob());let i=document.querySelector('#photo');i.src=next;i.hidden=false;if(url)URL.revokeObjectURL(url);url=next;s.textContent='Foto recibida.';}catch(e){s.textContent=e.message;}finally{b.disabled=false;}};</script></html>)HTML";

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  // Pinout oficial AI Thinker.
  config.pin_d0 = 5; config.pin_d1 = 18;
  config.pin_d2 = 19; config.pin_d3 = 21;
  config.pin_d4 = 36; config.pin_d5 = 39;
  config.pin_d6 = 34; config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  // Reducir el trafico mientras se verifica la estabilidad del enlace movil.
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 15;
  // Un único buffer para descartar la imagen retenida antes de una nueva captura.
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("Error al iniciar OV2640: 0x%x. Revisa flex y alimentacion.\n", error);
    return false;
  }
  Serial.println("OV2640 iniciada.");
  return true;
}

void capturePhoto() {
  if (pendingImage) {
    server.send(409, "text/plain", "Foto pendiente de envio al backend; no se reemplazara.");
    return;
  }
  const uint32_t started = millis();
  Serial.printf("[capture %lu] Peticion recibida; RSSI=%d; RAM=%u\n",
                static_cast<unsigned long>(started), WiFi.RSSI(), ESP.getFreeHeap());
  if (!cameraReady) {
    server.send(503, "text/plain", "Camara no disponible; revisa el monitor serie.");
    return;
  }
  CameraFrameLock frameLock(cameraFrameMutex, pdMS_TO_TICKS(1000));
  if (!frameLock) {
    server.send(503, "text/plain", "Camara ocupada; vuelve a intentar.");
    return;
  }
  // Descartar el buffer anterior: la foto entregada debe adquirirse tras la petición.
  camera_fb_t *old = esp_camera_fb_get();
  Serial.printf("[capture +%lu ms] Buffer anterior: %s\n",
                static_cast<unsigned long>(millis() - started), old ? "OK" : "NULL");
  if (old) esp_camera_fb_return(old);
  camera_fb_t *frame = esp_camera_fb_get();
  Serial.printf("[capture +%lu ms] Foto nueva: %s; bytes=%u\n",
                static_cast<unsigned long>(millis() - started), frame ? "OK" : "NULL",
                frame ? static_cast<unsigned>(frame->len) : 0);
  if (!frame) {
    server.send(503, "text/plain", "No fue posible adquirir la foto.");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(frame->len);
  Serial.println("[capture] Enviando cabeceras HTTP.");
  server.send(200, "image/jpeg", "");
  Serial.printf("[capture +%lu ms] Cabeceras enviadas; iniciando JPEG.\n",
                static_cast<unsigned long>(millis() - started));
  WiFiClient client = server.client();
  client.setTimeout(5);
  const size_t sent = sendJpegFrame(client, frame->buf, frame->len,
                                  []() -> uint32_t { return millis(); },
                                  []() { delay(1); }, 15000);
  lastFrameBytes = frame->len;
  lastSentBytes = sent;
  ++photosTaken;
  Serial.printf("Foto: %u/%u bytes enviados\n", (unsigned)sent, (unsigned)frame->len);
  if (sent != frame->len) {
    Serial.println("Transferencia JPEG incompleta: conexion cerrada o tiempo agotado.");
    client.stop();
  }
  esp_camera_fb_return(frame);
  Serial.printf("[capture +%lu ms] Fin de peticion.\n",
                static_cast<unsigned long>(millis() - started));
}

void setup() {
  Serial.begin(115200);
  cameraFrameMutex = xSemaphoreCreateMutex();
  videoHistoryMutex = xSemaphoreCreateMutex();
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);  // Flash apagado.
  cameraReady = cameraFrameMutex && videoHistoryMutex && initCamera();
  Serial.printf("PSRAM: %s; libre: %u bytes; RAM libre: %u bytes\n",
                psramFound() ? "SI" : "NO", ESP.getFreePsram(), ESP.getFreeHeap());
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      Serial.printf("Wi-Fi asociado a %s; esperando IP por DHCP.\n", WIFI_SSID);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("Wi-Fi IP: %s; gateway: %s; RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("Wi-Fi desconectado: motivo=%u; estado=%d\n",
                    info.wifi_sta_disconnected.reason, static_cast<int>(WiFi.status()));
    }
  });
  WiFi.mode(WIFI_STA);
  buttonClient.begin("camera", BACKEND_HOST, BACKEND_PORT);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (WIFI_PASSWORD[0]) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  else WiFi.begin(WIFI_SSID);
  // Como en CameraWebServer de Espressif: evita latencia por ahorro Wi-Fi
  // durante la transmision de JPEG, especialmente con puntos de acceso moviles.
  WiFi.setSleep(false);
  Serial.printf("Conectando a red %s...\n", WIFI_SSID);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", HOME); });
  server.on("/capture", HTTP_GET, capturePhoto);
  server.on("/health", HTTP_GET, []() {
    Serial.printf("[health %lu] Peticion recibida.\n", static_cast<unsigned long>(millis()));
    char status[280];
    snprintf(status, sizeof(status),
             "{\"camera_ready\":%s,\"uptime_ms\":%lu,\"rssi_dbm\":%d,"
             "\"resolution\":\"320x240\",\"transport\":\"chunked-v1\","
             "\"photos_taken\":%lu,\"last_frame_bytes\":%u,\"last_sent_bytes\":%u}",
             cameraReady ? "true" : "false", static_cast<unsigned long>(millis()),
             WiFi.RSSI(), static_cast<unsigned long>(photosTaken),
             static_cast<unsigned>(lastFrameBytes), static_cast<unsigned>(lastSentBytes));
    server.send(cameraReady ? 200 : 503, "application/json", status);
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Ruta no encontrada"); });
}

void loop() {
  pollPendingPhoto();
  uploadPendingPhoto();
  static uint32_t lastDiagnostic = 0;
  if (static_cast<uint32_t>(millis() - lastDiagnostic) >= 5000) {
    lastDiagnostic = millis();
    Serial.printf("[loop %lu] WiFi=%d; RSSI=%d; RAM=%u\n",
                  static_cast<unsigned long>(millis()), static_cast<int>(WiFi.status()),
                  WiFi.RSSI(), ESP.getFreeHeap());
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!networkReady) {
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
      else Serial.println("mDNS no disponible; usa la IP.");
      server.begin();
      if (startVideoServer(copyVideoFrame, onVideoFrameSent, onVideoSession)) {
        Serial.printf("Video MJPEG: http://%s:81/stream\n", WiFi.localIP().toString().c_str());
      } else Serial.println("[video] No se pudo iniciar el servidor.");
      networkReady = true;
      Serial.printf("Camara web: http://%s/ | http://%s.local/\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    }
    if (!pendingImage) buttonClient.pollClock();
    server.handleClient();
  } else {
    if (networkReady) {
      server.stop();
      stopVideoServer();
      MDNS.end();
      networkReady = false;
      Serial.println("Wi-Fi desconectado.");
    }
    if ((uint32_t)(millis() - lastReconnect) >= 10000) {
      lastReconnect = millis();
      Serial.printf("Reintentando conexion a %s...\n", WIFI_SSID);
      WiFi.reconnect();
    }
  }
  delay(2);
}

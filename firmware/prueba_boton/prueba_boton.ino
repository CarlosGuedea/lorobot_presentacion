// Una medicion por pulsacion. ESP32 Dev Module, Arduino-ESP32 2.0.17.
// Boton: GPIO2 a GND. DHT11: DATA GPIO32.
// INMP441: SCK 26, WS 25, SD 33, L/R GND, VDD 3V3, GND comun.
// Monitor serie: 115200. Boton compartido: sensores y foto se envian por separado.
#include <Arduino.h>
#include <DHT.h>
#include <driver/i2s.h>
#include <driver/gpio.h>
#include "audio_metrics.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "wifi_config.h"
#include <RobotLoroButton.h>
#include <soc/gpio_struct.h>

constexpr int PIN_BOTON = 2;
// Clasificacion relativa y ajustable; no es un limite ambiental en dBA.
constexpr float UMBRAL_RUIDO_DBFS = -35.0f;
constexpr uint32_t FRECUENCIA_AUDIO = 16000;
constexpr i2s_port_t PUERTO_AUDIO = I2S_NUM_0;
constexpr int PIN_DHT = 32;
DHT dht(PIN_DHT, DHT11);

unsigned long ultimaLecturaDht;
String deviceId;
String bootId;
uint32_t eventCounter = 0;
String pendingMeasurement;
String pendingEventId;
String lastReading;
uint32_t lastReadingMs = 0;
uint32_t lastSendAttempt = 0;
uint32_t lastWifiAttempt = 0;
bool networkReady = false;
WebServer sensorServer(80);
Preferences pendingStore;
ButtonInput sharedButton;
LoroButtonClient buttonClient;
portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onButtonChange() {
  portENTER_CRITICAL_ISR(&buttonMux);
  sharedButton.recordLevel((GPIO.in & (1UL << PIN_BOTON)) != 0, millis());
  portEXIT_CRITICAL_ISR(&buttonMux);
}

void sendPending() {
  lastSendAttempt = millis();
  if (pendingMeasurement.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  IPAddress backendIp;
  if (!backendIp.fromString(BACKEND_HOST)) backendIp = MDNS.queryHost(BACKEND_HOST, 2000);
  if (backendIp == IPAddress()) {
    Serial.println("Backend no localizado. La muestra sigue pendiente en RAM.");
    return;
  }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  const String url = "http://" + backendIp.toString() + ":" + String(BACKEND_PORT) + "/api/measurements";
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(pendingMeasurement);
  JsonDocument reply;
  const bool valid = (code == 200 || code == 201) && !deserializeJson(reply, http.getString());
  if (valid && reply["saved"].as<bool>() && reply["measurement"]["event_id"].as<String>() == pendingEventId) {
    Serial.printf("Medicion guardada en MySQL: %s\n", pendingEventId.c_str());
    const String photoStatus = reply["measurement"]["photo_status"].as<String>();
    if (photoStatus == "ok") Serial.println("Fotografia guardada en MySQL.");
    else if (photoStatus == "pending") Serial.println("Sensores guardados; foto pendiente del envio de la camara.");
    else Serial.println("Sensores guardados; fotografia no disponible.");
    pendingMeasurement = "";
    pendingEventId = "";
    pendingStore.remove("pending");
  } else {
    Serial.printf("Guardado NO confirmado (HTTP %d). Se reintentara con el mismo ID.\n", code);
  }
  http.end();
}

void handleNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!networkReady) {
      if (!MDNS.begin("esp32sensores")) Serial.println("No se pudo iniciar mDNS.");
      sensorServer.begin();
      networkReady = true;
      Serial.printf("Wi-Fi conectado. Sensores: http://%s/readings\n", WiFi.localIP().toString().c_str());
    }
    sensorServer.handleClient();
    buttonClient.pollClock();
    if (!pendingMeasurement.isEmpty() && static_cast<uint32_t>(millis() - lastSendAttempt) >= 10000) sendPending();
  } else {
    if (networkReady) {
      sensorServer.stop();
      MDNS.end();
      networkReady = false;
    }
    if (static_cast<uint32_t>(millis() - lastWifiAttempt) >= 10000) {
      lastWifiAttempt = millis();
      WiFi.reconnect();
    }
  }
}

bool medirAudio(AudioMetrics &resultado) {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = FRECUENCIA_AUDIO;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // Slot derecho: seleccion comprobada con el cableado de este proyecto.
  config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  if (i2s_driver_install(PUERTO_AUDIO, &config, 0, nullptr) != ESP_OK) {
    Serial.println("Error: no se pudo iniciar I2S.");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = 26;
  pins.ws_io_num = 25;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = 33;
  if (i2s_set_pin(PUERTO_AUDIO, &pins) != ESP_OK) {
    i2s_driver_uninstall(PUERTO_AUDIO);
    Serial.println("Error: no se pudieron configurar los pines I2S.");
    return false;
  }
  gpio_pulldown_en(GPIO_NUM_33);

  // Descarta 400 ms de arranque y mide el siguiente segundo de audio.
  constexpr size_t DESCARTAR = FRECUENCIA_AUDIO * 4 / 10;
  constexpr size_t TOTAL = DESCARTAR + FRECUENCIA_AUDIO;
  size_t recibidas = 0;
  int32_t buffer[256];
  AudioAccumulator acumulador;
  const uint32_t inicio = millis();
  bool correcto = true;
  while (recibidas < TOTAL) {
    if (static_cast<uint32_t>(millis() - inicio) > 4000) {
      correcto = false;
      break;
    }
    size_t bytes = 0;
    if (i2s_read(PUERTO_AUDIO, buffer, sizeof(buffer), &bytes,
                 pdMS_TO_TICKS(100)) != ESP_OK) {
      correcto = false;
      break;
    }
    for (size_t i = 0; i < bytes / sizeof(int32_t) && recibidas < TOTAL; ++i) {
      if (recibidas >= DESCARTAR) acumulador.add(buffer[i]);
      ++recibidas;
    }
  }
  i2s_driver_uninstall(PUERTO_AUDIO);
  if (!correcto) {
    Serial.println("Error: no se completo la captura de audio.");
    return false;
  }
  resultado = acumulador.finish();
  return true;
}

void tomarMedicion(uint32_t pressedMillis) {
  if (!pendingMeasurement.isEmpty()) {
    sendPending();
    if (!pendingMeasurement.isEmpty()) {
      Serial.println("Hay una medicion pendiente de guardar. No se sobrescribira.");
      return;
    }
  }
  Serial.println("\n--- Nueva medicion ---");
  const String pressId = buttonClient.newPressId();
  // La camara ya no se empareja por hora: la marca solo guia la eleccion del
  // fotograma (tolera 1,5 s). Sin reloj, triggerMs=0 y el backend estampa la
  // hora del servidor: toda pulsacion obtiene ticket y foto.
  uint64_t triggerMs = 0;
  uint32_t error = 2000;
  if (buttonClient.clock.usableForEvent(pressedMillis, millis(), 2000)) {
    triggerMs = buttonClient.clock.timestamp(pressedMillis);
    error = buttonClient.clock.uncertainty(pressedMillis);
    if (!buttonClient.clock.ready(millis()))
      Serial.printf("Reloj impreciso (error=%lu ms); se registra igual para no perder la foto.\n",
                    static_cast<unsigned long>(error));
  } else {
    Serial.println("Reloj no disponible: el backend estampara la hora del servidor.");
  }
  String ticket = buttonClient.registerPress(pressId, triggerMs, error);
  if (ticket.isEmpty()) ticket = buttonClient.registerPress(pressId, triggerMs, error);
  if (ticket.isEmpty()) Serial.println("Sin ticket compartido: se guardaran sensores sin asociar una foto.");
  // Espera conservadora de 2 s entre lecturas y al arrancar.
  const uint32_t transcurrido = millis() - ultimaLecturaDht;
  if (transcurrido < 2000) delay(2000 - transcurrido);
  const float humedad = dht.readHumidity(true);
  const float temperatura = dht.readTemperature();
  ultimaLecturaDht = millis();
  if (isnan(humedad) || isnan(temperatura)) {
    Serial.println("DHT11: lectura no valida; el audio se medira por separado.");
  } else {
    Serial.printf("Temperatura: %.1f C\nHumedad: %.1f %%\n", temperatura, humedad);
  }
  Serial.println("Midiendo audio durante 1 segundo...");
  AudioMetrics audio;
  const bool audioOk = medirAudio(audio) && audio.valid;
  if (audioOk) {
    Serial.printf("Nivel de audio: %.1f dBFS\nRMS: %.8f\n", audio.dbfs, audio.rms);
    Serial.printf("Ruido: %s (relativo al umbral de %.1f dBFS)\n",
                  audio.dbfs >= UMBRAL_RUIDO_DBFS ? "ALTO" : "BAJO", UMBRAL_RUIDO_DBFS);
    Serial.printf("Saturacion: %s\n", audio.clipped ? "SI" : "NO");
    Serial.println("Nivel digital sin calibracion SPL; no es dBA.");
  } else {
    Serial.println("INMP441: sin lectura valida; se guardara como fallo de sensor.");
  }
  JsonDocument reading;
  ++eventCounter;
  pendingEventId = ticket.isEmpty() ? deviceId + "-" + bootId + "-" + String(eventCounter) : ticket;
  reading["event_id"] = pendingEventId;
  reading["photo_mode"] = ticket.isEmpty() ? "none" : "push";
  if (!ticket.isEmpty()) reading["press_id"] = pressId;
  reading["device_id"] = deviceId;
  reading["uptime_ms"] = millis();
  const bool dhtOk = !isnan(temperatura) && !isnan(humedad);
  reading["dht_ok"] = dhtOk;
  if (dhtOk) {
    reading["temperature_c"] = temperatura;
    reading["humidity_pct"] = humedad;
  } else {
    reading["temperature_c"] = nullptr;
    reading["humidity_pct"] = nullptr;
  }
  reading["sound_ok"] = audioOk;
  if (audioOk) {
    reading["sound_rms"] = audio.rms;
    reading["sound_dbfs"] = audio.dbfs;
    reading["sound_peak_dbfs"] = audio.dbfs;
  } else {
    reading["sound_rms"] = nullptr;
    reading["sound_dbfs"] = nullptr;
    reading["sound_peak_dbfs"] = nullptr;
  }
  reading["sound_threshold_dbfs"] = UMBRAL_RUIDO_DBFS;
  reading["sound_clipped"] = audioOk && audio.clipped;
  reading["audio_seconds"] = 1;
  reading["sound_source"] = "inmp441";
  reading["sound_level"] = nullptr;
  reading["sound_detected"] = audioOk && audio.dbfs >= UMBRAL_RUIDO_DBFS;
  reading["sound_activity_count"] = eventCounter;
  lastReadingMs = millis();
  pendingMeasurement = "";
  serializeJson(reading, pendingMeasurement);
  if (!pendingStore.putString("pending", pendingMeasurement)) {
    Serial.println("No se pudo respaldar el pendiente en flash; permanece en RAM.");
  }
  lastReading = pendingMeasurement;
  Serial.printf("Enviando sensores sin esperar la foto, evento %s\n", pendingEventId.c_str());
  sendPending();
  Serial.println("Medicion terminada. Suelta y vuelve a pulsar para medir otra vez.");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  sharedButton.begin(digitalRead(PIN_BOTON) == HIGH, millis());
  attachInterrupt(digitalPinToInterrupt(PIN_BOTON), onButtonChange, CHANGE);
  dht.begin();
  ultimaLecturaDht = millis();

  pendingStore.begin("loro-event", false);
  pendingMeasurement = pendingStore.getString("pending", "");
  if (!pendingMeasurement.isEmpty()) {
    JsonDocument pending;
    if (!deserializeJson(pending, pendingMeasurement)) {
      pendingEventId = pending["event_id"].as<String>();
      Serial.println("Hay una medicion pendiente del arranque anterior; se reintentara.");
    } else {
      Serial.println("Pendiente en flash ilegible; no se puede reenviar.");
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32sensores");
  WiFi.setAutoReconnect(true);
  buttonClient.begin("sensors", BACKEND_HOST, BACKEND_PORT);
  deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  char boot[17];
  snprintf(boot, sizeof(boot), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  bootId = boot;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Conectando a %s. Backend: %s:%u\n", WIFI_SSID, BACKEND_HOST, BACKEND_PORT);
  sensorServer.on("/readings", HTTP_GET, []() {
    if (lastReading.isEmpty()) {
      sensorServer.send(503, "application/json", "{\"error\":\"Presiona el boton para medir.\"}");
      return;
    }
    JsonDocument reading;
    deserializeJson(reading, lastReading);
    const uint32_t age = millis() - lastReadingMs;
    reading["sample_age_ms"] = age;
    reading["sound_age_ms"] = age;
    reading["uptime_ms"] = millis();
    if (age > 5000) {
      reading["sound_ok"] = false;
      reading["sound_rms"] = nullptr;
      reading["sound_dbfs"] = nullptr;
      reading["sound_peak_dbfs"] = nullptr;
    }
    String response;
    serializeJson(reading, response);
    sensorServer.sendHeader("Cache-Control", "no-store");
    sensorServer.send(200, "application/json", response);
  });

  Serial.println("\nSensores listos para medir con el boton GPIO2 a GND.");
  Serial.println("Presiona el boton para tomar una medicion.");
}

void loop() {
  portENTER_CRITICAL(&buttonMux);
  const bool pressed = sharedButton.poll(millis());
  const uint32_t pressedAt = sharedButton.pressedMillis();
  portEXIT_CRITICAL(&buttonMux);
  if (pressed) tomarMedicion(pressedAt);
  handleNetwork();
  delay(1);
}

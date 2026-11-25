/* Proyecto Loro - ESP32 DevKit / ESP32-WROOM clasico.
 * DHT22 DATA -> GPIO32 (pull-up 10 kOhm a 3V3 si hace falta).
 * INMP441: SCK->26, WS->25, SD->33, L/R->GND, VDD->3V3 (microphone.h).
 * Wi-Fi y backend se configuran en wifi_config.h. GPIO2 a 3V3 registra la foto.
 * TFT ST7735: SCK->18, SDA/MOSI->23, A0/DC->16, CS->5, RESET->17, LED->3V3.
 * GET /readings entrega la ultima muestra.
 * Monitor serie y carga: 115200. Audio I2S en dBFS; no es dB SPL calibrado.
 */
#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <RobotLoroButton.h>
#include <soc/gpio_struct.h>
#include "microphone.h"
#include "wifi_config.h"

constexpr uint8_t PIN_DHT = 32;
constexpr uint8_t PIN_BUTTON = 2;
constexpr uint8_t PIN_TFT_CS = 5;
constexpr uint8_t PIN_TFT_DC = 16;
constexpr uint8_t PIN_TFT_RST = 17;
constexpr uint32_t INTERVALO_REPORTE_MS = 2500;
constexpr char HOSTNAME[] = "esp32sensores";
DHT dht(PIN_DHT, DHT22);
WebServer server(80);
Adafruit_ST7735 display(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
constexpr uint16_t COLOR_EYES = 0x3DFF;
constexpr uint16_t COLOR_GOOD = 0x07E0;
constexpr uint16_t COLOR_BAD = 0xF800;
constexpr uint32_t DISPLAY_RESULT_MS = 10000;
uint32_t resultadoPantallaMs = 0;
bool mostrandoResultado = false;

void drawEyes() {
  display.fillScreen(ST77XX_BLACK);
  for (uint8_t eye = 0; eye < 2; ++eye) {
    const int16_t centerX = eye == 0 ? 52 : 108;
    display.fillCircle(centerX, 64, 24, COLOR_EYES);
    display.fillCircle(centerX + 3, 64, 10, ST77XX_BLACK);
    display.fillCircle(centerX + 7, 60, 4, ST77XX_WHITE);
  }
}

void drawHeart() {
  display.fillScreen(ST77XX_BLACK);
  display.fillCircle(66, 53, 18, COLOR_GOOD);
  display.fillCircle(94, 53, 18, COLOR_GOOD);
  display.fillTriangle(48, 61, 112, 61, 80, 96, COLOR_GOOD);
}

void drawCross() {
  display.fillScreen(ST77XX_BLACK);
  for (int8_t offset = -4; offset <= 4; ++offset) {
    display.drawLine(56 + offset, 40, 104 + offset, 88, COLOR_BAD);
    display.drawLine(104 + offset, 40, 56 + offset, 88, COLOR_BAD);
  }
}

void updateDisplay() {
  const String body = server.arg("plain");
  if (body == "{\"state\":\"good\"}") {
    drawHeart();
    mostrandoResultado = true;
    resultadoPantallaMs = millis();
  } else if (body == "{\"state\":\"bad\"}") {
    drawCross();
    mostrandoResultado = true;
    resultadoPantallaMs = millis();
  } else {
    server.send(400, "application/json", "{\"error\":\"Estado de pantalla invalido\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

uint32_t ultimoReporteMs = 0;
uint32_t muestraMs = 0;
uint32_t ultimoReintentoMs = 0;
uint32_t actividadSonido = 0;
bool sonidoMuestra = false;
bool hayMuestra = false;
bool dhtValido = false;
bool redLista = false;
AudioSnapshot audio = {};
float temperatura = NAN;
float humedad = NAN;
ButtonInput sharedButton;
LoroButtonClient buttonClient;
portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;
String pendingPressId;
uint64_t pendingTriggerMs = 0;
uint32_t pendingPressAt = 0;
uint32_t pendingClockErrorMs = 2000;
uint32_t lastButtonAttemptMs = 0;

void IRAM_ATTR onButtonChange() {
  portENTER_CRITICAL_ISR(&buttonMux);
  const bool physicalHigh = (GPIO.in & (1UL << PIN_BUTTON)) != 0;
  sharedButton.recordLevel(!physicalHigh, millis());
  portEXIT_CRITICAL_ISR(&buttonMux);
}

String readingsJson() {
  String json;
  json.reserve(560);
  const uint32_t audioAge = audio.ageMs + static_cast<uint32_t>(millis() - muestraMs);
  const bool audioOk = audio.ok && audioAge <= 5000;
  json = "{\"temperature_c\":";
  json += dhtValido ? String(temperatura, 1) : "null";
  json += ",\"humidity_pct\":";
  json += dhtValido ? String(humedad, 1) : "null";
  json += ",\"dht_ok\":";
  json += dhtValido ? "true" : "false";
  json += ",\"sound_detected\":";
  json += (audioOk && sonidoMuestra) ? "true" : "false";
  json += ",\"sound_source\":\"inmp441\",\"sound_level\":null,\"sound_ok\":";
  json += audioOk ? "true" : "false";
  json += ",\"sound_rms\":";
  json += audioOk ? String(audio.rms, 8) : "null";
  json += ",\"sound_dbfs\":";
  json += audioOk ? String(audio.dbfs, 1) : "null";
  json += ",\"sound_peak_dbfs\":";
  json += audioOk ? String(audio.peakDbfs, 1) : "null";
  json += ",\"sound_threshold_dbfs\":";
  json += String(SOUND_THRESHOLD_DBFS, 1);
  json += ",\"sound_clipped\":";
  json += (audioOk && audio.clipped) ? "true" : "false";
  json += ",\"sound_age_ms\":";
  json += audioAge;
  json += ",\"sound_activity_count\":";
  json += actividadSonido;
  json += ",\"sample_age_ms\":";
  json += (uint32_t)(millis() - muestraMs);
  json += ",\"uptime_ms\":";
  json += millis();
  json += "}";
  return json;
}

void sendReadings() {
  server.sendHeader("Cache-Control", "no-store");
  if (!hayMuestra) {
    server.send(503, "application/json", "{\"error\":\"Esperando la primera muestra\"}");
    return;
  }
  server.send(200, "application/json", readingsJson());
}

void handleButton() {
  bool pressed;
  uint32_t pressedAt;
  portENTER_CRITICAL(&buttonMux);
  pressed = sharedButton.poll(millis());
  pressedAt = sharedButton.pressedMillis();
  portEXIT_CRITICAL(&buttonMux);
  if (pressed && pendingPressId.isEmpty()) {
    pendingPressId = buttonClient.newPressId();
    pendingPressAt = millis();
    pendingTriggerMs = 0;
    pendingClockErrorMs = 2000;
    if (buttonClient.clock.usableForEvent(pressedAt, millis(), 2000)) {
      pendingTriggerMs = buttonClient.clock.timestamp(pressedAt);
      pendingClockErrorMs = buttonClient.clock.uncertainty(pressedAt);
    }
    Serial.println("Boton GPIO2: solicitando foto a la ESP32-CAM.");
  }
  if (pendingPressId.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - pendingPressAt) >= 55000) {
    Serial.println("La solicitud de foto vencio sin conexion al backend.");
    pendingPressId = "";
    return;
  }
  if (static_cast<uint32_t>(now - lastButtonAttemptMs) < 1500) return;
  lastButtonAttemptMs = now;
  const String eventId = buttonClient.registerPress(pendingPressId, pendingTriggerMs,
                                                     pendingClockErrorMs);
  if (!eventId.isEmpty()) {
    Serial.printf("Foto solicitada. Evento: %s\n", eventId.c_str());
    pendingPressId = "";
  }
}

void setup() {
  Serial.begin(115200);
  display.initR(INITR_BLACKTAB);
  display.setRotation(1);
  drawEyes();
  if (!beginMicrophone()) Serial.println("INMP441: fallo de inicializacion I2S.");
  dht.begin();
  ultimoReporteMs = millis();
  pinMode(PIN_BUTTON, INPUT_PULLDOWN);
  // ButtonInput usa logica activa en bajo; invertir GPIO2 para el boton a 3V3.
  sharedButton.begin(digitalRead(PIN_BUTTON) == LOW, millis());
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonChange, CHANGE);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (WIFI_PASSWORD[0]) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  else WiFi.begin(WIFI_SSID);
  buttonClient.begin("sensors", BACKEND_HOST, BACKEND_PORT);
  server.on("/", HTTP_GET, sendReadings);
  server.on("/readings", HTTP_GET, sendReadings);
  server.on("/display", HTTP_POST, updateDisplay);
  server.onNotFound([]() { server.send(404, "application/json", "{\"error\":\"Ruta no encontrada\"}"); });
  Serial.println("Proyecto Loro: ESP32 de sensores, DHT22 GPIO32 e INMP441 I2S");
  Serial.printf("Conectando a %s. Primera muestra en 2.5 segundos.\n", WIFI_SSID);
}

void loop() {
  const uint32_t ahora = millis();
  if (mostrandoResultado && (uint32_t)(ahora - resultadoPantallaMs) >= DISPLAY_RESULT_MS) {
    drawEyes();
    mostrandoResultado = false;
  }
  if ((uint32_t)(ahora - ultimoReporteMs) >= INTERVALO_REPORTE_MS) {
    ultimoReporteMs = ahora;
    humedad = dht.readHumidity();
    temperatura = dht.readTemperature();
    dhtValido = isfinite(humedad) && isfinite(temperatura)
                && humedad >= 0 && humedad <= 100 && temperatura >= -40 && temperatura <= 80;
    audio = takeAudioSnapshot();
    sonidoMuestra = audio.detected;
    if (sonidoMuestra) ++actividadSonido;
    muestraMs = millis();
    hayMuestra = true;
    Serial.println(readingsJson());
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!redLista) {
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
      else Serial.println("mDNS no disponible; usa la IP.");
      server.begin();
      redLista = true;
      Serial.printf("Sensores: http://%s/readings | http://%s.local/readings\n",
                    WiFi.localIP().toString().c_str(), HOSTNAME);
    }
    server.handleClient();
    buttonClient.pollClock();
    handleButton();
  } else {
    if (redLista) {
      server.stop();
      MDNS.end();
      redLista = false;
      Serial.println("Wi-Fi desconectado.");
    }
    if ((uint32_t)(millis() - ultimoReintentoMs) >= 10000) {
      ultimoReintentoMs = millis();
      WiFi.reconnect();
      Serial.printf("Reintentando conexion a %s...\n", WIFI_SSID);
    }
    handleButton();
  }
  delay(1);
}

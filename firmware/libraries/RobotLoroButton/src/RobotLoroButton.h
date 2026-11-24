#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "button_input.h"
#include "button_clock.h"

class LoroButtonClient {
 public:
  ButtonClock clock;
  void begin(const char *deviceRole, const char *backendHost, uint16_t backendPort) {
    role = deviceRole;
    host = backendHost;
    port = backendPort;
    prefix = WiFi.macAddress();
    prefix.replace(":", "");
    char boot[18];
    snprintf(boot, sizeof(boot), "-%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    prefix += boot;
  }
  String baseUrl() const {
    IPAddress ip;
    if (!ip.fromString(host)) ip = MDNS.queryHost(host, 1000);
    if (ip == IPAddress()) return "";
    return "http://" + ip.toString() + ":" + String(port);
  }
  void pollClock() {
    const uint32_t now = millis();
    const uint32_t interval = clock.ready(now) ? 2000 : 1000;
    if (attempted && static_cast<uint32_t>(now - lastClockAttempt) < interval) return;
    if (WiFi.status() != WL_CONNECTED) return;
    lastClockAttempt = now;
    attempted = true;
    const String base = baseUrl();
    if (base.isEmpty()) return;
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(1000);
    if (!http.begin(client, base + "/api/clock")) return;
    const uint32_t start = millis();
    const int code = http.GET();
    const String response = http.getString();
    const uint32_t end = millis();
    JsonDocument reply;
    if (code == 200 && !deserializeJson(reply, response) && reply["server_ms"].is<uint64_t>()) {
      const bool ok = clock.synchronize(reply["server_ms"].as<uint64_t>(), start, end);
      Serial.printf("[clock] %s; RTT=%lu ms; incertidumbre=%lu ms\n",
                    ok ? "sincronizado" : "rechazado por latencia",
                    static_cast<unsigned long>(end - start),
                    static_cast<unsigned long>(clock.uncertainty(end)));
    } else {
      Serial.printf("[clock] Sin sincronizacion; HTTP=%d\n", code);
    }
    http.end();
  }
  String newPressId() { return prefix + "-" + String(++counter); }
  String registerPress(const String &pressId, uint64_t triggerMs, uint32_t uncertainty) {
    if (WiFi.status() != WL_CONNECTED) return "";
    const String base = baseUrl();
    if (base.isEmpty()) return "";
    JsonDocument request;
    request["role"] = role;
    request["press_id"] = pressId;
    request["triggered_at_ms"] = triggerMs;
    request["clock_error_ms"] = uncertainty;
    String body;
    serializeJson(request, body);
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(1500);
    if (!http.begin(client, base + "/api/button-events")) return "";
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    const String response = http.getString();
    JsonDocument reply;
    String ticket;
    if (code == 200 && !deserializeJson(reply, response)) ticket = reply["event_id"].as<String>();
    Serial.printf("[button] Registro HTTP=%d; evento=%s\n", code, ticket.c_str());
    if (code == 409) Serial.println(response);
    http.end();
    return ticket;
  }
 private:
  String role, host, prefix;
  uint16_t port = 8000;
  uint32_t counter = 0;
  uint32_t lastClockAttempt = 0;
  bool attempted = false;
};

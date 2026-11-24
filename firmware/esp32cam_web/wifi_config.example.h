#pragma once
// Ejecuta python3 firmware/configure_wifi.py para crear wifi_config.h.
constexpr char WIFI_SSID[] = "TU_RED_WIFI";
constexpr char WIFI_PASSWORD[] = "TU_CLAVE_WIFI";
constexpr char BACKEND_HOST[] = "NOMBRE_DE_LA_MAC_SIN_LOCAL_O_IP";
// LorBot_API escucha en 5055; el backend Proyecto Loro usa 8000.
constexpr unsigned short BACKEND_PORT = 5055;

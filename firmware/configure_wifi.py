"""Genera cabeceras locales de ambas placas sin guardar claves en Git."""
import getpass
import json
from pathlib import Path


def configure(ssid, password, backend_host, backend_port=8000):
    content = ("#pragma once\n// Configuracion local: no versionar este archivo.\n"
               f"constexpr char WIFI_SSID[] = {json.dumps(ssid, ensure_ascii=True)};\n"
               f"constexpr char WIFI_PASSWORD[] = {json.dumps(password)};\n"
               f"constexpr char BACKEND_HOST[] = {json.dumps(backend_host)};\n"
               f"constexpr unsigned short BACKEND_PORT = {int(backend_port)};\n")
    for folder in ("prueba_boton", "esp32_sensores", "esp32cam_web"):
        target = Path(__file__).resolve().parent / folder / "wifi_config.h"
        target.write_text(content)
        target.chmod(0o600)


if __name__ == "__main__":
    ssid = input("Nombre exacto de la red Wi-Fi: ")
    password = getpass.getpass("Clave Wi-Fi: ")
    backend_host = input("Nombre mDNS de la Mac sin .local o su IP: ")
    port_text = input("Puerto del backend [8000]: ").strip()
    backend_port = int(port_text or "8000")
    if not 1 <= backend_port <= 65535:
        raise SystemExit("El puerto debe estar entre 1 y 65535.")
    configure(ssid, password, backend_host, backend_port)
    print("Cabeceras locales creadas para los sketches de sensores y cámara.")

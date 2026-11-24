# LorBot API

En macOS, abre `Iniciar.command` para crear el entorno, instalar las dependencias e iniciar la API en `http://127.0.0.1:5055`. También puedes ejecutar `./Iniciar.command` desde Terminal. Se probó con Python 3.12. La primera ejecución necesita internet para instalar las dependencias y descargar CLIP. Mantén abierta la ventana de Terminal mientras usas la API.

Abre http://127.0.0.1:5055 en tu navegador. Pulsa **Tomar foto con ESP32-CAM**; la temperatura, humedad y ruido se actualizan automáticamente desde el ESP32 de sensores. La clasificación requiere una foto reciente y lecturas válidas de temperatura y ruido. La humedad se muestra como dato informativo.

Puedes consultar `GET /health` para saber si el servidor está listo. Para detenerlo, pulsa Ctrl+C en Terminal.

## ESP32

Conecta la Mac y las dos placas a la misma red Wi-Fi de 2.4 GHz. La cámara y el ESP32 de
sensores comparten los eventos de captura con esta API en el puerto **5055**.
Desde la carpeta del proyecto Proyecto Loro ejecuta
`python3 firmware/configure_wifi.py`, introduce la clave Wi-Fi,
el nombre mDNS de la Mac `MacBook-Pro-de-Carlos` (sin `.local`) o su IP, y el
puerto `5055`. El generador crea `wifi_config.h` local para las placas; esos
archivos con credenciales no se deben subir a Git.

Inicia LorBot_API con `./Iniciar.command` en esta carpeta. La interfaz ofrece
**Tomar foto con ESP32-CAM**, completa temperatura y ruido desde el ESP32 de
sensores y muestra también la humedad del DHT22. El clasificador actual usa
temperatura y ruido; la humedad es informativa.

La API no tiene autenticación y queda accesible dentro de la red local; úsala
solo en una red de confianza.

# ESP32-CAM AI Thinker / OV2640

Abre `esp32cam_web.ino` desde Arduino IDE, elige **AI Thinker ESP32-CAM** y velocidad de carga **115200**. Requiere ArduinoJson 7 y la biblioteca local RobotLoroButton de `firmware/libraries`; ver [boton compartido](../../docs/boton-compartido.md).

La red y su clave se configuran localmente en `wifi_config.h`, generado con
`python3 firmware/configure_wifi.py`. Ese archivo no se versiona; hay una
cabecera de ejemplo en esta carpeta. El monitor serie (115200 baudios)
muestra la IP. `GET /capture` devuelve una nueva fotografía y `/` abre una
vista directa con botón de captura. `/health` comprueba la inicialización.

La captura actual es **320 × 240**, con JPEG quality 15, para reducir el
trafico durante el diagnostico de la red movil. `/health` agrega tiempo
encendido, RSSI y bytes anunciados/enviados de la ultima foto para detectar
reinicios o transferencias incompletas. El envio se hace en bloques de
1024 bytes y reanuda escrituras parciales, con un limite de espera.

Prueba del envio sin hardware:

```bash
c++ -std=c++11 firmware/tests/jpeg_transfer_test.cpp -o /tmp/loro-jpeg-transfer-test
/tmp/loro-jpeg-transfer-test
```

El backend Python solicita la foto cuando el usuario pulsa **Tomar foto** en el frontend.
El boton fisico GPIO2 compartido con sensores captura y envia directamente el
JPEG al backend; en ese caso BACKEND_HOST debe contener la IP actual de la Mac.

La pagina de la camara incluye **Iniciar video / Detener video**. El stream
MJPEG esta en `http://IP_CAMARA:81/stream`, 320x240 y hasta 5 fps, con prioridad
para fotos del boton. Ver [video en vivo](../../docs/video-en-vivo.md).

Para conectarla con LorBot_API, configura `BACKEND_PORT=5055` y como
`BACKEND_HOST` el nombre mDNS de la Mac sin `.local` o su IP en la misma red.
El generador conserva el puerto 8000 como valor predeterminado para Proyecto
Loro; escribe `5055` cuando uses LorBot_API.

Consulta el [README principal](../../README.md) para alimentación, modo de carga, configuración del backend y pruebas.

## Compilación y carga con Arduino CLI

Verificado con Arduino-ESP32 **2.0.17**. Usa una carpeta temporal sin espacios para evitar un fallo de las recetas de compilación de esa versión:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32cam --libraries firmware/libraries --build-path /tmp/loro-esp32cam-build firmware/esp32cam_web
arduino-cli upload --fqbn esp32:esp32:esp32cam --upload-property upload.speed=115200 --port /dev/cu.usbserial-110 --input-dir /tmp/loro-esp32cam-build firmware/esp32cam_web
```

Ejecuta desde la raíz y sustituye el puerto según tu equipo. Algunas versiones no muestran un menú de velocidad para AI Thinker; `--upload-property upload.speed=115200` fija explícitamente la velocidad de carga.

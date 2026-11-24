# Medicion por boton

Abre `prueba_boton.ino` en Arduino IDE. Conserva `audio_metrics.h` en esta
misma carpeta; es una copia del calculo de audio de `esp32_sensores`.

- Placa: **ESP32 Dev Module**; compilacion verificada con Arduino-ESP32 2.0.17.
- Bibliotecas: **DHT sensor library by Adafruit**, **ArduinoJson 7** y la biblioteca local **RobotLoroButton** (`firmware/libraries`).
- Monitor serie: **115200 baudios**.
- Boton entre **GPIO2 y GND**, usando `INPUT_PULLUP` y antirrebote de 100 ms.
- DHT11: DATA **GPIO32**, VCC **3V3**, GND comun; pull-up de 10 kOhm
  entre DATA y 3V3 si el modulo no la incorpora.
- INMP441: VDD **3V3**, GND comun, SCK **GPIO26**, WS **GPIO25**,
  SD **GPIO33**, L/R **GND**. Se usa el slot derecho del driver,
  como en el montaje del proyecto.

Cada pulsacion toma una lectura de temperatura/humedad y una ventana de
audio de un segundo, tras descartar 400 ms de arranque del microfono.
Respeta al menos dos segundos entre lecturas del DHT11. Mantener el boton
presionado no repite la medicion; suelta y pulsa otra vez cuando termine.
La interrupcion conserva una pulsacion pendiente durante las esperas; no conserva
una cola ilimitada ni sobrescribe el primer pulso pendiente. Esperar a que termine
la medicion antes de volver a pulsar.

Muestra errores independientes de cada sensor. El audio se expresa en
**dBFS**, sin calibracion SPL ni ponderacion A. Una senal constante se
informa como invalida y no se clasifica como ruido bajo.
Muestra **ALTO** si el nivel es mayor o igual a `UMBRAL_RUIDO_DBFS`
(-35 dBFS inicialmente), y **BAJO** si es menor. Este umbral es relativo
y configurable: no representa un limite ambiental ni de seguridad auditiva.
El boton inicia la adquisicion: los sensores siguen
conectados a su alimentacion, pero no se toman mediciones en reposo y el
driver I2S se desinstala al terminar cada captura.

Este sketch envia las lecturas al backend sin esperar la fotografia. Con el
boton compartido, la ESP32-CAM envia su JPEG por separado con el mismo ticket.
Ver [cableado y protocolo](../../docs/boton-compartido.md). Genera `wifi_config.h` con
`python3 firmware/configure_wifi.py` antes de compilar.
Una muestra pendiente se respalda en flash y se reintenta sin duplicarla.
Cargarlo reemplaza el programa actual de la placa. Deja el boton suelto
durante la carga. Consulta [la guia MySQL](../../docs/captura-mysql.md).

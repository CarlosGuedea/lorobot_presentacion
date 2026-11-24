# ESP32 camera capture from both buttons

## Intent

Use the OV2640 photo in LorBot_API when either the web interface or the physical GPIO2 button is pressed. Use the DHT22 and INMP441 readings to fill the classifier inputs.

## Proposed behavior

- The web interface has a **Tomar foto** action. It creates a capture event, waits for the ESP32-CAM to poll it and upload its JPEG, then previews that image for the existing classification action.
- GPIO2 on the sensor ESP32 is active-high, matching the supplied wiring to 3.3 V. A debounced press registers the same kind of capture event; the ESP32-CAM captures and uploads the JPEG.
- LorBot_API proxies the sensor board's `/readings` endpoint to fill temperature and dBFS. Humidity is shown as a reading, but remains outside the current model inputs.
- Both events use the camera firmware's existing polling/upload protocol. The backend validates event IDs, press IDs, JPEG type, and the existing 16 MB request limit. It keeps pending events and the latest photo in memory.
- Serve `GET /api/clock` with Unix epoch milliseconds so both boards can align a button press with a recent video frame when streaming.
- Bind the API to the LAN so both ESP32 boards can reach it. Configure their backend address/port locally; do not commit Wi-Fi credentials.
- Connect the Mac and both ESP32 boards to the user's open `linksys` Wi-Fi. Store local network settings only in ignored `wifi_config.h` files generated for the firmware.
- Require the camera photo and live DHT22/INMP441 readings; do not allow manual substitutes or classification without them.

## Scope and constraints

- Update the LorBot_API Flask app and interface, plus the main `esp32_sensores` sketch and firmware connection examples. The camera sketch already polls for photos and uploads them.
- Match the camera protocol at `/api/button-events/pending-photo` and `/api/button-events/<event_id>/photo`; the current app does not implement it.
- Firmware source is `/Users/carloseduardoguedea/Documents/Maestría Internet/3er semestre/proyecto loro medidor/firmware`. The camera poll expects JSON `{pending, event_id, trigger_ms, camera_key}`; upload is raw `image/jpeg` with `X-Press-Id` and returns `{saved, event_id}`. The sensor board already exposes `GET /readings` with `temperature_c`, `humidity_pct`, `sound_dbfs`, `dht_ok`, and `sound_ok`.
- Add the active-high, debounced GPIO2 event to `esp32_sensores`; the main sketch currently only serves readings. Keep the camera sketch's existing capture/upload flow.
- Use port 5055 for the ESP32 backend configuration. The API currently binds only to localhost, so LAN access must be enabled for hardware integration.
- Allow one outstanding capture event at a time; reject a concurrent trigger with HTTP 409 rather than overwrite or misassociate a photo. Expire it after 55 seconds, matching the camera upload retry window.
- Keep no more than four event photos in memory for 55 seconds; keep only the latest JPEG beyond that, until the API restarts.
- A server restart clears pending capture events and the latest in-memory photo. Persistent storage and automatic classification from a physical press are out of scope; the photo appears in the interface and can be classified with the existing action.
- The API has no authentication and is reachable by devices on the open Wi-Fi; use it only on a trusted local network.
- Do not change the model's class logic or add humidity to its features.

## Error behavior

Show a clear interface error when the camera or sensor endpoint is unreachable, an event expires, or an invalid JPEG is uploaded. Disable classification until the ESP32-CAM photo and current DHT22/INMP441 readings are available.

## Implementation calls left to the builder

- The API's LAN bind default and configurable sensor/camera addresses.
- A small in-memory event store shaped to the existing firmware protocol and the latest-photo read route used by an open page.
- Whether a GPIO2 photo is shown on the already-open page by polling the latest-photo endpoint.

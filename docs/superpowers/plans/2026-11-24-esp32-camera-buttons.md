# ESP32 Camera Buttons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture and preview a fresh OV2640 JPEG from either the LorBot_API web button or the sensor board's active-high GPIO2 button, while using current DHT22/INMP441 readings in the existing prediction form.

**Architecture:** LorBot_API owns a small, in-memory capture event and photo state. The web UI and sensor firmware register events; the existing ESP32-CAM polls the event and uploads the JPEG. Flask proxies `/readings` from the sensor board and serves the uploaded photo to the open interface.

**Tech Stack:** Flask, Python standard-library HTTP client, existing HTML/JavaScript, Arduino ESP32 firmware and `RobotLoroButton`.

**Spec:** [2026-11-24-esp32-camera-buttons-design.md](../specs/2026-11-24-esp32-camera-buttons-design.md)

## Global Constraints

- One outstanding photo event at a time; reject concurrent events with HTTP 409.
- Expire an event after 55 seconds.
- Keep at most four event-specific JPEGs for 55 seconds, plus one latest-photo reference.
- Accept JPEGs up to 2 MiB from the camera and preserve the existing 16 MiB prediction-request limit.
- GPIO2 is active-high, using the supplied 3.3 V button wiring.
- Connect the Mac and both ESP32 boards to the open `linksys` Wi-Fi; keep network configuration only in Git-ignored local firmware files.
- Keep humidity display-only; keep model inputs as temperature and dBFS.
- Do not add tests or run tests unless the user asks.
- Do not commit Wi-Fi credentials or local `wifi_config.h` files.
- The API has no authentication; expose it only on the trusted local `linksys` network.

## Review Focus

- Camera disconnected or slow: return a visible timeout/error and leave the UI usable.
- Camera upload with invalid content type, oversized body, or incomplete JPEG: reject it without replacing a valid latest photo.
- Duplicate or overlapping UI/physical events: never pair an image with the wrong event; return 409 while an event is active.
- Missing, stale, null, or non-finite sensor values: show unavailable status and block classification; sensor fields stay read-only.
- GPIO2 bounce or a held button: emit one event per press and re-arm only after release.

## File Map

- `app.py`: in-memory event coordination, camera JPEG validation, sensor proxy, LAN bind.
- `templates/index.html`: capture button, live sensor fields, physical-photo refresh, and existing classification form.
- `README.md`: local network and ESP32 configuration.
- `firmware/esp32_sensores/esp32_sensores.ino`: active-high GPIO2 event registration alongside current DHT22/INMP441 readings.
- `firmware/esp32_sensores/wifi_config.example.h`: local backend configuration shape for the main sensor sketch.
- `firmware/configure_wifi.py`: generate config for the main sensor sketch and accept a backend port while preserving 8000 as the default for Proyecto Loro.
- `firmware/esp32cam_web/wifi_config.example.h` and its README: document port 5055 for LorBot_API while keeping credentials local.

## Tasks

### Task 1: Add capture and sensor routes to LorBot_API

**Files:**
- Modify `app.py`

**Interfaces:**
- `POST /api/button-events` accepts `role`, `press_id`, `triggered_at_ms`, and `clock_error_ms`; returns `event_id`.
- `GET /api/clock` returns `server_ms` in Unix epoch milliseconds for firmware clock synchronization.
- `GET /api/button-events/pending-photo` returns the camera protocol `{pending, event_id, trigger_ms, camera_key}`.
- `POST /api/button-events/<event_id>/photo` accepts raw `image/jpeg` plus `X-Press-Id`; returns `{saved, event_id}`.
- `GET /api/button-events/<event_id>/photo` returns 202 while pending and the JPEG after upload.
- `GET /api/latest-photo` serves the latest uploaded JPEG and its capture ID for the open UI to notice physical captures.
- `GET /api/sensors` proxies the configured sensor board `/readings` and returns finite temperature, humidity, and sound dBFS readings.

- [ ] Add event registration, expiry, one-event concurrency, and photo retrieval/upload handling within `create_app`, using UUID-hex event IDs and thread-safe state.
- [ ] Add `/api/clock` using the server's current epoch milliseconds.
- [ ] Validate raw JPEG type, size, SOI/EOI markers, and image decodability before storing; cap camera uploads at 2 MiB.
- [ ] Add a bounded sensor JSON proxy using `LORBOT_SENSORS_URL` (default `http://esp32sensores.local`); reject malformed/stale readings and keep exceptions as JSON errors.
- [ ] Make Flask's bind host configurable as `LORBOT_HOST` with LAN-accessible default `0.0.0.0`; preserve port 5055 and current `/predict` behavior.

### Task 2: Connect the interface to capture events and sensor readings

**Files:**
- Modify `templates/index.html`

**Interfaces:**
- “Tomar foto” registers a web event through `/api/button-events`, waits for that event's JPEG, then previews it and makes it available to the existing `/predict` form.
- The page refreshes `/api/latest-photo` so a photo requested by GPIO2 appears when the page is open.
- Sensor readings update read-only temperature and dBFS fields and show humidity/status; the photo must come from the ESP32-CAM.

- [ ] Add an explicit capture button and pending/error status without changing the existing classification action.
- [ ] Poll the matching event photo with a bounded timeout; create the preview and form file only after a valid JPEG response.
- [ ] Fetch `/api/sensors` on page load and periodically; update values only for finite, current readings and show humidity separately.
- [ ] Poll for a new physical capture ID and refresh the preview with camera photos only.

### Task 3: Register physical GPIO2 presses from the main sensor firmware

**Files:**
- Modify `firmware/esp32_sensores/esp32_sensores.ino`
- Create `firmware/esp32_sensores/wifi_config.example.h`
- Modify `firmware/configure_wifi.py`
- Modify `firmware/esp32cam_web/wifi_config.example.h` and `firmware/esp32cam_web/README.md`
- Modify `firmware/esp32_sensores/README.md`

**Interfaces:**
- Sensor firmware registers GPIO2 presses with the same `/api/button-events` JSON contract and uses the server clock if its local clock is not synchronized.
- `configure_wifi.py` emits `wifi_config.h` for `esp32_sensores` as well as the existing sketches; its port prompt defaults to 8000 to preserve Proyecto Loro, while LorBot_API uses 5055.

- [ ] Add a debounced active-high GPIO2 input using the shared `RobotLoroButton` event client; preserve existing DHT22 and INMP441 readings and `/readings` JSON.
- [ ] Configure sensor firmware Wi-Fi/backend settings from local `wifi_config.h` and add only a credential-free example header.
- [ ] Extend the local config generator to target the main sensor sketch and accept backend port, retaining 8000 as the existing default.
- [ ] Update camera example/configuration instructions to use port 5055 for LorBot_API and document the Mac LAN address and matching network requirements.

## Verification limits

Per the project instructions, this change will not add or run tests unless requested. Review the final diff and configuration examples only; real camera, sensor, LAN, and firmware-build behavior will remain unverified without hardware or an explicit verification request.

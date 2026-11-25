from io import BytesIO
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, build_opener
import json
import math
import os
import re
import threading
import time
import uuid

import joblib
import numpy as np
import torch
from flask import Flask, jsonify, render_template, request
from PIL import Image, ImageOps, UnidentifiedImageError
from transformers import CLIPModel, CLIPProcessor

MODEL_PATH = Path(__file__).with_name("lorbot_pipeline.joblib")
MAX_CAMERA_PHOTO_BYTES = 2 * 1024 * 1024
CAPTURE_TTL_SECONDS = 55
EVENT_ID_RE = re.compile(r"^[0-9a-f]{32}$")
PRESS_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,80}$")


def finite_number(value, minimum, maximum):
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return number if math.isfinite(number) and minimum <= number <= maximum else None


def validate_jpeg(data):
    if not data or len(data) > MAX_CAMERA_PHOTO_BYTES:
        raise ValueError("La foto está vacía o supera 2 MiB.")
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ValueError("La foto JPEG está incompleta.")
    try:
        with Image.open(BytesIO(data)) as image:
            if image.format != "JPEG" or image.width * image.height > 5_000_000:
                raise ValueError("La foto JPEG no es válida.")
            image.load()
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as error:
        raise ValueError("La foto JPEG no es válida.") from error
    return data


class LorBot:
    def __init__(self, path):
        self.pipeline = joblib.load(path)
        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        model_id = self.pipeline["clip_id"]
        revision = self.pipeline.get("clip_revision")
        self.processor = CLIPProcessor.from_pretrained(model_id, revision=revision)
        self.encoder = CLIPModel.from_pretrained(model_id, revision=revision)
        self.encoder.eval().to(self.device)
        self.references = torch.from_numpy(self.pipeline["text_vectors"]).to(self.device)

    @torch.inference_mode()
    def predict(self, image, temperature, noise):
        pixels = self.processor(images=[image], return_tensors="pt").to(self.device)
        output = self.encoder.get_image_features(**pixels)
        embeddings = output if torch.is_tensor(output) else output.pooler_output
        embeddings = torch.nn.functional.normalize(embeddings, dim=-1)
        scores = (embeddings @ self.references.T).cpu().numpy()
        inputs = np.column_stack(([[temperature, noise]], scores)).astype(np.float32)
        label = int(self.pipeline["classifier"].predict(inputs)[0])

        temperature_limit = self.pipeline.get("temperature_limit_c")
        noise_limit = self.pipeline.get("sound_limit_dbfs")
        if (
            temperature_limit is not None
            and noise_limit is not None
            and temperature >= temperature_limit
            and noise >= noise_limit
        ):
            label = self.pipeline["class_names"].index("No apto")

        return self.pipeline["class_names"][label]


def create_app():
    app = Flask(__name__)
    app.config["MAX_CONTENT_LENGTH"] = 16 * 1024 * 1024
    model = LorBot(MODEL_PATH)
    sensor_url = os.getenv("LORBOT_SENSORS_URL", "http://esp32sensores.local").rstrip("/")
    sensor_address = urlsplit(sensor_url)
    if (sensor_address.scheme != "http" or not sensor_address.hostname
            or sensor_address.username or sensor_address.password
            or sensor_address.path not in ("", "/") or sensor_address.query or sensor_address.fragment):
        raise ValueError("LORBOT_SENSORS_URL debe ser una dirección base HTTP, por ejemplo http://192.168.1.50")

    capture_lock = threading.Lock()
    prediction_lock = threading.Lock()
    capture_events = {}
    active_event_id = None
    latest_photo = None
    latest_photo_id = None

    def prune_events(now):
        nonlocal active_event_id
        expired = [event_id for event_id, event in capture_events.items()
                   if now - event["created_at"] >= CAPTURE_TTL_SECONDS]
        for event_id in expired:
            capture_events.pop(event_id, None)
            if active_event_id == event_id:
                active_event_id = None
        while len(capture_events) > 4:
            oldest = min(capture_events, key=lambda event_id: capture_events[event_id]["created_at"])
            capture_events.pop(oldest, None)
            if active_event_id == oldest:
                active_event_id = None

    @app.get("/")
    def index():
        return render_template("index.html")

    def read_sensor_reading():
        req = Request(sensor_url + "/readings", headers={"Accept": "application/json"})
        with build_opener(ProxyHandler({})).open(req, timeout=2) as response:
            raw = response.read(16 * 1024 + 1)
        if len(raw) > 16 * 1024:
            raise ValueError("La respuesta del sensor es demasiado grande.")
        reading = json.loads(raw)
        if not isinstance(reading, dict):
            raise ValueError("La respuesta del sensor no es un objeto JSON.")
        age = reading.get("sample_age_ms")
        if not isinstance(age, int) or isinstance(age, bool) or age < 0 or age > 5000:
            raise ValueError("La lectura del ESP32 de sensores está vencida.")
        return reading

    def update_sensor_display(state):
        body = json.dumps({"state": state}, separators=(",", ":")).encode()
        req = Request(sensor_url + "/display", data=body,
                      headers={"Content-Type": "application/json"}, method="POST")
        with build_opener(ProxyHandler({})).open(req, timeout=0.5) as response:
            if response.status != 200:
                raise OSError(f"La pantalla ESP32 respondió HTTP {response.status}.")

    def classify_image(image):
        reading = None
        for attempt in range(3):
            try:
                reading = read_sensor_reading()
                break
            except (HTTPError, URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError):
                if attempt == 2:
                    return {"error": "Se necesita una lectura reciente del ESP32 de sensores."}, 503
                time.sleep(0.25)
        temperature = finite_number(reading.get("temperature_c"), -40, 80)
        noise = finite_number(reading.get("sound_dbfs"), -120, 1)
        if (reading.get("dht_ok") is not True or reading.get("sound_ok") is not True
                or temperature is None or noise is None):
            return {"error": "Se necesitan lecturas válidas del DHT22 y del INMP441."}, 503

        with prediction_lock:
            prediction = model.predict(image, temperature, noise)
        display_state = {"Apto": "good", "No apto": "bad"}.get(prediction)
        if display_state:
            try:
                update_sensor_display(display_state)
            except (HTTPError, URLError, TimeoutError, OSError):
                app.logger.warning("No se pudo actualizar la pantalla del ESP32 de sensores.")
        return {"prediction": prediction, "temperature_c": temperature,
                "sound_dbfs": noise,
                "humidity_pct": finite_number(reading.get("humidity_pct"), 0, 100)}, 200

    def classify_captured_photo(event_id, data):
        try:
            with Image.open(BytesIO(data)) as source:
                image = ImageOps.exif_transpose(source).convert("RGB")
            result, status = classify_image(image)
        except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError):
            result, status = {"error": "La foto JPEG no se pudo procesar."}, 400
        except Exception:
            app.logger.exception("Falló la clasificación automática de la captura %s.", event_id)
            result, status = {"error": "No se pudo completar la clasificación automática."}, 500
        with capture_lock:
            event = capture_events.get(event_id)
            if event:
                event["classification"] = {
                    "status": "complete" if status == 200 else "failed",
                    **result,
                }

    @app.get("/health")
    def health():
        return jsonify(status="ok")

    @app.get("/api/clock")
    def clock():
        return jsonify(server_ms=int(time.time() * 1000))

    @app.get("/api/sensors")
    def sensors():
        try:
            reading = read_sensor_reading()
            age = reading.get("sample_age_ms")
            dht_ok = reading.get("dht_ok") is True
            sound_ok = reading.get("sound_ok") is True
            return jsonify(
                temperature_c=finite_number(reading.get("temperature_c"), -40, 80) if dht_ok else None,
                humidity_pct=finite_number(reading.get("humidity_pct"), 0, 100) if dht_ok else None,
                sound_dbfs=finite_number(reading.get("sound_dbfs"), -120, 1) if sound_ok else None,
                dht_ok=dht_ok,
                sound_ok=sound_ok,
                sample_age_ms=age,
            )
        except (HTTPError, URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError):
            return jsonify(error="No se pudo obtener una lectura válida del ESP32 de sensores."), 502

    @app.post("/api/button-events")
    def register_capture():
        nonlocal active_event_id
        if not request.content_length or request.content_length > 2048:
            return jsonify(error="La solicitud de foto es demasiado grande o está vacía."), 413
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify(error="Envía un objeto JSON para solicitar una foto."), 400
        role = payload.get("role")
        press_id = payload.get("press_id")
        trigger_ms = payload.get("triggered_at_ms", 0)
        clock_error = payload.get("clock_error_ms", 0)
        if (role not in ("web", "sensors") or not isinstance(press_id, str)
                or not PRESS_ID_RE.fullmatch(press_id)
                or not isinstance(trigger_ms, int) or isinstance(trigger_ms, bool) or trigger_ms < 0
                or not isinstance(clock_error, int) or isinstance(clock_error, bool)
                or not 0 <= clock_error <= 10000):
            return jsonify(error="La solicitud de foto no es válida."), 400

        now = time.monotonic()
        now_ms = int(time.time() * 1000)
        event_trigger_ms = trigger_ms if abs(now_ms - trigger_ms) <= 10000 else now_ms
        with capture_lock:
            prune_events(now)
            duplicate = next((event for event in capture_events.values()
                              if event["role"] == role and event["press_id"] == press_id), None)
            if duplicate:
                return jsonify(event_id=duplicate["event_id"]), 200
            if active_event_id:
                return jsonify(error="Ya hay una foto pendiente. Intenta de nuevo en unos segundos."), 409
            event_id = uuid.uuid4().hex
            capture_events[event_id] = {
                "event_id": event_id,
                "role": role,
                "press_id": press_id,
                "trigger_ms": event_trigger_ms,
                "camera_key": None,
                "created_at": now,
                "photo": None,
                "classification": {"status": "waiting_photo"},
            }
            active_event_id = event_id
        return jsonify(event_id=event_id), 200

    @app.get("/api/button-events/pending-photo")
    def pending_photo():
        with capture_lock:
            prune_events(time.monotonic())
            event = capture_events.get(active_event_id) if active_event_id else None
            if not event or event["photo"] is not None:
                return jsonify(pending=False)
            return jsonify(pending=True, event_id=event["event_id"],
                           trigger_ms=event["trigger_ms"], camera_key=event["camera_key"])

    @app.post("/api/button-events/<event_id>/photo")
    def receive_photo(event_id):
        nonlocal active_event_id, latest_photo, latest_photo_id
        if not EVENT_ID_RE.fullmatch(event_id):
            return jsonify(saved=False, error="El identificador de evento no es válido."), 404
        if request.mimetype != "image/jpeg":
            return jsonify(saved=False, error="La cámara debe enviar image/jpeg."), 415
        camera_key = request.headers.get("X-Press-Id", "")
        if not PRESS_ID_RE.fullmatch(camera_key):
            return jsonify(saved=False, error="Falta el identificador de cámara."), 400
        if not request.content_length or request.content_length > MAX_CAMERA_PHOTO_BYTES:
            return jsonify(saved=False, error="La foto debe tener entre 1 byte y 2 MiB."), 413
        try:
            data = validate_jpeg(request.get_data(cache=False, as_text=False))
        except ValueError as error:
            return jsonify(saved=False, error=str(error)), 400

        with capture_lock:
            now = time.monotonic()
            prune_events(now)
            event = capture_events.get(event_id)
            if not event:
                return jsonify(saved=False, error="La solicitud de foto venció o no existe."), 410
            if event["photo"] is not None:
                if event["camera_key"] != camera_key:
                    return jsonify(saved=False, error="La foto ya fue recibida desde otra cámara."), 409
                return jsonify(saved=True, event_id=event_id), 200
            event["photo"] = data
            event["camera_key"] = camera_key
            event["created_at"] = now
            event["classification"] = {"status": "processing"}
            latest_photo, latest_photo_id = data, event_id
            if active_event_id == event_id:
                active_event_id = None
            prune_events(now)
        threading.Thread(target=classify_captured_photo, args=(event_id, data),
                         name=f"classify-{event_id[:8]}", daemon=True).start()
        return jsonify(saved=True, event_id=event_id), 201

    @app.get("/api/button-events/<event_id>/photo")
    def get_event_photo(event_id):
        if not EVENT_ID_RE.fullmatch(event_id):
            return jsonify(error="El identificador de evento no es válido."), 404
        with capture_lock:
            prune_events(time.monotonic())
            event = capture_events.get(event_id)
            if not event:
                return jsonify(error="La solicitud de foto venció o no existe."), 410
            data = event["photo"]
        if data is None:
            return jsonify(pending=True), 202
        response = app.response_class(data, mimetype="image/jpeg")
        response.headers["X-Capture-Id"] = event_id
        response.headers["Cache-Control"] = "no-store"
        return response

    @app.get("/api/button-events/<event_id>/classification")
    def get_event_classification(event_id):
        if not EVENT_ID_RE.fullmatch(event_id):
            return jsonify(error="El identificador de evento no es válido."), 404
        with capture_lock:
            prune_events(time.monotonic())
            event = capture_events.get(event_id)
            if not event:
                return jsonify(error="La solicitud de foto venció o no existe."), 410
            classification = event["classification"]
        status = 202 if classification["status"] in ("waiting_photo", "processing") else 200
        return jsonify(classification), status

    @app.get("/api/latest-photo")
    def get_latest_photo():
        after = request.args.get("after")
        with capture_lock:
            data, event_id = latest_photo, latest_photo_id
        if data is None:
            return jsonify(error="Todavía no se ha tomado una foto."), 404
        if after == event_id:
            return "", 204
        response = app.response_class(data, mimetype="image/jpeg")
        response.headers["X-Capture-Id"] = event_id
        response.headers["Cache-Control"] = "no-store"
        return response

    @app.errorhandler(413)
    def request_too_large(_error):
        return jsonify(error="La petición supera el límite de 16 MB."), 413

    @app.post("/predict")
    def predict():
        upload = request.files.get("image")
        if not upload or not upload.filename:
            return jsonify(error="Envía una imagen en el campo image."), 400

        try:
            with Image.open(BytesIO(upload.read())) as source:
                image = ImageOps.exif_transpose(source).convert("RGB")
        except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError):
            return jsonify(error="Envía una imagen válida, por ejemplo JPG o PNG."), 400

        result, status = classify_image(image)
        return jsonify(result), status

    return app


if __name__ == "__main__":
    create_app().run(host=os.getenv("LORBOT_HOST", "0.0.0.0"), port=5055)

from io import BytesIO
import json
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from PIL import Image

import app as app_module


class SensorResponse:
    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, _limit=-1):
        return json.dumps({
            "temperature_c": 24.5,
            "humidity_pct": 47.0,
            "sound_dbfs": -37.0,
            "dht_ok": True,
            "sound_ok": True,
            "sample_age_ms": 400,
        }).encode()


class SensorOpener:
    def open(self, request, timeout=None):
        assert request.full_url == "http://sensor.test/readings"
        assert timeout == 2
        return SensorResponse()


class RecordingModel:
    def __init__(self):
        self.measurements = None

    def predict(self, _image, temperature, noise):
        self.measurements = (temperature, noise)
        return "Estudio"


class SensorOnlyPredictionTest(unittest.TestCase):
    def test_form_only_accepts_camera_photo_and_read_only_sensor_values(self):
        markup = (Path(__file__).parent.parent / "templates" / "index.html").read_text()

        self.assertNotIn('type="file"', markup)
        self.assertIn('name="temperature_c" type="number" step="any" placeholder="Esperando DHT22…" readonly required', markup)
        self.assertIn('name="sound_dbfs" type="number" step="any" placeholder="Esperando INMP441…" readonly required', markup)

    def test_prediction_uses_live_sensor_values_instead_of_form_values(self):
        model = RecordingModel()
        with patch.dict(os.environ, {"LORBOT_SENSORS_URL": "http://sensor.test"}), \
             patch.object(app_module, "build_opener", lambda _proxy: SensorOpener()), \
             patch.object(app_module, "LorBot", lambda _path: model):
            client = app_module.create_app().test_client()

            image = BytesIO()
            Image.new("RGB", (2, 2), "white").save(image, format="PNG")
            image.seek(0)
            response = client.post("/predict", data={
                "temperature_c": "79",
                "sound_dbfs": "-2",
                "image": (image, "room.png"),
            }, content_type="multipart/form-data")

        self.assertEqual(response.status_code, 200)
        self.assertEqual(model.measurements, (24.5, -37.0))
        self.assertEqual(response.json["temperature_c"], 24.5)
        self.assertEqual(response.json["sound_dbfs"], -37.0)

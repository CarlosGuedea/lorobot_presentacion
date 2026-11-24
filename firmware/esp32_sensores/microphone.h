#pragma once
#include <Arduino.h>
#include "audio_metrics.h"

constexpr int PIN_I2S_SCK = 26;
constexpr int PIN_I2S_WS = 25;
constexpr int PIN_I2S_SD = 33;
constexpr bool MICROPHONE_LEFT_CHANNEL = true; // L/R a GND selecciona el slot izquierdo del INMP441.
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr float SOUND_THRESHOLD_DBFS = -35.0f;

bool beginMicrophone();
AudioSnapshot takeAudioSnapshot();

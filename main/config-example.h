#pragma once

#include "secrets.h"

// ===== WiFi =====
#define WIFI_SSID           SECRET_WIFI_SSID
#define WIFI_PASSWORD       SECRET_WIFI_PASSWORD
#define WIFI_USE_STATIC_IP  1
#define WIFI_IP             "X.X.X.X"
#define WIFI_GATEWAY        "X.X.X.X"
#define WIFI_SUBNET         "255.255.255.0"
#define WIFI_DNS1           "X.X.X.X"

// ===== ElevenLabs TTS =====
#define ELEVENLABS_KEY      SECRET_ELEVENLABS_KEY
#define VOICE_ID            "6vTyAgAT8PncODBcLjRf"
#define ENABLE_ELEVENLABS   0

// ===== Auth =====
#define ENABLE_AUTH         1
#define API_TOKEN           SECRET_API_TOKEN

// ===== Home Assistant =====
#define HA_URL              "http://X.X.X.X:8123"
#define HA_TOKEN            SECRET_HA_TOKEN

// ===== Wake word / capture =====
#define HA_WAKE_WORD        "Kira"
#define VOICE_VAD_THRESHOLD 500
#define VOICE_MAX_RECORD_S  5

// ===== Whisper distant  =====
#define WHISPER_URL         "http://X.X.X.X:XXXX/transcribe"
#define WHISPER_TOKEN       SECRET_WHISPER_TOKEN

#pragma once

struct WiFiConfig {
  const char* ssid;
  const char* password;
  bool use_static_ip;
  const char* ip_addr;
  const char* gateway;
  const char* subnet;
  const char* dns1;
};

extern WiFiConfig myWiFi;
extern const char* ELEVENLABS_KEY;
extern const char* VOICE_ID;
extern const bool ENABLE_ELEVENLABS;
extern const bool ENABLE_AUTH;
extern const char* API_TOKEN;
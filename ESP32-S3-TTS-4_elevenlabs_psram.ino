#include <WiFi.h>
#include <WebServer.h>
#include "LittleFS.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "AudioFileSource.h"

// Audio 
#include "AudioFileSourceLittleFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "config.h"  

// ===== I2S PINS =====
#define I2S_LRCK 4
#define I2S_BCLK 5
#define I2S_DATA 6

// ===== API SERVER =====
WebServer server(80);

// ===== Cache in PSRAM =====
static String  g_tts_cache_txt = "";
static uint8_t* g_tts_cache_buf = nullptr;
static size_t  g_tts_cache_len = 0;

// Stats timing
static bool g_tts_debug_timing = true;

// ===== AUDIO OBJECTS =====
static AudioGeneratorMP3* mp3 = nullptr;
static AudioFileSource* file = nullptr;
static AudioOutputI2S* out = nullptr;

static bool isPlaying = false;
static float g_volume = 0.7f;        // 0.0..1.0
static volatile int requestedId = 0;  // 1..6 => play, -1 => stop

// Mapping ID -> filename
const char* kFiles[6] = {
  "/msg/Bonsoir.mp3",
  "/msg/Bonjour.mp3",
  "/msg/Bonjour_Emmanuel.mp3",
  "/msg/Bonsoir_Emmanuel.mp3",
  "/msg/Bonjour_Veronique.mp3",
  "/msg/Bonsoir_Veronique.mp3"
};

static String currentFile = "";
static String lastError = "";

// ===== ElevenLabs TTS (PSRAM buffer, no flash writes) =====
static uint8_t* ttsBuf = nullptr;
static size_t ttsLen = 0;

// A minimal AudioFileSource that reads from a memory buffer (allocated in PSRAM)
class AudioFileSourceMemory : public AudioFileSource {
 public:
  AudioFileSourceMemory(const uint8_t* data, size_t len) : _data(data), _len(len), _pos(0), _open(true) {}
  virtual ~AudioFileSourceMemory() override {}

  virtual bool open(const char* /*filename*/) override { _pos = 0; _open = true; return true; }
  virtual uint32_t read(void* data, uint32_t len) override {
    if (!_open || !_data) return 0;
    if (_pos >= _len) return 0;
    size_t avail = _len - _pos;
    if (len > avail) len = (uint32_t)avail;
    memcpy(data, _data + _pos, len);
    _pos += len;
    return len;
  }
  virtual bool seek(int32_t pos, int dir) override {
    if (!_open) return false;
    int32_t newPos = 0;
    if (dir == SEEK_SET) newPos = pos;
    else if (dir == SEEK_CUR) newPos = (int32_t)_pos + pos;
    else if (dir == SEEK_END) newPos = (int32_t)_len + pos;
    else return false;

    if (newPos < 0) newPos = 0;
    if ((size_t)newPos > _len) newPos = (int32_t)_len;
    _pos = (size_t)newPos;
    return true;
  }
  virtual bool close() override { _open = false; return true; }
  virtual bool isOpen() override { return _open; }
  virtual uint32_t getSize() override { return (uint32_t)_len; }
  virtual uint32_t getPos() override { return (uint32_t)_pos; }

 private:
  const uint8_t* _data;
  size_t _len;
  size_t _pos;
  bool _open;
};


// Basic URL decode (%xx and +)
static String urlDecode(const String& in) {
  String outS; outS.reserve(in.length());
  auto hex = [](char x)->int {
    if (x >= '0' && x <= '9') return x - '0';
    if (x >= 'a' && x <= 'f') return 10 + (x - 'a');
    if (x >= 'A' && x <= 'F') return 10 + (x - 'A');
    return -1;
  };
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') { outS += ' '; continue; }
    if (c == '%' && i + 2 < in.length()) {
      int a = hex(in[i + 1]);
      int b = hex(in[i + 2]);
      if (a >= 0 && b >= 0) { outS += char((a << 4) | b); i += 2; continue; }
    }
    outS += c;
  }
  return outS;
}

// Escape a string for JSON (minimal)
static String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '"':  o += "\\\""; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          // drop other control chars
        } else {
          o += c;
        }
    }
  }
  return o;
}

static bool elevenlabsDownloadToPSRAM(const String& text, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  if (!ELEVENLABS_KEY || strlen(ELEVENLABS_KEY) == 0) {
    lastError = "Missing ELEVENLABS_KEY in config.h";
    return false;
  }
  if (!VOICE_ID || strlen(VOICE_ID) == 0) {
    lastError = "Missing VOICE_ID in config.h";
    return false;
  }
  if (text.length() == 0) {
    lastError = "Empty txt";
    return false;
  }

  String url = String("https://api.elevenlabs.io/v1/text-to-speech/") + VOICE_ID;

  WiFiClientSecure client;
  client.setInsecure();  // TEST ONLY. For production, validate TLS cert.

  HTTPClient http;
  if (!http.begin(client, url)) {
    lastError = "HTTP begin failed";
    return false;
  }

  http.addHeader("xi-api-key", ELEVENLABS_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "audio/mpeg");

  // Keep it simple: multilingual model is good for FR.
  String body = "{";
  body += "\"text\":\"" + jsonEscape(text) + "\",";
  body += "\"model_id\":\"eleven_multilingual_v2\",";
  body += "\"voice_settings\":{\"stability\":0.5,\"similarity_boost\":0.7}";
  body += "}";

  int code = http.POST((uint8_t*)body.c_str(), body.length());
  if (code <= 0) {
    lastError = String("HTTP POST failed: ") + http.errorToString(code);
    http.end();
    return false;
  }
  if (code != 200) {
    lastError = String("ElevenLabs HTTP ") + code + ": " + http.getString();
    http.end();
    return false;
  }

  int contentLen = http.getSize();  // -1 if unknown
  size_t cap = 0;
  if (contentLen > 0) {
    cap = (size_t)contentLen + 1024;
  } else {
    cap = 128 * 1024; // fallback (your typical is ~50KB)
  }
  if (cap > 256 * 1024) cap = 256 * 1024;

  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    lastError = "PSRAM alloc failed";
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  unsigned long start = millis();

  while (http.connected()) {
    int avail = stream->available();
    if (avail > 0) {
      size_t toRead = (size_t)avail;
      if (toRead > 2048) toRead = 2048;
      if (total + toRead > cap) {
        // grow (rare)
        size_t newCap = cap * 2;
        if (newCap > 512 * 1024) newCap = 512 * 1024;
        if (newCap <= cap || total + toRead > newCap) {
          lastError = "TTS MP3 too large";
          heap_caps_free(buf);
          http.end();
          return false;
        }
        uint8_t* bigger = (uint8_t*)heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bigger) {
          lastError = "PSRAM realloc failed";
          heap_caps_free(buf);
          http.end();
          return false;
        }
        buf = bigger;
        cap = newCap;
      }

      int r = stream->readBytes(buf + total, toRead);
      if (r > 0) total += (size_t)r;
    } else {
      if (millis() - start > 20000) break; // 20s timeout
      delay(1);
    }

    if (!stream->connected() && stream->available() == 0) break;
  }

  http.end();

  if (total < 1000) {
    lastError = String("Downloaded too small: ") + total + " bytes";
    heap_caps_free(buf);
    return false;
  }

  *outBuf = buf;
  *outLen = total;
  return true;
}

// Play a freshly-downloaded MP3 held in PSRAM
static bool startPlaybackFromPSRAM(uint8_t* buf, size_t len) {
  if (!ENABLE_ELEVENLABS) {
    lastError = "elevenlabs disabled";
    return false;
  }
  
  if (isPlaying) {
      lastError = "busy";
      return false;
  }
  stopPlayback();
  ttsBuf = buf;
  ttsLen = len;

  file = new AudioFileSourceMemory(ttsBuf, ttsLen);
  mp3 = new AudioGeneratorMP3();

  if (!mp3->begin(file, out)) {
    lastError = "mp3->begin failed (PSRAM)";
    stopPlayback();
    return false;
  }

  isPlaying = true;
  currentFile = "(elevenlabs stream)";
  lastError = "";
  return true;
}


static void handleList() {
  String json = "{\"ok\":true,\"files\":[";
  bool first = true;

  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (!first) json += ",";
    first = false;
    json += "\"" + String(f.name()) + "\"";
    f = root.openNextFile();
  }

  // liste aussi /msg si le dossier existe
  File dir = LittleFS.open("/msg");
  if (dir && dir.isDirectory()) {
    File m = dir.openNextFile();
    while (m) {
      json += ",\"" + String(m.name()) + "\"";
      m = dir.openNextFile();
    }
  }

  json += "]}";
  server.send(200, "application/json", json);
}

// -------- Helpers --------
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void applyVolume(float v) {
  g_volume = clampf(v, 0.0f, 1.0f);
  if (out) out->SetGain(g_volume);
}

static void stopPlayback() {
  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (file) {
    delete file;
    file = nullptr;
  }
  if (ttsBuf) {
    heap_caps_free(ttsBuf);
    ttsBuf = nullptr;
    ttsLen = 0;
  }
  isPlaying = false;
  currentFile = "";
}

static bool startPlaybackById(int id) {
  if (id < 1 || id > 6) {
    lastError = "Invalid id (must be 1..6)";
    return false;
  }

  const char* path = kFiles[id - 1];
  if (!LittleFS.exists(path)) {
    lastError = String("File not found: ") + path;
    return false;
  }

  stopPlayback();

  file = new AudioFileSourceLittleFS(path);
  mp3  = new AudioGeneratorMP3();

  if (!mp3->begin(file, out)) {
    lastError = "mp3->begin failed";
    stopPlayback();
    return false;
  }

  isPlaying = true;
  currentFile = path;
  lastError = "";
  return true;
}

// -------- WiFi connect --------
static bool parseIP(const char* s, IPAddress& outIp) {
  return outIp.fromString(String(s));
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);

  if (myWiFi.use_static_ip) {
    IPAddress ip, gw, sn, dns1;
    if (!parseIP(myWiFi.ip_addr, ip) ||
        !parseIP(myWiFi.gateway, gw) ||
        !parseIP(myWiFi.subnet, sn)) {
      Serial.println("Static IP config invalid, fallback to DHCP");
    } else {
      if (myWiFi.dns1 && strlen(myWiFi.dns1) > 0 && parseIP(myWiFi.dns1, dns1)) {
        WiFi.config(ip, gw, sn, dns1);
      } else {
        WiFi.config(ip, gw, sn);
      }
    }
  }

  WiFi.begin(myWiFi.ssid, myWiFi.password);
  Serial.printf("WiFi connecting to %s", myWiFi.ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// -------- API Handlers --------
static void sendJson(int code, const String& json) {
  server.send(code, "application/json", json);
}

static void handlePlayPath() {
  // Route: /play/<id>
  // Exemple: /play/3?vol=0.6
  if (!requireAuth()) return;

  String uri = server.uri(); // ex: "/play/3"
  int slash = uri.lastIndexOf('/');
  if (slash < 0 || slash == (int)uri.length() - 1) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing id\"}");
    return;
  }
  int id = uri.substring(slash + 1).toInt();
  if (id < 1 || id > 6) {
    sendJson(400, "{\"ok\":false,\"error\":\"id must be 1..6\"}");
    return;
  }

  // Volume optionnel par requête
  if (server.hasArg("vol")) {
    float v = server.arg("vol").toFloat();
    applyVolume(v);
  }

  requestedId = id; // déclenche dans loop()
  sendJson(200, String("{\"ok\":true,\"queued\":") + id + ",\"volume\":" + String(g_volume, 2) + "}");
}

static void handlePlayQuery() {
  if (!requireAuth()) return;
  // Route: /play?id=3&vol=0.6
  if (!server.hasArg("id")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing id\"}");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 1 || id > 6) {
    sendJson(400, "{\"ok\":false,\"error\":\"id must be 1..6\"}");
    return;
  }
  if (server.hasArg("vol")) {
    float v = server.arg("vol").toFloat();
    applyVolume(v);
  }
  requestedId = id;
  sendJson(200, String("{\"ok\":true,\"queued\":") + id + ",\"volume\":" + String(g_volume, 2) + "}");
}


static void handlePlayTxt() {
  // Route: /playtxt?txt=Bonjour%20bienvenue&vol=0.6
  if (!requireAuth()) return;
  
  if (!server.hasArg("txt")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing txt\"}");
    return;
  }

  String txt = urlDecode(server.arg("txt"));

  if (server.hasArg("vol")) {
    applyVolume(server.arg("vol").toFloat());
  }

  uint8_t* buf = nullptr;
  size_t len = 0;
  if (!elevenlabsDownloadToPSRAM(txt, &buf, &len)) {
    sendJson(500, String("{\"ok\":false,\"error\":\"") + lastError + "\"}");
    return;
  }

  if (!startPlaybackFromPSRAM(buf, len)) {
    sendJson(500, String("{\"ok\":false,\"error\":\"") + lastError + "\"}");
    return;
  }

  sendJson(200, String("{\"ok\":true,\"playing\":true,\"bytes\":") + (int)len + ",\"volume\":" + String(g_volume,2) + "}");
}



static void handleVolume() {
   if (!requireAuth()) return;
  // Route: /volume?level=0.7
  if (!server.hasArg("level")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing level\"}");
    return;
  }
  float v = server.arg("level").toFloat();
  applyVolume(v);
  sendJson(200, String("{\"ok\":true,\"volume\":") + String(g_volume, 2) + "}");
}

static void handleStop() {
  if (!requireAuth()) return;
  requestedId = -1;
  sendJson(200, "{\"ok\":true}");
}

static void handleStatus() {
  String json = "{";
  json += "\"ok\":true,";
  json += "\"playing\":" + String(isPlaying ? "true" : "false") + ",";
  json += "\"volume\":" + String(g_volume, 2) + ",";
  json += "\"file\":\"" + currentFile + "\",";
  json += "\"error\":\"" + lastError + "\"";
  json += "}";
  sendJson(200, json);
}

static void handleNotFound() {
  sendJson(404, "{\"ok\":false,\"error\":\"Not found\"}");
}


static bool requireAuth() {
  if (!ENABLE_AUTH) return true;

  // Header recommandé
  if (server.hasHeader("X-API-Token")) {
    if (server.header("X-API-Token") == API_TOKEN) return true;
  }

  // Fallback query optionnel: ?token=...
  if (server.hasArg("token")) {
    if (server.arg("token") == API_TOKEN) return true;
  }

  sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    while (true) delay(1000);
  }

  // WiFi
  connectWiFi();

  // Audio output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DATA);
  applyVolume(g_volume);

  // API routes
  server.on("/play", HTTP_GET, handlePlayQuery);  // /play?id=1
  server.on("/playtxt", HTTP_GET, handlePlayTxt); // /playtxt?txt=...
  server.on("/volume", HTTP_GET, handleVolume);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/status", HTTP_GET, handleStatus);

  // path based: /play/1 .. /play/6
  server.onNotFound([]() {
    String u = server.uri();
    if (u.startsWith("/play/")) {
      handlePlayPath();
      return;
    }
    handleNotFound();
  });

  server.begin();
  Serial.println("API ready:");
  Serial.println("  GET /play/1");
  Serial.println("  GET /play/3?vol=0.6");
  Serial.println("  GET /play?id=2&vol=0.8");
  Serial.println("  GET /playtxt?txt=Bonjour%20bienvenue&vol=0.6");
  Serial.println("  GET /volume?level=0.7");
  Serial.println("  GET /stop");
  Serial.println("  GET /status");

  server.on("/list", HTTP_GET, handleList);
}

void loop() {
  server.handleClient();

  // Gestion commandes
  if (requestedId != 0) {
    int id = requestedId;
    requestedId = 0;

    if (id == -1) {
      stopPlayback();
    } else {
      startPlaybackById(id);
    }
  }

  // Lecture audio
  if (mp3 && isPlaying) {
    if (!mp3->loop()) {
      stopPlayback();
    }
  }
}

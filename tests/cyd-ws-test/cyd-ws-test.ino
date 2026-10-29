#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"

WebSocketsClient ws;
String bridgeHost;
uint32_t lastMsgMs = 0;
uint32_t msgCount = 0;

void printNowPlaying(const char *payload, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return;
  }
  msgCount++;
  const char *title  = doc["title"]  | "";
  const char *artist = doc["artist"] | "";
  const char *album  = doc["album"]  | "";
  const char *state  = doc["state"]  | "?";
  long pos = doc["position_ms"] | 0L;
  long dur = doc["duration_ms"] | 0L;
  const char *art = doc["art_url"].is<const char *>() ? doc["art_url"].as<const char *>() : "(none)";

  Serial.printf("#%lu [%s] %s - %s | %s | %ld/%ld ms | art=%s | heap=%u\n",
                (unsigned long)msgCount, state, artist, title, album, pos, dur, art,
                (unsigned)ESP.getFreeHeap());
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WS] connected to %s\n", (const char *)payload);
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] disconnected (will retry automatically)");
      break;
    case WStype_TEXT:
      lastMsgMs = millis();
      printNowPlaying((const char *)payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[WS] error");
      break;
    default:
      break;
  }
}

String resolveBridge() {
  if (strlen(BRIDGE_HOSTNAME) > 0) {
    IPAddress ip = MDNS.queryHost(BRIDGE_HOSTNAME, 2000);
    if ((uint32_t)ip != 0) {
      Serial.printf("Resolved %s.local -> %s\n", BRIDGE_HOSTNAME, ip.toString().c_str());
      return ip.toString();
    }
    Serial.printf("Could not resolve %s.local, using BRIDGE_IP\n", BRIDGE_HOSTNAME);
  }
  return String(BRIDGE_IP);
}

void connectWifi() {
  Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWi-Fi timeout, restarting");
      ESP.restart();
    }
  }
  Serial.printf("\nWi-Fi OK. IP=%s RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD Wi-Fi + WebSocket test ===");

  connectWifi();

  if (strlen(BRIDGE_HOSTNAME) > 0) MDNS.begin("cyd-display");
  bridgeHost = resolveBridge();

  Serial.printf("Connecting WebSocket to ws://%s:%d/media\n", bridgeHost.c_str(), BRIDGE_PORT);
  ws.begin(bridgeHost.c_str(), BRIDGE_PORT, "/media");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(2000);
  ws.enableHeartbeat(15000, 3000, 2);   // ping every 15 s, drop after 2 missed pongs
}

void loop() {
  ws.loop();

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 10000) {
    lastStatus = millis();
    Serial.printf("[status] wifi=%s ws=%s lastMsg=%lus ago heap=%u\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN",
                  ws.isConnected() ? "up" : "DOWN",
                  lastMsgMs ? (unsigned long)((millis() - lastMsgMs) / 1000) : 0UL,
                  (unsigned)ESP.getFreeHeap());
  }
}
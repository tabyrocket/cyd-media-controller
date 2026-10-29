#include <WiFi.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "config.h"

#define ART_X     10
#define ART_Y     10
#define ART_SIZE  160
#define MAX_JPG   40000          // artwork is ~5-20 KB, this leaves lots of room

TFT_eSPI tft = TFT_eSPI();
uint8_t *jpgBuf = nullptr;       // allocated once, reused, so the heap doesn't fragment

uint32_t lastHash = 0;
size_t   lastLen  = 0;
bool     showingPlaceholder = false;

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

uint32_t hashBytes(const uint8_t *p, size_t n) {   // FNV-1a
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

void drawPlaceholder(const char *why) {
  tft.fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 6, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawCentreString("NO ART", ART_X + ART_SIZE / 2, ART_Y + ART_SIZE / 2 - 14, 2);
  tft.drawCentreString(why,      ART_X + ART_SIZE / 2, ART_Y + ART_SIZE / 2 + 4, 2);
  showingPlaceholder = true;
  lastHash = 0;
  lastLen = 0;
}

// Returns 200 on success.
// -1 begin failed, -2 bad/missing Content-Length, -3 incomplete download
int fetchArt(size_t &len) {
  len = 0;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  String url = String("http://") + BRIDGE_IP + ":" + BRIDGE_PORT + "/artwork";
  if (!http.begin(url)) return -1;

  int code = http.GET();
  if (code != 200) { http.end(); return code; }

  int cl = http.getSize();
  if (cl <= 0 || cl > MAX_JPG) { http.end(); return -2; }

  WiFiClient *s = http.getStreamPtr();
  size_t got = 0;
  uint32_t last = millis();
  while (got < (size_t)cl && millis() - last < 5000) {
    int av = s->available();
    if (av > 0) {
      int n = s->readBytes(jpgBuf + got, min((size_t)av, (size_t)cl - got));
      got += n;
      last = millis();
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();
  len = got;
  return (got == (size_t)cl) ? 200 : -3;
}

void showStats(const char *line1, const char *line2, const char *line3) {
  tft.fillRect(180, 10, 140, 100, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line1, 184, 14, 2);
  tft.drawString(line2, 184, 38, 2);
  tft.drawString(line3, 184, 62, 2);
}

void fetchAndDraw() {
  size_t len = 0;
  uint32_t t0 = millis();
  int code = fetchArt(len);
  uint32_t tFetch = millis() - t0;

  if (code != 200) {
    Serial.printf("[art] fetch failed: code=%d (%lu ms) heap=%u\n", code, (unsigned long)tFetch, (unsigned)ESP.getFreeHeap());
    if (!showingPlaceholder) drawPlaceholder(code == 404 ? "404" : "error");
    char b[24]; snprintf(b, sizeof(b), "fetch: %d", code);
    showStats(b, "", "");
    return;
  }

  uint32_t h = hashBytes(jpgBuf, len);
  if (!showingPlaceholder && h == lastHash && len == lastLen) {
    Serial.printf("[art] unchanged (%u bytes, fetch %lu ms) - redraw skipped, heap=%u\n",
                  (unsigned)len, (unsigned long)tFetch, (unsigned)ESP.getFreeHeap());
    return;
  }

  uint16_t w = 0, hgt = 0;
  if (TJpgDec.getJpgSize(&w, &hgt, jpgBuf, len) != JDR_OK) {
    Serial.println("[art] getJpgSize failed (progressive or corrupt JPEG?)");
    drawPlaceholder("bad jpg");
    return;
  }

  uint32_t t1 = millis();
  TJpgDec.drawJpg(ART_X, ART_Y, jpgBuf, len);
  uint32_t tDecode = millis() - t1;

  lastHash = h;
  lastLen = len;
  showingPlaceholder = false;

  Serial.printf("[art] drew %ux%u, %u bytes, fetch %lu ms, decode+draw %lu ms, heap=%u\n",
                w, hgt, (unsigned)len, (unsigned long)tFetch, (unsigned long)tDecode, (unsigned)ESP.getFreeHeap());

  char a[24], b[24], c[24];
  snprintf(a, sizeof(a), "%ux%u  %uB", w, hgt, (unsigned)len);
  snprintf(b, sizeof(b), "fetch %lums", (unsigned long)tFetch);
  snprintf(c, sizeof(c), "draw %lums", (unsigned long)tDecode);
  showStats(a, b, c);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD JPEG test ===");

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  jpgBuf = (uint8_t *)malloc(MAX_JPG);
  if (!jpgBuf) { Serial.println("malloc failed!"); while (true) delay(1000); }
  Serial.printf("Heap after buffer alloc: %u\n", (unsigned)ESP.getFreeHeap());

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting Wi-Fi...", 10, 10, 2);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (millis() - start > 20000) { Serial.println("Wi-Fi timeout, restarting"); ESP.restart(); }
  }
  Serial.printf("Wi-Fi OK. IP=%s\n", WiFi.localIP().toString().c_str());

  tft.fillScreen(TFT_BLACK);
  drawPlaceholder("waiting");
}

void loop() {
  static uint32_t last = 0;
  if (WiFi.status() == WL_CONNECTED && (last == 0 || millis() - last >= 5000)) {
    last = millis();
    fetchAndDraw();
  }
  delay(10);
}
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "config.h"

// Touch
#define T_IRQ  36
#define T_MOSI 32
#define T_MISO 39
#define T_CLK  25
#define T_CS   33
#define TOUCH_MIN_X 360
#define TOUCH_MAX_X 3680
#define TOUCH_MIN_Y 370
#define TOUCH_MAX_Y 3800
#define SCR_W 320
#define SCR_H 240

struct Btn { int x, y, w, h; const char *label; const char *cmd; };
Btn btns[3] = {
  {  10, 170, 90, 60, "PREV", "prev"   },
  { 115, 170, 90, 60, "PLAY", "toggle" },
  { 220, 170, 90, 60, "NEXT", "next"   },
};

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);
WebSocketsClient ws;

bool wsUp = false;
String lastTitle, lastArtist, lastState;
String lastStatus;

void drawButtons(int pressed = -1) {
  for (int i = 0; i < 3; i++) {
    uint16_t fill = (i == pressed) ? TFT_GREEN : TFT_DARKGREY;
    tft.fillRoundRect(btns[i].x, btns[i].y, btns[i].w, btns[i].h, 8, fill);
    tft.setTextColor(TFT_WHITE, fill);
    tft.drawCentreString(btns[i].label, btns[i].x + btns[i].w / 2, btns[i].y + btns[i].h / 2 - 8, 2);
  }
}

void drawStatus(const String &s) {
  if (s == lastStatus) return;
  lastStatus = s;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextPadding(SCR_W);
  tft.drawString(s, 10, 10, 2);
  tft.setTextPadding(0);
}

void drawNowPlaying(const String &title, const String &artist, const String &state) {
  tft.setTextWrap(false);
  tft.setTextPadding(SCR_W);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(title.length() ? title : "(nothing playing)", 10, 50, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(artist, 10, 90, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(state, 10, 120, 2);
  tft.setTextPadding(0);
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsUp = true;
      drawStatus("Bridge: connected");
      Serial.println("[WS] connected");
      break;
    case WStype_DISCONNECTED:
      if (wsUp) Serial.println("[WS] disconnected (retrying)");
      wsUp = false;
      drawStatus("Bridge: reconnecting...");
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) return;
      String title  = doc["title"]  | "";
      String artist = doc["artist"] | "";
      String state  = doc["state"]  | "";
      if (title != lastTitle || artist != lastArtist || state != lastState) {   // redraw only on change
        lastTitle = title; lastArtist = artist; lastState = state;
        drawNowPlaying(title, artist, state);
      }
      break;
    }
    default: break;
  }
}

void sendCmd(const char *cmd) {
  if (!wsUp) { Serial.printf("[cmd] %s NOT sent (bridge not connected)\n", cmd); return; }
  String msg = String("{\"cmd\":\"") + cmd + "\"}";
  ws.sendTXT(msg);
  Serial.printf("[cmd] sent %s\n", cmd);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD command test ===");

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  drawButtons();
  drawStatus("Connecting Wi-Fi...");

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

  drawStatus("Connecting bridge...");
  ws.begin(BRIDGE_IP, BRIDGE_PORT, "/media");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(2000);
  ws.enableHeartbeat(15000, 3000, 2);
}

void loop() {
  ws.loop();

  static bool wasTouched = false;
  static int shown = -1;
  static uint32_t lastPress = 0;

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int sx = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCR_W), 0, SCR_W - 1);
    int sy = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCR_H), 0, SCR_H - 1);

    int hit = -1;
    for (int i = 0; i < 3; i++)
      if (sx >= btns[i].x && sx < btns[i].x + btns[i].w &&
          sy >= btns[i].y && sy < btns[i].y + btns[i].h) hit = i;

    // Fire once per touch-down, with a 250 ms debounce
    if (!wasTouched && hit >= 0 && millis() - lastPress > 250) {
      lastPress = millis();
      sendCmd(btns[hit].cmd);
    }
    if (hit != shown) { drawButtons(hit); shown = hit; }
    wasTouched = true;
  } else if (wasTouched) {
    wasTouched = false;
    shown = -1;
    drawButtons();
  }
  delay(10);
}
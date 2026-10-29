#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "config.h"
#include "esp_heap_caps.h"

// ---------- hardware ----------
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

// ---------- tunables ----------
#define BL_PIN          21
#define BL_FULL         204
#define BL_DIM          25        // 0-255, lower = darker
#define DIM_AFTER_MS    30000     // not playing for this long -> dim
#define SLEEP_AFTER_MS  300000   // total inactivity before the screen turns fully off (5 min)
#define SCROLL_STEP_MS  40        // time between scroll steps
#define SCROLL_PX       2         // pixels per step
#define SCROLL_HOLD_MS  1500      // pause at the start of each scroll
#define SCROLL_GAP      40        // gap between end and repeat of the title
#define NOART_GRACE_MS  2500      // wait this long for art before using the default background
#define WIFI_RETRY_MS      10000    // retry Wi-Fi this often while it is down
#define WIFI_RESTART_MS    300000   // restart the board if Wi-Fi stays down this long (5 min)
#define HEAP_RESTART_BELOW 60000    // restart if free heap stays below this (bytes)

// ---------- layout ----------
#define ART_X    8
#define ART_Y    10
#define ART_SZ   128                        // must match the bridge's JPEG size
#define TEXT_X   (ART_X + ART_SZ + 12)      // 148
#define TEXT_W   (SCR_W - TEXT_X - 6)       // 166
#define SPR_H    28                         // height of a text line buffer
#define TITLE_Y1 8
#define TITLE_Y2 36
#define TITLE_YS 22                         // y of the single scrolling title line
#define ARTIST_Y 74
#define ALBUM_Y  98
#define BAR_X    10
#define BAR_Y    160
#define BAR_W    300
#define BAR_H    4
#define TIME_Y   168
#define TIME_W   50
#define TIME_H   18
#define MAX_JPG  40000

#define RGB565(r,g,b) ((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))
#define C_BG     TFT_BLACK
#define C_BTN_ON RGB565(30,215,96)
#define C_GREY   RGB565(179,179,179)

struct Btn { int x, y, w, h; const char *cmd; };
Btn btns[3] = {
  {  10, 200, 90, 36, "prev"   },
  { 115, 200, 90, 36, "toggle" },
  { 220, 200, 90, 36, "next"   },
};

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite txtSpr  = TFT_eSprite(&tft);    // one text line, drawn over the gradient
TFT_eSprite timeSpr = TFT_eSprite(&tft);    // elapsed / total time label
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);
WebSocketsClient ws;

// ---------- state ----------
bool wsUp = false;
bool wifiUp = true;
String title, artist, album, stateStr;
bool isPlaying = false;
long posMs = 0, durMs = 0;
uint32_t posAt = 0;

enum Mode { M_NONE, M_NOWIFI, M_OFFLINE, M_IDLE, M_PLAY };
Mode drawnMode = M_NONE;
bool textDirty = true;
bool forceRepaint = false;
int pressedBtn = -1;
bool iconDirty = false;

bool progVisible = false;
int  lastFill = -1;
long lastSec = -1;
long durShown = -1;

// ---------- artwork state ----------
uint8_t *jpgBuf = nullptr;
size_t   artLen = 0;
bool     artValid = false;
String   wantUrl, wantKey, doneKey;
int      artRetries = 0;
uint32_t nextArtTry = 0;
uint32_t trackChangedAt = 0;

// ---------- background gradient ----------
const uint8_t DEF_TOP[3] = {22, 26, 34};     // neutral dark blue, used when there is no art
const uint8_t DEF_BOT[3] = {4, 5, 8};
uint8_t curTop[3] = {22, 26, 34}, curBot[3] = {4, 5, 8};
uint8_t pendTop[3] = {22, 26, 34}, pendBot[3] = {4, 5, 8};
bool    pendHave = false;
uint16_t rowCol[SCR_H];
uint16_t cBtn, cTrack, cPlace;

// ---------- title scrolling ----------
bool titleScroll = false;
int  titleW = 0, scrollOff = 0;
uint32_t scrollNext = 0, scrollHold = 0;

// ---------- backlight ----------
int blNow = BL_FULL;
bool screenOff = false;                // true once the sleep timeout has passed
uint32_t lastActivity = 0;

// ================= colour helpers =================
void rgb2hsv(float r, float g, float b, float &h, float &s, float &v) {
  r /= 255.0f; g /= 255.0f; b /= 255.0f;
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
  v = mx;
  s = (mx > 0.0001f) ? d / mx : 0;
  if (d < 0.0001f) h = 0;
  else if (mx == r) h = 60.0f * fmodf((g - b) / d, 6.0f);
  else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
  else              h = 60.0f * ((r - g) / d + 4.0f);
  if (h < 0) h += 360.0f;
}

void hsv2rgb(float h, float s, float v, uint8_t *out) {
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c, rf, gf, bf;
  switch (((int)(h / 60.0f)) % 6) {
    case 0:  rf = c; gf = x; bf = 0; break;
    case 1:  rf = x; gf = c; bf = 0; break;
    case 2:  rf = 0; gf = c; bf = x; break;
    case 3:  rf = 0; gf = x; bf = c; break;
    case 4:  rf = x; gf = 0; bf = c; break;
    default: rf = c; gf = 0; bf = x; break;
  }
  out[0] = (uint8_t)((rf + m) * 255.0f + 0.5f);
  out[1] = (uint8_t)((gf + m) * 255.0f + 0.5f);
  out[2] = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

// Keeps the hue, scales brightness down and clamps it to [lo, hi]
void darkenColour(int r, int g, int b, float scale, float lo, float hi, uint8_t *out) {
  float h, s, v;
  rgb2hsv(r, g, b, h, s, v);
  v = v * scale;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  hsv2rgb(h, s, v, out);
}

void bgAt(int y, uint8_t *c) {
  for (int i = 0; i < 3; i++) c[i] = curTop[i] + ((int)curBot[i] - (int)curTop[i]) * y / (SCR_H - 1);
}

uint16_t mixWhite(const uint8_t *c, int pct) {
  return tft.color565(c[0] + (255 - c[0]) * pct / 100,
                      c[1] + (255 - c[1]) * pct / 100,
                      c[2] + (255 - c[2]) * pct / 100);
}

void buildBg() {
  uint8_t c[3];
  for (int y = 0; y < SCR_H; y++) { bgAt(y, c); rowCol[y] = tft.color565(c[0], c[1], c[2]); }
  bgAt(BAR_Y, c); cTrack = mixWhite(c, 25);
  bgAt(218, c);   cBtn   = mixWhite(c, 14);
  bgAt(80, c);    cPlace = mixWhite(c, 20);
}

// Returns true (and schedules a full repaint) if the colours changed
bool setBg(const uint8_t *top, const uint8_t *bot) {
  bool diff = false;
  for (int i = 0; i < 3; i++)
    if (abs((int)top[i] - (int)curTop[i]) > 2 || abs((int)bot[i] - (int)curBot[i]) > 2) diff = true;
  if (!diff) return false;
  memcpy(curTop, top, 3);
  memcpy(curBot, bot, 3);
  buildBg();
  forceRepaint = true;
  return true;
}

// ================= text helpers =================
String asciiOnly(const char *in) {
  String out;
  for (const uint8_t *p = (const uint8_t *)in; *p; p++) {
    if (*p < 0x80) out += (char)*p;
    else if ((*p & 0xC0) == 0xC0) out += '?';
  }
  return out;
}

String fitText(const String &s, int maxW, int font) {
  if (tft.textWidth(s, font) <= maxW) return s;
  String t = s;
  while (t.length() > 0 && tft.textWidth(t + "...", font) > maxW) t.remove(t.length() - 1);
  return t + "...";
}

// Splits a title over two lines. Returns true if it does NOT fit in two lines (-> scroll it).
bool splitTitle(const String &s, int w, String &l1, String &l2) {
  if (tft.textWidth(s, 4) <= w) { l1 = s; l2 = ""; return false; }
  int cut = 0;
  for (int i = 1; i <= (int)s.length(); i++) {
    if (tft.textWidth(s.substring(0, i), 4) > w) break;
    cut = i;
  }
  int sp = s.lastIndexOf(' ', cut);
  if (sp > 0) { l1 = s.substring(0, sp); l2 = s.substring(sp + 1); }
  else        { l1 = s.substring(0, cut); l2 = s.substring(cut); }
  return tft.textWidth(l2, 4) > w;
}

String fmtTime(long ms) {
  if (ms < 0) ms = 0;
  long s = ms / 1000;
  char b[12];
  snprintf(b, sizeof(b), "%ld:%02ld", s / 60, s % 60);
  return String(b);
}

long curPos() {
  long p = posMs;
  if (isPlaying) p += (long)(millis() - posAt);
  if (p < 0) p = 0;
  if (durMs > 0 && p > durMs) p = durMs;
  return p;
}

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// ================= drawing =================
void paintBg(int x, int y, int w, int h) {
  for (int r = y; r < y + h && r < SCR_H; r++) tft.drawFastHLine(x, r, w, rowCol[r]);
}

void sprBg(TFT_eSprite &s, int w, int h, int y) {
  for (int r = 0; r < h; r++) s.drawFastHLine(0, r, w, rowCol[constrain(y + r, 0, SCR_H - 1)]);
}

// Draws one line of text over the gradient with no flicker (buffer is pushed in one go)
void drawLine(int x, int y, const String &str, int font, uint16_t col) {
  sprBg(txtSpr, TEXT_W, SPR_H, y);
  if (str.length()) { txtSpr.setTextColor(col); txtSpr.drawString(str, 0, 0, font); }
  txtSpr.pushSprite(x, y);
}

void drawTimeLabel(int x, const String &str, bool right) {
  sprBg(timeSpr, TIME_W, TIME_H, TIME_Y);
  timeSpr.setTextColor(C_GREY);
  int tx = right ? TIME_W - timeSpr.textWidth(str, 2) : 0;
  timeSpr.drawString(str, tx, 0, 2);
  timeSpr.pushSprite(x, TIME_Y);
}

void drawArtPlaceholder() {
  paintBg(ART_X, ART_Y, ART_SZ, ART_SZ);
  tft.fillRoundRect(ART_X, ART_Y, ART_SZ, ART_SZ, 6, cPlace);
  int cx = ART_X + ART_SZ / 2, cy = ART_Y + ART_SZ / 2;
  tft.fillCircle(cx - 12, cy + 16, 8, C_GREY);
  tft.fillRect(cx - 5, cy - 26, 4, 42, C_GREY);
  tft.fillRect(cx - 5, cy - 26, 20, 7, C_GREY);
}

void drawArt() {
  if (!artValid) { drawArtPlaceholder(); return; }
  uint16_t w = 0, h = 0;
  if (TJpgDec.getJpgSize(&w, &h, jpgBuf, artLen) != JDR_OK || w > ART_SZ || h > ART_SZ) {
    Serial.printf("[art] unusable image (%ux%u). Is the bridge set to 128?\n", w, h);
    artValid = false;
    drawArtPlaceholder();
    return;
  }
  TJpgDec.drawJpg(ART_X + (ART_SZ - w) / 2, ART_Y + (ART_SZ - h) / 2, jpgBuf, artLen);
}

void drawButton(int i, bool pressed) {
  Btn &b = btns[i];
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, pressed ? C_BTN_ON : cBtn);
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  uint16_t c = pressed ? TFT_BLACK : TFT_WHITE;
  if (i == 0) {
    tft.fillTriangle(cx + 7, cy - 9, cx + 7, cy + 9, cx - 5, cy, c);
    tft.fillRect(cx - 10, cy - 9, 3, 18, c);
  } else if (i == 2) {
    tft.fillTriangle(cx - 7, cy - 9, cx - 7, cy + 9, cx + 5, cy, c);
    tft.fillRect(cx + 8, cy - 9, 3, 18, c);
  } else if (isPlaying) {
    tft.fillRect(cx - 8, cy - 9, 6, 18, c);
    tft.fillRect(cx + 2, cy - 9, 6, 18, c);
  } else {
    tft.fillTriangle(cx - 6, cy - 10, cx - 6, cy + 10, cx + 9, cy, c);
  }
}

void drawButtons() {
  for (int i = 0; i < 3; i++) drawButton(i, i == pressedBtn);
}

void drawScrollTitle() {
  sprBg(txtSpr, TEXT_W, SPR_H, TITLE_YS);
  txtSpr.setTextColor(TFT_WHITE);
  txtSpr.drawString(title, -scrollOff, 0, 4);
  if (scrollOff > 0) txtSpr.drawString(title, -scrollOff + titleW + SCROLL_GAP, 0, 4);
  txtSpr.pushSprite(TEXT_X, TITLE_YS);
}

void updateScroll() {
  if (!titleScroll || drawnMode != M_PLAY) return;
  uint32_t now = millis();
  if (now < scrollNext) return;
  scrollNext = now + SCROLL_STEP_MS;
  if (now < scrollHold) return;                     // pausing at the start
  scrollOff += SCROLL_PX;
  if (scrollOff >= titleW + SCROLL_GAP) { scrollOff = 0; scrollHold = now + SCROLL_HOLD_MS; }
  drawScrollTitle();
}

void drawText(Mode m) {
  if (m == M_PLAY) {
    String l1, l2;
    titleScroll = splitTitle(title, TEXT_W, l1, l2);
    if (titleScroll) {
      drawLine(TEXT_X, TITLE_Y1, "", 4, TFT_WHITE);       // clear both title lines first
      drawLine(TEXT_X, TITLE_Y2, "", 4, TFT_WHITE);
      titleW = tft.textWidth(title, 4);
      scrollOff = 0;
      scrollNext = 0;
      scrollHold = millis() + SCROLL_HOLD_MS;
      drawScrollTitle();
    } else {
      drawLine(TEXT_X, TITLE_Y1, l1, 4, TFT_WHITE);
      drawLine(TEXT_X, TITLE_Y2, l2, 4, TFT_WHITE);
    }
    drawLine(TEXT_X, ARTIST_Y, fitText(artist, TEXT_W, 2), 2, TFT_WHITE);
    drawLine(TEXT_X, ALBUM_Y,  fitText(album,  TEXT_W, 2), 2, TFT_WHITE);
  } else {
    titleScroll = false;
    const char *a = "Nothing", *b = "playing";
    if (m == M_OFFLINE) { a = "Bridge"; b = "offline"; }
    if (m == M_NOWIFI)  { a = "Wi-Fi";  b = "lost"; }
    drawLine(TEXT_X, 14, a, 4, TFT_WHITE);
    drawLine(TEXT_X, 44, b, 4, TFT_WHITE);
    if (m == M_OFFLINE || m == M_NOWIFI) drawLine(TEXT_X, 84, "Reconnecting...", 2, C_GREY);
  }
}

void drawProgress() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 200) return;
  lastCheck = millis();

  if (durMs <= 0) {
    if (progVisible) { paintBg(0, BAR_Y - 2, SCR_W, 28); progVisible = false; }
    return;
  }
  if (!progVisible) { progVisible = true; lastFill = -1; lastSec = -1; durShown = -1; }

  long p = curPos();
  int fill = (int)((int64_t)p * BAR_W / durMs);
  long sec = p / 1000;

  if (fill != lastFill) {
    lastFill = fill;
    if (fill > 0) tft.fillRect(BAR_X, BAR_Y, fill, BAR_H, TFT_WHITE);
    tft.fillRect(BAR_X + fill, BAR_Y, BAR_W - fill, BAR_H, cTrack);
  }
  if (sec != lastSec) { lastSec = sec; drawTimeLabel(BAR_X, fmtTime(p), false); }
  if (durMs != durShown) { durShown = durMs; drawTimeLabel(BAR_X + BAR_W - TIME_W, fmtTime(durMs), true); }
}

// ================= artwork fetch =================
bool fetchArt(const String &url) {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("[art] HTTP %d\n", code); http.end(); return false; }
  int cl = http.getSize();
  if (cl <= 0 || cl > MAX_JPG) { Serial.printf("[art] bad length %d\n", cl); http.end(); return false; }

  WiFiClient *s = http.getStreamPtr();
  size_t got = 0;
  uint32_t last = millis();
  while (got < (size_t)cl && millis() - last < 5000) {
    int av = s->available();
    if (av > 0) {
      got += s->readBytes(jpgBuf + got, min((size_t)av, (size_t)cl - got));
      last = millis();
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();
  artLen = got;
  return got == (size_t)cl;
}

void updateArt() {
  if (drawnMode != M_PLAY) return;

  if (wantKey.length() == 0) {                         // the bridge says there is no art
    if (artValid) { artValid = false; doneKey = ""; drawArtPlaceholder(); }
    if (millis() - trackChangedAt > NOART_GRACE_MS) setBg(DEF_TOP, DEF_BOT);
    return;
  }
  if (wantKey != doneKey) {
    if (artRetries >= 3 || millis() < nextArtTry) return;
    artValid = false;
    uint32_t t0 = millis();
    if (fetchArt(wantUrl)) {
      doneKey = wantKey;
      artValid = true;
      bool changed = pendHave ? setBg(pendTop, pendBot) : setBg(DEF_TOP, DEF_BOT);
      if (!changed) drawArt();                         // otherwise the full repaint draws it
      Serial.printf("[art] ok %u bytes in %lu ms, bg %s, heap=%u\n",
                    (unsigned)artLen, (unsigned long)(millis() - t0),
                    changed ? "changed" : "same", (unsigned)ESP.getFreeHeap());
    } else {
      artRetries++;
      nextArtTry = millis() + 2000;
      Serial.printf("[art] fetch failed (try %d of 3)\n", artRetries);
      if (artRetries >= 3) drawArtPlaceholder();
    }
  }
}

// ================= render =================
void render() {
  Mode m = !wifiUp ? M_NOWIFI : (!wsUp ? M_OFFLINE : (title.length() == 0 ? M_IDLE : M_PLAY));
  if (m != drawnMode || forceRepaint) {
    if (m != drawnMode && m != M_PLAY) {
      artValid = false; doneKey = "";
      setBg(DEF_TOP, DEF_BOT);
    }
    drawnMode = m;
    forceRepaint = false;
    paintBg(0, 0, SCR_W, SCR_H);
    if (artValid && m == M_PLAY) drawArt(); else drawArtPlaceholder();
    drawButtons();
    textDirty = true;
    iconDirty = false;
    progVisible = false; lastFill = -1; lastSec = -1; durShown = -1;
  }
  if (textDirty) { textDirty = false; drawText(m); }
  if (iconDirty) { iconDirty = false; drawButton(1, pressedBtn == 1); }
  if (m == M_PLAY) { drawProgress(); updateArt(); }
}

// ================= backlight =================
void wakeBacklight() {
  lastActivity = millis();
  screenOff = false;
  if (blNow < BL_FULL) { blNow = BL_FULL; ledcWrite(0, blNow); }   // instant, no fade
}

void updateBacklight() {
  static uint32_t lastStep = 0;
  uint32_t now = millis();
  if (now - lastStep < 15) return;
  lastStep = now;
  if (wsUp && isPlaying) lastActivity = now;          // playing keeps the screen bright
  uint32_t idle = now - lastActivity;
  screenOff = (idle > SLEEP_AFTER_MS);
  int target = screenOff ? 0 : (idle > DIM_AFTER_MS ? BL_DIM : BL_FULL);
  int next = blNow;
  if (next < target)      next = min(next + 15, target);
  else if (next > target) next = max(next - 2, target);
  if (next != blNow) { blNow = next; ledcWrite(0, blNow); }
}

// ================= WebSocket =================
void handleMessage(uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;

  String nt = asciiOnly(doc["title"]  | "");
  String na = asciiOnly(doc["artist"] | "");
  String nb = asciiOnly(doc["album"]  | "");
  String ns = doc["state"] | "";
  long   np = doc["position_ms"] | 0L;
  long   nd = doc["duration_ms"] | 0L;
  String nurl = doc["art_url"].is<const char *>() ? String(doc["art_url"].as<const char *>()) : String("");

  JsonArray ac = doc["art_colors"].as<JsonArray>();
  pendHave = false;
  if (!ac.isNull() && ac.size() >= 6) {
    darkenColour(ac[0].as<int>(), ac[1].as<int>(), ac[2].as<int>(), 0.55f, 0.10f, 0.30f, pendTop);
    darkenColour(ac[3].as<int>(), ac[4].as<int>(), ac[5].as<int>(), 0.30f, 0.03f, 0.16f, pendBot);
    pendHave = true;
  }

  String nkey = nurl.length() ? nurl + "|" + nt + "|" + na : String("");
  if (nkey != wantKey) { wantKey = nkey; wantUrl = nurl; artRetries = 0; nextArtTry = 0; }

  bool sameTrack = (nt == title && na == artist);
  bool nowPlaying = (ns == "playing");

  if (!sameTrack || nb != album) {
    title = nt; artist = na; album = nb;
    textDirty = true;
    lastFill = -1; lastSec = -1; durShown = -1;
  }
  if (!sameTrack) { trackChangedAt = millis(); lastActivity = millis(); }
  if (nowPlaying != isPlaying) { isPlaying = nowPlaying; iconDirty = true; lastActivity = millis(); }
  stateStr = ns;

  long est = curPos();
  bool ignoreBackward = sameTrack && isPlaying && np < est && (est - np) < 2000 && nd == durMs;
  if (!ignoreBackward) { posMs = np; posAt = millis(); }
  if (!isPlaying) { posMs = np; posAt = millis(); }
  durMs = nd;
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:    wsUp = true; lastActivity = millis(); Serial.println("[WS] connected"); break;
    case WStype_DISCONNECTED:
      if (wsUp) Serial.println("[WS] disconnected (retrying)");
      wsUp = false;
      break;
    case WStype_TEXT:         handleMessage(payload, length); break;
    default: break;
  }
}

void sendCmd(const char *cmd) {
  if (!wsUp) { Serial.printf("[cmd] %s NOT sent (bridge not connected)\n", cmd); return; }
  ws.sendTXT(String("{\"cmd\":\"") + cmd + "\"}");
  Serial.printf("[cmd] sent %s\n", cmd);
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD display polish ===");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  // Backlight PWM
  ledcSetup(0, 5000, 8);
  ledcAttachPin(BL_PIN, 0);
  ledcWrite(0, BL_FULL);

  // Allocate the buffers first, while the heap is still unfragmented
  if (!txtSpr.createSprite(TEXT_W, SPR_H) || !timeSpr.createSprite(TIME_W, TIME_H)) {
    Serial.println("sprite alloc failed!");
    while (true) delay(1000);
  }
  jpgBuf = (uint8_t *)malloc(MAX_JPG);
  if (!jpgBuf) { Serial.println("malloc failed!"); while (true) delay(1000); }
  buildBg();

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  tft.setTextColor(TFT_WHITE, C_BG);
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
  Serial.printf("Wi-Fi OK. IP=%s  heap=%u\n", WiFi.localIP().toString().c_str(), (unsigned)ESP.getFreeHeap());

  ws.begin(BRIDGE_IP, BRIDGE_PORT, "/media");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(2000);
  ws.enableHeartbeat(15000, 3000, 2);

  lastActivity = millis();
}

// ------- Wi-Fi supervision and heap guard --------
uint32_t wifiDownSince = 0, lastWifiRetry = 0;

void serviceWifi() {
  bool up = (WiFi.status() == WL_CONNECTED);
  uint32_t now = millis();

  if (up) {
    if (!wifiUp) {
      wifiUp = true;
      lastActivity = now;                               // wake the screen
      Serial.printf("[wifi] back. IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    return;
  }

  if (wifiUp) {                                         // it just dropped
    wifiUp = false;
    wifiDownSince = lastWifiRetry = now;
    wsUp = false;
    ws.disconnect();                                    // don't leave socket unclosed
    Serial.println("[wifi] LOST");
    return;
  }

  if (now - wifiDownSince > WIFI_RESTART_MS) {
    Serial.println("[wifi] down too long, restarting");
    delay(200);
    ESP.restart();
  }
  if (now - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = now;
    Serial.printf("[wifi] retrying (down %lus)\n", (unsigned long)((now - wifiDownSince) / 1000));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void heapGuard() {
  static uint32_t last = 0;
  static int lowCount = 0;
  if (millis() - last < 60000) return;
  last = millis();
  uint32_t freeH = ESP.getFreeHeap();
  uint32_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[heap] free=%u largest=%u min=%u uptime=%lum\n",
                (unsigned)freeH, (unsigned)big, (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)(millis() / 60000));
  if (freeH < HEAP_RESTART_BELOW) {
    if (++lowCount >= 2) { Serial.println("[heap] critically low, restarting"); delay(200); ESP.restart(); }
  } else {
    lowCount = 0;
  }
}

void loop() {
  ws.loop();

  static bool wasTouched = false;
  static bool swallowTouch = false;       // true while a wake-up touch is still held down
  static uint32_t lastPress = 0;
  if (ts.touched()) {
    if (screenOff) swallowTouch = true;   // screen was fully off: this touch only wakes it
    wakeBacklight();                      // any touch wakes the screen
    if (!swallowTouch) {
      TS_Point p = ts.getPoint();
      int sx = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCR_W), 0, SCR_W - 1);
      int sy = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCR_H), 0, SCR_H - 1);
      int hit = -1;
      for (int i = 0; i < 3; i++)
        if (sx >= btns[i].x && sx < btns[i].x + btns[i].w &&
            sy >= btns[i].y && sy < btns[i].y + btns[i].h) hit = i;

      if (!wasTouched && hit >= 0 && millis() - lastPress > 250) {  // dim mode: touch still acts
        lastPress = millis();
        sendCmd(btns[hit].cmd);
      }
      if (hit != pressedBtn) { pressedBtn = hit; drawButtons(); }
      wasTouched = true;
    }
  } else {
    swallowTouch = false;
    if (wasTouched) {
      wasTouched = false;
      pressedBtn = -1;
      drawButtons();
    }
  }

  // Test hook: type 'w' in the Serial Monitor to simulate a Wi-Fi drop
  if (Serial.available() && Serial.read() == 'w') {
    Serial.println("[test] forcing Wi-Fi drop");
    WiFi.disconnect();
  }

  serviceWifi();
  render();
  updateScroll();
  updateBacklight();
  heapGuard();

  static uint32_t lastStat = 0;
  if (millis() - lastStat > 10000) {
    lastStat = millis();
    Serial.printf("[status] wifi=%s ws=%s bl=%d heap=%u minHeap=%u\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN", wsUp ? "up" : "DOWN", blNow,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  }
  delay(5);
}
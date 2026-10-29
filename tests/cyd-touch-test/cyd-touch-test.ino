#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// Touch pins
#define T_IRQ  36
#define T_MOSI 32
#define T_MISO 39
#define T_CLK  25
#define T_CS   33

// Starting calibration
#define TOUCH_MIN_X 360
#define TOUCH_MAX_X 3680
#define TOUCH_MIN_Y 370
#define TOUCH_MAX_Y 3800

#define SCR_W 320
#define SCR_H 240

// Three button zones along the bottom (x, y, w, h)
struct Btn { int x, y, w, h; const char *label; };
Btn btns[3] = {
  {  10, 170, 90, 60, "PREV" },
  { 115, 170, 90, 60, "PLAY" },
  { 220, 170, 90, 60, "NEXT" },
};

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);

void drawButtons(int pressed = -1) {
  for (int i = 0; i < 3; i++) {
    uint16_t fill = (i == pressed) ? TFT_GREEN : TFT_DARKGREY;
    tft.fillRoundRect(btns[i].x, btns[i].y, btns[i].w, btns[i].h, 8, fill);
    tft.setTextColor(TFT_WHITE, fill);
    tft.drawCentreString(btns[i].label, btns[i].x + btns[i].w / 2, btns[i].y + btns[i].h / 2 - 8, 2);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Touch test: tap anywhere", 10, 10, 2);
  drawButtons();
  Serial.println("=== CYD touch test ===");
}

void loop() {
  static int lastPressed = -1;
  static bool wasTouched = false;

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int sx = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCR_W), 0, SCR_W - 1);
    int sy = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCR_H), 0, SCR_H - 1);

    int hit = -1;
    for (int i = 0; i < 3; i++) {
      if (sx >= btns[i].x && sx < btns[i].x + btns[i].w &&
          sy >= btns[i].y && sy < btns[i].y + btns[i].h) hit = i;
    }

    if (!wasTouched) {
      Serial.printf("raw x=%d y=%d z=%d -> screen x=%d y=%d  button=%s\n",
                    p.x, p.y, p.z, sx, sy, hit >= 0 ? btns[hit].label : "none");
    }

    tft.fillCircle(sx, sy, 2, TFT_YELLOW);   // dot where it thinks you touched
    if (hit != lastPressed) { drawButtons(hit); lastPressed = hit; }
    wasTouched = true;
  } else if (wasTouched) {
    wasTouched = false;
    lastPressed = -1;
    drawButtons();
  }
  delay(10);
}
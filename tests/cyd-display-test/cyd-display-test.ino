#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== CYD display test ===");
  Serial.printf("Chip: %s rev %d, %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());

  pinMode(21, OUTPUT);       // backlight, in case TFT_BL isn't driving it
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);        // landscape, 320x240
  Serial.printf("Display size: %d x %d\n", tft.width(), tft.height());

  // 1. Colour bars, each labelled with the colour it SHOULD be
  const uint16_t cols[]  = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK};
  const char *names[]    = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
  int bw = tft.width() / 5;
  for (int i = 0; i < 5; i++) {
    tft.fillRect(i * bw, 0, bw, 90, cols[i]);
    tft.setTextColor(i == 3 ? TFT_BLACK : TFT_WHITE, cols[i]);
    tft.drawCentreString(names[i], i * bw + bw / 2, 38, 2);
  }

  // 2. Text
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Hello CYD", 10, 100, 4);
  tft.drawString("320x240 landscape", 10, 130, 2);

  // 3. Shapes
  tft.drawRect(10, 160, 60, 50, TFT_YELLOW);
  tft.fillCircle(110, 185, 25, TFT_CYAN);
  tft.fillTriangle(150, 210, 180, 160, 210, 210, TFT_MAGENTA);
  tft.fillRoundRect(225, 160, 80, 50, 8, TFT_ORANGE);

  // 4. Corner markers to confirm orientation (top-left white, bottom-right green)
  tft.fillRect(0, 0, 6, 6, TFT_WHITE);
  tft.fillRect(tft.width() - 6, tft.height() - 6, 6, 6, TFT_GREEN);

  // 5. Generated "test image" (gradient)
  static uint16_t row[64];
  for (int y = 0; y < 24; y++) {
    for (int x = 0; x < 64; x++) row[x] = tft.color565(x * 4, y * 10, 255 - x * 4);
    tft.pushImage(246, 214 + y, 64, 1, row);
  }

  Serial.println("Draw complete. Check the screen.");
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 2000) {
    t = millis();
    Serial.printf("alive, heap=%u\n", ESP.getFreeHeap());
  }
}
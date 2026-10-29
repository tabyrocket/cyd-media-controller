#define USER_SETUP_ID 2432

// ===== CHANGE THESE IF THE TEST SCREEN LOOKS WRONG =====
// 1 = ILI9341_2 (most common CYD)   2 = ST7789   3 = ILI9341 (plain)
#define CYD_DRIVER 1
// 1 if colours look inverted (black looks white), otherwise 0
#define CYD_INVERT 0
// ========================================================

#if CYD_DRIVER == 1
  #define ILI9341_2_DRIVER
#elif CYD_DRIVER == 2
  #define ST7789_DRIVER
#else
  #define ILI9341_DRIVER
#endif

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#if CYD_INVERT
  #define TFT_INVERSION_ON
#else
  #define TFT_INVERSION_OFF
#endif

// Display SPI pins (typical CYD, to be verified)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   21          // some units use 27
#define TFT_BACKLIGHT_ON HIGH

#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#include "ST7789LCDDisplay.h"

#ifdef ST7789_PORTRAIT_PROFILE
  #include "DisplayViewport.h"
#endif

#ifndef PIN_TFT_MISO
  #define PIN_TFT_MISO -1
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

#ifndef DISPLAY_SCALE_X
  #define DISPLAY_SCALE_X 2.5f // 320 / 128
#endif

#ifndef DISPLAY_SCALE_Y
  #define DISPLAY_SCALE_Y 3.75f // 240 / 64
#endif

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320

#ifdef ST7789_PORTRAIT_PROFILE
  #ifndef ST7789_PORTRAIT_TEXT_SCALE
    #define ST7789_PORTRAIT_TEXT_SCALE 2
  #endif

static DisplayViewport::Geometry portraitViewport(int16_t physical_width, int16_t physical_height) {
  return {128, 64, physical_width, physical_height};
}
#endif

bool ST7789LCDDisplay::i2c_probe(TwoWire& wire, uint8_t addr) {
  return true;
}

// Color scheme
ColorVal UIColor::window_bkg = ST77XX_WHITE;
ColorVal UIColor::title_bkg = ST77XX_BLUE;
ColorVal UIColor::title_txt = ST77XX_WHITE;
ColorVal UIColor::primary_txt = ST77XX_BLACK;
ColorVal UIColor::secondary_txt = (18 << 11) | (36 << 5) | 18;  // mid-gray
ColorVal UIColor::warning_txt = ST77XX_ORANGE;
ColorVal UIColor::popup_bkg = ST77XX_CYAN;
ColorVal UIColor::popup_txt = ST77XX_BLACK;
ColorVal UIColor::corp_blue = 0x001A;

bool ST7789LCDDisplay::begin() {
  if (!_isOn) {
    if (_peripher_power) {
      _peripher_power->claim();
    #ifdef HELTEC_V4_R8_TFT
      delay(100);
    #endif
    }

    if (PIN_TFT_LEDA_CTL != -1) {
      pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
    #ifdef HELTEC_V4_R8_TFT
      digitalWrite(PIN_TFT_LEDA_CTL, !PIN_TFT_LEDA_CTL_ACTIVE);
    #else
      digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    #endif
    }

    // Im not sure if this is just a t-deck problem or not, if your display is slow try this.
    #if defined(LILYGO_TDECK) || defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_V4_R8_TFT)
      displaySPI.begin(PIN_TFT_SCL, PIN_TFT_MISO, PIN_TFT_SDA, PIN_TFT_CS);
    #endif

    display.init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    display.setRotation(DISPLAY_ROTATION);

    display.setSPISpeed(40e6);

    display.fillScreen(ST77XX_BLACK);
    display.setTextColor(ST77XX_WHITE);
  #ifdef ST7789_PORTRAIT_PROFILE
    _logical_text_size = 1;
    display.setTextSize(ST7789_PORTRAIT_TEXT_SCALE);
    display.setTextWrap(false);
  #else
    display.setTextSize(2 * DISPLAY_SCALE_X);
  #endif
    display.cp437(true); // Use full 256 char 'Code Page 437' font

  #ifdef HELTEC_V4_R8_TFT
    if (PIN_TFT_LEDA_CTL != -1) {
      digitalWrite(PIN_TFT_LEDA_CTL, PIN_TFT_LEDA_CTL_ACTIVE);
    }
  #endif

    _isOn = true;
  }

  return true;
}

void ST7789LCDDisplay::turnOn() {
  ST7789LCDDisplay::begin();
}

void ST7789LCDDisplay::turnOff() {
  if (_isOn) {
    if (PIN_TFT_LEDA_CTL != -1) {
    #ifdef HELTEC_V4_R8_TFT
      digitalWrite(PIN_TFT_LEDA_CTL, !PIN_TFT_LEDA_CTL_ACTIVE);
    #else
      digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    #endif
    }
    if (PIN_TFT_RST != -1) {
      digitalWrite(PIN_TFT_RST, LOW);
    }
  #ifndef HELTEC_V4_R8_TFT
    if (PIN_TFT_LEDA_CTL != -1) {
      digitalWrite(PIN_TFT_LEDA_CTL, LOW);
    }
  #endif
    _isOn = false;

    if (_peripher_power) _peripher_power->release();
  }
}

void ST7789LCDDisplay::clear() {
  display.fillScreen(ST77XX_BLACK);
}

void ST7789LCDDisplay::startFrame(ColorVal bkg) {
  display.fillScreen(bkg);
  display.setTextColor(_color = UIColor::primary_txt);
#ifdef ST7789_PORTRAIT_PROFILE
  _logical_text_size = 1;
  display.setTextSize(ST7789_PORTRAIT_TEXT_SCALE);
#else
  display.setTextSize(1 * DISPLAY_SCALE_X); // This one affects size of Please wait... message
#endif
  display.cp437(true); // Use full 256 char 'Code Page 437' font
}

void ST7789LCDDisplay::setTextSize(int sz) {
#ifdef ST7789_PORTRAIT_PROFILE
  _logical_text_size = sz > 0 ? static_cast<uint8_t>(sz) : 1;
  display.setTextSize(_logical_text_size * ST7789_PORTRAIT_TEXT_SCALE);
#else
  display.setTextSize(sz * DISPLAY_SCALE_X);
#endif
}

void ST7789LCDDisplay::setColor(ColorVal c) {
  display.setTextColor(_color = c);
}

void ST7789LCDDisplay::setCursor(int x, int y) {
#ifdef ST7789_PORTRAIT_PROFILE
  DisplayViewport::Geometry viewport = portraitViewport(display.width(), display.height());
  display.setCursor(viewport.mapX(x), viewport.mapY(y));
#else
  display.setCursor(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y);
#endif
}

void ST7789LCDDisplay::print(const char* str) {
#ifdef ST7789_PORTRAIT_PROFILE
  int16_t cursor_x = display.getCursorX();
  if (cursor_x < 0) {
    cursor_x = 0;
    display.setCursor(cursor_x, display.getCursorY());
  }
  if (cursor_x >= display.width()) return;

  printFitted(str, static_cast<uint16_t>(display.width() - cursor_x));
#else
  display.print(str);
#endif
}

void ST7789LCDDisplay::fillRect(int x, int y, int w, int h) {
#ifdef ST7789_PORTRAIT_PROFILE
  DisplayViewport::Geometry viewport = portraitViewport(display.width(), display.height());
  display.fillRect(viewport.mapX(x), viewport.mapY(y), viewport.spanX(x, w), viewport.spanY(y, h), _color);
#else
  display.fillRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
#endif
}

void ST7789LCDDisplay::drawRect(int x, int y, int w, int h) {
#ifdef ST7789_PORTRAIT_PROFILE
  DisplayViewport::Geometry viewport = portraitViewport(display.width(), display.height());
  display.drawRect(viewport.mapX(x), viewport.mapY(y), viewport.spanX(x, w), viewport.spanY(y, h), _color);
#else
  display.drawRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
#endif
}

void ST7789LCDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  uint8_t byteWidth = (w + 7) / 8;

#ifdef ST7789_PORTRAIT_PROFILE
  DisplayViewport::Geometry viewport = portraitViewport(display.width(), display.height());
  int16_t physical_y = viewport.mapY(y);

  for (int j = 0; j < h; j++) {
    // Scale both bitmap axes from the logical X ratio so logo pixels stay square.
    int16_t y0 = physical_y + viewport.mapX(j);
    int16_t y1 = physical_y + viewport.mapX(j + 1);
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      bool pixelOn = byte & (0x80 >> (i & 7));

      if (pixelOn) {
        int16_t x0 = viewport.mapX(x + i);
        int16_t x1 = viewport.mapX(x + i + 1);
        display.fillRect(x0, y0, x1 - x0, y1 - y0, _color);
      }
    }
  }
#else
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      bool pixelOn = byte & (0x80 >> (i & 7));

      if (pixelOn) {
        for (int dy = 0; dy < DISPLAY_SCALE_X; dy++) {
          for (int dx = 0; dx < DISPLAY_SCALE_X; dx++) {
            display.drawPixel(x * DISPLAY_SCALE_X + i * DISPLAY_SCALE_X + dx, y * DISPLAY_SCALE_Y + j * DISPLAY_SCALE_X + dy, _color);
          }
        }
      }
    }
  }
#endif
}

uint16_t ST7789LCDDisplay::getTextWidth(const char* str) {
#ifdef ST7789_PORTRAIT_PROFILE
  uint8_t physical_scale = selectTextScale(str, display.width());
  uint16_t physical_width = measureTextWidth(str, physical_scale);
  if (physical_width > display.width()) physical_width = display.width();

  DisplayViewport::Geometry viewport = portraitViewport(display.width(), display.height());
  return viewport.logicalWidthForPhysical(physical_width);
#else
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);

  return w / DISPLAY_SCALE_X;
#endif
}

#ifdef ST7789_PORTRAIT_PROFILE
uint16_t ST7789LCDDisplay::measureTextWidth(const char* str, uint8_t physical_scale) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(physical_scale);
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
}

uint8_t ST7789LCDDisplay::selectTextScale(const char* str, uint16_t available_width) {
  uint16_t width_at_scale_one = measureTextWidth(str, 1);
  uint8_t preferred_scale = _logical_text_size * ST7789_PORTRAIT_TEXT_SCALE;
  return DisplayViewport::selectTextScale(width_at_scale_one, preferred_scale, _logical_text_size,
                                          available_width);
}

void ST7789LCDDisplay::printFitted(const char* str, uint16_t available_width) {
  if (!str || available_width == 0) return;

  uint8_t physical_scale = selectTextScale(str, available_width);
  if (measureTextWidth(str, physical_scale) <= available_width) {
    display.print(str);
    return;
  }

  static const char* ellipsis = "...";
  uint16_t ellipsis_width = measureTextWidth(ellipsis, physical_scale);
  if (ellipsis_width > available_width) return;

  char fitted[256];
  size_t len = strlen(str);
  if (len > sizeof(fitted) - 4) len = sizeof(fitted) - 4;
  memcpy(fitted, str, len);
  fitted[len] = 0;

  while (len > 0 && measureTextWidth(fitted, physical_scale) + ellipsis_width > available_width) {
    --len;
    while (len > 0 && (static_cast<uint8_t>(fitted[len]) & 0xC0) == 0x80)
      --len;
    fitted[len] = 0;
  }

  memcpy(fitted + len, ellipsis, 4);
  display.setTextSize(physical_scale);
  display.print(fitted);
}
#endif

void ST7789LCDDisplay::endFrame() {
  // display.display();
}

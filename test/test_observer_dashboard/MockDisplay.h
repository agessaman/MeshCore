#pragma once

#include "helpers/ui/DisplayDriver.h"
#include "helpers/ui/DisplayViewport.h"

#include <string>
#include <vector>

// A DisplayDriver that records physical-pixel draw calls instead of pushing
// them at a panel, reproducing the two real ST7789LCDDisplay coordinate and
// text-metric profiles:
//
//   portrait  (ST7789_PORTRAIT_PROFILE)  240x320, DisplayViewport mapping,
//                                        physical text scale = logical * 2
//   landscape (default)                  320x240, x * 2.5 / y * 3.75,
//                                        physical text scale = (int)(logical * 2.5)
//
// Text extents are recorded at the size the layout *asked* for, so a row that
// only fits because the driver would silently shrink or clip it still shows up
// as an out-of-bounds op.
class MockDisplay : public DisplayDriver {
public:
  enum Mode { PORTRAIT, LANDSCAPE };

  struct Op {
    enum Kind { FILL, RECT, TEXT } kind;
    int x, y, w, h;          // physical pixels
    ColorVal color;
    std::string text;
    int logical_size;
    bool scale_fallback;     // portrait driver would have shrunk this string
  };

  explicit MockDisplay(Mode mode)
      : DisplayDriver(128, 64), _mode(mode), _on(true), _color(0), _size(1), _cx(0), _cy(0) {}

  std::vector<Op> ops;

  int panelWidth() const { return _mode == PORTRAIT ? 240 : 320; }
  int panelHeight() const { return _mode == PORTRAIT ? 320 : 240; }

  void reset() { ops.clear(); }

  // --- DisplayDriver ---
  bool isOn() override { return _on; }
  void turnOn() override { _on = true; }
  void turnOff() override { _on = false; }
  void clear() override { ops.clear(); }
  void startFrame(ColorVal bkg = UIColor::window_bkg) override {
    ops.clear();
    ops.push_back(Op{Op::FILL, 0, 0, panelWidth(), panelHeight(), bkg, "", 1, false});
    _size = 1;
  }
  void setTextSize(int sz) override { _size = sz > 0 ? sz : 1; }
  void setColor(ColorVal c) override { _color = c; }
  void setCursor(int x, int y) override { _cx = x; _cy = y; }

  void print(const char* str) override {
    if (!str || !*str) return;
    int n = (int)strlen(str);
    int scale = physicalScale(_size);
    int px = mapX(_cx), py = mapY(_cy);
    bool fallback = false;
    if (_mode == PORTRAIT) {
      int available = panelWidth() - px;
      if (n * 6 * scale > available) {
        fallback = true;   // the real driver drops to the minimum scale here
      }
    }
    ops.push_back(Op{Op::TEXT, px, py, n * 6 * scale, 8 * scale, _color, std::string(str), _size,
                     fallback});
    _cx += (int)((n * 6 * scale) / xScale());
  }

  void fillRect(int x, int y, int w, int h) override {
    ops.push_back(Op{Op::FILL, mapX(x), mapY(y), spanX(x, w), spanY(y, h), _color, "", _size,
                     false});
  }
  void drawRect(int x, int y, int w, int h) override {
    ops.push_back(Op{Op::RECT, mapX(x), mapY(y), spanX(x, w), spanY(y, h), _color, "", _size,
                     false});
  }
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  void endFrame() override {}

  uint16_t getTextWidth(const char* str) override {
    if (!str) return 0;
    int n = (int)strlen(str);
    int scale = physicalScale(_size);
    if (_mode == PORTRAIT) {
      // Mirrors ST7789LCDDisplay::getTextWidth(): measure at the scale the
      // driver would pick, clamp to the panel, convert back to logical.
      if (n * 6 * scale > panelWidth()) scale = _size;
      int w = n * 6 * scale;
      if (w > panelWidth()) w = panelWidth();
      DisplayViewport::Geometry g{128, 64, 240, 320};
      return g.logicalWidthForPhysical((uint16_t)w);
    }
    return (uint16_t)((n * 6 * scale) / 2.5f);
  }

private:
  Mode _mode;
  bool _on;
  ColorVal _color;
  int _size, _cx, _cy;

  float xScale() const { return _mode == PORTRAIT ? (240.0f / 128.0f) : 2.5f; }

  int physicalScale(int logical) const {
    return _mode == PORTRAIT ? logical * 2 : (int)(uint8_t)(logical * 2.5f);
  }

  int mapX(int x) const {
    return _mode == PORTRAIT ? (int)((int32_t)x * 240 / 128) : (int)(x * 2.5f);
  }
  int mapY(int y) const {
    return _mode == PORTRAIT ? (int)((int32_t)y * 320 / 64) : (int)(y * 3.75f);
  }
  int spanX(int x, int w) const { return mapX(x + w) - mapX(x); }
  int spanY(int y, int h) const { return mapY(y + h) - mapY(y); }
};

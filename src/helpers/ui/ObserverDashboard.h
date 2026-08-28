#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../RadioActivityWindow.h"
#include "DisplayDriver.h"
#include "DisplayFrameSignature.h"

// Observer analytics dashboard for the Heltec V4 R8 TFT targets.
//
// Fork-owned and self-contained: the role UITasks only pick a layout, hand over
// a Context plus a snapshot, and ask for a full frame or a single changed row.
// Everything here is host-buildable against DisplayDriver, so the layout, the
// formatting and the redraw policy are all covered by test_observer_dashboard.

namespace ObserverDashboard {

// ---------------------------------------------------------------- palette ---
// Kept local to the dashboard. applyDarkPalette() retunes the shared UIColor
// slots at runtime, which is why no display driver needs editing for the theme.

constexpr ColorVal rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (ColorVal)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr ColorVal BG         = rgb565(10, 12, 16);
constexpr ColorVal HEADER_BG  = rgb565(18, 38, 74);
constexpr ColorVal HEADER_SUB = rgb565(130, 170, 214);
constexpr ColorVal TEXT       = rgb565(232, 238, 246);
constexpr ColorVal MUTED      = rgb565(122, 134, 150);
constexpr ColorVal ACCENT     = rgb565(64, 176, 240);
constexpr ColorVal BAR        = rgb565(38, 116, 168);
constexpr ColorVal BAR_NOW    = rgb565(96, 208, 255);
constexpr ColorVal GRID       = rgb565(38, 46, 60);
constexpr ColorVal GOOD       = rgb565(72, 208, 136);
constexpr ColorVal WARN       = rgb565(248, 176, 72);

// Retunes the shared colour slots so every screen this UITask draws - boot,
// setup portal, reboot, power-off and the dashboard - is coherently dark.
inline void applyDarkPalette() {
  UIColor::window_bkg   = BG;
  UIColor::title_bkg    = HEADER_BG;
  UIColor::title_txt    = TEXT;
  UIColor::primary_txt  = TEXT;
  UIColor::secondary_txt = MUTED;
  UIColor::warning_txt  = WARN;
  UIColor::popup_bkg    = HEADER_BG;
  UIColor::popup_txt    = TEXT;
  UIColor::corp_blue    = ACCENT;
}

// ----------------------------------------------------------------- layout ---
// Logical 128x64 coordinates, as the shared DisplayDriver API expects. The two
// orientations need separate row pitches: a text row is a fixed 16 physical
// pixels, which is 3.2 logical units in portrait (y scale 5) but 4.27 in
// landscape (y scale 3.75), so one shared grid would overlap in landscape.

struct Layout {
  int16_t margin_x;
  int16_t right_x;        // right edge of the content column (exclusive)
  int16_t header_h;
  int16_t header_text_y;
  int16_t header_sub_y;   // second header row for the role label; -1 = same row
  int16_t radio_y;
  int16_t window_y;
  int16_t headline_y;
  int16_t headline_h;
  int16_t rate_y;
  int16_t graph_y;
  int16_t graph_h;        // includes the one-unit baseline at the bottom
  int16_t rf_y;
  int16_t status_y;
  int16_t text_h;         // logical height of one size-1 text row
  int16_t max_chars;      // size-1 characters that fit between the margins
  int16_t max_chars_big;  // size-2 characters that fit
};

// 240x320 panel: x scale 1.875, y scale 5, 12x16 glyphs (24x32 at size 2).
constexpr Layout portraitLayout() {
  return Layout{
      /*margin_x*/ 4,     /*right_x*/ 124,
      /*header_h*/ 9,     /*header_text_y*/ 1,   /*header_sub_y*/ 5,
      /*radio_y*/ 11,
      /*window_y*/ 16,
      /*headline_y*/ 21,  /*headline_h*/ 7,
      /*rate_y*/ 29,
      /*graph_y*/ 34,     /*graph_h*/ 16,
      /*rf_y*/ 52,
      /*status_y*/ 57,
      /*text_h*/ 4,
      /*max_chars*/ 18,   /*max_chars_big*/ 9};
}

// 320x240 panel: x scale 2.5, y scale 3.75, 12x16 glyphs (30x40 at size 2).
constexpr Layout landscapeLayout() {
  return Layout{
      /*margin_x*/ 4,     /*right_x*/ 124,
      /*header_h*/ 7,     /*header_text_y*/ 1,   /*header_sub_y*/ -1,
      /*radio_y*/ 9,
      /*window_y*/ 14,
      /*headline_y*/ 19,  /*headline_h*/ 12,
      /*rate_y*/ 31,
      /*graph_y*/ 36,     /*graph_h*/ 12,
      /*rf_y*/ 50,
      /*status_y*/ 56,
      /*text_h*/ 5,
      /*max_chars*/ 25,   /*max_chars_big*/ 10};
}

inline const Layout& activeLayout() {
#ifdef ST7789_PORTRAIT_PROFILE
  static const Layout layout = portraitLayout();
#else
  static const Layout layout = landscapeLayout();
#endif
  return layout;
}

// ------------------------------------------------------------- formatting ---
// Every value is formatted from integers. Arguments are widened explicitly so
// the same code is correct on a 32-bit target and on a 64-bit host.

inline void formatCompactCount(char* out, size_t n, uint32_t v) {
  if (v < 10000) {
    snprintf(out, n, "%lu", (unsigned long)v);
  } else if (v < 100000) {
    snprintf(out, n, "%lu.%luk", (unsigned long)(v / 1000), (unsigned long)((v % 1000) / 100));
  } else if (v < 1000000) {
    snprintf(out, n, "%luk", (unsigned long)(v / 1000));
  } else if (v < 100000000) {
    snprintf(out, n, "%lu.%luM", (unsigned long)(v / 1000000),
             (unsigned long)((v % 1000000) / 100000));
  } else {
    snprintf(out, n, "%luM", (unsigned long)(v / 1000000));
  }
}

inline void formatCompactBytes(char* out, size_t n, uint32_t v) {
  if (v < 1024) {
    snprintf(out, n, "%lu B", (unsigned long)v);
    return;
  }
  if (v < 1048576UL) {
    uint32_t tenths = (uint32_t)(((uint64_t)v * 10) / 1024);
    if (tenths < 1000) {
      snprintf(out, n, "%lu.%lu KB", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    } else {
      snprintf(out, n, "%lu KB", (unsigned long)(tenths / 10));
    }
    return;
  }
  uint32_t tenths = (uint32_t)(((uint64_t)v * 10) / 1048576UL);
  if (tenths < 1000) {
    snprintf(out, n, "%lu.%lu MB", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
  } else {
    snprintf(out, n, "%lu MB", (unsigned long)(tenths / 10));
  }
}

// One decimal below 100, whole numbers above, so the field cannot grow wide.
inline void formatTenths(char* out, size_t n, uint32_t tenths) {
  if (tenths < 1000) {
    snprintf(out, n, "%lu.%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
  } else {
    formatCompactCount(out, n, tenths / 10);
  }
}

inline void formatSignedTenths(char* out, size_t n, int32_t tenths) {
  const char* sign = tenths < 0 ? "-" : "+";
  uint32_t mag = (uint32_t)(tenths < 0 ? -tenths : tenths);
  snprintf(out, n, "%s%lu.%lu", sign, (unsigned long)(mag / 10), (unsigned long)(mag % 10));
}

// Quantised to the 5 s activity cadence so the string is stable within a tick
// and cannot make the status row repaint more often than the panel updates.
inline uint32_t quantizeAgeSecs(uint32_t age_ms) { return (age_ms / 5000) * 5; }

inline void formatAge(char* out, size_t n, uint32_t age_ms, bool valid) {
  if (!valid) {
    snprintf(out, n, "--");
    return;
  }
  uint32_t secs = quantizeAgeSecs(age_ms);
  if (secs < 5) {
    snprintf(out, n, "now");
  } else if (secs < 60) {
    snprintf(out, n, "%lus", (unsigned long)secs);
  } else if (secs < 3600) {
    snprintf(out, n, "%lum", (unsigned long)(secs / 60));
  } else if (secs < 86400) {
    snprintf(out, n, "%luh", (unsigned long)(secs / 3600));
  } else {
    snprintf(out, n, "%lud", (unsigned long)(secs / 86400));
  }
}

// "910.525 SF7 BW62.5" / "869.618 SF8 BW250" - a trailing ".0" on the
// bandwidth would push the widest case past the content column.
inline void formatRadioStrip(char* out, size_t n, float freq, uint8_t sf, float bw) {
  int32_t tenths = (int32_t)(bw * 10.0f + 0.5f);
  char bw_str[12];
  if (tenths % 10 == 0) {
    snprintf(bw_str, sizeof(bw_str), "%ld", (long)(tenths / 10));
  } else {
    snprintf(bw_str, sizeof(bw_str), "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
  }
  snprintf(out, n, "%.3f SF%u BW%s", (double)freq, (unsigned)sf, bw_str);
}

// Trims to a character budget rather than a measured width. The font is fixed
// width, so the budget is exact - and DisplayDriver::drawTextEllipsized() must
// not be used here: it trims against getTextWidth(), which reports an
// over-long string at the portrait driver's *fallback* scale and so stops
// trimming while the string is still too wide to draw at full size.
inline void fitToChars(char* out, size_t n, const char* src, int max_chars) {
  if (max_chars < 0) max_chars = 0;
  if ((size_t)max_chars > n - 1) max_chars = (int)(n - 1);

  size_t len = src ? strlen(src) : 0;
  if (len <= (size_t)max_chars) {
    memcpy(out, src ? src : "", len);
    out[len] = 0;
    return;
  }
  if (max_chars <= 3) {
    memcpy(out, src, (size_t)max_chars);
    out[max_chars] = 0;
    return;
  }
  memcpy(out, src, (size_t)max_chars - 3);
  memcpy(out + max_chars - 3, "...", 4);
}

// ------------------------------------------------------------- row content --

enum Row { ROW_WINDOW = 0, ROW_HEADLINE, ROW_RATE, ROW_GRAPH, ROW_RF, ROW_STATUS, ROW_COUNT };

struct Context {
  const char* node_name;
  const char* role_label;   // "REPEATER" / "ROOM SERVER"
  float freq;
  uint8_t sf;
  float bw;
  bool link_up;
};

struct RowText {
  char left[24];
  char right[24];
  ColorVal left_color;
  ColorVal right_color;
};

inline void composeRow(Row row, const Context& ctx, const RadioActivitySnapshot& s, RowText* out) {
  out->left[0] = out->right[0] = 0;
  out->left_color = TEXT;
  out->right_color = MUTED;

  char scratch[24];
  switch (row) {
    case ROW_WINDOW:
      if (s.isWarmingUp()) {
        snprintf(out->left, sizeof(out->left), "LIVE %lum", (unsigned long)s.warmupMinutes());
      } else {
        snprintf(out->left, sizeof(out->left), "LAST 20m");
      }
      out->left_color = MUTED;
      if (s.peak_per_min > 0) {
        formatCompactCount(scratch, sizeof(scratch), s.peak_per_min);
        snprintf(out->right, sizeof(out->right), "max %s/m", scratch);
      }
      break;

    case ROW_HEADLINE:
      if (s.isEmpty()) {
        snprintf(out->left, sizeof(out->left), "No RF yet");
        out->left_color = MUTED;
      } else {
        formatCompactCount(scratch, sizeof(scratch), s.packets);
        snprintf(out->left, sizeof(out->left), "%s pkt", scratch);
        out->left_color = BAR_NOW;
      }
      break;

    case ROW_RATE:
      formatCompactBytes(out->left, sizeof(out->left), s.wire_bytes);
      formatTenths(scratch, sizeof(scratch), s.packetsPerMinuteX10());
      snprintf(out->right, sizeof(out->right), "%s/min", scratch);
      if (s.isEmpty()) {
        out->left_color = MUTED;
      }
      break;

    case ROW_RF:
      if (s.isEmpty()) {
        snprintf(out->left, sizeof(out->left), "SNR --");
        out->left_color = MUTED;
      } else {
        int32_t snr = s.avgSnrX10();
        formatSignedTenths(scratch, sizeof(scratch), snr);
        snprintf(out->left, sizeof(out->left), "SNR %s", scratch);
        out->left_color = snr >= 0 ? GOOD : (snr >= -70 ? TEXT : WARN);
      }
      {
        uint32_t air = s.airtimePercentX10();
        formatTenths(scratch, sizeof(scratch), air);
        snprintf(out->right, sizeof(out->right), "AIR %s%%", scratch);
        out->right_color = air >= 100 ? WARN : MUTED;
      }
      break;

    case ROW_STATUS:
      formatAge(scratch, sizeof(scratch), s.last_packet_age_ms, s.has_last_packet);
      snprintf(out->left, sizeof(out->left), "RX %s", scratch);
      out->left_color = s.has_last_packet ? TEXT : MUTED;
      snprintf(out->right, sizeof(out->right), ctx.link_up ? "WiFi OK" : "WiFi --");
      out->right_color = ctx.link_up ? GOOD : WARN;
      break;

    default:
      break;
  }
}

inline int16_t rowY(const Layout& l, Row row) {
  switch (row) {
    case ROW_WINDOW: return l.window_y;
    case ROW_HEADLINE: return l.headline_y;
    case ROW_RATE: return l.rate_y;
    case ROW_GRAPH: return l.graph_y;
    case ROW_RF: return l.rf_y;
    default: return l.status_y;
  }
}

inline int16_t rowH(const Layout& l, Row row) {
  if (row == ROW_HEADLINE) return l.headline_h;
  if (row == ROW_GRAPH) return l.graph_h;
  return l.text_h;
}

// --------------------------------------------------------------- the graph --

// Bar heights in logical units, oldest first. Any minute with traffic rounds up
// to at least one unit; empty minutes stay empty.
inline void barHeights(const Layout& l, const RadioActivitySnapshot& s,
                       uint8_t out[RADIO_ACTIVITY_BUCKETS]) {
  uint16_t scale = s.peak_per_min > 0 ? s.peak_per_min : 1;
  int16_t max_h = l.graph_h - 1;   // the last unit is the baseline
  for (int i = 0; i < RADIO_ACTIVITY_BUCKETS; i++) {
    uint32_t v = s.buckets[i];
    out[i] = v == 0 ? 0 : (uint8_t)((v * max_h + scale - 1) / scale);
  }
}

inline int16_t barSlot(const Layout& l) {
  return (int16_t)((l.right_x - l.margin_x) / RADIO_ACTIVITY_BUCKETS);
}

// ------------------------------------------------------------- signatures ---
// One signature per row, computed from exactly what is drawn, so a repaint
// happens only where the pixels actually differ.

inline uint32_t rowSignature(const Layout& l, Row row, const Context& ctx,
                             const RadioActivitySnapshot& s) {
  uint32_t sig = DisplayFrameSignature::INITIAL;
  if (row == ROW_GRAPH) {
    uint8_t h[RADIO_ACTIVITY_BUCKETS];
    barHeights(l, s, h);
    char buf[4 * RADIO_ACTIVITY_BUCKETS];
    size_t p = 0;
    for (int i = 0; i < RADIO_ACTIVITY_BUCKETS && p + 4 < sizeof(buf); i++) {
      p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%u,", (unsigned)h[i]);
    }
    return DisplayFrameSignature::append(sig, buf);
  }

  RowText t;
  composeRow(row, ctx, s, &t);
  sig = DisplayFrameSignature::append(sig, t.left);
  return DisplayFrameSignature::append(sig, t.right);
}

inline void allRowSignatures(const Layout& l, const Context& ctx, const RadioActivitySnapshot& s,
                             uint32_t out[ROW_COUNT]) {
  for (int r = 0; r < ROW_COUNT; r++) out[r] = rowSignature(l, (Row)r, ctx, s);
}

// ------------------------------------------------------------------ render --

// Right-hand text is measured and placed first, then the left text is
// ellipsized into whatever column is left, so the two can never collide.
//
// The right edge gets a one-unit gutter: getTextWidth() converts physical
// glyph widths back to logical units and rounds, so anchoring flush at right_x
// can land a pixel past it.
inline void drawPair(DisplayDriver& d, const Layout& l, int16_t y, const RowText& t,
                    int max_chars) {
  int right_len = (int)strlen(t.right);
  if (right_len > 0) {
    int16_t rw = (int16_t)d.getTextWidth(t.right);
    d.setColor(t.right_color);
    d.setCursor((int16_t)(l.right_x - rw - 1), y);
    d.print(t.right);
  }

  int budget = max_chars - (right_len > 0 ? right_len + 1 : 0);
  if (t.left[0] && budget > 0) {
    char fitted[32];
    fitToChars(fitted, sizeof(fitted), t.left, budget);
    d.setColor(t.left_color);
    d.setCursor(l.margin_x, y);
    d.print(fitted);
  }
}

inline void clearRow(DisplayDriver& d, const Layout& l, Row row) {
  d.setColor(BG);
  d.fillRect(l.margin_x, rowY(l, row), l.right_x - l.margin_x, rowH(l, row));
}

inline void drawGraph(DisplayDriver& d, const Layout& l, const RadioActivitySnapshot& s) {
  int16_t base_y = (int16_t)(l.graph_y + l.graph_h - 1);
  d.setColor(GRID);
  d.fillRect(l.margin_x, base_y, l.right_x - l.margin_x, 1);

  uint8_t h[RADIO_ACTIVITY_BUCKETS];
  barHeights(l, s, h);
  int16_t slot = barSlot(l);
  for (int i = 0; i < RADIO_ACTIVITY_BUCKETS; i++) {
    if (h[i] == 0) continue;
    d.setColor(i == RADIO_ACTIVITY_BUCKETS - 1 ? BAR_NOW : BAR);
    d.fillRect((int16_t)(l.margin_x + i * slot), (int16_t)(base_y - h[i]), (int16_t)(slot - 1),
               h[i]);
  }
}

inline void drawRow(DisplayDriver& d, const Layout& l, Row row, const Context& ctx,
                    const RadioActivitySnapshot& s, bool clear_first) {
  if (clear_first) clearRow(d, l, row);

  if (row == ROW_GRAPH) {
    drawGraph(d, l, s);
    return;
  }

  RowText t;
  composeRow(row, ctx, s, &t);
  d.setTextSize(row == ROW_HEADLINE ? 2 : 1);
  drawPair(d, l, rowY(l, row), t, row == ROW_HEADLINE ? l.max_chars_big : l.max_chars);
  if (row == ROW_HEADLINE) d.setTextSize(1);
}

inline void drawHeader(DisplayDriver& d, const Layout& l, const Context& ctx) {
  d.setColor(HEADER_BG);
  d.fillRect(0, 0, 128, l.header_h);
  d.setTextSize(1);

  RowText t{};
  t.left_color = TEXT;
  t.right_color = HEADER_SUB;
  snprintf(t.left, sizeof(t.left), "%s", ctx.node_name ? ctx.node_name : "");
  snprintf(t.right, sizeof(t.right), "%s", ctx.role_label ? ctx.role_label : "");

  if (l.header_sub_y < 0) {
    drawPair(d, l, l.header_text_y, t, l.max_chars);   // both fit on one line
    return;
  }

  // Narrow panel: the node name keeps the whole width and the role drops to a
  // second line, rather than the name being ellipsized down to a few letters.
  RowText name{};
  name.left_color = t.left_color;
  memcpy(name.left, t.left, sizeof(name.left));
  drawPair(d, l, l.header_text_y, name, l.max_chars);

  RowText role{};
  role.left_color = t.right_color;
  memcpy(role.left, t.right, sizeof(role.left));
  drawPair(d, l, l.header_sub_y, role, l.max_chars);
}

inline void drawRadioStrip(DisplayDriver& d, const Layout& l, const Context& ctx) {
  char tmp[32];
  formatRadioStrip(tmp, sizeof(tmp), ctx.freq, ctx.sf, ctx.bw);
  char fitted[32];
  fitToChars(fitted, sizeof(fitted), tmp, l.max_chars);
  d.setTextSize(1);
  d.setColor(MUTED);
  d.setCursor(l.margin_x, l.radio_y);
  d.print(fitted);
}

// Complete repaint. The caller has already run startFrame(), which clears to
// UIColor::window_bkg - the dark background applyDarkPalette() installed.
inline void drawFull(DisplayDriver& d, const Layout& l, const Context& ctx,
                     const RadioActivitySnapshot& s) {
  drawHeader(d, l, ctx);
  drawRadioStrip(d, l, ctx);
  for (int r = 0; r < ROW_COUNT; r++) drawRow(d, l, (Row)r, ctx, s, false);
}

// Repaints only the rows whose signature moved. Never touches the header, the
// radio strip, or anything outside the analytics rows, so no startFrame() and
// no whole-screen clear is involved.
inline bool drawChangedRows(DisplayDriver& d, const Layout& l, const Context& ctx,
                            const RadioActivitySnapshot& s, uint32_t signatures[ROW_COUNT]) {
  uint32_t fresh[ROW_COUNT];
  allRowSignatures(l, ctx, s, fresh);

  bool drew = false;
  for (int r = 0; r < ROW_COUNT; r++) {
    if (fresh[r] == signatures[r]) continue;
    drawRow(d, l, (Row)r, ctx, s, true);
    signatures[r] = fresh[r];
    drew = true;
  }
  return drew;
}

} // namespace ObserverDashboard

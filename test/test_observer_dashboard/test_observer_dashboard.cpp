#include "MockDisplay.h"
#include "helpers/ui/ObserverDashboard.h"

#include <gtest/gtest.h>

// UIColor's slots live in whichever display driver a firmware target links; the
// host build supplies its own.
ColorVal UIColor::window_bkg = 0;
ColorVal UIColor::title_bkg = 0;
ColorVal UIColor::title_txt = 0;
ColorVal UIColor::primary_txt = 0;
ColorVal UIColor::secondary_txt = 0;
ColorVal UIColor::warning_txt = 0;
ColorVal UIColor::popup_bkg = 0;
ColorVal UIColor::popup_txt = 0;
ColorVal UIColor::corp_blue = 0;

using namespace ObserverDashboard;

namespace {

const int N = RADIO_ACTIVITY_BUCKETS;

struct Profile {
  MockDisplay::Mode mode;
  Layout layout;
  const char* name;
  int panel_w, panel_h;
  int margin_left, margin_right;   // physical
};

Profile portrait() { return {MockDisplay::PORTRAIT, portraitLayout(), "portrait", 240, 320, 7, 232}; }
Profile landscape() { return {MockDisplay::LANDSCAPE, landscapeLayout(), "landscape", 320, 240, 10, 310}; }

Context makeContext(const char* name = "Ridgeline North") {
  Context c;
  c.node_name = name;
  c.role_label = "REPEATER";
  c.freq = 910.525f;
  c.sf = 7;
  c.bw = 62.5f;
  c.link_up = true;
  return c;
}

// A busy but plausible 20 minutes: 1843 packets, a peak minute of 214.
RadioActivitySnapshot makeBusy() {
  RadioActivityWindow w;
  w.reset(0);
  const uint16_t per_minute[RADIO_ACTIVITY_BUCKETS] = {12, 40, 8,  0,  97, 133, 71, 3,  214, 65,
                                                       19, 88, 44, 27, 0,  150, 92, 61, 7,   35};
  for (int m = 0; m < N; m++) {
    for (int i = 0; i < per_minute[m]; i++) {
      w.recordPacket((uint32_t)m * RADIO_ACTIVITY_BUCKET_MS + 1000 + i, 48, 120, 26, -103);
    }
  }
  RadioActivitySnapshot s;
  w.snapshot((uint32_t)(N - 1) * RADIO_ACTIVITY_BUCKET_MS + 30000, &s);
  return s;
}

RadioActivitySnapshot makeEmpty() {
  RadioActivityWindow w;
  w.reset(0);
  RadioActivitySnapshot s;
  w.snapshot(7 * RADIO_ACTIVITY_BUCKET_MS, &s);
  return s;
}

bool insideRect(const MockDisplay::Op& op, int x, int y, int w, int h) {
  return op.x >= x && op.y >= y && op.x + op.w <= x + w && op.y + op.h <= y + h;
}

} // namespace

// ------------------------------------------------------------- formatting ---

TEST(ObserverDashboardFormat, CompactCountsStayShortAtEveryMagnitude) {
  char b[24];
  struct { uint32_t v; const char* want; } cases[] = {
      {0, "0"},         {7, "7"},           {9999, "9999"},   {10000, "10.0k"},
      {12345, "12.3k"}, {99999, "99.9k"},   {100000, "100k"}, {999999, "999k"},
      {1000000, "1.0M"},{12345678, "12.3M"},{100000000, "100M"}};
  for (auto& c : cases) {
    formatCompactCount(b, sizeof(b), c.v);
    EXPECT_STREQ(c.want, b) << "value " << c.v;
    EXPECT_LE(strlen(b), 5u) << "value " << c.v;
  }
}

TEST(ObserverDashboardFormat, CompactBytesPickSensibleUnits) {
  char b[24];
  struct { uint32_t v; const char* want; } cases[] = {
      {0, "0 B"},       {1023, "1023 B"},   {1024, "1.0 KB"},
      {10240, "10.0 KB"}, {145408, "142 KB"}, {1048576, "1.0 MB"},
      {15728640, "15.0 MB"}};
  for (auto& c : cases) {
    formatCompactBytes(b, sizeof(b), c.v);
    EXPECT_STREQ(c.want, b) << "value " << c.v;
  }
}

TEST(ObserverDashboardFormat, TenthsAndSignedTenths) {
  char b[24];
  formatTenths(b, sizeof(b), 0);    EXPECT_STREQ("0.0", b);
  formatTenths(b, sizeof(b), 34);   EXPECT_STREQ("3.4", b);
  formatTenths(b, sizeof(b), 999);  EXPECT_STREQ("99.9", b);
  formatTenths(b, sizeof(b), 1000); EXPECT_STREQ("100", b);

  formatSignedTenths(b, sizeof(b), 72);   EXPECT_STREQ("+7.2", b);
  formatSignedTenths(b, sizeof(b), 0);    EXPECT_STREQ("+0.0", b);
  formatSignedTenths(b, sizeof(b), -115); EXPECT_STREQ("-11.5", b);
}

TEST(ObserverDashboardFormat, AgeIsQuantisedToTheFiveSecondCadence) {
  EXPECT_EQ(0u, quantizeAgeSecs(0));
  EXPECT_EQ(0u, quantizeAgeSecs(4999));
  EXPECT_EQ(5u, quantizeAgeSecs(5000));
  EXPECT_EQ(5u, quantizeAgeSecs(9999));

  char b[24];
  formatAge(b, sizeof(b), 0, false);       EXPECT_STREQ("--", b);
  formatAge(b, sizeof(b), 0, true);        EXPECT_STREQ("now", b);
  formatAge(b, sizeof(b), 4999, true);     EXPECT_STREQ("now", b);
  formatAge(b, sizeof(b), 12000, true);    EXPECT_STREQ("10s", b);
  formatAge(b, sizeof(b), 59999, true);    EXPECT_STREQ("55s", b);
  formatAge(b, sizeof(b), 60000, true);    EXPECT_STREQ("1m", b);
  formatAge(b, sizeof(b), 3599999, true);  EXPECT_STREQ("59m", b);
  formatAge(b, sizeof(b), 3600000, true);  EXPECT_STREQ("1h", b);
}

TEST(ObserverDashboardFormat, EmptyWindowNeverProducesNanOrInfinity) {
  RadioActivitySnapshot s = makeEmpty();
  Context ctx = makeContext();

  for (int r = 0; r < ROW_COUNT; r++) {
    if (r == ROW_GRAPH) continue;
    RowText t;
    composeRow((Row)r, ctx, s, &t);
    for (const char* p : {t.left, t.right}) {
      EXPECT_EQ(nullptr, strstr(p, "nan")) << p;
      EXPECT_EQ(nullptr, strstr(p, "inf")) << p;
    }
  }

  // Unmeasurable values read as "--"; measured zeroes read as real zeroes.
  RowText headline, rate, rf, status;
  composeRow(ROW_HEADLINE, ctx, s, &headline);
  composeRow(ROW_RATE, ctx, s, &rate);
  composeRow(ROW_RF, ctx, s, &rf);
  composeRow(ROW_STATUS, ctx, s, &status);
  EXPECT_STREQ("No RF yet", headline.left);
  EXPECT_STREQ("0 B", rate.left);
  EXPECT_STREQ("0.0/min", rate.right);
  EXPECT_STREQ("SNR --", rf.left);
  EXPECT_STREQ("AIR 0.0%", rf.right);
  EXPECT_STREQ("RX --", status.left);
}

TEST(ObserverDashboardFormat, EveryRowFitsTheCharacterBudget) {
  for (const Profile& p : {portrait(), landscape()}) {
    for (const RadioActivitySnapshot& s : {makeBusy(), makeEmpty()}) {
      for (int r = 0; r < ROW_COUNT; r++) {
        if (r == ROW_GRAPH) continue;
        RowText t;
        composeRow((Row)r, makeContext(), s, &t);
        int budget = (r == ROW_HEADLINE) ? p.layout.max_chars_big : p.layout.max_chars;
        int used = (int)strlen(t.left) + (int)strlen(t.right);
        if (t.left[0] && t.right[0]) used += 1;   // separating space
        EXPECT_LE(used, budget) << p.name << " row " << r << ": '" << t.left << "' / '" << t.right << "'";
      }
    }
  }
}

TEST(ObserverDashboardFormat, RadioStripDropsADeadBandwidthDecimal) {
  char b[32];
  formatRadioStrip(b, sizeof(b), 910.525f, 7, 62.5f);
  EXPECT_STREQ("910.525 SF7 BW62.5", b);
  formatRadioStrip(b, sizeof(b), 869.618f, 8, 250.0f);
  EXPECT_STREQ("869.618 SF8 BW250", b);
  formatRadioStrip(b, sizeof(b), 433.125f, 12, 125.0f);
  EXPECT_STREQ("433.125 SF12 BW125", b);
}

TEST(ObserverDashboardFormat, RadioStripFitsEveryOrientationsBudget) {
  // The widest realistic combination must not be ellipsized away.
  const struct { float freq; uint8_t sf; float bw; } cases[] = {
      {910.525f, 7, 62.5f}, {869.618f, 8, 250.0f}, {433.125f, 12, 125.0f},
      {915.000f, 11, 500.0f}, {868.000f, 9, 41.7f}};
  for (const Profile& p : {portrait(), landscape()}) {
    for (const auto& c : cases) {
      char b[32];
      formatRadioStrip(b, sizeof(b), c.freq, c.sf, c.bw);
      EXPECT_LE((int)strlen(b), p.layout.max_chars) << p.name << " '" << b << "'";
    }
  }
}

TEST(ObserverDashboardLayout, HeaderTextStaysInsideTheHeaderBar) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    drawHeader(d, p.layout, makeContext());

    ASSERT_FALSE(d.ops.empty());
    const auto& bar = d.ops.front();
    ASSERT_EQ(MockDisplay::Op::FILL, bar.kind) << p.name;
    for (size_t i = 1; i < d.ops.size(); i++) {
      EXPECT_TRUE(insideRect(d.ops[i], bar.x, bar.y, bar.w, bar.h))
          << p.name << " '" << d.ops[i].text << "'";
    }
  }
}

TEST(ObserverDashboardLayout, PortraitHeaderShowsTheWholeNodeName) {
  // A 16-character name must survive intact: on a 240 px panel the role label
  // moves to a second header line rather than eating the name.
  MockDisplay d(MockDisplay::PORTRAIT);
  Context ctx = makeContext("Ridgeline North");
  drawHeader(d, portraitLayout(), ctx);

  bool saw_name = false, saw_role = false;
  for (const auto& op : d.ops) {
    if (op.text == "Ridgeline North") saw_name = true;
    if (op.text == "REPEATER") saw_role = true;
  }
  EXPECT_TRUE(saw_name) << "node name was ellipsized";
  EXPECT_TRUE(saw_role);
}

TEST(ObserverDashboardFormat, TextIsAsciiOnly) {
  // The driver's UTF-8 fallback collapses every non-ASCII byte to a full block,
  // so any stray multi-byte character would render as a solid glyph.
  for (const RadioActivitySnapshot& s : {makeBusy(), makeEmpty()}) {
    for (int r = 0; r < ROW_COUNT; r++) {
      if (r == ROW_GRAPH) continue;
      RowText t;
      composeRow((Row)r, makeContext(), s, &t);
      for (const char* p : {t.left, t.right}) {
        for (const char* c = p; *c; c++) {
          EXPECT_GE((unsigned char)*c, 32u) << "row " << r;
          EXPECT_LE((unsigned char)*c, 126u) << "row " << r;
        }
      }
    }
  }
}

// ----------------------------------------------------------------- layout ---

TEST(ObserverDashboardLayout, EveryDrawnPixelStaysOnThePanel) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    drawFull(d, p.layout, makeContext(), makeBusy());
    ASSERT_FALSE(d.ops.empty());
    for (const auto& op : d.ops) {
      EXPECT_GE(op.x, 0) << p.name;
      EXPECT_GE(op.y, 0) << p.name;
      EXPECT_LE(op.x + op.w, p.panel_w) << p.name << " '" << op.text << "'";
      EXPECT_LE(op.y + op.h, p.panel_h) << p.name << " '" << op.text << "'";
    }
  }
}

TEST(ObserverDashboardLayout, AllTextIsPaddedInsideTheMargins) {
  for (const Profile& p : {portrait(), landscape()}) {
    for (const RadioActivitySnapshot& s : {makeBusy(), makeEmpty()}) {
      MockDisplay d(p.mode);
      drawFull(d, p.layout, makeContext(), s);
      for (const auto& op : d.ops) {
        if (op.kind != MockDisplay::Op::TEXT) continue;
        EXPECT_GE(op.x, p.margin_left) << p.name << " '" << op.text << "'";
        EXPECT_LE(op.x + op.w, p.margin_right) << p.name << " '" << op.text << "'";
      }
    }
  }
}

TEST(ObserverDashboardLayout, NoTextSilentlyShrinksToTheFallbackScale) {
  // The portrait driver halves the glyph size rather than clipping. A row that
  // only fits because of that would break the grid, so it must never happen.
  for (const RadioActivitySnapshot& s : {makeBusy(), makeEmpty()}) {
    MockDisplay d(MockDisplay::PORTRAIT);
    drawFull(d, portraitLayout(), makeContext(), s);
    for (const auto& op : d.ops) {
      EXPECT_FALSE(op.scale_fallback) << "'" << op.text << "'";
    }
  }
}

TEST(ObserverDashboardLayout, ContentClearsTheTopAndBottomEdges) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    drawFull(d, p.layout, makeContext(), makeBusy());
    int lowest = 0;
    for (const auto& op : d.ops) lowest = std::max(lowest, op.y + op.h);
    EXPECT_GE(p.panel_h - lowest, 8) << p.name << ": bottom margin too small";
  }
}

TEST(ObserverDashboardLayout, RowRectanglesDoNotOverlap) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    for (int a = 0; a < ROW_COUNT; a++) {
      for (int b = a + 1; b < ROW_COUNT; b++) {
        int ay = p.layout.margin_x, unused = ay;
        (void)unused;
        int a_top = rowY(p.layout, (Row)a), a_bot = a_top + rowH(p.layout, (Row)a);
        int b_top = rowY(p.layout, (Row)b), b_bot = b_top + rowH(p.layout, (Row)b);
        bool overlap = a_top < b_bot && b_top < a_bot;
        EXPECT_FALSE(overlap) << p.name << ": rows " << a << " and " << b;
      }
    }
    // ...and the header and radio strip sit above the first row.
    EXPECT_LT(p.layout.header_h, p.layout.radio_y) << p.name;
    EXPECT_LT(p.layout.radio_y + p.layout.text_h, rowY(p.layout, ROW_WINDOW) + 1) << p.name;
  }
}

TEST(ObserverDashboardLayout, EachRowRepaintCoversEverythingThatRowDraws) {
  // The no-flash invariant: a partial repaint clears one row rectangle and then
  // redraws inside it. Anything drawn outside that rectangle would leave stale
  // pixels behind or scribble on a neighbouring row.
  for (const Profile& p : {portrait(), landscape()}) {
    for (const RadioActivitySnapshot& s : {makeBusy(), makeEmpty()}) {
      for (int r = 0; r < ROW_COUNT; r++) {
        MockDisplay d(p.mode);
        drawRow(d, p.layout, (Row)r, makeContext(), s, true);
        ASSERT_FALSE(d.ops.empty()) << p.name << " row " << r;

        const auto& clear = d.ops.front();
        ASSERT_EQ(MockDisplay::Op::FILL, clear.kind) << p.name << " row " << r;
        EXPECT_EQ(BG, clear.color) << p.name << " row " << r;

        for (size_t i = 1; i < d.ops.size(); i++) {
          EXPECT_TRUE(insideRect(d.ops[i], clear.x, clear.y, clear.w, clear.h))
              << p.name << " row " << r << " op " << i << " '" << d.ops[i].text << "'";
        }
      }
    }
  }
}

// ------------------------------------------------------------------ graph ---

TEST(ObserverDashboardGraph, DrawsExactlyTwentyNonOverlappingBars) {
  for (const Profile& p : {portrait(), landscape()}) {
    RadioActivitySnapshot s = makeBusy();
    for (int i = 0; i < N; i++) s.buckets[i] = (uint16_t)(i + 1);
    s.peak_per_min = N;

    MockDisplay d(p.mode);
    drawGraph(d, p.layout, s);

    // First op is the baseline, then one bar per non-empty bucket.
    ASSERT_GE(d.ops.size(), 1u);
    std::vector<MockDisplay::Op> bars(d.ops.begin() + 1, d.ops.end());
    ASSERT_EQ((size_t)N, bars.size()) << p.name;

    for (size_t i = 1; i < bars.size(); i++) {
      EXPECT_GE(bars[i].x, bars[i - 1].x + bars[i - 1].w) << p.name << " bar " << i << " overlaps";
      EXPECT_GE(bars[i].h, bars[i - 1].h) << p.name << " bar " << i << " not monotonic";
    }
    EXPECT_EQ(BAR_NOW, bars.back().color) << p.name << ": current minute must stand out";
    EXPECT_EQ(BAR, bars.front().color) << p.name;
  }
}

TEST(ObserverDashboardGraph, BarsStayInsideTheGraphRectangle) {
  for (const Profile& p : {portrait(), landscape()}) {
    RadioActivitySnapshot s = makeBusy();
    MockDisplay probe(p.mode);
    probe.setColor(0);
    probe.fillRect(p.layout.margin_x, p.layout.graph_y, p.layout.right_x - p.layout.margin_x,
                   p.layout.graph_h);
    MockDisplay::Op rect = probe.ops.front();

    MockDisplay d(p.mode);
    drawGraph(d, p.layout, s);
    for (const auto& op : d.ops) {
      EXPECT_TRUE(insideRect(op, rect.x, rect.y, rect.w, rect.h)) << p.name;
    }
  }
}

TEST(ObserverDashboardGraph, AllZeroWindowDrawsOnlyTheBaseline) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    drawGraph(d, p.layout, makeEmpty());
    ASSERT_EQ(1u, d.ops.size()) << p.name << ": empty minutes must not draw one-pixel activity";
    EXPECT_EQ(GRID, d.ops.front().color) << p.name;
  }
}

TEST(ObserverDashboardGraph, SingleSpikeFillsTheGraphAndLeavesTheRestEmpty) {
  for (const Profile& p : {portrait(), landscape()}) {
    RadioActivitySnapshot s = makeEmpty();
    s.buckets[5] = 400;
    s.peak_per_min = 400;
    s.packets = 400;

    uint8_t h[RADIO_ACTIVITY_BUCKETS];
    barHeights(p.layout, s, h);
    EXPECT_EQ(p.layout.graph_h - 1, h[5]) << p.name;
    for (int i = 0; i < N; i++) {
      if (i != 5) EXPECT_EQ(0, h[i]) << p.name << " bucket " << i;
    }

    MockDisplay d(p.mode);
    drawGraph(d, p.layout, s);
    EXPECT_EQ(2u, d.ops.size()) << p.name;   // baseline + one bar
  }
}

TEST(ObserverDashboardGraph, AnyTrafficRoundsUpToAVisibleBar) {
  for (const Profile& p : {portrait(), landscape()}) {
    RadioActivitySnapshot s = makeEmpty();
    s.buckets[0] = 1;
    s.buckets[19] = 5000;
    s.peak_per_min = 5000;

    uint8_t h[RADIO_ACTIVITY_BUCKETS];
    barHeights(p.layout, s, h);
    EXPECT_EQ(1, h[0]) << p.name << ": a single packet must still be visible";
    EXPECT_EQ(p.layout.graph_h - 1, h[19]) << p.name;
  }
}

// ------------------------------------------------------------- signatures ---

TEST(ObserverDashboardSignature, IdenticallyFormattedDataDoesNotRepaint) {
  Layout l = portraitLayout();
  Context ctx = makeContext();

  RadioActivitySnapshot a = makeEmpty();
  a.packets = 100000;
  RadioActivitySnapshot b = a;
  b.packets = 100999;   // both render as "100k pkt"

  EXPECT_EQ(rowSignature(l, ROW_HEADLINE, ctx, a), rowSignature(l, ROW_HEADLINE, ctx, b));

  b.packets = 101500;   // renders as "101k pkt"
  EXPECT_NE(rowSignature(l, ROW_HEADLINE, ctx, a), rowSignature(l, ROW_HEADLINE, ctx, b));
}

TEST(ObserverDashboardSignature, AGraphChangeTouchesOnlyTheGraphRow) {
  Layout l = portraitLayout();
  Context ctx = makeContext();

  RadioActivitySnapshot a = makeBusy();
  RadioActivitySnapshot b = a;
  b.buckets[N - 1] = (uint16_t)(a.buckets[N - 1] + 40);   // the current minute grows

  uint32_t sa[ROW_COUNT], sb[ROW_COUNT];
  allRowSignatures(l, ctx, a, sa);
  allRowSignatures(l, ctx, b, sb);

  for (int r = 0; r < ROW_COUNT; r++) {
    if (r == ROW_GRAPH) {
      EXPECT_NE(sa[r], sb[r]) << "graph row must notice the new bar height";
    } else {
      EXPECT_EQ(sa[r], sb[r]) << "row " << r << " must not repaint";
    }
  }
}

TEST(ObserverDashboardSignature, DataChangesBelowTheGraphResolutionDoNotRepaint) {
  // Signatures are computed from bar heights, not from the packet counts behind
  // them, so a busy minute ticking up by one costs nothing on screen.
  Layout l = portraitLayout();
  Context ctx = makeContext();

  RadioActivitySnapshot a = makeBusy();
  RadioActivitySnapshot b = a;
  b.buckets[N - 1] = (uint16_t)(a.buckets[N - 1] + 1);

  uint8_t ha[RADIO_ACTIVITY_BUCKETS], hb[RADIO_ACTIVITY_BUCKETS];
  barHeights(l, a, ha);
  barHeights(l, b, hb);
  ASSERT_EQ(ha[N - 1], hb[N - 1]) << "test needs a change smaller than one bar unit";

  EXPECT_EQ(rowSignature(l, ROW_GRAPH, ctx, a), rowSignature(l, ROW_GRAPH, ctx, b));
}

TEST(ObserverDashboardSignature, LinkStateOnlyTouchesTheStatusRow) {
  Layout l = portraitLayout();
  RadioActivitySnapshot s = makeBusy();

  Context up = makeContext();
  Context down = makeContext();
  down.link_up = false;

  uint32_t sa[ROW_COUNT], sb[ROW_COUNT];
  allRowSignatures(l, up, s, sa);
  allRowSignatures(l, down, s, sb);

  for (int r = 0; r < ROW_COUNT; r++) {
    if (r == ROW_STATUS) {
      EXPECT_NE(sa[r], sb[r]);
    } else {
      EXPECT_EQ(sa[r], sb[r]) << "row " << r;
    }
  }
}

TEST(ObserverDashboardSignature, PartialRepaintDrawsOnlyTheChangedRow) {
  Profile p = portrait();
  Context ctx = makeContext();
  RadioActivitySnapshot a = makeBusy();

  uint32_t sigs[ROW_COUNT];
  allRowSignatures(p.layout, ctx, a, sigs);

  RadioActivitySnapshot b = a;
  b.buckets[N - 1] = (uint16_t)(a.buckets[N - 1] + 40);

  MockDisplay d(p.mode);
  EXPECT_TRUE(drawChangedRows(d, p.layout, ctx, b, sigs));

  // One clear plus the graph contents, all inside the graph rectangle.
  ASSERT_FALSE(d.ops.empty());
  const auto& clear = d.ops.front();
  EXPECT_EQ(BG, clear.color);
  for (const auto& op : d.ops) {
    EXPECT_TRUE(insideRect(op, clear.x, clear.y, clear.w, clear.h));
  }

  // Nothing left to do on a second pass with the same data.
  MockDisplay d2(p.mode);
  EXPECT_FALSE(drawChangedRows(d2, p.layout, ctx, b, sigs));
  EXPECT_TRUE(d2.ops.empty());
}

TEST(ObserverDashboardSignature, LongNodeNameIsTrimmedInsideTheHeader) {
  for (const Profile& p : {portrait(), landscape()}) {
    MockDisplay d(p.mode);
    Context ctx = makeContext("A Very Long Repeater Node Name That Cannot Possibly Fit");
    drawHeader(d, p.layout, ctx);

    bool saw_role = false, saw_ellipsis = false;
    for (const auto& op : d.ops) {
      if (op.kind != MockDisplay::Op::TEXT) continue;
      EXPECT_GE(op.x, p.margin_left) << p.name;
      EXPECT_LE(op.x + op.w, p.margin_right) << p.name << " '" << op.text << "'";
      EXPECT_FALSE(op.scale_fallback) << p.name << " '" << op.text << "'";
      if (op.text == "REPEATER") saw_role = true;
      if (op.text.size() >= 3 && op.text.compare(op.text.size() - 3, 3, "...") == 0)
        saw_ellipsis = true;
    }
    EXPECT_TRUE(saw_role) << p.name << ": the role label must survive a long node name";
    EXPECT_TRUE(saw_ellipsis) << p.name << ": the name must be visibly truncated";
  }
}

TEST(ObserverDashboardSignature, FitToCharsRespectsItsBudget) {
  char b[32];
  fitToChars(b, sizeof(b), "short", 18);        EXPECT_STREQ("short", b);
  fitToChars(b, sizeof(b), "exactly-18-chars!", 17); EXPECT_STREQ("exactly-18-chars!", b);
  fitToChars(b, sizeof(b), "Ridgeline North Ridge", 18);
  EXPECT_STREQ("Ridgeline North...", b);
  EXPECT_EQ(18u, strlen(b));
  fitToChars(b, sizeof(b), "abcdef", 3);        EXPECT_STREQ("abc", b);
  fitToChars(b, sizeof(b), "abcdef", 0);        EXPECT_STREQ("", b);
  fitToChars(b, sizeof(b), "", 18);             EXPECT_STREQ("", b);
}

TEST(ObserverDashboardSignature, DarkPaletteRetunesTheSharedColourSlots) {
  applyDarkPalette();
  EXPECT_EQ(BG, UIColor::window_bkg);
  EXPECT_EQ(TEXT, UIColor::primary_txt);
  EXPECT_EQ(HEADER_BG, UIColor::title_bkg);
  EXPECT_EQ(ACCENT, UIColor::corp_blue);
  // The setup portal's highlight must stay legible on the dark background.
  EXPECT_NE(UIColor::window_bkg, UIColor::warning_txt);
  EXPECT_NE(UIColor::window_bkg, UIColor::primary_txt);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

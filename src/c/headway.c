// Headway — a Pebble watchface that reads from the cuff in.
//
// Information is ranked by edge, so a sleeve sliding in from the wrist side
// uncovers the most time-critical zone last:
//
//   rail (outer edge) > countdown > minute > hour & date (wrist side)
//
// Layout is authored against the 144x168 reference and scaled by display
// width; text is positioned from measured glyph boxes rather than fixed
// offsets, so it holds on emery's 200x228 too.

#include <pebble.h>
#include <ctype.h>
#include <stddef.h>

// ---------------------------------------------------------------- settings

#define TIME_FMT_SYSTEM 0
#define TIME_FMT_12H    1
#define TIME_FMT_24H    2

#define THEME_DARK  0   // inverted: the default
#define THEME_LIGHT 1
#define THEME_AUTO  2   // dark through the night window

#define MODULE_NONE    0
#define MODULE_HR      1
#define MODULE_STEPS   2
#define MODULE_BATTERY 3
#define MODULE_WEATHER 4
#define MODULE_COUNT   3

#define TRANSIT_OFF   0   // no system: a plain watch, nothing asked of the phone
#define TRANSIT_CHIPS 1   // the countdown everywhere, and the hub's chips at the hub
#define TRANSIT_NEAR  2   // an older name for AUTO, kept for saved settings
#define TRANSIT_AUTO  3   // a plain watch until a hub it knows is near

typedef struct {
  uint8_t version;     // bumped whenever the fields below change
  bool wrist_right;    // true: rail on the left, sleeve from the right
  uint8_t offset;      // departure, minutes past the hour (0..headway-1)
  uint8_t headway;     // minutes between runs: 30, 20 or 15
  uint8_t buzz;        // BUZZ_*: where one pulse marks the block going solid
  uint8_t time_fmt;    // TIME_FMT_*
  uint8_t theme;       // THEME_*
  uint8_t night_start; // hour the night window opens
  uint8_t night_end;   // hour it closes
  uint32_t accent;     // 0xRRGGBB, snapped to the Pebble 64 at use
  uint8_t mod[3];      // MODULE_*, left to right from the wrist edge
  uint8_t mod_icons;   // MOD_ICONS_*: captions, icons, or icons in colour
  bool final_seconds;  // count the last minute down in seconds
  bool imperial;
  uint8_t transit;     // TRANSIT_*: what knowing the hub changes
  uint16_t radius;     // metres around the hub that count as near
  bool flick;          // a flick asks for the nearest stop
} Settings;

#define MOD_ICONS_OFF    0
#define MOD_ICONS_ON     1
#define MOD_ICONS_COLOUR 2

#define BUZZ_OFF    0
#define BUZZ_HUB    1   // only at the hub, where a countdown is a bus to catch
#define BUZZ_ALWAYS 2   // wherever the countdown runs

#define SETTINGS_KEY 1
#define SETTINGS_VERSION 5
#define WEATHER_KEY  2
#define TRANSIT_KEY  3
#define THRESHOLD 5   // minutes; block goes solid at or under this

typedef struct {
  int16_t temp;
  uint8_t code;
  bool valid;
  time_t at;
  int32_t rain_at;   // when rain is due to start, epoch seconds; 0 none due
} Weather;

// What the phone last said about the hub: whether the wearer is there in
// its hours, and the next departures of the routes on their own timetable,
// as minutes past midnight, 0xFFFF for none. The face counts down from
// these on its own; the phone refreshes them every few minutes.
#define TR_MAX 4
#define TR_NONE 0xFFFF
#define TR_STALE (3 * 60 * 60)   // seconds before the phone's word lapses
typedef struct {
  uint8_t state;           // 0 away or out of hours, 1 at the hub
  uint8_t area;            // 1 inside a system the face knows
  uint8_t near;            // 1 within a walk of a hub: the watch asks the phone to look as you walk
  uint16_t g[TR_MAX], b[TR_MAX];
  uint16_t km;             // outside every system: how far to the nearest, 0 unknown
  time_t at;
} Transit;

// The stop view: what the phone answered the last flick with. Shown for a
// few seconds, then the face is itself again.
#define SV_ROWS 3
#define SV_SHOW_MS 12000
#define SV_WAIT_MS 15000
#define SV_MIN_MS 8000    // an answer that lands late still gets this long
typedef struct { char route[8], head[20], when[24]; uint32_t color; bool live; } SvRow;
// Not on the Classic: its 24 KB holds neither the hub's view nor the yard's
// count and the heap too, so the phone sends it the board it always had.
#ifndef PBL_PLATFORM_APLITE
#define HAS_WAVE 1
#define HAS_YARD 1   // the yard's count in the countdown's place
#define HAS_RAIN 1   // RAIN IN 15 MIN beside the modules
#endif
static struct {
  bool valid, pending, lit;   // lit: a flick was heard; the seconds show until the answer's time is up
  bool wave;                  // the hub's answer: lines by departure time, not rows by route
  uint8_t take;               // a full-screen answer: 2 rows at a stop, 3 two columns, 4 the stops near; 0 none
#ifdef HAS_YARD
  int8_t out;                 // at the yard: how many buses are still out, -1 elsewhere
#endif
  // The hairline's countdown, from the flick: it drains from seg_f (in
  // ten-thousandths of the line) at seg_at to nothing at end_at, both in ms
  // since the flick at t0. An answer starts a new segment from where the line
  // is, so it never jumps, only slows.
  bool draining;
  time_t t0_s;
  uint16_t t0_ms;
  int32_t seg_at, end_at;
  int16_t seg_f;
  char stop[24];
  int dist, n;
  SvRow row[SV_ROWS];
  AppTimer *timer;
  AppTimer *drain;            // repaints the hairline as it shortens
} s_sv;
#ifdef HAS_YARD
#define AT_YARD() (s_sv.valid && s_sv.out >= 0)
// Coming home: for an hour after the day's last trip, at the yard, the phone
// says without a flick how many buses are still on trips (out), when the
// last trip ended (end) and when to stop saying so (until), in epoch seconds.
#define YARD_KEY 4
static struct { int8_t out; int32_t end, until; int32_t from, wuntil; bool near; } s_yd;
static time_t s_yd_asked;   // when the watch last asked the phone to look at the yard
// The buses out, from a flick's answer while it's up, else from the phone's
// word in its window; -1 when neither.
// Once none are on trips the minutes count up to YARD_AFTER, time enough for
// the last bus to finish a route and drive back, then the face is itself.
#define YARD_AFTER (25 * 60)
static bool yard_counting(void) {
  const time_t now = time(NULL);
  return s_yd.end && now < s_yd.until && now - s_yd.end < YARD_AFTER;
}
static int yard_out(void) {
  if (AT_YARD()) return s_sv.out;
  if (s_yd.out > 0 && time(NULL) < s_yd.until) return s_yd.out;
  if (s_yd.out == 0 && yard_counting()) return 0;
  return -1;
}
#define YARD_ON() (yard_out() >= 0)
#else
#define AT_YARD() false
#define YARD_ON() false
#endif

// The hub's answer, a line a departure minute: the minute, and every route
// leaving then as a badge, in route order. live has a bit a badge for a
// predicted time, away one for a bus not in at its bay yet. Not on
// aplite: its 24 KB could not hold it and the heap too, so the phone sends a
// Pebble Classic the board it always had. A line whose minute is empty (or
// only a day word) has no time: the flick's line of every route at the hub.
#ifndef PBL_PLATFORM_APLITE
#define WV_MAX 16
typedef struct { char when[16]; uint8_t n; char lab[WV_MAX][6]; uint32_t col[WV_MAX]; uint16_t live, away; } WaveLine;
static WaveLine s_wave[SV_ROWS];
#endif

static Settings s_set;
static Weather s_wx;
static Transit s_tr;

static void settings_defaults(void) {
  s_set.version = SETTINGS_VERSION;
  s_set.wrist_right = false;
  s_set.offset = 0;
  s_set.headway = 30;
  s_set.buzz = BUZZ_HUB;
  s_set.time_fmt = TIME_FMT_SYSTEM;
  s_set.theme = THEME_AUTO;   // dark through the night, light by day
  s_set.night_start = 19;
  s_set.night_end = 7;
  s_set.accent = 0x0055AA;   // Cobalt Blue, the design's one accent
  // Out of the box: heart rate and the weather as coloured icons, hung from
  // the outer edge over the date. A watch without a heart-rate sensor shows
  // its battery in that slot instead.
  s_set.mod[0] = MODULE_HR;
  s_set.mod[1] = MODULE_WEATHER;
  s_set.mod[2] = MODULE_NONE;
  s_set.mod_icons = MOD_ICONS_COLOUR;
  s_set.final_seconds = true;
  s_set.imperial = false;
  s_set.transit = TRANSIT_AUTO;
  s_set.radius = 100;   // every bay, and hardly a house
  s_set.flick = true;
}

static void settings_clamp(void) {
  if (s_set.headway != 30 && s_set.headway != 20 && s_set.headway != 15) {
    s_set.headway = 30;
  }
  if (s_set.offset >= s_set.headway) s_set.offset %= s_set.headway;
  if (s_set.time_fmt > TIME_FMT_24H) s_set.time_fmt = TIME_FMT_SYSTEM;
  if (s_set.theme > THEME_AUTO) s_set.theme = THEME_AUTO;
  if (s_set.night_start > 23) s_set.night_start = 19;
  if (s_set.night_end > 23) s_set.night_end = 7;
  s_set.accent &= 0xFFFFFF;
  for (int i = 0; i < MODULE_COUNT; i++) {
    if (s_set.mod[i] > MODULE_WEATHER) s_set.mod[i] = MODULE_NONE;
  }
  if (s_set.transit > TRANSIT_AUTO) s_set.transit = TRANSIT_AUTO;
  if (s_set.buzz > BUZZ_ALWAYS) s_set.buzz = BUZZ_HUB;
  if (s_set.radius < 50) s_set.radius = 50;
  if (s_set.radius > 2000) s_set.radius = 2000;
}

static void settings_load(void) {
  settings_defaults();
  // Only adopt a stored blob written by this layout. Size alone will not do
  // it: a new bool lands in the struct's existing padding, so the size is
  // unchanged while the byte it reads is whatever the old build left there —
  // which reads back as a setting silently stuck off.
  if (persist_exists(SETTINGS_KEY)) {
    const int n = persist_get_size(SETTINGS_KEY);
    Settings stored = s_set;   // the defaults, for whatever the blob lacks
    bool adopted = false;
    if (n == (int)sizeof(s_set)) {
      persist_read_data(SETTINGS_KEY, &stored, sizeof(stored));
      // Layout 4 is this one byte for byte; only the buzz's meaning moved.
      if (stored.version == SETTINGS_VERSION || stored.version == 4) { s_set = stored; adopted = true; }
    } else if (n >= (int)offsetof(Settings, transit) && n < (int)sizeof(s_set)) {
      // An older layout: the same fields up to where new ones were appended.
      // Carry it over rather than hand the wearer the defaults again.
      persist_read_data(SETTINGS_KEY, &stored, n);
      if (stored.version >= 1 && stored.version <= SETTINGS_VERSION) { s_set = stored; adopted = true; }
      // Before layout 4 the hub was off unless chosen; now it is automatic
      // unless chosen, and an unchosen off reads as automatic.
      if (stored.version < 4 && s_set.transit == TRANSIT_OFF) s_set.transit = TRANSIT_AUTO;
    }
    // Before layout 5 the buzz was a switch, off unless chosen. A chosen on
    // meant wherever the countdown ran; an unchosen off reads as the hub.
    if (adopted && stored.version < 5) s_set.buzz = stored.buzz ? BUZZ_ALWAYS : BUZZ_HUB;
  }
  s_set.version = SETTINGS_VERSION;
  settings_clamp();
}

static void settings_save(void) {
  persist_write_data(SETTINGS_KEY, &s_set, sizeof(s_set));
}

static void weather_load(void) {
  memset(&s_wx, 0, sizeof(s_wx));
  if (persist_exists(WEATHER_KEY)
      && persist_get_size(WEATHER_KEY) == (int)sizeof(s_wx)) {
    persist_read_data(WEATHER_KEY, &s_wx, sizeof(s_wx));
  }
  // A reading more than three hours old is worse than no reading.
  if (s_wx.valid && time(NULL) - s_wx.at > 3 * 60 * 60) s_wx.valid = false;
}

static void transit_load(void) {
  memset(&s_tr, 0, sizeof(s_tr));
  if (persist_exists(TRANSIT_KEY)
      && persist_get_size(TRANSIT_KEY) == (int)sizeof(s_tr)) {
    persist_read_data(TRANSIT_KEY, &s_tr, sizeof(s_tr));
  }
}
static void transit_save(void) {
  persist_write_data(TRANSIT_KEY, &s_tr, sizeof(s_tr));
}
// What the setting comes to right now: automatic is the second countdown
// and the flick inside a known system, and nothing outside one.
static uint8_t transit_mode(void) {
  if (s_set.transit == TRANSIT_AUTO || s_set.transit == TRANSIT_NEAR) return s_tr.area ? TRANSIT_CHIPS : TRANSIT_OFF;
  return s_set.transit;
}
// The phone's word holds for a few hours, then the face falls back to the
// plain countdown rather than stay quiet on a stale fix.
static bool transit_fresh(time_t now) {
  return transit_mode() != TRANSIT_OFF && s_tr.at != 0 && now - s_tr.at < TR_STALE;
}
// Minutes until the first of a route's departures still ahead, or -1.
// Minutes since the latest departure gone in the last BOARD_WAIT, or -1: a
// loop bus that late may still be at its bay. Not on the Classic, short of
// room for it.
#ifndef PBL_PLATFORM_APLITE
#define BOARD_WAIT 10
static int transit_prev(const uint16_t *list, int now_min) {
  int best = -1;
  for (int i = 0; i < TR_MAX; i++) {
    const int ago = now_min - (int)list[i];
    if (list[i] != TR_NONE && ago > 0 && ago <= BOARD_WAIT && (best < 0 || ago < best)) best = ago;
  }
  return best;
}
#endif
static int transit_next(const uint16_t *list, int now_min) {
  for (int i = 0; i < TR_MAX; i++) {
    if (list[i] != TR_NONE && (int)list[i] >= now_min) return (int)list[i] - now_min;
  }
  return -1;
}

// ------------------------------------------------------------------ layout

#define REF_W 144

// Reference metrics, in 144-wide pixels.
#define RAIL_W        7
#define RAIL_BORDER   1
#define PAD_TOP       8
#define PAD_BOTTOM    7
#define PAD_WRIST     8   // inline-start: the wrist side
#define PAD_GUTTER    6   // inline-end: between content and the rail
#define DATE_PAD_BOT  5
#define BLOCK_PAD_T   4
#define BLOCK_PAD_B   2
#define BLOCK_PAD_IN  6   // toward the wrist
#define BLOCK_PAD_OUT 5   // toward the rail
#define BLOCK_MIN_W  65
#define LABEL_GAP     2   // number to its MIN, on the baseline
// Label cap bottom to number cap top. The Gothic label's estimated cap runs a
// pixel long, two on emery's larger face, so the constant carries the slack.
#ifdef PBL_PLATFORM_EMERY
  #define BLOCK_ROW_GAP 9
#else
  #define BLOCK_ROW_GAP 8
#endif
#define DOW_GAP       5
#define YARD_GAP      6   // the yard's label, above the count's digits
#define ROW_GAP       2
#define MOD_GAP       9
#define MOD_LABEL_GAP 4
#define MOD_ICON_GAP  4
#define BLOCK_PAD_MIN 3
#define BLOCK_ROW_GAP_P 3   // and under a peek
#define TIME_MARGIN_TOP_P 4
#define TIME_MARGIN_TOP 10
#define COLON_GAP     0   // the design's 2px margin, less its tracking

// The design's own face. System LECO tops out at 42px, which is half the
// scale the mock draws, so the numerals ship as a resource and scale per
// platform. Labels stay in the system Gothic the build notes ask for.
#ifdef PBL_PLATFORM_EMERY
  #define RES_TIME  RESOURCE_ID_FONT_TIME_83
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_61
  #define RES_MOD   RESOURCE_ID_FONT_MOD_21
  #define RES_LABEL RESOURCE_ID_FONT_LABEL_15
  #define RES_DATE  RESOURCE_ID_FONT_DATE_17
  #define RES_BOARD RESOURCE_ID_FONT_BOARD_17
  #define RES_CAP   RESOURCE_ID_FONT_CAP_12
  #define RES_BIGDATE RESOURCE_ID_FONT_DATE_25
  #define RES_COUNT_S RESOURCE_ID_FONT_COUNT_42
  #define RES_TIME_P  RESOURCE_ID_FONT_TIME_53
#else
  #define RES_TIME  RESOURCE_ID_FONT_TIME_60
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_44
  #define RES_MOD   RESOURCE_ID_FONT_MOD_15
  #define RES_LABEL RESOURCE_ID_FONT_LABEL_11
  #define RES_DATE  RESOURCE_ID_FONT_DATE_12
  #define RES_BOARD RESOURCE_ID_FONT_BOARD_12
  #define RES_CAP   RESOURCE_ID_FONT_CAP_9
  #define RES_BIGDATE RESOURCE_ID_FONT_DATE_18
  #define RES_COUNT_S RESOURCE_ID_FONT_COUNT_30
  #define RES_TIME_P  RESOURCE_ID_FONT_TIME_38
#endif

static GFont s_f_time, s_f_count, s_f_mod, s_f_label, s_f_date, s_f_cap, s_f_bigdate;
static GFont s_f_board;   // the board's clock times, in the wider cut
static GFont s_f_count_s;   // the countdown a size down, under a timeline peek
static GFont s_f_time_p;                // the time under a timeline peek
static Window *s_window;
static Layer *s_face;
static int s_last_remaining = -1;
static int16_t s_w = REF_W;   // display width, for scaling

static int sc(int v) { return (v * s_w + REF_W / 2) / REF_W; }

// x of a logical box, measured from the wrist edge, mapped to the screen.
static int16_t mapx(int lx, int w) {
  return s_set.wrist_right ? (int16_t)(s_w - lx - w) : (int16_t)lx;
}

// Every call into the firmware's text engine goes through one of two leaves.
// The engine wants over a kilobyte of the app's 2KB stack on the watches
// this runs on, so what sits beneath it must be nearly nothing: a leaf's
// arguments live in static storage and its frame is the link register and
// the words the ABI spills.
static GContext *s_ctx;                                   // the frame being painted
static struct { const char *t; GFont f; GRect r; GTextOverflowMode mode; } s_tx;

static GSize measure(const char *t, GFont f) __attribute__((noinline));
static GSize measure(const char *t, GFont f) {
  return graphics_text_layout_get_content_size(
      t, f, GRect(0, 0, 400, 200), GTextOverflowModeWordWrap, GTextAlignmentLeft);
}
static void tx_draw(void) __attribute__((noinline));
static void tx_draw(void) {
  graphics_draw_text(s_ctx, s_tx.t, s_tx.f, s_tx.r, s_tx.mode, GTextAlignmentLeft, NULL);
}

// A system font's glyph box carries fixed top bearing and sits shorter than
// the box: LECO 42 draws a 30px glyph 11px down. Everything below is placed
// optically, from the glyph, so blocks and baselines land where the design
// puts them at any font scale.
#define BARLOW_BEARING 30
#define BARLOW_CAP 70
typedef struct { int bearing, cap; } Metrics;
static Metrics barlow_metrics(int boxh) {
  Metrics m = { boxh * BARLOW_BEARING / 100, boxh * BARLOW_CAP / 100 };
  return m;
}
static Metrics s_m_cap;   // the small caps', measured once the font loads

// Draw text with its measured box placed at a logical origin. Inlined: the
// caller stores the arguments and drops straight to the leaf.
static inline __attribute__((always_inline))
void draw_at(const char *t, GFont f, int lx, int y, GSize sz) {
  s_tx.t = t; s_tx.f = f; s_tx.mode = GTextOverflowModeTrailingEllipsis;
  s_tx.r = GRect(mapx(lx, sz.w), y, sz.w, sz.h);
  tx_draw();
}

// Text drawn a glyph at a time, so the face can do what the design's CSS
// does and the Pebble text layer cannot: tabular figures (every digit takes
// the advance of a zero, so a 1 no longer pulls its neighbours in and 10:11
// is as wide as 00:00) and tracking (the design's labels are set with 6-10%
// of letter-space). The design's -0.03em tracking on the numerals is folded
// in: a tabular cell less that tracking is, to the pixel, a zero's own
// advance in this face.
// One UTF-8 sequence — the degree sign is two bytes — copied into a buffer.
static int glyph_at(const char *p, char *out) {
  const unsigned char c = (unsigned char)*p;
  const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
  int i = 0;
  for (; i < n && p[i]; i++) out[i] = p[i];
  out[i] = 0;
  return i;
}

static char s_glyph[5];
static int run_w(const char *t, GFont f, bool tabular, int track) {
  const int cell = tabular ? measure("0", f).w : 0;
  int w = 0;
  for (const char *p = t; *p;) {
    p += glyph_at(p, s_glyph);
    w += ((tabular && isdigit((int)s_glyph[0])) ? cell : measure(s_glyph, f).w) + (*p ? track : 0);
  }
  return w;
}

// A run is described in static storage and drawn by one function whose
// frame is a few saved registers. It draws from a screen x, advancing
// rightwards: the glyphs keep their reading order whichever wrist the face
// is laid out for.
static struct { const char *t; GFont f; int x, y, track; bool tab; } s_run;
static void run_draw(void) __attribute__((noinline));
static void run_draw(void) {
  const int cell = s_run.tab ? measure("0", s_run.f).w : 0;
  for (const char *p = s_run.t; *p;) {
    p += glyph_at(p, s_glyph);
    const GSize z = measure(s_glyph, s_run.f);
    // A glyph boxed at exactly its measured width can still be judged not
    // to fit and drawn as an ellipsis, so the box gets slack past its end.
    s_tx.t = s_glyph; s_tx.f = s_run.f; s_tx.mode = GTextOverflowModeWordWrap;
    s_tx.r = GRect(s_run.x, s_run.y, 2 * z.w + 4, z.h);
    tx_draw();
    s_run.x += ((s_run.tab && isdigit((int)s_glyph[0])) ? cell : z.w) + s_run.track;
  }
}
static inline __attribute__((always_inline))
void draw_run_s(const char *t, GFont f, int x, int y, bool tabular, int track) {
  s_run.t = t; s_run.f = f; s_run.x = x; s_run.y = y; s_run.tab = tabular; s_run.track = track;
  run_draw();
}
// The same run placed by its logical x, measured from the wrist edge.
static inline __attribute__((always_inline))
void draw_run(const char *t, GFont f, int lx, int y, bool tabular, int track) {
  draw_run_s(t, f, mapx(lx, run_w(t, f, tabular, track)), y, tabular, track);
}

#define TRACK 1   // the design's 6-10% letter-space, one pixel at these sizes

// ---------------------------------------------------------------- icons
// Drawn rather than bundled: they invert with the theme, scale with the
// display, and cost no resource budget.

// Every icon is a hand-placed ten-by-ten pattern, painted nearest-neighbour
// into its box. Composed circles and rounded rects cannot hold a recognisable
// shape at ten pixels; placing the pixels by hand can, and a pattern still
// scales up for emery and inverts with the theme.
static void draw_bits(GContext *ctx, GRect r, const char *const *rows,
                      int rw, int rh, char mark) {
  for (int y = 0; y < rh; y++) {
    for (int x = 0; x < rw; x++) {
      if (rows[y][x] != mark) continue;
      // Rounded, not floored: rounding keeps the stretched columns
      // symmetric, so a symmetric glyph stays symmetric at emery's size.
      const int16_t x0 = r.origin.x + (2 * x * r.size.w + rw) / (2 * rw);
      const int16_t x1 = r.origin.x + (2 * (x + 1) * r.size.w + rw) / (2 * rw);
      const int16_t y0 = r.origin.y + (2 * y * r.size.h + rh) / (2 * rh);
      const int16_t y1 = r.origin.y + (2 * (y + 1) * r.size.h + rh) / (2 * rh);
      graphics_fill_rect(ctx, GRect(x0, y0, x1 > x0 ? x1 - x0 : 1,
                                    y1 > y0 ? y1 - y0 : 1), 0, GCornerNone);
    }
  }
}

// A pair of footprints mid-stride: each is a ball with the heel set off
// below it, and the two sit at different heights. The break between ball
// and heel is what makes a print a foot rather than a drop, and the stagger
// is what makes two of them a walk.
static const char *const FOOTPRINTS[10] = {
  "..###.....",
  ".####.....",
  ".####.###.",
  ".####.####",
  "..##..####",
  "......####",
  ".###...##.",
  ".###......",
  "......###.",
  "......###.",
};

static void icon_steps(GContext *ctx, GRect r) {
  draw_bits(ctx, r, FOOTPRINTS, 10, 10, '#');
}

static void icon_battery(GContext *ctx, GRect r, int pct, GColor tint) {
  const int16_t h = r.size.h * 3 / 4;
  const int16_t y = r.origin.y + (r.size.h - h) / 2;
  const int16_t bw = r.size.w - 2;
  graphics_draw_rect(ctx, GRect(r.origin.x, y, bw, h));
  graphics_fill_rect(ctx, GRect(r.origin.x + bw, y + h / 3, 2, h / 3), 0, GCornerNone);
  // Inset by two so the outline still reads at a full charge, and never let a
  // non-empty battery draw as empty.
  const int16_t inner = bw - 4;
  int16_t fill = (inner * pct + 50) / 100;
  if (fill <= 0 && pct > 0) fill = 1;
  graphics_context_set_fill_color(ctx, tint);
  if (fill > 0) graphics_fill_rect(ctx, GRect(r.origin.x + 2, y + 2, fill, h - 4), 0, GCornerNone);
}

// A lightning bolt, set beside the battery while it charges; the battery
// itself keeps its fill and the number stays.
static const char *const BOLT[10] = {
  "...##",
  "..##.",
  "..##.",
  ".##..",
  "#####",
  "..##.",
  ".##..",
  ".##..",
  "##...",
  "#....",
};

static const char *const HEART[10] = {
  "..........",
  ".##....##.",
  "####..####",
  "##########",
  "##########",
  ".########.",
  "..######..",
  "...####...",
  "....##....",
  "..........",
};

static void icon_heart(GContext *ctx, GRect r) {
  draw_bits(ctx, r, HEART, 10, 10, '#');
}

#define WX_SUN    0
#define WX_PARTLY 1
#define WX_CLOUD  2
#define WX_FOG    3
#define WX_RAIN   4
#define WX_SNOW   5
#define WX_STORM  6

// WMO codes, as Open-Meteo reports them. Overcast is a cloud and nothing
// more: adding precipitation to it would make it indistinguishable from rain.
static int wx_kind(int code) {
  if (code <= 1) return WX_SUN;
  if (code == 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if (code >= 95) return WX_STORM;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
  return WX_CLOUD;
}

// The sky, one pattern per kind. Every cloud is the same silhouette — a
// raised lump beside a lower shoulder on a flat base — so the family reads
// as one; what falls from it is what changes.
static const char *const SKY[7][10] = {
  { // sun
    "....##....",
    ".#......#.",
    "...####...",
    "..######..",
    "#.######.#",
    "#.######.#",
    "..######..",
    "...####...",
    ".#......#.",
    "....##....",
  },
  { // partly: the sun's disc high on the outer side, two rays, the cloud in front
    "....@.@@.@",
    ".....@@@@.",
    "....@@@@@.",
    "...#####..",
    "..######..",
    ".#########",
    "##########",
    "##########",
    ".########.",
    "..........",
  },
  { // cloud
    "..........",
    "..........",
    ".....###..",
    "....#####.",
    ".##.#####.",
    ".#########",
    "##########",
    "##########",
    ".########.",
    "..........",
  },
  { // fog
    "..........",
    "..........",
    "..########",
    "..........",
    "..........",
    "########..",
    "..........",
    "..........",
    "..########",
    "..........",
  },
  { // rain: slanted streaks
    ".....###..",
    "....#####.",
    ".##.#####.",
    ".#########",
    "##########",
    ".########.",
    "..........",
    "..#..#..#.",
    ".#..#..#..",
    "..........",
  },
  { // snow: a scatter
    ".....###..",
    "....#####.",
    ".##.#####.",
    ".#########",
    "##########",
    ".########.",
    "..........",
    ".#...#...#",
    "...#...#..",
    "..........",
  },
  { // storm: a shorter cloud, and the bolt gets the room
    ".....###..",
    ".##.#####.",
    ".#########",
    "##########",
    ".########.",
    ".....@@...",
    "....@@....",
    "...@@@@...",
    "....@@....",
    "...@@.....",
  },
};

// Whole-sky kinds take the tint entire; a cloud is never coloured, so
// partly and storm colour only what is marked: the sun, the bolt.
static void icon_weather(GContext *ctx, GRect r, int code, GColor base, GColor tint) {
  const int k = wx_kind(code);
  const bool whole = (k == WX_SUN || k == WX_RAIN || k == WX_SNOW);
  graphics_context_set_fill_color(ctx, whole ? tint : base);
  draw_bits(ctx, r, SKY[k], 10, 10, '#');
  graphics_context_set_fill_color(ctx, tint);
  draw_bits(ctx, r, SKY[k], 10, 10, '@');
}

// The caption for the same sky, when the modules are captioned.
static const char *wx_caption(int code) {
  static const char *const words[7] = {
    "SUNNY", "PARTLY", "CLOUDY", "FOG", "RAIN", "SNOW", "STORM" };
  return words[wx_kind(code)];
}

// --------------------------------------------------------------- schedule

typedef struct {
  int remaining;      // minutes until the next departure
  int secs;           // seconds until it, within the final minute
  int next_h, next_m; // clock time of that departure
  bool is_now, is_boarding, is_final;
} Schedule;

static Schedule schedule_for(int hour, int minute, int second) {
  Schedule s;
  int hw = s_set.headway;
  s.remaining = ((s_set.offset - minute) % hw + hw) % hw;
  s.secs = 60 - second;
  s.next_h = hour;
  s.next_m = minute + s.remaining;
  if (s.next_m >= 60) { s.next_m -= 60; s.next_h = (s.next_h + 1) % 24; }
  s.is_now = (s.remaining == 0);
  s.is_boarding = (s.remaining > 0 && s.remaining <= THRESHOLD);
  // The last minute, counted in seconds — the one place the face moves.
  s.is_final = s_set.final_seconds && s.remaining == 1;
  return s;
}

static bool use_24h(void) {
  switch (s_set.time_fmt) {
    case TIME_FMT_12H: return false;
    case TIME_FMT_24H: return true;
    default: return clock_is_24h_style();
  }
}

static int display_hour(int h) {
  if (use_24h()) return h;
  int r = h % 12;
  return r == 0 ? 12 : r;
}

// ------------------------------------------------------------------ render

// Theme: the face is inverted by default. Auto follows the night window.
static bool is_night(int hour) {
  const int a = s_set.night_start, b = s_set.night_end;
  return (a <= b) ? (hour >= a && hour < b) : (hour >= a || hour < b);
}

static bool dark_now(int hour) {
  switch (s_set.theme) {
    case THEME_LIGHT: return false;
    case THEME_AUTO:  return is_night(hour);
    default:          return true;
  }
}

static GColor s_ground, s_ink, s_dim;

static void theme_apply(int hour) {
  if (dark_now(hour)) {
    s_ground = GColorBlack;
    s_ink = GColorWhite;
    s_dim = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  } else {
    s_ground = GColorWhite;
    s_ink = GColorBlack;
    s_dim = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  }
}

// The accent is a user colour, snapped to the Pebble 64. One-bit builds have
// no accent to give, so it becomes ink, as the design's mono note asks.
static GColor accent(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(s_set.accent);
#else
  return s_ink;
#endif
}

// Text laid over a filled block: pick whichever of ink/ground reads on it.
static GColor on_fill(GColor fill) {
#ifdef PBL_COLOR
  const int lum = fill.r * 77 + fill.g * 151 + fill.b * 28;   // channels are 0..3
  return lum > (3 * 256) / 2 ? GColorBlack : GColorWhite;
#else
  return gcolor_equal(fill, GColorWhite) ? GColorBlack : GColorWhite;
#endif
}

// The 7 1/2 and 22 1/2 minute ticks read lighter than the half-hour tick.
static void draw_soft_tick(GContext *ctx, int16_t x0, int16_t x1, int16_t y) {
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, s_dim);
  graphics_draw_line(ctx, GPoint(x0, y), GPoint(x1, y));
#else
  graphics_context_set_stroke_color(ctx, s_ink);
  for (int16_t x = x0; x <= x1; x += 2) {
    graphics_draw_pixel(ctx, GPoint(x, y));
  }
#endif
}

static void draw_rail(GContext *ctx, const Schedule *sch, int16_t h) {
  const int rail_w = sc(RAIL_W), border = sc(RAIL_BORDER);
  const int16_t rx = mapx(s_w - rail_w, rail_w);           // rail column
  const int16_t bx = mapx(s_w - rail_w - border, border);  // its inner hairline

  graphics_context_set_fill_color(ctx, s_ink);
  graphics_fill_rect(ctx, GRect(bx, 0, border, h), 0, GCornerNone);

  // Drains as the headway runs out: full just after a departure, empty at zero.
  int fill = (sch->remaining * h + s_set.headway / 2) / s_set.headway;
  if (fill > 0) {
    graphics_context_set_fill_color(ctx, accent());
    graphics_fill_rect(ctx, GRect(rx, h - fill, rail_w, fill), 0, GCornerNone);
  }

  const int16_t x0 = rx, x1 = rx + rail_w - 1;
  draw_soft_tick(ctx, x0, x1, h / 4);
  graphics_context_set_fill_color(ctx, s_ink);
  graphics_fill_rect(ctx, GRect(x0, h / 2, rail_w, sc(RAIL_BORDER)), 0, GCornerNone);
  draw_soft_tick(ctx, x0, x1, (h * 3) / 4);
}

// Away from the hub the rail is only a hairline, and only through a flick.
static bool s_quiet_face;   // the plain face is up, so its hairline shows
// Milliseconds since the flick.
static int32_t since_flick(void) {
  time_t now_s; uint16_t now_ms;
  time_ms(&now_s, &now_ms);
  return (int32_t)(now_s - s_sv.t0_s) * 1000 + now_ms - s_sv.t0_ms;
}
// How much of the hairline is left, in ten-thousandths, or -1 with no flick.
static int32_t line_left(void) {
  if (!s_sv.lit || !s_sv.draining) return -1;
  const int32_t t = since_flick();
  if (t >= s_sv.end_at) return 0;
  if (t <= s_sv.seg_at) return s_sv.seg_f;
  return (int32_t)s_sv.seg_f * (s_sv.end_at - t) / (s_sv.end_at - s_sv.seg_at);
}
static void draw_hairline(GContext *ctx, int16_t h) {
  const int border = sc(RAIL_BORDER);
  graphics_context_set_fill_color(ctx, s_ink);
  // Only a flick draws it: the line is the answer's time, full at the
  // flick, shortening from the foot, gone as the answer goes. At rest the
  // edge is bare; a line there with nothing to count reads as a gap. It runs
  // down the rail's column, near its outer end, clear of the centred face.
  const int32_t left = line_left();
  if (left <= 0) return;
  h = (int16_t)((int32_t)h * left / 10000);
  graphics_fill_rect(ctx, GRect(mapx(s_w - border - sc(2), border), 0, border, h), 0, GCornerNone);
}

// A module is a caption over a value, as the design draws them — BPM 72,
// STEPS 4.8K, BATT 64%. Weather captions with the sky itself — CLOUDY 12° —
// since the condition says more than the unit. The captions can be swapped
// for icons.
typedef struct {
  bool present;
  char value[8];
  const char *caption;
  uint8_t kind;
  int extra;            // battery percent / weather code
  bool charging;        // a battery on the charger: a bolt beside the icon
} Module;

static bool module_read(uint8_t kind, Module *m) {
  m->kind = kind;
  m->extra = 0;
  switch (kind) {
    case MODULE_HR: {
#ifdef PBL_HEALTH
      const HealthServiceAccessibilityMask ok =
          health_service_metric_accessible(HealthMetricHeartRateBPM,
                                           time(NULL) - 60 * 60, time(NULL));
      if (ok & HealthServiceAccessibilityMaskAvailable) {
        const int bpm = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
        if (bpm > 0) {
          snprintf(m->value, sizeof(m->value), "%d", bpm);
          m->caption = "BPM";
          return true;
        }
      }
#endif
      return false;
    }
    case MODULE_STEPS: {
#ifdef PBL_HEALTH
      const HealthServiceAccessibilityMask ok =
          health_service_metric_accessible(HealthMetricStepCount,
                                           time_start_of_today(), time(NULL));
      if (ok & HealthServiceAccessibilityMaskAvailable) {
        const int st = (int)health_service_sum_today(HealthMetricStepCount);
        if (st >= 1000) snprintf(m->value, sizeof(m->value), "%d.%dK", st / 1000, (st % 1000) / 100);
        else snprintf(m->value, sizeof(m->value), "%d", st);
        m->caption = "STEPS";
        return true;
      }
#endif
      return false;
    }
    case MODULE_BATTERY: {
      const BatteryChargeState b = battery_state_service_peek();
      snprintf(m->value, sizeof(m->value), "%d%%", b.charge_percent);
      m->caption = (b.is_charging || b.is_plugged) ? "CHG" : "BATT";
      m->extra = b.charge_percent;
      return true;
    }
    case MODULE_WEATHER: {
      if (!s_wx.valid) return false;
      snprintf(m->value, sizeof(m->value), "%d\u00B0", s_wx.temp);
      m->caption = wx_caption(s_wx.code);
      m->extra = s_wx.code;
      return true;
    }
    default: return false;
  }
}

static bool module_uses_icon(const Module *m) {
  (void)m;
  return s_set.mod_icons != MOD_ICONS_OFF;
}

static int module_icon_w(const Module *m, int icon) {
  switch (m->kind) {
    case MODULE_BATTERY: return icon * 8 / 5 + (m->charging ? icon / 2 + sc(2) : 0);
    case MODULE_STEPS:   return icon;
    default:             return icon;
  }
}

// Colour where it says something: the heart red, the battery by its charge,
// the sky in its own colour. Steps stay in the caption grey, and so does
// every cloud. One-bit watches have only the grey.
static GColor module_tint(const Module *m) {
#ifdef PBL_COLOR
  if (s_set.mod_icons != MOD_ICONS_COLOUR) return s_dim;
  switch (m->kind) {
    case MODULE_HR:      return GColorFromHEX(0xFF0055);
#ifdef PBL_PLATFORM_EMERY
    // The Time 2's fuel gauge counts single percents: amber at a tenth, red
    // at a twentieth.
    case MODULE_BATTERY: return GColorFromHEX(m->extra > 10 ? 0x00AA55 : m->extra > 5 ? 0xFFAA00 : 0xFF0000);
#else
    // The Time counts in tens, so there is no 5 to see: red at a tenth.
    case MODULE_BATTERY: return GColorFromHEX(m->extra > 10 ? 0x00AA55 : 0xFF0000);
#endif
    case MODULE_WEATHER:
      switch (wx_kind(m->extra)) {
        // Yellow on the dark ground, where it glows; orange on the light,
        // where yellow would vanish into the white.
        case WX_SUN: case WX_PARTLY: case WX_STORM:
          return gcolor_equal(s_ground, GColorBlack) ? GColorFromHEX(0xFFFF00) : GColorFromHEX(0xFF5500);
        case WX_RAIN: return GColorFromHEX(0x55AAFF);
        case WX_SNOW: return GColorFromHEX(0x00AAFF);
        default: return s_dim;
      }
    default: return s_dim;
  }
#else
  (void)m;
  return s_dim;
#endif
}

static void module_draw_icon(GContext *ctx, const Module *m, GRect box) {
  const GColor tint = module_tint(m);
  switch (m->kind) {
    case MODULE_HR:      graphics_context_set_fill_color(ctx, tint); icon_heart(ctx, box); break;
    case MODULE_STEPS:   icon_steps(ctx, box); break;
    case MODULE_BATTERY:
      if (m->charging) {
        // The battery as ever, then the bolt after its nub: yellow on the
        // dark ground and orange on the light, as the sun is, grey where the
        // icons are.
        const int bw = box.size.h * 8 / 5, lw = box.size.h / 2;
        icon_battery(ctx, GRect(box.origin.x, box.origin.y, bw, box.size.h), m->extra, tint);
        graphics_context_set_fill_color(ctx, s_set.mod_icons == MOD_ICONS_COLOUR
            ? PBL_IF_COLOR_ELSE(gcolor_equal(s_ground, GColorWhite) ? GColorFromHEX(0xFF5500) : GColorFromHEX(0xFFFF00), s_dim) : s_dim);
        draw_bits(ctx, GRect(box.origin.x + bw + sc(2), box.origin.y, lw, box.size.h), BOLT, 5, 10, '#');
      } else {
        icon_battery(ctx, box, m->extra, tint);
      }
      break;
    case MODULE_WEATHER: icon_weather(ctx, box, m->extra, s_dim, tint); break;
    default: break;
  }
}

// The row sits on the wrist side of the band the design leaves open, so a
// sleeve covers it before the countdown or the rail. Laid out into static
// storage, then painted from it.
static struct {
  Module m[MODULE_COUNT];
  int n, widths[MODULE_COUNT], x0, y, label_h, lgap, gap, icon, total;
  Metrics m_cap, m_val;
  GFont f_val, f_cap;
} s_md;

static void layout_modules(GFont f_val, GFont f_cap, int c_start, int avail_w,
                           int band_top, int band_bot) __attribute__((noinline));
static void layout_modules(GFont f_val, GFont f_cap, int c_start, int avail_w,
                           int band_top, int band_bot) {
  s_md.n = 0;
  // On the charger the battery is what's worth a glance. It takes the slot
  // of something that can't change there — heart rate, which reads nothing
  // off the wrist, then steps, then an empty slot — unless a slot already
  // shows it. Weather keeps its place. Unplugged, the slot is the wearer's.
  int charger = -1;
  const BatteryChargeState bs = battery_state_service_peek();
  if (bs.is_plugged || bs.is_charging) {
    static const uint8_t give[] = { MODULE_HR, MODULE_STEPS, MODULE_NONE };
    bool shown = false;
    for (int j = 0; j < MODULE_COUNT; j++) if (s_set.mod[j] == MODULE_BATTERY) shown = true;
    for (int g = 0; !shown && charger < 0 && g < 3; g++)
      for (int j = 0; charger < 0 && j < MODULE_COUNT; j++) if (s_set.mod[j] == give[g]) charger = j;
  }
  for (int i = 0; i < MODULE_COUNT; i++) {
    uint8_t kind = i == charger ? MODULE_BATTERY : s_set.mod[i];
    if (kind == MODULE_NONE) continue;
    if (module_read(kind, &s_md.m[s_md.n])) {
      s_md.m[s_md.n].charging = kind == MODULE_BATTERY && (bs.is_plugged || bs.is_charging);
      s_md.n++;
      continue;
    }
    // No heart-rate sensor, or no reading yet: the slot shows the battery,
    // or steps if the battery already has a slot of its own.
    if (kind != MODULE_HR) continue;
    kind = MODULE_BATTERY;
    for (int j = 0; j < MODULE_COUNT; j++) if (s_set.mod[j] == MODULE_BATTERY) kind = MODULE_STEPS;
    if (module_read(kind, &s_md.m[s_md.n])) {
      s_md.m[s_md.n].charging = kind == MODULE_BATTERY && (bs.is_plugged || bs.is_charging);
      s_md.n++;
    }
  }
  if (s_md.n == 0) return;

  s_md.f_val = f_val; s_md.f_cap = f_cap;
  s_md.m_val = barlow_metrics(measure("88", f_val).h);
  s_md.icon = s_md.m_val.cap;                 // an icon reads as one cap tall
  // Captions are laid out from the measured box, not an estimated cap: the
  // box bounds the glyph whatever the font's metrics turn out to be. An icon
  // fills its box where a caption's box carries slack, so it gets a wider
  // gap to the value.
  const bool icons = s_set.mod_icons != MOD_ICONS_OFF;
  s_md.m_cap = barlow_metrics(measure("BPM", f_cap).h);
  // The value line sits where the captions put it whichever labels are on:
  // an icon is taller than a caption and stands up into the band's air
  // rather than push the numbers down.
  s_md.label_h = s_md.m_cap.cap;
  s_md.lgap = icons ? sc(MOD_ICON_GAP) : sc(MOD_LABEL_GAP);
  s_md.gap = sc(MOD_GAP);

  // Widths first: the row is dropped whole if it cannot fit the band.
  int total = 0;
  for (int i = 0; i < s_md.n; i++) {
    int w = run_w(s_md.m[i].value, f_val, true, 0);
    if (module_uses_icon(&s_md.m[i])) {
      const int iw = module_icon_w(&s_md.m[i], s_md.icon);
      if (iw > w) w = iw;
    } else {
      const int cw = run_w(s_md.m[i].caption, f_cap, false, TRACK);
      if (cw > w) w = cw;
    }
    s_md.widths[i] = w;
    total += w + (i ? s_md.gap : 0);
  }
  while (s_md.n > 1 && total > avail_w) { s_md.n--; total -= s_md.widths[s_md.n] + s_md.gap; }
  if (total > avail_w) { s_md.n = 0; return; }

  const int row_h = s_md.label_h + s_md.lgap + s_md.m_val.cap;
  // A timeline peek squeezes the band to nothing. The modules are the lowest
  // zone in the design's ranking, so they go first rather than crowd the
  // countdown — the same order a sleeve would take them in.
  if (band_bot - band_top < row_h) { s_md.n = 0; return; }
  // Centred, then the design's 4px padding-top nudges it down by half that.
  s_md.y = (band_top + band_bot) / 2 - row_h / 2 + sc(1);
  s_md.x0 = c_start;
  s_md.total = total;
}

// How far the row may run past the outer edge: a degree sign ending it hangs
// there, as a mark in a margin does, since it's air to the eye and the 12
// would otherwise read as set in from the date and time beside it. Only on
// the left wrist, where the value's end is the outer edge.
static int modules_hang(void) __attribute__((noinline));
static int modules_hang(void) {
  if (s_set.wrist_right || !s_md.n) return 0;
  const Module *m = &s_md.m[s_md.n - 1];
  if (m->kind != MODULE_WEATHER || !module_uses_icon(m)) return 0;   // a caption (PARTLY) may be the wider
  return measure("\u00B0", s_md.f_val).w;
}

static void paint_modules(void) __attribute__((noinline));
static void paint_modules(void) {
  int x = s_md.x0;
  for (int i = 0; i < s_md.n; i++) {
    const Module *m = &s_md.m[i];
    if (module_uses_icon(m)) {
      graphics_context_set_fill_color(s_ctx, s_dim);
      graphics_context_set_stroke_color(s_ctx, s_dim);
      const int iw = module_icon_w(m, s_md.icon);
      module_draw_icon(s_ctx, m, GRect(mapx(x, iw), s_md.y + s_md.label_h - s_md.icon, iw, s_md.icon));
    } else {
      graphics_context_set_text_color(s_ctx, s_dim);
      draw_run(m->caption, s_md.f_cap, x, s_md.y + s_md.label_h - s_md.m_cap.cap - s_md.m_cap.bearing, false, TRACK);
    }
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(m->value, s_md.f_val, x, s_md.y + s_md.label_h + s_md.lgap - s_md.m_val.bearing, true, 0);
    x += s_md.widths[i] + s_md.gap;
  }
}

typedef struct { int start, end, top, bot; } Frame;   // the content box, in logical x

// ---- the second countdown: routes that leave the hub together on their own
// timetable. It sits at the outer end of the band the modules share, in the
// module grammar: two colour chips on the caption line, the minutes and a
// small MIN beneath. The chips are the most time-critical thing in the row,
// so they take the end furthest from the cuff.
#define CHIP 10
static struct {
  bool show; int w, gap;
  char min[6], at[8]; const char *unit; int later;   // later: 0 both, 1 G leaves later, 2 B leaves later
  int cx, cy, sq, gx, bx, ty, vx, vy, mx, my, ax, ay;
#ifndef PBL_PLATFORM_APLITE
  char was[8]; int wx;   // the departure just gone, its bus maybe still at its bay
#endif
  bool now;
} s_gb;
#ifndef PBL_PLATFORM_APLITE
static bool s_gb_nowas;   // leave the departure just gone out: it would cost a module
#endif

// The departure's clock time, without its A or P: the next half hour needs
// no telling which.
static void gb_clock(char *out, size_t n, int minute) {
  const int h = (minute / 60) % 24, mm = minute % 60;
  snprintf(out, n, "%d:%02d", use_24h() ? h : display_hour(h), mm);
}

static void layout_gb(int c_start, int avail_w, int band_top, int band_bot, int now_min, int now_sec) __attribute__((noinline));
static void layout_gb(int c_start, int avail_w, int band_top, int band_bot, int now_min, int now_sec) {
  s_gb.show = false;
  const int g = transit_next(s_tr.g, now_min), b = transit_next(s_tr.b, now_min);
  if (g < 0 && b < 0) return;
  const int next = g < 0 ? b : b < 0 ? g : g < b ? g : b;
  s_gb.later = (g < 0 || g > next + 1) ? 1 : (b < 0 || b > next + 1) ? 2 : 0;
  s_gb.now = next == 0;
  s_gb.unit = "MIN";
  if (s_gb.now) strcpy(s_gb.min, "NOW");
  else if (next == 1 && s_set.final_seconds) { snprintf(s_gb.min, sizeof(s_gb.min), "%d", 60 - now_sec); s_gb.unit = "SEC"; }   // the last minute, in seconds
  else snprintf(s_gb.min, sizeof(s_gb.min), "%d", next);

  const Metrics m_val = barlow_metrics(measure("88", s_f_mod).h);
  const Metrics m_cap = barlow_metrics(measure("BPM", s_f_cap).h);
  const Metrics m_chip = barlow_metrics(measure("B", s_f_label).h);
  const int label_h = m_cap.cap, lgap = sc(MOD_LABEL_GAP);
  const int row_h = label_h + lgap + m_val.cap;
  if (band_bot - band_top < row_h) return;
  const int y = (band_top + band_bot) / 2 - row_h / 2 + sc(1);
  const int sq = sc(CHIP), sqg = sc(1), chips = 2 * sq + sqg;
  const GFont f_min = s_gb.now ? s_f_label : s_f_mod;
  const int min_w = run_w(s_gb.min, f_min, !s_gb.now, 0);
  // The clock time of that departure, beside the chips or in place of MIN.
  s_gb.at[0] = 0;
  if (!s_gb.now) gb_clock(s_gb.at, sizeof(s_gb.at), now_min + next);
  // A departure due in the last few minutes goes before it: its bus may be
  // the one still at the bay, late, and the two times tell which it is.
#ifndef PBL_PLATFORM_APLITE
  s_gb.was[0] = 0;
  const int pg = transit_prev(s_tr.g, now_min), pb = transit_prev(s_tr.b, now_min);
  const int prev = pg < 0 ? pb : pb < 0 ? pg : pg < pb ? pg : pb;
  // Its minutes alone (:25): the next time beside it says the hour.
  if (prev > 0 && !s_gb.now && !s_gb_nowas) snprintf(s_gb.was, sizeof(s_gb.was), ":%02d", (now_min - prev) % 60);
  const int was_w = s_gb.was[0] ? run_w(s_gb.was, s_f_cap, false, TRACK) + sc(4) : 0;
#else
  const int was_w = 0;
#endif
  const Metrics m_at = barlow_metrics(measure("8", s_f_cap).h);
  const int at_w = s_gb.at[0] ? was_w + run_w(s_gb.at, s_f_cap, false, TRACK) : 0;
  const int unit_w = s_gb.now ? 0 : run_w(s_gb.unit, s_f_cap, false, TRACK);
  const int v_w = s_gb.now ? min_w : min_w + sc(2) + unit_w;
  const int top_w = at_w ? at_w + sc(3) + chips : chips;
  s_gb.w = v_w > top_w ? v_w : top_w;
  s_gb.gap = sc(MOD_GAP);
  if (s_gb.w > avail_w) return;
  const int x = c_start + avail_w - s_gb.w;
  s_gb.sq = sq;
  // The caption line is one unit, the time then the chips, mirrored whole.
  const int rx = mapx(x + s_gb.w - top_w, top_w);
#ifndef PBL_PLATFORM_APLITE
  s_gb.wx = rx;
#endif
  s_gb.ax = rx + was_w;
  s_gb.cx = rx + (top_w - chips);
  s_gb.cy = y + label_h - sq + (sq > label_h ? (sq - label_h) / 2 : 0);
  s_gb.ay = s_gb.cy + (sq - m_chip.cap) / 2 + m_chip.cap - m_at.cap - m_at.bearing;
  s_gb.gx = s_gb.cx + (sq - run_w("G", s_f_label, false, 0)) / 2;
  s_gb.bx = s_gb.cx + sq + sqg + (sq - run_w("B", s_f_label, false, 0)) / 2;
  s_gb.ty = s_gb.cy + (sq - m_chip.cap) / 2 - m_chip.bearing;
  s_gb.vx = mapx(x + s_gb.w - v_w, v_w);
  s_gb.vy = y + label_h + lgap - (s_gb.now ? m_chip.bearing + (m_chip.cap - m_val.cap) : m_val.bearing);
  s_gb.mx = s_gb.vx + min_w + sc(2);
  s_gb.my = y + label_h + lgap + m_val.cap - m_cap.cap - m_cap.bearing;
  s_gb.show = true;
}

// The band's row: the second countdown at the hub, then the modules beside
// it. The departure just gone gives way before a module does: shown only
// where it costs the row nothing.
static void layout_band(const Frame *fr, bool at_hub, int band_top, int band_bot, const struct tm *t) __attribute__((noinline));
static void layout_band(const Frame *fr, bool at_hub, int band_top, int band_bot, const struct tm *t) {
  const int now_min = t->tm_hour * 60 + t->tm_min;
#ifndef PBL_PLATFORM_APLITE
  s_gb_nowas = false;
#endif
  if (at_hub) layout_gb(fr->start, fr->end - fr->start, band_top, band_bot, now_min, t->tm_sec);
  else s_gb.show = false;
  layout_modules(s_f_mod, s_f_cap, fr->start, fr->end - fr->start - (s_gb.show ? s_gb.w + s_gb.gap : 0), band_top, band_bot);
#ifndef PBL_PLATFORM_APLITE
  if (s_gb.show && s_gb.was[0]) {
    const int with = s_md.n;
    s_gb_nowas = true;
    layout_gb(fr->start, fr->end - fr->start, band_top, band_bot, now_min, t->tm_sec);
    layout_modules(s_f_mod, s_f_cap, fr->start, fr->end - fr->start - (s_gb.show ? s_gb.w + s_gb.gap : 0), band_top, band_bot);
    if (s_md.n <= with) {   // it cost nothing: back with it
      s_gb_nowas = false;
      layout_gb(fr->start, fr->end - fr->start, band_top, band_bot, now_min, t->tm_sec);
      layout_modules(s_f_mod, s_f_cap, fr->start, fr->end - fr->start - (s_gb.show ? s_gb.w + s_gb.gap : 0), band_top, band_bot);
    }
  }
#endif
}

static GColor chip_color(int route) {   // 0 G, 1 B
#ifdef PBL_COLOR
  return route ? GColorFromHEX(0x0055AA) : GColorFromHEX(0x00AA55);
#else
  (void)route; return s_ink;
#endif
}
static void paint_gb(void) __attribute__((noinline));
static void paint_gb(void) {
  // The route that leaves later, if they have parted, takes the dim ink.
  graphics_context_set_fill_color(s_ctx, s_gb.later == 1 ? s_dim : chip_color(0));
  graphics_fill_rect(s_ctx, GRect(s_gb.cx, s_gb.cy, s_gb.sq, s_gb.sq), 0, GCornerNone);
  graphics_context_set_fill_color(s_ctx, s_gb.later == 2 ? s_dim : chip_color(1));
  graphics_fill_rect(s_ctx, GRect(s_gb.cx + s_gb.sq + sc(1), s_gb.cy, s_gb.sq, s_gb.sq), 0, GCornerNone);
  graphics_context_set_text_color(s_ctx, PBL_IF_COLOR_ELSE(GColorWhite, s_ground));
  draw_run_s("G", s_f_label, s_gb.gx, s_gb.ty, false, 0);
  draw_run_s("B", s_f_label, s_gb.bx, s_gb.ty, false, 0);
  graphics_context_set_text_color(s_ctx, s_ink);
  if (s_gb.now) {
    draw_run_s(s_gb.min, s_f_label, s_gb.vx, s_gb.vy, false, TRACK);
  } else {
    draw_run_s(s_gb.min, s_f_mod, s_gb.vx, s_gb.vy, true, 0);
    graphics_context_set_text_color(s_ctx, s_dim);
    draw_run_s(s_gb.unit, s_f_cap, s_gb.mx, s_gb.my, false, TRACK);
  }
  if (s_gb.at[0]) {
    graphics_context_set_text_color(s_ctx, s_dim);
    draw_run_s(s_gb.at, s_f_cap, s_gb.ax, s_gb.ay, false, TRACK);
#ifndef PBL_PLATFORM_APLITE
    if (s_gb.was[0]) draw_run_s(s_gb.was, s_f_cap, s_gb.wx, s_gb.ay, false, TRACK);
#endif
  }
}

// ---- the quiet face: away from the hub, or outside its hours, there is
// nothing to count down to, so the block goes and the date grows into the
// room it leaves. Still on the wrist side: it is the part you already know.
#define IDLE_DOW_GAP 6
static struct { char dow[8], date[12]; int dow_x, date_x, dow_y, date_y, top; Metrics m; } s_id;
static void layout_idle(const struct tm *t, const Frame *fr) __attribute__((noinline));
static void layout_idle(const struct tm *t, const Frame *fr) {
  // Hung from the outer edge, the number takes the edge: SEP 22, where the
  // normal face's 22 SEP puts it on the wrist edge it hangs from.
  strftime(s_id.dow, sizeof(s_id.dow), "%a", t);
  strftime(s_id.date, sizeof(s_id.date), "%b %d", t);
  for (char *p = s_id.dow; *p; p++) *p = toupper((int)*p);
  for (char *p = s_id.date; *p; p++) *p = toupper((int)*p);
  s_id.m = barlow_metrics(measure("22 SEP", s_f_bigdate).h);
  s_id.date_y = fr->bot - sc(DATE_PAD_BOT) - s_id.m.cap;
  s_id.dow_y = s_id.date_y - sc(IDLE_DOW_GAP) - s_id.m.cap;
  s_id.top = s_id.dow_y - sc(IDLE_DOW_GAP);   // where the band above ends
  // With no countdown to hold the outer end, the date takes it: the face
  // is read past a sleeve, and the sleeve comes from the wrist side.
  s_id.dow_x = fr->end - run_w(s_id.dow, s_f_bigdate, false, TRACK);
  s_id.date_x = fr->end - run_w(s_id.date, s_f_bigdate, false, TRACK);
}
static void paint_idle(void) __attribute__((noinline));
static void paint_idle(void) {
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run(s_id.dow, s_f_bigdate, s_id.dow_x, s_id.dow_y - s_id.m.bearing, false, TRACK);
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run(s_id.date, s_f_bigdate, s_id.date_x, s_id.date_y - s_id.m.bearing, false, TRACK);
}

// ---- the stop view: on a flick, the nearest stop and what leaves it next.
// It takes the whole of the face below the time for a few seconds: a line
// for the stop, then a row a departure — the route in its own colour, where
// it is going, and when. The rail stays; it is the one thing the design
// never covers.
#define SV_ROW_H 15
#define SV_LINE_GAP 3      // the stop line's air below the time
#define SV_ROWS_GAP 5      // between the stop line and the first row
// A departure comes as minutes past midnight and is written the way the
// watch tells time: 5:13P, or 17:13 on a 24-hour watch.
static int s_now_min;   // set each frame, so a board can tell a due bus
static void sv_clock(char *out, size_t n, const char *tok) {
  const int m = atoi(tok), h = (m / 60) % 24, mm = m % 60;
  // The minute a bus is due, and the minute after, read NOW: a bus that
  // should be here is the one thing a board must not hide.
  if (m <= s_now_min && m >= s_now_min - 1) { strncpy(out, "NOW", n); out[n - 1] = 0; return; }
  // The minute before reads 1 MIN, as the countdown says it: the bus isn't
  // here yet, and a clock time a minute off takes working out.
  if (m == s_now_min + 1) { strncpy(out, "1 MIN", n); out[n - 1] = 0; return; }
  if (use_24h()) snprintf(out, n, "%d:%02d", h, mm);
  else snprintf(out, n, "%d:%02d%c", display_hour(h), mm, h < 12 ? 'A' : 'P');
}
// NOW, or 1 MIN: words, not a clock time.
static bool sv_is_now(const char *t) { return t[0] == 'N' || t[1] == ' '; }
// NOW is set in the label font, which has the letters; a time in the board's.
static GFont sv_font(const char *t) { return sv_is_now(t) ? s_f_label : s_f_board; }

// On the light theme grey text at this size loses its strokes, so the stop
// line is ink with a hairline of light grey beneath it; on one-bit watches
// the hairline is dotted. On the dark theme light grey reads as it is.
static void draw_stop_rule(int x0, int x1, int y) {
  if (x1 <= x0) return;
#ifdef PBL_COLOR
  graphics_context_set_fill_color(s_ctx, GColorLightGray);
  graphics_fill_rect(s_ctx, GRect(x0, y, x1 - x0, 1), 0, GCornerNone);
#else
  graphics_context_set_fill_color(s_ctx, s_ink);
  for (int x = x0; x < x1; x += 2) graphics_fill_rect(s_ctx, GRect(x, y, 1, 1), 0, GCornerNone);
#endif
}
static bool light_theme(void) { return gcolor_equal(s_ground, GColorWhite); }

static struct {
  int head_y, dist_w, stop_x, note_x, note_y, n, rule_y, date_x, date_y;
  char date[16];
  Metrics m_date;
  char dist[10], note[20], stop[24];
  struct { int y, badge_w, badge_h, glyph_dx, glyph_y, text_y, t1_dx, t2_dx, t2_y, row_x, row_w; bool t2_day; char t1[8], t2[10]; } r[SV_ROWS];
  Metrics m_lab, m_val, m_bad;
} s_svl;

static void format_dist(char *out, size_t n, int metres) {
  if (s_set.imperial) {
    const int ft = metres * 328 / 100;
    if (ft < 1000) snprintf(out, n, "%d FT", ft);
    else snprintf(out, n, "%d.%d MI", ft / 5280, (ft % 5280) * 10 / 5280);
  } else {
    if (metres < 1000) snprintf(out, n, "%d M", metres);
    else snprintf(out, n, "%d.%d KM", metres / 1000, (metres % 1000) / 100);
  }
}

// A stop's name cut down to the room there is: a word at a time from the
// end, so it never stops mid-word, and a glyph at a time only when one word
// alone is too wide. Inlined, so the text renderer is no deeper for it.
static inline __attribute__((always_inline)) void fit_name(char *s, GFont f, int room) {
  while (s[0] && run_w(s, f, false, TRACK) > room) {
    char *e = strrchr(s, ' ');
    if (!e) {
      e = s + strlen(s) - 1;
      while (e > s && ((unsigned char)*e & 0xC0) == 0x80) e--;
    }
    *e = 0;
    while (e > s && e[-1] == ' ') *--e = 0;
  }
}

// The board, as the design draws it: hung from the outer edge, a line for
// the stop, then a row a route — its badge and the next clock times, two
// where they fit, or the next time and its day when today's are done. It
// takes the whole of the room below the time and sits centred in it.
#define SV_BADGE_GAP 5     // badge to the first time
#define SV_TIME_GAP  7     // between the times
// The badges are the chips' size, with the chips' glyph: one badge on the face.
// The head and foot every answer shares: a small date at the foot, then the
// stop line, its name hung from the outer edge and at the wrist end the
// distance — or, at the hub after the day's last bus, the day. Inlined into
// each layout, so no painter gets deeper for sharing it.
static inline __attribute__((always_inline)) void layout_board_head(const struct tm *t, const Frame *fr) {
  // The flick may have been for the date: a small one keeps the foot.
  strftime(s_svl.date, sizeof(s_svl.date), "%a %d %b", t);
  for (char *c = s_svl.date; *c; c++) *c = toupper((int)*c);
  s_svl.m_date = barlow_metrics(measure(s_svl.date, s_f_date).h);
  s_svl.date_y = fr->bot - sc(DATE_PAD_BOT) - s_svl.m_date.cap;
  s_svl.date_x = fr->start;
  // The stop line and the badges in the caption font, the times in the
  // board font: the board is a small thing under a full-size time.
  s_svl.m_lab = s_m_cap;
  s_svl.m_val = barlow_metrics(measure("8", s_f_board).h);
  s_svl.m_bad = barlow_metrics(measure("8", s_f_label).h);
  s_svl.dist_w = 0;
#ifdef HAS_WAVE
  const char *word = s_sv.wave ? strchr(s_wave[0].when, ' ') : NULL;
#else
  const char *word = NULL;
#endif
  if (word && word[1]) {
    strncpy(s_svl.dist, word + 1, sizeof(s_svl.dist) - 1); s_svl.dist[sizeof(s_svl.dist) - 1] = 0;
    s_svl.dist_w = run_w(s_svl.dist, s_f_cap, false, TRACK);
  } else if (!s_sv.wave && s_sv.dist > 60) {
    format_dist(s_svl.dist, sizeof(s_svl.dist), s_sv.dist);
    s_svl.dist_w = run_w(s_svl.dist, s_f_cap, false, TRACK);
  }
  // The stop's name gives way to the distance, a word at a time.
  strncpy(s_svl.stop, s_sv.stop, sizeof(s_svl.stop) - 1); s_svl.stop[sizeof(s_svl.stop) - 1] = 0;
  fit_name(s_svl.stop, s_f_cap, fr->end - fr->start - (s_svl.dist_w ? s_svl.dist_w + sc(6) : 0));
  s_svl.stop_x = fr->end - run_w(s_svl.stop, s_f_cap, false, TRACK);
  s_svl.note[0] = 0;
}

static void layout_stopview(const struct tm *t, const Frame *fr, int band_top) __attribute__((noinline));
static void layout_stopview(const struct tm *t, const Frame *fr, int band_top) {
  layout_board_head(t, fr);
  const int band_bot = s_svl.date_y - sc(4);
  const GFont f_line = s_f_cap, f_badge = s_f_label;
  if (s_sv.n == 0) strncpy(s_svl.note, "NO SERVICE", sizeof(s_svl.note));   // a stop the data has nothing for
  s_svl.note_x = fr->end - run_w(s_svl.note, f_line, false, TRACK);

  const int badge_h = sc(CHIP);
  const int line_h = s_svl.m_lab.cap + sc(SV_ROWS_GAP);
  int rows = s_sv.n < SV_ROWS ? s_sv.n : SV_ROWS;
  if (s_sv.n == 0) rows = 1;   // the note takes a row's place
  while (rows > 0 && sc(SV_LINE_GAP) + line_h + (rows - 1) * sc(SV_ROW_H) + badge_h > band_bot - band_top - sc(2)) rows--;
  const int used = sc(SV_LINE_GAP) + line_h + (rows ? (rows - 1) * sc(SV_ROW_H) + badge_h : 0);
  const int top = band_top + (band_bot - band_top - used) / 2;
  s_svl.head_y = top + sc(SV_LINE_GAP) - s_svl.m_lab.bearing;
  s_svl.rule_y = top + sc(SV_LINE_GAP) + s_svl.m_lab.cap + sc(2);
  int y = top + sc(SV_LINE_GAP) + line_h;
  s_svl.note_y = y + sc(2) - s_svl.m_lab.bearing;
  s_svl.n = 0;
  if (s_sv.n == 0) return;
  // The board reads in columns, as the design sets it: each time column
  // starts at one x down the rows, and the badges end at one x before them.
  int W1 = 0, W2 = 0;
  for (int i = 0; i < rows; i++) {
    const SvRow *row = &s_sv.row[i];
    const int pad = sc(2);
    s_svl.r[i].y = y;
    // A one-glyph route gets a square badge; longer ones grow with the glyphs.
    // The run's width carries a trailing bearing the ink does not, so the
    // ink centres on width minus one; a wide badge is sized even for it.
    const int gw = run_w(row->route, f_badge, false, 0);
    s_svl.r[i].badge_w = gw + 2 * pad < badge_h ? badge_h : gw + 2 * pad + 1;
    s_svl.r[i].badge_h = badge_h;
    s_svl.r[i].glyph_dx = (s_svl.r[i].badge_w - gw + 1) / 2;
    s_svl.r[i].glyph_y = y + (badge_h - s_svl.m_bad.cap) / 2 - s_svl.m_bad.bearing;
    // The times sit on the badge's baseline: cap bottoms level.
    s_svl.r[i].text_y = y + (badge_h + s_svl.m_bad.cap) / 2 - s_svl.m_val.cap - s_svl.m_val.bearing;
    // The row comes as two tokens, a space between: minutes past midnight,
    // then either a second time or a word — a day, TOMORROW or MON, or the
    // direction where the route runs both ways from here.
    const char *sp = strchr(row->when, ' ');
    sv_clock(s_svl.r[i].t1, sizeof(s_svl.r[i].t1), row->when);
    s_svl.r[i].t2[0] = 0;
    s_svl.r[i].t2_day = false;
    if (sp && isdigit((int)sp[1])) sv_clock(s_svl.r[i].t2, sizeof(s_svl.r[i].t2), sp + 1);
    else if (sp && sp[1]) {
      strncpy(s_svl.r[i].t2, sp + 1, sizeof(s_svl.r[i].t2) - 1); s_svl.r[i].t2[sizeof(s_svl.r[i].t2) - 1] = 0;
      s_svl.r[i].t2_day = true;
    }
    // Proportional, as the design sets them: the colon takes its own width.
    // A word in the second column takes the caption font on the baseline.
    s_svl.r[i].t2_y = s_svl.r[i].t2_day ? s_svl.r[i].text_y + s_svl.m_val.bearing + s_svl.m_val.cap - s_svl.m_lab.cap - s_svl.m_lab.bearing : s_svl.r[i].text_y;
    const int w1 = run_w(s_svl.r[i].t1, sv_font(s_svl.r[i].t1), false, 0);
    const int w2 = s_svl.r[i].t2[0] ? run_w(s_svl.r[i].t2, s_svl.r[i].t2_day ? f_line : sv_font(s_svl.r[i].t2), false, s_svl.r[i].t2_day ? TRACK : 0) : 0;
    if (w1 > W1) W1 = w1;
    if (w2 > W2) W2 = w2;
    s_svl.n = i + 1;
    y += sc(SV_ROW_H);
  }
  const int x2 = fr->end - W2;                              // the second column
  const int x1 = W2 ? x2 - sc(SV_TIME_GAP) - W1 : fr->end - W1;   // the first
  const int bx = x1 - sc(SV_BADGE_GAP);                     // where the badges end
  for (int i = 0; i < s_svl.n; i++) {
    s_svl.r[i].row_x = bx - s_svl.r[i].badge_w;
    s_svl.r[i].row_w = fr->end - s_svl.r[i].row_x;
    s_svl.r[i].t1_dx = x1 - s_svl.r[i].row_x;
    s_svl.r[i].t2_dx = x2 - s_svl.r[i].row_x;
  }
}

static inline __attribute__((always_inline)) void paint_board_head(int fr_start, bool date) {
  graphics_context_set_text_color(s_ctx, light_theme() ? s_ink : s_dim);
  draw_run(s_svl.stop, s_f_cap, s_svl.stop_x, s_svl.head_y, false, TRACK);
  if (light_theme()) {
    const int w = run_w(s_svl.stop, s_f_cap, false, TRACK);
    draw_stop_rule(mapx(s_svl.stop_x, w), mapx(s_svl.stop_x, w) + w, s_svl.rule_y);
  }
  graphics_context_set_text_color(s_ctx, s_ink);
  if (date) draw_run(s_svl.date, s_f_date, s_svl.date_x, s_svl.date_y - s_svl.m_date.bearing, false, TRACK);
  if (s_svl.dist_w) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.dist, s_f_cap, fr_start, s_svl.head_y, false, TRACK);
  }
}

// A live time's mark: a small broadcast arc in the accent, the sign transit
// apps give a predicted time, beside the time or above a hub badge. A time
// without it is the timetable's. Fills only, so no deeper than its caller.
static const char *const LIVE_ARC[5] = { "XXX..", "...X.", "XX..X", "..X.X", "X.X.X" };
#define LIVE_W 5
static void paint_live_mark(int x, int y) __attribute__((noinline));
static void paint_live_mark(int x, int y) {
  graphics_context_set_fill_color(s_ctx, accent());
  for (int j = 0; j < 5; j++)
    for (int i = 0; i < 5; i++)
      if (LIVE_ARC[j][i] == 'X') graphics_fill_rect(s_ctx, GRect(x + i, y + j, 1, 1), 0, GCornerNone);
}

static void paint_stopview(int fr_start) __attribute__((noinline));
static void paint_stopview(int fr_start) {
  paint_board_head(fr_start, true);
  if (s_svl.note[0]) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.note, s_f_cap, s_svl.note_x, s_svl.note_y, false, TRACK);
  }
  for (int i = 0; i < s_svl.n; i++) {
    const SvRow *row = &s_sv.row[i];
    // The row is one unit, placed once by its logical x, then read rightwards.
    const int rx = mapx(s_svl.r[i].row_x, s_svl.r[i].row_w);
    const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(row->color), s_ink);
    graphics_context_set_fill_color(s_ctx, fill);
    graphics_fill_rect(s_ctx, GRect(rx, s_svl.r[i].y, s_svl.r[i].badge_w, s_svl.r[i].badge_h), sc(2), GCornersAll);
    graphics_context_set_text_color(s_ctx, on_fill(fill));
    draw_run_s(row->route, s_f_label, rx + s_svl.r[i].glyph_dx, s_svl.r[i].glyph_y, false, 0);
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run_s(s_svl.r[i].t1, sv_font(s_svl.r[i].t1), rx + s_svl.r[i].t1_dx, s_svl.r[i].text_y, false, 0);
    if (row->live)
      paint_live_mark(rx + s_svl.r[i].t1_dx + run_w(s_svl.r[i].t1, sv_font(s_svl.r[i].t1), false, 0) + 1,
                      s_svl.r[i].text_y + s_svl.m_val.bearing - 1);
    if (s_svl.r[i].t2_day) {
      graphics_context_set_text_color(s_ctx, s_dim);
      draw_run_s(s_svl.r[i].t2, s_f_cap, rx + s_svl.r[i].t2_dx, s_svl.r[i].t2_y, false, TRACK);
    } else if (s_svl.r[i].t2[0]) {
      // A second time can be NOW too, a bus hard on the heels of one just
      // gone: it takes the letters' font as the first column's does.
      draw_run_s(s_svl.r[i].t2, sv_font(s_svl.r[i].t2), rx + s_svl.r[i].t2_dx, s_svl.r[i].text_y, false, 0);
    }
  }
}

#ifdef HAS_WAVE
// ---- the hub's answer: departures by time. At the transit centre a dozen
// routes leave on the same minute, so a row a route would show three of them
// and repeat the countdown. Instead a line a departure minute, hung from the
// outer edge: the time in the board's column there, and every route leaving
// then as a badge beside it, in route order, wrapping under the badges when
// a wave is wider than the line. Each line mirrors as one unit. The small
// date gives its room to a fourth line: the :30 wave wraps to two, and the
// bus after it, or a late one, still shows.
#define WV_GAP 2     // between badges
#define WV_LINES 4
static struct {
  int n, t_x;
  struct { int y, x, w, glyph_y, text_y, wave, first, count; char t[8]; } l[WV_LINES];
  int16_t bx[WV_LINES][WV_MAX];   // logical x of each badge on its line
  uint8_t bw[WV_LINES][WV_MAX], gdx[WV_LINES][WV_MAX];
} s_wl;

static void layout_wave(const struct tm *t, const Frame *fr, int band_top) __attribute__((noinline));
static void layout_wave(const struct tm *t, const Frame *fr, int band_top) {
  layout_board_head(t, fr);
  const int band_bot = fr->bot - sc(DATE_PAD_BOT);
  const int badge_h = sc(CHIP), pad = sc(2), gap = sc(WV_GAP);
  const int line_h = s_svl.m_lab.cap + sc(SV_ROWS_GAP);
  int rows = WV_LINES;
  while (rows > 0 && sc(SV_LINE_GAP) + line_h + (rows - 1) * sc(SV_ROW_H) + badge_h > band_bot - band_top - sc(2)) rows--;
  // One time column for every line, as the board's.
  char tb[8];
  int W = 0;
  for (int i = 0; i < s_sv.n; i++) {
    if (!isdigit((int)s_wave[i].when[0])) continue;
    sv_clock(tb, sizeof(tb), s_wave[i].when);
    const int w = run_w(tb, sv_font(tb), false, 0);
    if (w > W) W = w;
  }
  s_wl.t_x = fr->end - W;
  s_wl.n = 0;
  for (int i = 0; i < s_sv.n && s_wl.n < rows; i++) {
    const WaveLine *w = &s_wave[i];
    // A line with no time has the time's column too.
    const bool timed = isdigit((int)w->when[0]);
    const int bend = timed ? s_wl.t_x - sc(SV_BADGE_GAP) : fr->end, room = bend - fr->start;
    for (int j = 0; j < w->n && s_wl.n < rows;) {
      const int k = s_wl.n;
      s_wl.l[k].wave = i; s_wl.l[k].first = j; s_wl.l[k].count = 0; s_wl.l[k].t[0] = 0;
      if (j == 0 && timed) sv_clock(s_wl.l[k].t, sizeof(s_wl.l[k].t), w->when);
      int used = 0;
      for (int c = j; c < w->n; c++) {
        // A badge as the board sizes it: square for one glyph, growing past.
        const int gw = run_w(w->lab[c], s_f_label, false, 0);
        const int bw = gw + 2 * pad < badge_h ? badge_h : gw + 2 * pad + 1;
        const int need = used + (used ? gap : 0) + bw;
        if (need > room && s_wl.l[k].count) break;
        s_wl.bw[k][s_wl.l[k].count] = bw;
        s_wl.gdx[k][s_wl.l[k].count] = (bw - gw + 1) / 2;
        s_wl.bx[k][s_wl.l[k].count] = used + (used ? gap : 0);   // from the line's start, set below
        used = need;
        s_wl.l[k].count++;
      }
      // The badges end at one x before the times, the line's start is its first badge.
      s_wl.l[k].x = bend - used;
      s_wl.l[k].w = fr->end - s_wl.l[k].x;
      for (int c = 0; c < s_wl.l[k].count; c++) s_wl.bx[k][c] += s_wl.l[k].x;
      j += s_wl.l[k].count;
      s_wl.n++;
    }
  }
  const int used = sc(SV_LINE_GAP) + line_h + (s_wl.n ? (s_wl.n - 1) * sc(SV_ROW_H) + badge_h : 0);
  const int top = band_top + (band_bot - band_top - used) / 2;
  s_svl.head_y = top + sc(SV_LINE_GAP) - s_svl.m_lab.bearing;
  s_svl.rule_y = top + sc(SV_LINE_GAP) + s_svl.m_lab.cap + sc(2);
  int y = top + sc(SV_LINE_GAP) + line_h;
  for (int k = 0; k < s_wl.n; k++) {
    s_wl.l[k].y = y;
    s_wl.l[k].glyph_y = y + (badge_h - s_svl.m_bad.cap) / 2 - s_svl.m_bad.bearing;
    s_wl.l[k].text_y = y + (badge_h + s_svl.m_bad.cap) / 2 - s_svl.m_val.cap - s_svl.m_val.bearing;
    y += sc(SV_ROW_H);
  }
}

static void paint_wave(int fr_start) __attribute__((noinline));
static void paint_wave(int fr_start) {
  paint_board_head(fr_start, false);
  const int badge_h = sc(CHIP);
  for (int k = 0; k < s_wl.n; k++) {
    const WaveLine *w = &s_wave[s_wl.l[k].wave];
    const int rx = mapx(s_wl.l[k].x, s_wl.l[k].w);   // the line, placed once, read rightwards
    for (int c = 0; c < s_wl.l[k].count; c++) {
      const int b = s_wl.l[k].first + c;
      const bool live = (w->live >> b) & 1;
      const int x = rx + (s_wl.bx[k][c] - s_wl.l[k].x);
      const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(w->col[b]), s_ink);
      if ((w->away >> b) & 1) {
        // Its bus isn't in at its bay yet: the badge hollow, outlined in its
        // colour, the route in ink, since a dark route colour alone would
        // vanish on the dark ground.
        graphics_context_set_stroke_color(s_ctx, fill);
        graphics_draw_round_rect(s_ctx, GRect(x, s_wl.l[k].y, s_wl.bw[k][c], badge_h), sc(2));
        graphics_context_set_text_color(s_ctx, s_ink);
      } else {
        graphics_context_set_fill_color(s_ctx, fill);
        graphics_fill_rect(s_ctx, GRect(x, s_wl.l[k].y, s_wl.bw[k][c], badge_h), sc(2), GCornersAll);
        graphics_context_set_text_color(s_ctx, on_fill(fill));
      }
      draw_run_s(w->lab[b], s_f_label, x + s_wl.gdx[k][c], s_wl.l[k].glyph_y, false, 0);
      // The mark sits in the air above the badge's outer corner.
      if (live) paint_live_mark(x + s_wl.bw[k][c] - LIVE_W, s_wl.l[k].y - 5);
    }
    if (s_wl.l[k].t[0]) {
      graphics_context_set_text_color(s_ctx, s_ink);
      draw_run_s(s_wl.l[k].t, sv_font(s_wl.l[k].t), rx + (s_wl.t_x - s_wl.l[k].x), s_wl.l[k].text_y, false, 0);
    }
  }
}

// ---- the flick's full-screen answers. The time steps down to a line at
// the top, its seconds beside it, and the face below is the answer's: at a
// stop a row a route with its next two times (2), or with more routes than
// rows every route's next time in two columns (3); away from a stop, the
// stops near, nearest first, each with its two soonest routes (4). Lines
// mirror as units, as the board's do.
#define TK_CELLS 12
#define TK_STOPS 4
typedef struct { char route[6]; int16_t t1, t2; char word[10]; uint32_t col; bool live; int8_t stop; } TkCell;
typedef struct { char name[24], dist[10]; int16_t y, name_x, dist_x, rule_y, rule_x0; } TkLine;
typedef struct { char t1[8], t2[10]; bool word; int16_t bx, by, bw, gx, gy, tx, ty, lx, ly, x2, y2; } TkPlace;
static struct {
  uint8_t n, ns;
  TkCell c[TK_CELLS];
  char name[TK_STOPS][24];
  int16_t dist[TK_STOPS];
  // laid out
  int head_y, time_x, secs_x, label_x, lines, nl, rule_w;
  char time[8], secs[4], label[16];
  TkLine line[TK_STOPS];
  TkPlace at[TK_CELLS];
} s_tk;

// The phone's lines: S|metres|name for a stop, R|route|time|second time or
// word|colour|live for a route, a newline between.
static GColor secs_color(void);
static void take_list(const char *p) __attribute__((noinline));
static void take_list(const char *p) {
  s_tk.n = s_tk.ns = 0;
  int stop = -1;
  while (*p) {
    const char *e = strchr(p, '\n');
    const int len = e ? e - p : (int)strlen(p);
    char line[48], *f[6] = { 0 };
    const int k = len < 47 ? len : 47;
    memcpy(line, p, k); line[k] = 0;
    int nf = 0;
    for (char *q = line; nf < 6; ) { f[nf++] = q; char *bar = strchr(q, '|'); if (!bar) break; *bar = 0; q = bar + 1; }
    if (line[0] == 'S' && nf >= 3 && s_tk.ns < TK_STOPS) {
      s_tk.dist[s_tk.ns] = (int16_t)atoi(f[1]);
      strncpy(s_tk.name[s_tk.ns], f[2], sizeof(s_tk.name[0]) - 1); s_tk.name[s_tk.ns][sizeof(s_tk.name[0]) - 1] = 0;
      stop = s_tk.ns++;
    } else if (line[0] == 'R' && nf >= 6 && s_tk.n < TK_CELLS) {
      TkCell *c = &s_tk.c[s_tk.n++];
      strncpy(c->route, f[1], sizeof(c->route) - 1); c->route[sizeof(c->route) - 1] = 0;
      c->t1 = (int16_t)atoi(f[2]);
      c->t2 = isdigit((int)f[3][0]) ? (int16_t)atoi(f[3]) : -1;
      c->word[0] = 0;
      if (c->t2 < 0) { strncpy(c->word, f[3], sizeof(c->word) - 1); c->word[sizeof(c->word) - 1] = 0; }
      // The colour's hex by hand: the library's strtol faults on basalt.
      uint32_t col = 0;
      for (const char *h = f[4]; *h; h++) col = col * 16 + (uint32_t)(isdigit((int)*h) ? *h - '0' : (*h | 32) - 'a' + 10);
      c->col = col & 0xFFFFFF;
      c->live = f[5][0] == '1';
      c->stop = (int8_t)stop;
    }
    p = e ? e + 1 : p + len;
  }
}

static void layout_take(const struct tm *t, const Frame *fr) __attribute__((noinline));
static void layout_take(const struct tm *t, const Frame *fr) {
  // The time, small, on the top line at the wrist end, its seconds beside
  // it; at the outer end the day, or NEARBY.
  if (use_24h()) snprintf(s_tk.time, sizeof(s_tk.time), "%d:%02d", t->tm_hour, t->tm_min);
  else snprintf(s_tk.time, sizeof(s_tk.time), "%d:%02d", display_hour(t->tm_hour), t->tm_min);
  snprintf(s_tk.secs, sizeof(s_tk.secs), "%02d", t->tm_sec);
  if (s_sv.take == 4) strncpy(s_tk.label, "NEARBY", sizeof(s_tk.label));
  else { strftime(s_tk.label, sizeof(s_tk.label), "%a %d %b", t); for (char *c = s_tk.label; *c; c++) *c = toupper((int)*c); }
  const Metrics m_t = barlow_metrics(measure("8", s_f_mod).h);
  const Metrics m_val = barlow_metrics(measure("8", s_f_board).h), m_bad = barlow_metrics(measure("8", s_f_label).h);
  s_tk.head_y = fr->top + sc(2) - m_t.bearing;
  const int tw0 = run_w(s_tk.time, s_f_mod, false, 0);
  s_tk.time_x = mapx(fr->start, tw0);
  s_tk.secs_x = mapx(fr->start + tw0 + sc(3), run_w(s_tk.secs, s_f_cap, false, TRACK));
  s_tk.label_x = mapx(fr->end - run_w(s_tk.label, s_f_cap, false, TRACK), run_w(s_tk.label, s_f_cap, false, TRACK));
  const int badge_h = sc(CHIP), row_h = sc(SV_ROW_H), line_h = s_m_cap.cap + sc(SV_ROWS_GAP), pad = sc(2), w = fr->end - fr->start;
  int y = fr->top + sc(2) + m_t.cap + sc(8);
  const int colw = w / 2;
  s_tk.lines = 0; s_tk.nl = 0;
  int stop = -2, prev_y = -1, prev_col = 1;
  for (int i = 0; i < s_tk.n; i++) {
    const TkCell *c = &s_tk.c[i];
    // A stop's line before its first route (the nearby list); at a stop the
    // board's own name line, once, at the top.
    if (c->stop != stop) {
      stop = c->stop;
      if (y + line_h + badge_h > fr->bot || s_tk.nl >= TK_STOPS) break;
      const char *name = stop >= 0 ? s_tk.name[stop] : s_sv.stop;
      TkLine *L = &s_tk.line[s_tk.nl++];
      strncpy(L->name, name, sizeof(L->name) - 1); L->name[sizeof(L->name) - 1] = 0;
      L->dist[0] = 0;
      if (stop >= 0) format_dist(L->dist, sizeof(L->dist), s_tk.dist[stop]);
      else if (s_sv.dist > 60) format_dist(L->dist, sizeof(L->dist), s_sv.dist);
      const int dw = L->dist[0] ? run_w(L->dist, s_f_cap, false, TRACK) : 0;
      fit_name(L->name, s_f_cap, w - (dw ? dw + sc(6) : 0));
      L->y = y - s_m_cap.bearing;
      L->name_x = mapx(fr->start, run_w(L->name, s_f_cap, false, TRACK));
      L->dist_x = mapx(fr->end - dw, dw);
      L->rule_y = y + s_m_cap.cap + sc(2);
      L->rule_x0 = mapx(fr->start, w);
      y += line_h; prev_col = 1;
    }
    // Two columns, a stop's pair or the grid's, a line between each pair.
    const int col = s_sv.take != 2 && prev_col == 0 && prev_y >= 0 ? 1 : 0;
    if (!col) { if (y + badge_h > fr->bot) break; prev_y = y; y += row_h; }
    prev_col = col;
    TkPlace *P = &s_tk.at[i];
    char tok[8];
    snprintf(tok, sizeof(tok), "%d", c->t1);
    sv_clock(P->t1, sizeof(P->t1), tok);
    P->t2[0] = 0; P->word = false;
    if (s_sv.take == 2 && c->t2 >= 0) { snprintf(tok, sizeof(tok), "%d", c->t2); sv_clock(P->t2, sizeof(P->t2), tok); }
    else if (c->word[0]) { strncpy(P->t2, c->word, sizeof(P->t2) - 1); P->t2[sizeof(P->t2) - 1] = 0; P->word = true; }
    const int gw = run_w(c->route, s_f_label, false, 0);
    const int bw = gw + 2 * pad < badge_h ? badge_h : gw + 2 * pad + 1;
    const int tw = run_w(P->t1, sv_font(P->t1), false, 0);
    const int cw = bw + sc(SV_BADGE_GAP) + tw + (c->live ? LIVE_W + 1 : 0);
    const int lx = fr->start + col * colw, rx = mapx(lx, cw);
    P->by = prev_y; P->bx = rx; P->bw = bw;
    P->gx = rx + (bw - gw + 1) / 2; P->gy = prev_y + (badge_h - m_bad.cap) / 2 - m_bad.bearing;
    P->tx = rx + bw + sc(SV_BADGE_GAP); P->ty = prev_y + (badge_h + m_bad.cap) / 2 - m_val.cap - m_val.bearing;
    P->lx = P->tx + tw + 1; P->ly = P->ty + m_val.bearing - 1;
    if (P->t2[0]) {
      // The second column: the next time but one, or a word (a day, a way),
      // in two columns a word only where no route sits beside it.
      const bool beside = s_sv.take != 2 && i + 1 < s_tk.n && s_tk.c[i + 1].stop == c->stop && col == 0;
      if (beside) P->t2[0] = 0;
      else {
        const int w2 = run_w(P->t2, P->word ? s_f_cap : sv_font(P->t2), false, P->word ? TRACK : 0);
        P->x2 = mapx(s_sv.take == 2 ? fr->end - w2 : lx + cw + sc(4), w2);
        P->y2 = P->word ? P->ty + m_val.bearing + m_val.cap - s_m_cap.cap - s_m_cap.bearing : P->ty;
      }
    }
    s_tk.lines = i + 1;
  }
  s_tk.rule_w = w;
}

// Every place worked out beforehand: the painter reads them and draws.
static void paint_take(void) __attribute__((noinline));
static void paint_take(void) {
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_tk.time, s_f_mod, s_tk.time_x, s_tk.head_y, false, 0);
  graphics_context_set_text_color(s_ctx, secs_color());
  draw_run_s(s_tk.secs, s_f_cap, s_tk.secs_x, s_tk.head_y, false, TRACK);
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run_s(s_tk.label, s_f_cap, s_tk.label_x, s_tk.head_y, false, TRACK);
  for (int j = 0; j < s_tk.nl; j++) {
    const TkLine *L = &s_tk.line[j];
    graphics_context_set_text_color(s_ctx, light_theme() ? s_ink : s_dim);
    draw_run_s(L->name, s_f_cap, L->name_x, L->y, false, TRACK);
    if (L->dist[0]) { graphics_context_set_text_color(s_ctx, s_dim); draw_run_s(L->dist, s_f_cap, L->dist_x, L->y, false, TRACK); }
    draw_stop_rule(L->rule_x0, L->rule_x0 + s_tk.rule_w, L->rule_y);
  }
  for (int i = 0; i < s_tk.lines; i++) {
    const TkPlace *P = &s_tk.at[i];
    const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(s_tk.c[i].col), s_ink);
    graphics_context_set_fill_color(s_ctx, fill);
    graphics_fill_rect(s_ctx, GRect(P->bx, P->by, P->bw, sc(CHIP)), sc(2), GCornersAll);
    graphics_context_set_text_color(s_ctx, on_fill(fill));
    draw_run_s(s_tk.c[i].route, s_f_label, P->gx, P->gy, false, 0);
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run_s(P->t1, sv_font(P->t1), P->tx, P->ty, false, 0);
    if (s_tk.c[i].live) paint_live_mark(P->lx, P->ly);
    if (P->t2[0]) {
      graphics_context_set_text_color(s_ctx, P->word ? s_dim : s_ink);
      draw_run_s(P->t2, P->word ? s_f_cap : sv_font(P->t2), P->x2, P->y2, false, P->word ? TRACK : 0);
    }
  }
}
#endif

// ---- the side block: one row never takes the face. The answer sits in the
// band beside the modules, three short lines hung from one edge: the stop,
// the badge and its time, then the distance, or the row's second column
// when stood at the stop. Date and modules stay; twelve seconds later the
// block is gone. Its lines start together at the start of the space left
// over, `left` (where the modules end on that side, less the gap), so they
// read as a list; `right` bounds it, and the block declines if it can't fit.
#define SB_GAP 3
#ifdef HAS_YARD
#define SB_NOROW s_sb.norow
#else
#define SB_NOROW false
#endif
static struct {
  bool show, q_day;
#ifdef HAS_YARD
  bool norow;
#endif
  int stop_x, stop_y, rule_y, row_x, row_w, badge_w, badge_h, glyph_dx, glyph_y, t_dx, t_y, q_x, q_y;
  char stop[24], t1[8], q[10];
  Metrics m_lab, m_val, m_bad;
} s_sb;

static void layout_sideblock(const Frame *fr, int band_top, int band_bot, int left, int right) __attribute__((noinline));
static void layout_sideblock(const Frame *fr, int band_top, int band_bot, int left, int right) {
  s_sb.show = false;
  // At the yard in its hour the block has no row: its line, what's pulling
  // in, and under it how far the nearest is.
#ifdef HAS_YARD
  s_sb.norow = AT_YARD() && s_sv.n == 0;
#endif
  if ((s_sv.n != 1 && !SB_NOROW) || s_sv.wave || s_sv.take) return;
  const SvRow *row = &s_sv.row[0];
  s_sb.m_lab = s_m_cap;
  s_sb.m_val = barlow_metrics(measure("8", s_f_board).h);
  s_sb.m_bad = barlow_metrics(measure("8", s_f_label).h);
  const int room = right - left - sc(8);
  // The stop's name, a word at a time down to the room there is.
  strncpy(s_sb.stop, s_sv.stop, sizeof(s_sb.stop) - 1); s_sb.stop[sizeof(s_sb.stop) - 1] = 0;
  fit_name(s_sb.stop, s_f_cap, room);
  // The row: badge and the next time, as on the board.
  const char *sp = SB_NOROW ? NULL : strchr(row->when, ' ');
  sv_clock(s_sb.t1, sizeof(s_sb.t1), row->when);
  const int pad = sc(2), badge_h = sc(CHIP);
  const int gw = run_w(row->route, s_f_label, false, 0);
  s_sb.badge_w = gw + 2 * pad < badge_h ? badge_h : gw + 2 * pad + 1;
  s_sb.badge_h = badge_h;
  s_sb.glyph_dx = (s_sb.badge_w - gw + 1) / 2;
  s_sb.t_dx = s_sb.badge_w + sc(SV_BADGE_GAP);
  s_sb.row_w = SB_NOROW ? 0 : s_sb.t_dx + run_w(s_sb.t1, sv_font(s_sb.t1), false, 0) + (row->live ? LIVE_W + 1 : 0);
  // The qualifier: how far, or, stood at the stop, the row's second column.
  s_sb.q[0] = 0; s_sb.q_day = false;
  if (s_sv.dist > 60) { format_dist(s_sb.q, sizeof(s_sb.q), s_sv.dist); s_sb.q_day = true; }
  else if (sp && isdigit((int)sp[1])) sv_clock(s_sb.q, sizeof(s_sb.q), sp + 1);
  else if (sp && sp[1]) { strncpy(s_sb.q, sp + 1, sizeof(s_sb.q) - 1); s_sb.q[sizeof(s_sb.q) - 1] = 0; s_sb.q_day = true; }
  const int q_w = s_sb.q[0] ? run_w(s_sb.q, s_sb.q_day ? s_f_cap : sv_font(s_sb.q), false, s_sb.q_day ? TRACK : 0) : 0;
  if (s_sb.row_w > room || q_w > room) return;   // no room beside the modules: the board it is
  // Three lines, centred in the band.
  const int row_h = SB_NOROW ? 0 : badge_h + sc(SB_GAP);
  const int h = s_sb.m_lab.cap + sc(SB_GAP) + row_h + (s_sb.q[0] ? (s_sb.q_day ? s_sb.m_lab.cap : s_sb.m_val.cap) : 0);
  if (h > band_bot - band_top) return;
  const int top = band_top + (band_bot - band_top - h) / 2;
  const int x0 = left + sc(8);
  s_sb.stop_x = x0;
  s_sb.stop_y = top - s_sb.m_lab.bearing;
  s_sb.rule_y = top + s_sb.m_lab.cap + sc(2);
  const int by = top + s_sb.m_lab.cap + sc(SB_GAP);
  s_sb.row_x = x0;
  s_sb.glyph_y = by + (badge_h - s_sb.m_bad.cap) / 2 - s_sb.m_bad.bearing;
  s_sb.t_y = by + (badge_h + s_sb.m_bad.cap) / 2 - s_sb.m_val.cap - s_sb.m_val.bearing;
  s_sb.q_x = x0;
  s_sb.q_y = by + row_h - (s_sb.q_day ? s_sb.m_lab.bearing : s_sb.m_val.bearing);
  s_sb.show = true;
}

// ---- rain on its way: within the hour, the quiet face says so in the band
// beside the modules, two short lines, RAIN IN over 15 MIN, to the nearest
// five minutes up: the forecast comes by the quarter hour. A flick's answer
// takes the place first.
#ifdef HAS_RAIN
static struct { bool show; char mins[10]; int x, y1, y2; } s_rn;
static void layout_rain(int band_top, int band_bot, int left, int right) __attribute__((noinline));
static void layout_rain(int band_top, int band_bot, int left, int right) {
  s_rn.show = false;
  const int32_t ahead = s_wx.valid && s_wx.rain_at ? s_wx.rain_at - (int32_t)time(NULL) : -1;
  if (ahead <= 0 || ahead > 60 * 60) return;
  const int m = ((ahead + 59) / 60 + 4) / 5 * 5;
  snprintf(s_rn.mins, sizeof(s_rn.mins), "%d MIN", m);
  const int w = run_w("RAIN IN", s_f_cap, false, TRACK), w2 = run_w(s_rn.mins, s_f_cap, false, TRACK);
  const int h = 2 * s_m_cap.cap + sc(SB_GAP);
  if ((w > w2 ? w : w2) > right - left - sc(8) || h > band_bot - band_top) return;
  const int top = band_top + (band_bot - band_top - h) / 2;
  s_rn.x = left + sc(8);
  s_rn.y1 = top - s_m_cap.bearing;
  s_rn.y2 = top + s_m_cap.cap + sc(SB_GAP) - s_m_cap.bearing;
  s_rn.show = true;
}
static void paint_rain(void) __attribute__((noinline));
static void paint_rain(void) {
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run("RAIN IN", s_f_cap, s_rn.x, s_rn.y1, false, TRACK);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run(s_rn.mins, s_f_cap, s_rn.x, s_rn.y2, false, TRACK);
}
#endif

static void paint_sideblock(void) __attribute__((noinline));
static void paint_sideblock(void) {
  const SvRow *row = &s_sv.row[0];
  graphics_context_set_text_color(s_ctx, light_theme() ? s_ink : s_dim);
  draw_run(s_sb.stop, s_f_cap, s_sb.stop_x, s_sb.stop_y, false, TRACK);
  if (light_theme()) {
    const int w = run_w(s_sb.stop, s_f_cap, false, TRACK);
    draw_stop_rule(mapx(s_sb.stop_x, w), mapx(s_sb.stop_x, w) + w, s_sb.rule_y);
  }
  if (!SB_NOROW) {
  const int rx = mapx(s_sb.row_x, s_sb.row_w);
  const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(row->color), s_ink);
  graphics_context_set_fill_color(s_ctx, fill);
  graphics_fill_rect(s_ctx, GRect(rx, s_sb.glyph_y + s_sb.m_bad.bearing - (s_sb.badge_h - s_sb.m_bad.cap) / 2, s_sb.badge_w, s_sb.badge_h), sc(2), GCornersAll);
  graphics_context_set_text_color(s_ctx, on_fill(fill));
  draw_run_s(row->route, s_f_label, rx + s_sb.glyph_dx, s_sb.glyph_y, false, 0);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_sb.t1, sv_font(s_sb.t1), rx + s_sb.t_dx, s_sb.t_y, false, 0);
  if (row->live)
    paint_live_mark(rx + s_sb.t_dx + run_w(s_sb.t1, sv_font(s_sb.t1), false, 0) + 1, s_sb.t_y + s_sb.m_val.bearing - 1);
  }
  if (s_sb.q[0]) {
    graphics_context_set_text_color(s_ctx, s_sb.q_day ? s_dim : s_ink);
    draw_run(s_sb.q, s_sb.q_day ? s_f_cap : sv_font(s_sb.q), s_sb.q_x, s_sb.q_y, false, s_sb.q_day ? TRACK : 0);
  }
}

// ---- zone 03/04: the time, flush to the outer edge so the minute survives
// a cuff that hides the hour.
static struct {
  char hh[4], mm[4];
  int x, y, w, hh_w, colon_w, cgap, band_top;
  GSize z_colon;
  GFont f;
  // Through a flick the seconds stand in the colon's place.
  bool secs;
  char ss[3];
  int ss_dx, s1_y, s2_y;
} s_tm;

// ---- the seconds through a flick: two digits stacked in the colon's place,
// in the accent, a step brighter on the dark ground. The larger date's
// digits are as wide as a colon dot and stand as tall as the pair, so the
// time keeps its width and nothing beneath it moves. A board read against a
// timetable wants to know where in the minute it is.
static void layout_secs(const struct tm *t) __attribute__((noinline));
static void layout_secs(const struct tm *t) {
  snprintf(s_tm.ss, sizeof(s_tm.ss), "%02d", t->tm_sec);
  const Metrics mf = barlow_metrics(measure("0", s_tm.f).h);
  const Metrics ml = barlow_metrics(measure("8", s_f_bigdate).h);
  const int dw = run_w("0", s_f_bigdate, true, 0);
  s_tm.ss_dx = s_tm.hh_w + (s_tm.colon_w - dw + 1) / 2;
  const int mid = s_tm.y + mf.bearing + mf.cap / 2, gap = sc(3);
  s_tm.s1_y = mid - gap / 2 - ml.cap - ml.bearing;
  s_tm.s2_y = mid + (gap + 1) / 2 - ml.bearing;
  s_tm.secs = true;
}

// The content box, inside the rail and the paddings, in logical x.

// Each zone is laid out into static storage by one function and painted by
// another. The painters are what sit beneath the firmware's text renderer,
// and with their numbers in memory rather than in locals their frames stay
// small enough to leave it the stack it needs.


static void layout_time(const struct tm *t, const Frame *fr, GFont f, int margin_top) __attribute__((noinline));
static void layout_time(const struct tm *t, const Frame *fr, GFont f, int margin_top) {
  snprintf(s_tm.hh, sizeof(s_tm.hh), "%02d", display_hour(t->tm_hour));
  snprintf(s_tm.mm, sizeof(s_tm.mm), "%02d", t->tm_min);
  s_tm.f = f;
  const Metrics m = barlow_metrics(measure("0", f).h);
  s_tm.z_colon = measure(":", f);
  s_tm.cgap = sc(COLON_GAP);
  s_tm.hh_w = run_w(s_tm.hh, f, true, 0);
  s_tm.colon_w = s_tm.z_colon.w + 2 * s_tm.cgap;
  s_tm.w = s_tm.hh_w + s_tm.colon_w + run_w(s_tm.mm, f, true, 0);
  s_tm.x = fr->end - s_tm.w;
  // The design's line box sits its digits well below the padding: the cap
  // top lands 18px down at this scale.
  s_tm.y = fr->top + sc(margin_top) - m.bearing;
  // The band the design leaves empty runs from the time's line box down to
  // the top of the countdown block.
  s_tm.band_top = s_tm.y + m.bearing + m.cap + sc(3);
  s_tm.secs = false;
}

// The accent a step brighter on the dark ground, where small digits in the
// cobalt itself sink into the black: each colour channel up one of Pebble's
// four levels. The light ground takes the accent as it is.
static GColor secs_color(void) {
#ifdef PBL_COLOR
  GColor c = accent();
  if (!light_theme()) {
    if (c.r < 3) c.r++;
    if (c.g < 3) c.g++;
    if (c.b < 3) c.b++;
  }
  return c;
#else
  return s_ink;
#endif
}

// The plain face keeps no room for a rail: its time sits centred as it's
// seen, and everything else hangs from the time's outer edge as ever. The
// digits each take a zero's width, drawn from its start, so a narrow last
// digit (10:11) leaves the end of its cell empty: that part isn't counted,
// and the face moves a few pixels as such a minute comes and goes.
static void centre_time(Frame *fr) __attribute__((noinline));
static void centre_time(Frame *fr) {
  if (!s_quiet_face) return;
  const char last[2] = { s_tm.mm[1], 0 };
  const int seen = s_tm.w - (measure("0", s_tm.f).w - measure(last, s_tm.f).w);
  const int px = (s_w - seen) / 2;   // where the time starts on the screen
  s_tm.x = s_set.wrist_right ? s_w - s_tm.w - px : px;
  fr->end = s_set.wrist_right ? s_tm.x + s_tm.w : s_tm.x + seen;
}

static void paint_time(void) __attribute__((noinline));
static void paint_time(void) {
  // The row is mirrored as a whole; hour, colon and minute keep their order.
  const int x = mapx(s_tm.x, s_tm.w);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_tm.hh, s_tm.f, x, s_tm.y, true, 0);
  draw_run_s(s_tm.mm, s_tm.f, x + s_tm.hh_w + s_tm.colon_w, s_tm.y, true, 0);
  if (s_tm.secs) {
    graphics_context_set_text_color(s_ctx, secs_color());
    char d[2] = { s_tm.ss[0], 0 };
    draw_run_s(d, s_f_bigdate, x + s_tm.ss_dx, s_tm.s1_y, true, 0);
    d[0] = s_tm.ss[1];
    draw_run_s(d, s_f_bigdate, x + s_tm.ss_dx, s_tm.s2_y, true, 0);
    return;
  }
  // The one accent in the time: the design's colon.
  graphics_context_set_text_color(s_ctx, accent());
  s_tx.t = ":"; s_tx.f = s_tm.f; s_tx.mode = GTextOverflowModeWordWrap;
  s_tx.r = GRect(x + s_tm.hh_w + s_tm.cgap, s_tm.y, s_tm.z_colon.w, s_tm.z_colon.h);
  tx_draw();
}

// ---- zone 02: the countdown, and zone 04: weekday and date.
static struct {
  char dow[8], date[12], num[8];
  const char *unit;
  const char *label;          // over the block, at the yard; none elsewhere
  bool now, solid, bare;
  GSize z_num;
  Metrics m_min, m_num, m_dow, m_date;
  int num_w, num_row_w, inner_w, inner_x, num_y, label_x, label_y;
  int block_x, block_y, block_w, block_h, date_y, dow_y, top;   // top: where the band above ends
  GFont f_num;
  bool show_date;
} s_bk;

static void layout_block(const struct tm *t, const Schedule *sch, const Frame *fr, GFont f_num, int row_gap, bool date) __attribute__((noinline));
static void layout_block(const struct tm *t, const Schedule *sch, const Frame *fr, GFont f_num, int row_gap, bool date) {
  strftime(s_bk.dow, sizeof(s_bk.dow), "%a", t);
  strftime(s_bk.date, sizeof(s_bk.date), "%d %b", t);
  for (char *p = s_bk.dow; *p; p++) *p = toupper((int)*p);
  for (char *p = s_bk.date; *p; p++) *p = toupper((int)*p);

  s_bk.now = sch->is_now;
  s_bk.solid = sch->is_now || sch->is_boarding;
  s_bk.unit = "MIN";
  // The block is the number and its unit; the departure's clock time is
  // the chips' to say, and the face at the hub is busy enough.
  s_bk.label = NULL;
  if (sch->is_now) {
    strncpy(s_bk.num, "NOW", sizeof(s_bk.num));
  } else {
    if (sch->is_final) {
      snprintf(s_bk.num, sizeof(s_bk.num), "%d", sch->secs);
      s_bk.unit = "SEC";
    } else {
      snprintf(s_bk.num, sizeof(s_bk.num), "%d", sch->remaining);
    }
  }

  // At the yard the block is the count of buses still out, a label over it.
#ifdef HAS_YARD
  const int out = yard_out();
  if (out >= 0) {
    s_bk.now = s_bk.solid = false;
    // The feed drops a bus as its last trip ends, not at the gate, so none on
    // a trip isn't all in: the last may still be driving back. In the hour
    // after the last trip the phone knows when it ended, and the face says
    // how long ago, flick or not, so the wearer judges; otherwise a bare 0.
    s_bk.unit = "OUT";
    if (out == 0 && yard_counting()) {
      const int ago = (int)((time(NULL) - s_yd.end) / 60);
      snprintf(s_bk.num, sizeof(s_bk.num), "%d", ago < 0 ? 0 : ago);
      s_bk.unit = "MIN AGO";
      s_bk.label = "LAST TRIP ENDED";
    } else {
      snprintf(s_bk.num, sizeof(s_bk.num), "%d", out);
      if (!out) s_bk.unit = "";
      s_bk.label = out == 0 ? "NONE ON TRIPS" : out == 1 ? "BUS STILL OUT" : "BUSES STILL OUT";
    }
  }
#endif

  // NOW is set in the countdown face itself, as the design draws it.
  const GFont f_label = s_f_label, f_date = s_f_date;
  s_bk.f_num = f_num; s_bk.show_date = date;
  s_bk.z_num = measure(s_bk.num, f_num);
  const bool bare = s_bk.now;   // NOW carries no unit
  const int min_w = bare ? 0 : run_w(s_bk.unit, f_label, false, TRACK);
  s_bk.m_min = barlow_metrics(bare ? 0 : measure(s_bk.unit, f_label).h);
  s_bk.m_num = barlow_metrics(s_bk.z_num.h);
  s_bk.m_dow = barlow_metrics(measure(s_bk.dow, f_date).h);
  s_bk.m_date = barlow_metrics(measure(s_bk.date, f_date).h);
  const int dow_w = run_w(s_bk.dow, f_date, false, TRACK);
  const int date_w2 = run_w(s_bk.date, f_date, false, TRACK);

  s_bk.num_w = s_bk.now ? s_bk.z_num.w : run_w(s_bk.num, f_num, true, 0);
  s_bk.num_row_w = s_bk.num_w + (bare ? 0 : sc(LABEL_GAP) + min_w);
  s_bk.bare = bare;
  int inner_w = s_bk.num_row_w;
  const int min_inner = sc(BLOCK_MIN_W) - sc(BLOCK_PAD_IN) - sc(BLOCK_PAD_OUT);
  if (inner_w < min_inner) inner_w = min_inner;

  // The bottom row is a space-between pair and the block must not grow into
  // the weekday/date column. Should a label outgrow the row, the block's own
  // padding gives way first and the normal state keeps the design's spacing.
  const int date_w = date ? (dow_w > date_w2 ? dow_w : date_w2) : 0;
  const int avail = fr->end - fr->start - date_w - sc(ROW_GAP);
  int pad_in = sc(BLOCK_PAD_IN), pad_out = sc(BLOCK_PAD_OUT);
  int over = inner_w + pad_in + pad_out - avail;
  if (over > 0) {
    int give = pad_in - sc(BLOCK_PAD_MIN);
    if (give > over) give = over;
    if (give > 0) { pad_in -= give; over -= give; }
  }
  if (over > 0) {
    int give = pad_out - sc(BLOCK_PAD_MIN);
    if (give > over) give = over;
    if (give > 0) { pad_out -= give; over -= give; }
  }
  if (over > 0) inner_w -= over;   // last resort: the label overruns

  s_bk.inner_w = inner_w;
  s_bk.block_w = inner_w + pad_in + pad_out;
  (void)row_gap;
  s_bk.block_h = s_bk.m_num.cap + sc(BLOCK_PAD_T) + sc(BLOCK_PAD_B);
  s_bk.block_x = fr->end - s_bk.block_w;
  s_bk.block_y = fr->bot - s_bk.block_h;
  // Label and number are end-aligned within the block. The design puts a
  // full line of air between them: measured with its own font, 15px from
  // the label's baseline to the top of the digits at 288 wide, so seven
  // here. The estimated cap of the label runs a pixel long, hence eight in
  // the constant.
  s_bk.inner_x = s_bk.block_x + pad_in;
  s_bk.num_y = s_bk.block_y + sc(BLOCK_PAD_T);
  // Weekday and date sit on the wrist side: first to disappear.
  s_bk.date_y = fr->bot - sc(DATE_PAD_BOT) - s_bk.m_date.cap;
  s_bk.dow_y = s_bk.date_y - sc(DOW_GAP) - s_bk.m_dow.cap;
  // A label stands over the block, and the band above ends there.
  s_bk.top = s_bk.block_y;
#ifdef HAS_YARD
  if (s_bk.label) {
    s_bk.label_y = s_bk.num_y - sc(YARD_GAP) - s_m_cap.cap - s_m_cap.bearing;
    s_bk.label_x = fr->end - run_w(s_bk.label, s_f_cap, false, TRACK);
    s_bk.top = s_bk.label_y + s_m_cap.bearing - sc(4);
  }
#endif
}

static void paint_block(int fr_start) __attribute__((noinline));
static void paint_block(int fr_start) {
  if (s_bk.solid) {
    const GColor fill = s_bk.now ? s_ink : accent();
    graphics_context_set_fill_color(s_ctx, fill);
    graphics_fill_rect(s_ctx, GRect(mapx(s_bk.block_x, s_bk.block_w), s_bk.block_y, s_bk.block_w, s_bk.block_h),
                       0, GCornerNone);
    graphics_context_set_text_color(s_ctx, on_fill(fill));
  } else {
    graphics_context_set_text_color(s_ctx, s_ink);
  }
  const int num_x = s_bk.inner_x + s_bk.inner_w - s_bk.num_row_w;
  if (s_bk.now) {
    draw_at(s_bk.num, s_bk.f_num, num_x, s_bk.num_y - s_bk.m_num.bearing, s_bk.z_num);
  } else {
    // One row, mirrored as a whole: the number, then MIN on its baseline.
    const int x = mapx(num_x, s_bk.num_row_w);
    draw_run_s(s_bk.num, s_bk.f_num, x, s_bk.num_y - s_bk.m_num.bearing, true, 0);
    if (!s_bk.bare)
      draw_run_s(s_bk.unit, s_f_label, x + s_bk.num_w + sc(LABEL_GAP),
                 s_bk.num_y + s_bk.m_num.cap - s_bk.m_min.cap - s_bk.m_min.bearing, false, TRACK);
  }
#ifdef HAS_YARD
  if (s_bk.label) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_bk.label, s_f_cap, s_bk.label_x, s_bk.label_y, false, TRACK);
  }
#endif
  if (!s_bk.show_date) return;
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run(s_bk.dow, s_f_date, fr_start, s_bk.dow_y - s_bk.m_dow.bearing, false, TRACK);
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run(s_bk.date, s_f_date, fr_start, s_bk.date_y - s_bk.m_date.bearing, false, TRACK);
}

static void face_update(Layer *layer, GContext *ctx) {
  // A timeline peek slides up over the bottom of the watchface — precisely
  // where the countdown and the date sit. Laying out against the
  // unobstructed area keeps the second-most important zone on screen
  // instead of letting the peek bury it.
  static GRect full, b;
  static Schedule sch;
  static struct tm *t;
  static Frame fr;
  static bool at_hub, peek, yard, idle;
  s_ctx = ctx;
  full = layer_get_bounds(layer);
  b = layer_get_unobstructed_bounds(layer);
  s_w = b.size.w;
  peek = b.size.h < full.size.h - sc(8);

  const time_t now = time(NULL);
  t = localtime(&now);
  sch = schedule_for(t->tm_hour, t->tm_min, t->tm_sec);
/*DEMO*/
  s_now_min = t->tm_hour * 60 + t->tm_min;
  // With the hub known, the countdown is for the hub: at it in its hours the
  // face runs as ever, with the second countdown; anywhere else it is quiet.
  at_hub = transit_fresh(now) && s_tr.state == 1;
  // The countdown is earned by being at the hub. Anywhere else the face is
  // a plain watch — unless the countdown is asked for everywhere.
  // At the yard a flick's answer takes the countdown's place: the count of
  // buses still out in the block, the stop up the road beside the modules.
  // Either way the edge is the hairline, not the rail.
  yard = YARD_ON();
  idle = !at_hub && s_set.transit != TRANSIT_CHIPS && !yard;
  s_quiet_face = idle || yard;

  theme_apply(t->tm_hour);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  if (s_quiet_face) draw_hairline(ctx, b.size.h);
  else draw_rail(ctx, &sch, b.size.h);

  fr.start = sc(PAD_WRIST);                                       // logical left
  fr.end = s_w - sc(RAIL_W) - sc(RAIL_BORDER) - sc(PAD_GUTTER);   // logical right
  fr.top = sc(PAD_TOP);
  fr.bot = b.size.h - sc(PAD_BOTTOM);

  // The answer to a flick is counted in rows, not metres. One row sits in
  // the band beside the modules and the face keeps everything else. Two or
  // three take the board: the time full size, the countdown stepped aside,
  // a small date at the foot. Laid out first, so a one-row answer with no
  // room beside the modules can still take the board.
  s_sb.show = false;
  if (!s_sv.valid || s_sv.n == 1 || yard) {
    // A timeline peek covers the bottom third. The date goes first, the time
    // and the countdown step down, and the modules and the second countdown
    // keep their band, as the design reflows it.
    layout_time(t, &fr, peek ? s_f_time_p : s_f_time, peek ? TIME_MARGIN_TOP_P : TIME_MARGIN_TOP);
    centre_time(&fr);
    if (idle) layout_idle(t, &fr);
    else if (peek) layout_block(t, &sch, &fr, s_f_count_s, BLOCK_ROW_GAP_P, false);
    else layout_block(t, &sch, &fr, s_f_count, BLOCK_ROW_GAP, true);
    const int band_bot = idle ? s_id.top : s_bk.top;
    layout_band(&fr, at_hub, s_tm.band_top, band_bot, t);
    // The quiet face has nothing at the outer end: the modules go there too,
    // in their order, out from under the sleeve.
    if (idle && s_md.n) s_md.x0 = fr.end - s_md.total + modules_hang();
    if (s_sv.valid && !s_gb.show) {
      // Beside the modules: on the quiet face to their wrist side, on the
      // countdown face at the outer end past them.
      if (idle) layout_sideblock(&fr, s_tm.band_top, band_bot, fr.start - sc(8), s_md.n ? s_md.x0 - sc(8) : fr.end);
      else layout_sideblock(&fr, s_tm.band_top, band_bot, s_md.n ? s_md.x0 + s_md.total : fr.start - sc(8), fr.end);
      if (s_sb.show) layout_secs(t);
    }
#ifdef HAS_RAIN
    s_rn.show = false;
    if (idle && !s_sb.show) layout_rain(s_tm.band_top, band_bot, fr.start - sc(8), s_md.n ? s_md.x0 - sc(8) : fr.end);
#endif
    // A flick with nothing to show, or one still waiting on the phone: the
    // seconds in the colon, on every face, so the gesture is seen to have
    // landed; the countdown keeps its minutes.
    if (s_sv.lit && !s_tm.secs) layout_secs(t);
  }
  // With no room for the stop beside the modules the yard keeps its count.
#ifdef HAS_WAVE
  if (s_sv.valid && s_sv.take && !yard) {
    // The answer takes the screen: the content box even on both sides, with
    // no rail to keep room for.
    fr.end = s_w - fr.start;
    layout_take(t, &fr);
    paint_take();
  } else
#endif
  if (s_sv.valid && !s_sb.show && !yard) {
    layout_time(t, &fr, s_f_time, TIME_MARGIN_TOP);
    centre_time(&fr);
    layout_secs(t);
    paint_time();
#ifdef HAS_WAVE
    if (s_sv.wave) {
      layout_wave(t, &fr, s_tm.band_top);
      paint_wave(fr.start);
    } else
#endif
    {
      layout_stopview(t, &fr, s_tm.band_top);
      paint_stopview(fr.start);
    }
  } else {
    paint_time();
    if (idle) paint_idle();
    else paint_block(fr.start);
    paint_modules();
    if (s_gb.show) paint_gb();
    if (s_sb.show) paint_sideblock();
#ifdef HAS_RAIN
    if (s_rn.show) paint_rain();
#endif
  }

  // ---- boarding buzz, once on the transition into the solid block: at the
  // hub by default, or wherever the countdown runs.
  const bool buzz = s_set.buzz == BUZZ_ALWAYS || (s_set.buzz == BUZZ_HUB && at_hub);
  if (!s_quiet_face && buzz && sch.remaining == THRESHOLD && s_last_remaining != THRESHOLD
      && !quiet_time_is_active()) {
    vibes_short_pulse();
  }
  s_last_remaining = sch.remaining;
}

// ------------------------------------------------------------------- wiring

// Once a minute, as the design asks, except through the final minute, where
// the countdown is running in seconds.
static bool s_ticking_seconds = false;

static void tick_handler(struct tm *tick_time, TimeUnits units);

static void retune_tick(void) {
  const time_t now = time(NULL);
  struct tm *t = localtime(&now);
  const Schedule s = schedule_for(t->tm_hour, t->tm_min, t->tm_sec);
  const int now_min = t->tm_hour * 60 + t->tm_min;
  const bool gb_final = s_set.final_seconds && transit_fresh(now) && s_tr.state == 1
      && (transit_next(s_tr.g, now_min) == 1 || transit_next(s_tr.b, now_min) == 1);
  const bool want = s.is_final || s_sv.lit || gb_final;
  if (want == s_ticking_seconds) return;
  tick_timer_service_unsubscribe();
  tick_timer_service_subscribe(want ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
  s_ticking_seconds = want;
}

// ---- near a hub the watch, not a timer, says when the phone should look:
// after a hundred steps, about seventy metres, since it last did. Sitting at
// home by the transit centre costs nothing; walking to it, the countdown is
// on as you arrive. A watch without a step count asks every five minutes.
#define LOOK_STEPS 100
#define LOOK_EVERY (5 * 60)
static int32_t s_look_steps = -1;   // the day's steps at the last look, -1 none yet
static time_t s_look_at;
static int32_t steps_today(void) {
#ifdef PBL_HEALTH
  const time_t now = time(NULL);
  if (health_service_metric_accessible(HealthMetricStepCount, time_start_of_today(), now) & HealthServiceAccessibilityMaskAvailable)
    return (int32_t)health_service_sum_today(HealthMetricStepCount);
#endif
  return -1;
}
static void looked(void) {   // a fix has just been taken, by whoever asked
  s_look_steps = steps_today();
  s_look_at = time(NULL);
}
static void walk_look(void) {
  if (!s_tr.near || s_set.transit == TRANSIT_OFF) return;
  const int32_t steps = steps_today();
  const bool due = steps >= 0
      ? (s_look_steps < 0 || steps < s_look_steps || steps - s_look_steps >= LOOK_STEPS)
      : time(NULL) - s_look_at >= LOOK_EVERY;
  if (!due) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_LOOK, 1);
  if (app_message_outbox_send() == APP_MSG_OK) looked();
}

#ifdef HAS_YARD
// Through the yard's hour, while the phone last saw the wearer near it, the
// watch asks the phone to look every two minutes: the phone's own timer can
// be held back in the background, a message from the watch wakes it.
#define YARD_EVERY 120
static void yard_ask(void) {
  const time_t now = time(NULL);
  if (!s_yd.near || now < s_yd.from || now >= s_yd.wuntil || now - s_yd_asked < YARD_EVERY) return;
  if (s_yd.out == 0 && s_yd.end && now - s_yd.end >= YARD_AFTER) return;   // done: the last is long in
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_YARD, 1);
  if (app_message_outbox_send() == APP_MSG_OK) s_yd_asked = now;
}
#endif

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  retune_tick();
  if (units & MINUTE_UNIT) walk_look();
#ifdef HAS_YARD
  yard_ask();
#endif
  layer_mark_dirty(s_face);
}

// A setting from the page arrives as the string it was chosen as, but a
// value can also come back as an int, and reading an int as a string gives
// whatever bytes it holds. Take either.
static int tuple_int(const Tuple *tp) {
  return tp->type == TUPLE_CSTRING ? atoi(tp->value->cstring) : (int)tp->value->int32;
}

// The hairline's own clock, only while it drains on the plain face. It wakes
// exactly when the line has a whole step less to show, so every step is the
// same: a pixel at a time while the light is on and a little past, when the
// face is being read; three at a time after that. None once the answer goes.
#define DRAIN_SMOOTH_MS 3500   // the light's few seconds, and a little past them
#define DRAIN_LATE_PX 3
static void drain_tick(void *data) {
  (void)data;
  s_sv.drain = NULL;
  const int32_t left = line_left();
  if (left <= 0 || !s_quiet_face) return;
  layer_mark_dirty(s_face);
  const int32_t h = layer_get_bounds(s_face).size.h > 0 ? layer_get_bounds(s_face).size.h : 168;
  const int32_t t = since_flick();
  const int32_t px = h * left / 10000;
  const int32_t target = px - (t < DRAIN_SMOOTH_MS ? 1 : DRAIN_LATE_PX);
  // When the line is halfway through the target pixel, on the current
  // segment: waking at the pixel's edge, a millisecond late reads a pixel
  // short and the step comes out one too many.
  int32_t at = s_sv.end_at;
  if (target > 0 && s_sv.seg_f > 0)
    at = s_sv.end_at - (2 * target + 1) * 10000 / (2 * h) * (s_sv.end_at - s_sv.seg_at) / s_sv.seg_f;   // fits 32 bits: 10000 x 15000
  const int32_t wait = at - t;
  s_sv.drain = app_timer_register(wait < 15 ? 15 : (uint32_t)wait, drain_tick, NULL);
}

// A flick starts the line's countdown, twelve seconds from now.
static void drain_start(void) {
  time_ms(&s_sv.t0_s, &s_sv.t0_ms);
  s_sv.draining = true;
  s_sv.seg_at = 0; s_sv.seg_f = 10000; s_sv.end_at = SV_SHOW_MS;
  if (s_sv.drain) app_timer_cancel(s_sv.drain);
  s_sv.drain = app_timer_register(0, drain_tick, NULL);
}
// The answer has landed: the line goes on from where it is, to nothing by
// twelve seconds from the flick, or by SV_MIN_MS from now if that is later.
// Returns how long the answer stays up.
static uint32_t drain_answer(void) {
  if (!s_sv.draining) drain_start();
  const int32_t t = since_flick(), left = line_left();
  const int32_t end = t + SV_MIN_MS > SV_SHOW_MS ? t + SV_MIN_MS : SV_SHOW_MS;
  s_sv.seg_at = t; s_sv.seg_f = (int16_t)(left < 0 ? 10000 : left); s_sv.end_at = end;
  if (s_sv.drain) app_timer_cancel(s_sv.drain);
  s_sv.drain = app_timer_register(0, drain_tick, NULL);
  return (uint32_t)(end - t);
}

static void stopview_done(void *data) {
  (void)data;
  s_sv.timer = NULL;
  if (s_sv.drain) { app_timer_cancel(s_sv.drain); s_sv.drain = NULL; }
  s_sv.valid = false;
  s_sv.pending = false;
  s_sv.lit = false;
  s_sv.draining = false;
  retune_tick();
  layer_mark_dirty(s_face);
}
static void stopview_hold(uint32_t ms) {
  if (s_sv.timer) app_timer_reschedule(s_sv.timer, ms);
  else s_sv.timer = app_timer_register(ms, stopview_done, NULL);
}

// Whether a flick asks the phone, and so takes a fix. Not with transit
// off. In the chosen modes, always. Automatic: inside a system, or where the
// wearer could have driven into one since the phone last looked, at highway
// speed; nobody drives from the next state into Logan in half an hour, so a
// flick far from every system never takes a fix. With nothing heard yet, ask.
#define DRIVE_KMH 130
static bool flick_asks(void) {
  if (s_set.transit == TRANSIT_OFF || !s_set.flick) return false;
  if (s_set.transit != TRANSIT_AUTO && s_set.transit != TRANSIT_NEAR) return true;
  if (s_tr.area || !s_tr.at || !s_tr.km) return true;
  const int32_t gone = (int32_t)(time(NULL) - s_tr.at);
  return gone < 0 || gone / 60 * DRIVE_KMH / 60 + 1 >= s_tr.km;
}

// A flick of the wrist asks the phone for the nearest stop. The light comes
// on with the gesture, and again when the answer lands, so the answer is
// read in the same glance.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis; (void)direction;
  // Every flick is heard: the light, and the seconds, at once. One made
  // while the phone is still answering an earlier flick (a stray one from
  // the arm swinging, say) lights the face again and waits on that answer.
  // The phone is asked only where the answer could be worth a fix
  // (flick_asks).
  light_enable_interaction();
  if (s_sv.pending) return;
  s_sv.lit = true;
  drain_start();
  stopview_hold(SV_SHOW_MS);
  retune_tick();
  layer_mark_dirty(s_face);
  if (!flick_asks()) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_FLICK, 1);
  if (app_message_outbox_send() != APP_MSG_OK) return;
  s_sv.pending = true;
  stopview_hold(SV_WAIT_MS);
}

#ifdef HAS_WAVE
// A wave: labels comma-separated (a label may hold a space, "16 AM"), the
// colours three bytes a badge, the live and away bits an int each.
static void take_wave(DictionaryIterator *iter, int i, uint32_t kr, uint32_t kw, uint32_t kk, uint32_t kt, uint32_t ko) {
  Tuple *tr = dict_find(iter, kr), *tw = dict_find(iter, kw), *tk = dict_find(iter, kk), *tt = dict_find(iter, kt);
  Tuple *to = dict_find(iter, ko);
  WaveLine *w = &s_wave[i];
  strncpy(w->when, tw ? tw->value->cstring : "", sizeof(w->when) - 1); w->when[sizeof(w->when) - 1] = 0;
  w->live = tt ? (uint16_t)tt->value->int32 : 0;
  w->away = to ? (uint16_t)to->value->int32 : 0;
  w->n = 0;
  const char *p = tr ? tr->value->cstring : "";
  while (*p && w->n < WV_MAX) {
    const char *e = strchr(p, ',');
    const int len = e ? e - p : (int)strlen(p);
    const int k = len < (int)sizeof(w->lab[0]) - 1 ? len : (int)sizeof(w->lab[0]) - 1;
    memcpy(w->lab[w->n], p, k); w->lab[w->n][k] = 0;
    const int b = 3 * w->n;
    const uint8_t *d = tk && tk->type == TUPLE_BYTE_ARRAY && b + 2 < tk->length ? tk->value->data : NULL;
    w->col[w->n] = d ? ((uint32_t)d[b] << 16) | ((uint32_t)d[b + 1] << 8) | d[b + 2] : 0x888888;
    w->n++;
    if (!e) break;
    p = e + 1;
  }
}
#endif

static void take_row(DictionaryIterator *iter, int i, uint32_t kr, uint32_t kh, uint32_t kw, uint32_t kc, uint32_t kt) {
  Tuple *tr = dict_find(iter, kr), *th = dict_find(iter, kh), *tw = dict_find(iter, kw);
  Tuple *tc = dict_find(iter, kc), *tt = dict_find(iter, kt);
  SvRow *row = &s_sv.row[i];
  strncpy(row->route, tr ? tr->value->cstring : "", sizeof(row->route) - 1); row->route[sizeof(row->route) - 1] = 0;
  strncpy(row->head, th ? th->value->cstring : "", sizeof(row->head) - 1); row->head[sizeof(row->head) - 1] = 0;
  strncpy(row->when, tw ? tw->value->cstring : "", sizeof(row->when) - 1); row->when[sizeof(row->when) - 1] = 0;
  row->color = tc ? (uint32_t)tc->value->int32 & 0xFFFFFF : 0x888888;
  row->live = tt ? tt->value->int32 != 0 : false;
}

// The hub's word, pushed by the phone: from its background check, or with a
// flick's answer, worked out from the flick's own fix.
static void take_transit(DictionaryIterator *iter) {
  Tuple *tp = dict_find(iter, MESSAGE_KEY_TR_STATE);
  if (!tp) return;
  Tuple *tg = dict_find(iter, MESSAGE_KEY_TR_G);
  Tuple *tb = dict_find(iter, MESSAGE_KEY_TR_B);
  Tuple *ta = dict_find(iter, MESSAGE_KEY_TR_AT);
  Tuple *tarea = dict_find(iter, MESSAGE_KEY_TR_AREA);
  Tuple *tkm = dict_find(iter, MESSAGE_KEY_TR_KM);
  Tuple *tnear = dict_find(iter, MESSAGE_KEY_TR_NEAR);
  s_tr.state = (uint8_t)tp->value->int32;
  s_tr.near = tnear ? (uint8_t)(tnear->value->int32 != 0) : 0;
  looked();
  s_tr.km = tkm ? (uint16_t)tkm->value->int32 : 0;
  s_tr.area = tarea ? (uint8_t)(tarea->value->int32 != 0) : 1;
  for (int i = 0; i < TR_MAX; i++) {
    s_tr.g[i] = (tg && tg->length >= 2 * TR_MAX) ? (uint16_t)(tg->value->data[2 * i] | (tg->value->data[2 * i + 1] << 8)) : TR_NONE;
    s_tr.b[i] = (tb && tb->length >= 2 * TR_MAX) ? (uint16_t)(tb->value->data[2 * i] | (tb->value->data[2 * i + 1] << 8)) : TR_NONE;
  }
  s_tr.at = ta ? (time_t)ta->value->int32 : time(NULL);
  transit_save();
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *tp;
  take_transit(iter);
#ifdef HAS_YARD
  if ((tp = dict_find(iter, MESSAGE_KEY_YD_FROM))) {
    Tuple *tu = dict_find(iter, MESSAGE_KEY_YD_UNTIL), *tn = dict_find(iter, MESSAGE_KEY_YD_NEAR);
    s_yd.from = tp->value->int32;
    s_yd.wuntil = tu ? tu->value->int32 : 0;
    s_yd.near = tn && tn->value->int32 != 0;
    persist_write_data(YARD_KEY, &s_yd, sizeof(s_yd));
    yard_ask();
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_YD_OUT))) {
    Tuple *te = dict_find(iter, MESSAGE_KEY_YD_END), *tu = dict_find(iter, MESSAGE_KEY_YD_UNTIL);
    s_yd.out = (int8_t)(tp->value->int32 > 99 ? 99 : tp->value->int32 < -1 ? -1 : tp->value->int32);
    s_yd.end = te ? te->value->int32 : 0;
    s_yd.until = tu ? tu->value->int32 : 0;
    persist_write_data(YARD_KEY, &s_yd, sizeof(s_yd));
    layer_mark_dirty(s_face);
  }
#endif
  if ((tp = dict_find(iter, MESSAGE_KEY_SV_N))) {
    Tuple *ts = dict_find(iter, MESSAGE_KEY_SV_STOP);
    Tuple *td = dict_find(iter, MESSAGE_KEY_SV_DIST);
    s_sv.n = (int)tp->value->int32;
    if (s_sv.n > SV_ROWS) s_sv.n = SV_ROWS;
    if (s_sv.n < 0) s_sv.n = 0;
    strncpy(s_sv.stop, ts ? ts->value->cstring : "", sizeof(s_sv.stop) - 1); s_sv.stop[sizeof(s_sv.stop) - 1] = 0;
    s_sv.dist = td ? (int)td->value->int32 : 0;
#ifdef HAS_YARD
    Tuple *tout = dict_find(iter, MESSAGE_KEY_SV_OUT);
    s_sv.out = tout ? (int8_t)(tout->value->int32 > 99 ? 99 : tout->value->int32) : -1;
#endif
#ifdef HAS_WAVE
    Tuple *tm = dict_find(iter, MESSAGE_KEY_SV_MODE);
    s_sv.wave = tm && tm->value->int32 == 1 && s_sv.n > 0;
    s_sv.take = 0;
    Tuple *tl = dict_find(iter, MESSAGE_KEY_SV_LIST);
    if (tm && tm->value->int32 >= 2 && tm->value->int32 <= 4 && tl) {
      take_list(tl->value->cstring);
      if (s_tk.n) s_sv.take = (uint8_t)tm->value->int32;
    }
#endif
    if (s_sv.wave) {
#ifdef HAS_WAVE
      take_wave(iter, 0, MESSAGE_KEY_SV_R1, MESSAGE_KEY_SV_W1, MESSAGE_KEY_SV_K1, MESSAGE_KEY_SV_T1, MESSAGE_KEY_SV_O1);
      take_wave(iter, 1, MESSAGE_KEY_SV_R2, MESSAGE_KEY_SV_W2, MESSAGE_KEY_SV_K2, MESSAGE_KEY_SV_T2, MESSAGE_KEY_SV_O2);
      take_wave(iter, 2, MESSAGE_KEY_SV_R3, MESSAGE_KEY_SV_W3, MESSAGE_KEY_SV_K3, MESSAGE_KEY_SV_T3, MESSAGE_KEY_SV_O3);
#endif
    } else {
      take_row(iter, 0, MESSAGE_KEY_SV_R1, MESSAGE_KEY_SV_H1, MESSAGE_KEY_SV_W1, MESSAGE_KEY_SV_C1, MESSAGE_KEY_SV_T1);
      take_row(iter, 1, MESSAGE_KEY_SV_R2, MESSAGE_KEY_SV_H2, MESSAGE_KEY_SV_W2, MESSAGE_KEY_SV_C2, MESSAGE_KEY_SV_T2);
      take_row(iter, 2, MESSAGE_KEY_SV_R3, MESSAGE_KEY_SV_H3, MESSAGE_KEY_SV_W3, MESSAGE_KEY_SV_C3, MESSAGE_KEY_SV_T3);
    }
    s_sv.pending = false;
    if (s_sv.n == 0 && !s_sv.stop[0]) {
      // No stop near enough to speak of: the face stays as it was, and the
      // seconds under the time say the flick was heard. Nothing is taken
      // away to say nothing.
      s_sv.valid = false;
      stopview_hold(drain_answer());
      layer_mark_dirty(s_face);
      return;
    }
    s_sv.valid = true;
    light_enable_interaction();
    stopview_hold(drain_answer());
    retune_tick();
    layer_mark_dirty(s_face);
    return;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_WRIST))) {
    s_set.wrist_right = (strcmp(tp->value->cstring, "right") == 0);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_OFFSET))) {
    s_set.offset = (uint8_t)tp->value->int32;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_HEADWAY))) {
    s_set.headway = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_BUZZ_AT))) {
    s_set.buzz = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_H24))) {
    s_set.time_fmt = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_THEME))) {
    s_set.theme = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_NIGHT_START))) {
    s_set.night_start = (uint8_t)tp->value->int32;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_NIGHT_END))) {
    s_set.night_end = (uint8_t)tp->value->int32;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_ACCENT))) {
    s_set.accent = (uint32_t)tp->value->int32 & 0xFFFFFF;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD1))) {
    s_set.mod[0] = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD2))) {
    s_set.mod[1] = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD3))) {
    s_set.mod[2] = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD_ICONS))) {
    s_set.mod_icons = (uint8_t)tuple_int(tp);
    if (s_set.mod_icons > MOD_ICONS_COLOUR) s_set.mod_icons = MOD_ICONS_ON;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_SECONDS))) {
    s_set.final_seconds = tp->value->int32 != 0;
  }
  // "auto" is the phone's to resolve: it sends metric or imperial itself.
  if ((tp = dict_find(iter, MESSAGE_KEY_UNITS)) && tp->value->cstring[0] != 'a') {
    const bool imperial = (strcmp(tp->value->cstring, "imperial") == 0);
    // The stored reading is in the old unit. Showing it under the new label
    // would be wrong by 30-odd degrees, so drop it until the next fetch.
    if (imperial != s_set.imperial) s_wx.valid = false;
    s_set.imperial = imperial;
  }

  if ((tp = dict_find(iter, MESSAGE_KEY_TRANSIT))) {
    s_set.transit = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_FLICK_ON))) {
    s_set.flick = tp->value->int32 != 0;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_TR_RADIUS))) {
    s_set.radius = (uint16_t)tp->value->int32;
  }
  // Weather arrives on the same channel, pushed by the JS side.
  if ((tp = dict_find(iter, MESSAGE_KEY_WOK))) {
    Tuple *tt = dict_find(iter, MESSAGE_KEY_TEMP);
    Tuple *tc = dict_find(iter, MESSAGE_KEY_WCODE);
    if (tp->value->int32 && tt) {
      s_wx.temp = (int16_t)tt->value->int32;
      s_wx.code = tc ? (uint8_t)tc->value->int32 : 0;
#ifdef HAS_RAIN
      Tuple *tr = dict_find(iter, MESSAGE_KEY_RAIN_AT);
      s_wx.rain_at = tr ? tr->value->int32 : 0;
#endif
      s_wx.valid = true;
      s_wx.at = time(NULL);
      persist_write_data(WEATHER_KEY, &s_wx, sizeof(s_wx));
    }
  }

  settings_clamp();
  settings_save();
  retune_tick();
  layer_mark_dirty(s_face);
}

// Plugged in or out: the modules change at once, not at the next minute.
static void battery_changed(BatteryChargeState state) {
  (void)state;
  layer_mark_dirty(s_face);
}

static void unobstructed_changing(AnimationProgress progress, void *context) {
  layer_mark_dirty(s_face);
}

static void unobstructed_settled(void *context) {
  layer_mark_dirty(s_face);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_face = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_face, face_update);
  layer_add_child(root, s_face);

  const UnobstructedAreaHandlers handlers = {
      .change = unobstructed_changing,
      .did_change = unobstructed_settled,
  };
  unobstructed_area_service_subscribe(handlers, NULL);
}

static void window_unload(Window *window) {
  unobstructed_area_service_unsubscribe();
  layer_destroy(s_face);
}

static void init(void) {
  settings_load();
  weather_load();
  transit_load();
#ifdef HAS_YARD
  s_yd.out = -1;
  if (persist_exists(YARD_KEY) && persist_get_size(YARD_KEY) == (int)sizeof(s_yd)) persist_read_data(YARD_KEY, &s_yd, sizeof(s_yd));
#endif
  s_f_time = fonts_load_custom_font(resource_get_handle(RES_TIME));
  s_f_count = fonts_load_custom_font(resource_get_handle(RES_COUNT));
  s_f_mod = fonts_load_custom_font(resource_get_handle(RES_MOD));
  s_f_label = fonts_load_custom_font(resource_get_handle(RES_LABEL));
  s_f_date = fonts_load_custom_font(resource_get_handle(RES_DATE));
  s_f_board = fonts_load_custom_font(resource_get_handle(RES_BOARD));
  s_f_cap = fonts_load_custom_font(resource_get_handle(RES_CAP));
  s_m_cap = barlow_metrics(measure("B", s_f_cap).h);
#ifdef HAS_YARD
  s_sv.out = -1;   // no yard until the phone says so
#endif
  s_f_bigdate = fonts_load_custom_font(resource_get_handle(RES_BIGDATE));
#ifdef PBL_PLATFORM_APLITE
  // The Classic has no timeline peek, so the peek's smaller time and
  // countdown are never drawn; each custom font costs 60 B of its heap.
  s_f_count_s = s_f_count;
  s_f_time_p = s_f_time;
#else
  s_f_count_s = fonts_load_custom_font(resource_get_handle(RES_COUNT_S));
  s_f_time_p = fonts_load_custom_font(resource_get_handle(RES_TIME_P));
#endif

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load, .unload = window_unload});
  window_set_background_color(s_window, GColorBlack);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  retune_tick();

  app_message_register_inbox_received(inbox_received);
  app_message_open(512, 64);
  accel_tap_service_subscribe(tap_handler);
  battery_state_service_subscribe(battery_changed);
}

static void deinit(void) {
  fonts_unload_custom_font(s_f_time);
  fonts_unload_custom_font(s_f_count);
  fonts_unload_custom_font(s_f_mod);
  fonts_unload_custom_font(s_f_label);
  fonts_unload_custom_font(s_f_date);
  fonts_unload_custom_font(s_f_board);
  fonts_unload_custom_font(s_f_cap);
  fonts_unload_custom_font(s_f_bigdate);
#ifndef PBL_PLATFORM_APLITE
  fonts_unload_custom_font(s_f_count_s);
  fonts_unload_custom_font(s_f_time_p);
#endif
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

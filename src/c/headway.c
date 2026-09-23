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

#define TRANSIT_OFF   0   // the hub is not consulted
#define TRANSIT_CHIPS 1   // countdown everywhere; the second one near the hub
#define TRANSIT_NEAR  2   // both only near the hub, in its hours

typedef struct {
  uint8_t version;     // bumped whenever the fields below change
  bool wrist_right;    // true: rail on the left, sleeve from the right
  uint8_t offset;      // departure, minutes past the hour (0..headway-1)
  uint8_t headway;     // minutes between runs: 30, 20 or 15
  bool buzz;           // single pulse when the boarding block goes solid
  uint8_t time_fmt;    // TIME_FMT_*
  uint8_t theme;       // THEME_*
  uint8_t night_start; // hour the night window opens
  uint8_t night_end;   // hour it closes
  uint32_t accent;     // 0xRRGGBB, snapped to the Pebble 64 at use
  uint8_t mod[3];      // MODULE_*, left to right from the wrist edge
  bool mod_icons;      // icons in place of the captions
  bool final_seconds;  // count the last minute down in seconds
  bool imperial;
  uint8_t transit;     // TRANSIT_*: what knowing the hub changes
  uint16_t radius;     // metres around the hub that count as near
} Settings;

#define SETTINGS_KEY 1
#define SETTINGS_VERSION 2
#define WEATHER_KEY  2
#define TRANSIT_KEY  3
#define THRESHOLD 5   // minutes; block goes solid at or under this

typedef struct {
  int16_t temp;
  uint8_t code;
  bool valid;
  time_t at;
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
  uint16_t g[TR_MAX], b[TR_MAX];
  time_t at;
} Transit;

// The stop view: what the phone answered the last flick with. Shown for a
// few seconds, then the face is itself again.
#define SV_ROWS 3
#define SV_SHOW_MS 12000
#define SV_WAIT_MS 15000
typedef struct { char route[8], head[20], when[8]; uint32_t color; bool mins; } SvRow;
static struct {
  bool valid, pending;
  char stop[24];
  int dist, n;
  SvRow row[SV_ROWS];
  AppTimer *timer;
} s_sv;

static Settings s_set;
static Weather s_wx;
static Transit s_tr;

static void settings_defaults(void) {
  s_set.version = SETTINGS_VERSION;
  s_set.wrist_right = false;
  s_set.offset = 0;
  s_set.headway = 30;
  s_set.buzz = false;
  s_set.time_fmt = TIME_FMT_SYSTEM;
  s_set.theme = THEME_AUTO;   // dark through the night, light by day
  s_set.night_start = 19;
  s_set.night_end = 7;
  s_set.accent = 0x0055AA;   // Cobalt Blue, the design's one accent
  // Off duty it is a plain digital watch, as the design says: the modules
  // are opt-in from the settings page.
  s_set.mod[0] = MODULE_NONE;
  s_set.mod[1] = MODULE_NONE;
  s_set.mod[2] = MODULE_NONE;
  s_set.mod_icons = false;
  s_set.final_seconds = true;
  s_set.imperial = false;
  s_set.transit = TRANSIT_OFF;
  s_set.radius = 300;
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
  if (s_set.transit > TRANSIT_NEAR) s_set.transit = TRANSIT_OFF;
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
    if (n == (int)sizeof(s_set)) {
      persist_read_data(SETTINGS_KEY, &stored, sizeof(stored));
      if (stored.version == SETTINGS_VERSION) s_set = stored;
    } else if (n >= (int)offsetof(Settings, transit) && n < (int)sizeof(s_set)) {
      // Layout 1: the same fields up to the hub settings, which are appended.
      // Carry it over rather than hand the wearer the defaults again.
      persist_read_data(SETTINGS_KEY, &stored, n);
      if (stored.version == 1) s_set = stored;
    }
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
// The phone's word holds for a few hours, then the face falls back to the
// plain countdown rather than stay quiet on a stale fix.
static bool transit_fresh(time_t now) {
  return s_set.transit != TRANSIT_OFF && s_tr.at != 0 && now - s_tr.at < TR_STALE;
}
// Minutes until the first of a route's departures still ahead, or -1.
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
#define ROW_GAP       2
#define MOD_GAP       9
#define MOD_LABEL_GAP 4
#define MOD_ICON_GAP  4
#define BLOCK_PAD_MIN 3
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
  #define RES_CAP   RESOURCE_ID_FONT_CAP_12
  #define RES_BIGDATE RESOURCE_ID_FONT_DATE_25
#else
  #define RES_TIME  RESOURCE_ID_FONT_TIME_60
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_44
  #define RES_MOD   RESOURCE_ID_FONT_MOD_15
  #define RES_LABEL RESOURCE_ID_FONT_LABEL_11
  #define RES_DATE  RESOURCE_ID_FONT_DATE_12
  #define RES_CAP   RESOURCE_ID_FONT_CAP_9
  #define RES_BIGDATE RESOURCE_ID_FONT_DATE_18
#endif

static GFont s_f_time, s_f_count, s_f_mod, s_f_label, s_f_date, s_f_cap, s_f_bigdate;
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
                      int rw, int rh) {
  for (int y = 0; y < rh; y++) {
    for (int x = 0; x < rw; x++) {
      if (rows[y][x] != '#') continue;
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
  draw_bits(ctx, r, FOOTPRINTS, 10, 10);
}

static void icon_battery(GContext *ctx, GRect r, int pct) {
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
  if (fill > 0) graphics_fill_rect(ctx, GRect(r.origin.x + 2, y + 2, fill, h - 4), 0, GCornerNone);
}

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
  draw_bits(ctx, r, HEART, 10, 10);
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
    "....#.##.#",
    ".....####.",
    "....#####.",
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
    ".....##...",
    "....##....",
    "...####...",
    "....##....",
    "...##.....",
  },
};

static void icon_weather(GContext *ctx, GRect r, int code) {
  draw_bits(ctx, r, SKY[wx_kind(code)], 10, 10);
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

// Away from the hub the rail keeps only its hairline: nothing to drain.
static void draw_hairline(GContext *ctx, int16_t h) {
  const int rail_w = sc(RAIL_W), border = sc(RAIL_BORDER);
  graphics_context_set_fill_color(ctx, s_ink);
  graphics_fill_rect(ctx, GRect(mapx(s_w - rail_w - border, border), 0, border, h), 0, GCornerNone);
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
  return s_set.mod_icons;
}

static int module_icon_w(const Module *m, int icon) {
  switch (m->kind) {
    case MODULE_BATTERY: return icon * 8 / 5;
    case MODULE_STEPS:   return icon;
    default:             return icon;
  }
}

static void module_draw_icon(GContext *ctx, const Module *m, GRect box) {
  switch (m->kind) {
    case MODULE_HR:      icon_heart(ctx, box); break;
    case MODULE_STEPS:   icon_steps(ctx, box); break;
    case MODULE_BATTERY: icon_battery(ctx, box, m->extra); break;
    case MODULE_WEATHER: icon_weather(ctx, box, m->extra); break;
    default: break;
  }
}

// The row sits on the wrist side of the band the design leaves open, so a
// sleeve covers it before the countdown or the rail. Laid out into static
// storage, then painted from it.
static struct {
  Module m[MODULE_COUNT];
  int n, widths[MODULE_COUNT], x0, y, label_h, lgap, gap, icon;
  Metrics m_cap, m_val;
  GFont f_val, f_cap;
} s_md;

static void layout_modules(GFont f_val, GFont f_cap, int c_start, int avail_w,
                           int band_top, int band_bot) __attribute__((noinline));
static void layout_modules(GFont f_val, GFont f_cap, int c_start, int avail_w,
                           int band_top, int band_bot) {
  s_md.n = 0;
  for (int i = 0; i < MODULE_COUNT; i++) {
    if (s_set.mod[i] == MODULE_NONE) continue;
    if (module_read(s_set.mod[i], &s_md.m[s_md.n])) s_md.n++;
  }
  if (s_md.n == 0) return;

  s_md.f_val = f_val; s_md.f_cap = f_cap;
  s_md.m_val = barlow_metrics(measure("88", f_val).h);
  s_md.icon = s_md.m_val.cap;                 // an icon reads as one cap tall
  // Captions are laid out from the measured box, not an estimated cap: the
  // box bounds the glyph whatever the font's metrics turn out to be. An icon
  // fills its box where a caption's box carries slack, so it gets a wider
  // gap to the value.
  const bool icons = s_set.mod_icons;
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
  char min[6]; int later;                  // later: 0 both, 1 G leaves later, 2 B leaves later
  int cx, cy, sq, gx, bx, ty, vx, vy, mx, my;
  bool now;
} s_gb;

static void layout_gb(int c_start, int avail_w, int band_top, int band_bot, int now_min) __attribute__((noinline));
static void layout_gb(int c_start, int avail_w, int band_top, int band_bot, int now_min) {
  s_gb.show = false;
  const int g = transit_next(s_tr.g, now_min), b = transit_next(s_tr.b, now_min);
  if (g < 0 && b < 0) return;
  const int next = g < 0 ? b : b < 0 ? g : g < b ? g : b;
  s_gb.later = (g < 0 || g > next + 1) ? 1 : (b < 0 || b > next + 1) ? 2 : 0;
  s_gb.now = next == 0;
  if (s_gb.now) strcpy(s_gb.min, "NOW");
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
  const int unit_w = s_gb.now ? 0 : run_w("MIN", s_f_cap, false, TRACK);
  const int v_w = s_gb.now ? min_w : min_w + sc(2) + unit_w;
  s_gb.w = v_w > chips ? v_w : chips;
  s_gb.gap = sc(MOD_GAP);
  if (s_gb.w > avail_w) return;
  const int x = c_start + avail_w - s_gb.w;
  s_gb.sq = sq;
  s_gb.cx = mapx(x + s_gb.w - chips, chips);
  s_gb.cy = y + label_h - sq + (sq > label_h ? (sq - label_h) / 2 : 0);
  s_gb.gx = s_gb.cx + (sq - run_w("G", s_f_label, false, 0)) / 2;
  s_gb.bx = s_gb.cx + sq + sqg + (sq - run_w("B", s_f_label, false, 0)) / 2;
  s_gb.ty = s_gb.cy + (sq - m_chip.cap) / 2 - m_chip.bearing;
  s_gb.vx = mapx(x + s_gb.w - v_w, v_w);
  s_gb.vy = y + label_h + lgap - (s_gb.now ? m_chip.bearing + (m_chip.cap - m_val.cap) : m_val.bearing);
  s_gb.mx = s_gb.vx + min_w + sc(2);
  s_gb.my = y + label_h + lgap + m_val.cap - m_cap.cap - m_cap.bearing;
  s_gb.show = true;
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
    draw_run_s("MIN", s_f_cap, s_gb.mx, s_gb.my, false, TRACK);
  }
}

// ---- the quiet face: away from the hub, or outside its hours, there is
// nothing to count down to, so the block goes and the date grows into the
// room it leaves. Still on the wrist side: it is the part you already know.
#define IDLE_DOW_GAP 6
static struct { char dow[8], date[12]; int dow_y, date_y, top; Metrics m; } s_id;
static void layout_idle(const struct tm *t, const Frame *fr) __attribute__((noinline));
static void layout_idle(const struct tm *t, const Frame *fr) {
  strftime(s_id.dow, sizeof(s_id.dow), "%a", t);
  strftime(s_id.date, sizeof(s_id.date), "%d %b", t);
  for (char *p = s_id.dow; *p; p++) *p = toupper((int)*p);
  for (char *p = s_id.date; *p; p++) *p = toupper((int)*p);
  s_id.m = barlow_metrics(measure("22 SEP", s_f_bigdate).h);
  s_id.date_y = fr->bot - sc(DATE_PAD_BOT) - s_id.m.cap;
  s_id.dow_y = s_id.date_y - sc(IDLE_DOW_GAP) - s_id.m.cap;
  s_id.top = s_id.dow_y - sc(IDLE_DOW_GAP);   // where the band above ends
}
static void paint_idle(int fr_start) __attribute__((noinline));
static void paint_idle(int fr_start) {
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run(s_id.dow, s_f_bigdate, fr_start, s_id.dow_y - s_id.m.bearing, false, TRACK);
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run(s_id.date, s_f_bigdate, fr_start, s_id.date_y - s_id.m.bearing, false, TRACK);
}

// ---- the stop view: on a flick, the nearest stop and what leaves it next.
// It takes the whole of the face below the time for a few seconds: a line
// for the stop, then a row a departure — the route in its own colour, where
// it is going, and when. The rail stays; it is the one thing the design
// never covers.
#define SV_ROW_H 24
#define SV_HEAD_GAP 18
static struct {
  int top, head_y, dist_x, dist_w, n;
  char dist[10], note[20], stop[24];
  struct { int y, badge_x, badge_w, badge_h, text_x, text_y, head_x, head_y, when_x, when_y, unit_x, unit_y; char head[20]; } r[SV_ROWS];
  Metrics m_lab, m_val, m_cap;
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

static void layout_stopview(const Frame *fr, int band_top) __attribute__((noinline));
static void layout_stopview(const Frame *fr, int band_top) {
  s_svl.m_lab = barlow_metrics(measure("B", s_f_label).h);
  s_svl.m_val = barlow_metrics(measure("8", s_f_mod).h);
  s_svl.m_cap = barlow_metrics(measure("M", s_f_cap).h);
  s_svl.top = band_top;
  s_svl.head_y = band_top + sc(4) - s_svl.m_lab.bearing;
  s_svl.dist_w = 0;
  if (s_sv.dist > 60) {
    format_dist(s_svl.dist, sizeof(s_svl.dist), s_sv.dist);
    s_svl.dist_w = run_w(s_svl.dist, s_f_label, false, TRACK);
    s_svl.dist_x = fr->end - s_svl.dist_w;
  }
  // The stop's name gives way to the distance, a glyph at a time.
  strncpy(s_svl.stop, s_sv.stop, sizeof(s_svl.stop) - 1); s_svl.stop[sizeof(s_svl.stop) - 1] = 0;
  const int name_room = (s_svl.dist_w ? s_svl.dist_x - sc(6) : fr->end) - fr->start;
  while (s_svl.stop[0] && run_w(s_svl.stop, s_f_label, false, TRACK) > name_room) {
    char *e = s_svl.stop + strlen(s_svl.stop) - 1;
    while (e > s_svl.stop && ((unsigned char)*e & 0xC0) == 0x80) e--;
    *e = 0;
    while (e > s_svl.stop && e[-1] == ' ') *--e = 0;
  }
  s_svl.note[0] = 0;
  if (s_sv.n == 0) strncpy(s_svl.note, s_sv.stop[0] ? "NO MORE TODAY" : "NO STOPS NEARBY", sizeof(s_svl.note));
  s_svl.n = s_sv.n;
  int y = band_top + sc(SV_HEAD_GAP);
  for (int i = 0; i < s_sv.n; i++) {
    const SvRow *row = &s_sv.row[i];
    const int pad = sc(3);
    s_svl.r[i].y = y;
    s_svl.r[i].badge_w = run_w(row->route, s_f_mod, false, 0) + 2 * pad;
    s_svl.r[i].badge_h = s_svl.m_val.cap + 2 * sc(2);
    s_svl.r[i].badge_x = fr->start;
    s_svl.r[i].text_x = fr->start + pad;
    s_svl.r[i].text_y = y + sc(2) - s_svl.m_val.bearing;
    // when: minutes in the value font with a small MIN, or a clock time
    const int w_w = run_w(row->when, s_f_mod, row->mins, 0);
    const int u_w = row->mins ? sc(2) + run_w("MIN", s_f_cap, false, TRACK) : 0;
    s_svl.r[i].when_x = fr->end - w_w - u_w;
    s_svl.r[i].when_y = y + sc(2) - s_svl.m_val.bearing;
    s_svl.r[i].unit_x = fr->end - u_w + sc(2);
    s_svl.r[i].unit_y = y + sc(2) + s_svl.m_val.cap - s_svl.m_cap.cap - s_svl.m_cap.bearing;
    // the headsign takes what is left, a glyph at a time
    s_svl.r[i].head_x = fr->start + s_svl.r[i].badge_w + sc(4);
    s_svl.r[i].head_y = y + sc(2) + s_svl.m_val.cap - s_svl.m_lab.cap - s_svl.m_lab.bearing;
    const int room = s_svl.r[i].when_x - sc(4) - s_svl.r[i].head_x;
    strncpy(s_svl.r[i].head, row->head, sizeof(s_svl.r[i].head) - 1);
    s_svl.r[i].head[sizeof(s_svl.r[i].head) - 1] = 0;
    while (s_svl.r[i].head[0] && run_w(s_svl.r[i].head, s_f_label, false, TRACK) > room) {
      char *e = s_svl.r[i].head + strlen(s_svl.r[i].head) - 1;
      while (e > s_svl.r[i].head && ((unsigned char)*e & 0xC0) == 0x80) e--;   // a whole UTF-8 sequence
      *e = 0;
      while (e > s_svl.r[i].head && e[-1] == ' ') *--e = 0;
    }
    y += sc(SV_ROW_H);
  }
}

static void paint_stopview(int fr_start) __attribute__((noinline));
static void paint_stopview(int fr_start) {
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run(s_svl.stop, s_f_label, fr_start, s_svl.head_y, false, TRACK);
  if (s_svl.dist_w) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.dist, s_f_label, s_svl.dist_x, s_svl.head_y, false, TRACK);
  }
  if (s_svl.note[0]) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.note, s_f_label, fr_start, s_svl.top + sc(SV_HEAD_GAP) + sc(2) - s_svl.m_lab.bearing, false, TRACK);
  }
  for (int i = 0; i < s_svl.n; i++) {
    const SvRow *row = &s_sv.row[i];
    const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(row->color), s_ink);
    graphics_context_set_fill_color(s_ctx, fill);
    graphics_fill_rect(s_ctx, GRect(mapx(s_svl.r[i].badge_x, s_svl.r[i].badge_w), s_svl.r[i].y,
                                    s_svl.r[i].badge_w, s_svl.r[i].badge_h), sc(3), GCornersAll);
    graphics_context_set_text_color(s_ctx, on_fill(fill));
    draw_run(row->route, s_f_mod, s_svl.r[i].text_x, s_svl.r[i].text_y, false, 0);
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.r[i].head, s_f_label, s_svl.r[i].head_x, s_svl.r[i].head_y, false, TRACK);
    draw_run(row->when, s_f_mod, s_svl.r[i].when_x, s_svl.r[i].when_y, row->mins, 0);
    if (row->mins) {
      graphics_context_set_text_color(s_ctx, s_dim);
      draw_run("MIN", s_f_cap, s_svl.r[i].unit_x, s_svl.r[i].unit_y, false, TRACK);
    }
  }
}

// The content box, inside the rail and the paddings, in logical x.

// Each zone is laid out into static storage by one function and painted by
// another. The painters are what sit beneath the firmware's text renderer,
// and with their numbers in memory rather than in locals their frames stay
// small enough to leave it the stack it needs.

// ---- zone 03/04: the time, flush to the outer edge so the minute survives
// a cuff that hides the hour.
static struct {
  char hh[4], mm[4];
  int x, y, w, hh_w, colon_w, cgap, band_top;
  GSize z_colon;
} s_tm;

static void layout_time(const struct tm *t, const Frame *fr) __attribute__((noinline));
static void layout_time(const struct tm *t, const Frame *fr) {
  snprintf(s_tm.hh, sizeof(s_tm.hh), "%02d", display_hour(t->tm_hour));
  snprintf(s_tm.mm, sizeof(s_tm.mm), "%02d", t->tm_min);
  const GFont f = s_f_time;
  const Metrics m = barlow_metrics(measure("0", f).h);
  s_tm.z_colon = measure(":", f);
  s_tm.cgap = sc(COLON_GAP);
  s_tm.hh_w = run_w(s_tm.hh, f, true, 0);
  s_tm.colon_w = s_tm.z_colon.w + 2 * s_tm.cgap;
  s_tm.w = s_tm.hh_w + s_tm.colon_w + run_w(s_tm.mm, f, true, 0);
  s_tm.x = fr->end - s_tm.w;
  // The design's line box sits its digits well below the padding: the cap
  // top lands 18px down at this scale.
  s_tm.y = fr->top + sc(TIME_MARGIN_TOP) - m.bearing;
  // The band the design leaves empty runs from the time's line box down to
  // the top of the countdown block.
  s_tm.band_top = s_tm.y + m.bearing + m.cap + sc(3);
}

static void paint_time(void) __attribute__((noinline));
static void paint_time(void) {
  // The row is mirrored as a whole; hour, colon and minute keep their order.
  const int x = mapx(s_tm.x, s_tm.w);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_tm.hh, s_f_time, x, s_tm.y, true, 0);
  draw_run_s(s_tm.mm, s_f_time, x + s_tm.hh_w + s_tm.colon_w, s_tm.y, true, 0);
  // The one accent in the time: the design's colon.
  graphics_context_set_text_color(s_ctx, accent());
  s_tx.t = ":"; s_tx.f = s_f_time; s_tx.mode = GTextOverflowModeWordWrap;
  s_tx.r = GRect(x + s_tm.hh_w + s_tm.cgap, s_tm.y, s_tm.z_colon.w, s_tm.z_colon.h);
  tx_draw();
}

// ---- zone 02: the countdown, and zone 04: weekday and date.
static struct {
  char dow[8], date[12], label[20], num[8];
  const char *unit;
  bool now, solid;
  GSize z_num;
  Metrics m_lab, m_min, m_num, m_dow, m_date;
  int label_w, num_w, num_row_w, inner_w, inner_x, label_y, num_y;
  int block_x, block_y, block_w, block_h, date_y, dow_y;
} s_bk;

static void layout_block(const struct tm *t, const Schedule *sch, const Frame *fr) __attribute__((noinline));
static void layout_block(const struct tm *t, const Schedule *sch, const Frame *fr) {
  strftime(s_bk.dow, sizeof(s_bk.dow), "%a", t);
  strftime(s_bk.date, sizeof(s_bk.date), "%d %b", t);
  for (char *p = s_bk.dow; *p; p++) *p = toupper((int)*p);
  for (char *p = s_bk.date; *p; p++) *p = toupper((int)*p);

  s_bk.now = sch->is_now;
  s_bk.solid = sch->is_now || sch->is_boarding;
  s_bk.unit = "MIN";
  if (sch->is_now) {
    strncpy(s_bk.label, "DEPARTS", sizeof(s_bk.label));
    strncpy(s_bk.num, "NOW", sizeof(s_bk.num));
  } else {
    snprintf(s_bk.label, sizeof(s_bk.label), "%s %d:%02d",
             sch->is_boarding ? "LEAVES" : "NEXT",
             display_hour(sch->next_h), sch->next_m);
    snprintf(s_bk.num, sizeof(s_bk.num), "%d", sch->is_final ? sch->secs : sch->remaining);
    if (sch->is_final) s_bk.unit = "SEC";
  }

  // NOW is set in the countdown face itself, as the design draws it.
  const GFont f_label = s_f_label, f_date = s_f_date, f_num = s_f_count;
  s_bk.z_num = measure(s_bk.num, f_num);
  s_bk.label_w = run_w(s_bk.label, f_label, false, TRACK);
  const int min_w = sch->is_now ? 0 : run_w(s_bk.unit, f_label, false, TRACK);
  s_bk.m_lab = barlow_metrics(measure(s_bk.label, f_label).h);
  s_bk.m_min = barlow_metrics(sch->is_now ? 0 : measure(s_bk.unit, f_label).h);
  s_bk.m_num = barlow_metrics(s_bk.z_num.h);
  s_bk.m_dow = barlow_metrics(measure(s_bk.dow, f_date).h);
  s_bk.m_date = barlow_metrics(measure(s_bk.date, f_date).h);
  const int dow_w = run_w(s_bk.dow, f_date, false, TRACK);
  const int date_w2 = run_w(s_bk.date, f_date, false, TRACK);

  s_bk.num_w = sch->is_now ? s_bk.z_num.w : run_w(s_bk.num, f_num, true, 0);
  s_bk.num_row_w = s_bk.num_w + (sch->is_now ? 0 : sc(LABEL_GAP) + min_w);
  int inner_w = s_bk.num_row_w > s_bk.label_w ? s_bk.num_row_w : s_bk.label_w;
  const int min_inner = sc(BLOCK_MIN_W) - sc(BLOCK_PAD_IN) - sc(BLOCK_PAD_OUT);
  if (inner_w < min_inner) inner_w = min_inner;

  // The bottom row is a space-between pair and the block must not grow into
  // the weekday/date column. Should a label outgrow the row, the block's own
  // padding gives way first and the normal state keeps the design's spacing.
  const int date_w = dow_w > date_w2 ? dow_w : date_w2;
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
  s_bk.block_h = s_bk.m_lab.cap + sc(BLOCK_ROW_GAP) + s_bk.m_num.cap + sc(BLOCK_PAD_T) + sc(BLOCK_PAD_B);
  s_bk.block_x = fr->end - s_bk.block_w;
  s_bk.block_y = fr->bot - s_bk.block_h;
  // Label and number are end-aligned within the block. The design puts a
  // full line of air between them: measured with its own font, 15px from
  // the label's baseline to the top of the digits at 288 wide, so seven
  // here. The estimated cap of the label runs a pixel long, hence eight in
  // the constant.
  s_bk.inner_x = s_bk.block_x + pad_in;
  s_bk.label_y = s_bk.block_y + sc(BLOCK_PAD_T);
  s_bk.num_y = s_bk.label_y + s_bk.m_lab.cap + sc(BLOCK_ROW_GAP);
  // Weekday and date sit on the wrist side: first to disappear.
  s_bk.date_y = fr->bot - sc(DATE_PAD_BOT) - s_bk.m_date.cap;
  s_bk.dow_y = s_bk.date_y - sc(DOW_GAP) - s_bk.m_dow.cap;
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
  draw_run(s_bk.label, s_f_label, s_bk.inner_x + s_bk.inner_w - s_bk.label_w,
           s_bk.label_y - s_bk.m_lab.bearing, false, TRACK);
  const int num_x = s_bk.inner_x + s_bk.inner_w - s_bk.num_row_w;
  if (s_bk.now) {
    draw_at(s_bk.num, s_f_count, num_x, s_bk.num_y - s_bk.m_num.bearing, s_bk.z_num);
  } else {
    // One row, mirrored as a whole: the number, then MIN on its baseline.
    const int x = mapx(num_x, s_bk.num_row_w);
    draw_run_s(s_bk.num, s_f_count, x, s_bk.num_y - s_bk.m_num.bearing, true, 0);
    draw_run_s(s_bk.unit, s_f_label, x + s_bk.num_w + sc(LABEL_GAP),
               s_bk.num_y + s_bk.m_num.cap - s_bk.m_min.cap - s_bk.m_min.bearing, false, TRACK);
  }
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
  static bool quiet, at_hub;
  s_ctx = ctx;
  full = layer_get_bounds(layer);
  b = layer_get_unobstructed_bounds(layer);
  s_w = b.size.w;

  const time_t now = time(NULL);
  t = localtime(&now);
  sch = schedule_for(t->tm_hour, t->tm_min, t->tm_sec);
/*DEMO*/
  // With the hub known, the countdown is for the hub: at it in its hours the
  // face runs as ever, with the second countdown; anywhere else it is quiet.
  at_hub = transit_fresh(now) && s_tr.state == 1;
  quiet = transit_fresh(now) && s_tr.state == 0 && s_set.transit == TRANSIT_NEAR;

  theme_apply(t->tm_hour);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  if (quiet) draw_hairline(ctx, b.size.h);
  else draw_rail(ctx, &sch, b.size.h);

  fr.start = sc(PAD_WRIST);                                       // logical left
  fr.end = s_w - sc(RAIL_W) - sc(RAIL_BORDER) - sc(PAD_GUTTER);   // logical right
  fr.top = sc(PAD_TOP);
  fr.bot = b.size.h - sc(PAD_BOTTOM);

  layout_time(t, &fr);
  paint_time();
  if (s_sv.valid) {
    // The answer to a flick stands in for everything below the time.
    layout_stopview(&fr, s_tm.band_top);
    paint_stopview(fr.start);
  } else {
    if (quiet) layout_idle(t, &fr);
    else layout_block(t, &sch, &fr);
    const int band_bot = quiet ? s_id.top : s_bk.block_y;
    if (at_hub) layout_gb(fr.start, fr.end - fr.start, s_tm.band_top, band_bot, t->tm_hour * 60 + t->tm_min);
    else s_gb.show = false;
    layout_modules(s_f_mod, s_f_cap, fr.start, fr.end - fr.start - (s_gb.show ? s_gb.w + s_gb.gap : 0),
                   s_tm.band_top, band_bot);
    if (quiet) paint_idle(fr.start);
    else paint_block(fr.start);
    paint_modules();
    if (s_gb.show) paint_gb();
  }

  // ---- boarding buzz, once on the transition into the solid block.
  if (!quiet && s_set.buzz && sch.remaining == THRESHOLD && s_last_remaining != THRESHOLD
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
  if (s.is_final == s_ticking_seconds) return;
  tick_timer_service_unsubscribe();
  tick_timer_service_subscribe(s.is_final ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
  s_ticking_seconds = s.is_final;
}

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  retune_tick();
  layer_mark_dirty(s_face);
}

// A setting from the page arrives as the string it was chosen as, but a
// value can also come back as an int, and reading an int as a string gives
// whatever bytes it holds. Take either.
static int tuple_int(const Tuple *tp) {
  return tp->type == TUPLE_CSTRING ? atoi(tp->value->cstring) : (int)tp->value->int32;
}

static void stopview_done(void *data) {
  (void)data;
  s_sv.timer = NULL;
  s_sv.valid = false;
  s_sv.pending = false;
  layer_mark_dirty(s_face);
}
static void stopview_hold(uint32_t ms) {
  if (s_sv.timer) app_timer_reschedule(s_sv.timer, ms);
  else s_sv.timer = app_timer_register(ms, stopview_done, NULL);
}

// A flick of the wrist asks the phone for the nearest stop. The light comes
// on with the gesture, and again when the answer lands, so the answer is
// read in the same glance.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis; (void)direction;
  if (s_set.transit == TRANSIT_OFF || s_sv.pending) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_FLICK, 1);
  if (app_message_outbox_send() != APP_MSG_OK) return;
  s_sv.pending = true;
  light_enable_interaction();
  stopview_hold(SV_WAIT_MS);
}

static void take_row(DictionaryIterator *iter, int i, uint32_t kr, uint32_t kh, uint32_t kw, uint32_t kc, uint32_t kt) {
  Tuple *tr = dict_find(iter, kr), *th = dict_find(iter, kh), *tw = dict_find(iter, kw);
  Tuple *tc = dict_find(iter, kc), *tt = dict_find(iter, kt);
  SvRow *row = &s_sv.row[i];
  strncpy(row->route, tr ? tr->value->cstring : "", sizeof(row->route) - 1); row->route[sizeof(row->route) - 1] = 0;
  strncpy(row->head, th ? th->value->cstring : "", sizeof(row->head) - 1); row->head[sizeof(row->head) - 1] = 0;
  strncpy(row->when, tw ? tw->value->cstring : "", sizeof(row->when) - 1); row->when[sizeof(row->when) - 1] = 0;
  row->color = tc ? (uint32_t)tc->value->int32 & 0xFFFFFF : 0x888888;
  row->mins = tt ? tt->value->int32 != 0 : true;
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *tp;
  if ((tp = dict_find(iter, MESSAGE_KEY_SV_N))) {
    Tuple *ts = dict_find(iter, MESSAGE_KEY_SV_STOP);
    Tuple *td = dict_find(iter, MESSAGE_KEY_SV_DIST);
    s_sv.n = (int)tp->value->int32;
    if (s_sv.n > SV_ROWS) s_sv.n = SV_ROWS;
    if (s_sv.n < 0) s_sv.n = 0;
    strncpy(s_sv.stop, ts ? ts->value->cstring : "", sizeof(s_sv.stop) - 1); s_sv.stop[sizeof(s_sv.stop) - 1] = 0;
    s_sv.dist = td ? (int)td->value->int32 : 0;
    take_row(iter, 0, MESSAGE_KEY_SV_R1, MESSAGE_KEY_SV_H1, MESSAGE_KEY_SV_W1, MESSAGE_KEY_SV_C1, MESSAGE_KEY_SV_T1);
    take_row(iter, 1, MESSAGE_KEY_SV_R2, MESSAGE_KEY_SV_H2, MESSAGE_KEY_SV_W2, MESSAGE_KEY_SV_C2, MESSAGE_KEY_SV_T2);
    take_row(iter, 2, MESSAGE_KEY_SV_R3, MESSAGE_KEY_SV_H3, MESSAGE_KEY_SV_W3, MESSAGE_KEY_SV_C3, MESSAGE_KEY_SV_T3);
    s_sv.valid = true;
    s_sv.pending = false;
    light_enable_interaction();
    stopview_hold(SV_SHOW_MS);
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
  if ((tp = dict_find(iter, MESSAGE_KEY_BUZZ))) {
    s_set.buzz = tp->value->int32 != 0;
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
    s_set.mod_icons = tp->value->int32 != 0;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_SECONDS))) {
    s_set.final_seconds = tp->value->int32 != 0;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_UNITS))) {
    const bool imperial = (strcmp(tp->value->cstring, "imperial") == 0);
    // The stored reading is in the old unit. Showing it under the new label
    // would be wrong by 30-odd degrees, so drop it until the next fetch.
    if (imperial != s_set.imperial) s_wx.valid = false;
    s_set.imperial = imperial;
  }

  if ((tp = dict_find(iter, MESSAGE_KEY_TRANSIT))) {
    s_set.transit = (uint8_t)tuple_int(tp);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_TR_RADIUS))) {
    s_set.radius = (uint16_t)tp->value->int32;
  }
  // The hub's word arrives on the same channel, pushed by the JS side.
  if ((tp = dict_find(iter, MESSAGE_KEY_TR_STATE))) {
    Tuple *tg = dict_find(iter, MESSAGE_KEY_TR_G);
    Tuple *tb = dict_find(iter, MESSAGE_KEY_TR_B);
    Tuple *ta = dict_find(iter, MESSAGE_KEY_TR_AT);
    s_tr.state = (uint8_t)tp->value->int32;
    for (int i = 0; i < TR_MAX; i++) {
      s_tr.g[i] = (tg && tg->length >= 2 * TR_MAX) ? (uint16_t)(tg->value->data[2 * i] | (tg->value->data[2 * i + 1] << 8)) : TR_NONE;
      s_tr.b[i] = (tb && tb->length >= 2 * TR_MAX) ? (uint16_t)(tb->value->data[2 * i] | (tb->value->data[2 * i + 1] << 8)) : TR_NONE;
    }
    s_tr.at = ta ? (time_t)ta->value->int32 : time(NULL);
    transit_save();
  }

  // Weather arrives on the same channel, pushed by the JS side.
  if ((tp = dict_find(iter, MESSAGE_KEY_WOK))) {
    Tuple *tt = dict_find(iter, MESSAGE_KEY_TEMP);
    Tuple *tc = dict_find(iter, MESSAGE_KEY_WCODE);
    if (tp->value->int32 && tt) {
      s_wx.temp = (int16_t)tt->value->int32;
      s_wx.code = tc ? (uint8_t)tc->value->int32 : 0;
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
  s_f_time = fonts_load_custom_font(resource_get_handle(RES_TIME));
  s_f_count = fonts_load_custom_font(resource_get_handle(RES_COUNT));
  s_f_mod = fonts_load_custom_font(resource_get_handle(RES_MOD));
  s_f_label = fonts_load_custom_font(resource_get_handle(RES_LABEL));
  s_f_date = fonts_load_custom_font(resource_get_handle(RES_DATE));
  s_f_cap = fonts_load_custom_font(resource_get_handle(RES_CAP));
  s_f_bigdate = fonts_load_custom_font(resource_get_handle(RES_BIGDATE));

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
}

static void deinit(void) {
  fonts_unload_custom_font(s_f_time);
  fonts_unload_custom_font(s_f_count);
  fonts_unload_custom_font(s_f_mod);
  fonts_unload_custom_font(s_f_label);
  fonts_unload_custom_font(s_f_date);
  fonts_unload_custom_font(s_f_cap);
  fonts_unload_custom_font(s_f_bigdate);
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

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
  bool buzz;           // single pulse when the boarding block goes solid
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

#define SETTINGS_KEY 1
#define SETTINGS_VERSION 4
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
  uint8_t area;            // 1 inside a system the face knows
  uint16_t g[TR_MAX], b[TR_MAX];
  time_t at;
} Transit;

// The stop view: what the phone answered the last flick with. Shown for a
// few seconds, then the face is itself again.
#define SV_ROWS 3
#define SV_SHOW_MS 12000
#define SV_WAIT_MS 15000
typedef struct { char route[8], head[20], when[24]; uint32_t color; bool mins; } SvRow;
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
  s_set.radius = 300;
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
      // An older layout: the same fields up to where new ones were appended.
      // Carry it over rather than hand the wearer the defaults again.
      persist_read_data(SETTINGS_KEY, &stored, n);
      if (stored.version >= 1 && stored.version <= SETTINGS_VERSION) s_set = stored;
      // Before layout 4 the hub was off unless chosen; now it is automatic
      // unless chosen, and an unchosen off reads as automatic.
      if (stored.version < 4 && s_set.transit == TRANSIT_OFF) s_set.transit = TRANSIT_AUTO;
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
  return s_set.mod_icons != MOD_ICONS_OFF;
}

static int module_icon_w(const Module *m, int icon) {
  switch (m->kind) {
    case MODULE_BATTERY: return icon * 8 / 5;
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
    case MODULE_BATTERY: return GColorFromHEX(m->extra > 50 ? 0x00AA55 : m->extra > 20 ? 0xFFAA00 : 0xFF0000);
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
    case MODULE_BATTERY: icon_battery(ctx, box, m->extra, tint); break;
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
  for (int i = 0; i < MODULE_COUNT; i++) {
    uint8_t kind = s_set.mod[i];
    if (kind == MODULE_NONE) continue;
    if (module_read(kind, &s_md.m[s_md.n])) { s_md.n++; continue; }
    // No heart-rate sensor, or no reading yet: the slot shows the battery,
    // or steps if the battery already has a slot of its own.
    if (kind != MODULE_HR) continue;
    kind = MODULE_BATTERY;
    for (int j = 0; j < MODULE_COUNT; j++) if (s_set.mod[j] == MODULE_BATTERY) kind = MODULE_STEPS;
    if (module_read(kind, &s_md.m[s_md.n])) s_md.n++;
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
  bool now;
} s_gb;

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
  const Metrics m_at = barlow_metrics(measure("8", s_f_cap).h);
  const int at_w = s_gb.at[0] ? run_w(s_gb.at, s_f_cap, false, TRACK) : 0;
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
  s_gb.ax = rx;
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
static void sv_clock(char *out, size_t n, const char *tok) {
  const int m = atoi(tok), h = (m / 60) % 24, mm = m % 60;
  if (use_24h()) snprintf(out, n, "%d:%02d", h, mm);
  else snprintf(out, n, "%d:%02d%c", display_hour(h), mm, h < 12 ? 'A' : 'P');
}

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

// The board, as the design draws it: hung from the outer edge, a line for
// the stop, then a row a route — its badge and the next clock times, two
// where they fit, or the next time and its day when today's are done. It
// takes the whole of the room below the time and sits centred in it.
#define SV_BADGE_GAP 5     // badge to the first time
#define SV_TIME_GAP  7     // between the times
// The badges are the chips' size, with the chips' glyph: one badge on the face.
static void layout_stopview(const struct tm *t, const Frame *fr, int band_top) __attribute__((noinline));
static void layout_stopview(const struct tm *t, const Frame *fr, int band_top) {
  // The flick may have been for the date: a small one keeps the foot.
  strftime(s_svl.date, sizeof(s_svl.date), "%a %d %b", t);
  for (char *c = s_svl.date; *c; c++) *c = toupper((int)*c);
  s_svl.m_date = barlow_metrics(measure(s_svl.date, s_f_date).h);
  s_svl.date_y = fr->bot - sc(DATE_PAD_BOT) - s_svl.m_date.cap;
  s_svl.date_x = fr->start;
  const int band_bot = s_svl.date_y - sc(4);
  // The stop line and the badges in the caption font, the times in the
  // board font: the board is a small thing under a full-size time.
  const GFont f_line = s_f_cap, f_time = s_f_board, f_badge = s_f_label;
  s_svl.m_lab = barlow_metrics(measure("B", f_line).h);
  s_svl.m_val = barlow_metrics(measure("8", f_time).h);
  s_svl.m_bad = barlow_metrics(measure("8", f_badge).h);
  s_svl.dist_w = 0;
  if (s_sv.dist > 60) {
    format_dist(s_svl.dist, sizeof(s_svl.dist), s_sv.dist);
    s_svl.dist_w = run_w(s_svl.dist, f_line, false, TRACK);
  }
  // The stop's name gives way to the distance, a glyph at a time.
  strncpy(s_svl.stop, s_sv.stop, sizeof(s_svl.stop) - 1); s_svl.stop[sizeof(s_svl.stop) - 1] = 0;
  const int name_room = fr->end - fr->start - (s_svl.dist_w ? s_svl.dist_w + sc(6) : 0);
  while (s_svl.stop[0] && run_w(s_svl.stop, f_line, false, TRACK) > name_room) {
    char *e = s_svl.stop + strlen(s_svl.stop) - 1;
    while (e > s_svl.stop && ((unsigned char)*e & 0xC0) == 0x80) e--;
    *e = 0;
    while (e > s_svl.stop && e[-1] == ' ') *--e = 0;
  }
  s_svl.stop_x = fr->end - run_w(s_svl.stop, f_line, false, TRACK);
  s_svl.note[0] = 0;
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
    const int w1 = run_w(s_svl.r[i].t1, f_time, false, 0);
    const int w2 = s_svl.r[i].t2[0] ? run_w(s_svl.r[i].t2, s_svl.r[i].t2_day ? f_line : f_time, false, s_svl.r[i].t2_day ? TRACK : 0) : 0;
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

static void paint_stopview(int fr_start) __attribute__((noinline));
static void paint_stopview(int fr_start) {
  graphics_context_set_text_color(s_ctx, light_theme() ? s_ink : s_dim);
  draw_run(s_svl.stop, s_f_cap, s_svl.stop_x, s_svl.head_y, false, TRACK);
  if (light_theme()) {
    const int w = run_w(s_svl.stop, s_f_cap, false, TRACK);
    draw_stop_rule(mapx(s_svl.stop_x, w), mapx(s_svl.stop_x, w) + w, s_svl.rule_y);
  }
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run(s_svl.date, s_f_date, s_svl.date_x, s_svl.date_y - s_svl.m_date.bearing, false, TRACK);
  if (s_svl.dist_w) {
    graphics_context_set_text_color(s_ctx, s_ink);
    draw_run(s_svl.dist, s_f_cap, fr_start, s_svl.head_y, false, TRACK);
  }
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
    draw_run_s(s_svl.r[i].t1, s_f_board, rx + s_svl.r[i].t1_dx, s_svl.r[i].text_y, false, 0);
    if (s_svl.r[i].t2_day) {
      graphics_context_set_text_color(s_ctx, s_dim);
      draw_run_s(s_svl.r[i].t2, s_f_cap, rx + s_svl.r[i].t2_dx, s_svl.r[i].t2_y, false, TRACK);
    } else if (s_svl.r[i].t2[0]) {
      draw_run_s(s_svl.r[i].t2, s_f_board, rx + s_svl.r[i].t2_dx, s_svl.r[i].text_y, false, 0);
    }
  }
}

// ---- the side block: one row never takes the face. The answer sits in the
// band beside the modules, three short lines hung from one edge: the stop,
// the badge and its time, then the distance, or the row's second column
// when stood at the stop. Date and modules stay; twelve seconds later the
// block is gone. Right-aligned to `right` in logical x; `left` is where the
// modules end on that side, so the block can decline if it would overlap.
#define SB_GAP 3
static struct {
  bool show, q_day;
  int stop_x, stop_y, rule_y, row_x, row_w, badge_w, badge_h, glyph_dx, glyph_y, t_dx, t_y, q_x, q_y;
  char stop[24], t1[8], q[10];
  Metrics m_lab, m_val, m_bad;
} s_sb;

static void layout_sideblock(const Frame *fr, int band_top, int band_bot, int left, int right) __attribute__((noinline));
static void layout_sideblock(const Frame *fr, int band_top, int band_bot, int left, int right) {
  s_sb.show = false;
  if (s_sv.n != 1) return;
  const SvRow *row = &s_sv.row[0];
  s_sb.m_lab = barlow_metrics(measure("B", s_f_cap).h);
  s_sb.m_val = barlow_metrics(measure("8", s_f_board).h);
  s_sb.m_bad = barlow_metrics(measure("8", s_f_label).h);
  const int room = right - left - sc(8);
  // The stop's name, a glyph at a time down to the room there is.
  strncpy(s_sb.stop, s_sv.stop, sizeof(s_sb.stop) - 1); s_sb.stop[sizeof(s_sb.stop) - 1] = 0;
  while (s_sb.stop[0] && run_w(s_sb.stop, s_f_cap, false, TRACK) > room) {
    char *e = s_sb.stop + strlen(s_sb.stop) - 1;
    while (e > s_sb.stop && ((unsigned char)*e & 0xC0) == 0x80) e--;
    *e = 0;
    while (e > s_sb.stop && e[-1] == ' ') *--e = 0;
  }
  // The row: badge and the next time, as on the board.
  const char *sp = strchr(row->when, ' ');
  sv_clock(s_sb.t1, sizeof(s_sb.t1), row->when);
  const int pad = sc(2), badge_h = sc(CHIP);
  const int gw = run_w(row->route, s_f_label, false, 0);
  s_sb.badge_w = gw + 2 * pad < badge_h ? badge_h : gw + 2 * pad + 1;
  s_sb.badge_h = badge_h;
  s_sb.glyph_dx = (s_sb.badge_w - gw + 1) / 2;
  s_sb.t_dx = s_sb.badge_w + sc(SV_BADGE_GAP);
  s_sb.row_w = s_sb.t_dx + run_w(s_sb.t1, s_f_board, false, 0);
  // The qualifier: how far, or, stood at the stop, the row's second column.
  s_sb.q[0] = 0; s_sb.q_day = false;
  if (s_sv.dist > 60) { format_dist(s_sb.q, sizeof(s_sb.q), s_sv.dist); s_sb.q_day = true; }
  else if (sp && isdigit((int)sp[1])) sv_clock(s_sb.q, sizeof(s_sb.q), sp + 1);
  else if (sp && sp[1]) { strncpy(s_sb.q, sp + 1, sizeof(s_sb.q) - 1); s_sb.q[sizeof(s_sb.q) - 1] = 0; s_sb.q_day = true; }
  const int q_w = s_sb.q[0] ? run_w(s_sb.q, s_sb.q_day ? s_f_cap : s_f_board, false, s_sb.q_day ? TRACK : 0) : 0;
  if (s_sb.row_w > room || q_w > room) return;   // no room beside the modules: the board it is
  // Three lines, centred in the band.
  const int h = s_sb.m_lab.cap + sc(SB_GAP) + badge_h + (s_sb.q[0] ? sc(SB_GAP) + (s_sb.q_day ? s_sb.m_lab.cap : s_sb.m_val.cap) : 0);
  if (h > band_bot - band_top) return;
  const int top = band_top + (band_bot - band_top - h) / 2;
  s_sb.stop_x = right - run_w(s_sb.stop, s_f_cap, false, TRACK);
  s_sb.stop_y = top - s_sb.m_lab.bearing;
  s_sb.rule_y = top + s_sb.m_lab.cap + sc(2);
  const int by = top + s_sb.m_lab.cap + sc(SB_GAP);
  s_sb.row_x = right - s_sb.row_w;
  s_sb.glyph_y = by + (badge_h - s_sb.m_bad.cap) / 2 - s_sb.m_bad.bearing;
  s_sb.t_y = by + (badge_h + s_sb.m_bad.cap) / 2 - s_sb.m_val.cap - s_sb.m_val.bearing;
  s_sb.q_x = right - q_w;
  s_sb.q_y = by + badge_h + sc(SB_GAP) - (s_sb.q_day ? s_sb.m_lab.bearing : s_sb.m_val.bearing);
  s_sb.show = true;
}

static void paint_sideblock(void) __attribute__((noinline));
static void paint_sideblock(void) {
  const SvRow *row = &s_sv.row[0];
  graphics_context_set_text_color(s_ctx, light_theme() ? s_ink : s_dim);
  draw_run(s_sb.stop, s_f_cap, s_sb.stop_x, s_sb.stop_y, false, TRACK);
  if (light_theme()) {
    const int w = run_w(s_sb.stop, s_f_cap, false, TRACK);
    draw_stop_rule(mapx(s_sb.stop_x, w), mapx(s_sb.stop_x, w) + w, s_sb.rule_y);
  }
  const int rx = mapx(s_sb.row_x, s_sb.row_w);
  const GColor fill = PBL_IF_COLOR_ELSE(GColorFromHEX(row->color), s_ink);
  graphics_context_set_fill_color(s_ctx, fill);
  graphics_fill_rect(s_ctx, GRect(rx, s_sb.glyph_y + s_sb.m_bad.bearing - (s_sb.badge_h - s_sb.m_bad.cap) / 2, s_sb.badge_w, s_sb.badge_h), sc(2), GCornersAll);
  graphics_context_set_text_color(s_ctx, on_fill(fill));
  draw_run_s(row->route, s_f_label, rx + s_sb.glyph_dx, s_sb.glyph_y, false, 0);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_sb.t1, s_f_board, rx + s_sb.t_dx, s_sb.t_y, false, 0);
  if (s_sb.q[0]) {
    graphics_context_set_text_color(s_ctx, s_sb.q_day ? s_dim : s_ink);
    draw_run(s_sb.q, s_sb.q_day ? s_f_cap : s_f_board, s_sb.q_x, s_sb.q_y, false, s_sb.q_day ? TRACK : 0);
  }
}

// ---- the seconds through a flick: two small dim digits just beneath the
// time, at its outer edge, where a sleeve uncovers them first. A board read
// against a timetable wants to know where in the minute it is.
static struct { bool show; char t[3]; int x, y, h; } s_sec;
static void layout_secs(const struct tm *t, const Frame *fr, int band_top) __attribute__((noinline));
static void layout_secs(const struct tm *t, const Frame *fr, int band_top) {
  const Metrics m = barlow_metrics(measure("8", s_f_cap).h);
  snprintf(s_sec.t, sizeof(s_sec.t), "%02d", t->tm_sec);
  s_sec.x = fr->end - run_w(s_sec.t, s_f_cap, false, TRACK);
  s_sec.y = band_top - m.bearing;
  s_sec.h = m.cap + sc(3);   // what the band beneath gives up
  s_sec.show = true;
}
static void paint_secs(void) __attribute__((noinline));
static void paint_secs(void) {
  graphics_context_set_text_color(s_ctx, s_dim);
  draw_run(s_sec.t, s_f_cap, s_sec.x, s_sec.y, false, TRACK);
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
  GFont f;
} s_tm;

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
}

static void paint_time(void) __attribute__((noinline));
static void paint_time(void) {
  // The row is mirrored as a whole; hour, colon and minute keep their order.
  const int x = mapx(s_tm.x, s_tm.w);
  graphics_context_set_text_color(s_ctx, s_ink);
  draw_run_s(s_tm.hh, s_tm.f, x, s_tm.y, true, 0);
  draw_run_s(s_tm.mm, s_tm.f, x + s_tm.hh_w + s_tm.colon_w, s_tm.y, true, 0);
  // The one accent in the time: the design's colon.
  graphics_context_set_text_color(s_ctx, accent());
  s_tx.t = ":"; s_tx.f = s_tm.f; s_tx.mode = GTextOverflowModeWordWrap;
  s_tx.r = GRect(x + s_tm.hh_w + s_tm.cgap, s_tm.y, s_tm.z_colon.w, s_tm.z_colon.h);
  tx_draw();
}

// ---- zone 02: the countdown, and zone 04: weekday and date.
static struct {
  char dow[8], date[12], label[20], num[8];
  const char *unit;
  bool now, solid, bare;
  GSize z_num;
  Metrics m_lab, m_min, m_num, m_dow, m_date;
  int label_w, num_w, num_row_w, inner_w, inner_x, label_y, num_y;
  int block_x, block_y, block_w, block_h, date_y, dow_y;
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
  const GFont f_label = s_f_label, f_date = s_f_date;
  s_bk.f_num = f_num; s_bk.show_date = date;
  s_bk.z_num = measure(s_bk.num, f_num);
  // The block is the number and its unit; the departure's clock time is
  // the chips' to say, and the face at the hub is busy enough.
  s_bk.label[0] = 0;
  s_bk.label_w = 0;
  const bool bare = sch->is_now;   // NOW carries no unit
  const int min_w = bare ? 0 : run_w(s_bk.unit, f_label, false, TRACK);
  s_bk.m_lab = barlow_metrics(0);
  s_bk.m_min = barlow_metrics(bare ? 0 : measure(s_bk.unit, f_label).h);
  s_bk.m_num = barlow_metrics(s_bk.z_num.h);
  s_bk.m_dow = barlow_metrics(measure(s_bk.dow, f_date).h);
  s_bk.m_date = barlow_metrics(measure(s_bk.date, f_date).h);
  const int dow_w = run_w(s_bk.dow, f_date, false, TRACK);
  const int date_w2 = run_w(s_bk.date, f_date, false, TRACK);

  s_bk.num_w = sch->is_now ? s_bk.z_num.w : run_w(s_bk.num, f_num, true, 0);
  s_bk.num_row_w = s_bk.num_w + (bare ? 0 : sc(LABEL_GAP) + min_w);
  s_bk.bare = bare;
  int inner_w = s_bk.num_row_w > s_bk.label_w ? s_bk.num_row_w : s_bk.label_w;
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
  s_bk.label_y = s_bk.block_y + sc(BLOCK_PAD_T);
  s_bk.num_y = s_bk.label_y;
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
  static bool quiet, at_hub, peek;
  s_ctx = ctx;
  full = layer_get_bounds(layer);
  b = layer_get_unobstructed_bounds(layer);
  s_w = b.size.w;
  peek = b.size.h < full.size.h - sc(8);

  const time_t now = time(NULL);
  t = localtime(&now);
  sch = schedule_for(t->tm_hour, t->tm_min, t->tm_sec);
/*DEMO*/
  // With the hub known, the countdown is for the hub: at it in its hours the
  // face runs as ever, with the second countdown; anywhere else it is quiet.
  at_hub = transit_fresh(now) && s_tr.state == 1;
  // The countdown is earned by being at the hub. Anywhere else the face is
  // a plain watch — unless the countdown is asked for everywhere.
  quiet = !at_hub && s_set.transit != TRANSIT_CHIPS;

  theme_apply(t->tm_hour);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  if (quiet) draw_hairline(ctx, b.size.h);
  else draw_rail(ctx, &sch, b.size.h);

  fr.start = sc(PAD_WRIST);                                       // logical left
  fr.end = s_w - sc(RAIL_W) - sc(RAIL_BORDER) - sc(PAD_GUTTER);   // logical right
  fr.top = sc(PAD_TOP);
  fr.bot = b.size.h - sc(PAD_BOTTOM);

  // The answer to a flick: the time a size smaller, the stop and its rows
  // in the room that makes, and the countdown still at the foot, the largest
  // number on the face. Nothing is lost to a flick, least of all a bus
  // boarding. Date and modules sit it out.
  // The answer to a flick is counted in rows, not metres. One row sits in
  // the band beside the modules and the face keeps everything else. Two or
  // three take the board: the time full size, the countdown stepped aside,
  // a small date at the foot. Laid out first, so a one-row answer with no
  // room beside the modules can still take the board.
  s_sb.show = false;
  s_sec.show = false;
  if (!s_sv.valid || s_sv.n == 1) {
    // A timeline peek covers the bottom third. The date goes first, the time
    // and the countdown step down, and the modules and the second countdown
    // keep their band, as the design reflows it.
    layout_time(t, &fr, peek ? s_f_time_p : s_f_time, peek ? TIME_MARGIN_TOP_P : TIME_MARGIN_TOP);
    if (quiet) layout_idle(t, &fr);
    else if (peek) layout_block(t, &sch, &fr, s_f_count_s, BLOCK_ROW_GAP_P, false);
    else layout_block(t, &sch, &fr, s_f_count, BLOCK_ROW_GAP, true);
    const int band_bot = quiet ? s_id.top : s_bk.block_y;
    if (at_hub) layout_gb(fr.start, fr.end - fr.start, s_tm.band_top, band_bot, t->tm_hour * 60 + t->tm_min, t->tm_sec);
    else s_gb.show = false;
    layout_modules(s_f_mod, s_f_cap, fr.start, fr.end - fr.start - (s_gb.show ? s_gb.w + s_gb.gap : 0),
                   s_tm.band_top, band_bot);
    // The quiet face has nothing at the outer end: the modules go there too,
    // in their order, out from under the sleeve.
    if (quiet && s_md.n) s_md.x0 = fr.end - s_md.total;
    if (s_sv.valid && !s_gb.show) {
      // Beside the modules: on the quiet face to their wrist side, on the
      // countdown face at the outer end past them.
      if (quiet) layout_sideblock(&fr, s_tm.band_top, band_bot, fr.start - sc(8), s_md.n ? s_md.x0 - sc(8) : fr.end);
      else layout_sideblock(&fr, s_tm.band_top, band_bot, s_md.n ? s_md.x0 + s_md.total : fr.start - sc(8), fr.end);
      if (s_sb.show) layout_secs(t, &fr, s_tm.band_top);
    }
  }
  if (s_sv.valid && !s_sb.show) {
    layout_time(t, &fr, s_f_time, TIME_MARGIN_TOP);
    paint_time();
    layout_secs(t, &fr, s_tm.band_top);
    layout_stopview(t, &fr, s_tm.band_top + s_sec.h);
    paint_stopview(fr.start);
    paint_secs();
  } else {
    paint_time();
    if (quiet) paint_idle();
    else paint_block(fr.start);
    paint_modules();
    if (s_gb.show) paint_gb();
    if (s_sb.show) paint_sideblock();
    if (s_sec.show) paint_secs();
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
  const int now_min = t->tm_hour * 60 + t->tm_min;
  const bool gb_final = s_set.final_seconds && transit_fresh(now) && s_tr.state == 1
      && (transit_next(s_tr.g, now_min) == 1 || transit_next(s_tr.b, now_min) == 1);
  const bool want = s.is_final || s_sv.valid || gb_final;
  if (want == s_ticking_seconds) return;
  tick_timer_service_unsubscribe();
  tick_timer_service_subscribe(want ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
  s_ticking_seconds = want;
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
  retune_tick();
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
  if (transit_mode() == TRANSIT_OFF || !s_set.flick || s_sv.pending) return;
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
    s_sv.pending = false;
    if (s_sv.n == 0 && !s_sv.stop[0]) {
      // No stop near enough to speak of: the flick was for the light, and
      // the face stays as it was. Nothing is taken away to say nothing.
      if (s_sv.timer) { app_timer_cancel(s_sv.timer); s_sv.timer = NULL; }
      s_sv.valid = false;
      layer_mark_dirty(s_face);
      return;
    }
    s_sv.valid = true;
    light_enable_interaction();
    stopview_hold(SV_SHOW_MS);
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
    s_set.mod_icons = (uint8_t)tuple_int(tp);
    if (s_set.mod_icons > MOD_ICONS_COLOUR) s_set.mod_icons = MOD_ICONS_ON;
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
  if ((tp = dict_find(iter, MESSAGE_KEY_FLICK_ON))) {
    s_set.flick = tp->value->int32 != 0;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_TR_RADIUS))) {
    s_set.radius = (uint16_t)tp->value->int32;
  }
  // The hub's word arrives on the same channel, pushed by the JS side.
  if ((tp = dict_find(iter, MESSAGE_KEY_TR_STATE))) {
    Tuple *tg = dict_find(iter, MESSAGE_KEY_TR_G);
    Tuple *tb = dict_find(iter, MESSAGE_KEY_TR_B);
    Tuple *ta = dict_find(iter, MESSAGE_KEY_TR_AT);
    Tuple *tarea = dict_find(iter, MESSAGE_KEY_TR_AREA);
    s_tr.state = (uint8_t)tp->value->int32;
    s_tr.area = tarea ? (uint8_t)(tarea->value->int32 != 0) : 1;
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
  s_f_board = fonts_load_custom_font(resource_get_handle(RES_BOARD));
  s_f_cap = fonts_load_custom_font(resource_get_handle(RES_CAP));
  s_f_bigdate = fonts_load_custom_font(resource_get_handle(RES_BIGDATE));
  s_f_count_s = fonts_load_custom_font(resource_get_handle(RES_COUNT_S));
  s_f_time_p = fonts_load_custom_font(resource_get_handle(RES_TIME_P));

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
  fonts_unload_custom_font(s_f_board);
  fonts_unload_custom_font(s_f_cap);
  fonts_unload_custom_font(s_f_bigdate);
  fonts_unload_custom_font(s_f_count_s);
  fonts_unload_custom_font(s_f_time_p);
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

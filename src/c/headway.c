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
} Settings;

#define SETTINGS_KEY 1
#define SETTINGS_VERSION 1
#define WEATHER_KEY  2
#define THRESHOLD 5   // minutes; block goes solid at or under this

typedef struct {
  int16_t temp;
  uint8_t code;
  bool valid;
  time_t at;
} Weather;

static Settings s_set;
static Weather s_wx;

static void settings_defaults(void) {
  s_set.version = SETTINGS_VERSION;
  s_set.wrist_right = false;
  s_set.offset = 0;
  s_set.headway = 30;
  s_set.buzz = false;
  s_set.time_fmt = TIME_FMT_SYSTEM;
  s_set.theme = THEME_DARK;
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
}

static void settings_clamp(void) {
  if (s_set.headway != 30 && s_set.headway != 20 && s_set.headway != 15) {
    s_set.headway = 30;
  }
  if (s_set.offset >= s_set.headway) s_set.offset %= s_set.headway;
  if (s_set.time_fmt > TIME_FMT_24H) s_set.time_fmt = TIME_FMT_SYSTEM;
  if (s_set.theme > THEME_AUTO) s_set.theme = THEME_DARK;
  if (s_set.night_start > 23) s_set.night_start = 19;
  if (s_set.night_end > 23) s_set.night_end = 7;
  s_set.accent &= 0xFFFFFF;
  for (int i = 0; i < MODULE_COUNT; i++) {
    if (s_set.mod[i] > MODULE_WEATHER) s_set.mod[i] = MODULE_NONE;
  }
}

static void settings_load(void) {
  settings_defaults();
  // Only adopt a stored blob written by this layout. Size alone will not do
  // it: a new bool lands in the struct's existing padding, so the size is
  // unchanged while the byte it reads is whatever the old build left there —
  // which reads back as a setting silently stuck off.
  if (persist_exists(SETTINGS_KEY)
      && persist_get_size(SETTINGS_KEY) == (int)sizeof(s_set)) {
    Settings stored;
    persist_read_data(SETTINGS_KEY, &stored, sizeof(stored));
    if (stored.version == SETTINGS_VERSION) s_set = stored;
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
#else
  #define RES_TIME  RESOURCE_ID_FONT_TIME_60
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_44
  #define RES_MOD   RESOURCE_ID_FONT_MOD_15
  #define RES_LABEL RESOURCE_ID_FONT_LABEL_11
  #define RES_DATE  RESOURCE_ID_FONT_DATE_12
  #define RES_CAP   RESOURCE_ID_FONT_CAP_9
#endif

static GFont s_f_time, s_f_count, s_f_mod, s_f_label, s_f_date, s_f_cap;
static Window *s_window;
static Layer *s_face;
static int s_last_remaining = -1;
static int16_t s_w = REF_W;   // display width, for scaling

static int sc(int v) { return (v * s_w + REF_W / 2) / REF_W; }

// x of a logical box, measured from the wrist edge, mapped to the screen.
static int16_t mapx(int lx, int w) {
  return s_set.wrist_right ? (int16_t)(s_w - lx - w) : (int16_t)lx;
}

static GSize measure(const char *t, GFont f) {
  return graphics_text_layout_get_content_size(
      t, f, GRect(0, 0, 400, 200), GTextOverflowModeWordWrap, GTextAlignmentLeft);
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

// Draw text with its measured box placed at a logical origin.
static void draw_at(GContext *ctx, const char *t, GFont f, int lx, int y, GSize sz) {
  graphics_draw_text(ctx, t, f, GRect(mapx(lx, sz.w), y, sz.w, sz.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
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

static int run_w(const char *t, GFont f, bool tabular, int track) {
  const int cell = tabular ? measure("0", f).w : 0;
  int w = 0;
  for (const char *p = t; *p;) {
    char one[5];
    p += glyph_at(p, one);
    w += ((tabular && isdigit((int)one[0])) ? cell : measure(one, f).w) + (*p ? track : 0);
  }
  return w;
}

// This sits directly beneath the firmware's text renderer, which on the new
// PebbleOS needs some 1.2KB of the app's 2KB stack, so its frame is kept to
// a handful of registers: the glyph lives in static storage and the box is
// built in place.
static char s_glyph[5];
// Draws from a screen x, advancing rightwards: the glyphs of a run keep
// their reading order whichever wrist the face is laid out for.
static void draw_run_s(GContext *ctx, const char *t, GFont f, int x, int y, bool tabular, int track) {
  const int cell = tabular ? measure("0", f).w : 0;
  for (const char *p = t; *p;) {
    p += glyph_at(p, s_glyph);
    const GSize z = measure(s_glyph, f);
    // A glyph boxed at exactly its measured width can still be judged not
    // to fit and drawn as an ellipsis, so the box gets slack past its end.
    graphics_draw_text(ctx, s_glyph, f, GRect(x, y, 2 * z.w + 4, z.h),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    x += ((tabular && isdigit((int)s_glyph[0])) ? cell : z.w) + track;
  }
}

// The same from a logical x: the run is mirrored as a whole for the other
// wrist, so what is end-aligned on one is start-aligned on the other, as the
// design lays it out.
static void draw_run(GContext *ctx, const char *t, GFont f, int lx, int y, bool tabular, int track) {
  draw_run_s(ctx, t, f, mapx(lx, run_w(t, f, tabular, track)), y, tabular, track);
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
  "......##..",
  ".....####.",
  ".....####.",
  ".##..####.",
  "####......",
  "####...##.",
  "####...##.",
  "..........",
  ".##.......",
  ".##.......",
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
  { // partly: a small sun high on the wrist side, the cloud in front
    "#..#..#...",
    "..........",
    "..###.....",
    "#.###.#...",
    "..###.....",
    ".....###..",
    "#...#####.",
    "...#######",
    "...#######",
    "....#####.",
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
// sleeve covers it before the countdown or the rail.
static void draw_modules(GContext *ctx, GFont f_val, GFont f_cap,
                         int c_start, int avail_w, int band_top, int band_bot) {
  Module mods[MODULE_COUNT];
  int n = 0;
  for (int i = 0; i < MODULE_COUNT; i++) {
    if (s_set.mod[i] == MODULE_NONE) continue;
    if (module_read(s_set.mod[i], &mods[n])) n++;
  }
  if (n == 0) return;

  const GSize z_cap = measure("BPM", f_cap);
  const GSize z_val = measure("88", f_val);
  const Metrics m_val = barlow_metrics(z_val.h);
  const int icon = m_val.cap;                 // an icon reads as one cap tall
  // Captions are laid out from the measured box, not an estimated cap: the
  // box bounds the glyph whatever the font's metrics turn out to be. An icon
  // fills its box where a caption's box carries slack, so it gets a wider
  // gap to the value.
  const bool icons = s_set.mod_icons;
  const Metrics m_cap = barlow_metrics(z_cap.h);
  const int label_h = icons ? icon : m_cap.cap;
  const int lgap = icons ? sc(MOD_ICON_GAP) : sc(MOD_LABEL_GAP);
  const int gap = sc(MOD_GAP);

  // Widths first: the row is dropped whole if it cannot fit the band.
  int widths[MODULE_COUNT], total = 0;
  for (int i = 0; i < n; i++) {
    int w = run_w(mods[i].value, f_val, true, 0);
    if (module_uses_icon(&mods[i])) {
      const int iw = module_icon_w(&mods[i], icon);
      if (iw > w) w = iw;
    }
    else { const int cw = run_w(mods[i].caption, f_cap, false, TRACK); if (cw > w) w = cw; }
    widths[i] = w;
    total += w + (i ? gap : 0);
  }
  while (n > 1 && total > avail_w) { n--; total -= widths[n] + gap; }
  if (total > avail_w) return;

  const int row_h = label_h + lgap + m_val.cap;
  // A timeline peek squeezes the band to nothing. The modules are the lowest
  // zone in the design's ranking, so they go first rather than crowd the
  // countdown — the same order a sleeve would take them in.
  if (band_bot - band_top < row_h) return;
  // Centred, then the design's 4px padding-top nudges it down by half that.
  const int y = (band_top + band_bot) / 2 - row_h / 2 + sc(1);

  int x = c_start;
  for (int i = 0; i < n; i++) {
    if (module_uses_icon(&mods[i])) {
      graphics_context_set_fill_color(ctx, s_dim);
      graphics_context_set_stroke_color(ctx, s_dim);
      const int iw = module_icon_w(&mods[i], icon);
      module_draw_icon(ctx, &mods[i], GRect(mapx(x, iw), y + label_h - icon, iw, icon));
    } else {
      graphics_context_set_text_color(ctx, s_dim);
      draw_run(ctx, mods[i].caption, f_cap, x, y + label_h - m_cap.cap - m_cap.bearing, false, TRACK);
    }
    graphics_context_set_text_color(ctx, s_ink);
    draw_run(ctx, mods[i].value, f_val, x, y + label_h + lgap - m_val.bearing, true, 0);
    x += widths[i] + gap;
  }
}

// The content box, inside the rail and the paddings, in logical x.
typedef struct { int start, end, top, bot; } Frame;

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

static void paint_time(GContext *ctx) __attribute__((noinline));
static void paint_time(GContext *ctx) {
  // The row is mirrored as a whole; hour, colon and minute keep their order.
  const int x = mapx(s_tm.x, s_tm.w);
  graphics_context_set_text_color(ctx, s_ink);
  draw_run_s(ctx, s_tm.hh, s_f_time, x, s_tm.y, true, 0);
  draw_run_s(ctx, s_tm.mm, s_f_time, x + s_tm.hh_w + s_tm.colon_w, s_tm.y, true, 0);
  // The one accent in the time: the design's colon.
  graphics_context_set_text_color(ctx, accent());
  graphics_draw_text(ctx, ":", s_f_time,
                     GRect(x + s_tm.hh_w + s_tm.cgap, s_tm.y, s_tm.z_colon.w, s_tm.z_colon.h),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
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

static void paint_block(GContext *ctx, const Frame *fr) __attribute__((noinline));
static void paint_block(GContext *ctx, const Frame *fr) {
  if (s_bk.solid) {
    const GColor fill = s_bk.now ? s_ink : accent();
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(mapx(s_bk.block_x, s_bk.block_w), s_bk.block_y, s_bk.block_w, s_bk.block_h),
                       0, GCornerNone);
    graphics_context_set_text_color(ctx, on_fill(fill));
  } else {
    graphics_context_set_text_color(ctx, s_ink);
  }
  draw_run(ctx, s_bk.label, s_f_label, s_bk.inner_x + s_bk.inner_w - s_bk.label_w,
           s_bk.label_y - s_bk.m_lab.bearing, false, TRACK);
  const int num_x = s_bk.inner_x + s_bk.inner_w - s_bk.num_row_w;
  if (s_bk.now) {
    draw_at(ctx, s_bk.num, s_f_count, num_x, s_bk.num_y - s_bk.m_num.bearing, s_bk.z_num);
  } else {
    // One row, mirrored as a whole: the number, then MIN on its baseline.
    const int x = mapx(num_x, s_bk.num_row_w);
    draw_run_s(ctx, s_bk.num, s_f_count, x, s_bk.num_y - s_bk.m_num.bearing, true, 0);
    draw_run_s(ctx, s_bk.unit, s_f_label, x + s_bk.num_w + sc(LABEL_GAP),
               s_bk.num_y + s_bk.m_num.cap - s_bk.m_min.cap - s_bk.m_min.bearing, false, TRACK);
  }
  graphics_context_set_text_color(ctx, s_ink);
  draw_run(ctx, s_bk.dow, s_f_date, fr->start, s_bk.dow_y - s_bk.m_dow.bearing, false, TRACK);
  graphics_context_set_text_color(ctx, s_dim);
  draw_run(ctx, s_bk.date, s_f_date, fr->start, s_bk.date_y - s_bk.m_date.bearing, false, TRACK);
}

static void face_update(Layer *layer, GContext *ctx) {
  // A timeline peek slides up over the bottom of the watchface — precisely
  // where the countdown and the date sit. Laying out against the
  // unobstructed area keeps the second-most important zone on screen
  // instead of letting the peek bury it.
  const GRect full = layer_get_bounds(layer);
  const GRect b = layer_get_unobstructed_bounds(layer);
  s_w = b.size.w;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  Schedule sch = schedule_for(t->tm_hour, t->tm_min, t->tm_sec);
/*DEMO*/

  theme_apply(t->tm_hour);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  draw_rail(ctx, &sch, b.size.h);

  const Frame fr = {
    .start = sc(PAD_WRIST),                                       // logical left
    .end = s_w - sc(RAIL_W) - sc(RAIL_BORDER) - sc(PAD_GUTTER),   // logical right
    .top = sc(PAD_TOP),
    .bot = b.size.h - sc(PAD_BOTTOM),
  };

  layout_time(t, &fr);
  layout_block(t, &sch, &fr);
  paint_time(ctx);
  paint_block(ctx, &fr);
  draw_modules(ctx, s_f_mod, s_f_cap, fr.start, fr.end - fr.start, s_tm.band_top, s_bk.block_y);

  // ---- boarding buzz, once on the transition into the solid block.
  if (s_set.buzz && sch.remaining == THRESHOLD && s_last_remaining != THRESHOLD
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

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *tp;
  if ((tp = dict_find(iter, MESSAGE_KEY_WRIST))) {
    s_set.wrist_right = (strcmp(tp->value->cstring, "right") == 0);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_OFFSET))) {
    s_set.offset = (uint8_t)tp->value->int32;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_HEADWAY))) {
    s_set.headway = (uint8_t)atoi(tp->value->cstring);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_BUZZ))) {
    s_set.buzz = tp->value->int32 != 0;
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_H24))) {
    s_set.time_fmt = (uint8_t)atoi(tp->value->cstring);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_THEME))) {
    s_set.theme = (uint8_t)atoi(tp->value->cstring);
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
    s_set.mod[0] = (uint8_t)atoi(tp->value->cstring);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD2))) {
    s_set.mod[1] = (uint8_t)atoi(tp->value->cstring);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_MOD3))) {
    s_set.mod[2] = (uint8_t)atoi(tp->value->cstring);
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
  s_f_time = fonts_load_custom_font(resource_get_handle(RES_TIME));
  s_f_count = fonts_load_custom_font(resource_get_handle(RES_COUNT));
  s_f_mod = fonts_load_custom_font(resource_get_handle(RES_MOD));
  s_f_label = fonts_load_custom_font(resource_get_handle(RES_LABEL));
  s_f_date = fonts_load_custom_font(resource_get_handle(RES_DATE));
  s_f_cap = fonts_load_custom_font(resource_get_handle(RES_CAP));

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load, .unload = window_unload});
  window_set_background_color(s_window, GColorBlack);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  retune_tick();

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  fonts_unload_custom_font(s_f_time);
  fonts_unload_custom_font(s_f_count);
  fonts_unload_custom_font(s_f_mod);
  fonts_unload_custom_font(s_f_label);
  fonts_unload_custom_font(s_f_date);
  fonts_unload_custom_font(s_f_cap);
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

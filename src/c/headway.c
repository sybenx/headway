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
  s_set.mod[0] = MODULE_HR;
  s_set.mod[1] = MODULE_STEPS;
  s_set.mod[2] = MODULE_BATTERY;
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
#define DATE_PAD_BOT  3
#define BLOCK_PAD_T   4
#define BLOCK_PAD_B   2
#define BLOCK_PAD_IN  6   // toward the wrist
#define BLOCK_PAD_OUT 5   // toward the rail
#define BLOCK_MIN_W  65
#define LABEL_GAP     3
#define DOW_GAP       3
#define ROW_GAP       2
#define MOD_GAP       9
#define MOD_LABEL_GAP 3
#define MOD_ICON_GAP  4
#define BLOCK_PAD_MIN 3
#define TIME_MARGIN_TOP 3
#define COLON_GAP     1

// The design's own face. System LECO tops out at 42px, which is half the
// scale the mock draws, so the numerals ship as a resource and scale per
// platform. Labels stay in the system Gothic the build notes ask for.
#ifdef PBL_PLATFORM_EMERY
  #define RES_TIME  RESOURCE_ID_FONT_TIME_83
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_61
#else
  #define RES_TIME  RESOURCE_ID_FONT_TIME_60
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_44
#endif

static GFont s_f_time, s_f_count;
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
static Metrics gothic_metrics(int boxh) {
  Metrics m = { boxh * 3 / 14, boxh * 10 / 14 };
  return m;
}

// Draw text with its measured box placed at a logical origin.
static void draw_at(GContext *ctx, const char *t, GFont f, int lx, int y, GSize sz) {
  graphics_draw_text(ctx, t, f, GRect(mapx(lx, sz.w), y, sz.w, sz.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}


// ---------------------------------------------------------------- icons
// Drawn rather than bundled: they invert with the theme, scale with the
// display, and cost no resource budget.

static void icon_heart(GContext *ctx, GRect r) {
  const int16_t w = r.size.w, h = r.size.h;
  const int16_t rad = w / 4;
  graphics_fill_circle(ctx, GPoint(r.origin.x + rad, r.origin.y + rad + 1), rad);
  graphics_fill_circle(ctx, GPoint(r.origin.x + w - rad - 1, r.origin.y + rad + 1), rad);
  // The point: a triangle tapering to the bottom centre.
  for (int16_t i = 0; i < h - rad - 1; i++) {
    const int16_t half = (w / 2) * (h - rad - 1 - i) / (h - rad - 1);
    graphics_draw_line(ctx, GPoint(r.origin.x + w / 2 - half, r.origin.y + rad + 1 + i),
                            GPoint(r.origin.x + w / 2 + half, r.origin.y + rad + 1 + i));
  }
}

// Scale a 1-bit pattern into a box. Composed circles and rounded rects
// cannot hold a recognisable shape at ten pixels; placing the pixels by
// hand can, and this still scales up for emery and inverts with the theme.
static void draw_bits(GContext *ctx, GRect r, const uint8_t *rows,
                      int rw, int rh) {
  for (int y = 0; y < rh; y++) {
    for (int x = 0; x < rw; x++) {
      if (!(rows[y] & (0x80 >> x))) continue;
      const int16_t x0 = r.origin.x + x * r.size.w / rw;
      const int16_t x1 = r.origin.x + (x + 1) * r.size.w / rw;
      const int16_t y0 = r.origin.y + y * r.size.h / rh;
      const int16_t y1 = r.origin.y + (y + 1) * r.size.h / rh;
      graphics_fill_rect(ctx, GRect(x0, y0, x1 > x0 ? x1 - x0 : 1,
                                    y1 > y0 ? y1 - y0 : 1), 0, GCornerNone);
    }
  }
}

// A shoe in profile: high at the heel, sloping to the toe, flat sole.
static const uint8_t SHOE[7] = {
  0xC0,  // # # . . . . . .
  0xE0,  // # # # . . . . .
  0xF0,  // # # # # . . . .
  0xF8,  // # # # # # . . .
  0xFE,  // # # # # # # # .
  0xFF,  // # # # # # # # #
  0xFF,  // # # # # # # # #
};

static void icon_steps(GContext *ctx, GRect r) {
  const int16_t h = r.size.w * 7 / 8;
  draw_bits(ctx, GRect(r.origin.x, r.origin.y + (r.size.h - h) / 2,
                       r.size.w, h), SHOE, 8, 7);
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

// A cloud is a raised centre lump between two lower shoulders, on a flat
// base. Equal lumps at equal heights just merge into a dome, which reads as
// a hill rather than a cloud.
static void cloud_body(GContext *ctx, GRect r) {
  const int16_t w = r.size.w, h = r.size.h;
  const int16_t base = r.origin.y + h;
  const int16_t big = h / 2 > 2 ? h / 2 : 2;
  const int16_t small = h / 3 > 1 ? h / 3 : 1;
  graphics_fill_circle(ctx, GPoint(r.origin.x + small, base - small), small);
  graphics_fill_circle(ctx, GPoint(r.origin.x + w - small - 1, base - small), small);
  graphics_fill_circle(ctx, GPoint(r.origin.x + w / 2, base - big - 1), big);
  graphics_fill_rect(ctx, GRect(r.origin.x, base - small - 1, w, small + 1), 0, GCornerNone);
}

static void icon_sun(GContext *ctx, GRect r, bool rays) {
  const int16_t cx = r.origin.x + r.size.w / 2, cy = r.origin.y + r.size.h / 2;
  // At module size the rays have nowhere to go; a solid disc is unmistakable
  // next to the cloud shapes, so they only appear once there is room.
  const int16_t rad = r.size.w / 3;
  graphics_fill_circle(ctx, GPoint(cx, cy), rad);
  if (!rays || r.size.w < 16) return;
  const int16_t a = rad + 2, b2 = rad + r.size.w / 5 + 1;
  graphics_draw_line(ctx, GPoint(cx, cy - a), GPoint(cx, cy - b2));
  graphics_draw_line(ctx, GPoint(cx, cy + a), GPoint(cx, cy + b2));
  graphics_draw_line(ctx, GPoint(cx - a, cy), GPoint(cx - b2, cy));
  graphics_draw_line(ctx, GPoint(cx + a, cy), GPoint(cx + b2, cy));
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

static void icon_weather(GContext *ctx, GRect r, int code, GColor ink, GColor ground) {
  const int16_t w = r.size.w, h = r.size.h;
  const int kind = wx_kind(code);

  if (kind == WX_SUN) { icon_sun(ctx, r, true); return; }

  if (kind == WX_FOG) {
    for (int16_t i = 0; i < 3; i++) {
      const int16_t y = r.origin.y + h / 4 + i * (h / 4);
      graphics_draw_line(ctx, GPoint(r.origin.x + (i & 1 ? 2 : 0), y),
                              GPoint(r.origin.x + w - 1 - (i & 1 ? 0 : 2), y));
    }
    return;
  }

  if (kind == WX_PARTLY) {
    // Sun behind, then the cloud punched out of it so the two stay legible
    // instead of merging into one blob.
    icon_sun(ctx, GRect(r.origin.x, r.origin.y, w * 2 / 3, h * 2 / 3), true);
    const GRect cloud = GRect(r.origin.x + w / 4, r.origin.y + h / 3,
                              w - w / 4, h - h / 3);
    graphics_context_set_fill_color(ctx, ground);
    cloud_body(ctx, GRect(cloud.origin.x - 1, cloud.origin.y - 1,
                          cloud.size.w + 2, cloud.size.h + 1));
    graphics_context_set_fill_color(ctx, ink);
    cloud_body(ctx, cloud);
    return;
  }

  const bool precip = (kind == WX_RAIN || kind == WX_SNOW || kind == WX_STORM);
  const int16_t body_h = precip ? h * 3 / 5 : h * 4 / 5;
  const GRect body = GRect(r.origin.x, r.origin.y + (precip ? 0 : h / 8), w, body_h);
  cloud_body(ctx, body);
  if (!precip) return;

  const int16_t base = body.origin.y + body_h + 1;
  if (kind == WX_STORM) {
    // A drawn zigzag disappears at this size; two offset blocks keep the
    // diagonal of a bolt and stay distinct from rain's streaks.
    const int16_t cx = r.origin.x + w / 2;
    const int16_t t = (h - body_h) / 2 > 1 ? (h - body_h) / 2 : 2;
    graphics_fill_rect(ctx, GRect(cx, base, t, t), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(cx - t, base + t, t, t), 0, GCornerNone);
    return;
  }
  for (int16_t i = 0; i < 3; i++) {
    const int16_t x = r.origin.x + 1 + i * ((w - 2) / 2);
    if (kind == WX_SNOW) graphics_fill_rect(ctx, GRect(x, base + 1, 2, 2), 0, GCornerNone);
    else graphics_draw_line(ctx, GPoint(x + 1, base), GPoint(x, base + (h - body_h) - 1));
  }
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
// STEPS 4.8K, BATT 64%. The caption can be swapped for an icon, and weather
// always uses one, since the sky condition says more than the word "TEMP".
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
      m->caption = s_set.imperial ? "\u00B0F" : "\u00B0C";
      m->extra = s_wx.code;
      return true;
    }
    default: return false;
  }
}

// Weather is icon-only by request; everything else follows the toggle.
static bool module_uses_icon(const Module *m) {
  return s_set.mod_icons || m->kind == MODULE_WEATHER;
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
    case MODULE_WEATHER: icon_weather(ctx, box, m->extra, s_dim, s_ground); break;
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
  const Metrics m_val = gothic_metrics(z_val.h);
  const int icon = m_val.cap;                 // an icon reads as one cap tall
  // Captions are laid out from the measured box, not an estimated cap: the
  // box bounds the glyph whatever the font's metrics turn out to be. The row
  // takes one label height and one baseline across every module, so a mixed
  // row — caption mode with a weather module, which always draws an icon —
  // still lines its values up.
  int label_h = 0;
  bool any_icon = false;
  for (int i = 0; i < n; i++) {
    const bool ic = module_uses_icon(&mods[i]);
    any_icon |= ic;
    const int h = ic ? icon : z_cap.h;
    if (h > label_h) label_h = h;
  }
  const int lgap = any_icon ? sc(MOD_ICON_GAP) : sc(MOD_LABEL_GAP);
  const int gap = sc(MOD_GAP);

  // Widths first: the row is dropped whole if it cannot fit the band.
  int widths[MODULE_COUNT], total = 0;
  for (int i = 0; i < n; i++) {
    const GSize zv = measure(mods[i].value, f_val);
    int w = zv.w;
    if (module_uses_icon(&mods[i])) {
      const int iw = module_icon_w(&mods[i], icon);
      if (iw > w) w = iw;
    }
    else { const GSize zc = measure(mods[i].caption, f_cap); if (zc.w > w) w = zc.w; }
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
  const int y = (band_top + band_bot) / 2 - row_h / 2;

  int x = c_start;
  for (int i = 0; i < n; i++) {
    if (module_uses_icon(&mods[i])) {
      graphics_context_set_fill_color(ctx, s_dim);
      graphics_context_set_stroke_color(ctx, s_dim);
      const int iw = module_icon_w(&mods[i], icon);
      module_draw_icon(ctx, &mods[i], GRect(mapx(x, iw), y + label_h - icon, iw, icon));
    } else {
      graphics_context_set_text_color(ctx, s_dim);
      const GSize zc = measure(mods[i].caption, f_cap);
      draw_at(ctx, mods[i].caption, f_cap, x, y + label_h - zc.h, zc);
    }
    graphics_context_set_text_color(ctx, s_ink);
    const GSize zv = measure(mods[i].value, f_val);
    draw_at(ctx, mods[i].value, f_val, x, y + label_h + lgap - m_val.bearing, zv);
    x += widths[i] + gap;
  }
}


static void face_update(Layer *layer, GContext *ctx) {
  const GRect b = layer_get_bounds(layer);
  s_w = b.size.w;
  theme_apply(0);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, s_ink);
  graphics_context_set_stroke_color(ctx, s_ink);
  const int S = 10, PAD = 4;
  int codes[] = {0, 2, 3, 45, 61, 71, 95};
  // row 0: heart, steps, battery 15/60/100
  int x = PAD, y = PAD;
  icon_heart(ctx, GRect(x, y, S, S)); x += S + 8;
  icon_steps(ctx, GRect(x, y, S*4/5, S)); x += S + 8;
  icon_battery(ctx, GRect(x, y, S*8/5, S), 15); x += S*8/5 + 8;
  icon_battery(ctx, GRect(x, y, S*8/5, S), 60); x += S*8/5 + 8;
  icon_battery(ctx, GRect(x, y, S*8/5, S), 100);
  // rows 1-2: every distinct weather condition
  x = PAD; y = PAD + S + 14;
  for (int i = 0; i < 7; i++) {
    if (i == 4) { x = PAD; y += S + 14; }
    icon_weather(ctx, GRect(x, y, S, S), codes[i], s_ink, s_ground);
    x += S + 10;
  }
  // and the same weather set at double size, to see the intent
  x = PAD; y += S + 16;
  for (int i = 0; i < 4; i++) {
    icon_weather(ctx, GRect(x, y, S*2, S*2), codes[i], s_ink, s_ground);
    x += S*2 + 8;
  }
  x = PAD; y += S*2 + 8;
  for (int i = 4; i < 7; i++) {
    icon_weather(ctx, GRect(x, y, S*2, S*2), codes[i], s_ink, s_ground);
    x += S*2 + 8;
  }
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
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

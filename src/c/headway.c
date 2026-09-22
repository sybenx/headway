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
  bool imperial;
} Settings;

#define SETTINGS_KEY 1
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
  if (persist_exists(SETTINGS_KEY)) {
    persist_read_data(SETTINGS_KEY, &s_set, sizeof(s_set));
  }
  settings_clamp();
}

static void settings_save(void) {
  persist_write_data(SETTINGS_KEY, &s_set, sizeof(s_set));
}

static void weather_load(void) {
  memset(&s_wx, 0, sizeof(s_wx));
  if (persist_exists(WEATHER_KEY)) persist_read_data(WEATHER_KEY, &s_wx, sizeof(s_wx));
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
#define MOD_LABEL_GAP 1
#define BLOCK_PAD_MIN 3
#define TIME_MARGIN_TOP 3
#define COLON_GAP     1

// The design's own face. System LECO tops out at 42px, which is half the
// scale the mock draws, so the numerals ship as a resource and scale per
// platform. Labels stay in the system Gothic the build notes ask for.
#ifdef PBL_PLATFORM_EMERY
  #define RES_TIME  RESOURCE_ID_FONT_TIME_83
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_61
  #define RES_MOD   RESOURCE_ID_FONT_MOD_21
#else
  #define RES_TIME  RESOURCE_ID_FONT_TIME_60
  #define RES_COUNT RESOURCE_ID_FONT_COUNT_44
  #define RES_MOD   RESOURCE_ID_FONT_MOD_15
#endif

static GFont s_f_time, s_f_count, s_f_mod;
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

static void icon_steps(GContext *ctx, GRect r) {
  const int16_t fw = r.size.w * 2 / 5, fh = r.size.h * 3 / 5;
  graphics_fill_rect(ctx, GRect(r.origin.x, r.origin.y, fw, fh), fw / 2, GCornersAll);
  graphics_fill_rect(ctx, GRect(r.origin.x + r.size.w - fw, r.origin.y + r.size.h - fh, fw, fh),
                     fw / 2, GCornersAll);
}

static void icon_battery(GContext *ctx, GRect r, int pct) {
  const int16_t h = r.size.h * 3 / 4;
  const int16_t y = r.origin.y + (r.size.h - h) / 2;
  const int16_t bw = r.size.w - 2;
  graphics_draw_rect(ctx, GRect(r.origin.x, y, bw, h));
  graphics_fill_rect(ctx, GRect(r.origin.x + bw, y + h / 3, 2, h / 3), 0, GCornerNone);
  const int16_t inner = bw - 2;
  const int16_t fill = (inner * pct + 50) / 100;
  if (fill > 0) graphics_fill_rect(ctx, GRect(r.origin.x + 1, y + 1, fill, h - 2), 0, GCornerNone);
}

static void icon_cloud(GContext *ctx, GRect r, int16_t drop) {
  const int16_t w = r.size.w, y = r.origin.y + drop;
  const int16_t rad = w / 4;
  graphics_fill_circle(ctx, GPoint(r.origin.x + rad + 1, y + rad + 1), rad);
  graphics_fill_circle(ctx, GPoint(r.origin.x + w - rad - 1, y + rad + 1), rad - 1);
  graphics_fill_circle(ctx, GPoint(r.origin.x + w / 2, y + rad), rad);
  graphics_fill_rect(ctx, GRect(r.origin.x + 1, y + rad, w - 2, rad + 1), 0, GCornerNone);
}

static void icon_sun(GContext *ctx, GRect r, bool rays) {
  const int16_t cx = r.origin.x + r.size.w / 2, cy = r.origin.y + r.size.h / 2;
  const int16_t rad = r.size.w / 4;
  graphics_fill_circle(ctx, GPoint(cx, cy), rad);
  if (!rays) return;
  const int16_t a = rad + 1, b = rad + r.size.w / 5;
  graphics_draw_line(ctx, GPoint(cx, cy - a), GPoint(cx, cy - b));
  graphics_draw_line(ctx, GPoint(cx, cy + a), GPoint(cx, cy + b));
  graphics_draw_line(ctx, GPoint(cx - a, cy), GPoint(cx - b, cy));
  graphics_draw_line(ctx, GPoint(cx + a, cy), GPoint(cx + b, cy));
}

// WMO codes, as Open-Meteo reports them.
static void icon_weather(GContext *ctx, GRect r, int code) {
  const int16_t w = r.size.w, h = r.size.h;
  if (code == 0 || code == 1) { icon_sun(ctx, r, true); return; }
  if (code == 2) {                                   // partly cloudy
    GRect sr = GRect(r.origin.x, r.origin.y, w * 3 / 5, h * 3 / 5);
    icon_sun(ctx, sr, true);
    icon_cloud(ctx, GRect(r.origin.x + w / 4, r.origin.y, w * 3 / 4, h), h / 3);
    return;
  }
  if (code == 45 || code == 48) {                    // fog
    for (int16_t i = 0; i < 3; i++) {
      const int16_t y = r.origin.y + h / 4 + i * (h / 4);
      graphics_draw_line(ctx, GPoint(r.origin.x + (i & 1 ? 2 : 0), y),
                              GPoint(r.origin.x + w - (i & 1 ? 0 : 2), y));
    }
    return;
  }
  icon_cloud(ctx, r, 0);
  const int16_t base = r.origin.y + h * 3 / 5;
  if (code == 95 || code == 96 || code == 99) {      // thunder
    graphics_draw_line(ctx, GPoint(r.origin.x + w / 2 + 1, base),
                            GPoint(r.origin.x + w / 2 - 2, base + h / 4));
    graphics_draw_line(ctx, GPoint(r.origin.x + w / 2 - 2, base + h / 4),
                            GPoint(r.origin.x + w / 2 + 2, base + h / 4));
    graphics_draw_line(ctx, GPoint(r.origin.x + w / 2 + 2, base + h / 4),
                            GPoint(r.origin.x + w / 2 - 1, base + h / 2));
    return;
  }
  const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  for (int16_t i = 0; i < 3; i++) {
    const int16_t x = r.origin.x + 2 + i * ((w - 4) / 2);
    if (snow) graphics_fill_rect(ctx, GRect(x, base + h / 5, 2, 2), 0, GCornerNone);
    else graphics_draw_line(ctx, GPoint(x, base), GPoint(x - 1, base + h / 3));
  }
}

// --------------------------------------------------------------- schedule

typedef struct {
  int remaining;      // minutes until the next departure
  int next_h, next_m; // clock time of that departure
  bool is_now, is_boarding;
} Schedule;

static Schedule schedule_for(int hour, int minute) {
  Schedule s;
  int hw = s_set.headway;
  s.remaining = ((s_set.offset - minute) % hw + hw) % hw;
  s.next_h = hour;
  s.next_m = minute + s.remaining;
  if (s.next_m >= 60) { s.next_m -= 60; s.next_h = (s.next_h + 1) % 24; }
  s.is_now = (s.remaining == 0);
  s.is_boarding = (s.remaining > 0 && s.remaining <= THRESHOLD);
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
      m->caption = "BATT";
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
  return m->kind == MODULE_BATTERY ? icon * 8 / 5 : icon;
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
  // box bounds the glyph whatever the font's metrics turn out to be, so the
  // value underneath can never ride up into it.
  const int label_h = module_uses_icon(&mods[0]) ? icon : z_cap.h;
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

  const int row_h = label_h + sc(MOD_LABEL_GAP) + m_val.cap;
  int y = (band_top + band_bot) / 2 - row_h / 2;
  if (y < band_top) y = band_top;

  int x = c_start;
  for (int i = 0; i < n; i++) {
    if (module_uses_icon(&mods[i])) {
      graphics_context_set_fill_color(ctx, s_dim);
      graphics_context_set_stroke_color(ctx, s_dim);
      const int iw = module_icon_w(&mods[i], icon);
      module_draw_icon(ctx, &mods[i], GRect(mapx(x, iw), y, iw, icon));
    } else {
      graphics_context_set_text_color(ctx, s_dim);
      const GSize zc = measure(mods[i].caption, f_cap);
      draw_at(ctx, mods[i].caption, f_cap, x, y, zc);
    }
    graphics_context_set_text_color(ctx, s_ink);
    const GSize zv = measure(mods[i].value, f_val);
    draw_at(ctx, mods[i].value, f_val, x,
            y + label_h + sc(MOD_LABEL_GAP) - m_val.bearing, zv);
    x += widths[i] + gap;
  }
}

static void face_update(Layer *layer, GContext *ctx) {
  const GRect b = layer_get_bounds(layer);
  s_w = b.size.w;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  Schedule sch = schedule_for(t->tm_hour, t->tm_min);
/*DEMO*/

  theme_apply(t->tm_hour);
  graphics_context_set_fill_color(ctx, s_ground);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  draw_rail(ctx, &sch, b.size.h);

  // Content box, inside the rail and the paddings.
  const int rail_total = sc(RAIL_W) + sc(RAIL_BORDER);
  const int c_start = sc(PAD_WRIST);                        // logical left
  const int c_end   = s_w - rail_total - sc(PAD_GUTTER);     // logical right
  const int c_top   = sc(PAD_TOP);
  const int c_bot   = b.size.h - sc(PAD_BOTTOM);

  GFont f_time  = s_f_time;
  GFont f_count = s_f_count;
  // Gothic 14 Bold caps, per the build notes — one step up on emery, whose
  // 200px width would otherwise leave the labels undersized next to the
  // numerals, which do scale.
#ifdef PBL_PLATFORM_EMERY
  GFont f_label = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f_caption = fonts_get_system_font(FONT_KEY_GOTHIC_14);
#else
  GFont f_label = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  // The module caption is 7px at design scale, where a hand-tuned bitmap
  // face beats anything a TTF rasterises.
  GFont f_caption = fonts_get_system_font(FONT_KEY_GOTHIC_09);
#endif

  // ---- zone 03/04: the time, flush to the outer edge so the minute
  // survives a cuff that hides the hour.
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%02d", display_hour(t->tm_hour));
  snprintf(mm, sizeof(mm), "%02d", t->tm_min);

  const GSize z_hh = measure(hh, f_time), z_mm = measure(mm, f_time);
  const GSize z_colon = measure(":", f_time);
  const Metrics m_time = barlow_metrics(z_hh.h);
  const int cgap = sc(COLON_GAP);
  const int colon_w = z_colon.w + 2 * cgap;
  const int time_w = z_hh.w + colon_w + z_mm.w;
  const int time_x = c_end - time_w;
  const int time_y = c_top + sc(TIME_MARGIN_TOP) - m_time.bearing;

  graphics_context_set_text_color(ctx, s_ink);
  draw_at(ctx, hh, f_time, time_x, time_y, z_hh);
  draw_at(ctx, mm, f_time, time_x + z_hh.w + colon_w, time_y, z_mm);

  // The one accent in the time: the design's colon.
  graphics_context_set_text_color(ctx, accent());
  draw_at(ctx, ":", f_time, time_x + z_hh.w + cgap, time_y, z_colon);

  // ---- optional extras, in the band the design leaves empty.
  const int band_top = time_y + m_time.bearing + m_time.cap + sc(6);

  // ---- zone 02: the countdown, and zone 04: weekday and date.
  char dow[8], date[12], label[20], num[8];
  strftime(dow, sizeof(dow), "%a", t);
  strftime(date, sizeof(date), "%d %b", t);
  for (char *p = dow; *p; p++) *p = toupper((int)*p);
  for (char *p = date; *p; p++) *p = toupper((int)*p);

  const bool solid = sch.is_now || sch.is_boarding;
  if (sch.is_now) {
    strncpy(label, "DEPARTS", sizeof(label));
    strncpy(num, "NOW", sizeof(num));
  } else {
    snprintf(label, sizeof(label), "%s %d:%02d",
             sch.is_boarding ? "LEAVES" : "NEXT",
             display_hour(sch.next_h), sch.next_m);
    snprintf(num, sizeof(num), "%d", sch.remaining);
  }

  // "NOW" is letters, so it needs a text face rather than LECO numbers.
  GFont f_num = sch.is_now ? fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD) : f_count;
  const GSize z_label = measure(label, f_label);
  const GSize z_num = measure(num, f_num);
  const GSize z_min = sch.is_now ? GSize(0, 0) : measure("MIN", f_label);

  const Metrics m_lab = gothic_metrics(z_label.h);
  const Metrics m_min = gothic_metrics(z_min.h);
  const Metrics m_num = sch.is_now ? gothic_metrics(z_num.h) : barlow_metrics(z_num.h);

  const GSize z_dow = measure(dow, f_label), z_date = measure(date, f_label);

  const int num_row_w = z_num.w + (sch.is_now ? 0 : sc(LABEL_GAP) + z_min.w);
  int inner_w = num_row_w > z_label.w ? num_row_w : z_label.w;
  const int min_inner = sc(BLOCK_MIN_W) - sc(BLOCK_PAD_IN) - sc(BLOCK_PAD_OUT);
  if (inner_w < min_inner) inner_w = min_inner;

  // The bottom row is a space-between pair and the block must not grow into
  // the weekday/date column. The longest label ("LEAVES 10:30" in Gothic 14
  // Bold) is a few pixels wider than the 144px row allows, so the block's own
  // padding gives way first and the normal state keeps the design's spacing.
  const int date_w = z_dow.w > z_date.w ? z_dow.w : z_date.w;
  const int avail = c_end - c_start - date_w - sc(ROW_GAP);
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
  if (over > 0) inner_w -= over;   // last resort: the label ellipsizes

  const int inner_h = m_lab.cap + sc(LABEL_GAP) + m_num.cap;
  const int block_w = inner_w + pad_in + pad_out;
  const int block_h = inner_h + sc(BLOCK_PAD_T) + sc(BLOCK_PAD_B);
  const int block_x = c_end - block_w;
  const int block_y = c_bot - block_h;

  if (solid) {
    const GColor fill = sch.is_now ? s_ink : accent();
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(mapx(block_x, block_w), block_y, block_w, block_h), 0, GCornerNone);
    graphics_context_set_text_color(ctx, on_fill(fill));
  } else {
    graphics_context_set_text_color(ctx, s_ink);
  }

  // Label and number are end-aligned within the block.
  const int inner_x = block_x + pad_in;
  const int label_y = block_y + sc(BLOCK_PAD_T);
  graphics_draw_text(ctx, label, f_label,
                     GRect(mapx(inner_x, inner_w), label_y - m_lab.bearing, inner_w, z_label.h),
                     GTextOverflowModeTrailingEllipsis,
                     s_set.wrist_right ? GTextAlignmentLeft : GTextAlignmentRight, NULL);

  const int num_y = label_y + m_lab.cap + sc(LABEL_GAP);
  const int num_x = inner_x + inner_w - num_row_w;
  draw_at(ctx, num, f_num, num_x, num_y - m_num.bearing, z_num);
  if (!sch.is_now) {
    // "MIN" rides the baseline of the big number.
    const int min_y = num_y + m_num.cap - m_min.cap;
    draw_at(ctx, "MIN", f_label, num_x + z_num.w + sc(LABEL_GAP), min_y - m_min.bearing, z_min);
  }

  // Weekday and date sit on the wrist side: first to disappear.
  const Metrics m_dow = gothic_metrics(z_dow.h), m_date = gothic_metrics(z_date.h);
  const int date_y = c_bot - sc(DATE_PAD_BOT) - m_date.cap;
  const int dow_y = date_y - sc(DOW_GAP) - m_dow.cap;

  graphics_context_set_text_color(ctx, s_ink);
  draw_at(ctx, dow, f_label, c_start, dow_y - m_dow.bearing, z_dow);
  graphics_context_set_text_color(ctx, s_dim);
  draw_at(ctx, date, f_label, c_start, date_y - m_date.bearing, z_date);

  draw_modules(ctx, s_f_mod, f_caption, c_start, c_end - c_start, band_top, dow_y - sc(4));

  // ---- boarding buzz, once on the transition into the solid block.
  if (s_set.buzz && sch.remaining == THRESHOLD && s_last_remaining != THRESHOLD) {
    vibes_short_pulse();
  }
  s_last_remaining = sch.remaining;
}

// ------------------------------------------------------------------- wiring

static void tick_handler(struct tm *tick_time, TimeUnits units) {
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
  if ((tp = dict_find(iter, MESSAGE_KEY_UNITS))) {
    s_set.imperial = (strcmp(tp->value->cstring, "imperial") == 0);
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
  layer_mark_dirty(s_face);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_face = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_face, face_update);
  layer_add_child(root, s_face);
}

static void window_unload(Window *window) {
  layer_destroy(s_face);
}

static void init(void) {
  settings_load();
  weather_load();
  s_f_time = fonts_load_custom_font(resource_get_handle(RES_TIME));
  s_f_count = fonts_load_custom_font(resource_get_handle(RES_COUNT));
  s_f_mod = fonts_load_custom_font(resource_get_handle(RES_MOD));

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load, .unload = window_unload});
  window_set_background_color(s_window, GColorBlack);
  window_stack_push(s_window, true);

  // Once per minute. No seconds, no animation — battery and legibility.
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  fonts_unload_custom_font(s_f_time);
  fonts_unload_custom_font(s_f_count);
  fonts_unload_custom_font(s_f_mod);
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

// Clay's prebuilt bundle: the SDK's webpack cannot parse Clay's ES6 source.
var Clay = require('pebble-clay/dist/js/index.js');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

// The systems the face knows: written by tools/transit.py from each agency's
// GTFS. Each has a hub, its hours, the routes on their own timetable, the
// box its stops sit in, and where its per-stop files are served from.
var transit = require('./transit.json');
var MODE_OFF = '0', MODE_CHIPS = '1', MODE_NEAR = '2', MODE_AUTO = '3';

// Weather comes from Open-Meteo, which needs no API key and no account.
var WEATHER_TTL = 30 * 60 * 1000;
var lastFetch = 0;

function settings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

// day: Open-Meteo's daily block, for today's high and low, sent only when
// both are there.
function send(temp, code, rain, day) {
  var msg = { WOK: 1, TEMP: Math.round(temp), WCODE: code || 0, RAIN_AT: rain || 0 };
  var hi = day && day.temperature_2m_max && day.temperature_2m_max[0], lo = day && day.temperature_2m_min && day.temperature_2m_min[0];
  if (typeof hi === 'number' && typeof lo === 'number') { msg.WHI = Math.round(hi); msg.WLO = Math.round(lo); }
  Pebble.sendAppMessage(msg);
}

// When rain is due to start, in epoch seconds, from the forecast's quarter
// hours: each one's precipitation falls in the fifteen minutes before its
// time. 0 when none is due in the next two hours, or it's raining already
// (the sky in the module says so).
var RAIN_MM = 0.1;
function rainAt(m) {
  if (!m || !m.time || !m.precipitation) return 0;
  var now = Date.now() / 1000;
  for (var i = 0; i < m.time.length; i++) {
    if (m.time[i] <= now || !(m.precipitation[i] >= RAIN_MM)) continue;
    var start = m.time[i] - 900;
    return start <= now ? 0 : start;
  }
  return 0;
}

// Only when a slot is actually showing weather.
function weatherWanted() {
  var s = settings();
  return s.MOD1 === undefined || [s.MOD1, s.MOD2, s.MOD3].some(function (m) { return String(m) === '4'; });
}

// The weather for a fix already taken: the phone looks once for weather and
// transit both (see look()).
function weatherAt(pos, force) {
  var s = settings();
  if (!weatherWanted()) return;
  if (!force && Date.now() - lastFetch < WEATHER_TTL) return;
  var unit = units() === 'imperial' ? 'fahrenheit' : 'celsius';
  var url = 'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + pos.coords.latitude.toFixed(3) +
    '&longitude=' + pos.coords.longitude.toFixed(3) +
    '&current=temperature_2m,weather_code' +
    '&minutely_15=precipitation&forecast_minutely_15=8&timeformat=unixtime' +
    '&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto' +
    '&temperature_unit=' + unit;
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function () {
    if (req.status !== 200) return;
    try {
      var d = JSON.parse(req.responseText), cur = d.current;
      lastFetch = Date.now();
      send(cur.temperature_2m, cur.weather_code, rainAt(d.minutely_15), d.daily);
    } catch (e) {}
  };
  req.send();
}

// ---- units: the setting, or, left on Automatic, where the wearer is: °F and
// feet in the United States, °C and metres elsewhere. The watch is told the
// choice itself, never "auto".
// The United States, coarsely: the lower 48 by their borders and coasts, a
// few kilometres either way, then Alaska and Hawaii as boxes. [lat, lon].
var US = [[48.3, -124.8], [48.25, -123.25], [48.7, -123.0], [49.0, -123.1], [49.0, -95.15], [49.4, -95.15], [48.0, -89.6],
  [46.51, -84.6], [46.505, -84.2], [46.0, -83.5], [45.3, -82.5], [43.0, -82.42], [42.35, -82.95], [42.05, -83.15], [41.7, -82.7], [42.9, -78.9],
  [43.3, -79.05], [43.6, -77.0], [44.2, -76.3], [45.0, -74.7], [45.0, -71.5], [46.4, -70.0],
  [47.45, -69.2], [47.1, -67.8], [45.2, -67.4], [44.8, -66.9], [41.0, -69.8], [35.2, -75.3],
  [30.5, -80.9], [25.2, -80.0], [24.4, -81.9], [28.9, -89.0], [25.95, -97.15], [25.885, -97.5], [26.1, -98.3],
  [27.5, -99.5], [29.4, -100.9], [29.8, -101.4], [29.1, -103.2], [30.6, -104.9], [31.75, -106.5],
  [31.78, -108.2], [31.33, -108.2], [31.33, -111.07], [32.49, -114.8], [32.72, -114.72],
  [32.53, -117.12], [32.4, -117.3], [34.4, -120.7], [40.4, -124.5], [46.2, -124.1], [48.4, -124.8]];
function inUS(lat, lon) {
  if (lat > 51 && lat < 72 && lon > -170 && lon < -141) return true;                 // Alaska
  if (lat > 54.5 && lat < 60.5 && lon >= -141 && lon < -130) return true;             // its panhandle, and a few Canadian towns
  if (lat > 18.5 && lat < 22.5 && lon > -161 && lon < -154.5) return true;           // Hawaii
  var inside = false;
  for (var i = 0, j = US.length - 1; i < US.length; j = i++) {
    var a = US[i], b = US[j];
    if ((a[1] > lon) !== (b[1] > lon) && lat < (b[0] - a[0]) * (lon - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
  }
  return inside;
}
function units() {
  var u = settings().UNITS;
  if (u === 'metric' || u === 'imperial') return u;
  return localStorage.getItem('hw-units-here') || 'metric';
}
var unitsTold = null;
// True when the watch was told something new.
function tellUnits() {
  var u = units();
  if (u === unitsTold) return false;
  unitsTold = u;
  lastFetch = 0;   // the watch drops a reading in the other unit; fetch it anew
  Pebble.sendAppMessage({ UNITS: u });
  return true;
}

// ---- transit: is the wearer at the hub, and what leaves it next.
//
// The face counts down on its own; the phone only tells it whether the
// countdown applies here and now, and the next few departures of the
// routes on their own timetable. A coarse fix does: the question is
// "within a few hundred metres of the hub", not "which side of the road".
var TR_MAX = 4;

function metres(aLat, aLon, bLat, bLon) {
  var R = 6371000, toRad = Math.PI / 180;
  var dLat = (bLat - aLat) * toRad, dLon = (bLon - aLon) * toRad;
  var h = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos(aLat * toRad) * Math.cos(bLat * toRad) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return 2 * R * Math.asin(Math.sqrt(h));
}

// The system whose area holds the fix, or null.
function systemAt(lat, lon) {
  var list = transit.systems || [];
  for (var i = 0; i < list.length; i++) {
    var a = list[i].area;
    if (a && lat >= a[0] && lon >= a[1] && lat <= a[2] && lon <= a[3]) return list[i];
  }
  return null;
}
function firstHub() {
  var list = transit.systems || [];
  for (var i = 0; i < list.length; i++) if (list[i].hub) return list[i];
  return null;
}
function hubMode() {
  var s = settings();
  var m = s.TRANSIT === undefined ? MODE_AUTO : String(s.TRANSIT);
  return m === MODE_NEAR ? MODE_AUTO : m;   // an older name for automatic
}

// The day type in the hub's own week: weekday, saturday, sunday, or none.
function dayTable(sys, d) {
  var wd = d.getDay();
  var kind = wd === 0 ? 'sunday' : wd === 6 ? 'saturday' : 'weekday';
  return sys.days[kind] || null;
}

// The next departures of a route from a minute of the day, as minutes.
// The next departures, and the one just gone if it was due in the last ten
// minutes: a late bus may still be at its bay, and the watch shows its time
// beside the next one's so the two can be told apart.
var BOARD_WAIT = 10;
function upcoming(list, nowMin) {
  var out = [], prev = -1;
  for (var i = 0; i < list.length && out.length < TR_MAX; i++) {
    if (list[i] >= nowMin) out.push(list[i]);
    else if (nowMin - list[i] <= BOARD_WAIT) prev = list[i];
  }
  if (prev >= 0) { out.unshift(prev); if (out.length > TR_MAX) out.pop(); }
  return out;
}

function packMinutes(list) {
  var bytes = [];
  for (var i = 0; i < TR_MAX; i++) {
    var m = i < list.length ? list[i] : 0xFFFF;
    bytes.push(m & 0xFF, (m >> 8) & 0xFF);
  }
  return bytes;
}

function transitMsg(st) {
  return {
    TR_AREA: st.area,
    TR_STATE: st.state,
    TR_G: packMinutes(st.g),
    TR_B: packMinutes(st.b),
    TR_AT: Math.floor(Date.now() / 1000),
    TR_KM: st.km || 0,
    TR_NEAR: st.near ? 1 : 0,
  };
}

// Kilometres from a fix to the nearest system's area, 0 inside one. The
// watch weighs a flick by it: nobody drives from the next state into Logan
// in half an hour, so far from every system a flick needn't ask at all.
function kmToSystem(lat, lon) {
  var best = Infinity;
  (transit.systems || []).forEach(function (sys) {
    var a = sys.area;
    if (!a) return;
    var cl = Math.min(Math.max(lat, a[0]), a[2]), co = Math.min(Math.max(lon, a[1]), a[3]);
    best = Math.min(best, metres(lat, lon, cl, co) / 1000);
  });
  return best === Infinity ? 0 : Math.min(65535, Math.round(best));
}

// What the face should know about a position: whether it is in a system it
// knows, and whether it is at that system's hub in its hours, with the next
// departures of the routes on their own timetable. Null when there is
// nothing to say. The background check and a flick both ask this, a flick
// with its own fresh fix, so walking up to the hub and flicking starts the
// countdown then and there.
function transitState(lat, lon) {
  var mode = hubMode();
  if (mode === MODE_OFF) return null;
  var radius = Number(settings().TR_RADIUS) || 100;
  var sys = systemAt(lat, lon);
  if (!sys) {
    // Outside every system the face knows. Automatic: a plain watch; the
    // always-on modes still take the first system as their hub.
    if (mode === MODE_AUTO) {
      var km = kmToSystem(lat, lon);
      return { area: 0, state: 0, g: [], b: [], km: km, why: 'outside any known system, ' + km + ' km away' };
    }
    sys = firstHub();
    if (!sys) return null;
  }
  if (!sys.hub) {
    // A system of stops and no hub: in its area a flick answers, and the
    // face stays the plain watch. The always-on modes count down to the
    // first system that has a hub.
    if (mode === MODE_AUTO || !firstHub()) return { area: 1, state: 0, g: [], b: [], why: sys.agency + ', no hub' };
    sys = firstHub();
  }
  var d = metres(lat, lon, sys.hub.lat, sys.hub.lon);
  var now = new Date();
  var nowMin = now.getHours() * 60 + now.getMinutes();
  var day = dayTable(sys, now);
  // A quarter hour before the first run counts as hours: that is when the
  // first riders are standing there.
  var inHours = !!day && nowMin >= day.hours[0] - 15 && nowMin <= day.hours[1];
  var active = d <= radius && inHours;
  var routes = sys.routes;
  return {
    area: 1, state: active ? 1 : 0, near: d <= NEAR_HUB,
    g: active ? upcoming(day.dep[routes[0]] || [], nowMin) : [],
    b: active ? upcoming(day.dep[routes[1]] || [], nowMin) : [],
    why: sys.agency + ' ' + Math.round(d) + 'm from the hub, ' + (active ? 'at it' : 'away'),
  };
}

// ---- when the phone looks. One rough fix every half hour serves the weather
// and transit both. Within a walk of a hub (NEAR_HUB, about twelve minutes)
// the watch asks for more as the wearer walks, by its step count, so the
// countdown comes on as they arrive; the phone looks for it only while the
// hub's buses run, and after the day's last bus transit asks nothing until
// a quarter hour before the first one next morning. A flick takes its own
// fix, and counts as a look.
var LOOK_EVERY = 30 * 60 * 1000, NEAR_HUB = 1000;
var lastLook = 0, lastSys = null, lastHubM = Infinity;

function hubRunning(sys) {
  var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
  var day = sys && sys.days ? dayTable(sys, now) : null;
  return !!day && nowMin >= day.hours[0] - 15 && nowMin <= day.hours[1];
}
// True when the fix changed the units, so the weather wants fetching anew.
function sawFix(lat, lon) {
  lastLook = Date.now();
  try { localStorage.setItem('hw-units-here', inUS(lat, lon) ? 'imperial' : 'metric'); } catch (e) { /* the last word stands */ }
  var changed = tellUnits();
  lastSys = systemAt(lat, lon);
  lastHubM = lastSys && lastSys.hub ? metres(lat, lon, lastSys.hub.lat, lastSys.hub.lon) : Infinity;
  lastBaseM = lastSys && lastSys.base ? metres(lat, lon, lastSys.base.lat, lastSys.base.lon) : Infinity;
  return changed;
}

// A walking look near the hub takes a fresh fix once the last one is within
// FRESH_NEAR of it, where the countdown's hundred metres is decided; further
// out one a few minutes old tells closer from farther well enough. Nothing
// here asks for high accuracy: the phone's everyday fix is the one used.
var FRESH_NEAR = 400;
function look(force, fresh) {
  var mode = hubMode(), since = Date.now() - lastLook;
  // Inside a hub's system out of its hours, transit has nothing to ask.
  var transitWants = mode !== MODE_OFF && (force || !(lastSys && lastSys.hub && !hubRunning(lastSys)));
  if (!force && since < LOOK_EVERY) return;
  var wx = weatherWanted();
  if (!wx && !transitWants) return;
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    sawFix(lat, lon);
    tellYardWindow();
    // A countdown whose stop is well behind the wearer now ends.
    if (cdStop && metres(lat, lon, cdStop.lat, cdStop.lon) > Math.max(CD_GONE, pos.coords.accuracy || 0)) {
      console.log('headway: gone from the countdown\'s stop');
      cdStop = null;
      Pebble.sendAppMessage({ CD_END: 1 });
    }
    if (wx) weatherAt(pos, force);
    if (!transitWants) return;
    var st = transitState(lat, lon);
    if (!st) return;
    console.log('headway: ' + st.why);
    Pebble.sendAppMessage(transitMsg(st));
  }, function () {
    // No fix: say nothing, and the watch keeps its last word until it is stale.
  }, fresh ? { timeout: 9000, maximumAge: 20000 }
           : { timeout: 10000, maximumAge: 4 * 60 * 1000 });
}

// ---- the flick: the nearest stop and what leaves it next.
//
// The stop index and the per-stop files come from the repo's own pages,
// written nightly by tools/transit.py. The fix picks the stop, and every stop
// it could be at by its own accuracy is read with it; twin stops across a road
// are merged, since the headsign tells them apart.
//
// Both are kept on the phone, and whatever is kept answers when the network
// doesn't: a flick at a stop with no signal still gets its timetable. The
// index says which cut of the timetable it is (v); a stop file kept from the
// same cut is good until the agency publishes a new one, and never mixed with
// another cut's index, whose services it wouldn't match. An index without a
// version (pages built before it had one) goes by time, as before.
var INDEX_TTL = 24 * 60 * 60 * 1000, STOP_TTL = 6 * 60 * 60 * 1000;
// At a stop, the board; off it, the board with its distance; further than
// two kilometres from any stop, nothing.
var AT_STOP = 60, TWIN = 80, HUB = 100, FAR = 2000;
// However rough the fix, the stops within this much further than the nearest
// are the most a flick reads: past it the board is a neighbourhood, not a stop.
var MAX_REACH = 150;

function kept(key) {
  try { return JSON.parse(localStorage.getItem(key)); } catch (e) { return null; }
}
function remember(key, data, v) {
  try { localStorage.setItem(key, JSON.stringify({ at: Date.now(), v: v, data: data })); } catch (e) {}
}
// The kept stop files, most recently used last, KEPT_MAX at most: the oldest
// gives way, so a phone that has flicked across a whole city doesn't fill.
var KEPT_MAX = 150;
function useKept(key) {
  try {
    var list = JSON.parse(localStorage.getItem('hw2-kept')) || [], at = list.indexOf(key);
    if (at === list.length - 1 && at >= 0) return;
    if (at >= 0) list.splice(at, 1);
    list.push(key);
    while (list.length > KEPT_MAX) localStorage.removeItem(list.shift());
    localStorage.setItem('hw2-kept', JSON.stringify(list));
  } catch (e) { /* kept as it is */ }
}
// A kept stop file that answers for this index as it stands.
function stopKept(entry, index) {
  if (!entry) return false;
  return index.v ? entry.v === index.v : Date.now() - entry.at < STOP_TTL;
}
function fetchJSON(url, cb) {
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function () {
    if (req.status !== 200) return cb(null);
    try { cb(JSON.parse(req.responseText)); } catch (e) { cb(null); }
  };
  req.onerror = function () { cb(null); };
  req.timeout = 8000;
  req.ontimeout = function () { cb(null); };
  req.send();
}
// The index: the kept one while it's a day old at most, else the network's,
// else the kept one however old. cb(index, old): old when that last is more
// than INDEX_OLD past its fetch, so the answer can say its timetable may
// have changed since.
var INDEX_OLD = 3 * 24 * 60 * 60 * 1000;
function getIndex(sys, cb) {
  var key = 'hw2-stops-' + sys.agency.toLowerCase(), entry = kept(key);
  if (entry && Date.now() - entry.at < INDEX_TTL) return cb(entry.data, false);
  fetchJSON(sys.data + 'stops.json', function (d) {
    if (d && d.stops) { remember(key, d); return cb(d, false); }
    cb(entry ? entry.data : null, !!entry && Date.now() - entry.at > INDEX_OLD);
  });
}
// A stop's file: the kept one when it answers for this index, else the
// network's, else, with an index that doesn't say its cut, the kept one
// however old.
function getStop(sys, index, id, cb) {
  var key = 'hw2-stop-' + sys.agency.toLowerCase() + '-' + id, entry = kept(key);
  if (stopKept(entry, index)) { useKept(key); return cb(entry.data); }
  fetchJSON(sys.data + 'stops/' + id + '.json', function (d) {
    if (d) { remember(key, d, index.v); useKept(key); return cb(d); }
    cb(entry && !index.v ? entry.data : null);
  });
}

// The stops around a flick, kept before they're asked for: the next flick
// nearby finds its timetable on the phone, signal or not. Only after a flick,
// which has already asked for this place's stop, so it tells no one anything
// new; the background looks never ask for stop files. One file at a time,
// only those not already kept for this cut of the timetable.
var KEEP_NEAR = 400, KEEP_MAX = 24, keeping = false;
function keepNear(sys, lat, lon) {
  if (keeping || !sys || !sys.data || hubMode() === MODE_OFF) return;
  keeping = true;
  getIndex(sys, function (index) {
    if (!index) { keeping = false; return; }
    var tag = sys.agency.toLowerCase();
    var todo = index.stops.map(function (st) { return { id: st[0], d: metres(lat, lon, st[1], st[2]) }; })
      .filter(function (st) { return st.d <= KEEP_NEAR; })
      .sort(function (a, b) { return a.d - b.d; })
      .slice(0, KEEP_MAX)
      .filter(function (st) { return !stopKept(kept('hw2-stop-' + tag + '-' + st.id), index); });
    if (todo.length) console.log('headway: keeping ' + todo.length + ' stops nearby');
    (function next() {
      var st = todo.shift();
      if (!st) { keeping = false; return; }
      getStop(sys, index, st.id, function () { next(); });
    })();
  });
}

// A direction word that fits the board's second column: the compass word
// when the headsign has one, else as many of its words as fit in eight, or
// its first eight letters when the first word alone is longer.
function dirWord(head) {
  head = plain(head);   // HYRUM, not HYRUM,: the fonts have no comma
  var m = /\b(NORTH|SOUTH|EAST|WEST|IN|OUT)BOUND\b/.exec(head || '');
  if (m) return m[1].length <= 2 ? m[1] + 'BOUND' : m[1];
  var words = (head || '').trim().split(/\s+/), out = '';
  for (var i = 0; i < words.length && (out ? out.length + 1 : 0) + words[i].length <= 8; i++) out += (out ? ' ' : '') + words[i];
  return out || (head || '').trim().slice(0, 8);
}

// Live predictions, when the system names a relay for them. Only ever an
// overlay: whatever goes wrong here — no relay, no answer inside the wait,
// an error, a feed gone quiet — the answer is the schedule, as it always was.
var LIVE_WAIT = 3000, LIVE_FRESH = 90;
// A bus this close to its bay, by a position this fresh, is in and waiting.
var AT_BAY = 60, BUS_FRESH = 180;
// One request for a live answer, of either shape: the body, or null on any
// failure inside LIVE_WAIT. binary asks for a GTFS-realtime feed's bytes.
function askLive(url, binary, cb) {
  var done = false;
  function finish(v) { if (!done) { done = true; cb(v); } }
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  if (binary) req.responseType = 'arraybuffer';
  req.onload = function () {
    var body = binary ? req.response : req.responseText;
    finish(req.status === 200 && body && (!binary || typeof body !== 'string') ? body : null);
  };
  req.onerror = function () { finish(null); };
  req.timeout = LIVE_WAIT;
  req.ontimeout = function () { finish(null); };
  setTimeout(function () { finish(null); }, LIVE_WAIT + 250);   // not every phone honours XHR timeouts
  req.send();
}
// An answer fresh enough to lay over the schedule, else null. A feed that
// has stopped moving still answers, so its own clock is checked.
function fresh(d, what) {
  if (!d || !d.trips) return null;
  var age = Date.now() / 1000 - d.t;
  console.log('headway: ' + what + ' ' + Object.keys(d.trips).length + ' trips, age ' + Math.round(age) + 's');
  return age < LIVE_FRESH ? d : null;
}
// The relay, asked by stop ids.
function getLive(sys, ids, cb) {
  if (!sys.live) return cb(null);
  askLive(sys.live + '?stops=' + ids.join(','), false, function (text) {
    var d = null;
    try { d = text && JSON.parse(text); } catch (e) { /* the schedule it is */ }
    cb(fresh(d, 'live'));
  });
}
// A feed the phone reads itself (liveBy: 'feed'): the agency's own
// GTFS-realtime TripUpdate, where its server turns relays away and its
// predictions name no stop, only each trip's place in it. The bytes are
// asked for with the stop files; once those say which trips leave soon,
// only those are unpacked, into the relay's shape so the overlay is shared.
function readFeed(buf, want) {
  if (!buf) return null;
  try { return fresh(skimTrips(new Uint8Array(buf.buffer || buf), want), 'feed'); } catch (e) { return null; }
}

// A lean protobuf walk. Numbers stay plain numbers: the times and sequences
// fit well inside 2^53.
function walkPb(b, from, to, fn) {   // fn(field, wireType, valueOrStart, end)
  var i = from;
  function uv() { var v = 0, m = 1, c; do { c = b[i++]; v += (c & 0x7f) * m; m *= 128; } while (c & 0x80); return v; }
  while (i < to) {
    var k = uv(), f = Math.floor(k / 8), wt = k & 7;
    if (wt === 0) fn(f, 0, uv());
    else if (wt === 2) { var n = uv(); fn(f, 2, i, i + n); i += n; }
    else if (wt === 5) i += 4;
    else if (wt === 1) i += 8;
    else return;
  }
}
// { t: the header's time, 0 without one; trips: { id: { s: [[null, seq,
// time, rel]] } } }, a cancelled trip as { c: 1, s: [] }.
function skimTrips(b, want) {
  var out = { t: 0, trips: {} };
  walkPb(b, 0, b.length, function (f, wt, s, e) {
    if (f === 1 && wt === 2) walkPb(b, s, e, function (g, w, v) { if (g === 3 && w === 0) out.t = v; });
    if (f !== 2 || wt !== 2) return;
    walkPb(b, s, e, function (g, w, s2, e2) {
      if (g !== 3 || w !== 2) return;   // the entity's trip_update
      var id = null, seqs = null, cancelled = false, hits = [];
      walkPb(b, s2, e2, function (h, w3, s3, e3) {
        if (h === 1 && w3 === 2) walkPb(b, s3, e3, function (k, w4, s4, e4) {
          if (k === 1 && w4 === 2) {
            id = '';
            for (var j = s4; j < e4; j++) id += String.fromCharCode(b[j]);
            seqs = want[id] || null;
          } else if (k === 4 && w4 === 0 && s4 === 3) cancelled = true;   // schedule_relationship: CANCELED
        });
        else if (h === 2 && w3 === 2 && seqs) {
          var seq = null, arr = null, dep = null, rel = 0;
          walkPb(b, s3, e3, function (k, w5, v5, e5) {
            if (k === 1 && w5 === 0) seq = v5;
            else if ((k === 2 || k === 3) && w5 === 2) walkPb(b, v5, e5, function (m, w6, v6) {
              if (m === 2 && w6 === 0) { if (k === 3) dep = v6; else arr = v6; }
            });
            else if (k === 5 && w5 === 0) rel = v5;
          });
          var time = dep || arr;
          if (seq !== null && seqs[seq] && (time || rel === 1)) hits.push([null, seq, time || 0, rel]);
        }
      });
      if (id && seqs && cancelled) out.trips[id] = { c: 1, s: [] };
      else if (id && hits.length) out.trips[id] = { s: hits };
    });
  });
  return out;
}

// Route order for ties and within a wave: numbers by number, then the
// lettered routes in the agency's own order (G before B, as the chips read).
var routeRank = {};
function routeKey(r) {
  var n = parseInt(r, 10);
  return isNaN(n) ? 1000 + (routeRank[r] || 0) : n;
}
function byRoute(a, b) { return routeKey(a) - routeKey(b) || (a < b ? -1 : a > b ? 1 : 0); }

// The services running on a date, from the agency's own calendar: a weekly
// pattern between two dates, and dates added or taken away. Each service is
// [weekmask Mon..Sun, "start", "end", [added], [removed]], dates YYYYMMDD.
function ymd(d) {
  var m = d.getMonth() + 1, day = d.getDate();
  return d.getFullYear() + (m < 10 ? '0' : '') + m + (day < 10 ? '0' : '') + day;
}
function activeServices(services, d) {
  var key = ymd(d), bit = 1 << ((d.getDay() + 6) % 7), on = {};
  (services || []).forEach(function (sv, i) {
    var weekly = (sv[0] & bit) && sv[1] <= key && key <= sv[2] && sv[4].indexOf(key) < 0;
    if (weekly || sv[3].indexOf(key) >= 0) on[i] = true;
  });
  return on;
}

// Times go to the watch as minutes past midnight; the watch writes them in
// its own clock style, 12-hour with A or P, or 24-hour.

// The watch keeps 23 characters of a stop's name; cut at a word, never in one.
// Names go through plain() first: the nightly data keeps the agency's '/' and
// '-', which basalt's hand-drawn caption font has no letters for.
function fitName(name) {
  name = plain(name, true);
  if (name.length <= 23) return name;
  var cut = name.slice(0, 24).lastIndexOf(' ');
  return name.slice(0, cut > 0 ? cut : 23);
}

// The transit state worked out from a flick's own fix, sent with its answer
// in the same message rather than a second one hard on its heels.
var flickTransit = null;
function withTransit(msg) {
  if (flickTransit) {
    var t = transitMsg(flickTransit);
    Object.keys(t).forEach(function (k) { msg[k] = t[k]; });
    console.log('headway: ' + flickTransit.why + ' (flick)');
    flickTransit = null;
  }
  return msg;
}

// Within HERE_M of a stop the wearer is at it: a fix on the sidewalk by one
// reads fifty or eighty metres off as often as not.
var HERE_M = 100;

// A flick's answer, sent at most once as it stands: the timetable, then the
// live answer only where it says something new. lastAnswer is cleared by each
// flick.
var lastAnswer = null;
function sendAnswer(msg) {
  var key = JSON.stringify(Object.keys(msg).filter(function (k) { return k.indexOf('SV_') === 0; }).map(function (k) { return [k, msg[k]]; }));
  if (key === lastAnswer) return console.log('headway: live answer changes nothing');
  lastAnswer = key;
  Pebble.sendAppMessage(withTransit(msg));
}
// The stop's list: the stop, how far (0 at it), and its next departures,
// CD_MAX at most, a line each: route|epoch seconds|colour|live. The watch
// counts down to the first not yet gone, moves on as each goes, and steps
// through the routes on a second flick. The nearest stop's departures come
// first, by time, then those of the stops the fix's accuracy adds: the
// countdown is for the stop you're at, not one across the park with a sooner
// bus.
var CD_MAX = 8;
// Where the countdown's stop is, for the half-hourly look to end it once the
// wearer has gone; null with none running.
var cdStop = null, CD_GONE = 400;
// How near a stop must be to count down to it: a flick further from every
// stop than this shows only its seconds. The wearer's to set.
var CD_RANGE_DEFAULT = 400;
function cdRange() { return Number(settings().CD_RANGE) || CD_RANGE_DEFAULT; }
function departures(deps, dayMs, colour) {
  var out = [], seen = {};
  deps.filter(function (d) { return d.near; }).concat(deps.filter(function (d) { return !d.near; })).forEach(function (d) {
    var key = d.route + '|' + d.t;
    if (out.length >= CD_MAX || seen[key]) return;
    seen[key] = true;
    out.push([d.route.slice(0, 7), Math.round(dayMs / 1000) + d.t * 60, colour(d.route) & 0xFFFFFF, d.live ? 1 : 0].join('|'));
  });
  return out;
}
// quiet: the watch asked again near the end, not a flick; nothing lights.
function sendList(name, dist, list, liveSys, at, quiet) {
  console.log('headway: ' + (name || '(none)') + ' ' + Math.round(dist) + ' m, ' + list.length + ' departures' + (quiet ? ' (again)' : ''));
  var msg = { SV_STOP: fitName(name), SV_DIST: Math.round(dist), SV_N: list.length, SV_MODE: 0, SV_LIST: list.join('\n'), SV_LIVE: liveSys ? 1 : 0 };
  if (quiet) msg.SV_QUIET = 1;
  cdStop = list.length && at ? at : null;
  sendAnswer(msg);
}
// Nothing to answer: the countdown ends, unless the phone simply got no fix.
function sendNothing(keep) {
  console.log('headway: ' + (keep ? 'no fix; the last answer stands' : 'nothing to answer'));
  if (!keep) cdStop = null;
  sendAnswer(keep ? { SV_N: 0, SV_KEEP: 1 } : { SV_N: 0 });
}

// At the hub the answer is by time, not by route: a line a departure minute,
// every route leaving then as a badge in route order. SV_R holds the labels
// comma-separated (a label may hold a space: 16 AM),
// SV_K their colours three bytes a badge, SV_T a bit a badge for live, SV_O a
// bit a badge for a bus not in at its bay yet.
function sendWaves(name, waves) {
  console.log('headway: hub view ' + waves.map(function (w) {
    return w.when + ' [' + w.routes.map(function (r, j) { return r + ((w.live >> j) & 1 ? '~' : '') + ((w.away >> j) & 1 ? '(away)' : ''); }).join(' ') + ']';
  }).join(', '));
  var msg = { SV_STOP: fitName(name), SV_DIST: 0, SV_N: waves.length, SV_MODE: 1 };
  waves.forEach(function (w, i) {
    var k = [];
    w.colors.forEach(function (c) { k.push((c >> 16) & 255, (c >> 8) & 255, c & 255); });
    msg['SV_R' + (i + 1)] = w.routes.join(',');
    msg['SV_W' + (i + 1)] = w.when;
    msg['SV_K' + (i + 1)] = k;
    msg['SV_T' + (i + 1)] = w.live;
    msg['SV_O' + (i + 1)] = w.away;
  });
  sendAnswer(msg);
}

// Which routes have a bus in at the hub: a fresh live position within AT_BAY
// of any bay, on a trip of that route. The bays stand too close together to
// tell apart by position, so the trip says whose bus it is: from the stop
// files where the trip is one of ours, else from the agency's trip id (3_1030
// and 3S_1030 are route 3's, B1_0625 is B's), for a bus still on its way in.
// Null when there is no live answer, so nothing is ever said to be away on no
// news.
function routesIn(group, live, stops) {
  if (!live || !live.at) return null;
  var tripRoute = {}, nowS = Date.now() / 1000, out = {};
  stops.forEach(function (stop) {
    (stop.deps || []).forEach(function (d) { if (d[3]) tripRoute[d[3]] = d[1]; });
  });
  live.at.forEach(function (b) {
    if (!(nowS - b[2] < BUS_FRESH) || !b[3]) return;
    if (!group.some(function (st) { return metres(b[0], b[1], st.lat, st.lon) <= AT_BAY; })) return;
    out[routeStem(tripRoute[b[3]] || b[3])] = true;
  });
  return out;
}
function routeColour(index, r) { return parseInt((index.routes[r] || ['888888'])[0], 16); }
// A route's stem, to match a trip id's: 16 AM and 16 PM are 16, B1 is B.
function routeStem(r) { var m = /^(\d+|[A-Z])/.exec(r || ''); return m ? m[1] : r; }

// The hub's answer: the next three departure minutes, each with every route
// leaving then, once, in route order, twelve at most. After the day's last
// bus the first line carries the day word. With live positions the hub's
// flick shows which buses are in instead (hubChips).
function waves(deps, dayWord, index) {
  var out = [], byMin = {};
  deps.forEach(function (dep) {
    var w = byMin[dep.t];
    if (!w) {
      if (out.length === 3) return;
      w = byMin[dep.t] = { t: dep.t, list: [], seen: {} };
      out.push(w);
    }
    if (w.seen[dep.route]) { w.seen[dep.route].live = w.seen[dep.route].live || dep.live; return; }
    if (w.list.length === 12) return;
    w.list.push(w.seen[dep.route] = { route: dep.route, live: dep.live });
  });
  return out.map(function (w, i) {
    w.list.sort(function (a, b) { return byRoute(a.route, b.route); });
    var bits = 0;
    w.list.forEach(function (x, j) { if (x.live) bits |= 1 << j; });
    return {
      when: String(w.t) + (i === 0 && dayWord ? ' ' + dayWord : ''),
      routes: w.list.map(function (x) { return x.route; }),
      colors: w.list.map(function (x) { return routeColour(index, x.route); }),
      live: bits,
      // With no word from the feed on which buses are in, none is claimed:
      // colour on the board means a bus at its bay, so every badge is grey.
      away: (1 << w.list.length) - 1,
    };
  });
}

// A flick at the hub with live positions: which routes have a bus in. Every
// route that leaves the hub on the day as a badge on one line, in route
// order, solid where its bus is at a bay now and hollow where it isn't; then
// the loops' next departures, a line each (one if they leave together), as
// the room allows. The line of routes has no time, only the day word when
// the next bus is another day's.
function hubChips(deps, dayWord, index, stops, inNow, loops, onDay) {
  var seen = {}, chips = [];
  stops.forEach(function (stop) {
    (stop.deps || []).forEach(function (row) {
      if (!onDay[row[4]]) return;
      var stem = routeStem(row[1]);
      if (!seen[stem]) { seen[stem] = true; chips.push({ stem: stem, label: row[1] }); }
    });
  });
  chips.sort(function (a, b) { return byRoute(a.label, b.label); });
  chips = chips.slice(0, 16);   // the watch's WV_MAX
  var away = 0;
  chips.forEach(function (c, j) { if (!inNow[c.stem]) away |= 1 << j; });
  var line = {
    when: dayWord ? ' ' + dayWord : '',
    routes: chips.map(function (c) { return c.stem; }),
    colors: chips.map(function (c) { return routeColour(index, c.label); }),
    live: 0,
    away: away,
  };
  // Each loop's next departure, by time as the hub's lines always are.
  var next = {};
  var firsts = deps.filter(function (d) {
    if ((loops || []).indexOf(d.route) < 0 || next[d.route]) return false;
    return (next[d.route] = true);
  });
  // The loops' lines are in colour, like the chips, only where the bus is in.
  return [line].concat(waves(firsts, '', index).slice(0, 2).map(function (w) {
    w.away = 0;
    w.routes.forEach(function (r, j) { if (!inNow[routeStem(r)]) w.away |= 1 << j; });
    return w;
  }));
}

// A departure's minute on the day whose services are onDay, the evening
// before's being onEve (GTFS writes 12:53 AM as 24:53 on the evening's
// service), or -1 when it doesn't run then.
function depMinute(row, onDay, onEve) {
  return row[0] >= 1440 && onEve[row[4]] ? row[0] - 1440 : onDay[row[4]] ? row[0] : -1;
}

// The trips worth unpacking from a feed, by trip and place in it: today's
// departures from these stops in the next ninety minutes, as { trip: { seq: true } }.
function tripsSoon(stops, services, now, nowMin) {
  var today = activeServices(services, now);
  var eve = activeServices(services, new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1));
  var want = {};
  stops.forEach(function (stop) {
    (stop.deps || []).forEach(function (row) {
      if (!row[3] || row[5] === undefined) return;
      var t = depMinute(row, today, eve);
      if (t >= nowMin - 1 && t <= nowMin + 90) (want[row[3]] = want[row[3]] || {})[row[5]] = true;
    });
  });
  return want;
}

// A flick's answer from its departures, whoever's timetable they come
// from: deps by time, each {t, route, head, live, near, sid}; dayMs the
// midnight of the day they're on; best the nearest stop; liveSys whether
// the system has live times; quiet for a re-check the watch asked for.
function answerStop(o) {
  // Within the fix's own accuracy of the stop, the wearer may well be at it.
  var dist = o.best.d <= Math.max(HERE_M, o.reach) ? 0 : o.best.d;
  if (o.best.d > cdRange()) {
    console.log('headway: nearest stop ' + Math.round(o.best.d) + ' m off, past the range of ' + cdRange());
    return sendNothing(false);
  }
  var list = departures(o.deps, o.dayMs, o.colour);
  if (!list.length) return sendNothing(false);
  sendList(o.title, dist, list, o.liveSys, { lat: o.best.lat, lon: o.best.lon }, o.quiet);
}

// ---- Transitous: a flick anywhere, for those who turn it on
// Off by default, and staying so: the agency's own timetable answers
// wherever the face knows one, and Transitous (MOTIS over every open feed,
// transitous.org) only outside them. It is asked for the departures within
// ANY_RADIUS of the fix rounded to three places (about 100 m), and a stop
// the wearer is at, if the first answer is thin there, by that stop alone.
var ANY_URL = 'https://api.transitous.org/api/v6/stoptimes', ANY_RADIUS = 500, ANY_N = 24, ANY_STOP_N = 12;
var ANY_WAIT = 6000, VERSION = require('../../package.json').version;
// A switch to hold it back whatever was saved, as before Transitous had heard
// from us (1.23.2); the watch keeps its own.
var ANY_READY = true;
function anywhere() {
  if (!ANY_READY) return false;
  var v = settings().ANYWHERE;
  return v === true || v === 1 || v === '1' || v === 'true';
}
function askAnywhere(query, cb) {
  var done = false;
  function finish(v) { if (!done) { done = true; cb(v); } }
  var req = new XMLHttpRequest();
  req.open('GET', ANY_URL + '?' + query, true);
  // Transitous asks every client to say who it is, in the User-Agent. The
  // phone's web view drops a User-Agent set by a page without a word (Core's
  // Android app sends its own, and no Referer), so the same words go in
  // X-Client too, which it does send.
  var who = 'Headway/' + VERSION + ' (support@cacherider.com; https://github.com/sybenx/headway)';
  try { req.setRequestHeader('User-Agent', who); } catch (e) {}
  req.setRequestHeader('X-Client', who);
  req.onload = function () {
    var d = null;
    try { d = req.status === 200 ? JSON.parse(req.responseText) : null; } catch (e) {}
    finish(d && d.stopTimes ? d.stopTimes : null);
  };
  req.onerror = function () { finish(null); };
  req.timeout = ANY_WAIT;
  req.ontimeout = function () { finish(null); };
  setTimeout(function () { finish(null); }, ANY_WAIT + 250);
  req.send();
}
// Names from anywhere in the world, in the letters the face's own fonts
// draw: capitals, digits and a little punctuation. Accents fold away,
// apostrophes go, a slash or a plus reads as and, and the rest is dropped.
function plain(text, keep) {
  var t = String(text || '').toUpperCase();
  try { t = t.normalize('NFD').replace(/[\u0300-\u036f]/g, ''); } catch (e) {}
  t = t.replace(/\u00df/g, 'SS').replace(/\u00c6/g, 'AE').replace(/\u00d8/g, 'O').replace(/\u0152/g, 'OE')
    .replace(/\u0141/g, 'L').replace(/\u0110/g, 'D').replace(/\u00de/g, 'TH').replace(/\u0131/g, 'I');
  t = t.replace(/['\u2019]/g, '');   // MCDONALDS, not MCDONALD S
  if (keep) t = t.replace(/\s*[\/+]\s*/g, ' & ');
  return t.replace(keep ? /[^A-Z0-9&.: ]+/g : /[^A-Z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim();
}
// A route's badge: its short name, else the first word of its long one.
function anyRoute(st) {
  return plain(st.routeShortName || st.displayName || st.routeLongName).split(' ')[0].slice(0, 6) || '?';
}
function flickAnywhere(pos) {
  var lat = pos.coords.latitude, lon = pos.coords.longitude;
  var reach = Math.min(Math.max(pos.coords.accuracy || 0, 0), MAX_REACH);
  var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
  var midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  console.log('headway: asking Transitous');
  askAnywhere('center=' + lat.toFixed(3) + ',' + lon.toFixed(3) + '&radius=' + ANY_RADIUS + '&n=' + ANY_N, function (got) {
    if (!got || !got.length) return sendNothing(false);
    var byId = {}, ranked = [], colours = {}, seen = {}, all = [];
    function take(list) {
      list.forEach(function (st) {
        var p = st.place || {};
        if (!p.stopId || st.cancelled || st.tripCancelled || p.cancelled || p.pickupType === 'NOT_ALLOWED') return;
        var key = st.tripId + '|' + p.stopId;
        if (seen[key]) return;
        seen[key] = true;
        if (!byId[p.stopId]) {
          byId[p.stopId] = { id: p.stopId, name: plain(p.name, true) };
          ranked.push({ id: p.stopId, d: metres(lat, lon, p.lat, p.lon), lat: p.lat, lon: p.lon, station: plain(p.name, true) });
        }
        var route = anyRoute(st);
        if (!colours[route]) colours[route] = parseInt(st.routeColor || '888888', 16) || 0x888888;
        var at = new Date(p.departure || p.scheduledDeparture).getTime();
        if (isNaN(at)) return;
        all.push({ ms: at, route: route, head: plain(st.headsign, true), live: !!st.realTime, sid: p.stopId });
      });
    }
    function answer() {
      ranked.sort(function (a, b) { return a.d - b.d; });
      var best = ranked[0];
      if (best.d > cdRange()) {
        console.log('headway: nearest stop ' + Math.round(best.d) + ' m off, past the range of ' + cdRange());
        return sendNothing(false);
      }
      var here = best.d <= Math.max(HERE_M, reach);
      var near = {};
      // The stop's twin across the road, and the stops Transitous names the
      // same (a station's platforms), go with it.
      ranked.forEach(function (st) {
        if (metres(best.lat, best.lon, st.lat, st.lon) <= TWIN || st.station === best.station) near[st.id] = true;
      });
      var group = {};
      ranked.forEach(function (st) {
        if (near[st.id] || st.d <= best.d + reach) group[st.id] = true;
      });
      // Today's departures; when there are none, the first day ahead with
      // any, with its word, as for the face's own systems.
      var deps = [], dayWord = '';
      all.sort(function (a, b) { return a.ms - b.ms; });
      var first = all.filter(function (d) { return group[d.sid] && Math.floor((d.ms - midnight) / 60000) >= nowMin - 1; })[0];
      var ahead = first ? Math.max(0, Math.floor((first.ms - midnight) / 86400000)) : 0;
      all.forEach(function (d) {
        var t = Math.floor((d.ms - midnight) / 60000) - ahead * 1440;
        if (!group[d.sid] || t < (ahead ? 0 : nowMin - 1) || t >= 1440) return;
        deps.push({ t: t, route: d.route, head: d.head, live: d.live, near: !!near[d.sid], sid: d.sid });
      });
      if (!deps.length) return sendNothing(false);
      if (ahead) dayWord = ahead === 1 ? 'TOMORROW' : ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][new Date(first.ms).getDay()];
      deps.sort(function (a, b) { return a.t - b.t || (a.route < b.route ? -1 : a.route > b.route ? 1 : 0); });
      answerStop({ deps: deps, dayMs: midnight + ahead * 86400000, title: byId[best.id].name, best: best, reach: reach,
        liveSys: false, colour: function (r) { return colours[r] || 0x888888; } });
    }
    take(got);
    var best = ranked.slice().sort(function (a, b) { return a.d - b.d; })[0];
    var atIt = best && best.d <= Math.max(HERE_M, reach);
    var there = all.filter(function (d) { return best && d.sid === best.id; }).length;
    if (!atIt || there >= 3) return answer();
    // At a stop the first answer spread thin: ask again by the stop alone,
    // which sends no location at all.
    askAnywhere('stopId=' + encodeURIComponent(best.id) + '&n=' + ANY_STOP_N, function (more) {
      if (more) take(more);
      answer();
    });
  });
}

// The last flick's fix, for the watch's re-check near the end of a countdown.
var lastFix = null;
function onFlick() {
  // The watch only asks when its own setting allows, so no gate here.
  lastAnswer = null;
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    console.log('headway: fix ' + lat.toFixed(4) + ',' + lon.toFixed(4) + ' +-' + Math.round(pos.coords.accuracy) + 'm');
    flickTransit = transitState(lat, lon);
    // The flick's fix answers the background question too.
    if (sawFix(lat, lon)) weatherAt(pos, true);
    lastFix = { coords: { latitude: lat, longitude: lon, accuracy: pos.coords.accuracy } };
    answerAt(pos, false);
  }, function (err) {
    console.log('headway: no fix ' + (err && err.message));
    sendNothing(true);
  }, { timeout: 9000, maximumAge: 20000 });
}
// Where the system has live times, the watch asks again at five minutes and
// at one: the same stops from the last flick's fix, nothing lit, no new fix.
function onCdCheck() {
  if (!lastFix || !cdStop) return;
  lastAnswer = null;
  answerAt(lastFix, true);
}
function answerAt(pos, quiet) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    var sys = systemAt(lat, lon);
    // Outside every system the face knows, Transitous answers, if the
    // wearer has turned it on; never by default.
    var elsewhere = function () { return !quiet && anywhere() ? flickAnywhere(pos) : sendNothing(quiet); };
    if (!sys || !sys.data) return elsewhere();
    getIndex(sys, function (index, old) {
      if (!index) return sendNothing(true);
      Object.keys(index.routes || {}).forEach(function (r, i) { routeRank[r] = i; });
      var ranked = index.stops.map(function (st) {
        return { id: st[0], d: metres(lat, lon, st[1], st[2]), lat: st[1], lon: st[2], station: st[3] };
      }).sort(function (a, b) { return a.d - b.d; });
      if (!ranked.length || ranked[0].d > FAR) return elsewhere();
      var best = ranked[0];
      // A fix is only as good as the phone says: every stop within its
      // accuracy of the nearest could be the one the wearer is at, so all of
      // them answer, and so do twins across a road and a station: every stop
      // the index marks with the nearest one's station number. At the hub,
      // every bay does: the group is the whole hub, and it goes by the hub's
      // own name rather than whichever bay happened to be nearest.
      var reach = Math.min(Math.max(pos.coords.accuracy || 0, 0), MAX_REACH);
      var atHub = sys.hub && metres(best.lat, best.lon, sys.hub.lat, sys.hub.lon) <= HUB;
      // At the yard the question is whether the buses are back, and the
      // phone's looks answer it on the watch; a flick there asks for a look
      // now, and answers nothing itself.
      if (sys.base && metres(lat, lon, sys.base.lat, sys.base.lon) <= sys.base.r) {
        if (!quiet) yardTick(true);
        return sendNothing(false);
      }
      // A stop further off than the range isn't the wearer's: nothing to
      // count to, and a countdown left from one that was ends.
      if (!atHub && best.d > cdRange()) {
        console.log('headway: nearest stop ' + Math.round(best.d) + ' m off, past the range of ' + cdRange());
        return sendNothing(false);
      }
      // The answer is the nearest stop's buses, at it or on the way to it.
      var group = atHub
        ? ranked.filter(function (st) { return metres(sys.hub.lat, sys.hub.lon, st.lat, st.lon) <= HUB; }).slice(0, 16)
        : ranked.filter(function (st) {
            return st.d <= best.d + reach || metres(best.lat, best.lon, st.lat, st.lon) <= TWIN || (best.station && st.station === best.station);
          }).slice(0, 16);
      var near = {};
      group.forEach(function (st) {
        if (atHub || metres(best.lat, best.lon, st.lat, st.lon) <= TWIN || (best.station && st.station === best.station)) near[st.id] = true;
      });
      var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
      var midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
      var byFeed = sys.liveBy === 'feed', feed = null, live = null;
      var left = group.length, name = atHub ? sys.hub.name : '', stops = [];
      // The live answer is asked for alongside the schedule. The schedule
      // never waits on it: the timetable goes to the watch as soon as the
      // stop files are in, and a live answer that comes inside LIVE_WAIT
      // redraws it, if it changes anything.
      var gotLive = function () { if (!left && (feed || live)) answer(); };
      if (byFeed) askLive(sys.live, true, function (buf) { feed = buf; gotLive(); });
      else getLive(sys, group.map(function (st) { return st.id; }), function (l) { live = l; gotLive(); });
      group.forEach(function (st) {
        getStop(sys, index, st.id, function (stop) {
          // The board goes by the nearest stop's name, whichever file comes first.
          if (stop) { if (!atHub && (!name || st.id === best.id)) name = stop.name; stop.id = stop.id || st.id; stops.push(stop); }
          if (!--left) answer();
        });
      });
      // Once the answer and its live word are in, the stops around are kept
      // for the next flick.
      if (!quiet) setTimeout(function () { keepNear(sys, lat, lon); }, LIVE_WAIT + 1000);
      function answer() {
        if (byFeed && feed && !live) live = readFeed(feed, tripsSoon(stops, index.services, now, nowMin));
        // Today's remaining departures; when there are none, the first
        // day ahead with any — tomorrow, or Monday after a Saturday —
        // so the answer is the next bus, whenever that is.
        var deps = [], dayWord = '';
        for (var ahead = 0; ahead < 8 && !deps.length; ahead++) {
          var date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + ahead);
          // A day's buses are its own services' departures, and the ones the
          // day before's services run past midnight. Today's list keeps the
          // minute just gone: a bus due a minute ago may still be pulling
          // in, and the watch reads it as NOW.
          var onDay = activeServices(index.services, date);
          var onEve = activeServices(index.services, new Date(date.getFullYear(), date.getMonth(), date.getDate() - 1));
          var from = ahead ? 0 : nowMin - 1;
          stops.forEach(function (stop) {
            (stop.deps || []).forEach(function (row) {
              var m = depMinute(row, onDay, onEve);
              if (m < 0) return;
              var dep = [m, row[1], row[2], row[3]];
              // Today's departures take the live prediction for this trip
              // at this stop where there is one. A stop the bus will skip
              // drops out; at the hub a bus is held to its time, never
              // early. Elsewhere an early time stands, a timepoint's too:
              // most early buses wait there, but showing one held costs the
              // wearer the bus the times it doesn't, and early only a wait.
              // No prediction: the schedule, unmarked.
              var t = dep[0], isLive = false, at = null;
              var trip = !ahead && live && dep[3] && live.trips[dep[3]];
              // A run the feed says is cancelled isn't coming.
              if (trip && trip.c) return;
              // By the stop and the departure's place in its trip: most of
              // Connect's trips leave from and come back to the same bay, so
              // the stop alone would take the arrival back for the departure.
              // A feed that names no stop is matched by place alone; one that
              // gives no place, by the stop where the trip passes it once.
              var once = null, seen = 0;
              for (var i = 0; trip && i < trip.s.length; i++) {
                var pr = trip.s[i];
                if (pr[0] !== null && pr[0] !== stop.id) continue;
                if (pr[1] !== null && pr[1] !== undefined && row[5] !== undefined) { if (pr[1] === row[5]) at = pr; }
                else if (pr[0] !== null) { once = pr; seen++; }
              }
              if (!at && seen === 1) at = once;
              if (at) {
                if (at[3] === 1) return;
                var p = Math.floor((at[2] * 1000 - midnight) / 60000);
                // A bus still listed at its hub bay after the feed's time for
                // it is still there, boarding: it leaves now, not already. For
                // ten minutes past its time on a loop, thirty on a route: a
                // listing older than that is the feed's leftover, not a bus.
                var past = now.getTime() - at[2] * 1000;
                var boarding = past > 30000 && past < ((sys.routes || []).indexOf(dep[1]) >= 0 ? 600000 : 1800000) ? nowMin : -1;
                t = atHub ? Math.max(t, p, boarding) : p; isLive = true;
              }
              if (t >= from) deps.push({ t: t, route: dep[1], head: dep[2], live: isLive, near: !!near[stop.id], sid: stop.id });
            });
          });
          if (deps.length && ahead) dayWord = ahead === 1 ? 'TOMORROW' : ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][date.getDay()];
        }
        // Ties in route order, never in whichever file happened to load first.
        deps.sort(function (a, b) { return a.t - b.t || byRoute(a.route, b.route); });
        // A timetable kept from days ago, with no network to check it, says so
        // first: the stop's name keeps its first words however it's cut.
        var title = old && name ? 'OLD ' + name : name;
        // At the hub the countdown is the hub's already; a flick there asks
        // the one thing it can't say, which buses are in, and with no live
        // word says nothing. Either way a stop's countdown ends there.
        if (atHub) {
          var inNow = routesIn(group, live, stops);
          if (!inNow) return sendNothing(false);
          // onDay: the services of the day these departures are from.
          return sendWaves(title, hubChips(deps, dayWord, index, stops, inNow, sys.routes, onDay));
        }
        answerStop({ deps: deps, dayMs: date.getTime(), title: title, best: best, reach: reach,
          liveSys: !!sys.live, quiet: quiet, colour: function (r) { return routeColour(index, r); } });
      }
    });
}

// The watch asks for a look as the wearer walks near a hub; the phone takes
// it only while the hub's buses run.
function onLook() {
  if (hubMode() === MODE_OFF || !(lastSys && lastSys.hub && hubRunning(lastSys))) return;
  if (Date.now() - lastLook < 60 * 1000) return;
  console.log('headway: look (walking near the hub)');
  lastLook = 0;
  look(false, lastHubM <= FRESH_NEAR);
}

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.FLICK) { console.log('headway: flick'); onFlick(); }
  else if (e.payload && e.payload.CD_CHECK) { console.log('headway: the watch asks again'); onCdCheck(); }
  else if (e.payload && e.payload.LOOK) onLook();
  else if (e.payload && e.payload.YARD) { console.log('headway: yard asked by the watch'); yardTick(false); }
});

Pebble.addEventListener('ready', function () { look(true); });
Pebble.addEventListener('webviewclosed', function () {
  // Settings may have switched weather or transit on, or changed units.
  setTimeout(function () { look(true); }, 500);
});
setInterval(function () { look(false); }, 5 * 60 * 1000);

// ---- the yard, coming home: when to close the gates. From the day's last
// trip, at the yard its buses sleep in, the watch shows how many buses are
// still moving and, once none are, how long since the last one stopped, with
// no flick. The feed is no help on who is logged off: it keeps re-stamping a
// bus's last fix with the current time, so on a Sunday night every bus reads
// fresh, parked at the bays where it logged off the day before. What a
// logged-off bus cannot do is move. So each look is compared with the one
// before: a bus whose position has changed by YARD_MOVED since is still
// coming, one that hasn't is in, or as good as. Fifteen minutes after the
// last one stops the block goes solid: close the gates.
// The window is the timetable's: the latest end of the services running that
// day (or, past midnight, the day before), and YARD_HOUR after it. Only a
// phone last seen within YARD_NEAR of the yard looks, every two minutes;
// anywhere else the half-hourly look (or a flick there) has to bring it near.
var YARD_HOUR = 90, YARD_EVERY = 2 * 60 * 1000, YARD_NEAR = 1000, YARD_MOVED = 40, lastBaseM = Infinity;
var yard = { told: null, asked: 0, start: 0, last: null, lastAt: 0, lastMoved: 0 };
// A bus's name across looks: its trip, else its route from the tracker site.
function busKey(b) { return b[3] || (b[4] ? 'r' + b[4] : ''); }
// This look against the one before: how many buses have moved, and when any
// last did. Null on the first look of a window, with nothing to compare.
function yardLook(live, w) {
  var now = Date.now() / 1000, cur = {};
  (live.at || []).forEach(function (b) { var k = busKey(b); if (k) cur[k] = b; });
  var verdict = null;
  if (yard.last) {
    var moving = 0;
    Object.keys(cur).forEach(function (k) {
      var p = yard.last[k];
      if (!p || metres(p[0], p[1], cur[k][0], cur[k][1]) > YARD_MOVED) moving++;
    });
    if (moving) yard.lastMoved = now;
    verdict = { moving: moving, lastMoved: yard.lastMoved };
  } else {
    yard.lastMoved = now;   // offline since at least now, for all the phone knows
  }
  yard.last = cur; yard.lastAt = now;
  return verdict;
}
// [start, until] in epoch seconds around now, or null outside it.
function yardWindow(index, now) {
  var ends = index.ends || [];
  var midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime() / 1000;
  var cands = [];
  [0, -1].forEach(function (back) {
    var day = new Date(now.getFullYear(), now.getMonth(), now.getDate() + back);
    var on = activeServices(index.services, day), end = -1;
    Object.keys(on).forEach(function (i) { if (on[i] && ends[i] > end) end = ends[i]; });
    if (end >= 0) cands.push(midnight + (back * 1440 + end) * 60);
  });
  var t = now.getTime() / 1000;
  for (var i = 0; i < cands.length; i++) {
    if (t >= cands[i] && t < cands[i] + YARD_HOUR * 60) return { start: cands[i], until: cands[i] + YARD_HOUR * 60 };
  }
  return null;
}
// Today's window ahead or under way (null when there's none left today), for
// the watch to keep: through it the watch asks the phone to look, every two
// minutes, while the phone was last seen near the yard. A message from the
// watch wakes the phone's script where its own timer may be held back.
function yardAhead(index, now) {
  var ends = index.ends || [], midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime() / 1000;
  var t = now.getTime() / 1000, best = null;
  [-1, 0].forEach(function (back) {
    var day = new Date(now.getFullYear(), now.getMonth(), now.getDate() + back);
    var on = activeServices(index.services, day), end = -1;
    Object.keys(on).forEach(function (i) { if (on[i] && ends[i] > end) end = ends[i]; });
    var start = midnight + (back * 1440 + end) * 60;
    if (end >= 0 && t < start + YARD_HOUR * 60 && !best) best = { start: start, until: start + YARD_HOUR * 60 };
  });
  return best;
}
var windowTold = null;
function tellYardWindow() {
  var sys = lastSys;
  if (!sys || !sys.base || hubMode() === MODE_OFF) return;
  getIndex(sys, function (index) {
    var w = index && index.ends && yardAhead(index, new Date());
    var msg = { YD_FROM: w ? Math.round(w.start) : 0, YD_UNTIL: w ? Math.round(w.until) : 0, YD_NEAR: lastBaseM <= YARD_NEAR ? 1 : 0 };
    var key = JSON.stringify(msg);
    if (key === windowTold) return;
    windowTold = key;
    console.log('headway: yard window ' + key);
    Pebble.sendAppMessage(msg);
  });
}
function tellYard(msg) {
  var key = JSON.stringify(msg);
  if (key === yard.told) return;
  yard.told = key;
  console.log('headway: yard ' + key);
  Pebble.sendAppMessage(msg);
}
// force: a flick at the yard asks for a look now.
function yardTick(force) {
  var sys = lastSys;
  if (!sys || !sys.base || hubMode() === MODE_OFF || lastBaseM > YARD_NEAR) return yardOff();
  getIndex(sys, function (index) {
    var w = index && index.ends && yardWindow(index, new Date());
    if (!w) return yardOff();
    if (w.start !== yard.start) { yard.start = w.start; yard.last = null; yard.lastMoved = 0; }
    if (!force && Date.now() - yard.asked < YARD_EVERY) return;
    yard.asked = Date.now();
    navigator.geolocation.getCurrentPosition(function (pos) {
      var lat = pos.coords.latitude, lon = pos.coords.longitude;
      sawFix(lat, lon);
      if (metres(lat, lon, sys.base.lat, sys.base.lon) > sys.base.r) return yardOff();
      // The relay answers for a stop; the base's nearest will do, since only
      // where the buses are is wanted.
      var near = null;
      index.stops.forEach(function (st) {
        var d = metres(sys.base.lat, sys.base.lon, st[1], st[2]);
        if (!near || d < near.d) near = { id: st[0], d: d };
      });
      getLive(sys, [near.id], function (live) {
        if (!live || !live.at) return;   // no news: the watch keeps its last word
        var v = yardLook(live, w);
        if (!v) return;   // the first look: nothing to compare yet
        console.log('headway: yard ' + v.moving + ' moving, last moved ' + Math.round(Date.now() / 1000 - v.lastMoved) + ' s ago');
        tellYard({ YD_OUT: v.moving, YD_END: v.moving ? 0 : Math.round(v.lastMoved), YD_UNTIL: Math.round(w.until) });
      });
    }, function () { /* no fix: the watch keeps its last word until the window ends */ },
    { timeout: 10000, maximumAge: 4 * 60 * 1000 });
  });
}
function yardOff() {
  if (yard.told !== null && yard.told !== '{"YD_OUT":-1}') tellYard({ YD_OUT: -1 });
}
setInterval(function () { yardTick(false); }, 60 * 1000);

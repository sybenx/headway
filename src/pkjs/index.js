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

function send(temp, code) {
  Pebble.sendAppMessage({ WOK: 1, TEMP: Math.round(temp), WCODE: code || 0 });
}

function fetchWeather(force) {
  var s = settings();
  // Only fetch when a slot is actually showing weather.
  var wanted = [s.MOD1, s.MOD2, s.MOD3].some(function (m) { return String(m) === '4'; });
  if (!wanted && s.MOD1 !== undefined) return;
  var now = Date.now();
  if (!force && now - lastFetch < WEATHER_TTL) return;

  navigator.geolocation.getCurrentPosition(function (pos) {
    var unit = s.UNITS === 'imperial' ? 'fahrenheit' : 'celsius';
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + pos.coords.latitude.toFixed(3) +
      '&longitude=' + pos.coords.longitude.toFixed(3) +
      '&current=temperature_2m,weather_code' +
      '&temperature_unit=' + unit;

    var req = new XMLHttpRequest();
    req.open('GET', url, true);
    req.onload = function () {
      if (req.status !== 200) return;
      try {
        var cur = JSON.parse(req.responseText).current;
        lastFetch = Date.now();
        send(cur.temperature_2m, cur.weather_code);
      } catch (e) {}
    };
    req.send();
  }, function () {}, { timeout: 15000, maximumAge: 10 * 60 * 1000 });
}

// ---- transit: is the wearer at the hub, and what leaves it next.
//
// The face counts down on its own; the phone only tells it whether the
// countdown applies here and now, and the next few departures of the
// routes on their own timetable. A coarse fix does: the question is
// "within a few hundred metres of the hub", not "which side of the road".
var TRANSIT_EVERY = 5 * 60 * 1000;
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
function upcoming(list, nowMin) {
  var out = [];
  for (var i = 0; i < list.length && out.length < TR_MAX; i++) {
    if (list[i] >= nowMin) out.push(list[i]);
  }
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
  };
}

// Away from every known system the phone asks less often.
var lastOutside = 0;
var OUTSIDE_EVERY = 15 * 60 * 1000;

// What the face should know about a position: whether it is in a system it
// knows, and whether it is at that system's hub in its hours, with the next
// departures of the routes on their own timetable. Null when there is
// nothing to say. The background check and a flick both ask this, a flick
// with its own precise fix, so walking up to the hub and flicking starts the
// countdown then and there.
function transitState(lat, lon) {
  var mode = hubMode();
  if (mode === MODE_OFF) return null;
  var radius = Number(settings().TR_RADIUS) || 300;
  var sys = systemAt(lat, lon);
  if (!sys) {
    // Outside every system the face knows. Automatic: a plain watch; the
    // always-on modes still take the first system as their hub.
    if (mode === MODE_AUTO) return { area: 0, state: 0, g: [], b: [], why: 'outside any known system' };
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
    area: 1, state: active ? 1 : 0,
    g: active ? upcoming(day.dep[routes[0]] || [], nowMin) : [],
    b: active ? upcoming(day.dep[routes[1]] || [], nowMin) : [],
    why: sys.agency + ' ' + Math.round(d) + 'm from the hub, ' + (active ? 'at it' : 'away'),
  };
}

function checkTransit(force) {
  var mode = hubMode();
  if (mode === MODE_OFF) return;
  if (!force && mode === MODE_AUTO && lastOutside && Date.now() - lastOutside < OUTSIDE_EVERY) return;
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    lastOutside = systemAt(lat, lon) ? 0 : Date.now();
    var st = transitState(lat, lon);
    if (!st) return;
    console.log('headway: ' + st.why);
    Pebble.sendAppMessage(transitMsg(st));
  }, function () {
    // No fix: say nothing, and the watch keeps its last word until it is stale.
  }, { timeout: 10000, maximumAge: 4 * 60 * 1000 });
}

// ---- the flick: the nearest stop and what leaves it next.
//
// The stop index and the per-stop files come from the repo's own pages,
// written nightly by tools/transit.py. A precise fix picks the stop; twin
// stops across a road are merged, since the headsign tells them apart.
var INDEX_TTL = 24 * 60 * 60 * 1000, STOP_TTL = 6 * 60 * 60 * 1000;
// At a stop, the board; off it, the board with its distance; further than
// two kilometres from any stop, nothing.
var AT_STOP = 60, TWIN = 80, HUB = 100, FAR = 2000;

function cached(key, ttl) {
  try {
    var v = JSON.parse(localStorage.getItem(key));
    if (v && Date.now() - v.at < ttl) return v.data;
  } catch (e) {}
  return null;
}
function remember(key, data) {
  try { localStorage.setItem(key, JSON.stringify({ at: Date.now(), data: data })); } catch (e) {}
}
function getJSON(url, key, ttl, cb) {
  var hit = cached(key, ttl);
  if (hit) return cb(hit);
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function () {
    if (req.status !== 200) return cb(null);
    try { var d = JSON.parse(req.responseText); remember(key, d); cb(d); } catch (e) { cb(null); }
  };
  req.onerror = function () { cb(null); };
  req.timeout = 8000;
  req.ontimeout = function () { cb(null); };
  req.send();
}

// A direction word that fits the board's second column: the compass word
// when the headsign has one, else as many of its words as fit in eight, or
// its first eight letters when the first word alone is longer.
function dirWord(head) {
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
function getLive(sys, ids, cb) {
  if (!sys.live || !ids.length) return cb(null);
  var done = false;
  function finish(v) { if (!done) { done = true; cb(v); } }
  var req = new XMLHttpRequest();
  req.open('GET', sys.live + '?stops=' + ids.join(','), true);
  req.onload = function () {
    if (req.status !== 200) return finish(null);
    try {
      var d = JSON.parse(req.responseText);
      // The relay passes the feed's own clock through; a feed that has
      // stopped moving still answers, so its age is checked here.
      var age = Date.now() / 1000 - d.t;
      console.log('headway: live ' + Object.keys(d.trips || {}).length + ' trips, age ' + Math.round(age) + 's');
      finish(d.trips && age < LIVE_FRESH ? d : null);
    } catch (e) { finish(null); }
  };
  req.onerror = function () { finish(null); };
  req.timeout = LIVE_WAIT;
  req.ontimeout = function () { finish(null); };
  setTimeout(function () { finish(null); }, LIVE_WAIT + 250);   // not every phone honours XHR timeouts
  req.send();
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
function fitName(name) {
  name = name || '';
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

function sendStopView(name, dist, rows) {
  console.log('headway: stop view ' + (name || '(none)') + ' ' + rows.length + ' rows');
  var msg = withTransit({ SV_STOP: fitName(name), SV_DIST: Math.round(dist), SV_N: rows.length, SV_MODE: 0 });
  for (var i = 0; i < rows.length && i < 3; i++) {
    msg['SV_R' + (i + 1)] = rows[i].route;
    msg['SV_H' + (i + 1)] = rows[i].head;
    msg['SV_W' + (i + 1)] = rows[i].when;
    msg['SV_T' + (i + 1)] = rows[i].live ? 1 : 0;
    msg['SV_C' + (i + 1)] = rows[i].color;
  }
  Pebble.sendAppMessage(msg);
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
  var msg = withTransit({ SV_STOP: fitName(name), SV_DIST: 0, SV_N: waves.length, SV_MODE: 1 });
  waves.forEach(function (w, i) {
    var k = [];
    w.colors.forEach(function (c) { k.push((c >> 16) & 255, (c >> 8) & 255, c & 255); });
    msg['SV_R' + (i + 1)] = w.routes.join(',');
    msg['SV_W' + (i + 1)] = w.when;
    msg['SV_K' + (i + 1)] = k;
    msg['SV_T' + (i + 1)] = w.live;
    msg['SV_O' + (i + 1)] = w.away;
  });
  Pebble.sendAppMessage(msg);
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
// A route's stem, to match a trip id's: 16 AM and 16 PM are 16, B1 is B.
function routeStem(r) { var m = /^(\d+|[A-Z])/.exec(r || ''); return m ? m[1] : r; }

// The hub's answer: the next three departure minutes, each with every route
// leaving then, once, in route order, twelve at most. After the day's last
// bus the first line carries the day word. With live positions, a route's
// first badge says whether its bus is in yet; later ones say nothing, since
// the bus at the bay is the one leaving first.
function waves(deps, dayWord, index, routesInNow) {
  var out = [], byMin = {}, firstSeen = {};
  deps.forEach(function (dep) {
    var w = byMin[dep.t];
    if (!w) {
      if (out.length === 3) return;
      w = byMin[dep.t] = { t: dep.t, list: [], seen: {} };
      out.push(w);
    }
    var here = !routesInNow || !!routesInNow[routeStem(dep.route)];
    if (w.seen[dep.route]) { var x = w.seen[dep.route]; x.live = x.live || dep.live; x.here = x.here || here; return; }
    if (w.list.length === 12) return;
    w.list.push(w.seen[dep.route] = { route: dep.route, live: dep.live, here: here, first: !firstSeen[dep.route] });
    firstSeen[dep.route] = true;
  });
  return out.map(function (w, i) {
    w.list.sort(function (a, b) { return byRoute(a.route, b.route); });
    var bits = 0, away = 0;
    w.list.forEach(function (x, j) {
      if (x.live) bits |= 1 << j;
      if (x.first && !x.here) away |= 1 << j;
    });
    return {
      when: String(w.t) + (i === 0 && dayWord ? ' ' + dayWord : ''),
      routes: w.list.map(function (x) { return x.route; }),
      colors: w.list.map(function (x) { return parseInt((index.routes[x.route] || ['888888'])[0], 16); }),
      live: bits,
      away: away,
    };
  });
}

// A Pebble Classic (aplite) has no room for the hub's view; it keeps the board.
function classic() {
  try { return Pebble.getActiveWatchInfo().platform === 'aplite'; } catch (e) { return false; }
}

function onFlick() {
  // The watch only asks when its own setting allows, so no gate here.
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    console.log('headway: fix ' + lat.toFixed(4) + ',' + lon.toFixed(4) + ' +-' + Math.round(pos.coords.accuracy) + 'm');
    flickTransit = transitState(lat, lon);
    var sys = systemAt(lat, lon);
    if (!sys || !sys.data) return sendStopView('', 0, []);
    var DATA_URL = sys.data, tag = sys.agency.toLowerCase();
    getJSON(DATA_URL + 'stops.json', 'hw2-stops-' + tag, INDEX_TTL, function (index) {
      if (!index) return sendStopView('', 0, []);
      Object.keys(index.routes || {}).forEach(function (r, i) { routeRank[r] = i; });
      var ranked = index.stops.map(function (st) {
        return { id: st[0], d: metres(lat, lon, st[1], st[2]), lat: st[1], lon: st[2], station: st[3] };
      }).sort(function (a, b) { return a.d - b.d; });
      if (!ranked.length || ranked[0].d > FAR) return sendStopView('', 0, []);
      var best = ranked[0];
      // Twins across a road are read as one stop, and so is a station: every
      // stop the index marks with the nearest one's station number. At the
      // hub, every bay is: the group is the whole hub, and it goes by the
      // hub's own name rather than whichever bay happened to be nearest.
      var atHub = sys.hub && metres(best.lat, best.lon, sys.hub.lat, sys.hub.lon) <= HUB;
      var group = atHub
        ? ranked.filter(function (st) { return metres(sys.hub.lat, sys.hub.lon, st.lat, st.lon) <= HUB; }).slice(0, 16)
        : ranked.filter(function (st) {
          return metres(best.lat, best.lon, st.lat, st.lon) <= TWIN || (best.station && st.station === best.station);
        }).slice(0, 16);
      var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
      var midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
      var pending = group.length + 1, name = atHub ? sys.hub.name : '', stops = [], live = null;
      // The live answer is asked for alongside the schedule and waited on
      // for at most LIVE_WAIT; the schedule never waits on it beyond that.
      getLive(sys, group.map(function (st) { return st.id; }), function (l) { live = l; answer(); });
      group.forEach(function (st) {
        getJSON(DATA_URL + 'stops/' + st.id + '.json', 'hw2-stop-' + tag + '-' + st.id, STOP_TTL, function (stop) {
          if (stop) { if (!name) name = stop.name; stop.id = stop.id || st.id; stops.push(stop); }
          answer();
        });
      });
      function answer() {
        if (--pending) return;
        // Today's remaining departures; when there are none, the first
        // day ahead with any — tomorrow, or Monday after a Saturday —
        // so the answer is the next bus, whenever that is.
        var deps = [], dayWord = '';
        for (var ahead = 0; ahead < 8 && !deps.length; ahead++) {
          var date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + ahead);
          // A day's buses are its own services' departures, and the ones the
          // day before's services run past midnight (GTFS writes 12:53 AM as
          // 24:53 on the evening's service). Today's list keeps the minute
          // just gone: a bus due a minute ago may still be pulling in, and
          // the watch reads it as NOW.
          var onDay = activeServices(index.services, date);
          var onEve = activeServices(index.services, new Date(date.getFullYear(), date.getMonth(), date.getDate() - 1));
          var from = ahead ? 0 : nowMin - 1;
          stops.forEach(function (stop) {
            (stop.deps || []).forEach(function (row) {
              var shift = row[0] >= 1440 && onEve[row[4]] ? 1440 : 0;
              if (!shift && !onDay[row[4]]) return;
              var dep = [row[0] - shift, row[1], row[2], row[3]];
              // Today's departures take the live prediction for this trip
              // at this stop where there is one. A stop the bus will skip
              // drops out; at the hub a bus is held to its time, never
              // early. No prediction: the schedule, unmarked.
              var t = dep[0], isLive = false, at = null;
              var trip = !ahead && live && dep[3] && live.trips[dep[3]];
              for (var i = 0; trip && i < trip.s.length; i++) if (trip.s[i][0] === stop.id) at = trip.s[i];
              if (at) {
                if (at[3] === 1) return;
                var p = Math.floor((at[2] * 1000 - midnight) / 60000);
                // A bus still listed at its hub bay after the feed's time for
                // it is still there, boarding: it leaves now, not already.
                var boarding = at[2] * 1000 < now.getTime() - 30000 ? nowMin : -1;
                t = atHub ? Math.max(t, p, boarding) : p; isLive = true;
              }
              if (t >= from) deps.push({ t: t, route: dep[1], head: dep[2], live: isLive });
            });
          });
          if (deps.length && ahead) dayWord = ahead === 1 ? 'TOMORROW' : ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][date.getDay()];
        }
        // Ties in route order, never in whichever file happened to load first.
        deps.sort(function (a, b) { return a.t - b.t || byRoute(a.route, b.route); });
        if (atHub && !classic()) return sendWaves(name, waves(deps, dayWord, index, routesIn(group, live, stops)));
        // A row a route and direction, in order of its next departure,
        // with its next two times, or its next time and the day. Where a
        // route runs both ways from here — twin stops across a road — the
        // second column is the direction instead, since two identical
        // badges would say nothing.
        var groups = [], byKey = {}, perRoute = {};
        deps.forEach(function (dep) {
          var key = dep.route + '|' + dep.head, g = byKey[key];
          if (!g) { g = byKey[key] = { route: dep.route, head: dep.head, times: [], live: dep.live }; groups.push(g); perRoute[dep.route] = (perRoute[dep.route] || 0) + 1; }
          if (g.times.length < 2) g.times.push(dep.t);
        });
        // Every row, near or far: the watch counts rows, not metres, and
        // seats one row beside the modules and more on the board. The
        // second column holds one qualifier: the day first, since a bus
        // you can't catch today is the costliest thing to misread; then
        // the direction at a merged pair; else the second time.
        var rows = groups.slice(0, 3).map(function (g) {
          var col = (index.routes[g.route] || ['888888'])[0];
          var word = dayWord || (perRoute[g.route] > 1 ? dirWord(g.head) : '');
          var when = word ? [g.times[0], word] : g.times;
          return { route: g.route, head: g.head, when: when.join(' '), color: parseInt(col, 16), live: g.live };
        });
        sendStopView(name, best.d <= AT_STOP ? 0 : best.d, rows);
      }
    });
  }, function (err) {
    console.log('headway: no fix ' + (err && err.message));
    sendStopView('', 0, []);
  }, { enableHighAccuracy: true, timeout: 9000, maximumAge: 20000 });
}

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.FLICK) { console.log('headway: flick'); onFlick(); }
});

Pebble.addEventListener('ready', function () { fetchWeather(true); checkTransit(true); });
Pebble.addEventListener('webviewclosed', function () {
  // Settings may have switched weather or transit on, or changed units.
  setTimeout(function () { fetchWeather(true); checkTransit(true); }, 500);
});
setInterval(function () { fetchWeather(false); }, 10 * 60 * 1000);
setInterval(function () { checkTransit(false); }, TRANSIT_EVERY);

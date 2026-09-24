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
function hubMode() {
  var s = settings();
  return s.TRANSIT === undefined ? MODE_AUTO : String(s.TRANSIT);
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

function sendTransit(area, state, g, b) {
  Pebble.sendAppMessage({
    TR_AREA: area,
    TR_STATE: state,
    TR_G: packMinutes(g),
    TR_B: packMinutes(b),
    TR_AT: Math.floor(Date.now() / 1000),
  });
}

// Away from every known system the phone asks less often.
var lastOutside = 0;
var OUTSIDE_EVERY = 15 * 60 * 1000;

function checkTransit(force) {
  var mode = hubMode();
  if (mode === MODE_OFF) return;
  if (!force && mode === MODE_AUTO && lastOutside && Date.now() - lastOutside < OUTSIDE_EVERY) return;
  var s = settings();
  var radius = Number(s.TR_RADIUS) || 300;

  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    var sys = systemAt(lat, lon);
    if (!sys) {
      // Outside every system the face knows. Automatic: a plain watch; the
      // always-on modes still take the first system as their hub.
      lastOutside = Date.now();
      if (mode === MODE_AUTO) { console.log('headway: outside any known system'); return sendTransit(0, 0, [], []); }
      sys = (transit.systems || [])[0];
      if (!sys) return;
    } else {
      lastOutside = 0;
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
    var g = active ? upcoming(day.dep[routes[0]] || [], nowMin) : [];
    var b = active ? upcoming(day.dep[routes[1]] || [], nowMin) : [];
    console.log('headway: ' + sys.agency + ' ' + Math.round(d) + 'm from the hub, ' + (active ? 'at it' : 'away'));
    sendTransit(1, active ? 1 : 0, g, b);
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
// when the headsign has one, else the headsign cut to eight.
function dirWord(head) {
  var m = /\b(NORTH|SOUTH|EAST|WEST|IN|OUT)BOUND\b/.exec(head || '');
  if (m) return m[1].length <= 2 ? m[1] + 'BOUND' : m[1];
  return (head || '').replace(/\s+$/, '').slice(0, 8);
}

function dayKind(d) { var wd = d.getDay(); return wd === 0 ? 'sunday' : wd === 6 ? 'saturday' : 'weekday'; }

// Times go to the watch as minutes past midnight; the watch writes them in
// its own clock style, 12-hour with A or P, or 24-hour.

function sendStopView(name, dist, rows) {
  console.log('headway: stop view ' + (name || '(none)') + ' ' + rows.length + ' rows');
  var msg = { SV_STOP: name, SV_DIST: Math.round(dist), SV_N: rows.length };
  for (var i = 0; i < rows.length && i < 3; i++) {
    msg['SV_R' + (i + 1)] = rows[i].route;
    msg['SV_H' + (i + 1)] = rows[i].head;
    msg['SV_W' + (i + 1)] = rows[i].when;
    msg['SV_T' + (i + 1)] = 0;
    msg['SV_C' + (i + 1)] = rows[i].color;
  }
  Pebble.sendAppMessage(msg);
}

function onFlick() {
  // The watch only asks when its own setting allows, so no gate here.
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    console.log('headway: fix ' + lat.toFixed(4) + ',' + lon.toFixed(4) + ' +-' + Math.round(pos.coords.accuracy) + 'm');
    var sys = systemAt(lat, lon);
    if (!sys || !sys.data) return sendStopView('', 0, []);
    var DATA_URL = sys.data, tag = sys.agency.toLowerCase();
    getJSON(DATA_URL + 'stops.json', 'hw-stops-' + tag, INDEX_TTL, function (index) {
      if (!index) return sendStopView('', 0, []);
      var ranked = index.stops.map(function (st) {
        return { id: st[0], d: metres(lat, lon, st[1], st[2]), lat: st[1], lon: st[2] };
      }).sort(function (a, b) { return a.d - b.d; });
      if (!ranked.length || ranked[0].d > FAR) return sendStopView('', 0, []);
      var best = ranked[0];
      // Twins across a road are read as one stop. At the hub, every bay is:
      // the group is the whole hub, and it goes by the hub's own name rather
      // than whichever bay happened to be nearest.
      var atHub = sys.hub && metres(best.lat, best.lon, sys.hub.lat, sys.hub.lon) <= HUB;
      var group = atHub
        ? ranked.filter(function (st) { return metres(sys.hub.lat, sys.hub.lon, st.lat, st.lon) <= HUB; }).slice(0, 16)
        : ranked.filter(function (st) { return metres(best.lat, best.lon, st.lat, st.lon) <= TWIN; }).slice(0, 8);
      var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
      var pending = group.length, name = atHub ? sys.hub.name : '', stops = [];
      group.forEach(function (st) {
        getJSON(DATA_URL + 'stops/' + st.id + '.json', 'hw-stop-' + tag + '-' + st.id, STOP_TTL, function (stop) {
          if (stop) { if (!name) name = stop.name; stops.push(stop); }
          if (--pending) return;
          // Today's remaining departures; when there are none, the first
          // day ahead with any — tomorrow, or Monday after a Saturday —
          // so the answer is the next bus, whenever that is.
          var deps = [], dayWord = '';
          for (var ahead = 0; ahead < 8 && !deps.length; ahead++) {
            var date = new Date(now.getTime() + ahead * 24 * 60 * 60 * 1000);
            var kind = dayKind(date), from = ahead ? 0 : nowMin;
            stops.forEach(function (stop) {
              (stop.days[kind] || []).forEach(function (dep) {
                if (dep[0] >= from) deps.push({ t: dep[0], route: dep[1], head: dep[2] });
              });
            });
            if (deps.length && ahead) dayWord = ahead === 1 ? 'TOMORROW' : ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][date.getDay()];
          }
          deps.sort(function (a, b) { return a.t - b.t; });
          // A row a route and direction, in order of its next departure,
          // with its next two times, or its next time and the day. Where a
          // route runs both ways from here — twin stops across a road — the
          // second column is the direction instead, since two identical
          // badges would say nothing.
          var groups = [], byKey = {}, perRoute = {};
          deps.forEach(function (dep) {
            var key = dep.route + '|' + dep.head, g = byKey[key];
            if (!g) { g = byKey[key] = { route: dep.route, head: dep.head, times: [] }; groups.push(g); perRoute[dep.route] = (perRoute[dep.route] || 0) + 1; }
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
            return { route: g.route, head: g.head, when: when.join(' '), color: parseInt(col, 16) };
          });
          sendStopView(name, best.d <= AT_STOP ? 0 : best.d, rows);
        });
      });
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

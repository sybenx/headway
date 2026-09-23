// Clay's prebuilt bundle: the SDK's webpack cannot parse Clay's ES6 source.
var Clay = require('pebble-clay/dist/js/index.js');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

// The hub the face knows: written by tools/transit.py from the agency's GTFS.
var transit = require('./transit.json');

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

// The day type in the hub's own week: weekday, saturday, sunday, or none.
function dayTable(d) {
  var wd = d.getDay();
  var kind = wd === 0 ? 'sunday' : wd === 6 ? 'saturday' : 'weekday';
  return transit.days[kind] || null;
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

function sendTransit(state, g, b) {
  Pebble.sendAppMessage({
    TR_STATE: state,
    TR_G: packMinutes(g),
    TR_B: packMinutes(b),
    TR_AT: Math.floor(Date.now() / 1000),
  });
}

function checkTransit() {
  var s = settings();
  if (!s.TRANSIT || String(s.TRANSIT) === '0') return;
  var radius = Number(s.TR_RADIUS) || 300;

  navigator.geolocation.getCurrentPosition(function (pos) {
    var d = metres(pos.coords.latitude, pos.coords.longitude, transit.hub.lat, transit.hub.lon);
    var now = new Date();
    var nowMin = now.getHours() * 60 + now.getMinutes();
    var day = dayTable(now);
    // A quarter hour before the first run counts as hours: that is when the
    // first riders are standing there.
    var inHours = !!day && nowMin >= day.hours[0] - 15 && nowMin <= day.hours[1];
    var active = d <= radius && inHours;
    var routes = transit.routes;
    var g = active ? upcoming(day.dep[routes[0]] || [], nowMin) : [];
    var b = active ? upcoming(day.dep[routes[1]] || [], nowMin) : [];
    sendTransit(active ? 1 : 0, g, b);
  }, function () {
    // No fix: say nothing, and the watch keeps its last word until it is stale.
  }, { timeout: 10000, maximumAge: 4 * 60 * 1000 });
}

// ---- the flick: the nearest stop and what leaves it next.
//
// The stop index and the per-stop files come from the repo's own pages,
// written nightly by tools/transit.py. A precise fix picks the stop; twin
// stops across a road are merged, since the headsign tells them apart.
var DATA_URL = 'https://sybenx.github.io/headway/data/cvtd/';
var INDEX_TTL = 24 * 60 * 60 * 1000, STOP_TTL = 6 * 60 * 60 * 1000;
var AT_STOP = 60, TWIN = 45, NEARBY = 1500;

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

function whenText(dep, nowMin, h24) {
  var m = dep - nowMin;
  if (m < 60) return { text: String(m), mins: 1 };
  var h = Math.floor(dep / 60) % 24, mm = dep % 60;
  if (h24) return { text: h + ':' + (mm < 10 ? '0' : '') + mm, mins: 0 };
  var hh = ((h + 11) % 12) + 1;
  return { text: hh + ':' + (mm < 10 ? '0' : '') + mm + (h < 12 ? 'A' : 'P'), mins: 0 };
}

function sendStopView(name, dist, rows) {
  console.log('headway: stop view ' + (name || '(none)') + ' ' + rows.length + ' rows');
  var msg = { SV_STOP: name, SV_DIST: Math.round(dist), SV_N: rows.length };
  for (var i = 0; i < rows.length && i < 3; i++) {
    msg['SV_R' + (i + 1)] = rows[i].route;
    msg['SV_H' + (i + 1)] = rows[i].head;
    msg['SV_W' + (i + 1)] = rows[i].when.text;
    msg['SV_T' + (i + 1)] = rows[i].when.mins;
    msg['SV_C' + (i + 1)] = rows[i].color;
  }
  Pebble.sendAppMessage(msg);
}

function onFlick() {
  // The watch only asks when its own setting allows, so no gate here.
  var s = settings();
  var h24 = String(s.H24) === '2';
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    console.log('headway: fix ' + lat.toFixed(4) + ',' + lon.toFixed(4) + ' +-' + Math.round(pos.coords.accuracy) + 'm');
    getJSON(DATA_URL + 'stops.json', 'hw-stops', INDEX_TTL, function (index) {
      if (!index) return sendStopView('', 0, []);
      var ranked = index.stops.map(function (st) {
        return { id: st[0], d: metres(lat, lon, st[1], st[2]), lat: st[1], lon: st[2] };
      }).sort(function (a, b) { return a.d - b.d; });
      if (!ranked.length || ranked[0].d > NEARBY) return sendStopView('', 0, []);
      var best = ranked[0];
      // Twins across a road, or the bays of a hub: read as one stop.
      var group = ranked.filter(function (st) { return metres(best.lat, best.lon, st.lat, st.lon) <= TWIN; }).slice(0, 8);
      var kind = (function (d) { var wd = d.getDay(); return wd === 0 ? 'sunday' : wd === 6 ? 'saturday' : 'weekday'; })(new Date());
      var now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
      var pending = group.length, name = '', deps = [];
      group.forEach(function (st) {
        getJSON(DATA_URL + 'stops/' + st.id + '.json', 'hw-stop-' + st.id, STOP_TTL, function (stop) {
          if (stop) {
            if (!name) name = stop.name;
            (stop.days[kind] || []).forEach(function (dep) {
              if (dep[0] >= nowMin) deps.push({ t: dep[0], route: dep[1], head: dep[2] });
            });
          }
          if (--pending) return;
          deps.sort(function (a, b) { return a.t - b.t; });
          var rows = deps.slice(0, 3).map(function (dep) {
            var col = (index.routes[dep.route] || ['888888'])[0];
            return { route: dep.route, head: dep.head, when: whenText(dep.t, nowMin, h24), color: parseInt(col, 16) };
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

Pebble.addEventListener('ready', function () { fetchWeather(true); checkTransit(); });
Pebble.addEventListener('webviewclosed', function () {
  // Settings may have switched weather or transit on, or changed units.
  setTimeout(function () { fetchWeather(true); checkTransit(); }, 500);
});
setInterval(function () { fetchWeather(false); }, 10 * 60 * 1000);
setInterval(checkTransit, TRANSIT_EVERY);

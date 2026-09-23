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
  if (!s.TRANSIT) return;
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

Pebble.addEventListener('ready', function () { fetchWeather(true); checkTransit(); });
Pebble.addEventListener('webviewclosed', function () {
  // Settings may have switched weather or transit on, or changed units.
  setTimeout(function () { fetchWeather(true); checkTransit(); }, 500);
});
setInterval(function () { fetchWeather(false); }, 10 * 60 * 1000);
setInterval(checkTransit, TRANSIT_EVERY);

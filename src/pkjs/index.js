// Clay's prebuilt bundle: the SDK's webpack cannot parse Clay's ES6 source.
var Clay = require('pebble-clay/dist/js/index.js');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

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

Pebble.addEventListener('ready', function () { fetchWeather(true); });
Pebble.addEventListener('webviewclosed', function () {
  // Settings may have switched weather on or changed units.
  setTimeout(function () { fetchWeather(true); }, 500);
});
setInterval(function () { fetchWeather(false); }, 10 * 60 * 1000);

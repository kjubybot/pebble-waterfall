var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

var POLL_MS = 20 * 60 * 1000;

function readUnit() {
  try {
    var raw = localStorage.getItem('clay-settings');
    if (!raw) return 'celsius';
    var parsed = JSON.parse(raw);
    var v = parsed && parsed.TEMPERATURE_UNIT;
    return v === 'fahrenheit' ? 'fahrenheit' : 'celsius';
  } catch (e) {
    return 'celsius';
  }
}

function readTapAnimation() {
  try {
    var raw = localStorage.getItem('clay-settings');
    if (!raw) return true;
    var parsed = JSON.parse(raw);
    return !parsed || parsed.TAP_ANIMATION !== false;
  } catch (e) {
    return true;
  }
}

var pollHandle = null;

function startPolling() {
  if (pollHandle !== null) return;
  fetchWeather();
  pollHandle = setInterval(fetchWeather, POLL_MS);
}

function stopPolling() {
  if (pollHandle !== null) {
    clearInterval(pollHandle);
    pollHandle = null;
  }
  sendTemp('');
}

function sendTemp(str) {
  Pebble.sendAppMessage({ TEMPERATURE: str }, function () {}, function (e) {
    console.log('temp send failed: ' + JSON.stringify(e));
  });
}

function requestWeather(lat, lon, unit) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + lat +
    '&longitude=' + lon + '&current=temperature_2m&temperature_unit=' + unit;
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.onload = function () {
    if (xhr.status < 200 || xhr.status >= 300) {
      console.log('open-meteo status ' + xhr.status);
      sendTemp('--');
      return;
    }
    try {
      var data = JSON.parse(xhr.responseText);
      var t = data && data.current && data.current.temperature_2m;
      if (typeof t !== 'number') {
        sendTemp('--');
        return;
      }
      var suffix = unit === 'fahrenheit' ? 'F' : 'C';
      sendTemp(Math.round(t) + ' ' + suffix);
    } catch (e) {
      console.log('open-meteo parse error: ' + e);
      sendTemp('--');
    }
  };
  xhr.onerror = function () {
    console.log('open-meteo network error');
    sendTemp('--');
  };
  xhr.send();
}

function fetchWeather() {
  var unit = readUnit();
  if (!navigator.geolocation) {
    sendTemp('--');
    return;
  }
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      requestWeather(pos.coords.latitude, pos.coords.longitude, unit);
    },
    function (err) {
      console.log('geolocation error: ' + err.message);
      sendTemp('--');
    },
    { timeout: 15000, maximumAge: 5 * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', function () {
  if (readTapAnimation()) {
    startPolling();
  } else {
    stopPolling();
  }
});

Pebble.addEventListener('webviewclosed', function () {
  if (readTapAnimation()) {
    if (pollHandle === null) {
      startPolling();
    } else {
      fetchWeather();
    }
  } else {
    stopPolling();
  }
});

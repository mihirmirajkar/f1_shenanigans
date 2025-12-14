#include <WebServer.h>

extern WebServer server;
extern const int MAX_LAPS;
String formatTime(unsigned long ms);

// Camera-based multi-car tracking (phone browser does the vision)
const int MAX_CARS = 6;  // hard limit to keep memory small
const unsigned long MIN_CAMERA_TRIGGER_GAP_MS = 400;  // debounce per car for camera triggers

static bool cameraModeActive = false;
static int carCount = 2;  // configurable from UI

static String carIds[MAX_CARS];
static String carLabels[MAX_CARS];
static String carColors[MAX_CARS];

static unsigned long carLapTimes[MAX_CARS][MAX_LAPS];
static unsigned long carLastLapMs[MAX_CARS];
static unsigned long carLapStartMs[MAX_CARS];
static int carLapCount[MAX_CARS];
static bool carStartedOnce[MAX_CARS];
static bool carTimerRunning[MAX_CARS];
static unsigned long lastCameraTriggerMs[MAX_CARS];

static const char* DEFAULT_COLORS[MAX_CARS] = {
  "#ff4d4f", "#4dabff", "#6fdd8b", "#ffb347", "#d17bff", "#ffd166"
};

static void assignDefaults() {
  for (int i = 0; i < MAX_CARS; i++) {
    carIds[i] = "car" + String(i + 1);
    carLabels[i] = "Car " + String(i + 1);
    carColors[i] = DEFAULT_COLORS[i % 6];
  }
}

static void clampCarCount(int requested) {
  carCount = requested;
  if (carCount < 1) carCount = 1;
  if (carCount > MAX_CARS) carCount = MAX_CARS;
}

void resetCarTiming() {
  for (int i = 0; i < MAX_CARS; i++) {
    carLastLapMs[i] = 0;
    carLapStartMs[i] = 0;
    carLapCount[i] = 0;
    carStartedOnce[i] = false;
    carTimerRunning[i] = false;
    lastCameraTriggerMs[i] = 0;
    for (int j = 0; j < MAX_LAPS; j++) {
      carLapTimes[i][j] = 0;
    }
  }
}

bool cameraIsActive() { return cameraModeActive; }
void cameraDeactivate() { cameraModeActive = false; }

static int findCarIndex(const String& carId) {
  for (int i = 0; i < carCount; i++) {
    if (carIds[i].equalsIgnoreCase(carId)) return i;
  }
  return -1;
}

static void addLapTimeForCar(int carIdx, unsigned long ms) {
  for (int i = MAX_LAPS - 1; i > 0; i--) {
    carLapTimes[carIdx][i] = carLapTimes[carIdx][i - 1];
  }
  carLapTimes[carIdx][0] = ms;
  if (carLapCount[carIdx] < MAX_LAPS) carLapCount[carIdx]++;
}

static void splitCsv(const String& csv, String out[], int maxItems, int& outCount) {
  outCount = 0;
  int start = 0;
  while (start < (int)csv.length() && outCount < maxItems) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String token = csv.substring(start, comma);
    token.trim();
    out[outCount++] = token;
    start = comma + 1;
  }
}

static void applyCarConfig(int newCount, String names[], String colors[], int namesCount, int colorsCount) {
  clampCarCount(newCount);
  for (int i = 0; i < carCount; i++) {
    carIds[i] = "car" + String(i + 1);
    if (i < namesCount && names[i].length() > 0) {
      carLabels[i] = names[i];
    } else {
      carLabels[i] = "Car " + String(i + 1);
    }
    if (i < colorsCount && colors[i].length() > 0) {
      carColors[i] = colors[i];
    } else {
      carColors[i] = DEFAULT_COLORS[i % 6];
    }
  }
  resetCarTiming();
}

void handleLapPing() {
  cameraModeActive = true;  // keep IR disabled while camera is in use

  if (!server.hasArg("car")) {
    server.send(400, "text/plain", "Missing car");
    return;
  }

  int idx = findCarIndex(server.arg("car"));
  if (idx < 0) {
    server.send(404, "text/plain", "Unknown car");
    return;
  }

  unsigned long now = millis();
  if (now - lastCameraTriggerMs[idx] < MIN_CAMERA_TRIGGER_GAP_MS) {
    server.send(200, "text/plain", "IGNORED");
    return;
  }
  lastCameraTriggerMs[idx] = now;

  if (!carStartedOnce[idx]) {
    carStartedOnce[idx] = true;
    carTimerRunning[idx] = true;
    carLapStartMs[idx] = now;
    server.send(200, "text/plain", "STARTED");
    return;
  }

  if (carTimerRunning[idx]) {
    unsigned long lap = now - carLapStartMs[idx];
    carLastLapMs[idx] = lap;
    addLapTimeForCar(idx, lap);
    carLapStartMs[idx] = now;
    server.send(200, "text/plain", "LAP");
  } else {
    carTimerRunning[idx] = true;
    carLapStartMs[idx] = now;
    server.send(200, "text/plain", "RESUMED");
  }
}

void handleCarStatus() {
  unsigned long now = millis();
  String json = "{";
  json += "\"count\":" + String(carCount) + ",";
  json += "\"cars\":[";
  for (int i = 0; i < carCount; i++) {
    unsigned long currentMs = 0;
    if (carTimerRunning[i] && carLapStartMs[i] > 0) {
      currentMs = now - carLapStartMs[i];
    }

    json += "{";
    json += "\"id\":\"" + carIds[i] + "\",";
    json += "\"label\":\"" + carLabels[i] + "\",";
    json += "\"color\":\"" + carColors[i] + "\",";
    json += "\"lastLapMs\":" + String(carLastLapMs[i]) + ",";
    json += "\"lastLapMsFormatted\":\"" + formatTime(carLastLapMs[i]) + "\",";
    json += "\"running\":" + String(carTimerRunning[i] ? "true" : "false") + ",";
    json += "\"currentMs\":" + String(currentMs) + ",";
    json += "\"laps\":[";
    for (int j = 0; j < carLapCount[i]; j++) {
      json += "{\"ms\":" + String(carLapTimes[i][j]) + ",\"formatted\":\"" + formatTime(carLapTimes[i][j]) + "\"}";
      if (j < carLapCount[i] - 1) json += ",";
    }
    json += "]";
    json += "}";
    if (i < carCount - 1) json += ",";
  }
  json += "]";
  json += "}";

  server.send(200, "application/json", json);
}

void handleCarConfig() {
  if (server.method() == HTTP_GET) {
    String json = "{";
    json += "\"maxCars\":" + String(MAX_CARS) + ",";
    json += "\"count\":" + String(carCount) + ",";
    json += "\"cars\":[";
    for (int i = 0; i < carCount; i++) {
      json += "{\"id\":\"" + carIds[i] + "\",\"label\":\"" + carLabels[i] + "\",\"color\":\"" + carColors[i] + "\"}";
      if (i < carCount - 1) json += ",";
    }
    json += "]";
    json += "}";
    server.send(200, "application/json", json);
    return;
  }

  // Accept POST/any with query args: count, names(csv), colors(csv)
  int newCount = carCount;
  if (server.hasArg("count")) {
    newCount = server.arg("count").toInt();
  }

  String namesCsv = server.hasArg("names") ? server.arg("names") : "";
  String colorsCsv = server.hasArg("colors") ? server.arg("colors") : "";

  String names[MAX_CARS];
  String colors[MAX_CARS];
  int namesCount = 0;
  int colorsCount = 0;
  splitCsv(namesCsv, names, MAX_CARS, namesCount);
  splitCsv(colorsCsv, colors, MAX_CARS, colorsCount);

  applyCarConfig(newCount, names, colors, namesCount, colorsCount);
  server.send(200, "text/plain", "OK");
}

// === HTML: Camera-Based Multi-Car Tracking Page ===
void handleCameraPage() {
  cameraModeActive = true;  // pause IR processing while using camera mode

  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Camera Lap Tracker</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <style>
    body {
      font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
      background: #0d1117;
      color: #e6edf3;
      margin: 0;
      padding: 12px;
      text-align: center;
    }
    h1 { margin: 0.2em 0 0.1em; }
    .hint { color: #8b949e; font-size: 0.95rem; }
    .controls { margin: 0.5em auto 0.6em; max-width: 940px; display: flex; flex-wrap: wrap; gap: 8px; justify-content: center; }
    .card { background: #161b22; border: 1px solid #30363d; border-radius: 10px; padding: 10px; }
    .car { display: grid; grid-template-columns: 1fr 90px 70px 70px; gap: 8px; align-items: center; margin: 6px 0; }
    .car input[type="text"] { width: 100%; padding: 6px 8px; background: #0d1117; color: #e6edf3; border: 1px solid #30363d; border-radius: 6px; }
    video, canvas { width: 100%; max-width: 940px; border-radius: 10px; border: 1px solid #30363d; background: #000; }
    .sliders { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    label { font-size: 0.9rem; color: #c9d1d9; }
    input[type="range"] { width: 100%; }
    button { background: #238636; color: #fff; border: none; border-radius: 8px; padding: 10px 14px; font-weight: 700; cursor: pointer; }
    button.alt { background: #1f6feb; }
    button.small { padding: 7px 10px; font-size: 0.9rem; }
    button.warn { background: #d2553f; }
    #log { font-family: "SF Mono", "Roboto Mono", monospace; text-align: left; max-width: 940px; margin: 0.6em auto; background: #0b0f14; border: 1px solid #30363d; border-radius: 10px; padding: 10px; white-space: pre-wrap; height: 150px; overflow: auto; }
    .laps { max-width: 940px; margin: 0.6em auto; text-align: left; }
    .laps table { width: 100%; border-collapse: collapse; font-family: "SF Mono", "Roboto Mono", monospace; }
    .laps th, .laps td { padding: 6px 8px; border-bottom: 1px solid #30363d; font-size: 0.95rem; }
    .laps th { text-transform: uppercase; letter-spacing: 0.06em; color: #8b949e; }
  </style>
</head>
<body>
  <h1>Camera Lap Tracker</h1>
  <div class="hint">Mount phone above start/finish. Set car count, names, colors; align the ROI band over the line. Keep lighting steady.</div>

  <div class="controls">
    <div class="card" style="flex: 1 1 300px; min-width: 260px;">
      <div style="display:flex; gap:8px; align-items:center; margin-bottom:6px;">
        <strong>Cars</strong>
        <input id="carCount" type="number" min="1" max="6" value="2" style="width:70px; padding:6px 8px; background:#0d1117; color:#e6edf3; border:1px solid #30363d; border-radius:6px;" />
        <button id="applyCount" class="small alt">Apply Count</button>
        <button id="saveCars" class="small">Save Names/Colors</button>
      </div>
      <div class="hint" style="text-align:left; margin-bottom:6px;">Reorder with ↑/↓ to match start order.</div>
      <div id="carList"></div>
      <button id="resetBg" class="small warn" style="margin-top:6px;">Relearn Background</button>
    </div>
    <div class="card" style="flex: 1 1 260px; min-width: 240px;">
      <div class="sliders">
        <div>
          <label>Band Y (height %)</label>
          <input id="bandY" type="range" min="5" max="90" value="65" />
        </div>
        <div>
          <label>Band H (height %)</label>
          <input id="bandH" type="range" min="5" max="60" value="15" />
        </div>
        <div>
          <label>Sensitivity (motion px)</label>
          <input id="motionPx" type="range" min="200" max="4000" value="1200" />
        </div>
        <div>
          <label>Diff Threshold</label>
          <input id="diffThr" type="range" min="10" max="80" value="32" />
        </div>
      </div>
    </div>
  </div>

  <video id="video" autoplay playsinline muted></video>
  <canvas id="canvas"></canvas>

  <div style="margin: 0.5em auto 0.2em; max-width: 940px; text-align: left;" class="hint">
    Tip: set phone focus/exposure manually if possible. Keep the finish line inside the colored band. Use bright, distinct sticker colors.
  </div>

  <div id="log"></div>

  <div class="laps">
    <table>
      <thead><tr><th>Car</th><th>Last Lap</th><th>Recent Laps</th></tr></thead>
      <tbody id="lapsBody"></tbody>
    </table>
  </div>

  <script>
    let carProfiles = [];
    let maxCars = 6;

    const video = document.getElementById('video');
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const logEl = document.getElementById('log');
    const lapsBody = document.getElementById('lapsBody');
    const bandY = document.getElementById('bandY');
    const bandH = document.getElementById('bandH');
    const motionPx = document.getElementById('motionPx');
    const diffThr = document.getElementById('diffThr');
    const resetBgBtn = document.getElementById('resetBg');
    const carCountInput = document.getElementById('carCount');
    const applyCountBtn = document.getElementById('applyCount');
    const saveCarsBtn = document.getElementById('saveCars');

    let bgFrame = null;
    let lastGlobalHit = 0;

    function log(msg) {
      const now = new Date().toLocaleTimeString();
      logEl.textContent = `[${now}] ${msg}\n` + logEl.textContent;
    }

    function hexToRgb(hex) {
      const v = parseInt(hex.slice(1), 16);
      return { r: (v >> 16) & 255, g: (v >> 8) & 255, b: v & 255 };
    }

    function rebuildCarList() {
      const container = document.getElementById('carList');
      container.innerHTML = '';
      carProfiles.forEach((car, idx) => {
        const row = document.createElement('div');
        row.className = 'car';
        row.innerHTML = `
          <input type="text" value="${car.label}" data-idx="${idx}" />
          <input type="color" value="${car.color}" data-idx="${idx}" />
          <button class="small alt" data-idx="${idx}" data-dir="up">↑</button>
          <button class="small alt" data-idx="${idx}" data-dir="down">↓</button>
        `;
        const [nameInput, colorInput, upBtn, downBtn] = row.querySelectorAll('input, button');
        nameInput.addEventListener('input', (e) => {
          carProfiles[idx].label = e.target.value;
        });
        colorInput.addEventListener('input', (e) => {
          carProfiles[idx].color = e.target.value;
        });
        upBtn.addEventListener('click', () => moveCar(idx, -1));
        downBtn.addEventListener('click', () => moveCar(idx, 1));
        container.appendChild(row);
      });
    }

    function moveCar(index, delta) {
      const newIndex = index + delta;
      if (newIndex < 0 || newIndex >= carProfiles.length) return;
      const tmp = carProfiles[index];
      carProfiles[index] = carProfiles[newIndex];
      carProfiles[newIndex] = tmp;
      rebuildCarList();
    }

    function applyCount() {
      let desired = parseInt(carCountInput.value || '1', 10);
      if (Number.isNaN(desired)) desired = 1;
      desired = Math.min(Math.max(desired, 1), maxCars);
      carCountInput.value = desired;
      while (carProfiles.length < desired) {
        const n = carProfiles.length + 1;
        carProfiles.push({ id: `car${n}`, label: `Car ${n}`, color: '#ff4d4f', lastHit: 0 });
      }
      while (carProfiles.length > desired) {
        carProfiles.pop();
      }
      rebuildCarList();
    }

    async function saveCars() {
      const names = carProfiles.map(c => encodeURIComponent(c.label)).join(',');
      const colors = carProfiles.map(c => encodeURIComponent(c.color)).join(',');
      const body = new URLSearchParams();
      body.set('count', String(carProfiles.length));
      body.set('names', names);
      body.set('colors', colors);
      await fetch('/car_config', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      log('Saved car names/colors.');
    }

    function drawBand(w, h) {
      const y = (bandY.value / 100) * h;
      const bandHeight = (bandH.value / 100) * h;
      ctx.strokeStyle = '#00ffaa';
      ctx.lineWidth = 3;
      ctx.strokeRect(0, y, w, bandHeight);
      ctx.fillStyle = 'rgba(0,255,170,0.12)';
      ctx.fillRect(0, y, w, bandHeight);
    }

    async function startCamera() {
      try {
        const stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' }, audio: false });
        video.srcObject = stream;
        video.onloadedmetadata = () => {
          canvas.width = video.videoWidth || 640;
          canvas.height = video.videoHeight || 480;
          log('Camera ready. Align the band with the finish line.');
          requestAnimationFrame(loop);
        };
      } catch (e) {
        log('Camera error: ' + e.message);
      }
    }

    function processFrame() {
      const w = canvas.width;
      const h = canvas.height;
      if (!w || !h) return null;

      ctx.drawImage(video, 0, 0, w, h);

      const y = Math.max(0, Math.min(h - 1, Math.round((bandY.value / 100) * h)));
      const bandHeight = Math.max(2, Math.round((bandH.value / 100) * h));
      const bandYpx = Math.min(h - bandHeight, y);

      const img = ctx.getImageData(0, bandYpx, w, bandHeight);
      const data = img.data;

      if (!bgFrame || bgFrame.length !== data.length) {
        bgFrame = new Uint8ClampedArray(data);
        return null;
      }

      const diffLimit = Number(diffThr.value);
      let motionPixels = 0;
      let sumR = 0, sumG = 0, sumB = 0;

      const stride = 8; // sample every 2 pixels horizontally
      for (let i = 0; i < data.length; i += stride) {
        const dr = Math.abs(data[i] - bgFrame[i]);
        const dg = Math.abs(data[i + 1] - bgFrame[i + 1]);
        const db = Math.abs(data[i + 2] - bgFrame[i + 2]);
        const d = (dr + dg + db) / 3;
        if (d > diffLimit) {
          motionPixels++;
          sumR += data[i];
          sumG += data[i + 1];
          sumB += data[i + 2];
        }
      }

      const lerp = 0.02;
      for (let i = 0; i < data.length; i++) {
        bgFrame[i] = bgFrame[i] * (1 - lerp) + data[i] * lerp;
      }

      ctx.putImageData(img, 0, bandYpx);
      drawBand(w, h);

      return { motionPixels, bandYpx, bandHeight, w, h, avg: motionPixels ? { r: sumR / motionPixels, g: sumG / motionPixels, b: sumB / motionPixels } : null };
    }

    async function sendLap(carId) {
      try {
        await fetch('/lap_ping?car=' + encodeURIComponent(carId));
      } catch (e) {
        log('Send error: ' + e.message);
      }
    }

    function chooseCar(avgColor) {
      let best = null;
      carProfiles.forEach((car) => {
        const c = hexToRgb(car.color);
        const dist = Math.hypot(c.r - avgColor.r, c.g - avgColor.g, c.b - avgColor.b);
        if (!best || dist < best.dist) {
          best = { car, dist };
        }
      });
      return best && best.dist < 200 ? best.car : (carProfiles[0] || null);
    }

    function loop() {
      const res = processFrame();
      if (res && carProfiles.length) {
        const { motionPixels, avg } = res;
        const threshold = Number(motionPx.value);
        if (motionPixels > threshold && avg) {
          const now = performance.now();
          if (now - lastGlobalHit > 150) {
            const car = chooseCar(avg);
            if (car && now - (car.lastHit || 0) > 200) {
              car.lastHit = now;
              lastGlobalHit = now;
              log(`Motion ${motionPixels} → ${car.label}`);
              sendLap(car.id);
            }
          }
        }
      }
      requestAnimationFrame(loop);
    }

    async function fetchCarStatus() {
      try {
        const res = await fetch('/car_status');
        const data = await res.json();
        const tbody = lapsBody;
        tbody.innerHTML = '';
        (data.cars || []).forEach((car) => {
          const tr = document.createElement('tr');
          const last = car.lastLapMs > 0 ? car.lastLapMsFormatted : '--:--.---';
          const laps = (car.laps || []).map(ms => ms.formatted).join(', ');
          tr.innerHTML = `<td>${car.label}</td><td>${last}</td><td>${laps}</td>`;
          tbody.appendChild(tr);
        });
      } catch (e) {
        // ignore
      }
    }

    async function loadConfig() {
      const res = await fetch('/car_config');
      const data = await res.json();
      maxCars = data.maxCars || 6;
      carProfiles = (data.cars || []).map((c, idx) => ({
        id: c.id || `car${idx + 1}`,
        label: c.label || `Car ${idx + 1}`,
        color: c.color || '#ff4d4f',
        lastHit: 0,
      }));
      if (!carProfiles.length) {
        carProfiles = [{ id: 'car1', label: 'Car 1', color: '#ff4d4f', lastHit: 0 }];
      }
      carCountInput.max = maxCars;
      carCountInput.value = carProfiles.length;
      rebuildCarList();
    }

    setInterval(fetchCarStatus, 600);
    fetchCarStatus();

    resetBgBtn.addEventListener('click', () => { bgFrame = null; log('Background reset. Hold cars still.'); });
    applyCountBtn.addEventListener('click', applyCount);
    saveCarsBtn.addEventListener('click', () => { saveCars(); fetchCarStatus(); });

    loadConfig().then(() => {
      startCamera();
    });
  </script>

</body>
</html>
)HTML";

  server.send(200, "text/html", html);
}

// Ensure defaults are set once at startup
struct CameraInitHook {
  CameraInitHook() { assignDefaults(); resetCarTiming(); }
} cameraInitHook;

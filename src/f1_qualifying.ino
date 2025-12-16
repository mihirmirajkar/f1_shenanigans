#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// === CONFIG ===
const int IR_PIN = 15;               // IR receiver OUT connected here
const bool IR_BROKEN_IS_LOW = true;  // true if LOW means beam broken

// Wi-Fi AP config
const char* AP_SSID = "F1-Quali-Timer";
const char* AP_PASS = "f1qualilap";  // at least 8 chars

// LED pins for F1 start lights
const int LED1 = 14;
const int LED2 = 27;
const int LED3 = 26;
const int LED4 = 25;
const int LED5 = 33;

const int NUM_LEDS = 5;
int leds[NUM_LEDS] = { LED1, LED2, LED3, LED4, LED5 };


// Buzzer pin
const int BUZZER_PIN = 4;  // D4

// === QUALI TIMER STATE ===
WebServer server(80);

bool beamBroken = false;
bool prevBeamBroken = false;

// Timer / lap state
bool timerRunning = false;
bool timerStartedOnce = false;

unsigned long lapStartTime = 0;   // when current lap started
unsigned long lastLapTime = 0;    // last completed lap (ms)
unsigned long stopTime = 0;       // when timer was stopped

// Lap history (last 10 laps, newest at 0)
const int MAX_LAPS = 10;
unsigned long lapTimes[MAX_LAPS];
int lapCount = 0;
// === Race lap timing by barcode (slit count) ===
const int MAX_RACE_CARS = 6;
const int MAX_RACE_LAPS_PER_CAR = 20;
uint16_t raceLapCount[MAX_RACE_CARS] = {0};
unsigned long raceLastCrossMs[MAX_RACE_CARS] = {0};
unsigned long raceLapTimes[MAX_RACE_CARS][MAX_RACE_LAPS_PER_CAR]; // Store all lap times

int raceTargetLaps = 5;     // Target laps to complete (default 5)
int raceWinnerCar = 0;      // 0 = no winner yet, 1-6 = winning car
unsigned long raceWinTimeMs = 0; // Time when winner finished

volatile int lastDetectedCar = 0;        // 1..6
volatile int lastDetectedSlits = -1;     // 0..5
volatile unsigned long lastDetectedAtMs = 0;

// Barcode/slit decoder tuning
const uint32_t BARCODE_END_GAP_US = 30000;     // how long beam must stay clear to consider "frame ended"
const uint32_t BARCODE_EDGE_DEBOUNCE_US = 200; // ignore faster edges (noise) - reduced for faster sampling
const unsigned long MIN_RACE_LAP_MS = 1500;    // ignore faster laps / double-counts


// Modes: true = continuous, false = out-lap
bool continuousMode = true;

// Debounce for IR beam
const unsigned long MIN_TRIGGER_GAP_MS = 500;
unsigned long lastTriggerTime = 0;

// === RACE MODE STATE (LED START SEQUENCE) ===
enum RacePhase {
  RACE_IDLE = 0,
  RACE_COUNTDOWN,
  RACE_LIGHTS_FILL,
  RACE_RANDOM_WAIT,
  RACE_GO_DONE
};

RacePhase racePhase = RACE_IDLE;
unsigned long raceCountdownStartMs = 0;
unsigned long racePhaseStartMs = 0;
unsigned long raceRandomWaitMs = 0;
int lightsLit = 0;  // how many LEDs currently lit (0–5)

// === HELPERS ===
bool readBeamBroken() {
  int raw = digitalRead(IR_PIN);
  if (IR_BROKEN_IS_LOW) {
    return (raw == LOW);
  } else {
    return (raw == HIGH);
  }
}

String formatTime(unsigned long ms) {
  unsigned long totalSeconds = ms / 1000;
  unsigned long minutes = totalSeconds / 60;
  unsigned long seconds = totalSeconds % 60;
  unsigned long millisPart = ms % 1000;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%03lu", minutes, seconds, millisPart);
  return String(buf);
}

void addLapTime(unsigned long ms) {
  for (int i = MAX_LAPS - 1; i > 0; i--) {
    lapTimes[i] = lapTimes[i - 1];
  }
  lapTimes[0] = ms;
  if (lapCount < MAX_LAPS) lapCount++;
}


// Reset per-car race stats (called on race start/finish)
void resetRaceStats() {
  for (int i = 0; i < MAX_RACE_CARS; i++) {
    raceLapCount[i] = 0;
    raceLastCrossMs[i] = 0;
    for (int j = 0; j < MAX_RACE_LAPS_PER_CAR; j++) {
      raceLapTimes[i][j] = 0;
    }
  }
  raceWinnerCar = 0;
  raceWinTimeMs = 0;
  lastDetectedCar = 0;
  lastDetectedSlits = -1;
  lastDetectedAtMs = 0;
}

// LED helpers
void allLightsOff() {
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(leds[i], LOW);
  }
  lightsLit = 0;
}

void setLightsLit(int count) {
  lightsLit = count;
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(leds[i], (i < count) ? HIGH : LOW);
  }
}

// === HTML: Qualifying Page (unchanged from your F1-style version) ===
void handleRoot() {
  // Reset race phase to IDLE when accessing qualifying page
  // This ensures qualifying works after switching from race mode
  if (racePhase != RACE_IDLE) {
    racePhase = RACE_IDLE;
    allLightsOff();
  }
  
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>F1 Qualifying Timer</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <style>
    body {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", "Helvetica Neue", Arial, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
      color: #f5f5f5;
      text-align: center;
      margin: 0;
      padding: 16px;
      min-height: 100vh;
    }

    h1 {
      font-size: 4vw;
      margin-top: 0.3em;
      margin-bottom: 0.1em;
      text-transform: uppercase;
      letter-spacing: 0.15em;
      font-weight: 900;
      color: #ff0050;
      text-shadow: 0 0 20px rgba(255,0,80,0.5), 0 4px 6px rgba(0,0,0,0.3);
    }

    #mode {
      font-size: 4vw;
      margin: 0.2em 0 0.4em;
      color: #29b6f6;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.12em;
    }

    #status {
      font-size: 3.8vw;
      margin-bottom: 0.4em;
      color: #ff5252;
      font-weight: 700;
    }

    #currentTime {
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
      font-size: 14vw;
      font-weight: 900;
      letter-spacing: 0.06em;
      color: #00ff7f;
      margin: 0.15em 0;
      text-shadow: 0 0 30px rgba(0,255,127,0.8), 0 0 60px rgba(0,255,127,0.4);
      background: rgba(0,255,127,0.05);
      padding: 0.2em 0.3em;
      border-radius: 0.15em;
      border: 2px solid rgba(0,255,127,0.2);
    }

    #bestLap {
      font-size: 6vw;
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
      color: #ffc107;
      margin-bottom: 0.6em;
    }

    #lastLap {
      font-size: 6vw;
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
      color: #ffc107;
      margin-bottom: 0.6em;
    }

    button {
      font-size: 4vw;
      padding: 0.55em 1.1em;
      margin: 0.3em;
      border-radius: 0.5em;
      border: 2px solid transparent;
      cursor: pointer;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.1em;
      transition: all 0.2s ease;
      box-shadow: 0 4px 12px rgba(0,0,0,0.3);
    }

    .btn-reset { 
      background: linear-gradient(135deg, #ff0050 0%, #d50032 100%);
      color: #fff;
      border-color: #ff0050;
    }
    .btn-stop { 
      background: linear-gradient(135deg, #ffd700 0%, #ffb700 100%);
      color: #000;
      border-color: #ffd700;
    }
    .btn-mode { 
      background: linear-gradient(135deg, #536976 0%, #292e49 100%);
      color: #fff;
      width: 70%;
      border-color: #536976;
    }
    .btn-page { 
      background: linear-gradient(135deg, #0575e6 0%, #021b79 100%);
      color: #fff;
      width: 70%;
      border-color: #0575e6;
    }

    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(0,0,0,0.4);
    }

    button:active {
      transform: translateY(0) scale(0.98);
      box-shadow: 0 2px 8px rgba(0,0,0,0.3);
    }

    .laps-container {
      max-width: 480px;
      margin: 0.8em auto 0;
      padding: 1em;
      border-top: 2px solid rgba(255,0,80,0.3);
      background: rgba(0,0,0,0.2);
      border-radius: 0.8em;
      box-shadow: 0 4px 12px rgba(0,0,0,0.3);
    }

    .laps-title {
      font-size: 4vw;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      margin-bottom: 0.3em;
      color: #ffffff;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
    }

    th, td {
      padding: 0.25em 0.4em;
      font-size: 3.2vw;
    }

    th {
      text-align: left;
      font-weight: 700;
      color: #bbbbbb;
      border-bottom: 1px solid rgba(255,255,255,0.25);
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    td {
      border-bottom: 1px solid rgba(255,255,255,0.1);
    }

    tr:nth-child(1) td {
      color: #00ff7f;
      font-weight: 700;
    }

    .lap-index { width: 20%; text-align: left; }
    .lap-time  { width: 80%; text-align: right; }

    @media (min-width: 900px) {
      h1           { font-size: 2.5rem; }
      #mode        { font-size: 1.3rem; }
      #status      { font-size: 1.2rem; }
      #currentTime { font-size: 5rem; }
      #bestLap     { font-size: 1.8rem; }
      #lastLap     { font-size: 1.8rem; }
      button       { font-size: 1.1rem; }
      .laps-title  { font-size: 1.2rem; }
      th, td       { font-size: 1rem; }
    }

  
#qualCarTable { width: 100%; border-collapse: collapse; margin-top: 6px; }
#qualCarTable th, #qualCarTable td { border: 1px solid rgba(255,255,255,0.2); padding: 8px; font-size: 14px; }
#qualCarTable th { background: rgba(255,255,255,0.08); }
.seen { background: rgba(41,182,246,0.18); }
</style>
</head>
<body>

  <h1>F1 Qualifying Timer</h1>
  <button class="btn-page" onclick="location.href='/race'">Go to Race Mode</button>

  <div id="mode">Mode: ...</div>
  <button class="btn-mode" onclick="toggleMode()" id="modeButton">Toggle Mode</button>

  <div id="status">Status: ...</div>

<div style="margin-top:10px;">
  <div style="font-size:16px;opacity:0.9;margin-bottom:6px;">Car ID (slits)</div>
  <table id="qualCarTable">
    <thead><tr><th>Car</th><th>Slits</th><th>Last seen</th></tr></thead>
    <tbody id="qualCarTableBody"></tbody>
  </table>
</div>

  <div id="bestLap">Best: --:--.---</div>
  <div id="currentTime">00:00.000</div>
  <div id="lastLap">Last: --:--.---</div>

  <button class="btn-reset" onclick="doReset()">Reset</button>
  <button class="btn-stop" onclick="doStop()">Stop</button>

  <div class="laps-container">
    <div class="laps-title">Lap Times (Last 10)</div>
    <table>
      <thead>
        <tr>
          <th>Lap</th>
          <th>Time</th>
        </tr>
      </thead>
      <tbody id="lapsBody"></tbody>
    </table>
  </div>

  <script>
    let serverCurrentTimeMs = 0;
    let serverLastLapMs = 0;
    let serverRunning = false;
    let lastServerUpdateAtClientMs = 0;
    let serverStatusText = "WAITING";
    let serverLapTimes = [];
    let serverMode = "continuous";

    function formatTime(ms) {
      const totalSeconds = Math.floor(ms / 1000);
      const minutes = Math.floor(totalSeconds / 60);
      const seconds = totalSeconds % 60;
      const millisPart = ms % 1000;

      return (
        String(minutes).padStart(2,'0') + ":" +
        String(seconds).padStart(2,'0') + "." +
        String(millisPart).padStart(3,'0')
      );
    }

    function updateModeLabel() {
      const label = (serverMode === "continuous") ? "Mode: CONTINUOUS" : "Mode: OUT LAP";
      document.getElementById('mode').innerText = label;
      document.getElementById('modeButton').innerText =
        (serverMode === "continuous") ? "Switch to Out Lap" : "Switch to Continuous";
    }

    async function toggleMode() {
      const newMode = (serverMode === "continuous") ? "outlap" : "continuous";
      await fetch('/mode?m=' + newMode);
      serverMode = newMode;
      updateModeLabel();
    }

    function renderLaps() {
      const body = document.getElementById('lapsBody');
      body.innerHTML = "";

      for (let i = 0; i < serverLapTimes.length; i++) {
        const lapMs = serverLapTimes[i];
        const tr = document.createElement('tr');

        const tdIdx = document.createElement('td');
        tdIdx.textContent = "#" + (i + 1);

        const tdTime = document.createElement('td');
        tdTime.textContent = formatTime(lapMs);

        tr.appendChild(tdIdx);
        tr.appendChild(tdTime);
        body.appendChild(tr);
      }
    }

    async function fetchStatus() {
      try {
        const res = await fetch('/status');
        const data = await res.json();

        serverRunning = data.running;
        serverCurrentTimeMs = data.currentTimeMs;
        serverLastLapMs = data.lastLapMs;
        serverStatusText = data.status;
        serverLapTimes = data.lapTimes || [];
        serverMode = data.mode;

        lastServerUpdateAtClientMs = Date.now();

        document.getElementById('status').innerText = "Status: " + serverStatusText;

// Car table (slit-count IDs)
const tb = document.getElementById('qualCarTableBody');
if (tb) {
  const lastCar = data.lastCar || 0;
  const lastAt = data.lastCarAtMs || 0;
  let html = "";
  for (let c = 1; c <= 6; c++) {
    const slits = c - 1;
    const seen = (c === lastCar) ? "✓" : "";
    const cls = (c === lastCar) ? "seen" : "";
    html += `<tr class="${cls}"><td>${c}</td><td>${slits}</td><td>${seen}</td></tr>`;
  }
  tb.innerHTML = html;
}

        // Calculate and display best lap
        if (serverLapTimes.length > 0) {
          const bestLapMs = Math.min(...serverLapTimes);
          document.getElementById('bestLap').innerText = "Best: " + formatTime(bestLapMs);
        } else {
          document.getElementById('bestLap').innerText = "Best: --:--.---";
        }

        if (serverLastLapMs > 0) {
          document.getElementById('lastLap').innerText = "Last: " + formatTime(serverLastLapMs);
        } else {
          document.getElementById('lastLap').innerText = "Last: --:--.---";
        }

        updateModeLabel();
        renderLaps();
      } catch (e) {
        console.error(e);
      }
    }

    function animationLoop() {
      let displayMs = serverCurrentTimeMs;

      if (serverRunning) {
        const elapsed = Date.now() - lastServerUpdateAtClientMs;
        displayMs = serverCurrentTimeMs + elapsed;
      }

      document.getElementById('currentTime').innerText = formatTime(displayMs);
      requestAnimationFrame(animationLoop);
    }

    async function doReset() {
      await fetch('/reset');
      serverCurrentTimeMs = 0;
      serverLastLapMs = 0;
      serverLapTimes = [];
      serverRunning = false;
      document.getElementById('currentTime').innerText = formatTime(0);
      document.getElementById('bestLap').innerText = "Best: --:--.---";
      document.getElementById('lastLap').innerText = "Last: --:--.---";
      renderLaps();
    }

    async function doStop() {
      await fetch('/stop');
      setTimeout(fetchStatus, 200);
    }

    fetchStatus();
    setInterval(fetchStatus, 250);
    requestAnimationFrame(animationLoop);
  </script>

</body>
</html>
)HTML";

  server.send(200, "text/html", html);
}

// === HTML: Race Mode Page ===
void handleRacePage() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>F1 Race Start</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <style>
    body {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", "Helvetica Neue", Arial, sans-serif;
      background: linear-gradient(135deg, #0f0c29 0%, #302b63 50%, #24243e 100%);
      color: #f5f5f5;
      text-align: center;
      margin: 0;
      padding: 16px;
      min-height: 100vh;
    }

    h1 {
      font-size: 4.5vw;
      margin-top: 0.3em;
      margin-bottom: 0.2em;
      text-transform: uppercase;
      letter-spacing: 0.18em;
      font-weight: 900;
      color: #ff0050;
      text-shadow: 0 0 25px rgba(255,0,80,0.6), 0 4px 8px rgba(0,0,0,0.4);
    }

    #raceStatus {
      font-size: 4vw;
      margin-bottom: 0.8em;
      color: #29b6f6;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      font-weight: 700;
    }

    #countdown {
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
      font-size: 20vw;
      font-weight: 900;
      color: #ff5252;
      margin: 0.2em 0;
      text-shadow: 0 0 40px rgba(255,82,82,1), 0 0 80px rgba(255,82,82,0.5);
      background: rgba(255,82,82,0.08);
      padding: 0.15em 0.3em;
      border-radius: 0.1em;
      border: 3px solid rgba(255,82,82,0.3);
    }

    button {
      font-size: 5vw;
      padding: 0.6em 1.2em;
      margin: 0.4em;
      border-radius: 0.5em;
      border: 2px solid transparent;
      cursor: pointer;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.1em;
      transition: all 0.2s ease;
      box-shadow: 0 6px 16px rgba(0,0,0,0.4);
    }

    .btn-start { 
      background: linear-gradient(135deg, #00e676 0%, #00c853 100%);
      color: #fff;
      border-color: #00e676;
    }
    .btn-back { 
      background: linear-gradient(135deg, #0575e6 0%, #021b79 100%);
      color: #fff;
      border-color: #0575e6;
    }
    .btn-finish { 
      background: linear-gradient(135deg, #ff0050 0%, #d50032 100%);
      color: #fff;
      border-color: #ff0050;
    }

    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 8px 24px rgba(0,0,0,0.5);
    }

    button:active {
      transform: translateY(0) scale(0.98);
      box-shadow: 0 3px 10px rgba(0,0,0,0.3);
    }

    @media (min-width: 900px) {
      h1           { font-size: 2.8rem; }
      #raceStatus  { font-size: 1.4rem; }
      #countdown   { font-size: 6rem; }
      button       { font-size: 1.6rem; }
    }
  
table { width: 100%; border-collapse: collapse; margin-top: 8px; }
th, td { border: 1px solid rgba(255,255,255,0.2); padding: 8px; font-size: 14px; }
th { background: rgba(255,255,255,0.08); }
.car-highlight { background: rgba(41,182,246,0.18); }
</style>
</head>
<body>

  <h1>F1 Race Start</h1>
  
  <div style="margin: 12px auto; max-width: 400px;">
    <label style="font-size: 16px; color: #29b6f6; margin-right: 10px;">Target Laps:</label>
    <input type="number" id="targetLaps" value="5" min="1" max="50" 
           style="width: 80px; padding: 8px; font-size: 18px; border-radius: 6px; border: 2px solid #0575e6; background: #1a1a2e; color: #fff; text-align: center;">
    <button onclick="setTargetLaps()" style="padding: 8px 16px; font-size: 14px; margin-left: 10px;">Set</button>
  </div>
  
  <div id="winnerDisplay" style="display:none; margin: 16px auto; padding: 20px; background: linear-gradient(135deg, #ffd700 0%, #ffb700 100%); border-radius: 12px; max-width: 400px; box-shadow: 0 8px 24px rgba(255,215,0,0.5);">
    <div style="font-size: 28px; font-weight: 900; color: #000; margin-bottom: 8px;">🏆 WINNER! 🏆</div>
    <div id="winnerText" style="font-size: 24px; font-weight: 700; color: #000;"></div>
  </div>
  
  <div id="raceStatus">Idle</div>
  <div id="countdown">--</div>
  <div id="lastCar" style="margin-top:8px;font-size:18px;">Last car: --</div>
  <div style="margin-top:10px;">
  <div id="carLapDisplay"></div>
</div>

  <button class="btn-start" onclick="startRace()">Start</button>
  <button class="btn-finish" onclick="finishRace()">Finish</button>
  <button class="btn-back" onclick="location.href='/'">Back to Quali</button>

  <audio id="goAudio" preload="auto">
  </audio>

  <script>
    const goAudio = document.getElementById('goAudio');
    let lastPhase = '';
    let audioUnlocked = false;
    let randomInterval = null;
    let randomTimeout = null;
    let isFinished = false;
    let targetLaps = 5;

    function setTargetLaps() {
      const input = document.getElementById('targetLaps');
      targetLaps = parseInt(input.value) || 5;
      fetch('/set_target_laps?laps=' + targetLaps);
      alert('Target set to ' + targetLaps + ' laps');
    }

    // Unlock audio on any user interaction (required for autoplay)
    document.addEventListener('click', function() {
      if (!audioUnlocked) {
        goAudio.play().then(() => {
          goAudio.pause();
          goAudio.currentTime = 0;
          audioUnlocked = true;
          console.log('Audio unlocked');
        }).catch(e => console.log('Audio unlock failed:', e));
      }
    }, { once: true });


    async function startRace() {
      goAudio.src = '/lights_out_' + (Math.floor(Math.random() * 8) + 1) + '.mp3';
      if (!audioUnlocked) {
        try {
          await goAudio.play();
          goAudio.pause();
          goAudio.currentTime = 0;
          audioUnlocked = true;
        } catch (e) {}
      }
      await fetch('/race_start');
      setTimeout(fetchRaceStatus, 150);
    }

    async function fetchRaceStatus() {
      try {
        const res = await fetch('/race_status');
        const data = await res.json();

        document.getElementById('raceStatus').innerText = data.phaseText;
        document.getElementById('countdown').innerText = data.display;
        
        // Show winner if we have one
        if (data.winnerCar && data.winnerCar > 0) {
          document.getElementById('winnerDisplay').style.display = 'block';
          document.getElementById('winnerText').innerText = 'Car ' + data.winnerCar + ' wins!';
        } else {
          document.getElementById('winnerDisplay').style.display = 'none';
        }
        
        // Update target laps from server
        if (data.targetLaps) {
          targetLaps = data.targetLaps;
          document.getElementById('targetLaps').value = targetLaps;
        }

        // Barcode/slit lap timing
        const carText = (data.lastCar && data.lastCar > 0) ? ("Car " + data.lastCar) : "--";
        const slitText = (data.lastSlits !== undefined && data.lastSlits >= 0) ? (" (" + data.lastSlits + " slits)") : "";
        const lastEl = document.getElementById('lastCar');
        if (lastEl) lastEl.innerText = "Last car: " + carText + slitText;

        // Display all laps for each car
        const carDisplay = document.getElementById('carLapDisplay');
        if (carDisplay && data.lapTimes) {
          const lastCarNum = data.lastCar || 0;
          let html = "";
          for (let i = 0; i < data.lapTimes.length; i++) {
            const carNum = i + 1;
            const laps = data.lapTimes[i] || [];
            const lapCount = data.lapCount[i] || 0;
            
            if (lapCount > 0) {
              const cls = (carNum === lastCarNum) ? "car-highlight" : "";
              const best = laps.length > 0 ? Math.min(...laps.filter(l => l > 0)) : 0;
              const isWinner = (data.winnerCar === carNum);
              const borderColor = isWinner ? 'rgba(255,215,0,0.8)' : 'rgba(255,255,255,0.2)';
              const bgColor = isWinner ? 'rgba(255,215,0,0.15)' : 'rgba(0,0,0,0.3)';
              
              html += `<div style="margin-bottom: 16px; padding: 12px; border: 2px solid ${borderColor}; border-radius: 6px; background: ${bgColor};" class="${cls}">`;
              html += `<div style="font-size: 18px; font-weight: 700; margin-bottom: 8px; color: #29b6f6;">Car ${carNum} — ${lapCount}/${targetLaps} laps${isWinner ? ' 🏆' : ''}</div>`;
              
              if (best > 0) {
                html += `<div style="font-size: 14px; margin-bottom: 8px; color: #ffc107;">Best: ${(best / 1000).toFixed(3)}s</div>`;
              }
              
              html += `<div style="font-family: 'SF Mono','Roboto Mono',monospace; font-size: 13px; line-height: 1.6;">`;
              for (let j = 0; j < laps.length && j < lapCount; j++) {
                const lapTime = laps[j];
                const isBest = lapTime === best;
                const color = isBest ? '#ffc107' : '#ffffff';
                html += `<div style="color: ${color};">${j + 1}. ${(lapTime / 1000).toFixed(3)}s${isBest ? ' 🏆' : ''}</div>`;
              }
              html += `</div></div>`;
            }
          }
          if (html === "") {
            html = `<div style="padding: 20px; color: rgba(255,255,255,0.5);">No laps recorded yet</div>`;
          }
          carDisplay.innerHTML = html;
        }

        // Play audio when phase changes to GO!
        if (data.phaseText === 'GO!' && lastPhase !== 'GO!') {
          isFinished = false;
          // Clear any existing intervals/timeouts
          if (randomInterval) {
            clearInterval(randomInterval);
            randomInterval = null;
          }
          if (randomTimeout) {
            clearTimeout(randomTimeout);
            randomTimeout = null;
          }
          goAudio.currentTime = 0;
          goAudio.play().catch(e => console.log('Audio play failed:', e));
          setTimeout(() => {
            if (isFinished) return;
            goAudio.src = '/turn_one_' + (Math.floor(Math.random() * 8) + 1) + '.mp3';
            goAudio.currentTime = 0;
            goAudio.onended = () => {
              if (isFinished) return;
              randomTimeout = setTimeout(() => {
                if (isFinished) return;
                playRandom();
              }, 15000);
            };
            goAudio.play().catch(e => console.log('Turn audio play failed:', e));
          }, 3000);
        }
        lastPhase = data.phaseText;
      } catch (e) {
        console.error('Fetch error:', e);
      }
    }

    function playRandom() {
      if (isFinished) return;
      goAudio.src = '/random_' + (Math.floor(Math.random() * 17) + 1) + '.mp3';
      goAudio.currentTime = 0;
      goAudio.onended = () => {
        if (isFinished) return;
        randomTimeout = setTimeout(() => {
          if (isFinished) return;
          playRandom();
        }, 15000);
      };
      goAudio.play().catch(e => console.log('Random audio play failed:', e));
    }

    async function finishRace() {
      isFinished = true;
      if (randomInterval) {
        clearInterval(randomInterval);
        randomInterval = null;
      }
      if (randomTimeout) {
        clearTimeout(randomTimeout);
        randomTimeout = null;
      }
      goAudio.onended = null;
      goAudio.pause();
      await fetch('/race_finish');
      setTimeout(fetchRaceStatus, 150);
    }

    setInterval(fetchRaceStatus, 150);
    fetchRaceStatus();
  </script>

</body>
</html>
)HTML";

  server.send(200, "text/html", html);
}

// === API: Quali /status ===
void handleStatus() {
  unsigned long now = millis();
  unsigned long currentTimeMs = 0;

  if (timerRunning) {
    currentTimeMs = now - lapStartTime;
  } else if (timerStartedOnce && stopTime > lapStartTime) {
    currentTimeMs = stopTime - lapStartTime;
  } else {
    currentTimeMs = 0;
  }

  String status;
  if (!timerStartedOnce) {
    status = "WAITING FOR FIRST PASS";
  } else if (timerRunning) {
    status = "RUNNING";
  } else {
    status = "FINISHED";
  }

  String modeStr = continuousMode ? "continuous" : "outlap";

  String json = "{";
  json += "\"status\":\"" + status + "\",";
  json += "\"currentTimeMs\":" + String(currentTimeMs) + ",";
  json += "\"lastLapMs\":" + String(lastLapTime) + ",";
  json += "\"running\":" + String(timerRunning ? "true" : "false") + ",";
  json += "\"mode\":\"" + modeStr + "\",";
  json += "\"lastCar\":" + String((int)lastDetectedCar) + ",";
  json += "\"lastSlits\":" + String((int)lastDetectedSlits) + ",";
  json += "\"lastCarAtMs\":" + String((unsigned long)lastDetectedAtMs) + ",";
  json += "\"lapTimes\":[";
  for (int i = 0; i < lapCount; i++) {
    json += String(lapTimes[i]);
    if (i < lapCount - 1) json += ",";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleReset() {
  timerRunning = false;
  timerStartedOnce = false;
  lapStartTime = 0;
  lastLapTime = 0;
  stopTime = 0;
  lapCount = 0;
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  if (timerRunning) {
    stopTime = millis();
    lastLapTime = stopTime - lapStartTime;
    timerRunning = false;
    addLapTime(lastLapTime);
  }
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  if (!server.hasArg("m")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }
  String m = server.arg("m");
  if (m == "continuous") {
    continuousMode = true;
  } else {
    continuousMode = false;
  }
  server.send(200, "text/plain", "OK");
}

// === API: Race Mode ===
void handleRaceStart() {
  // Reset sequence
  allLightsOff();
  resetRaceStats();
  racePhase = RACE_COUNTDOWN;
  raceCountdownStartMs = millis();
  racePhaseStartMs = raceCountdownStartMs;
  raceRandomWaitMs = 0;
  server.send(200, "text/plain", "OK");
}

void handleRaceFinish() {
  // Reset race phase to IDLE
  allLightsOff();
  resetRaceStats();
  racePhase = RACE_IDLE;
  server.send(200, "text/plain", "OK");
}

void handleSetTargetLaps() {
  if (server.hasArg("laps")) {
    int laps = server.arg("laps").toInt();
    if (laps >= 1 && laps <= 50) {
      raceTargetLaps = laps;
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Invalid laps");
}

// Serve MP3 file from LittleFS
void handleMP3() {
  File file = LittleFS.open(server.uri(), "r");
  if (!file) {
    Serial.println("ERROR: MP3 file not found in LittleFS");
    server.send(404, "text/plain", "File not found");
    return;
  }
  
  Serial.println("Serving MP3 file");
  server.sendHeader("Content-Type", "audio/mpeg");
  server.sendHeader("Accept-Ranges", "bytes");
  server.streamFile(file, "audio/mpeg");
  file.close();
}

void handleRaceStatus() {
  unsigned long now = millis();
  String phaseText;
  String display;

  switch (racePhase) {
    case RACE_IDLE:
      phaseText = "Idle";
      display = "--";
      break;
    case RACE_COUNTDOWN: {
      phaseText = "Countdown";
      unsigned long elapsed = now - raceCountdownStartMs;
      long remaining = 5000 - (long)elapsed;
      if (remaining < 0) remaining = 0;
      long secs = (remaining + 999) / 1000; // round up
      display = String(secs);
      break;
    }
    case RACE_LIGHTS_FILL:
      phaseText = "Lights On";
      display = "●●●";
      break;
    case RACE_RANDOM_WAIT:
      phaseText = "Ready";
      display = "●";
      break;
    case RACE_GO_DONE:
      phaseText = "GO!";
      display = "GO";
      break;
  }

  String json = "{";
  json += "\"phaseText\":\"" + phaseText + "\",";
  json += "\"display\":\"" + display + "\",";
  json += "\"lastCar\":" + String(lastDetectedCar) + ",";
  json += "\"lastSlits\":" + String(lastDetectedSlits) + ",";
  json += "\"lastAtMs\":" + String(lastDetectedAtMs) + ",";
  json += "\"targetLaps\":" + String(raceTargetLaps) + ",";
  json += "\"winnerCar\":" + String(raceWinnerCar) + ",";

  json += "\"lapCount\":[";
  for (int i = 0; i < MAX_RACE_CARS; i++) {
    json += String(raceLapCount[i]);
    if (i < MAX_RACE_CARS - 1) json += ",";
  }
  json += "],\"lapTimes\":[";
  for (int i = 0; i < MAX_RACE_CARS; i++) {
    json += "[";
    int lapsToPrint = (raceLapCount[i] < MAX_RACE_LAPS_PER_CAR) ? raceLapCount[i] : MAX_RACE_LAPS_PER_CAR;
    for (int j = 0; j < lapsToPrint; j++) {
      json += String(raceLapTimes[i][j]);
      if (j < lapsToPrint - 1) json += ",";
    }
    json += "]";
    if (i < MAX_RACE_CARS - 1) json += ",";
  }
  json += "]";

  json += "}";

  server.send(200, "application/json", json);
}

// === SETUP & LOOP ===
void setup() {
  Serial.begin(115200);
  pinMode(IR_PIN, INPUT_PULLUP);

  // LED pins
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(leds[i], OUTPUT);
    digitalWrite(leds[i], LOW);
  }

  // Buzzer pin
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Race mode initial
  racePhase = RACE_IDLE;
  allLightsOff();
  randomSeed(analogRead(0));

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
  } else {
    Serial.println("LittleFS Mounted");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println();
  Serial.print("AP started. Connect to WiFi SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.println("Then open: http://192.168.4.1");

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/reset", handleReset);
  server.on("/stop", handleStop);
  server.on("/mode", handleMode);

  server.on("/race", handleRacePage);
  server.on("/race_start", handleRaceStart);
  server.on("/race_status", handleRaceStatus);
  server.on("/race_finish", handleRaceFinish);
  server.on("/set_target_laps", handleSetTargetLaps);
  
  // Register routes for all MP3 files
  for (int i = 1; i <= 8; i++) {
    String route = "/lights_out_" + String(i) + ".mp3";
    server.on(route.c_str(), handleMP3);
    route = "/turn_one_" + String(i) + ".mp3";
    server.on(route.c_str(), handleMP3);
  }
  for (int i = 1; i <= 17; i++) {
    String route = "/random_" + String(i) + ".mp3";
    server.on(route.c_str(), handleMP3);
  }

  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // === Quali IR logic ===
  // Always read beam state to keep prevBeamBroken updated
  // But only process triggers when race mode is idle (not active)
  // This prevents qualifying timer from tracking times during race mode
  bool currentBroken = readBeamBroken();

  // === Barcode/slit decode (single beam) ===
  // FAST SAMPLING approach: When beam breaks, enter tight sampling loop to catch all transitions
  // This ensures we don't miss narrow slits on angled passes
  // CarId mapping: Car1 = 0 slits, Car2 = 1 slit, ... Car6 = 5 slits.
  static bool bcActive = false;
  static bool bcPrevState = false;
  static uint32_t bcLastEdgeUs = 0;
  static int bcTransitions = 0;

  uint32_t nowUs = micros();

  // Start fast sampling when beam first breaks (barcode detected)
  if (!bcActive && currentBroken) {
    bcActive = true;
    bcTransitions = 0;
    bcPrevState = currentBroken;
    bcLastEdgeUs = nowUs;
    
    // FAST SAMPLING LOOP - sample at maximum speed while barcode passes
    while (true) {
      nowUs = micros();
      bool currentState = readBeamBroken();
      
      // Detect state change with debounce
      if (currentState != bcPrevState) {
        uint32_t timeSinceLastEdge = nowUs - bcLastEdgeUs;
        if (timeSinceLastEdge > BARCODE_EDGE_DEBOUNCE_US) {
          bcTransitions++;
          bcPrevState = currentState;
          bcLastEdgeUs = nowUs;
        }
      }
      
      // Exit fast sampling when beam clear for end-gap duration
      if (!currentState && (nowUs - bcLastEdgeUs > BARCODE_END_GAP_US)) {
        // Frame complete - decode it
        int bcSlits = bcTransitions / 2;
        int carId = bcSlits + 1;
        
        if (carId >= 1 && carId <= MAX_RACE_CARS) {
          lastDetectedCar = carId;
          lastDetectedSlits = bcSlits;
          lastDetectedAtMs = millis();
          
          // Update lap timing if in race GO mode
          if (racePhase == RACE_GO_DONE) {
            int ci = carId - 1;
            unsigned long now = millis();
            if (raceLastCrossMs[ci] != 0) {
              unsigned long lap = now - raceLastCrossMs[ci];
              if (lap >= MIN_RACE_LAP_MS) {
                if (raceLapCount[ci] < MAX_RACE_LAPS_PER_CAR) {
                  raceLapTimes[ci][raceLapCount[ci]] = lap;
                }
                raceLapCount[ci]++;
                
                // Check if this car just won (first to reach target laps)
                if (raceWinnerCar == 0 && raceLapCount[ci] >= raceTargetLaps) {
                  raceWinnerCar = carId;
                  raceWinTimeMs = now;
                }
              }
            }
            raceLastCrossMs[ci] = now;
          }
        } else {
          lastDetectedCar = 0;
          lastDetectedSlits = bcSlits;
          lastDetectedAtMs = millis();
        }
        
        bcActive = false;
        break;
      }
      
      // Safety timeout: exit if frame takes too long (500ms max)
      if (nowUs - bcLastEdgeUs > 500000) {
        bcActive = false;
        break;
      }
      
      // Small delay to prevent overwhelming the sensor
      delayMicroseconds(50);
    }
  }




  if (racePhase == RACE_IDLE) {
    if (currentBroken && !prevBeamBroken) {
      if (now - lastTriggerTime > MIN_TRIGGER_GAP_MS) {
        lastTriggerTime = now;

        if (!timerStartedOnce) {
          timerStartedOnce = true;
          timerRunning = true;
          lapStartTime = now;
          Serial.println("First pass detected. Lap timing started.");
        } else if (timerRunning) {
          stopTime = now;
          lastLapTime = stopTime - lapStartTime;
          addLapTime(lastLapTime);

          if (continuousMode) {
            lapStartTime = now;
            timerRunning = true;
            Serial.print("Lap complete (continuous). Lap time (ms): ");
            Serial.println(lastLapTime);
          } else {
            timerRunning = false;
            Serial.print("Lap complete (out lap). Lap time (ms): ");
            Serial.println(lastLapTime);
          }
        } else {
          timerRunning = true;
          lapStartTime = now;
          Serial.println("New lap started.");
        }
      }
    }
  }

  // Always update prevBeamBroken to keep state in sync
  prevBeamBroken = currentBroken;

  // === Race Mode State Machine ===
  switch (racePhase) {
    case RACE_IDLE:
      // nothing to do
      break;

    case RACE_COUNTDOWN: {
      unsigned long elapsed = now - raceCountdownStartMs;
      if (elapsed >= 5000) {
        // Start lights fill phase
        racePhase = RACE_LIGHTS_FILL;
        racePhaseStartMs = now;
        lightsLit = 0;
        allLightsOff();
      }
      break;
    }

    case RACE_LIGHTS_FILL: {
      unsigned long elapsed = now - racePhaseStartMs;
      int step = elapsed / 1000; // each second, add a light
      if (step > NUM_LEDS) step = NUM_LEDS;
      if (step != lightsLit) {
        setLightsLit(step);
        // Play buzzer tone when a new LED lights up
        tone(BUZZER_PIN, 550, 200);
      }
      if (step >= NUM_LEDS) {
        // All lights on, move to random wait
        racePhase = RACE_RANDOM_WAIT;
        racePhaseStartMs = now;
        raceRandomWaitMs = random(700, 1800); // 0.7 to 1.8 s
        Serial.print("Random wait (ms): ");
        Serial.println(raceRandomWaitMs);
      }
      break;
    }

    case RACE_RANDOM_WAIT: {
      unsigned long elapsed = now - racePhaseStartMs;
      if (elapsed >= raceRandomWaitMs) {
        // Lights out, GO!
        allLightsOff();
        racePhase = RACE_GO_DONE;
        Serial.println("LIGHTS OUT! GO!");
      }
      break;
    }

    case RACE_GO_DONE:
      // Audio is played in browser when UI shows "GO!"
      break;
  }
}
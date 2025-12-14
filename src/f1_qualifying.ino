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

// Modes: true = continuous, false = out-lap
bool continuousMode = false;

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
      background: radial-gradient(circle at top, #202020 0, #000 55%);
      color: #f5f5f5;
      text-align: center;
      margin: 0;
      padding: 16px;
    }

    h1 {
      font-size: 4vw;
      margin-top: 0.3em;
      margin-bottom: 0.1em;
      text-transform: uppercase;
      letter-spacing: 0.15em;
      font-weight: 900;
      color: #ffeb3b;
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
      margin: 0.05em 0;
      text-shadow: 0 0 12px rgba(0,255,127,0.7);
    }

    #lastLap {
      font-size: 6vw;
      font-family: "SF Mono","Roboto Mono",Menlo,Consolas,monospace;
      color: #ffc107;
      margin-bottom: 0.6em;
    }

    button {
      font-size: 4vw;
      padding: 0.45em 0.9em;
      margin: 0.25em;
      border-radius: 0.4em;
      border: none;
      cursor: pointer;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .btn-reset { background: #f44336; color: #fff; }
    .btn-stop  { background: #ffc107; color: #000; }
    .btn-mode  { background: #424242; color: #fff; width: 70%; }
    .btn-page  { background: #1565c0; color: #fff; width: 70%; }

    .btn-reset:active, .btn-stop:active, .btn-mode:active, .btn-page:active {
      transform: scale(0.97);
    }

    .laps-container {
      max-width: 480px;
      margin: 0.8em auto 0;
      padding-top: 0.4em;
      border-top: 1px solid rgba(255,255,255,0.15);
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

  </style>
</head>
<body>

  <h1>F1 Qualifying Timer</h1>
  <button class="btn-page" onclick="location.href='/race'">Go to Race Mode</button>

  <div id="mode">Mode: ...</div>
  <button class="btn-mode" onclick="toggleMode()" id="modeButton">Toggle Mode</button>

  <div id="status">Status: ...</div>

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
    let serverMode = "outlap";

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
      background: radial-gradient(circle at top, #202020 0, #000 55%);
      color: #f5f5f5;
      text-align: center;
      margin: 0;
      padding: 16px;
    }

    h1 {
      font-size: 4.5vw;
      margin-top: 0.3em;
      margin-bottom: 0.2em;
      text-transform: uppercase;
      letter-spacing: 0.18em;
      font-weight: 900;
      color: #ffeb3b;
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
      text-shadow: 0 0 14px rgba(255,82,82,0.8);
    }

    button {
      font-size: 5vw;
      padding: 0.5em 1em;
      margin: 0.4em;
      border-radius: 0.4em;
      border: none;
      cursor: pointer;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .btn-start { background: #00c853; color: #fff; }
    .btn-back  { background: #1565c0; color: #fff; }

    .btn-start:active, .btn-back:active {
      transform: scale(0.97);
    }

    @media (min-width: 900px) {
      h1           { font-size: 2.8rem; }
      #raceStatus  { font-size: 1.4rem; }
      #countdown   { font-size: 6rem; }
      button       { font-size: 1.6rem; }
    }
  </style>
</head>
<body>

  <h1>F1 Race Start</h1>
  <div id="raceStatus">Idle</div>
  <div id="countdown">--</div>

  <button class="btn-start" onclick="startRace()">Start</button>
  <button class="btn-back" onclick="location.href='/'">Back to Quali</button>

  <audio id="goAudio" preload="auto">
  </audio>

  <script>
    const goAudio = document.getElementById('goAudio');
    let lastPhase = '';
    let audioUnlocked = false;

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

        // Play audio when phase changes to GO!
        if (data.phaseText === 'GO!' && lastPhase !== 'GO!') {
          goAudio.currentTime = 0;
          goAudio.play().catch(e => console.log('Audio play failed:', e));
          setTimeout(() => {
            goAudio.src = '/turn_one_' + (Math.floor(Math.random() * 8) + 1) + '.mp3';
            goAudio.currentTime = 0;
            goAudio.play().catch(e => console.log('Turn audio play failed:', e));
          }, 3000);
        }
        lastPhase = data.phaseText;
      } catch (e) {
        console.error('Fetch error:', e);
      }
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
  racePhase = RACE_COUNTDOWN;
  raceCountdownStartMs = millis();
  racePhaseStartMs = raceCountdownStartMs;
  raceRandomWaitMs = 0;
  server.send(200, "text/plain", "OK");
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
  json += "\"display\":\"" + display + "\"";
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
  
  // Register routes for all MP3 files
  for (int i = 1; i <= 8; i++) {
    String route = "/lights_out_" + String(i) + ".mp3";
    server.on(route.c_str(), handleMP3);
    route = "/turn_one_" + String(i) + ".mp3";
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
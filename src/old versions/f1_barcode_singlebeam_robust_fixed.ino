// Forward decl to satisfy Arduino auto-prototype generator
struct DecodeResult;
/*
  Single break-beam "self-clocking" barcode lap timer (ESP32 / Arduino)
  - Robust to speed changes while crossing the line (local spacer-based decoding)
  - One sensor on the arch, one cardboard/plastic strip ("barcode") on each car

  Sensor wiring:
    IR_PIN receives the break-beam receiver output.
    Set IR_BROKEN_IS_LOW = true if the receiver output goes LOW when the beam is blocked.

  Barcode format along the strip (direction of travel, leading edge first):
    START: 3U BLOCKED
    Then 3 bits (MSB->LSB), each bit is:
      1U CLEAR spacer, then
      1U BLOCKED = bit 0
      2U BLOCKED = bit 1
    STOP: 3U CLEAR (no edge required; we measure by the silence after the last edge)

  Car mapping (3-bit id -> car #):
    Car 1 = 000
    Car 2 = 001
    Car 3 = 010
    Car 4 = 011
    Car 5 = 100
    Car 6 = 101
*/

#include <Arduino.h>

static const int IR_PIN = 15;                  // break-beam receiver output
static const bool IR_BROKEN_IS_LOW = true;     // true if LOW means beam broken

// ==== Tuning ====
static const uint32_t MIN_EDGE_US       = 200;     // ignore bounces/noise edges closer than this
static const uint32_t END_SILENCE_US    = 50000;   // end-of-frame when no edges for this long (and beam is clear)
static const uint32_t MAX_FRAME_US      = 600000;  // safety: abort if a "frame" lasts longer than this
static const uint32_t MIN_SEG_US        = 350;     // ignore ultra-short segments during decode
static const float    START_RATIO_MIN   = 2.2f;    // start blocked should be >= this * first spacer
static const float    STOP_RATIO_MIN    = 1.8f;    // stop clear should be >= this * last spacer
static const float    ONE_THRESHOLD     = 1.5f;    // pulse/spacer > this => bit 1, else 0

static const int MAX_EDGES = 32; // we expect ~8 edges; this is generous

// ==== ISR capture buffer ====
volatile bool     g_capturing = false;
volatile bool     g_lastBroken = false;
volatile uint32_t g_lastEdgeUs = 0;
volatile uint32_t g_startUs = 0;

volatile int      g_edgeCount = 0;
volatile uint32_t g_edgeUs[MAX_EDGES];
volatile bool     g_edgeBroken[MAX_EDGES];

inline bool readBeamBrokenFast() {
  int raw = digitalRead(IR_PIN);
  return IR_BROKEN_IS_LOW ? (raw == LOW) : (raw == HIGH);
}

void IRAM_ATTR onBeamEdge() {
  uint32_t nowUs = micros();
  bool broken = readBeamBrokenFast();

  // basic noise filter
  if (nowUs - g_lastEdgeUs < MIN_EDGE_US) {
    g_lastBroken = broken;
    return;
  }

  // Start capture only on a clean clear->broken transition
  if (!g_capturing) {
    if (broken && !g_lastBroken) {
      g_capturing = true;
      g_startUs = nowUs;
      g_edgeCount = 0;
    } else {
      g_lastBroken = broken;
      g_lastEdgeUs = nowUs;
      return;
    }
  }

  if (g_edgeCount < MAX_EDGES) {
    g_edgeUs[g_edgeCount] = nowUs;
    g_edgeBroken[g_edgeCount] = broken; // state AFTER this edge
    g_edgeCount++;
  }

  g_lastEdgeUs = nowUs;
  g_lastBroken = broken;
}

struct DecodeResult {
  bool ok;
  int carNum;        // 1..6 if ok, else -1
  uint8_t bits;      // 0..7 decoded id
  String debug;
};

// Explicit prototype to prevent Arduino from generating a broken one
DecodeResult decodeFrame(const uint32_t *edgeUs, const bool *edgeBroken, int edgeCount, uint32_t frameEndUs);

DecodeResult decodeFrame(const uint32_t *edgeUs, const bool *edgeBroken, int edgeCount, uint32_t frameEndUs) {
  DecodeResult r;
  r.ok = false;
  r.carNum = -1;
  r.bits = 0;
  r.debug = "";

  if (edgeCount < 6) {
    r.debug = "Too few edges";
    return r;
  }

  // Filter edges that would create extremely short segments
  uint32_t fEdgeUs[MAX_EDGES];
  bool     fEdgeBroken[MAX_EDGES];
  int fCount = 0;

  for (int i = 0; i < edgeCount; i++) {
    if (fCount == 0) {
      fEdgeUs[fCount] = edgeUs[i];
      fEdgeBroken[fCount] = edgeBroken[i];
      fCount++;
      continue;
    }
    uint32_t dt = edgeUs[i] - fEdgeUs[fCount - 1];
    if (dt < MIN_SEG_US) {
      // ignore this edge as noise
      continue;
    }
    fEdgeUs[fCount] = edgeUs[i];
    fEdgeBroken[fCount] = edgeBroken[i];
    fCount++;
    if (fCount >= MAX_EDGES) break;
  }

  if (fCount < 6) {
    r.debug = "Edges filtered too much";
    return r;
  }

  // Build segments (durations + states). Segment state is the state AFTER an edge until next edge.
  // We start capture on clear->broken, so first segment should be broken.
  uint32_t segDur[16];
  bool     segBroken[16];
  int segCount = 0;

  bool curState = fEdgeBroken[0]; // should be broken
  for (int i = 1; i < fCount && segCount < 16; i++) {
    uint32_t dur = fEdgeUs[i] - fEdgeUs[i - 1];
    if (dur < MIN_SEG_US) continue;
    segDur[segCount] = dur;
    segBroken[segCount] = curState;
    segCount++;
    curState = fEdgeBroken[i];
  }

  // Final segment to frame end
  if (segCount < 16) {
    uint32_t lastDur = frameEndUs - fEdgeUs[fCount - 1];
    segDur[segCount] = lastDur;
    segBroken[segCount] = curState;
    segCount++;
  }

  // We expect: [B start] [C spacer] [B pulse] [C spacer] [B pulse] [C spacer] [B pulse] [C stop]
  // That is 8 segments alternating, starting broken and ending clear.
  if (segCount < 8) {
    r.debug = "Too few segments";
    return r;
  }

  // Take first 8 segments (most reliable) — extra noise after is ignored
  // Validate alternation pattern
  for (int i = 0; i < 8; i++) {
    bool expectBroken = (i % 2 == 0); // 0,2,4,6 broken
    if (segBroken[i] != expectBroken) {
      r.debug = "Bad alternation @seg " + String(i);
      return r;
    }
  }

  float startDur = (float)segDur[0];
  float sp0 = (float)segDur[1];
  if (sp0 < 1) {
    r.debug = "Spacer too small";
    return r;
  }
  if (startDur / sp0 < START_RATIO_MIN) {
    r.debug = "Start ratio low: " + String(startDur / sp0, 2);
    return r;
  }

  // Decode 3 bits using local pulse/spacer ratio
  uint8_t id = 0;
  for (int bi = 0; bi < 3; bi++) {
    float spacer = (float)segDur[1 + bi * 2];
    float pulse  = (float)segDur[2 + bi * 2];

    // guardrails
    if (spacer < MIN_SEG_US || pulse < MIN_SEG_US) {
      r.debug = "Segment too short for bit " + String(bi);
      return r;
    }

    int bit = (pulse / spacer > ONE_THRESHOLD) ? 1 : 0;
    id = (id << 1) | (uint8_t)bit;
  }

  float spLast = (float)segDur[5]; // spacer before last pulse
  float stopDur = (float)segDur[7];
  if (stopDur / spLast < STOP_RATIO_MIN) {
    r.debug = "Stop ratio low: " + String(stopDur / spLast, 2);
    return r;
  }

  r.bits = id;

  if (id <= 5) {
    r.ok = true;
    r.carNum = (int)id + 1;
  } else {
    r.ok = false;
    r.carNum = -1;
  }

  // Debug string (durations in ms-ish)
  r.debug = "id=" + String(id) +
            " start/sp=" + String(startDur / sp0, 2) +
            " stop/sp=" + String(stopDur / spLast, 2) +
            " seg(us)=";
  for (int i = 0; i < 8; i++) {
    r.debug += String(segDur[i]);
    if (i != 7) r.debug += ",";
  }

  return r;
}

// ==== Lap timing ====
static const int NUM_CARS = 6;
uint32_t lastPassMs[NUM_CARS] = {0};
uint32_t lastLapMs[NUM_CARS] = {0};
uint32_t lapCount[NUM_CARS] = {0};

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(IR_PIN, INPUT_PULLUP);

  // initialize state
  g_lastBroken = readBeamBrokenFast();
  g_lastEdgeUs = micros();

  attachInterrupt(digitalPinToInterrupt(IR_PIN), onBeamEdge, CHANGE);

  Serial.println();
  Serial.println("=== Single-beam barcode lap timer ready ===");
  Serial.println("Expecting START(3U blocked) + 3 bits + STOP(3U clear).");
  Serial.println("Car mapping: 1=000 2=001 3=010 4=011 5=100 6=101");
}

void loop() {
  // Check for frame end
  if (g_capturing) {
    uint32_t nowUs = micros();
    uint32_t silence = nowUs - g_lastEdgeUs;

    // Abort if taking too long (car stopped on sensor, etc.)
    if (nowUs - g_startUs > MAX_FRAME_US) {
      noInterrupts();
      g_capturing = false;
      g_edgeCount = 0;
      interrupts();
      Serial.println("Frame aborted (too long).");
      return;
    }

    // End condition: no edges for a while AND beam is currently clear
    bool beamClear = !readBeamBrokenFast();
    if (beamClear && silence > END_SILENCE_US) {
      // copy buffer atomically
      uint32_t edgeUs[MAX_EDGES];
      bool edgeBroken[MAX_EDGES];
      int edgeCount;

      noInterrupts();
      edgeCount = g_edgeCount;
      if (edgeCount > MAX_EDGES) edgeCount = MAX_EDGES;
      for (int i = 0; i < edgeCount; i++) {
        edgeUs[i] = g_edgeUs[i];
        edgeBroken[i] = g_edgeBroken[i];
      }
      g_capturing = false;
      g_edgeCount = 0;
      interrupts();

      DecodeResult res = decodeFrame(edgeUs, edgeBroken, edgeCount, nowUs);

      if (res.ok) {
        int idx = res.carNum - 1;
        uint32_t nowMs = millis();
        if (lastPassMs[idx] != 0) {
          lastLapMs[idx] = nowMs - lastPassMs[idx];
          lapCount[idx]++;
          Serial.printf("PASS Car %d  lap=%lu  lapTime=%lu ms   (%s)\n",
                        res.carNum, (unsigned long)lapCount[idx],
                        (unsigned long)lastLapMs[idx],
                        res.debug.c_str());
        } else {
          Serial.printf("PASS Car %d  (first seen)   (%s)\n", res.carNum, res.debug.c_str());
        }
        lastPassMs[idx] = nowMs;
      } else {
        Serial.printf("BARCODE UNKNOWN / BAD  (%s)\n", res.debug.c_str());
      }
    }
  }
}
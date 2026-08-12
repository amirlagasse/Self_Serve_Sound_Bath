// ===========================================================================
// TUB_MASTER.INO
// ---------------------------------------------------------------------------
// Teensy 4.1 master, phase 2: real inputs.
//
// This board is a MIDI controller that also relays to a lighting
// controller:
//   1. Occupancy: VL53L1X time-of-flight sensor over I2C. Distance under
//      PRESENCE_MM held for ENTER_HOLD_MS flips IDLE -> ACTIVE; distance
//      over it held for EXIT_HOLD_MS flips back. All debounce lives here:
//      by the time the Uno hears about it, the decision is final.
//   2. Encoders: 16 rotary encoders (14 detented at 20 detents/rev, 2
//      detentless, all EC11). In IDLE their input is discarded. In ACTIVE
//      each knob's position drives two outputs at once:
//        - USB MIDI control changes (0-127) to the computer
//        - position deltas (0-255 scale) to the Uno over UART
//   3. UART transmitter to the Uno: contract is docs/PROTOCOL.md.
//
// KNOB MODEL
// The Teensy holds the authoritative position of each knob in detent
// steps, clamped to that knob's configured travel (knobCfg, below: e.g.
// 80 steps = 4 full rotations of a 20-detent knob for full range). MIDI
// and light values are both DERIVED from that one position, so they stay
// locked together and hit their endpoints on the same physical rotation
// regardless of the range setting. The Uno receives deltas of the derived
// light value; its own accumulation then tracks ours exactly.
//
// Per-knob defaults are also in knobCfg. They must match the Uno's
// resetEncoderPositions() table (defaults x2), since both sides snap to
// their own defaults on every mode change.
//
// Wiring: Teensy TX1 (pin 1) -> Uno RX (pin 0), grounds bonded, one-way.
// ToF on I2C0: SDA=18, SCL=19. Encoder pin map below, PROVISIONAL.
//
// Build with USB Type "Serial + MIDI" (fqbn option usb=serialmidi). The
// MIDI code is guarded so plain Serial builds still compile, minus MIDI.
//
// This board has 1MB of RAM. The Uno-side memory rules do not apply here.
//
// USB Serial commands:
//   b  burst test: hammer all 16 encoders for 2 seconds
//   m  flip mode manually (works with or without the ToF attached)
//   t  print ToF status and latest distance
//   k  print all knob positions (steps, MIDI, light)
//   v  toggle per-packet hex logging
//   h  help
//   e  <id byte> <signed delta byte>: inject simulated knob ticks from the
//      digital twin. Goes through the same knob model as a real encoder
//      (MIDI + UART stay locked), in light units (0-255 over full travel).
// ===========================================================================

#include <Wire.h>
#include <Encoder.h>
#include <VL53L1X.h>

// Phase 1 simulators, kept for benchless testing. Leave off when real
// inputs are wired. Note the sims push raw ticks and bypass the knob
// position model, so light positions drift from MIDI while simming: fine
// for a bench demo, meaningless for the real system.
#define SIM_INPUTS 0

#if defined(USB_MIDI) || defined(USB_MIDI_SERIAL) || defined(USB_MIDI16) || defined(USB_MIDI16_SERIAL)
  #define HAS_USB_MIDI 1
#else
  #define HAS_USB_MIDI 0
#endif

// --- Protocol constants ----------------------------------------------------

const uint8_t START_BYTE  = 0xAA;
const uint8_t MSG_MODE    = 0x01;
const uint8_t MSG_ENCODER = 0x02;

const uint8_t MODE_IDLE   = 0x00;
const uint8_t MODE_ACTIVE = 0x01;

const uint8_t N_ENCODERS  = 16;

// The Uno drops UART bytes while it writes LED strips (interrupts off),
// so each mode change is sent MODE_REPEATS times; the Uno ignores
// duplicates. See docs/PROTOCOL.md and OPEN_QUESTIONS 10.
const uint8_t  MODE_REPEATS           = 3;
const uint16_t MODE_REPEAT_SPACING_MS = 60;

// Batch window per the contract: one message per moved encoder per window.
const uint16_t BATCH_MS = 25;

// --- Occupancy sensing -----------------------------------------------------
// PROVISIONAL threshold and holds. The spoken spec gave both "5 seconds"
// and "7 seconds" for the enter hold in the same sentence; 5s is used
// here. Exit hold matching at 5s is an assumption. Tune once the tub is
// real. See docs/OPEN_QUESTIONS.md item 12.

const uint16_t      PRESENCE_MM   = 900;
// Asymmetric on purpose: wake fast, sleep reluctantly. The sensor throws
// occasional long reads while someone is sitting still, and a symmetric
// short exit hold let those flip the tub to IDLE mid-soak. Any "present"
// blip during the exit countdown resets it.
const unsigned long ENTER_HOLD_MS = 3000;
// 5s exit per Amir 2026-08-11: 10s felt too long. Still safe against the
// sensor's occasional long reads because any present blip resets the
// countdown; only 5 full seconds of continuous absence exits.
const unsigned long EXIT_HOLD_MS  = 5000;
const uint32_t      TOF_PERIOD_MS = 50;

VL53L1X tof;
bool tofOk = false;
uint16_t lastDistanceMm = 0;
bool lastReadValid = false;

bool presentNow = false;
unsigned long streakStartMs = 0;

// --- Encoder hardware ------------------------------------------------------
// PROVISIONAL pin map: nothing is characterized yet. A/B pairs, avoiding
// 0/1 (Serial1 to the Uno) and 18/19 (I2C0 for the ToF). All Teensy 4.1
// pins are interrupt-capable, so any reassignment is fine.
// A/B order per knob sets spin direction; swapped pairs are knobs that
// counted backwards on hardware (enc 0 FL, 1 FR, 2 FC, 3 RBL, 6 RUL,
// 7 LUL, 12 JUR, 14 JBR, 15 JC).
// Enc 5 (RUR) moved off pin 13: the onboard LED loads that pin and holds
// it low, killing one quadrature channel (knob counted up-only). Its
// second wire lives on pin 36 instead. Never put an encoder on pin 13.
const uint8_t ENC_PINS[N_ENCODERS][2] = {
  { 3,  2}, { 5,  4}, { 7,  6}, { 9,  8},   // enc 0-3, FL FR FC RBL flipped
  {10, 11}, {36, 12}, {15, 14}, {17, 16},   // enc 4-7, RUR + RUL + LUL flipped
  {20, 21}, {22, 23}, {24, 25}, {26, 27},   // enc 8-11
  {29, 28}, {30, 31}, {33, 32}, {34, 35},   // enc 12-15, JUR + JBR flipped
};

// EC11: 4 quadrature counts per detent. The 2 detentless encoders (which
// two is not yet known, see OPEN_QUESTIONS 12) use the same divisor for
// now so one "step" feels comparable across all knobs.
const int8_t COUNTS_PER_STEP = 4;

Encoder *encoders[N_ENCODERS];

// --- Per-knob configuration ------------------------------------------------
// stepsPerRange: how many detent-steps of turning cover the full 0-127
// MIDI travel. 20-detent EC11, so 80 = 4 full rotations end to end.
// Tune per knob: a fine-control knob wants a bigger number, a fast toggle
// wants a smaller one.
// midiDefault: where the knob rests after boot and after every mode
// change. MUST match the Uno's resetEncoderPositions() table (this x2).
// Names and defaults from Nile, docs/OPEN_QUESTIONS.md item 11.

struct KnobConfig {
  uint16_t stepsPerRange;
  uint8_t  midiDefault;
};

// stepsPerRange 16: full travel in under one rotation of a 20-detent
// knob. Was 80 (4 rotations), dropped 5x on request: felt way too slow.
// Full mapping confirmed with Nile 2026-08-11. Hot and cold racks are
// mirror images: same param under the same hand position on each side.
// JC is percussion master volume, the old "timer" idea is dropped.
KnobConfig knobCfg[N_ENCODERS] = {
  { 16, 64 },   // 0  FL cold blend (volume of cold pads, tracks 3-4)
  { 16, 64 },   // 1  FR hot blend (volume of hot pads, tracks 1-2)
  { 16, 64 },   // 2  FC filter, left LP / right HP, 64 = open (center detent)
  { 16, 0  },   // 3  RBL reverb, hot (Valhalla)
  { 16, 0  },   // 4  RBR bitcrush, hot (Logic Bitcrusher)
  { 16, 64 },   // 5  RUR pitch, hot (AU Pitch)
  { 16, 0  },   // 6  RUL mod x, hot (Alchemy)
  { 16, 64 },   // 7  LUL pitch, cold (AU Pitch)
  { 16, 0  },   // 8  LUR mod x, cold (Alchemy)
  { 16, 0  },   // 9  LBR reverb, cold (Valhalla)
  { 16, 0  },   // 10 LBL bitcrush, cold (Logic Bitcrusher)
  { 16, 64 },   // 11 JUL percussion pitch (AU Pitch)
  { 16, 64 },   // 12 JUR percussion rate/speed
  { 16, 64 },   // 13 JBL percussion transients (Chimera)
  { 16, 0  },   // 14 JBR percussion flanger
  { 16, 0  },   // 15 JC percussion volume, silent until turned up
};

// Authoritative knob positions in steps, plus the last derived values so
// only changes are transmitted.
int32_t stepPos[N_ENCODERS];
uint8_t midiLast[N_ENCODERS];
uint8_t lightLast[N_ENCODERS];
bool midiDirty[N_ENCODERS];

// Fractional carry for twin-injected ticks: the twin speaks in light units
// (0-255 over full travel) but the knob model is in detent steps, so each
// injected tick accumulates light*range here until a whole step is owed.
int32_t simTickAccum[N_ENCODERS];

// --- MIDI ------------------------------------------------------------------
// PROVISIONAL: CC 16-31 on channel 1, CC number = CC_BASE + encoder id.
// On entering ACTIVE a full snapshot of defaults is sent so the sound
// engine starts from the same picture (assumption, confirm with Nile).

const uint8_t MIDI_CHANNEL = 1;
const uint8_t CC_BASE      = 16;

// --- Transmitter state -----------------------------------------------------

int16_t pendingTicks[N_ENCODERS];
unsigned long lastBatchMs = 0;

uint8_t currentMode = MODE_IDLE;
uint8_t modeRepeatsLeft = 0;
unsigned long nextModeRepeatMs = 0;

bool logPackets = true;
uint32_t txPacketTotal = 0;
uint32_t ppsWindowCount = 0;
unsigned long lastPpsMs = 0;

// --- Simulated inputs (phase 1, off by default) ----------------------------

#if SIM_INPUTS
unsigned long lastFlipMs = 0;
const unsigned long FLIP_INTERVAL_MS = 15000;
bool jitterOn = true;
unsigned long lastSimMs = 0;
bool sweepOn = true;
const uint8_t SWEEP_ID = 5;
int16_t sweepPos = 128;
int8_t sweepDir = 1;
#endif

bool burstOn = false;
unsigned long burstEndMs = 0;
uint8_t burstNextId = 0;
uint32_t burstPacketCount = 0;
unsigned long burstStartMs = 0;

// ===========================================================================
// KNOB MODEL: derive MIDI and light values from step position
// ===========================================================================

uint8_t deriveMidi(uint8_t id) {
  uint32_t range = knobCfg[id].stepsPerRange;
  return (uint8_t)(((uint32_t)stepPos[id] * 127 + range / 2) / range);
}

uint8_t deriveLight(uint8_t id) {
  uint32_t range = knobCfg[id].stepsPerRange;
  return (uint8_t)(((uint32_t)stepPos[id] * 255 + range / 2) / range);
}

void resetKnobPositions() {
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    uint32_t range = knobCfg[i].stepsPerRange;
    stepPos[i] = (int32_t)(((uint32_t)knobCfg[i].midiDefault * range + 63) / 127);
    midiLast[i] = deriveMidi(i);
    lightLast[i] = deriveLight(i);
    midiDirty[i] = false;
    pendingTicks[i] = 0;
    simTickAccum[i] = 0;
    if (encoders[i]) encoders[i]->write(0);
  }
}

// ===========================================================================
// PACKET BUILDERS
// Checksum: XOR of every byte after the start byte, through end of payload.
// Known-good case: encoder 5, delta -3 -> aa 02 05 fd fa
// ===========================================================================

void buildModePacket(uint8_t *out, uint8_t mode) {
  out[0] = START_BYTE;
  out[1] = MSG_MODE;
  out[2] = mode;
  out[3] = MSG_MODE ^ mode;
}

void buildEncoderPacket(uint8_t *out, uint8_t id, int8_t delta) {
  uint8_t d = (uint8_t)delta;            // two's complement byte
  out[0] = START_BYTE;
  out[1] = MSG_ENCODER;
  out[2] = id;
  out[3] = d;
  out[4] = MSG_ENCODER ^ id ^ d;
}

void txPacket(const uint8_t *pkt, uint8_t len, bool logIt) {
  Serial1.write(pkt, len);
  txPacketTotal++;
  ppsWindowCount++;
  if (logIt && logPackets) {
    Serial.print("tx ");
    for (uint8_t i = 0; i < len; i++) {
      Serial.printf("%02x%s", pkt[i], i + 1 < len ? " " : "\n");
    }
  }
}

void sendMode(uint8_t mode) {
  uint8_t pkt[4];
  buildModePacket(pkt, mode);
  txPacket(pkt, 4, true);
}

void sendEncoderDelta(uint8_t id, int8_t delta) {
  if (delta == 0) return;
  uint8_t pkt[5];
  buildEncoderPacket(pkt, id, delta);
  txPacket(pkt, 5, true);
}

void sendEncoderDeltaQuiet(uint8_t id, int8_t delta) {
  if (delta == 0) return;
  uint8_t pkt[5];
  buildEncoderPacket(pkt, id, delta);
  txPacket(pkt, 5, false);
}

// ===========================================================================
// MIDI OUT
// ===========================================================================

void midiSendCc(uint8_t id) {
#if HAS_USB_MIDI
  usbMIDI.sendControlChange(CC_BASE + id, midiLast[id], MIDI_CHANNEL);
#endif
}

void midiSendSnapshot() {
#if HAS_USB_MIDI
  for (uint8_t i = 0; i < N_ENCODERS; i++) midiSendCc(i);
  Serial.println("midi: snapshot of defaults sent");
#else
  Serial.println("midi: NOT BUILT IN. Set USB Type to Serial + MIDI (usb=serialmidi).");
#endif
}

// ===========================================================================
// MODE HANDLING
// ===========================================================================

void changeMode(uint8_t mode) {
  currentMode = mode;

  Serial.printf("\n==== MODE -> %s ====\n\n", mode == MODE_ACTIVE ? "ACTIVE" : "IDLE");
  sendMode(mode);
  modeRepeatsLeft = (MODE_REPEATS > 0) ? (uint8_t)(MODE_REPEATS - 1) : 0;
  nextModeRepeatMs = millis() + MODE_REPEAT_SPACING_MS;

  // Both sides snap to their defaults on a mode change (docs/SPEC.md
  // section 5): the Uno resets its positions on the message, and every
  // knob position resets here. Stale ticks must not leak across.
  resetKnobPositions();
  // Snapshot on EVERY transition: the spec requires each CC transmitted
  // once at its default on the way back to IDLE, and ACTIVE starts from
  // the same defaults, so both directions send the same picture.
  midiSendSnapshot();

#if SIM_INPUTS
  sweepPos = 128;
  sweepDir = 1;
#endif
}

void serviceModeRepeats() {
  if (modeRepeatsLeft == 0) return;
  if ((long)(millis() - nextModeRepeatMs) < 0) return;
  modeRepeatsLeft--;
  nextModeRepeatMs = millis() + MODE_REPEAT_SPACING_MS;
  sendMode(currentMode);   // duplicate: the Uno ignores it if the first landed
}

// Direct mode set from the host: the digital twin acts as the occupancy
// sensor for now (ToF shelved). A same-mode command re-sends the packet
// burst without resetting knob positions, since the Uno ignores
// duplicates: useful to nudge an Uno that rebooted mid-session.
void applyModeCommand(uint8_t mode) {
  if (mode == currentMode) {
    modeRepeatsLeft = MODE_REPEATS;
    nextModeRepeatMs = millis();
    Serial.printf("mode command: %s again, re-sending burst\n",
                  mode == MODE_ACTIVE ? "ACTIVE" : "IDLE");
  } else {
    changeMode(mode);
  }
}

// ===========================================================================
// OCCUPANCY: ToF with hold-time debounce
// ===========================================================================

void serviceOccupancy() {
  if (!tofOk) return;
  if (!tof.dataReady()) return;

  uint16_t d = tof.read(false);
  lastReadValid = (tof.ranging_data.range_status == VL53L1X::RangeValid);
  if (!lastReadValid) return;   // ignore bad reads, do not advance streaks
  lastDistanceMm = d;

  bool present = d < PRESENCE_MM;
  unsigned long now = millis();

  if (present != presentNow) {
    presentNow = present;
    streakStartMs = now;
    Serial.printf("tof: %s streak begins (%u mm)\n", present ? "PRESENT" : "ABSENT", d);
    return;
  }

  unsigned long streak = now - streakStartMs;
  if (currentMode == MODE_IDLE && present && streak >= ENTER_HOLD_MS) {
    Serial.printf("tof: present %lums, entering ACTIVE\n", streak);
    changeMode(MODE_ACTIVE);
    streakStartMs = now;
  } else if (currentMode == MODE_ACTIVE && !present && streak >= EXIT_HOLD_MS) {
    Serial.printf("tof: absent %lums, entering IDLE\n", streak);
    changeMode(MODE_IDLE);
    streakStartMs = now;
  }
}

// ===========================================================================
// ENCODERS
// In IDLE the hardware counts are read and thrown away. In ACTIVE whole
// detent-steps move the authoritative position, and both outputs are
// derived from it: MIDI CC when the 0-127 value changes, UART deltas when
// the 0-255 light value changes.
// ===========================================================================

void serviceEncoders() {
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    if (!encoders[i]) continue;
    long counts = encoders[i]->read();
    long steps = counts / COUNTS_PER_STEP;

    if (currentMode != MODE_ACTIVE) {
      if (counts != 0) encoders[i]->write(0);   // idle: discard
      continue;
    }

    if (steps == 0) continue;

    // Keep the sub-step remainder so slow turns never lose motion.
    encoders[i]->write(counts - steps * COUNTS_PER_STEP);

    int32_t range = knobCfg[i].stepsPerRange;
    int32_t p = stepPos[i] + steps;
    if (p < 0) p = 0;
    if (p > range) p = range;

    // Ticks past an end still go on the wire. The Uno clamps to the same
    // end, so overshoot can never move it wrongly, but it drags a desynced
    // Uno position (deltas get lost while the Uno writes LED strips) back
    // to the rail. Without this a pegged knob goes silent and the Uno can
    // sit stuck at e.g. 23 forever while the user keeps spinning toward 0.
    int32_t overshoot = (stepPos[i] + steps) - p;
    if (overshoot != 0) {
      int32_t lightOver = overshoot * 255 / range;
      if (lightOver == 0) lightOver = (overshoot > 0) ? 1 : -1;
      pendingTicks[i] += (int16_t)lightOver;
    }

    if (p == stepPos[i]) continue;   // pegged, only overshoot to report
    stepPos[i] = p;

    uint8_t midiNow = deriveMidi(i);
    if (midiNow != midiLast[i]) {
      midiLast[i] = midiNow;
      midiDirty[i] = true;
    }

    uint8_t lightNow = deriveLight(i);
    if (lightNow != lightLast[i]) {
      pendingTicks[i] += (int16_t)lightNow - (int16_t)lightLast[i];
      lightLast[i] = lightNow;
    }
  }
}

// Twin-injected knob ticks ('e' USB command). Same rules as a real
// encoder: discarded in IDLE, moves the authoritative step position, and
// both MIDI and UART outputs are derived from it so nothing drifts. delta
// is in light units; whole steps are peeled off the light*range carry.
void injectSimTicks(uint8_t id, int8_t delta) {
  if (id >= N_ENCODERS) return;
  if (currentMode != MODE_ACTIVE) return;

  int32_t range = knobCfg[id].stepsPerRange;
  simTickAccum[id] += (int32_t)delta * range;
  int32_t steps = simTickAccum[id] / 255;
  if (steps == 0) return;
  simTickAccum[id] -= steps * 255;

  int32_t p = stepPos[id] + steps;
  if (p < 0) p = 0;
  if (p > range) p = range;

  // Same end-rail healing as serviceEncoders: overshoot still transmits.
  int32_t overshoot = (stepPos[id] + steps) - p;
  if (overshoot != 0) {
    int32_t lightOver = overshoot * 255 / range;
    if (lightOver == 0) lightOver = (overshoot > 0) ? 1 : -1;
    pendingTicks[id] += (int16_t)lightOver;
  }

  if (p == stepPos[id]) return;
  stepPos[id] = p;

  uint8_t midiNow = deriveMidi(id);
  if (midiNow != midiLast[id]) {
    midiLast[id] = midiNow;
    midiDirty[id] = true;
  }

  uint8_t lightNow = deriveLight(id);
  if (lightNow != lightLast[id]) {
    pendingTicks[id] += (int16_t)lightNow - (int16_t)lightLast[id];
    lightLast[id] = lightNow;
  }
}

// ===========================================================================
// BATCH FLUSH: UART deltas and MIDI CCs, one per moved encoder per window
// ===========================================================================

void serviceBatchFlush() {
  unsigned long now = millis();
  if (now - lastBatchMs < BATCH_MS) return;
  lastBatchMs = now;

  for (uint8_t id = 0; id < N_ENCODERS; id++) {
    int16_t t = pendingTicks[id];
    if (t != 0) {
      int16_t step = t;
      if (step > 127) step = 127;
      if (step < -128) step = -128;
      sendEncoderDelta(id, (int8_t)step);
      pendingTicks[id] = (int16_t)(t - step);
    }
    if (midiDirty[id]) {
      midiDirty[id] = false;
      midiSendCc(id);
    }
  }
}

// ===========================================================================
// SIMULATED INPUTS (phase 1, compiled out by default)
// ===========================================================================

#if SIM_INPUTS
void serviceSimOccupancy() {
  unsigned long now = millis();
  if (now - lastFlipMs < FLIP_INTERVAL_MS) return;
  lastFlipMs = now;
  changeMode(currentMode == MODE_ACTIVE ? MODE_IDLE : MODE_ACTIVE);
}

void serviceSimEncoders() {
  unsigned long now = millis();
  if (now - lastSimMs < BATCH_MS) return;
  lastSimMs = now;
  if (currentMode != MODE_ACTIVE) return;

  if (jitterOn) {
    for (uint8_t n = 0; n < 2; n++) {
      uint8_t id = (uint8_t)random(0, N_ENCODERS);
      pendingTicks[id] += (random(0, 2) == 0) ? 1 : -1;
    }
  }
  if (sweepOn) {
    pendingTicks[SWEEP_ID] += sweepDir;
    sweepPos += sweepDir;
    if (sweepPos >= 255) { sweepPos = 255; sweepDir = -1; }
    if (sweepPos <= 0)   { sweepPos = 0;   sweepDir = 1;  }
  }
}
#endif

// Burst pushes raw deltas straight onto the wire to stress the Uno's
// parser. It bypasses the knob model, so light positions are garbage
// until the next mode change resyncs everything. That is fine: it is a
// parser stress test, not a musical instrument.
void startBurst() {
  burstOn = true;
  burstStartMs = millis();
  burstEndMs = burstStartMs + 2000;
  burstPacketCount = 0;
  burstNextId = 0;
  Serial.println("BURST: hammering all 16 encoders for 2s (per-packet log suppressed)");
}

void serviceBurst() {
  if (!burstOn) return;

  if ((long)(millis() - burstEndMs) >= 0) {
    burstOn = false;
    unsigned long dur = millis() - burstStartMs;
    Serial.printf("BURST done: %lu packets in %lu ms (%.0f pkt/s)\n",
                  (unsigned long)burstPacketCount, dur,
                  dur ? burstPacketCount * 1000.0f / dur : 0.0f);
    return;
  }

  uint8_t sentThisPass = 0;
  while (Serial1.availableForWrite() >= 5 && sentThisPass < 64) {
    int8_t delta = (int8_t)((random(0, 2) == 0) ? random(1, 4) : -random(1, 4));
    sendEncoderDeltaQuiet(burstNextId, delta);
    burstNextId = (uint8_t)((burstNextId + 1) % N_ENCODERS);
    burstPacketCount++;
    sentThisPass++;
  }
}

// ===========================================================================
// USB SERIAL COMMANDS AND STATS
// ===========================================================================

void printHelp() {
  Serial.println("commands: 0=set IDLE, 1=set ACTIVE, m=flip mode, b=burst 2s, t=tof status, k=knob positions, v=toggle packet log, e<id><delta>=inject twin knob ticks, h=help");
}

void printKnobs() {
  Serial.println("id  steps/range  midi  light");
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    Serial.printf("%2u  %4ld/%-4u    %3u   %3u\n",
                  i, (long)stepPos[i], knobCfg[i].stepsPerRange,
                  midiLast[i], lightLast[i]);
  }
}

void handleUsbCommands() {
  // 'e' carries two payload bytes that may arrive in a later USB chunk, so
  // collection spans calls: 0 = command mode, 1 = awaiting id, 2 = awaiting
  // delta.
  static uint8_t injState = 0;
  static uint8_t injId = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (injState == 1) { injId = (uint8_t)c; injState = 2; continue; }
    if (injState == 2) { injectSimTicks(injId, (int8_t)c); injState = 0; continue; }

    switch (c) {
      case 'e': injState = 1; break;
      case 'b': startBurst(); break;
      case 'm': changeMode(currentMode == MODE_ACTIVE ? MODE_IDLE : MODE_ACTIVE); break;
      case '0': applyModeCommand(MODE_IDLE); break;
      case '1': applyModeCommand(MODE_ACTIVE); break;
      case 't':
        if (tofOk) {
          Serial.printf("tof: %u mm, %s, %s streak %lums\n",
                        lastDistanceMm, lastReadValid ? "valid" : "invalid",
                        presentNow ? "present" : "absent",
                        millis() - streakStartMs);
        } else {
          Serial.println("tof: sensor not initialized");
        }
        break;
      case 'k': printKnobs(); break;
      case 'v': logPackets = !logPackets;
                Serial.printf("packet log: %s\n", logPackets ? "on" : "off");
                break;
      case 'h': case '?': printHelp(); break;
      default: break;
    }
  }
}

// Machine-readable sensor telemetry for the digital twin, 10 Hz:
// $tof <mm|-1> <valid> <present> <streakMs> <mode> <enterHoldMs> <exitHoldMs>
// The twin draws the live distance and the countdown-to-switch bar from
// this. Human-readable logs stay as they are; the twin keys on "$tof".
unsigned long lastTofTelemMs = 0;
const unsigned long TOF_TELEM_MS = 100;

void serviceTofTelemetry() {
  unsigned long now = millis();
  if (now - lastTofTelemMs < TOF_TELEM_MS) return;
  lastTofTelemMs = now;
  Serial.printf("$tof %ld %d %d %lu %d %lu %lu\n",
                tofOk ? (long)lastDistanceMm : -1L,
                lastReadValid ? 1 : 0,
                presentNow ? 1 : 0,
                tofOk ? (now - streakStartMs) : 0UL,
                currentMode == MODE_ACTIVE ? 1 : 0,
                ENTER_HOLD_MS, EXIT_HOLD_MS);
}

void servicePpsLog() {
  unsigned long now = millis();
  if (now - lastPpsMs < 1000) return;
  lastPpsMs = now;
  Serial.printf("throughput: %lu pkt/s, %lu total | mode %s | tof %s\n",
                (unsigned long)ppsWindowCount, (unsigned long)txPacketTotal,
                currentMode == MODE_ACTIVE ? "ACTIVE" : "IDLE",
                tofOk ? (presentNow ? "present" : "absent") : "OFFLINE");
  ppsWindowCount = 0;
}

// ===========================================================================
// SELF-TEST
// ===========================================================================

bool checksumSelfTest() {
  const uint8_t expect[5] = { 0xAA, 0x02, 0x05, 0xFD, 0xFA };
  uint8_t pkt[5];
  buildEncoderPacket(pkt, 5, (int8_t)-3);

  bool ok = memcmp(pkt, expect, 5) == 0;
  Serial.print("checksum self-test (enc 5, delta -3): ");
  for (uint8_t i = 0; i < 5; i++) Serial.printf("%02x ", pkt[i]);
  Serial.println(ok ? "PASS" : "FAIL <- packets are wrong, fix before trusting anything");
  return ok;
}

// ===========================================================================
// SETUP / LOOP
// ===========================================================================

void setup() {
  Serial.begin(115200);               // USB debug; baud is ignored on Teensy
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }

  Serial1.begin(115200);              // the wire to the Uno

  randomSeed(micros());

  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    encoders[i] = new Encoder(ENC_PINS[i][0], ENC_PINS[i][1]);
  }
  resetKnobPositions();
  midiSendSnapshot();   // spec: every CC transmitted once at default on boot

  Wire.begin();
  Wire.setClock(400000);
  tof.setTimeout(500);
  if (tof.init()) {
    tofOk = true;
    tof.setDistanceMode(VL53L1X::Short);        // good to ~1.3m, covers the 900mm threshold
    tof.setMeasurementTimingBudget(33000);
    tof.startContinuous(TOF_PERIOD_MS);
    Serial.println("tof: VL53L1X initialized, short range mode, 50ms period");
  } else {
    Serial.println("tof: INIT FAILED, occupancy disabled. Use 'm' to flip mode manually.");
  }

  Serial.println();
  Serial.println("tub_master: Teensy 4.1, phase 2: ToF occupancy + encoders + USB MIDI");
#if !HAS_USB_MIDI
  Serial.println("WARNING: built without USB MIDI. Set USB Type to Serial + MIDI.");
#endif
#if SIM_INPUTS
  Serial.println("SIM_INPUTS on: simulators running, do NOT combine with real inputs");
#endif
  checksumSelfTest();
  printHelp();
  Serial.printf("occupancy: <%umm for %lums enters ACTIVE, >=%umm for %lums exits\n",
                PRESENCE_MM, ENTER_HOLD_MS, PRESENCE_MM, EXIT_HOLD_MS);
}

void loop() {
  handleUsbCommands();
  serviceModeRepeats();

#if SIM_INPUTS
  serviceSimOccupancy();
  serviceSimEncoders();
#else
  serviceOccupancy();
  serviceEncoders();
#endif

  serviceBurst();
  serviceBatchFlush();
  serviceTofTelemetry();
  servicePpsLog();

#if HAS_USB_MIDI
  while (usbMIDI.read()) { }   // drain anything incoming, we only transmit
#endif
}

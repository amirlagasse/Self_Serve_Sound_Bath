// ===========================================================================
// TUB.INO
// ---------------------------------------------------------------------------
// Arduino wrapper around the pure render functions in render_core.h.
// UART parser, state machine, FastLED driver. Does not modify render_core.h.
//
// One CRGB array exists in this whole program: `leds`. See docs/MEMORY.md
// before changing anything here.
// ===========================================================================

#include <FastLED.h>
#include "render_core.h"

// Set to 0 for the installed build. Framebuffer dumps cost wire time and are
// only useful for the browser twin.
#define TWIN_DEBUG 1

// --- Strip data pins ---------------------------------------------------
// PROVISIONAL. From docs/OPEN_QUESTIONS.md item 3, owner Amir.
// Four independent runs, not three: the panel used to be one chain covering
// faucet + both aux racks + jets on a single pin, so any wiring problem
// anywhere in that chain could take out every rack downstream of it. Jets
// now gets its own pin instead of daisy-chaining off the front panel.
#define PIN_UNDER   6
#define PIN_POLE    7
#define PIN_FRONT   8   // faucet + aux left + aux right, one contiguous run
#define PIN_JETS    9   // jets rack, isolated on its own pin

#define PIN_STATUS 13

#define MODE_IDLE   0
#define MODE_ACTIVE 1

// --- The one framebuffer ------------------------------------------------
CRGB leds[N_TOTAL];

// --- Top-level state machine --------------------------------------------
enum TopState : uint8_t { ST_LAMP_TEST, ST_WAITING, ST_RUNNING };
TopState state = ST_LAMP_TEST;

unsigned long lampTestStartMs = 0;

// --- Mode / transition state --------------------------------------------
uint8_t modeFrom = MODE_IDLE;
uint8_t modeTo   = MODE_IDLE;
bool transitioning = false;
unsigned long transitionStartMs = 0;

uint8_t encPos[N_ENCODERS];
bool jetsAwake = false;

// Active-turn feedback: 255 right after a knob tick, stepped down each
// frame. The renderer doubles this value with a saturating add, so the
// glow holds at full strength while heat is above 128 (~425ms after the
// last tick) and then fades over the remaining ~425ms: bright the whole
// time a knob is moving, a short linger, then it settles.
uint8_t turnHeat[N_ENCODERS];
const uint8_t HEAT_DECAY_PER_FRAME = 10;

// --- Frame gate -----------------------------------------------------------
const unsigned long FRAME_INTERVAL_MS = 1000UL / TARGET_FPS;
unsigned long lastFrameMs = 0;

// --- UART parser ------------------------------------------------------
enum ParseState : uint8_t { P_SYNC, P_TYPE, P_PAYLOAD, P_CHECKSUM };
ParseState parseState = P_SYNC;
uint8_t msgType;
uint8_t payloadLen;
uint8_t payloadBuf[2];   // largest payload is the 2-byte encoder delta
uint8_t payloadIdx;
uint8_t runningChecksum;

// --- Status LED: non-latching error blink --------------------------------
bool errorBlinkActive = false;
unsigned long errorBlinkStartMs = 0;
const uint16_t ERROR_SEG_MS = 80;
const uint8_t ERROR_SEGMENTS = 6;   // 3 blinks: on,off,on,off,on,off

#if TWIN_DEBUG
unsigned long lastDumpMs = 0;
const unsigned long DUMP_INTERVAL_MS = 250UL;   // 4 Hz
uint16_t dumpPos = 0;        // next payload byte index into leds[]
uint8_t  dumpChecksum = 0;
bool     dumpActive = false;

unsigned long lastStatusMs = 0;
const unsigned long STATUS_INTERVAL_MS = 100UL; // 10 Hz
#endif

// Default positions from the sound side (Nile), in rotation units: MIDI
// defaults are out of 127 and rotation is 0-255, an exact 2x. Faucet
// cold/hot/filter and both PITCH knobs rest at center 128; Transform X
// and everything unnamed rests at 0. Must match defaultKnobValue() in
// twin/index.html. Used on boot and on every mode-change reset.
void resetEncoderPositions() {
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    switch (i) {
      case ENC_FAUCET_COLD:      // MIDI 64
      case ENC_FAUCET_HOT:       // MIDI 64
      case ENC_FAUCET_FILTER:    // MIDI 64, center detent
      case ENC_PITCH_HOT:        // MIDI 64
      case ENC_PITCH_COLD:       // MIDI 64
      case ENC_JETS_PITCH:       // MIDI 64
      case ENC_JETS_RATE:        // MIDI 64
      case ENC_JETS_TRANSIENT:   // MIDI 64
        encPos[i] = 128;
        break;
      default:
        encPos[i] = 0;
        break;
    }
    turnHeat[i] = 0;
  }
}

// ===========================================================================
// SETUP
// ===========================================================================

void setup() {
  Serial.begin(115200);
  pinMode(PIN_STATUS, OUTPUT);

  FastLED.addLeds<WS2812B, PIN_UNDER, GRB>(leds + OFF_UNDER, N_UNDER);
  FastLED.addLeds<WS2812B, PIN_POLE,  GRB>(leds + OFF_POLE,  N_POLE);
  // Front panel (faucet + aux left + aux right) and jets are contiguous in
  // the shared framebuffer, in that order, so this is a straight split with
  // no reindexing: render_core.h is unchanged.
  FastLED.addLeds<WS2812B, PIN_FRONT, GRB>(leds + OFF_PANEL, PANEL_JETS_START);
  FastLED.addLeds<WS2812B, PIN_JETS,  GRB>(leds + OFF_PANEL + PANEL_JETS_START, PANEL_JETS_LEN);

  resetEncoderPositions();

  lampTestStartMs = millis();
  state = ST_LAMP_TEST;
}

// ===========================================================================
// UART PARSER
// Non-blocking. Drains whatever the hardware RX buffer has this loop.
// ===========================================================================

void flagChecksumFailure() {
  errorBlinkActive = true;
  errorBlinkStartMs = millis();
}

// Mode to enter the moment the lamp test finishes: IDLE unless a mode
// message arrives during the test.
uint8_t bootMode = MODE_IDLE;

void handleModeChange(uint8_t mode) {
  if (state == ST_LAMP_TEST) {
    // Never interrupt the boot lamp test. Remember the request and apply
    // it when the test completes.
    bootMode = mode;
    return;
  }

  // Ignore duplicates of the mode we are already in or already fading
  // toward. Serial bytes arriving during FastLED.show() are lost because
  // interrupts are off, so a sender may repeat the mode message to get it
  // through; without this guard each repeat would restart the crossfade
  // and re-zero the encoders.
  if (state == ST_RUNNING && mode == modeTo) return;

  resetEncoderPositions();
  jetsAwake = false;

  if (state != ST_RUNNING) {
    // Unreachable in the current flow (the lamp test hands straight into
    // ST_RUNNING), kept as a safety net.
    modeFrom = mode;
    modeTo   = mode;
    transitioning = false;
    state = ST_RUNNING;
  } else {
    modeFrom = modeTo;
    modeTo   = mode;
    transitioning = true;
    transitionStartMs = millis();
  }
}

void handleEncoderDelta(uint8_t id, int8_t delta) {
  if (state != ST_RUNNING) return;
  if (transitioning) return;
  if (modeTo == MODE_IDLE) return;
  if (id >= N_ENCODERS) return;

  int16_t v = (int16_t)encPos[id] + delta;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  encPos[id] = (uint8_t)v;

  if (delta != 0) turnHeat[id] = 255;   // knob is actively being turned

  if (id == ENC_JETS_CENTER && delta != 0) jetsAwake = true;
}

void handleMessage(uint8_t type, const uint8_t *payload) {
  if (type == 0x01) {
    handleModeChange(payload[0]);
  } else if (type == 0x02) {
    handleEncoderDelta(payload[0], (int8_t)payload[1]);
  }
}

void pollUart() {
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();

    switch (parseState) {
      case P_SYNC:
        if (b == 0xAA) {
          runningChecksum = 0;
          parseState = P_TYPE;
        }
        break;

      case P_TYPE:
        msgType = b;
        runningChecksum ^= b;
        if (msgType == 0x01) payloadLen = 1;
        else if (msgType == 0x02) payloadLen = 2;
        else { parseState = P_SYNC; break; }   // unknown type, resync
        payloadIdx = 0;
        parseState = P_PAYLOAD;
        break;

      case P_PAYLOAD:
        payloadBuf[payloadIdx++] = b;
        runningChecksum ^= b;
        if (payloadIdx >= payloadLen) parseState = P_CHECKSUM;
        break;

      case P_CHECKSUM:
        if (b == runningChecksum) {
          handleMessage(msgType, payloadBuf);
        } else {
          flagChecksumFailure();
        }
        parseState = P_SYNC;
        break;
    }
  }
}

// ===========================================================================
// RENDERING
// ===========================================================================

void renderRunningFrame() {
  uint16_t t = (uint16_t)millis();
  uint8_t progress = 255;

  if (transitioning) {
    unsigned long elapsed = millis() - transitionStartMs;
    uint32_t p = (elapsed * 255UL) / TRANSITION_MS;
    if (p >= 255) {
      transitioning = false;
      modeFrom = modeTo;
      progress = 255;
    } else {
      progress = (uint8_t)p;
    }
  }

  // Step the turn-feedback glow down once per rendered frame.
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    turnHeat[i] = qsub8(turnHeat[i], HEAT_DECAY_PER_FRAME);
  }

  renderFrame(leds, modeFrom, modeTo, progress, t, encPos, turnHeat, jetsAwake);
}

#if TWIN_DEBUG
// Chunked, non-blocking framebuffer dump.
//
// The old version wrote all 957 bytes in one call. The TX buffer is 64
// bytes, so at 115200 baud that call blocked the loop for roughly 80 ms,
// four times a second: no rendering, no UART polling. That was the visible
// lag, and worse, encoder packets arriving during the stall overflowed the
// 64 byte RX buffer and were silently dropped, so knob positions on the
// board drifted away from the sender's mirror.
//
// This version writes only what the TX buffer can take without blocking
// and resumes on later loop iterations. The payload is not double buffered
// (there is no RAM for that), so a frame rendered mid-dump tears the
// snapshot. Harmless for a 4 Hz debug viewer, and the checksum is computed
// over the bytes actually sent, so the packet stays valid.
void serviceFramebufferDump() {
  if (!dumpActive) {
    unsigned long now = millis();
    if (now - lastDumpMs < DUMP_INTERVAL_MS) return;
    if (Serial.availableForWrite() < 2) return;
    lastDumpMs = now;
    Serial.write((uint8_t)0xAB);
    Serial.write((uint8_t)0x80);
    dumpChecksum = 0x80;
    dumpPos = 0;
    dumpActive = true;
  }

  const uint8_t *bytes = (const uint8_t *)leds;
  const uint16_t payloadLen = N_TOTAL * 3;

  while (dumpPos < payloadLen && Serial.availableForWrite() > 1) {
    uint8_t b = bytes[dumpPos++];
    Serial.write(b);
    dumpChecksum ^= b;
  }

  if (dumpPos >= payloadLen && Serial.availableForWrite() > 0) {
    Serial.write(dumpChecksum);
    dumpActive = false;
  }
}

// Board status report: [0xAB][0x81][16 x encPos][mode][flags][xor], 21 bytes.
// mode is 0xFF until the first mode message has arrived. flags: bit0 =
// transitioning, bit1 = jetsAwake.
//
// This closes the loop for the twin. The one-way protocol has no delivery
// guarantee, and bytes really are lost during FastLED.show(), so the twin
// uses this to confirm mode changes landed, to heal its knob mirrors from
// the board's true positions, and to show the real transition state.
// Written whole-packet-or-nothing so it never blocks, and never while a
// framebuffer dump is mid-flight so the two streams cannot interleave.
void maybeSendStatus() {
  if (dumpActive) return;
  unsigned long now = millis();
  if (now - lastStatusMs < STATUS_INTERVAL_MS) return;
  if (Serial.availableForWrite() < 21) return;
  lastStatusMs = now;

  uint8_t sum = 0x81;
  Serial.write((uint8_t)0xAB);
  Serial.write((uint8_t)0x81);
  for (uint8_t i = 0; i < N_ENCODERS; i++) {
    Serial.write(encPos[i]);
    sum ^= encPos[i];
  }
  uint8_t mode = (state == ST_RUNNING) ? modeTo : 0xFF;
  uint8_t flags = (transitioning ? 1 : 0) | (jetsAwake ? 2 : 0);
  Serial.write(mode);
  sum ^= mode;
  Serial.write(flags);
  sum ^= flags;
  Serial.write(sum);
}
#endif

// ===========================================================================
// STATUS LED
// solid = WAITING, slow blink = RUNNING idle, off = RUNNING active,
// triple blink = checksum failure, non-latching.
// ===========================================================================

void updateStatusLed() {
  if (errorBlinkActive) {
    unsigned long elapsed = millis() - errorBlinkStartMs;
    uint8_t seg = (uint8_t)(elapsed / ERROR_SEG_MS);
    if (seg >= ERROR_SEGMENTS) {
      errorBlinkActive = false;
    } else {
      digitalWrite(PIN_STATUS, (seg % 2 == 0) ? HIGH : LOW);
      return;
    }
  }

  if (state == ST_WAITING) {
    digitalWrite(PIN_STATUS, HIGH);
  } else if (state == ST_RUNNING && !transitioning && modeTo == MODE_IDLE) {
    digitalWrite(PIN_STATUS, ((millis() / 500) % 2) ? HIGH : LOW);
  } else if (state == ST_RUNNING && !transitioning && modeTo == MODE_ACTIVE) {
    digitalWrite(PIN_STATUS, LOW);
  } else {
    // Lamp test, or mid-transition: not specified. Slow blink as a safe
    // default so the board never looks dead.
    digitalWrite(PIN_STATUS, ((millis() / 500) % 2) ? HIGH : LOW);
  }
}

// ===========================================================================
// MAIN LOOP
// ===========================================================================

void loop() {
  pollUart();

  unsigned long now = millis();

  switch (state) {
    case ST_LAMP_TEST:
      if (now - lastFrameMs >= FRAME_INTERVAL_MS) {
        lastFrameMs = now;
        bool done = lampTestFrame(leds, (uint16_t)(now - lampTestStartMs));
        FastLED.show();
        if (done) {
          fill_solid(leds, N_TOTAL, CRGB::Black);
          FastLED.show();
          // Straight into a mode, IDLE unless one arrived during the
          // test. No dark waiting state: an empty tub should breathe.
          resetEncoderPositions();
          jetsAwake = false;
          modeFrom = bootMode;
          modeTo   = bootMode;
          transitioning = false;
          state = ST_RUNNING;
        }
      }
      break;

    case ST_WAITING:
      // No longer entered: the lamp test hands straight into ST_RUNNING.
      // Kept so the state machine stays total.
      break;

    case ST_RUNNING:
      if (now - lastFrameMs >= FRAME_INTERVAL_MS) {
        lastFrameMs = now;
        renderRunningFrame();
        FastLED.show();
      }
      break;
  }

#if TWIN_DEBUG
  serviceFramebufferDump();
  maybeSendStatus();
#endif

  updateStatusLed();
}

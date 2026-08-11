#ifndef RENDER_CORE_H
#define RENDER_CORE_H

#include <FastLED.h>

// ===========================================================================
// RENDER CORE
// ---------------------------------------------------------------------------
// Every function here is PURE: given a strip, a pixel index, and a time value,
// it returns one color. It reads no globals it does not have to and it
// allocates nothing.
//
// This is the whole memory strategy. A crossfade between two modes costs zero
// extra RAM because we compute both colors for pixel i, blend, write once, and
// move on. Never introduce a second CRGB array.
// ===========================================================================

// --- Strip geometry --------------------------------------------------------

#define N_UNDER   160
#define N_POLE    100   // was 110; physical pole run confirmed at 100
#define N_PANEL    48
#define N_TOTAL   (N_UNDER + N_POLE + N_PANEL)   // 308

// Offsets into the single shared framebuffer.
#define OFF_UNDER   0
#define OFF_POLE    (OFF_UNDER + N_UNDER)        // 160
#define OFF_PANEL   (OFF_POLE + N_POLE)          // 270

// Strip identifiers.
enum Strip : uint8_t { S_UNDER = 0, S_POLE = 1, S_PANEL = 2 };

// --- Panel layout ----------------------------------------------------------
// 16 knobs, 48 LEDs, 4 racks. Panel-local indices (0-47).
//
// PROVISIONAL. Confirm the physical LED order inside each rack against the
// hardware before trusting this.

#define PANEL_FAUCET_START   0    // 9 LEDs: 5 top (temp pair) + 4 front (filter)
#define PANEL_FAUCET_LEN     9
// CONFIRMED: the chain reaches the physical RIGHT aux rack first (9-20),
// then the LEFT rack (21-32). Each rack is a 2x2 of knobs, 3 horizontal
// LEDs per knob, serpentine wired: see the aux dispatch in
// pixelActivePanel for chain order and row reversal.
#define PANEL_AUXR_START     9    // physical RIGHT rack, 12 LEDs, enc 3-6
#define PANEL_AUXR_LEN      12
#define PANEL_AUXL_START    21   // physical LEFT rack, 12 LEDs, enc 7-10
#define PANEL_AUXL_LEN      12
#define PANEL_JETS_START    33    // 15 LEDs, 5 knobs
#define PANEL_JETS_LEN      15

// --- Encoder IDs -----------------------------------------------------------
// PROVISIONAL. This is the single biggest open question. Get the real mapping
// from Max before Phase 2.

#define ENC_FAUCET_COLD    0
#define ENC_FAUCET_HOT     1
#define ENC_FAUCET_FILTER  2
#define ENC_AUXR_FIRST     3    // 3,4,5,6: RIGHT rack, chain order BL,BR,TR,TL
#define ENC_AUXL_FIRST     7    // 7,8,9,10: LEFT rack, chain order TL,TR,BR,BL

// Sound-side knob names (from Nile). The right aux is the HOT aux, the
// left aux is the COLD aux. Unnamed knobs (4, 6, 8, 10) are still TBD.
// MIDI defaults are 0-127; rotation units are 0-255, exactly double.
#define ENC_TRANSFORM_HOT   3   // Transform X, hot aux bottom-left,  default 0
#define ENC_PITCH_HOT       5   // Pitch,       hot aux top-right,    default 128
#define ENC_PITCH_COLD      7   // Pitch,       cold aux top-left,    default 128
#define ENC_TRANSFORM_COLD  9   // Transform X, cold aux bottom-right, default 0
#define ENC_JETS_FIRST    11    // 11,12,13,14
#define ENC_JETS_CENTER   15    // the jets "timer" knob

#define N_ENCODERS        16

// --- Tunables --------------------------------------------------------------

#define TRANSITION_MS   5000UL   // DISAGREEMENT: spec says 3000, Max said 5000
#define TARGET_FPS        30

#define IDLE_BRIGHTNESS   160    // 0-255, ceiling of the idle breathing pulse
#define ACTIVE_BRIGHTNESS 200

// --- Per-knob identity hues ------------------------------------------------
// Stored in flash, not RAM. The hue is a knob's IDENTITY and never changes
// with position: each knob is recognizably "the teal one" or "the amber
// one" at every setting. Value is read from the fill meter, not the hue.
//
// The 12 standard hues are evenly spaced around the wheel (~21 apart) and
// ordered so adjacent knobs in the same rack sit ~107 apart: neighbors are
// never near-neighbors on the wheel.

const uint8_t KNOB_BASE_HUE[N_ENCODERS] PROGMEM = {
  160, 0, 140,          // faucet: cold(blue) hot(red) filter(sky), own renderers
  0, 107, 213, 64,      // enc 3-6, RIGHT rack: red, green, purple, yellow
  171, 21, 128, 235,    // enc 7-10, LEFT rack: blue, orange, aqua, pink
  85, 192, 43, 149,     // jets outer: green, purple, orange, azure
  245                   // jets center: was 80, moved off jets-1's 85 so the
                        // rack's five knobs stay distinguishable
};

inline uint8_t knobHue(uint8_t enc) {
  return pgm_read_byte(&KNOB_BASE_HUE[enc]);
}

// ===========================================================================
// IDLE MODE
// Warm, dim, slow. Yellow through orange through red. Pole and underlight
// deliberately out of phase so they do not pulse in lockstep.
// Panel is fully off.
// ===========================================================================

inline CRGB pixelIdle(uint8_t strip, uint8_t i, uint16_t t) {
  if (strip == S_PANEL) return CRGB::Black;

  // Phase offset splits the two big runs apart.
  uint8_t phase = (strip == S_POLE) ? 128 : 0;

  // Slow travelling wave along the strip.
  uint8_t wave = sin8((uint8_t)(i * 3) + (uint8_t)(t >> 4) + phase);

  // Wider warm band than before: red through yellow through a bit of
  // amber-gold. Still nowhere near the full rainbow ACTIVE mode uses.
  uint8_t hue = scale8(wave, 56);

  // Breathing brightness: one full cycle roughly every 4 seconds. The floor
  // sits at 25 so the swing up to IDLE_BRIGHTNESS reads as a real pulse
  // instead of a barely-visible flicker.
  uint8_t breath = sin8((uint8_t)(t >> 4) + phase);
  uint8_t val = 25 + scale8(breath, IDLE_BRIGHTNESS - 25);

  return CHSV(hue, 255, val);
}

// ===========================================================================
// ACTIVE MODE: POLE AND UNDERLIGHT
// Same pattern family as idle, wider color range, brighter.
// ===========================================================================

inline CRGB pixelActiveAmbient(uint8_t strip, uint8_t i, uint16_t t) {
  uint8_t phase = (strip == S_POLE) ? 128 : 0;

  // Full hue wheel now instead of a warm band.
  uint8_t hue = (uint8_t)(i * 2) + (uint8_t)(t >> 4) + phase;

  uint8_t breath = sin8((uint8_t)(t >> 6) + phase);
  uint8_t val = 120 + scale8(breath, ACTIVE_BRIGHTNESS - 120);

  return CHSV(hue, 240, val);
}

// ===========================================================================
// ACTIVE MODE: PANEL
// Standard knobs render as fill meters. Hue = knob identity, fixed forever.
// Brightness across the 3 LEDs = knob value. Through diffused PLA in a dim
// room, brightness reads at a glance where a hue shift never did.
//
// Faucet and jets special cases have their own renderers below.
// ===========================================================================

// Fill meter brightness range. The floor is deliberate: an unfilled LED
// stays dimly lit so the knob's identity color never disappears.
#define KNOB_FLOOR_BRIGHT   40
#define KNOB_FULL_BRIGHT   220

// Standard knob fill meter with a fractional leading edge.
// Position 0-255 maps across the knob's 3 LEDs, 85 units of travel each.
// LEDs below the fill point are full, the leading LED is proportional,
// LEDs above sit at the floor. All integer math: the fraction within a
// segment is (pos - segStart) * 3, since 85 * 3 = 255.
//
// heat: 255 right after an encoder tick, decays to 0 in the frame loop
// over ~500ms. While hot the knob desaturates toward white by ~40% and
// brightens ~15%: Max's "color feedback while you're actively turning,
// then it settles to the stagnant color for that knob."
inline CRGB pixelStandardKnob(uint8_t enc, uint8_t subIndex, uint8_t pos, uint8_t heat) {
  uint8_t hue = knobHue(enc);          // identity, never varies with pos

  uint8_t segStart = subIndex * 85;
  uint8_t val;
  if (pos <= segStart) {
    val = KNOB_FLOOR_BRIGHT;
  } else {
    uint8_t into = pos - segStart;
    if (into >= 85) {
      val = KNOB_FULL_BRIGHT;
    } else {
      uint8_t frac = into * 3;         // 0..84 -> 0..252, no division
      val = lerp8by8(KNOB_FLOOR_BRIGHT, KNOB_FULL_BRIGHT, frac);
    }
  }

  // Saturating double: the glow holds at FULL strength for the top half of
  // the countdown, then fades through the bottom half. The knob stays
  // bright the whole time it is being turned, lingers briefly after the
  // last tick, and only then settles back to its identity color.
  uint8_t glow = qadd8(heat, heat);
  uint8_t sat = 255 - scale8(glow, 100);   // hot: sat 155, ~40% toward white
  val = qadd8(val, scale8(glow, 38));      // hot: ~15% brighter, saturating add

  return CHSV(hue, sat, val);
}

// Faucet top 5 spatial weights, left (cold anchor) to right (hot anchor).
// Fixed table, not a formula: the previous "i * 64" wrapped a uint8_t at
// i == 4 (4*64 == 256), which silently zeroed LED4's hot weight and pushed
// its cold weight to 255, so the hot-anchor LED showed the same blue as the
// cold-anchor LED regardless of the hot knob. These values match the
// approximate percentages in docs/SPEC.md 6.2: 100/0, 80/20, 50/50, 20/80,
// 0/100.
// CONFIRMED ON HARDWARE: the strip enters the faucet rack from the hot
// side, so index 0 is the HOT anchor and index 4 the COLD anchor. This is
// the reverse of the original guess. Answers part of open question 2.
const uint8_t FAUCET_COLD_WEIGHT[5] PROGMEM = { 0, 51, 127, 204, 255 };
const uint8_t FAUCET_HOT_WEIGHT[5]  PROGMEM = { 255, 204, 128, 51, 0 };

// Faucet top 5: hot and cold as *amounts*, mixed left to right.
// LED0 is fully cold-weighted, LED4 fully hot-weighted.
inline CRGB pixelFaucetTemp(uint8_t i, uint8_t coldPos, uint8_t hotPos) {
  uint8_t coldWeight = pgm_read_byte(&FAUCET_COLD_WEIGHT[i]);
  uint8_t hotWeight  = pgm_read_byte(&FAUCET_HOT_WEIGHT[i]);

  uint8_t blue = scale8(coldWeight, coldPos);
  uint8_t red  = scale8(hotWeight,  hotPos);

  return CRGB(red, 0, blue);
}

// Fixed spatial position of each of the 4 filter LEDs across the knob's
// 0-255 range, two left of center and two right.
const uint8_t FILTER_LED_POS[4] PROGMEM = { 0, 85, 170, 255 };

// Faucet front 4: filter knob. 2 LEDs left, 2 right. WHITE, full blast:
// sky blue at half brightness read as dead on hardware. 128 is neutral.
// A triangular falloff from the knob's position lights whichever LEDs are
// spatially closest, so turning the knob sweeps a peak of brightness
// across the row. A dim floor keeps all 4 LEDs faintly present so the
// row never looks broken.
inline CRGB pixelFaucetFilter(uint8_t i, uint8_t pos) {
  uint8_t ledPos = pgm_read_byte(&FILTER_LED_POS[i]);
  uint8_t dist = (pos > ledPos) ? (pos - ledPos) : (ledPos - pos);

  uint16_t drop = (uint16_t)dist * 2;
  uint8_t peak = (drop >= 255) ? 0 : (255 - (uint8_t)drop);
  uint8_t val = (peak < 24) ? 24 : peak;

  return CHSV(0, 0, val);   // sat 0 = white, peak hits full 255
}

// ===========================================================================
// PANEL DISPATCH
// Maps a panel-local index (0-47) to the right behavior.
// ===========================================================================

inline CRGB pixelActivePanel(uint8_t i, const uint8_t *encPos,
                             const uint8_t *turnHeat, uint16_t t, bool jetsAwake) {

  // --- Faucet rack: 9 LEDs, special ---
  if (i < 9) {
    if (i < 5) {
      return pixelFaucetTemp(i, encPos[ENC_FAUCET_COLD], encPos[ENC_FAUCET_HOT]);
    }
    return pixelFaucetFilter(i - 5, encPos[ENC_FAUCET_FILTER]);
  }

  // --- Jets rack: 15 LEDs, sleeps until the center knob is touched ---
  if (i >= PANEL_JETS_START) {
    uint8_t local = i - PANEL_JETS_START;

    if (!jetsAwake) {
      // Mirror the underlight so the rack blends into the tub surface.
      return pixelActiveAmbient(S_UNDER, local * 4, t);
    }

    uint8_t knobIdx = local / 3;                  // 0..4, chain order
    uint8_t sub     = local % 3;
    // CONFIRMED on hardware by identity colors: the LED chain walks the
    // rack JBL, JBR, JC(timer), JUL, JUR, not encoder-id order. Encoder
    // ids: JUL 11, JUR 12, JBL 13, JBR 14, JC 15.
    static const uint8_t JETS_CHAIN_ENC[5] PROGMEM = { 13, 14, 15, 11, 12 };
    uint8_t enc = pgm_read_byte(&JETS_CHAIN_ENC[knobIdx]);
    // JC's 3 LEDs run opposite the chain direction on hardware: reverse
    // its sub-index so the fill meter grows the right way.
    if (knobIdx == 2) sub = 2 - sub;
    return pixelStandardKnob(enc, sub, encPos[enc], turnHeat[enc]);
  }

  // --- Aux racks: 2x2 knobs each, serpentine wired ---
  // In each rack the chain covers one row of two knobs, turns around, and
  // comes back along the other row, so the second-traversed row's LEDs run
  // right to left. Reversing that row's sub-index keeps every fill meter
  // growing the same visual direction. Confirmed on the right rack; the
  // left rack is the same printed part and is assumed to match (its fill
  // directions are not yet verified on hardware).
  uint8_t local   = i - PANEL_AUXR_START;         // 0..23
  uint8_t knobIdx = local / 3;                    // 0..7, chain order
  uint8_t sub     = local % 3;
  if ((knobIdx & 3) >= 2) sub = 2 - sub;          // second row: reversed run
  uint8_t enc     = ENC_AUXR_FIRST + knobIdx;     // 3..10
  return pixelStandardKnob(enc, sub, encPos[enc], turnHeat[enc]);
}

// ===========================================================================
// TOP LEVEL
// One entry point. Handles both modes and the crossfade between them without
// allocating anything.
//
// progress: 0 = fully in `from` mode, 255 = fully in `to` mode.
// ===========================================================================

inline CRGB pixelForMode(uint8_t mode, uint8_t strip, uint8_t i,
                         uint16_t t, const uint8_t *encPos,
                         const uint8_t *turnHeat, bool jetsAwake) {
  if (mode == 0) {                                // IDLE
    return pixelIdle(strip, i, t);
  }
  if (strip == S_PANEL) {                         // ACTIVE panel
    return pixelActivePanel(i, encPos, turnHeat, t, jetsAwake);
  }
  return pixelActiveAmbient(strip, i, t);         // ACTIVE pole / underlight
}

// Render the entire framebuffer for one frame.
// During a transition this blends two modes per pixel. Zero extra RAM.
inline void renderFrame(CRGB *leds, uint8_t fromMode, uint8_t toMode,
                        uint8_t progress, uint16_t t,
                        const uint8_t *encPos, const uint8_t *turnHeat,
                        bool jetsAwake) {

  for (uint16_t n = 0; n < N_TOTAL; n++) {
    uint8_t strip, idx;

    if (n < OFF_POLE)       { strip = S_UNDER; idx = (uint8_t)n; }
    else if (n < OFF_PANEL) { strip = S_POLE;  idx = (uint8_t)(n - OFF_POLE); }
    else                    { strip = S_PANEL; idx = (uint8_t)(n - OFF_PANEL); }

    if (progress == 255 || fromMode == toMode) {
      leds[n] = pixelForMode(toMode, strip, idx, t, encPos, turnHeat, jetsAwake);
    } else {
      CRGB a = pixelForMode(fromMode, strip, idx, t, encPos, turnHeat, jetsAwake);
      CRGB b = pixelForMode(toMode,   strip, idx, t, encPos, turnHeat, jetsAwake);
      leds[n] = blend(a, b, progress);
    }
  }
}

// ===========================================================================
// STARTUP LAMP TEST
// A 20-pixel white block runs the length of each strip in sequence.
// Verifies every LED before the show starts.
// ===========================================================================

#define LAMP_BLOCK 20

// Returns true when the test is finished.
// stepMs 32: one pixel per 32ms, roughly 11 seconds for the full walk.
// Total duration (338 * stepMs) must stay under 65535 so elapsedMs fits
// in a uint16_t; 32 gives 10816.
inline bool lampTestFrame(CRGB *leds, uint16_t elapsedMs) {
  const uint16_t stepMs = 32;
  uint16_t head = elapsedMs / stepMs;

  if (head > N_TOTAL + LAMP_BLOCK) return true;

  for (uint16_t n = 0; n < N_TOTAL; n++) {
    bool lit = (n <= head) && (n + LAMP_BLOCK > head);
    leds[n] = lit ? CRGB(255, 255, 255) : CRGB::Black;
  }
  return false;
}

#endif // RENDER_CORE_H

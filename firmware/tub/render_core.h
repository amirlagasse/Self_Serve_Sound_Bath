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

// Sound-side knob names, full mapping confirmed with Nile 2026-08-11.
// The right aux is the HOT aux, the left aux is the COLD aux; the racks
// are mirror images (same param under the same hand position each side).
// MIDI defaults are 0-127; rotation units are 0-255, exactly double.
#define ENC_REVERB_HOT      3   // Reverb,   hot aux bottom-left,   default 0
#define ENC_BITCRUSH_HOT    4   // Bitcrush, hot aux bottom-right,  default 0
#define ENC_PITCH_HOT       5   // Pitch,    hot aux top-right,     default 128
#define ENC_MODX_HOT        6   // Mod X,    hot aux top-left,      default 0
#define ENC_PITCH_COLD      7   // Pitch,    cold aux top-left,     default 128
#define ENC_MODX_COLD       8   // Mod X,    cold aux top-right,    default 0
#define ENC_REVERB_COLD     9   // Reverb,   cold aux bottom-right, default 0
#define ENC_BITCRUSH_COLD  10   // Bitcrush, cold aux bottom-left,  default 0
#define ENC_JETS_PITCH     11   // JUL, percussion pitch,      default 128
#define ENC_JETS_RATE      12   // JUR, percussion rate,       default 128
#define ENC_JETS_TRANSIENT 13   // JBL, percussion transients, default 128
#define ENC_JETS_FLANGER   14   // JBR, percussion flanger,    default 0
#define ENC_JETS_FIRST    11    // 11,12,13,14
#define ENC_JETS_CENTER   15    // JC, percussion volume, default 0 (timer idea dropped)

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
// Family scheme per Amir 2026-08-11: the hot aux wears only warm hues,
// the cold aux only cool hues, and the jets corners take the leftover
// wheel space (green / purple / magenta / rose) so they read as neither
// family. JC is WHITE: its hue entry is unused, pixelStandardKnob forces
// its saturation to 0. All 12 identity colors are unique. Must match
// KNOB_HUES in twin/index.html.

const uint8_t KNOB_BASE_HUE[N_ENCODERS] PROGMEM = {
  160, 0, 140,          // faucet: cold(blue) hot(red) filter(sky), own renderers
  0, 22, 43, 64,        // enc 3-6, hot aux: red, orange, amber, yellow
  160, 143, 128, 180,   // enc 7-10, cold aux: blue, cyan, aqua, indigo
  85, 192, 43, 149,     // jets corners: ORIGINAL colors restored per the
                        // 2026-08-11 bench review (green, purple, orange,
                        // azure). Note 43 repeats the hot rack's amber and
                        // 149 sits near the cold rack's cyan; accepted,
                        // different racks.
  0                     // JC: white, sat forced to 0 in pixelStandardKnob
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
// ACTIVE MODE: POLE (faucet stem / showerhead)
// Water climbing the pipe. Color mixes the two faucet knobs: majority
// cold reads blue, majority hot reads red, an even split reads as a
// violet blend. Brightness pulses travel up the stem like slugs of
// water, and the combined knob travel sets the flow: taps low is a
// faint trickle, taps cranked is full blast.
// ===========================================================================

// Direction flag for the climb. Flipped to 0 on 2026-08-11: on the real
// pole the run starts at the TOP, so index must be reversed for pulses
// to wash upward. Set back to 1 if a rewire changes the feed end.
#define POLE_UP_IS_INCREASING 0

inline CRGB pixelPoleWater(uint8_t i, uint16_t t, uint8_t coldPos, uint8_t hotPos) {
#if POLE_UP_IS_INCREASING
  uint8_t h = i;
#else
  uint8_t h = (uint8_t)(N_POLE - 1 - i);
#endif

  // Majority mix with no division: even knobs sit at 128, all-hot 255,
  // all-cold 0. Positions are 0-255 rotation units so >>1 cannot wrap.
  uint8_t mix = (uint8_t)(128 + (hotPos >> 1) - (coldPos >> 1));

  // Cold water blue against hot water red-orange.
  CRGB c = blend(CRGB(0, 72, 255), CRGB(255, 24, 0), mix);

  // Travelling pulse. (t >> 2) mod 256 advances ~250 units/s against a
  // 5-unit-per-pixel spatial ramp: peaks climb ~50 px/s, about 2 seconds
  // bottom to top, roughly two slugs visible on the run at once.
  // Squaring the sine sharpens the slugs. Wraps clean at t rollover
  // because 65536 >> 2 is a multiple of 256.
  uint8_t wave = sin8((uint8_t)(t >> 2) - (uint8_t)(h * 5));
  wave = scale8(wave, wave);

  // Flow: how hard the pulses hit. qadd8 saturates, so the default
  // 128 + 128 already reads as full water. The floor keeps the stem
  // faintly water-colored even with both taps shut.
  uint8_t flow = qadd8(coldPos, hotPos);
  uint8_t val = qadd8(20, scale8(wave, scale8(flow, 235)));

  c.nscale8_video(val);
  return c;
}

// ===========================================================================
// ACTIVE MODE: BOWL BUBBLES
// Jets bubbles on the underlight, driven ONLY by the jets center knob
// (JC, the volume jet). A brightness modifier, not a color change: the
// ambient rainbow keeps flowing underneath and bright patches pop on
// top of it. JC at 0 (its default) is no bubbles at all; the patches
// get denser and harder as the jet level rises. Subtle at low levels.
// ===========================================================================

inline uint8_t bubbleBoost(uint8_t i, uint16_t t, uint8_t level) {
  if (level == 0) return 0;

  // Two waves drifting at different speeds and spatial frequencies
  // multiply into soft blobs that wander around the bowl.
  uint8_t patch   = sin8((uint8_t)(i * 5)  + (uint8_t)(t >> 5));
  uint8_t shimmer = sin8((uint8_t)(i * 11) - (uint8_t)(t >> 3));
  uint8_t field   = scale8(patch, shimmer);

  // Only the crests pop: cut the floor away, then steepen what is left
  // so bubbles read as distinct bits, not a global brightening.
  uint8_t crest = qsub8(field, 160);
  uint8_t pop   = qadd8(qadd8(crest, crest), crest);

  return scale8(pop, level);
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
  // JC is the white knob: base saturation 0, and qsub8 keeps the turning
  // glow from wrapping it back to a color.
  uint8_t glow = qadd8(heat, heat);
  uint8_t baseSat = (enc == ENC_JETS_CENTER) ? 0 : 255;
  uint8_t sat = qsub8(baseSat, scale8(glow, 100));   // hot: ~40% toward white
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
  // Center-snap display (Amir 2026-08-11): the first detent click away
  // from center throws the lit peak half the row's travel; clicks past
  // that move it much less. Off-center is unmissable, and re-centering
  // is easy because the peak only snaps home on the exact center click.
  // One detent = 16 rotation units (knobCfg stepsPerRange 16).
  uint8_t mag = (pos >= 128) ? (pos - 128) : (128 - pos);
  uint8_t off;
  if (mag == 0)      off = 0;
  else if (mag < 16) off = mag * 4;                                  // first click: x4
  else               off = 64 + (uint8_t)(((uint16_t)(mag - 16) * 9) >> 4);
  uint8_t disp = (pos >= 128) ? (uint8_t)(128 + off) : (uint8_t)(128 - off);

  uint8_t ledPos = pgm_read_byte(&FILTER_LED_POS[i]);
  uint8_t dist = (disp > ledPos) ? (disp - ledPos) : (ledPos - disp);

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
  CRGB c = pixelStandardKnob(enc, sub, encPos[enc], turnHeat[enc]);

  // Faucet master ceiling (Amir 2026-08-11): the hot faucet knob gates
  // the whole hot rack's brightness, the cold knob gates the cold rack.
  // Faucet at 0 = rack dark. Ramps linearly to half travel (128), fully
  // open from there up: past half the faucet stops affecting the rack.
  // Each knob's own fill meter still moves underneath the ceiling.
  uint8_t master = (enc <= ENC_MODX_HOT) ? encPos[ENC_FAUCET_HOT]
                                         : encPos[ENC_FAUCET_COLD];
  if (master < 128) {
    c.nscale8_video((uint8_t)(master << 1));
  }
  return c;
}

// ===========================================================================
// TOP LEVEL
// One entry point. Handles both modes and the crossfade between them without
// allocating anything.
//
// progress: 0 = fully in `from` mode, 255 = fully in `to` mode.
// ===========================================================================

// Boot pseudo-mode: renders black. Never on the wire, only used as the
// crossfade source for the first real mode after the dark boot wait.
#define MODE_OFF 0xFE

inline CRGB pixelForMode(uint8_t mode, uint8_t strip, uint8_t i,
                         uint16_t t, const uint8_t *encPos,
                         const uint8_t *turnHeat, bool jetsAwake) {
  if (mode == MODE_OFF) return CRGB::Black;       // dark boot wait
  if (mode == 0) {                                // IDLE
    return pixelIdle(strip, i, t);
  }
  if (strip == S_PANEL) {                         // ACTIVE panel
    return pixelActivePanel(i, encPos, turnHeat, t, jetsAwake);
  }
  if (strip == S_POLE) {                          // ACTIVE pole: water climb
    return pixelPoleWater(i, t, encPos[ENC_FAUCET_COLD], encPos[ENC_FAUCET_HOT]);
  }
  // ACTIVE underlight: flowing ambient plus jets bubbles on top.
  CRGB c = pixelActiveAmbient(strip, i, t);
  uint8_t boost = bubbleBoost(i, t, encPos[ENC_JETS_CENTER]);
  if (boost) {
    c.r = qadd8(c.r, boost);
    c.g = qadd8(c.g, boost);
    c.b = qadd8(c.b, boost);
  }
  return c;
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

# TUB WIRING PINOUTS

Everything needed to assemble the tub electronics. Matches the firmware as flashed; do not improvise pin choices, the code expects exactly these.

## The three boxes

| Thing | Powered by | Connects to |
|---|---|---|
| Teensy 4.1 | USB cable to the music computer (also carries MIDI) | Uno (1 wire + ground), ToF sensor, all 16 knobs |
| Arduino Uno | LED power supply 5V (see Power below) | LED strips (4 data wires), Teensy |
| LED strips | LED power supply 5V directly | Uno data pins |

## Board-to-board (2 wires, do not skip the ground)

| From | To |
|---|---|
| Teensy pin **1** (TX1) | Uno pin **0** (RX) |
| Teensy **GND** | Uno **GND** |

Nothing ever connects to Uno pin 1.

## Power

- LED strips: 5V and GND from the power supply, direct.
- Uno: 5V from the power supply's screw terminals into the Uno's **5V pin**, PSU GND to Uno **GND**.
  - **NEVER plug USB into the Uno while the 5V pin is fed.** Pull the 5V wire first.
  - All grounds end up common: PSU, strips, Uno, Teensy.
- Teensy: its USB cable (music computer or any USB power brick).
- Power-up order: Uno/strips first, then Teensy.

## Uno pins

| Uno pin | Goes to |
|---|---|
| 0 (RX) | Wire from Teensy pin 1 |
| 1 (TX) | NOTHING, keep empty |
| 6 | UNDERLIGHT strip data (160 LEDs) |
| 7 | POLE strip data (100 LEDs) |
| 8 | FRONT PANEL strip data (faucet + both ox racks, 33 LEDs) |
| 9 | JACUZZI rack strip data (15 LEDs) |
| 5V | From LED PSU screw terminals |
| GND | PSU ground + Teensy ground |

Data wires go to each strip's INPUT end (arrows on the strip point away from the input).

## Teensy pins: sensor and serial

| Teensy pin | Goes to |
|---|---|
| 1 (TX1) | Uno pin 0 |
| 0 (RX1) | NOTHING, keep empty |
| 3.3V (3rd pin from USB corner, next to VIN and GND) | ToF sensor VIN (red) |
| GND | ToF sensor GND (black) |
| 18 | ToF sensor SDA (serial data) |
| 19 | ToF sensor SCL (serial clock) |

ToF board extra pins (XSHUT / GPIO1 / INT): leave unconnected.
**Do NOT power the ToF from 5V or from the LED supply. 3.3V pin only.**

## Teensy pins: the 16 knobs

Every knob (rotary encoder) has 3 signal legs:

- **Middle leg: GND** (always)
- **YELLOW wire** (the one with the yellow marker) and **RED wire**: exact pins below. The colors are NOT interchangeable, the firmware expects exactly this. Wire it colorblind-perfect and every knob turns the right way with zero fixes.

| Knob | Where it is | YELLOW pin | RED pin |
|---|---|---|---|
| FL | Faucet left (cold) | **2** | **3** |
| FR | Faucet right (hot) | **4** | **5** |
| FC | Faucet center (shifter/filter) | **6** | **7** |
| RBL | Hot ox (right rack), bottom-left | **8** | **9** |
| RBR | Hot ox, bottom-right | **11** | **10** |
| RUR | Hot ox, upper-right | **12** | **36** (NOT 13, see warning) |
| RUL | Hot ox, upper-left | **14** | **15** |
| LUL | Cold ox (left rack), upper-left | **16** | **17** |
| LUR | Cold ox, upper-right | **21** | **20** |
| LBR | Cold ox, bottom-right | **23** | **22** |
| LBL | Cold ox, bottom-left | **25** | **24** |
| JUL | Jacuzzi, upper-left | **27** | **26** |
| JUR | Jacuzzi, upper-right | **28** | **29** |
| JBL | Jacuzzi, bottom-left | **31** | **30** |
| JBR | Jacuzzi, bottom-right | **32** | **33** |
| JC | Jacuzzi center (the timer) | **35** | **34** |

Read each row carefully: on some knobs the yellow pin number is HIGHER than the red one. That is intentional. Follow the table, not a pattern.

This table was verified against the fully working bench build on 2026-08-11.

**WARNING: Teensy pin 13 must never be used for a knob.** The board's onboard LED is tied to it and kills the signal (the knob will only count one direction). That is why RUR lives on 12 + 36.

## Backwards knob?

If the colors are wired exactly per the table, this should not happen. But if a knob does count the wrong way (mislabeled wire, replaced encoder), the fix needs no computer: **swap that knob's yellow and red wires with each other at the Teensy end.** The middle/GND leg stays put. If a knob only counts in ONE direction no matter what, one of its two signal wires is loose (or landed on pin 13).

## Quick health check after assembly

1. Power the strips + Uno: a 20-pixel white block should chase along every strip (lamp test), then the tub settles into slow warm breathing.
2. Plug in the Teensy: nothing visible changes (still breathing) until someone triggers the sensor.
3. Sit in front of the ToF sensor (under 70 cm) for a few seconds: the whole tub should switch to vibrant rainbow and the knob racks light up.
4. Turn each knob and check its own 3 LEDs respond, in the right direction.
5. Walk away: after the exit delay the tub drops back to warm breathing.

If a whole strip is dark: data wire on the wrong end or wrong Uno pin. If everything flickers garbage: a missing ground bond.

# TUB MIDI CONNECTION GUIDE

Everything needed to hook the tub's knobs into Logic Pro. Matches the firmware as flashed 2026-08-11. Wiring lives in `PINOUTS.md`.

## The one cable

The Teensy's USB cable plugs into the music computer. That single cable carries:

- **USB MIDI**: what Logic sees. The Teensy appears as a standard USB MIDI device in macOS. No drivers, no config.
- **USB serial**: debug console and the digital twin (Chrome only). Both work at the same time as MIDI.

If the Teensy does not show up as a MIDI device, the firmware was built wrong. It must be compiled with `usb=serialmidi` (see README). A plain serial build silently drops all MIDI.

## The numbers

All messages are **Control Change on channel 1**. CC number = 16 + encoder id.

| CC | Knob | Panel position | Controls | Default |
|----|------|----------------|----------|---------|
| 16 | FL | Faucet left | Cold blend: volume of cold pads, tracks 3-4 | 64 |
| 17 | FR | Faucet right | Hot blend: volume of hot pads, tracks 1-2 | 64 |
| 18 | FC | Faucet center | Filter: left = low-pass, right = high-pass | 64 (open) |
| 19 | RBL | Hot rack, bottom left | Reverb hot (Valhalla) | 0 |
| 20 | RBR | Hot rack, bottom right | Bitcrush hot (Logic Bitcrusher) | 0 |
| 21 | RUR | Hot rack, top right | Pitch hot (AU Pitch) | 64 |
| 22 | RUL | Hot rack, top left | Mod X hot (Alchemy) | 0 |
| 23 | LUL | Cold rack, top left | Pitch cold (AU Pitch) | 64 |
| 24 | LUR | Cold rack, top right | Mod X cold (Alchemy) | 0 |
| 25 | LBR | Cold rack, bottom right | Reverb cold (Valhalla) | 0 |
| 26 | LBL | Cold rack, bottom left | Bitcrush cold (Logic Bitcrusher) | 0 |
| 27 | JUL | Jacuzzi, upper left | Percussion pitch (AU Pitch) | 64 |
| 28 | JUR | Jacuzzi, upper right | Percussion rate/speed | 64 |
| 29 | JBL | Jacuzzi, bottom left | Percussion transients (Chimera) | 64 |
| 30 | JBR | Jacuzzi, bottom right | Percussion flanger | 0 |
| 31 | JC | Jacuzzi, center | Percussion volume, silent until turned up | 0 |

The hot and cold racks are mirror images: the same parameter sits under the same hand position on each side.

Logic track set: 1 sub bass hot, 2 cmaj pad hot, 3 sub bass cold, 4 emin pad cold, 5-8 Logic Sampler percussion.

## How the controller behaves

- **Knobs only work in ACTIVE mode.** In IDLE every tick is read and thrown away. No MIDI leaves the board.
- **Defaults blast on boot and on every mode change.** All 16 CCs transmit their default values once, both entering and leaving ACTIVE. Whenever the tub empties, Logic resets to the neutral soundscape: percussion silent, effects off, pads at even blend, pitch unshifted. No stale settings survive between soakers.
- **ACTIVE starts from defaults.** A new soaker always begins at the neutral picture and moves live from there.
- **There is no knob click/press function.** Reset happens only through the mode cycle.
- **Knob travel**: full 0-127 range in under one rotation (16 detent steps).
- **Occupancy** (what flips the mode): ToF sensor, someone within 900mm for 3s continuous enters ACTIVE. 5s of continuous absence exits back to IDLE. Any presence blip during the exit countdown resets it.

## Mapping session game plan

1. Plug the Teensy into the Logic machine. Confirm it shows in Audio MIDI Setup. MIDI Monitor.app is handy for watching raw CCs.
2. Force ACTIVE and keep it there (see below). Turn a knob, confirm CCs arrive.
3. Route tracks 1-2 to a HOT summing bus, 3-4 to COLD, 5-8 to PERC. MIDI-learn CC17, CC16, CC31 onto those bus faders: one mapping each instead of four.
4. Map plugin params with Logic Controller Assignments (Cmd+L): click the parameter, turn the physical knob one click, done. Work down the table.
5. Full rehearsal with the real sensor: climb in, 3s, sound fades in from defaults. Climb out, 5s, everything resets. Verify the reset snapshot sounds right.

### The two traps

1. **Stay in ACTIVE for the whole session.** A mode flip mid-mapping blasts all 16 CCs and hijacks whatever parameter MIDI Learn is listening on.
2. **One knob at a time while learning.** Any other CC arriving mid-learn steals the assignment.

### Forcing ACTIVE without sitting in the tub

Two options:

- Park something in front of the ToF sensor within 90cm.
- Leave the ToF unplugged. Occupancy disables itself when the sensor is missing at boot, and manual mode commands then stick. Send `1` over the USB serial console (or the twin's ACTIVE button). Other useful console commands: `0` = IDLE, `m` = flip mode, `t` = sensor status, `k` = knob positions, `h` = help.

## Troubleshooting

| Symptom | Cause |
|---------|-------|
| No MIDI device appears | Firmware built without `usb=serialmidi` |
| Device present, knobs send nothing | Tub is in IDLE. Check mode first, not the wiring |
| All params jumped at once | A mode change fired the defaults snapshot. Normal |
| One knob turns backwards | Swap that knob's yellow/red wires at the Teensy (`PINOUTS.md`) |
| Sensor never triggers | ToF only detected at Teensy boot. Power-cycle the Teensy after wiring |

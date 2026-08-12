# Tub Lighting

Arduino Uno LED controller for an interactive bathtub art exhibit.

A person sits in a bathtub. Knobs around the rim control a generative sound engine. 318 addressable LEDs respond to occupancy and to knob movement. A Teensy 4.1 reads the sensors and encoders and sends state to this Uno over UART. The Uno owns all animation.

Roles: DMC is the Teensy side (Max). Uno is this repo (Amir). Wires is audio and MIDI mapping (Nile).

## Read these first, in order

1. `docs/MEMORY.md` : the 2 KB SRAM constraint. This shapes every architectural decision. Read it before writing any code.
2. `docs/SPEC.md` : full behavioral spec. Single source of truth.
3. `docs/PROTOCOL.md` : UART wire format, verbatim from Max.
4. `docs/OPEN_QUESTIONS.md` : what is still undecided and who owns it.

## Layout

```
firmware/render_core.h    animation logic, pure per-pixel functions
firmware/tub.ino          UART parser, state machine, FastLED driver
twin/index.html           browser control panel and pixel viewer
docs/                     spec, protocol, constraints
tasks/                    ordered build prompts
```

## How the digital twin works

There is no separate simulator. Duplicating animation logic in JavaScript produces two codebases that drift, and the JS version will happily do things that do not fit in 2 KB of SRAM.

Instead the real Uno runs the real firmware and streams its framebuffer back up the USB cable. The browser draws what the board actually computed, and sends real protocol bytes back down.

```
Browser  --[0xAA packets, byte-identical to Teensy]-->  Uno
Browser  <--[0xAB framebuffer dump, 4fps]------------  Uno
```

Requirements: a bare Arduino Uno and a USB cable. No LEDs, no Teensy, no wiring.

When Max's Teensy replaces the USB connection, the Uno cannot tell the difference. Same bytes.

## Quickstart

```
1. Open firmware/tub.ino in Arduino IDE
2. Install FastLED via Library Manager
3. Upload to the Uno
4. Serve the twin:  cd twin && python3 -m http.server 8000
5. Open http://localhost:8000 in Chrome or Edge
6. Click Connect, pick the Arduino port
```

WebSerial requires Chrome or Edge. It does not work in Safari or Firefox.

## Bench simulation with the Teensy in the loop

Once the Teensy wire is attached (Teensy pin 1 TX1 -> Uno pin 0 RX, grounds bonded), the Teensy electrically owns the Uno's RX line and the browser can no longer send packets to the Uno directly. The twin handles this with a second WebSerial link:

```
Browser --['0'/'1' mode, 'e' id delta knob ticks]--> Teensy --[0xAA packets]--> Uno
Browser <--[0xAB framebuffer + status dumps]---------------------------------- Uno
```

Setup order:

1. Flash the Uno with the wire DETACHED from pin 0 (the bootloader needs that pin), then reattach.
2. Flash the Teensy (USB Type: Serial + MIDI).
3. Serve the twin, open it in Chrome, click Connect and pick the Uno's port. The strips stay dark until the Teensy tells the Uno its mode (2s heartbeat), then IDLE breathing fades in.
4. Click Connect Teensy and pick the Teensy's port.
5. Click Active. Mode clicks route through the Teensy, which transmits the real packets.
6. Spin dials or turn real encoders. With the Teensy link up, dial ticks also route through the Teensy ('e' command) and go through its real knob model, so MIDI and lights stay locked. Knobs only respond in ACTIVE: the firmware discards deltas in IDLE and during crossfades.

The Uno cannot tell simulated input from real input. Same bytes, same wire.

## Critical constraint, short version

The Uno has 2 KB of SRAM. One framebuffer of 318 LEDs costs 954 bytes, which is 47 percent of it. Never allocate a second framebuffer. See `docs/MEMORY.md`.

## Debug channel warning

On the Uno, hardware serial and USB share pins 0 and 1. Once the Teensy wire is attached, the serial monitor is unavailable. Do not build a debugging workflow that depends on `Serial.println`. Use the pin 13 LED or a section of a strip for status.

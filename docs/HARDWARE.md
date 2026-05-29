# Hardware Reference

The badge is a 100 mm × 100 mm single-sided FR-4 PCB carrying an
ESP32-C3-based microcontroller, an NFC reader, two light sources, a
magnetic sensor, an ambient-light sensor, and a small set of indicator
LEDs. Power is provided either by USB Type-A (edge mount) or by two
CR2032 coin cells in parallel.

This document is a reference for the firmware. The authoritative
source is the project schematic:

📄 **[`BSides_Badge_2025_r2.0_Schematics.PDF`](BSides_Badge_2025_r2.0_Schematics.PDF)**
(5 pages: top sheet, MCU, power, sensors/actuators, RFID reader & antenna)

What follows is the firmware-side summary — what the code can see and
which GPIO each device is wired to.

---

## Components on the board

| Designator | Part                       | Purpose                                            |
|-----------:|----------------------------|----------------------------------------------------|
| U1         | ESP32-C3FH4                | RISC-V MCU + WiFi/BT radio, 4 MB flash             |
| U2         | TPS62173 (3.3 V regulator) | Main 3V3 rail for MCU, sensors, NFC                |
| U3         | MGH201A1T3 Hall switch     | **Bipolar latching** magnetic sensor (active-LOW)  |
| U4         | ALS-PT19-315C              | Ambient-light photodiode (analog output)           |
| U7         | ST25R3911B                 | 13.56 MHz NFC reader (ISO-15693 / NFC-V)           |
| U8         | TPS7A2050 (5 V regulator)  | 5 V rail dedicated to the RGB LEDs                 |
| D11–D15    | SK6812MINI-HS              | 5 × addressable RGB "NeoPixel"-style LEDs          |
| D3–D9      | various LEDs               | 6 × charlieplexed indicator LEDs                   |
| S1, S2     | TS-1088-AR02016            | BTN1, BTN2 tactile pushbuttons                     |
| B1, B2     | CR2032 holders             | Optional battery power (cells in parallel)         |
| P2         | USB Type-A edge connector  | Power + serial/JTAG                                |

---

## Power

There are three rails:

- **VIN** — the higher-voltage input. Comes either from USB VBUS (~5 V
  via Schottky diode D2) or from the two CR2032 holders (~3 V).
- **VDD3V3** — 3.3 V from the TPS62173 step-down regulator. Powers the
  MCU, the NFC reader, the Hall sensor, the ALS, and the button
  pull-ups.
- **5V0** — 5 V from the TPS7A2050. Powers only the SK6812 RGB LED
  chain. Generated only when USB power is present.

> **Coin-cell caveat.** Two CR2032s in parallel can drive the MCU + Hall
> + ALS comfortably, but they cannot sustain the inrush of bringing up
> the ST25R3911B's analog front-end. The firmware exposes a
> `BADGE_LOW_POWER` switch (see [`BUILDING.md`](BUILDING.md)) that skips
> NFC init and drops the CPU clock to 80 MHz so the badge can boot from
> coin cells alone — useful only for off-USB testing.

---

## GPIO pin map (ESP32-C3)

This is the canonical map. The firmware never hard-codes a GPIO number;
every assignment lives in [`main/project_pins.h`](../main/project_pins.h).

| GPIO | Direction | Net name        | Purpose                                              |
|-----:|-----------|-----------------|------------------------------------------------------|
| 0    | input (PU)| `MCU_ADC_HALL1` | MGH201A1T3 OUT (active-LOW open drain)               |
| 1    | input     | `MCU_ADC_LUX`   | ALS-PT19 output (analog read for level, digital for sleep wake) |
| 2    | input     | `MCU_BTN2`      | BTN2 (RTC-capable → can wake from light sleep)       |
| 3    | input     | `MCU_IRQ`       | ST25R3911B IRQ                                       |
| 8    | output    | `MCU_RGB_LED`   | Single GPIO drives the WS2812B chain                 |
| 9    | input     | `MCU_BTN1`      | BTN1 / also the ESP32-C3 boot-mode strap pin         |
| 10   | output    | `MCU_LED_X2`    | Charlieplex pin 2                                    |
| 20   | output    | `MCU_LED_X3`    | Charlieplex pin 3                                    |
| 21   | output    | `MCU_LED_X1`    | Charlieplex pin 1                                    |
| —    | SPI       | MISO/MOSI/SCK/SS| ST25R3911B SPI bus (standard Arduino-ESP32 defaults) |

Pin choices were dictated by hardware constraints, not free choice:

- **BTN2 on GPIO 2** so it can wake the chip from light sleep. The
  ESP32-C3 only supports wake-from-sleep on RTC GPIOs 0..5.
- **BTN1 on GPIO 9** because GPIO 9 is the bootloader strap pin — a
  built-in physical means of forcing the ROM bootloader for flashing.
- **HALL on GPIO 0** also for RTC wake-capability, although the firmware
  does not currently wake on a magnet (the sensor is a latch — the line
  level after wake would be ambiguous).
- **NFC SPI** at Arduino defaults so no remapping is needed.

---

## LED layout

### Addressable RGB chain (5 × SK6812)

The LEDs are wired in a daisy chain in this order, viewed from the front
of the badge:

| Index | Logical name      | Notes                                                |
|------:|-------------------|------------------------------------------------------|
| 0     | `HALL_RGB`        | Used by firmware as the "awake" indicator (Green) and as the colour the Hall-hold puzzle paints |
| 1     | `Team_Colour1`    | Set by NFC team-colour tags                          |
| 2     | `LUX_RGB`         | Colour the Lux-cover puzzle paints                   |
| 3     | `Reserved`        | Used as the **master-unlock** indicator (BSides orange) once every attendance challenge is done |
| 4     | `Team_Colour2`    | Set by NFC team-colour tags (mirror of slot 1)       |

### Charlieplexed indicator LEDs (6 LEDs, 3 GPIOs)

| Index | Designator | Lights up when                                   |
|------:|-----------:|--------------------------------------------------|
| 0     | D3         | Presentation 1 attendance recorded               |
| 1     | D4         | Presentation 3 attendance recorded               |
| 2     | D5         | Malware Village attendance recorded              |
| 3     | D7         | Presentation 2 attendance recorded               |
| 4     | D8         | HALL_LED — follows the Hall sensor's latched state |
| 5     | D9         | LUX_LED — set when ambient light is above LUX_LIGHT_THRESHOLD |

A three-pin tri-state matrix (`X1`, `X2`, `X3`) drives the six LEDs. See
`CLED_TABLE` in `main.cpp` for the per-LED pin states.

---

## NFC

The ST25R3911B is a full-featured 13.56 MHz NFC reader. It is connected
to the MCU over SPI plus a single IRQ line. The badge firmware
configures it as a **reader-only** node (no card emulation, no
peer-to-peer) and polls for **NFC-V (ISO-15693)** tags only.

Tag content is an NDEF text record whose first byte is a command — see
[`NFC_TAGS.md`](NFC_TAGS.md).

The antenna is a 13.56 MHz spiral printed on the PCB (ANT1). The reader's
TX power is set to maximum at boot via direct register write (the chip
default is conservative).

---

## Buttons

Both buttons are wired in a classic pull-up + GND-to-press
configuration:

- **BTN1** (GPIO 9): short press triggers an NFC tag scan. Holding it at
  power-on forces ROM bootloader mode for flashing.
- **BTN2** (GPIO 2): short press registers as user activity (keeps the
  badge awake). Holding it for 2 s toggles the WiFi portal on/off.
  Holding it while plugging USB in performs a **hard reset** that wipes
  all NVS state.

---

## A note on the schematic revision

The current PCB revision is REV 2 (dated 2025-05-05). An earlier
prototype board placed BTN2 on GPIO 11 (which is not RTC-capable, so it
could not wake from sleep) and had a microphone on GPIO 2; production
removed the microphone and moved BTN2 to GPIO 2. The firmware contains
both pin definitions for historical reference but always builds for the
production board.

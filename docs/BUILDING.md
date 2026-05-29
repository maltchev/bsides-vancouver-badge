# Building and Flashing

A from-scratch setup guide for building the badge firmware on Windows,
macOS, or Linux. The procedure is the same on every OS; the only thing
that differs is how you install ESP-IDF.

---

## 1. Install ESP-IDF v5.4.1

The firmware is pinned to ESP-IDF **v5.4.1**. Newer versions may work but
are untested; older ones definitely will not.

### Windows
Use the [Espressif IDF Installer for Windows](https://dl.espressif.com/dl/esp-idf/).
Choose offline installer → version `v5.4.1` → target `esp32c3`. It will
install the toolchain, Python environment, and add an "ESP-IDF 5.4
PowerShell" shortcut to the Start menu.

### macOS / Linux
```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.4.1
cd esp-idf-v5.4.1
./install.sh esp32c3
```

Then in any shell where you want to build the badge:
```sh
. ~/esp/esp-idf-v5.4.1/export.sh
```

(On Windows, open the "ESP-IDF 5.4 PowerShell" shortcut instead — it does
the equivalent.)

You should now have `idf.py` on your `PATH`.

---

## 2. Clone the project

```sh
git clone https://github.com/maltchev/bsides-vancouver-badge.git
cd bsides-vancouver-badge
```

The `components/` directory contains vendored libraries (FastLED,
Arduino-ESP32, the ST NFC driver). These are committed in-tree so you do
not need to fetch them separately.

---

## 3. Configure and build

From the project root, with ESP-IDF activated:

```sh
idf.py set-target esp32c3
idf.py build
```

The first build takes 3–10 minutes (most of it compiling Arduino-ESP32 and
lwIP). Subsequent builds are incremental and finish in seconds.

If the build complains about missing components, run:
```sh
idf.py reconfigure
```

---

## 4. Flash the badge

### Find your COM port / device path

- **Windows**: Open Device Manager → "Ports (COM & LPT)" → look for "USB
  Serial Device". Typically `COM7`-ish.
- **macOS**: `ls /dev/cu.usbmodem*`
- **Linux**: `ls /dev/ttyACM*`

### Flash and monitor

```sh
idf.py -p <PORT> flash monitor
```

Examples:
```sh
idf.py -p COM7 flash monitor          # Windows
idf.py -p /dev/cu.usbmodem1101 flash monitor   # macOS
idf.py -p /dev/ttyACM0 flash monitor  # Linux
```

Exit the monitor with `Ctrl-]`.

### If `idf.py flash` cannot connect

The ESP32-C3 uses USB Serial/JTAG for both serial console and flashing.
If the chip has just been through a light-sleep cycle, esptool's
auto-reset can fail. Two workarounds:

1. **Power-cycle the badge**: unplug the USB cable and plug it back in.
   The chip enters a clean boot state and esptool's auto-reset works
   normally.
2. **Force the ROM bootloader**: hold **BTN1** while plugging USB in.
   GPIO 9 is the boot-mode strap on the ESP32-C3, and BTN1 is wired
   there. Release BTN1 once the USB enumeration sound has played, then
   run `idf.py flash`.

---

## 5. Configuration knobs

A few constants in [`main/project_pins.h`](../main/project_pins.h) tune
the firmware's behaviour without touching code:

| `#define`                  | Default | What it does |
|----------------------------|--------:|--------------|
| `INACTIVITY_TIMEOUT_S`     | 60      | Seconds with no activity before light sleep |
| `LUX_POCKET_SLEEP_MS`      | 60000   | ms of continuous darkness before "in-pocket" sleep |
| `HALL_HOLD_DURATION_MS`    | 2500    | ms a magnet must be held to fire the hold puzzle |
| `LUX_HOLD_DURATION_MS`     | 2500    | ms the lux sensor must be covered to fire its puzzle |
| `BADGE_DEBUG_NO_SLEEP`     | 0       | 1 = disable all sleep paths (USB monitoring mode) |
| `BADGE_LOW_POWER`          | 0       | 1 = skip NFC init + drop CPU to 80 MHz (battery-only test mode) |

The full list lives in `project_pins.h`. Rebuild after editing.

---

## 6. Verifying the build

After flashing, you should see something like the following over the
serial monitor:

```
W (xxx) Badge: CPU frequency: 160 MHz
W (xxx) Badge: Reset reason: 1 (POWERON)
...
W (xxx) Badge: Welcome to the BSides Badge!!!
W (xxx) Badge: Initialize successful
```

Press **BTN1** to start an NFC scan. Long-press **BTN2** to bring up the
WiFi portal. If those work, the badge is healthy.

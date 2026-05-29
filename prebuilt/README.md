# Pre-built firmware images

Ready-to-flash binaries for the BSides Vancouver badge. Use this if you
just want to restore the badge to a known-good state without setting up
ESP-IDF and building from source.

## What's here

| Folder              | What it contains                                            |
|---------------------|-------------------------------------------------------------|
| `2025-production/`  | The original firmware that shipped on the 2025 badges.      |

(More builds — the current `v2.4.x` workshop firmware, future revisions —
can be added in their own subfolders alongside `2025-production/`.)

---

## Prerequisites

You need [`esptool.py`](https://github.com/espressif/esptool) and a USB
cable to the badge. `esptool.py` comes bundled with ESP-IDF; you can
also install it standalone via pip:

```sh
pip install esptool
```

---

## Flashing — 2025 production firmware

From the `2025-production/` folder, on Linux / macOS / Windows:

```sh
esptool.py --chip esp32c3 -p <PORT> -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
    0x0     bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 BSides_Badge_2025.bin
```

Substitute `<PORT>`:

- Windows: `COM7` (look in Device Manager)
- macOS:   `/dev/cu.usbmodem*`
- Linux:   `/dev/ttyACM0`

If `esptool` fails to connect with "No serial data received", hold
**BTN1** on the badge while plugging the USB cable in (GPIO 9 is the
ESP32-C3 bootloader strap pin). Release after the COM port enumerates,
then retry.

---

## Verifying

After a successful flash, open a serial monitor at 115 200 baud:

```sh
python -m serial.tools.miniterm <PORT> 115200
```

You should see boot messages and (for the 2025 production firmware)
the original game-flow logging.

---

## Notes for the curious

The three files together cover the entire flash layout:

| Offset      | File                  | Region                                           |
|-------------|-----------------------|--------------------------------------------------|
| `0x0000`    | `bootloader.bin`      | Second-stage bootloader                          |
| `0x8000`    | `partition-table.bin` | Partition table (single-app layout)              |
| `0x10000`   | `BSides_Badge_2025.bin` | Application image (the firmware itself)        |

These offsets match the ESP-IDF default for `esp32c3` projects. They are
the same offsets used by the main firmware in this repository, so you
can mix-and-match (e.g. flash this bootloader + this partition table +
your own application binary).

# NFC Tag Command Reference

The badge reads **NFC-V (ISO-15693)** tags. When you press BTN1 it
energises the antenna for ~10 seconds, looking for any tag in range.
When it finds one, it decodes the first NDEF text record on the tag
and dispatches on the **first byte of the payload**.

This is the reference for what each command does and what its tag
payload should look like.

> **How to author tags.** Use NXP "TagWriter" (Android) or "NFC Tools"
> (iOS/Android). Pick *Text record*, language `en`, encoding UTF-8, and
> enter the payload exactly as shown below.

---

## Single-character commands

These commands have **no argument** — the payload is just the one
letter.

| Payload | Effect                                                                  |
|---------|-------------------------------------------------------------------------|
| `R`     | Team colour → **Red** (RGB[1] and RGB[4])                               |
| `G`     | Team colour → **Green**                                                 |
| `B`     | Team colour → **Blue**                                                  |
| `Y`     | Team colour → **Yellow**                                                |
| `P`     | Team colour → **Purple**                                                |
| `O`     | Team colour → **Orange**                                                |

The colour is saved to NVS and survives a reboot. Both team-colour LED
slots (the ones at positions 1 and 4 in the chain) are set together so
the badge has matching colours on both sides.

---

## Custom RGB colour — `C`

Payload: `C` followed by **nine decimal digits** encoding red, green,
and blue as three 3-digit numbers.

```
C r r r g g g b b b
```

| Field | Range  | Example                                |
|-------|--------|----------------------------------------|
| `rrr` | 000–255 | `C255000128` → R=255, G=0, B=128 (hot pink) |
| `ggg` | 000–255 | `C000255000` → G=255 only (pure green)      |
| `bbb` | 000–255 | `C100100255` → light blue-ish                |

Like the preset team colours, this is saved to NVS.

---

## Session attendance — `T`

Payload: `T` followed by a **9-byte session code**. The valid codes are
hard-coded in `main.cpp` (see the `PRESENTATION_CODES` array).

There are eight slots. The first match increments
`presentationCounter`, lights the corresponding charlieplex LED, and
records the attendance in NVS so re-scanning the same tag does not
double-count.

Once all eight sessions plus the Malware Village have been collected,
the **master-unlock indicator** lights on RGB[3] in BSides orange.

---

## Malware Village — `V`

Payload: `V` followed by a **9-byte unlock code** (also hard-coded in
`main.cpp` as `MALWARE_CODE`).

A correct scan lights the Malware Village LED and counts toward the
master unlock.

---

## Brightness — `L`

Payload: `L` followed by **three decimal digits** in 0–255.

```
L d d d
```

Examples:
- `L010` → very dim, default
- `L030` → comfortable indoor brightness
- `L255` → max (warning: bright, draws ~300 mA peak)

The value is clamped to 0..255 and persisted to NVS.

---

## Full badge wipe — `X`

Payload: `X` followed by a **9-byte secret unlock code**
(`BADGE_WIPE_CODE` in `main.cpp`).

Effect:
- All charlieplex LED states cleared.
- All RGB LED colours reset to black (off).
- Presentation counter and attendance tracker reset to zero.
- Brightness reset to default.

This is the "remote control" version of the BTN2-hold-at-startup hard
reset. It writes through to NVS immediately.

---

## Quick reference table

| First byte | Total payload | What it does                                |
|------------|---------------|---------------------------------------------|
| `R/G/B/Y/P/O` | 1 byte       | Set team colour to a preset                 |
| `C`        | 10 bytes      | Set custom RGB team colour (`Crrrgggbbb`)   |
| `T`        | 10 bytes      | Record session attendance                   |
| `V`        | 10 bytes      | Malware Village unlock                      |
| `L`        | 4 bytes       | Set brightness (`Lddd`)                     |
| `X`        | 10 bytes      | Wipe all badge state                        |

If the first byte isn't recognised, the badge logs `Unknown tag
command '<c>' — ignored` and does nothing.

---

## Adding your own command

In [`main.cpp`](../main/main.cpp), find `ndefReadTag()` and add a new
`case` to the switch. Use `BadgeState::` to persist anything that
should survive a reboot. See the `'L'` brightness handler for a
minimal example.

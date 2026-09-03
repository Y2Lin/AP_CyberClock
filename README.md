<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AP_CyberClock

A **cyberpunk-style clock** firmware for the FoloToy AI Passport (ESP32-C3), built on top of [folotoy/ai-passport](https://github.com/folotoy/ai-passport) (MIT License).

Supports **BLE** and **USB cable** time sync, with a dedicated time-sync page owning all Bluetooth activity (the clock face stays radio-free). No companion app required — a browser page or any BLE debug tool is enough.

![firmware](https://img.shields.io/badge/firmware-v10-00f0ff) ![chip](https://img.shields.io/badge/chip-ESP32--C3-ff2ec4) ![license](https://img.shields.io/badge/license-MIT-green)

## Navigation (v8+, unchanged in v10)

```
boot -> [TIME SYNC page]  --OK: go to clock (BT turns off)-->  [Clock face]
        (BLE only here;                                      OK long |
         stays after sync)                                          v
                                                                 [Menu]
                                                   1 Clock | 2 Brightness | 3 Time Sync
```

## Features

### Cyberpunk clock UI (240x320, v10 B1++ face)
- HUD layout: inset frame + top/bottom ruler scales + double-line corner brackets; header with model / hex stream / battery %; 12 spectrum bars; 10-segment battery + bottom hex stream
- Large `HH:MM` digits with **cyan/magenta split-shadow ghosting** + separate `SS` field and a vertical seconds progress bar
- Terminal line: `> UTC+8_` blinking cursor + **pixel heart** beating lub-dub every second + uptime
- **Glitch bursts** (~every 7 s): shadow separation widens, digits jitter/flicker, sliced offset bars; plus a glitch color bar that relocates every 3 s
- Date + weekday + timezone + low-battery warning in red
- **5 themes**: neon cyan-purple / **e-ink** (paper-white background, ink-black text, fully static) / teal / amber / matrix green
- **2 display modes**: full / minimal (glitch effects disabled in minimal)

### BLE time sync
- Device advertises as `CyberClock` (BLE Peripheral)
- Custom GATT service (`FFC0`): write a Unix timestamp to set the clock, timezone supported
- Status notification every 5 s while connected (JSON)
- Repeat pairing supported (stale bonds are cleared automatically); works with nRF Connect / LightBlue / Web Bluetooth

### USB cable time sync (v5+, no Bluetooth)
- Reuses the flashing USB cable via the ESP32-C3 USB Serial/JTAG port
- Works on **any page**
- Text command protocol: `PING` / `T <unix> <tz>` / `Q`
- Browser direct connect (Chrome/Edge Web Serial) or `usb-sync.py`
- DTR/RTS are held deasserted during the session, so closing the page no longer resets the device (the C3's built-in download-reset path reads those line transitions as a reset pulse; v9.1 fix)

### Bluetooth lives on the sync page (v8/v9)
- Entering the TIME SYNC page (boot first screen) starts advertising; leaving it stops Bluetooth entirely — the clock face never touches the radio
- The sync page shows `BT: ADVERTISING / BT: LINKED`
- **After a successful sync the device stays on the sync page** (v9: no auto-jump) — the connection is kept so the web page keeps receiving status; pressing OK goes to the clock face and Bluetooth turns off
- Re-sync any time: long-press OK -> menu -> `3 TIME SYNC`

### Brightness (v8)
- UP/DOWN adjusts backlight in 10% steps (10–100%) on the Brightness page
- Factory default 80% (v10.1); user adjustments persist to NVS and are restored on boot
- **Idle auto-dim** (v10.2): after 90 s without a key press the backlight drops to 20%; any key restores the configured level instantly (the stored value never changes)

### Power (v10.2)
- Default CPU clock lowered from 160 to 80 MHz, with DFS enabled: 80 MHz busy, 40 MHz idle
- Off the clock face the refresh tick relaxes from 100 ms to 250 ms, cutting idle wakeups
- No light sleep: the USB console and USB time sync must stay online
- Bluetooth belongs to the sync page only (see below); the clock face keeps the radio off

### Misc
- Time and timezone persisted to NVS and rewritten every 5 minutes while running, so a power cut costs at most 5 minutes of drift (v10.1)
- The device has no RTC backup battery: after any reset it always comes back unsynced and waits for a fresh BLE/USB calibration (v10.1, every reset source)
- LVGL thread safety: callbacks only set flags; UI refresh happens in the LVGL timer context

## Keys (per page, v8+)

| Page | Key | Action | Function |
|---|---|---|---|
| TIME SYNC (boot) | OK | short | Go to clock face (page stays after sync, no auto-jump) |
| | OK | long | Menu |
| Clock face | UP | short | Cycle theme (neon cyan-purple -> e-ink -> teal -> amber -> matrix green -> loop) |
| | DOWN | short | Toggle display mode (full <-> minimal) |
| | OK | long | Menu |
| Menu | UP / DOWN | short | Move selection (1 Clock / 2 Brightness / 3 Time Sync) |
| | OK | short | Enter selection; long = back to clock |
| Brightness | UP / DOWN | short | Backlight +10% / -10% (10–100%, saved instantly) |
| | OK | short/long | Back to menu |

## Quick start

### 1. Flash the firmware
Prebuilt binaries (v10) live in [`tools/cyber-clock-sync/firmware/`](tools/cyber-clock-sync/firmware/):
- Browser flash (no install): open the official flasher page — FoloToy's [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/) (vendor) or [esptool-js](https://espressif.github.io/esptool-js/) (Espressif) — in Chrome/Edge, connect the device, add the merged firmware with offset `0x0` and flash — direct download link: <https://y2lin.github.io/AP_CyberClock/firmware/FoloToy-AI-Passport-full.bin>
- Or esptool: `esptool --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin`

### 2. Set the time (pick one)
- **USB cable (easiest)**: open `index.html` (locally or online, see below), switch to the "USB" tab, connect, done
- **Phone Bluetooth**: connect to `CyberClock` with nRF Connect / LightBlue, write a timestamp to `FFC1`
- **Mac Bluetooth**: the BLE tab of `index.html`, or the `mac-sync.py` script

### 3. Online time-sync page (GitHub Pages)

The sync page is auto-deployed as the GitHub Pages root on every push to `main`:

- <https://y2lin.github.io/AP_CyberClock/>

HTTPS is required by Web Bluetooth / Web Serial, which GitHub Pages provides out of the box. The standalone file keeps working offline as well. Note: `github.io` may be unreachable from mainland China; use the local file in that case.

## Repository layout

```
main/                          Firmware sources
├── cyber_clock.c/h            Cyberpunk clock: 4-page state machine + v10 B1++ face (5 themes / glitch FX / pixel heart)
├── ble_time_sync.c/h          BLE GATT time-sync service
├── usb_time_sync.c/h          USB Serial/JTAG text-command time sync
├── time_manager.c/h           Time manager: NVS persistence, timezone, sync state
└── (demo_* / bsp_* are upstream files)
tools/
├── cyber-clock-sync/          Sync toolkit (also the Pages site root)
│   ├── index.html             Time-sync page (BLE + Web Serial), serves as the Pages home
│   ├── usb-sync.py            CLI sync script (pyserial)
│   ├── mac-sync.py            Mac system-Bluetooth sync script (bleak)
│   ├── firmware/              Prebuilt images (bootloader / partition table / app / merged)
│   └── *.md                   Docs in Simplified Chinese: flashing guide, feature & sync manual, build info
└── validate.sh and friends    Upstream build & check tooling
.github/workflows/pages.yml    Pages auto-deploy (deploys tools/cyber-clock-sync)
```

Docs inside `tools/cyber-clock-sync/` are written in Simplified Chinese — browse [the directory](tools/cyber-clock-sync/) on GitHub to read them.

## Build from source

```bash
git clone https://github.com/Y2Lin/AP_CyberClock.git
cd AP_CyberClock

# ESP-IDF v5.5.3
source ~/esp/v5.5.3/export.sh
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

- 8 MB flash, partition table in `partitions.csv`; the app partition limit is 3 MB
- Do **not** modify the `cardid@0x356000` and `recovery@0x700000` partitions (required by the mini-program BLE recovery)
- Clean-room build check: `./tools/validate.sh --firmware` (environment and SHA256 checksums in the build-info doc under `tools/cyber-clock-sync/`)

## Protocol cheat sheet

**BLE** (service `FFC0`, write `FFC1`, notify `FFC2`): write 6 little-endian bytes to `FFC1` = `uint32 Unix timestamp + int16 timezone hours`. Example `2026-09-01 00:00:00 UTC` + UTC+8 -> ts `1788220800` -> `80 15 96 6A 08 00`.

**USB** (serial text, newline-terminated):

| Send | Reply |
|---|---|
| `PING` | `PONG` |
| `T 1788220800 8` | `OK TS=1788220800 TZ=8` |
| `Q` | `{"ts":1788220800,"tz":28800,"synced":true}` |

## GitHub Pages deployment

`.github/workflows/pages.yml` deploys `tools/cyber-clock-sync/` (the time-sync page as site root, plus firmware binaries and docs) to GitHub Pages whenever those files change on `main`. It can also be triggered manually ("Run workflow"). One-time setup: enable Pages in repository Settings -> Pages with **Source: GitHub Actions**.

## Acknowledgments & license

- Built on [folotoy/ai-passport](https://github.com/folotoy/ai-passport); its full commit history is preserved as a tribute
- Both upstream and this project use the [MIT License](LICENSE)

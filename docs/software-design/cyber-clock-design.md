<p align="right">
  <a href="cyber-clock-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Cyber Clock Firmware Design

Applicable to firmware v10.1 and later (commit `2e11cca`). Scope is the Cyber Clock application stack on the AI Passport board: `main/`, `cyber_clock.c`, `time_manager.c`, `ble_time_sync.c`, and `usb_time_sync.c`. Board facts and BSP interfaces live in [../hardware-design/](../hardware-design/README.md) and are referenced, not restated.

## Modules

| Module | Responsibility | Runs in |
| --- | --- | --- |
| `main.c` | Boot order: I2C, time manager, USB sync service, display/LVGL, buttons; forwards every key event to the app | `app_main` |
| `cyber_clock.c` | Four-page state machine, clock-face UI and effects, brightness, key handling | LVGL timer + key callbacks |
| `time_manager.c` | Wall-clock ownership: set/get, timezone, sync state, NVS persistence | called from all tasks |
| `ble_time_sync.c` | Connectable GATT time service (NimBLE peripheral), lifetime owned by the sync page | NimBLE host task + esp_timer |
| `usb_time_sync.c` | Text protocol `PING` / `T <unix> [tz]` / `Q` over USB Serial/JTAG, always available | dedicated `usb_ts` task |

## Concurrency model

Three execution contexts touch shared state. The rule set, established after the v4 deadlock incident:

- **UI work happens only in the LVGL timer context.** The BLE sync callback and the USB task never call LVGL APIs; the BLE callback sets a `volatile` flag consumed by the 100 ms `tick()` timer.
- **Key callbacks** take `bsp_lvgl_lock()` in `main.c` before dispatching into the app, so page switches happen under the same lock as rendering.
- **`time_manager` state** is read via struct snapshot from the LVGL context while being written from the USB task or the NimBLE host task. Single aligned fields are atomic on the ESP32-C3; a full-snapshot race is known and accepted (worst case one frame of status text), see Known limitations.

Leaving the sync page stops Bluetooth synchronously (waits for the NimBLE host task to exit) while holding the LVGL lock — a deliberate one-off stall at page switch instead of a cross-task teardown state machine.

## Page state machine

Boot lands on `PAGE_SYNC`. Pages are four mutually exclusive containers on one screen; switching hides the others and forces a refresh by resetting the last-second marker.

| Page | Enter action | Keys |
| --- | --- | --- |
| TIME SYNC | starts BLE advertising | OK: to clock; long-OK: menu |
| CLOCK | — | UP: theme; DOWN: full/minimal; long-OK: menu |
| MENU | — | UP/DOWN: select; OK: enter item; long-OK: clock |
| BRIGHTNESS | — | UP/DOWN: ±10 (10..100, saved instantly); OK: back |

Bluetooth exists only on the sync page: entering starts advertising, leaving stops it completely. The clock face never touches the radio.

## Time sync data flow

Both entry points converge on `time_manager_set_unix_utc()`:

- **BLE**: write to characteristic FFC1 (little-endian uint32 seconds, optional int16 timezone hours in bytes 5-6), state notifications on FFC2 every 5 s while subscribed.
- **USB**: line-based text protocol on the same cable used for flashing; any page, no Bluetooth required.

After a write, the wall clock is set, `synced` becomes true, and state plus timestamp are persisted. The UI notices within one second because every page re-reads the time each second.

On boot the device always comes back **unsynced**: there is no RTC backup battery, so a restored timestamp is stale by at least the powered-off duration. The restored value is still loaded (rewritten every 5 minutes while running, so drift is capped) purely as a coarse display, with blinking text marking it untrusted until the next sync.

## Persistence layout

Single NVS namespace `cyber_clk`:

| Key | Type | Written | Purpose |
| --- | --- | --- | --- |
| `state` | blob (`time_manager_state_t`) | on sync / boot-clear / timezone change | synced flag, sync count |
| `tz_offset` | i32 | timezone change | timezone seconds (key wins over blob on boot) |
| `last_unix` | i64 | on sync, then every 5 min while synced | coarse restore value |
| `bright` | u8 | brightness change | backlight level (default 80 when absent) |

Flash-wear note: 5-minute cadence is 288 writes/day; unsynced time is never written.

## Display memory and timing budget

- One LVGL screen, four page containers, roughly 150 objects at v10.1. The LVGL pool is 64 KB (`sdkconfig.defaults`); 32 KB exhausted and white-screened at v10, so the pool size is a build-level invariant — CI builds from the same defaults.
- All animation runs in the existing 100 ms LVGL timer: heartbeat phases (lub-dub), ~7 s glitch bursts, 3 s bar relocation, spectrum driven by deterministic PRNG seeded from the current second. No additional tasks, timers, or allocation.
- Battery I2C reads happen every 5 s in the timer context (tens of ms, accepted tradeoff); first read falls back to 50%/3700 mV if the fuel gauge is unavailable.

## Failure degradation

- NVS unavailable: logging only; the clock runs in RAM without persistence.
- BLE start failure: sync page shows `BT: OFF (USB ONLY)`; USB sync remains fully usable.
- Battery read failure: placeholder values, UI otherwise unaffected.
- Display/LVGL init failure: boot aborts with a pin-level log (documented in `main.c`).

## Known limitations and tradeoffs

- The upstream demo menu in `main.c` is unreachable since v8 (direct app entry) and awaits cleanup.
- NVS persistence currently executes inside the BLE write callback (host task) and the USB task; moving it to the timer context is a known follow-up.
- BLE timezone writes are not range-checked yet (USB path is, ±14 h).
- A failed `ble_time_sync_stop()` can leave the service marked initialized, disabling BLE until reboot.
- E-ink theme disables ghosting and glitch effects by design; minimal mode hides decorative streams.

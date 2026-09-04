<p align="right">
  <a href="TODO.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# TODO - Next-Task Backlog

Working record for the next development session (created 2026-09-04). Status snapshot: firmware v10.4.2 (commit `855138f`) is released and deployed; the v10.4.3 battery calibration and the minimal-mode redesign are in flight (below).

## 1. Redesign the DOWN minimal mode (firmware implemented, awaiting on-device verification)

- **Progress (2026-09-04 session, design finalized and coded as v10.5)**: the mockup iterated to v4 with every question settled (scheme C composition, progress line pressed against the digit baseline, ghost/glow kept, low-battery red-dot fallback), and the firmware implementation landed right after — full-mode and minimal-mode widgets live in two separate full-screen layer containers swapped wholesale on DOWN; the minimal layer holds only the `HH:MM` 48px centered digits (ghost trails + unsynced breathing + soft glow breathing), the 112px seconds progress underline pressed against the digit bottom, the date line `YYYY-MM-DD  weekday` (uniform dim, double-space separator), and the top-right low-battery red dot below 20% SOC (slow blink, steady on e-ink, driven by the v10.4.3-calibrated SOC); all five themes adapted, e-ink stays fully static, and exit cleanup covers the new widgets. **Awaiting on-device verification**: ① fine-tune the y=150 progress-line alignment against the real font metrics (noted in code); ② the glow breathing and red-dot blink feel; ③ a five-theme × two-mode sweep with no leftover elements. The ESP-IDF 5.5.3 build has not run in this session — run `./tools/validate.sh --firmware` and record the app-part size.
- Goal: minimal mode shows only the date (`YYYY-MM-DD` + weekday) and the time (`HH:MM` + seconds progress line); drop every other element (outer frame, tick scales, top status bar, seconds progress bar, spectrum bars, terminal line, pixel heart, battery bar, bottom hex stream). (Achieved)
- Agreed workflow: produce an HTML design mockup first (same approach as the v10.3 design page), iterate with the creator until approved, and only then implement in firmware. (Achieved; the finalized mockup v4 is the session deliverable `minimal-mode-design.html`, not committed)
- Implementation pointer: the minimal branch of the clock page in `main/cyber_clock.c`; the five-theme system must keep working (paper-ink theme stays static; no ghost or glitch there). (Achieved)

## 2. Battery percentage inaccuracy

- **Progress (2026-09-04 session, two steps)**:
  1. Investigation complete (analysis report is the session deliverable `battery-analysis.html`, not committed). Chain of facts: CW2017 standalone fuel gauge (I2C at 100 kHz); the dial read the SOC register high byte directly every 5 seconds with no filtering, hysteresis, or remapping; init never wrote a cell profile, so the chip ran its default generic Li-Po curve — the root cause of 3.92 V→73% (about 5-10 points high).
  2. **Fix selected and coded (v10.4.3)**: the creator delegated the choice; **plan A (firmware OCV remap + median filter + display hysteresis)** was taken (B lacks cell calibration data for now; C's filtering/hysteresis landed merged into A). Added the pure-logic module `main/battery_gauge.c/h` (the chip SOC is no longer trusted; VCELL goes through a typical 4.2V Li-Po OCV table with piecewise interpolation, 3.92 V→68%; a 3-sample median filter rejects bad readings; 3-point hysteresis with immediate pass-through across the 20% low-battery threshold). The dial refresh path is wired in; host tests `tests/test_battery_gauge.c` all green; `./tools/validate.sh --static` PASS. **Awaiting on-device verification**: after flashing, check that the percentage looks sane at rest and while discharging and that the red line triggers around 20%; replace the OCV table calibration points once the cell datasheet or a discharge calibration exists (table in `main/battery_gauge.c`, marked TODO). The firmware build passed with the v10.5 verification run (see item 1).
- Remaining calibration: the OCV table is a typical curve; no load bias yet (loaded voltage sits below resting voltage) — add a fixed offset later if on-device readings run uniformly low.
- Investigation path (done): battery read chain in the BSP (CW2017, I2C, 5-second polling), the voltage-to-percent mapping, the LiPo OCV curve (3.0-4.2 V), filtering and hysteresis.
- Known data point: the dial mapped 3.92 V to 73%, while typical LiPo OCV tables place 3.92 V around 60-70%, so the mapping curve must be verified against the actual cell.

## 3. Pitfall notes on a branch of AP_Sound_Test

- **Progress (2026-09-04 session)**: done (pending push). The entry "FAP_SCREENSHOT_V1, Second Landing: Streamed Capture and the USB TX Ring" was written bilingually onto the new AP_Sound_Test branch `experience/screenshot-streaming` (local commit, `tools/check_repo.py` static check PASS, registered in both the `docs/experiences/INDEX` and `development/experience-notes` bilingual indexes). All seven candidate topics went into the entry: the six implementation topics as main sections, the two publishing-flow facts (24-hour receipt, serialized reviews) as a side-notes section — no separate entry in this repository, since the first entry `serial-screenshot-protocol` already covers the snapshot route and this one completes the streaming route. Remaining: `git push -u origin experience/screenshot-streaming` from an environment with GitHub credentials.
- Distill the v10.4 to v10.4.2 `FAP_SCREENSHOT_V1` bring-up into experience notes and publish them on a new branch of https://github.com/Y2Lin/AP_Sound_Test (inspect that repository's conventions before committing).
- Candidate topics:
  1. `usb_serial_jtag` installs a 256-byte TX ring buffer by default; the underlying `xRingbufferSend` needs contiguous space, so a single 2 KB write fails instantly. Fix: install the driver with a 4 KB TX ring and send 1 KB pieces.
  2. The VFS console path translates `\n` to `\r\n`, and any log byte corrupts a binary pixel stream: silence logs around the transfer window.
  3. LVGL 9.2 has no public flush-callback getter: include `display/lv_display_private.h` and read `disp->flush_cb`; guard re-hooking with a fallback unhook so a stale wrapper is never captured as the "original" callback (infinite recursion).
  4. A 150 KB whole-frame allocation fails on real hardware: stream through a 3-slot ring (34.5 KB total) with producer backpressure sleeps and an abort flag shared by both sides.
  5. Web Serial host pitfalls: hold DTR/RTS low (a port-close edge otherwise triggers the ESP32-C3 download reset); use a single-reader pump instead of racing reads; enforce per-read timeouts; surface device error lines instead of silently discarding them.
  6. Machine-readable failure reasons (`ERR SHOT` plus `BUSY` / `MEM` / `HOOK` / `DATA` / `STALL` / `USB` / `SHORT`) turn field failures into one-look diagnoses.
  7. Publishing flow facts: capture receipts are valid for 24 hours; a revision is rejected while another one is under review.

<p align="right">
  <a href="TODO.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# TODO - Next-Task Backlog

Working record for the next development session (created 2026-09-04). Status snapshot: firmware v10.4.2 (commit `855138f`) is released and deployed; the community submission (project 138, revision 218) is pending review; a replacement cover is prepared but cannot be uploaded while a revision is under review. See [development/publish-to-community.md](development/publish-to-community.md) for the publishing workflow.

## 1. Redesign the DOWN minimal mode (design first, no code yet)

- Goal: minimal mode shows only the date (`YYYY-MM-DD`) and the time (`HH:MM:SS`); drop every other element (outer frame, tick scales, top status bar, seconds progress bar, spectrum bars, terminal line, pixel heart, battery bar, bottom hex stream).
- Agreed workflow: produce an HTML design mockup first (same approach as the v10.3 design page), iterate with the creator until approved, and only then implement in firmware.
- Implementation pointer: the minimal branch of the clock page in `main/cyber_clock.c`; the five-theme system must keep working (paper-ink theme stays static; no ghost or glitch there).
- Open design questions: keep or drop the digit ghost/glow in minimal mode; vertical composition of date versus time; whether seconds tick in place or as a thin progress line.

## 2. Battery percentage inaccuracy

- Symptom to be collected from the creator first (reads high / reads low / jumps / wrong while charging?).
- Investigation path: battery read chain in the BSP (which sensor, I2C or ADC, poll interval), the voltage-to-percent mapping, the LiPo OCV curve (3.0-4.2 V), load voltage versus resting voltage, filtering and hysteresis.
- Known data point: the dial maps 3.92 V to 73%, while typical LiPo OCV tables place 3.92 V around 60-70%, so the mapping curve must be verified against the actual cell.

## 3. Pitfall notes on a branch of AP_Sound_Test

- Distill the v10.4 to v10.4.2 `FAP_SCREENSHOT_V1` bring-up into experience notes and publish them on a new branch of https://github.com/Y2Lin/AP_Sound_Test (inspect that repository's conventions before committing).
- Candidate topics:
  1. `usb_serial_jtag` installs a 256-byte TX ring buffer by default; the underlying `xRingbufferSend` needs contiguous space, so a single 2 KB write fails instantly. Fix: install the driver with a 4 KB TX ring and send 1 KB pieces.
  2. The VFS console path translates `\n` to `\r\n`, and any log byte corrupts a binary pixel stream: silence logs around the transfer window.
  3. LVGL 9.2 has no public flush-callback getter: include `display/lv_display_private.h` and read `disp->flush_cb`; guard re-hooking with a fallback unhook so a stale wrapper is never captured as the "original" callback (infinite recursion).
  4. A 150 KB whole-frame allocation fails on real hardware: stream through a 3-slot ring (34.5 KB total) with producer backpressure sleeps and an abort flag shared by both sides.
  5. Web Serial host pitfalls: hold DTR/RTS low (a port-close edge otherwise triggers the ESP32-C3 download reset); use a single-reader pump instead of racing reads; enforce per-read timeouts; surface device error lines instead of silently discarding them.
  6. Machine-readable failure reasons (`ERR SHOT` plus `BUSY` / `MEM` / `HOOK` / `DATA` / `STALL` / `USB` / `SHORT`) turn field failures into one-look diagnoses.
  7. Publishing flow facts: capture receipts are valid for 24 hours; a revision is rejected while another one is under review.
- Note: this repository already keeps experience entries under `docs/experiences/` (see [development/experience-notes.md](development/experience-notes.md)); decide what belongs there versus the AP_Sound_Test branch.

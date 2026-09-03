<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Firmware v10.3 (code-review follow-up, all five P3 and nine P4 findings fixed): `main.c` drops the unreachable demo menu and demo entry table — unreferenced demo pages and the Wi-Fi stack they pulled in are now linker-pruned, shrinking the app partition 1,547,216 -> 1,046,448 bytes (-32.4%; -39% cumulative vs v10.1); `time_manager` NVS writes moved out of the BLE/USB task callbacks into the LVGL timer context (dirty flags + `flush_pending()`, so flash erases can no longer stall the protocol-stack task); timezone writes gain a final ±14-hour guard (the BLE entry previously had none); `s_state` reads/writes are wrapped in a critical section, eliminating torn multi-field snapshots; a failed `ble_time_sync_stop()` now resets the initialized flag instead of leaving BLE dead until reboot; plus nine polish items (dead APIs, duplicate macros, misleading comments, fake default echo).
- Firmware v10.2 (power and size): default CPU clock 160 -> 80 MHz with DFS enabled (80 MHz busy / 40 MHz idle; SPI and I2C drivers hold PM locks automatically); new idle auto-dim (backlight drops to 20% after 90 s without a key press, any key restores instantly, stored level untouched); off-clock refresh tick relaxed from 100 ms to 250 ms; compiler optimization switched from debug (-Og) to size (-Os) and the unused 20px Montserrat font dropped — app partition shrinks 1,716,832 -> 1,547,216 bytes (-9.9%); the local `sdkconfig` is now regenerated wholesale from `sdkconfig.defaults`, so local and CI build configurations are identical from here on.
- Added the first software-design document: `docs/software-design/cyber-clock-design.md` (+ Chinese pair) covering the Cyber Clock application stack — module map, concurrency model, page state machine, time-sync data flow, NVS persistence layout, display memory budget, failure degradation, and known limitations as of firmware v10.1. Registered in the software-design index and `docs/INDEX.md`.
- Firmware v10.1 (display fixes + two low-priority asks): right-column labels (battery %, weekday, uptime, voltage) no longer collapse onto row 0 — `lv_obj_get_y()` returns placeholder coords before the first layout pass, so y is now passed explicitly; the small pixel heart had its y231 row missing, leaving a 1px transparent seam across the waist (the "incomplete heart + stray horizontal line"); factory default brightness is now 80 instead of 100; after any reset the device always comes back unsynced (the old monotonic-clock-rewind check failed on some wake paths, so a power cycle kept showing SYNCED with a frozen time), and the current time is now flushed to NVS every 5 minutes while running, capping power-loss drift at 5 minutes instead of the whole uptime; `sdkconfig.defaults` raises the LVGL pool from 32KB to 64KB (the v10 white-screen fix previously lived only in the gitignored local `sdkconfig`, so CI builds still white-screened). Prebuilt binaries and the source patch are updated to match.
- Made mini-program BLE install compatibility a template-level invariant: fixed
  protected `cardid`/Recovery partitions, retained the five-second UP-key
  Recovery boot hook, and added CI validation for merged-image structure,
  partition MD5/ranges, the 3 MB app limit, and protected payload exclusion.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.

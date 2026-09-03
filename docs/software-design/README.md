<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Software Design

This directory holds application-, component-, and system-level designs: module boundaries, interfaces, state machines, resources, persistence, concurrency, failure degradation, and test strategy.

Use one descriptive file or subdirectory per topic. State scope and applicable version or commit. Reference stable hardware documents instead of copying pin or board facts.

Registered designs:

- [cyber-clock-design.md](cyber-clock-design.md) — Cyber Clock application stack: modules, concurrency model, page state machine, time-sync data flow, persistence layout, display budget, and failure degradation (firmware v10.1+).

The authoritative AI entry point is [AGENTS.md](../../AGENTS.md); collaboration rules are under `docs/contribution/`, engineering rules under `docs/development/`, and fork workflow in `docs/fork-guide.md`.

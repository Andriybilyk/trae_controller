[OPEN] Debug Session: settings-entry-lag

Date: 2026-06-16
Scope: Slow entry into Settings screen on ESP32-S3

Symptoms:
- Entering `Settings` still feels slow even after lazy-loading content.
- Provided log shows `create_settings: done` quickly, but the UI still appears delayed.

Evidence Collected:
- `requestScreen: requested=5`
- `create_settings: enter` at ~49751
- `create_settings: done sel=0 section_open=0` at ~49781
- `loadScreenForce: screen=5 current=5` at ~49811
- No obvious long gap inside `create_screen_settings()`
- Delay remains observable after screen creation
- New timing shows `settings_nav: after_temp_load ... elapsed_ms=37`, confirming time is already spent before `Settings` itself is shown

Hypotheses:
1. Screen creation is fast, but first visible frame is delayed in LVGL/touch/input processing after navigation.
2. Some hidden Settings subtree still triggers layout/draw work after `create_settings: done`.
3. `lv_indev_reset(NULL, NULL)` on transition causes a transient input stall or race.
4. A periodic Settings tick/update path is doing work that current slow-log thresholds miss.
5. The lag is in global rendering/backend state after screen switch, not in Settings construction itself.

Interim Conclusion:
- Confirmed: the previous lazy-build change made `create_screen_settings()` fast.
- Confirmed: a measurable part of the delay is in the fresh-navigation pipeline (`temp screen -> delete_except -> async_target_load`) before `Settings` becomes active.
- Confirmed by runtime evidence: `ui_debug_settings_freeze_reportf(...)` inside the Settings navigation path blocks for about 18.2 seconds (`report_ms=18245`).
- Root cause: synchronous network debug reporting was still executed on the UI navigation path.
- Fix applied: removed the network debug report call from the Settings navigation completion path; keeping only local timing logs.

Plan:
1. Inspect navigation and screen load path around `requestScreen` / `loadScreenForce`.
2. Add minimal instrumentation only around post-navigation and first-settings-frame timing.
3. Rebuild, flash, reproduce, and compare timestamps.

Status:
- Session opened
- Awaiting targeted instrumentation

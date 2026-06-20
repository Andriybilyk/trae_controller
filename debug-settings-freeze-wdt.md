# Debug Session: settings-freeze-wdt

Status: OPEN

Symptom:
- Controller freezes when pressing the settings icon.
- Task watchdog triggers on `ui` task on CPU1.
- Provided backtrace points into LVGL style/widget code during settings entry.

Scope:
- Repro happens on device after recent firmware changes.
- Regression window appears to be within the last 1-2 flashes.

Initial Evidence:
- WDT log mentions `ui` task only.
- Stack includes LVGL widget/style functions and app code paths around UI/program/settings updates.

Hypotheses:
1. Settings screen creation triggers a heavy synchronous config/programs save on the UI task, blocking long enough to hit WDT.
2. Entering settings causes recursive LVGL object updates/layout recalculation, especially roller/scale/widget option updates, leading to a UI-thread stall.
3. A settings-entry callback now calls program info refresh code that touches LittleFS/JSON on the UI thread and regressed recently.
4. There is memory corruption or invalid object reuse in `screens.c` around settings construction, causing LVGL to spin during style/layout reads.
5. The freeze is caused by an unexpected cross-call from settings initialization into unrelated program/model sync code rather than the settings widgets themselves.

Planned Observations:
- Trace exact path from settings button click to freeze point.
- Measure elapsed time around settings screen build/update functions.
- Check whether config/program save or filesystem access runs during settings entry.
- Check whether LVGL update/tick code re-enters settings build unexpectedly.

Evidence Collected:
- Runtime serial log shows `requestScreen: requested=5` followed by `ui_tick: consume pending screen=5`, so navigation request itself succeeds.
- WDT backtrace lands in `ui_programs_persist_save_impl()` with LittleFS/cJSON frames, not in `create_screen_settings()`.
- The same backtrace also includes `programs_update_info_from_model()`, confirming a programs/model sync path is unexpectedly active during settings entry.
- LVGL frames (`lv_roller`, `lv_scale`) appear above the save path, consistent with a stale release/commit path from the previous screen during navigation.

Hypothesis Status:
1. Confirmed: synchronous program/config save can run on the UI task during settings entry.
2. Rejected as primary cause: LVGL layout code is present in the stack, but only as part of the event path leading into the save.
3. Confirmed: a settings-entry side path reaches `programs_update_info_from_model()` and touches LittleFS/JSON on the UI thread.
4. No supporting evidence so far.
5. Confirmed: freeze is caused by an unexpected cross-call from navigation into program persistence, not by the settings cards themselves.

Fix Applied:
- Deferred `ui_programs_persist_save()` whenever a screen transition is active or a screen request is pending.
- Added a single deferred save flush in `ui_tick()` after navigation fully completes.

Next Step:
- Build, flash, reproduce the settings tap again, and compare post-fix behavior against the original WDT symptom.

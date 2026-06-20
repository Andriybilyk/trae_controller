[OPEN] settings-errors-wdt

## Symptom
- WDT in `ui` / `swdraw` after entering `Settings -> Errors`
- Latest logs show `errors_load_log: parsed` but do not reach `rows_ready/end`
- Backtrace points into LVGL draw/screen-load path while `errors_load_async` is active

## Repro
1. Open `Settings`
2. Tap `Errors`
3. Wait until UI stalls and WDT triggers

## Hypotheses
1. `errors_load_log` stalls while creating/updating rows and triggers heavy LVGL redraw before it can log `rows_ready`
2. A screen-scale/layout call on the settings screen runs during `Errors` async load and amplifies redraw cost enough to starve the UI task
3. A stale keyboard/programs/nav path is still firing during `Errors` navigation and invalidates extra areas, causing unrelated LVGL work
4. The WDT happens after file parse completes but before the async callback returns, meaning the hot path is UI object mutation, not filesystem or JSON parsing

## Evidence
- User log 2026-06-18: `errors_load_log: begin -> file -> parsed`, then no `rows_ready/end`
- User log 2026-06-18: backtrace enters LVGL display/draw path during settings screen work
- User log 2026-06-18: latest backtrace still fires on `ui` task and resolves through LVGL draw/display code while `errors_load_async` is active
- User log 2026-06-18: latest stack also symbolicates unrelated widget paths, so we still need checkpoints inside the `Errors` row create/update loop to separate root cause from redraw noise
- User log 2026-06-18 (post-instrumentation): stalls during `errors_load_log: create_loop slot=2` after `errors_row_create: enter slot=2` (no matching `errors_row_create: done`), then WDT in `ui`
- User log 2026-06-18 (after row-create mitigation): `errors_load_log` completes through `errors_load_async: done`, then WDT reproduces only after `back_cb -> show_home(true) -> calib_build_async`
- User log 2026-06-18 (after removing hidden prebuild): WDT still reproduces on explicit `Calibration` open; checkpoints stop after `calib_build_async: enter` for stage 2, before `calib_build: stage=pid_high done`
- User log 2026-06-18 (after tick-driven split like Program Edit steps list): `Calibration` now advances through `pid_high_kd`, then stalls at `calib_build_tick: stage=7`, before `stage=ssr done`
- User log 2026-06-18 (after splitting SSR/Autotune cards): `Calibration` advances through `stage=ssr_shell done`, then stalls at `calib_build_tick: stage=8`, before `stage=ssr_btn done`
- User log 2026-06-18 (latest): crash class changed from WDT to `StoreProhibited` exactly at `calib_build_tick: stage=8`; last checkpoint is `stage=ssr_shell done`, then panic while entering `SSR` button shell creation
- User log 2026-06-18 (follow-up): reproduced as WDT instead of immediate panic, but stack still centers on `lv_roller -> release_handler -> lv_screen_load_anim -> lv_indev_reset` immediately after `calib_build_tick: stage=8`

## Latest Checkpoints (2026-06-18)
- `errors_load_log: before_create_loop count=10 rows_created=0`
- `errors_row_create: done slot=0`
- `errors_row_create: done slot=1`
- `errors_row_create: enter slot=2` → stall → WDT

## Current Conclusion
- Confirmed hot checkpoint: row creation path, not FS tail-read nor parse loop
- Next fix attempt: reduce layout/refresh work while constructing each row (hide + ignore-layout during build, feed/yield within row creation)
- `Errors` hot path is now mitigated enough to complete; the remaining WDT shifts to hidden `Calibration` prebuild started from `show_home(true)` after leaving `Errors`
- Minimal fix candidate: stop background calibration prebuild on home return and keep calibration build only on explicit entry into section 5
- Remaining hot checkpoint: `Calibration` `pid_high` stage object creation/layout, likely amplified by nested card/row/button layout work
- Current fix: construct calibration cards and PID rows under `HIDDEN | IGNORE_LAYOUT`, then reveal after children are fully created
- Updated hot checkpoint: `Calibration` SSR card build is still too heavy as one stage, even after moving the overall flow into one-stage-per-tick
- Current fix: split `SSR` and `Autotune` into additional tick stages so the whole section follows the same fine-grained opening pattern as Program Edit step rows
- Refined hot checkpoint: even the `SSR` button stage is still too heavy when done in one tick
- Current fix: split `SSR` button into `shell -> style/event -> label/reveal` and split `Autotune` button into `shell -> label/reveal`
- Latest evidence suggests this is no longer a pure "heavy stage" issue; the first interactive `SSR` button creation may be colliding with stale LVGL indev/focus state
- Next instrumentation target: isolate whether panic happens on `lv_btn_create`, `lv_obj_align`, or immediately after button creation because a stale active indev/object is still live
- Minimal fix based on evidence: insert an explicit `input barrier` tick before creating the first `SSR` button (`stop_processing + wait_release + reset`) so stale release/focus state is drained before any new interactive object appears
- Follow-up evidence: `ssr_input_barrier` succeeds and `ssr_btn_shell after_create` is reached with `indev=0x0`, so the remaining hot call is after creation, before size/align finish
- Current fix: replace specialized `lv_btn_create()` with plain `lv_obj_create()` for `SSR`/`Autotune` action surfaces and enable `CLICKABLE` only after geometry/style setup
- Latest evidence: `ssr_btn_shell` now completes through `after_size/after_align/done`; the next stall is `stage=10`, i.e. when the object is being turned interactive
- Current fix: keep `SSR`/`Autotune` action surfaces non-interactive during staged build and attach `CLICKABLE` + event callbacks only after `calib_build_tick: done`
- New evidence: the next repro stalls even earlier, at `stage=9` before `ssr_btn_shell after_create`, so the remaining risky operation is creating a dedicated child action surface inside the `SSR` card
- Current fix: remove dedicated child action-surface creation entirely and use the card itself as the clickable target, with a separate label rendered directly inside the card
- Latest evidence: a new panic can happen immediately after `errors_load_async: done`; `errors_load_log` fully completes, so the hot path has shifted again from row construction to the final reveal/redraw of the `Errors` list
- Next instrumentation target: checkpoints immediately before and after clearing `LV_OBJ_FLAG_HIDDEN` on the errors log list, plus root/list hidden-state at async exit

## Plan
1. Add instrumentation around row creation/update and async exit path
2. Reproduce and compare which checkpoint is last reached
3. Only after evidence confirms hot path, apply minimal fix
4. Keep business logic unchanged until runtime evidence isolates the exact failing checkpoint

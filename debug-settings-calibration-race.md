[OPEN] settings-calibration-race

## Symptom
- Opening `Settings -> Calibration` still triggers `task_wdt` in `ui`.
- Latest evidence shows `Calibration` async reaches `after_ssr`, while the backtrace still points into old UI paths such as `create_screen_programs`, `lv_chart`, `lv_bar`, `buttonmatrix`, and `keyboard_open_for_label`.

## Current Evidence
- `calib_build_async: after_ssr ...` is present, so the narrowed SSR build path is no longer the primary blocker.
- `programs_list=0x0 programs=0x0 program_graph=0x0` at `calib_build_async: enter`, so the stale root-pointer hypothesis is weaker.
- The runtime still wedges later with stacks rooted at:
  - `create_screen_programs`
  - `lv_chart_*`
  - `lv_bar_*`
  - `keyboard_open_for_label`

## Falsifiable Hypotheses
1. A hidden or transitioning `programs` screen is still being recreated while `Settings -> Calibration` opens.
2. A stale indev/group/focus path is dispatching input into `programs` widgets after the screen transition.
3. A delayed animation/timer tied to `programs` survives screen deletion and re-enters draw/layout code.
4. `keyboard_open_for_label` or related overlay logic is still targeting an object in the old `programs` tree during the transition.
5. The `program_graph` / chart subtree is not referenced by root pointers anymore, but is still alive via LVGL internal callbacks/events.

## Plan
1. Instrument `create_screen_programs` entry/exit and key object creation checkpoints.
2. Instrument screen deletion / transition edges around `programs`, `programs_list`, and `program_graph`.
3. Reproduce and compare which old-screen path still fires after `Calibration` has already started building.

## Status
- Session opened.
- No business-logic fix in this session yet.
- Next code change must be instrumentation only.

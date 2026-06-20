[OPEN] settings-calibration-wdt

## Symptom
- `Settings -> Calibration` still triggers `task_wdt`.
- Latest stacks no longer point to calibration staged build itself.
- Backtrace now repeatedly includes `create_screen_programs_list`, `lv_bar`, `lv_chart`, and LVGL draw/scroll paths while entering `Settings`.

## Repro
1. Open `Settings`.
2. Open `Calibration`.
3. Observe WDT after `calib_build_async: enter`.

## Current Hypotheses
1. The crash is inside the SSR hint sub-block of `ui_settings_build_kiln_calibration_async()`, because logs now reach `ssr_after_title` but not `ssr_after_hint`.
2. The likely failing call is one of the hint creation/configuration calls: `lv_label_create(card)`, `lv_obj_set_width`, `lv_label_set_long_mode`, style setters, or `lv_label_set_text`.
3. The previous `programs_list` leak hypothesis remains weakened, because `programs_list=0x0 programs=0x0 program_graph=0x0` at `calib_build_async: enter`.
4. The backtrace still includes unrelated-looking draw/program symbols, but the direct runtime checkpoints show the actual failure window is narrower: inside SSR hint creation.
5. The next evidence gate is to split the hint sub-block into even finer checkpoints and locate the first missing one.

## Evidence
- `calib_build_async: enter ... programs_list=0x0 programs=0x0 program_graph=0x0 ...` proves the old screen pointers are already cleared when `Calibration` async starts.
- The build now reaches:
  - `after_clear`
  - `after_offset`
  - `after_pid_low`
  - `after_pid_high`
  - `ssr_enter`
  - `ssr_after_card`
  - `ssr_after_title`
- It does **not** reach `ssr_after_hint`.
- Therefore the current failing window is the SSR hint creation/configuration block.
- One later user log showed a panic earlier (`after_pid_low`) together with `Checksum mismatch between flashed and built applications`, so that log is treated as stale / from a different firmware image and is not used to invalidate the latest SSR-hint-removal fix.

## Next Step
- Add finer instrumentation inside the SSR hint sub-block of `ui_settings_build_kiln_calibration_async()`:
  - `ssr_hint_after_create`
  - `ssr_hint_after_pos`
  - `ssr_hint_after_width`
  - `ssr_hint_after_long_mode`
  - `ssr_hint_after_style`
  - `ssr_hint_after_text`
- Reflash the latest built image and collect a fresh post-fix log before drawing any new conclusions.

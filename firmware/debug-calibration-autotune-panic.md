[OPEN] calibration-autotune-panic

## Symptom
- `Settings -> Calibration` now passes all `SSR` stages and panics at `calib_build_tick: stage=15`
- Latest panic is `StoreProhibited` after `ssr_btn_label done`, before any `autotune_* done` checkpoint
- Backtrace still symbolicates through unrelated LVGL / keyboard / JSON / littlefs paths, so the last reached calibration checkpoint is the reliable evidence

## Repro
1. Open `Settings`
2. Open `Calibration`
3. Wait until staged build reaches `stage=15`
4. Observe panic before `autotune_shell done`

## Falsifiable Hypotheses
1. `autotune_shell` is still too heavy as one tick because it creates the card plus title and hint in the same stage
2. The first `lv_label_create()` inside `autotune_shell` is the actual hot call, similar to the earlier `SSR` path
3. The `autotune` card shell itself is safe, and the crash is triggered only by the child label creation/layout work
4. The panic stack remains redraw-symbolization noise, while the true failing checkpoint is the first object mutation inside `stage=15`
5. If `autotune_shell` is split into `shell -> title -> hint`, the next log will move the failure to one of those new checkpoints or pass into `autotune_status`

## Latest Evidence
- User log 2026-06-18: `ssr_shell`, `ssr_title`, `ssr_hint`, `ssr_status`, `ssr_input_barrier`, `ssr_btn_shell`, `ssr_btn_style`, `ssr_btn_label` all complete
- User log 2026-06-18: panic happens immediately after `calib_build_tick: stage=15`
- User log 2026-06-18: no `autotune_shell done` appears before panic

## Current Plan
1. Confirm current `autotune` stage map in code
2. Add or verify narrow checkpoints inside `autotune_shell`
3. Reproduce and compare which `autotune_*` checkpoint is last reached
4. Only then apply or validate the minimal staged-build fix

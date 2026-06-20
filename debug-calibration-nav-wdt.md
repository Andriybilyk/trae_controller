# [OPEN] calibration-nav-wdt

## Symptom
- Перехід зі сторінки помилок на сторінку калібрування інколи завершується `task_wdt`, зависає `ui` task.
- Наданий backtrace вказує на важкий шлях через `ui_s3_post_scale()` і LVGL redraw/theme/style code.

## Repro
1. Зайти на сторінку помилки.
2. Спробувати перейти на сторінку калібрування.
3. Отримати `task_wdt` у `ui (CPU 1)`.

## Hypotheses
1. `ui_s3_post_scale()` на калібруванні виконує надто важкий синхронний прохід.
2. Відкриття клавіатури/overlay для calibration widgets запускає повторний layout/theme cascade.
3. Після екрана помилок не очищається transient/overlay/input state.
4. Навігаційний callback веде у некоректний target або подвійний screen transition.
5. WDT викликає redraw конкретного calibration widget, а не сама навігація.

## Evidence Plan
- Додати маркерні логи в navigation path `errors -> calibration`.
- Додати таймінги входу/виходу з `ui_s3_post_scale()` для calibration screen.
- Додати маркерні логи в callback відкриття transient/keyboard на calibration screen.

## Status
- Instrumentation build flashed and repro log collected.

## Evidence
- `UI_NAV_RACE: close_transient_overlays: nav=0 guard=0 kb_overlay=0x0 step_active=0`
- No `keyboard_open_for_label` log appears before freeze.
- `requestScreen: requested=5` at `25283`, but `ui_tick: consume pending screen=5` appears only at `43793` (~18.5s later).
- No `menu_click: done` / `post_scale:begin` logs for `SCREEN_ID_SETTINGS` appear before watchdog.

## Hypothesis Status
- H1 `ui_s3_post_scale()` itself is primary blocker: not supported by current evidence, because post-scale begin log is never reached.
- H2 keyboard/overlay cascade causes freeze: rejected by `kb_overlay=0`, `step_active=0`, and missing keyboard-open log.
- H3 stale transient from Errors screen: not supported by current evidence.
- H4 wrong callback/transition target: still possible but weaker; `requestScreen(SCREEN_ID_SETTINGS)` is correct.
- H5 blocking work happens earlier in Settings navigation path: confirmed strongest candidate.

## Confirmed Root Cause Candidate
- Legacy `ui_debug_settings_freeze_reportf(...)` calls remain in the `Settings` navigation/create path and can block the UI task before the screen fully enters local instrumentation points.

## Fix In Progress
- Remove blocking `ui_debug_settings_freeze_reportf(...)` calls from `action_goto_settings()`, `ui_load_screen_fresh()`, and `create_screen_settings()`.
- Keep lightweight local `ESP_LOGI(...)` instrumentation for post-fix verification.

## Additional Evidence
- After leaving Calibration and switching to Errors, watchdog no longer points to keyboard/overlay paths.
- New backtrace points to `lv_indev_scroll.c` (`find_snap_point_y`, `lv_indev_find_scroll_obj`) with CPU1 in `swdraw`.
- This indicates hidden section roots are still participating in LVGL scroll/input traversal.

## Refined Root Cause
- `s_settings_kiln_calib_root` and `s_settings_errors_root` remain `LV_OBJ_FLAG_SCROLLABLE` even while hidden.
- After switching sections, LVGL still traverses those hidden roots during scroll handling/draw, eventually stalling UI on redraw-heavy widgets.

## Current Fix
- Make `Calibration` and `Errors` roots scrollable only while the section is active.
- Reset their scroll position when hiding/showing to avoid stale scroll state.
- Keep autotune/SSR UI state caching fix to reduce redraw pressure on Calibration.

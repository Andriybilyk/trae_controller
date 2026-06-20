# [OPEN] programs-edit-memory

## Симптом
- Користувач хоче закрити питання цифрами: чи проблема `Programs/Edit` пов'язана з нестачею `internal heap` / `PSRAM`, чи з перевантаженням redraw у LVGL.
- Поточні runtime-логи вже показували `task_wdt` у `lv_draw_label`, але ще немає цільового зрізу `free internal / free psram / redraw pressure` саме під час входу в `Programs/Edit`.

## Гіпотези
1. На вході в `Programs/Edit` різко просідає `internal heap`, і UI впирається в внутрішню пам'ять, навіть якщо `PSRAM` ще вільна.
2. `PSRAM` вільна, але redraw зависає через CPU-навантаження від великої кількості label redraw, а не через алокації.
3. Під час `Programs/Edit` є каскадний relayout / repeated refresh, який штучно збільшує redraw pressure.
4. Віджетів/тексту стільки, що LVGL стабільно проводить занадто довгий цикл redraw навіть без реального memory pressure.
5. Є окремий memory-спайк у момент побудови або refresh списку кроків, який видно по `largest_free_block` / `free_internal`.

## План
- Додати лише інструментаційні логи для `free heap`, `free internal`, `free psram`, `largest internal`, `largest psram`.
- Залогувати точки входу в `create_screen_programs()`, `screen_programs_refresh_rows()` і `tick_screen_programs()`.
- Зібрати `monitor`-логи на реальному девайсі.
- Порівняти цифри пам'яті з backtrace/WDT і вирішити, чи проблема в RAM, чи в redraw/CPU.

## Прогрес
- У `src/ui/screens.c` додано instrumentation-only snapshot для `Programs/Edit` без зміни бізнес-логіки.
- Логи тепер виходять у `create:start`, `create:before_refresh`, `create:after_refresh`, `refresh:begin`, `refresh:end`, `tick:before_refresh`.
- Snapshot містить:
  - `heap_free`, `heap_min`
  - `free_internal`, `largest_internal`
  - `free_psram`, `largest_psram`
  - `steps_total`, `steps_built`, `first`, `scroll_y`, `list_h`, `list_children`
  - `visible_rows`, `apply_calls`, `refresh_calls`, `tick_refresh`
  - `label_updates_last`, `label_updates_total`
- Для наближеного виміру `redraw pressure` додано лічильник реальних `lv_label_set_text()` оновлень у `programs_steps_apply_scroll()`.
- Локальна build-перевірка в цьому середовищі не завершена, бо термінал запущений поза `ESP-IDF` shell:
  - `idf.py` відсутній у `PATH`
  - прямий виклик `.../tools/idf.py` падає на відсутньому python-модулі `click`

## Що перевірити в monitor
- Чи падає `free_internal` / `largest_internal` саме на `create:*` або `refresh:*`.
- Чи лишається `free_psram` великою, поки ростуть `tick_refresh` і `label_updates_total`.
- Чи видно цикл `tick:before_refresh` багато разів поспіль ще до взаємодії з екраном.

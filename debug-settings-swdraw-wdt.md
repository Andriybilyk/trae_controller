[OPEN] settings-swdraw-wdt

## Симптоми
- Після переходів між секціями Settings (особливо після відвідування інших сторінок і повернення) періодично спрацьовує `task_wdt`.
- У момент WDT CPU1 виконує `swdraw`.
- Backtrace містить LVGL `lv_anim_delete`, `lv_event_mark_deleted`, `lv_grid.c`, `lv_calendar_set_month_shown`.

## Очікувана поведінка
- Переходи між секціями Settings не мають приводити до зависання/`task_wdt`.

## Гіпотези (фальсифіковані)
- H1: Десь реально існує `lv_calendar` обʼєкт, який лишається живим і його апдейти/лейаут провокують довгий `swdraw`.
- H2: Під час переходів відбувається delete/mark_deleted обʼєктів або анімацій під час активного рендера, що заганяє `swdraw` у важкий/аномальний стан.
- H3: Важкий `ui_s3_post_scale(SCREEN_ID_SETTINGS)` або `lv_obj_update_layout(objects.settings)` запускається в невдалий момент і провокує дорогий redraw/лейаут.
- H4: Backtrace частково спотворений, а root cause — постійна інвалідація великої області (наприклад, opacity/alpha blend), яка час від часу затягує `swdraw` до WDT.

## План збору доказів
- Додати lightweight інструментацію:
  - таймінг `ui_s3_post_scale` і `lv_obj_update_layout(objects.settings)` (де викликаються),
  - детект/лічильник `lv_calendar` в дереві обʼєктів активного екрану (з лімітом по вузлах),
  - лог стану при `tick_slow` (sel, section_open, anim_count).
- Зняти один повний лог прогону, що приводить до WDT, і порівняти з прогоном без WDT.

## Поточні висновки
- H1 наразі не підтверджується: debug-скан дерева не знайшов `lv_calendar` (`cal_count=0`), тому стек із `lv_calendar_*` схоже побічний, а не прямий доказ наявності календаря в нашому UI.
- Guard для `Programs` keyboard path додано, але останній лог усе одно обривається на `errors_load_async: enter`, без `errors_load_async: done`.
- Це зсуває основний фокус на `ui_settings_errors_load_log()` як на актуальний блокуючий path.

## Наступний крок
- Додати stage-логи всередині `ui_settings_errors_load_log()`:
  - `begin`
  - `file path=... tail_len=...`
  - `parsed path=... count=...`
  - `rows_ready`
  - `end`
- За цими рядками визначити, чи зависання стається на читанні хвоста файлу, парсингу чи побудові rows.

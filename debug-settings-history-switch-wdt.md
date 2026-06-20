# [OPEN] settings-history-switch-wdt

## Симптом

- Після відкриття `Налаштування -> Історія` та подальших переходів між секціями (`Інфо`, `Помилки`, назад) інтерфейс через деякий час зависає.
- У логах спрацьовує `task_wdt`, а стек показує `CPU 1: swdraw` та шлях усередині LVGL `buttonmatrix`/`layout`/`anim`.

## Поточні спостереження

- `history_load_async` і `history_rebuild_list` завершуються успішно.
- Падіння відбувається пізніше, не в JSON-парсері.
- У backtrace повторюється шлях, пов'язаний із `lv_buttonmatrix_event`, а не з побудовою самих карток історії.
- Новий лог показує типовий сценарій: `history_rebuild_list: done` на ~7.9 с, після натискання `back` секція закривається штатно, а `task_wdt` спрацьовує лише на ~55.3 с.
- На момент WDT `CPU 1` виконує `swdraw`, а стек проходить через `lv_layout_apply`, `lv_anim_path_*`, `lv_buttonmatrix_event`, `invalidate_button_area`, що підсилює підозру на живий `keyboard/buttonmatrix` або інший overlay-об'єкт, а не на сам список `Історія`.
- Новий runtime-доказ (лог із `overlay_state`) показує, що `lv_layer_top()` порожній (`top_children=0`) під час переходів `History -> Back -> Info`, але `task_wdt` все одно відбувається в `lv_buttonmatrix_event`.
- Отже, проблема не виглядає як “завислий keyboard overlay на top-layer”; під підозрою `lv_layer_sys()` або об'єкт у дереві активного screen, який є `lv_buttonmatrix`-класом.
- Пізніші стеки перестали вказувати на `buttonmatrix` і стабільно змістилися в `anim_completed_handler` з дивним завершенням у `lv_calendar.c` (`highlight_update` / `get_day_of_week`), хоча в нашому UI календар не створюється взагалі.
- Це схоже не на реальний `calendar` у дереві UI, а на зіпсований або “stale” LVGL animation callback/state після переходів `History -> Back -> menu`.
- Плитки меню Settings були єдиними важливими `lv_btn`, які не проходили через `ui_flatten_button_style()`. Це вже виправлено як окремий мінімальний фікс, але WDT все ще відтворюється.
- Новий `anim_dump` показав 6 коротких анімацій (`dur=80`) у момент `close_transient_overlays`, і всі вони декодуються в шлях `lv_arc` (`inv_arc_area` / `lv_draw_sw_mask_*`), а не в `buttonmatrix`.
- Отже, root cause змістився ще точніше: під час навігації в Settings живе набір `arc`-подібних анімацій, джерело яких ми ще не локалізували до конкретного widget/parent.

## Фальсифіковані гіпотези

1. Після переходу в Settings або між секціями лишається активний `lv_keyboard`/`buttonmatrix`, який продовжує анімуватися і тримає `swdraw`.
2. У Settings використовується анімований перехід екрану/контенту, який не скасовується під час lazy-build `Історія`, тому `lv_anim` продовжує інвалідовувати області.
3. `Історія` після приховування все ще потрапляє в layout/reflow через один із контейнерів або дочірніх елементів і разом із темою LVGL створює довгу перерисовку.
4. Один із верхніх overlay/popup на `lv_layer_top()` не закривається повністю, хоча в логах вказівники вже `0x0`, і лишає внутрішній animated/buttonmatrix state.
5. Проблема не в `Історія` як такій, а у загальному toolbar/menu buttonmatrix Settings, який переобчислюється після переходів і зависає в draw path.
6. Після переходів у Settings лишається або псується `lv_anim_t` із `reverse_duration`, і під час `anim_completed_handler` він викликає вже невалідний callback / працює по звільненій пам'яті, що маскується під стек `lv_calendar`.

## Далі

- Перевірити, які саме `buttonmatrix`/keyboard/anim об'єкти живі під час відкриття `Історія` і після `back_cb`.
- Додати точкову інструментацію в місця створення/закриття Settings і keyboard/buttonmatrix state, не змінюючи бізнес-логіку.
- Окремо зібрати runtime-стан `lv_layer_top()`/overlay pointers після переходів `menu_item_cb -> set_section -> back_cb`, щоб підтвердити або спростувати гіпотези 1 і 4.
- Задампити сам список живих `lv_anim_t` у моменти `overlay_state`, включно з `var`, `exec_cb`, `completed_cb`, `reverse_duration`, `repeat_cnt`, щоб підтвердити або спростувати гіпотезу 6.
- Задампити для кожної живої анімації ще й `var_kind` та `var_parent`, щоб точно побачити, який саме LVGL-об'єкт є джерелом `arc`-анімацій.

## Додана інструментація

- У `src/ui/actions.c` додано `action_debug_dump_overlay_state(origin)`:
  - логує `lv_layer_top()`, кількість дочірніх елементів і `lv_anim_count_running()`;
  - дампить до 4 дочірніх об'єктів top-layer і до 4 вкладених дітей з типами `keyboard` / `buttonmatrix` / `textarea` / `button` / `label`.
- Виклики дампу додано в:
  - `keyboard_open_for_label()`
  - `keyboard_close()`
  - `wifi_kb_open()`
  - `wifi_kb_close_event()`
  - `action_close_transient_overlays()`
- У `src/ui/screens.c` дамп додано в:
  - `ui_settings_set_section()`
  - `ui_settings_show_home()`
  - `ui_settings_back_cb()`
- У `src/ui/actions.c` додано дамп перших живих анімацій `anim_dump` через `LV_GLOBAL_DEFAULT()->anim_state.anim_ll`, щоб бачити callback-адреси й reverse/repeat-поля до WDT.
- Додатково в `anim_dump` додано `var_kind` і `var_parent` для `anim->var`, щоб відрізнити `arc`/`bar`/`slider`/`button` і прив'язати анімацію до батьківського контейнера.

## Очікуваний доказ із наступного логу

- Якщо після `History -> Back` або `set_section` на `lv_layer_top()` лишається `keyboard`/`buttonmatrix`, підтвердиться гіпотеза про незакритий overlay/state.
- Якщо top-layer чистий, але `anims > 0`, треба буде шукати об'єкт з анімацією поза keyboard-overlay.
- Якщо і top-layer чистий, і `anims == 0`, root cause, ймовірно, у layout/invalidations всередині Settings tree, а не в overlay.
- Додатково: якщо `top_children=0`, але `sys_children>0` або `kb/bm>0` у сумарному скані дерев (top/sys/screen), тоді можна точно локалізувати, де живе buttonmatrix на момент зависання.

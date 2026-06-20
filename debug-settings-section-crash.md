[OPEN] settings-section-crash

# Симптом

- `Налаштування` вже відкривається.
- Під час переходу між внутрішніми секціями (`Калібрування печі`, ймовірно також `Інфо`) контролер падає з `Guru Meditation Error: LoadProhibited`.
- Останній лог користувача містить `Checksum mismatch between flashed and built applications`, тому поточний decoded backtrace може бути частково недостовірним.

# Гіпотези

1. Падає не сама секція `Калібрування`, а інший callback/секція, а через mismatch стек декодується хибно.
2. Під час кліку по меню `Settings` спрацьовує не той `idx`, який очікується, через layout/hitbox/event race.
3. Відкладений `lv_async_call` для `INFO` або `CALIBRATION` виконується вже після зміни стану екрана і працює з невалідним root/state.
4. Під час входу в секцію паралельно ще живе сторонній transient callback із попереднього екрана (`keyboard/programs`), і саме він тригерить падіння.
5. Crash відбувається вже після вибору секції, у першому `tick_screen_settings()` або `relayout`, а не в самому menu click handler.

# Поточний план

- Додати точкові маркери тільки в маршруті вибору секцій `Settings`.
- Підтвердити фактичний `idx`, порядок викликів і чи доходить виконання до async callback.
- Після цього зробити мінімальний fix тільки за підтвердженим шляхом.

# Поточний стан

- Додано інструментаційні `ESP_LOGI` маркери з тегом `UI_SET_SEC` у:
  - `ui_settings_menu_item_cb()`
  - `ui_settings_set_section()`
  - `ui_settings_show_home()`
  - `ui_settings_ensure_info_created_async()`
  - `ui_settings_build_kiln_calibration_async()`
- Логічну поведінку UI на цьому кроці не змінювали.
- `idf.py -B build_verify build` пройшов успішно.
- `idf.py -B build_verify -p COM9 flash` не виконався: `Wrong boot mode detected (0xa)`.
- Наступний крок: перевести плату в bootloader/download mode, повторити flash і зняти нові логи `UI_SET_SEC` під час входу в `Калібрування печі`.

# Нові докази

- Після успішної прошивки інструментаційного білда отримано логи:
  - `menu_click: idx=5`
  - `set_section: enter idx=5`
  - `set_section: queue calib_async`
  - `calib_async: enter`
  - `calib_async: done ... built=1`
- Це відкидає гіпотези про:
  - неправильний `idx` меню;
  - невалідний `calib_root` під час `lv_async_call`;
  - падіння всередині самого lazy/async build `Калібрування`.
- Найсильніша поточна гіпотеза: crash стається вже після `calib_async: done`, імовірно в одному з перших `tick_screen_settings()` для секції `Калібрування` або в суміжному callback після завершення переходу.
- Додано другий шар інструментації в перші тики `tick_screen_settings()` для `idx == 5`, але цей білд ще не прошито через повторний `Wrong boot mode detected (0xa)`.

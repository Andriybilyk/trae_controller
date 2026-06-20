[OPEN] settings-wdt-freeze

# Симптом

- При натисканні іконки `Налаштування` контролер зависає.
- Спрацьовує `task_wdt` для `ui (CPU 1)`.
- У логах стабільно є `create_settings: enter`, але немає `create_settings: done`.

# Відомі спостереження

- Навігація через `requestScreen()` і `ui_tick` спрацьовує.
- `delete_except` завершується для всіх екранів.
- Перед WDT у backtrace регулярно з'являються LVGL `roller/scale`.
- У стеку також з'являються побічні шляхи з `actions.c` та `ui.c`, пов'язані з:
  - `confirm_prog_cancel_cb`
  - `keyboard_open_for_label`
  - `wifi_setup_panel_cb`
  - `ui_programs_load_from_config_json`
  - `ui_programs_persist_load`

# Гіпотези

1. Під час переходу в `Settings` продовжує жити transient overlay або keyboard з попереднього екрана, і саме його cleanup/event loop блокує `ui`.
2. `create_screen_settings()` створює занадто важкий LVGL-дерево одним шматком, а WDT спрацьовує до завершення першого layout/draw.
3. Під час входу в `Settings` випадково тригериться callback з Wi-Fi setup або програмного редактора, який звертається до persistence/model і затягує UI task.
4. Проблема не в самому побудуванні `Settings`, а в першому `tick_screen_settings()` або `ui_settings_set_section()/ui_settings_show_home()`.
5. Один з LVGL-об'єктів попереднього екрана лишається валідним у pointer state і викликає повторний draw/layout під час переключення екрана.

# Докази

- Поки що підтверджено тільки те, що зависання відбувається після `create_settings: enter`.
- Точна точка всередині `create_screen_settings()` або суміжного callback ще не доведена остаточно.
- Після першого fix з `lv_async_call` для відкладеного `Settings` користувач підтвердив, що симптом лишився.
- Отже гіпотеза "єдина причина це гонка між `delete_async` і негайним створенням `Settings`" не підтвердилась як повне пояснення.

# Наступний крок

- Зібрати ще одну ітерацію доказів із внутрішніх маркерів навколо:
  - `ui_settings_set_section()`
  - `ui_settings_show_home()`
  - `tick_screen_settings()`
- Після цього або звузити до конкретної підфункції, або підтвердити, що реально блокує не `Settings`, а чужий callback під час переходу.
- Якщо новий лог знову обірветься до цих маркерів, поставити ще ранніші маркери по секціях початку `create_screen_settings()`.

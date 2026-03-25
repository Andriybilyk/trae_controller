# AGENT.md — операційна карта проєкту

## 1) Мета документа

- Це внутрішня карта firmware-проєкту для швидкого входу в контекст.
- Пріоритет: зрозуміти архітектуру, потік даних, точки збереження налаштувань, build/flash сценарії.
- Документ орієнтований на підтримку, діагностику та безпечне розширення функцій.

## 2) Високорівнева архітектура

- Платформа: ESP32-S3 + ESP-IDF.
- Ядро керування печі: `components/kiln_control/*`.
- Локальний екран: Slint UI (`slint_ui/*`) через C-bridge (`main/ui/slint_bridge.cpp`).
- Веб-інтерфейс: `data/*` (LittleFS), API/WebSocket у `main/net/*`.
- Віддалений доступ: MQTT remote у `main/net/remote_access.cpp`.

## 3) Структура проєкту (відсортовано по важливості)

### 3.1 Критичні entrypoint-и

- `main/app/app_main.cpp` — старт системи, задачі, ініціалізація NVS/LittleFS/Wi-Fi/RTC/Control/UI.
- `components/kiln_control/thermal_control.cpp` — PID, schedule runtime, autotune, safety/fault.
- `main/net/wifi_server.cpp` — реєстрація HTTP/WS маршрутів.
- `slint_ui/src/lib.rs` — головний цикл локального UI.

### 3.2 Домени коду

- `main/app/`
  - `app_main.cpp`
  - `device_commands.cpp`
- `main/net/`
  - `wifi_server.cpp`
  - `wifi_server_settings.cpp`
  - `wifi_server_pid.cpp`
  - `wifi_server_fan.cpp`
  - `wifi_server_history.cpp`
  - `wifi_server_remote.cpp`
  - `wifi_server_touch.cpp`
  - `wifi_server_ota.cpp`
  - `wifi_connection.cpp`
  - `remote_access.cpp`
  - `dns_server.cpp`
- `main/ui/`
  - `slint_bridge.cpp`
- `main/drivers/`
  - `display_driver.cpp`
  - `fan_driver.cpp`
  - `rtc_ds3231.cpp`
- `components/kiln_control/`
  - `thermal_control.cpp`
  - `include/kiln_control/thermal_control.h`
- `components/kiln_config/`
  - `config_store.cpp`
  - `fs_utils.cpp`
  - `include/kiln_config/config.h`
  - `include/kiln_config/config_store.h`
- `components/kiln_hal_heater/`
  - `heater_hal.cpp`
- `components/kiln_safety/`
  - `faults.cpp`
- `slint_ui/`
  - `src/lib.rs`
  - `ui/app.slint`
- `data/`
  - `index.html`
  - `bootstrap.js`
  - `assets/*`
  - `wifi_setup.html`
  - `touch_calibration.html`
  - `mqtt_gateway.html`

### 3.3 Технічні директорії, які не плутати з runtime

- `build/`, `slint_ui/target/`, `.component_cache/` — артефакти/кеші, не джерела функціоналу.
- `legacy/lvgl_ui/` — legacy код, не основний активний UI-шлях.

## 4) Runtime-потік

1. `app_main()` ініціалізує NVS, LittleFS, RTC, fan, thermal control, Wi-Fi.
2. Піднімаються задачі:
   - `ui_task` → `slint_ui_run()`
   - `control_task` → `thermalCtrl.loop()` + heartbeat/watchdog
   - `server_task` → `wifiServer.begin()/loop()` + `remote_access_loop()`
3. Веб і екран керують одним `thermalCtrl` через API/bridge.

## 5) Налаштування і персистентність

- Базовий конфіг зберігається у JSON через `kiln_config_save_json_config()` з дублюванням у NVS + LittleFS.
- Ключові runtime-поля:
  - `temp_offset_c`
  - `maxC`
  - `autotune_target_c` (синхронізується між Web і Slint)
  - PID tunings
- На boot: відновлення конфіг-файлу з NVS у LittleFS (якщо потрібно).

## 6) API-карта (основне)

- `GET/POST /api/settings` — загальні налаштування.
- `GET /api/pid`, `POST /api/pid/reset`.
- `POST /api/autotune`, `POST /api/autotune/stop`.
- `GET /api/status` + `/ws` для live-стану.
- `POST /api/start|stop|skip|addTemp|addTime`.
- Додатково: `/api/fan`, `/api/history`, `/api/remote`, `/api/touch/*`, `/api/ota/update`.

## 7) Важливі правила змін

- Для змін `data/*` обов’язково `flash_webui.bat`.
- Для firmware логіки — `flash_and_run.bat`.
- Не редагувати згенеровані `data/assets/*.js` як основне джерело логіки, якщо можна обійтись `bootstrap.js` або source UI.
- Якщо додається новий shared-параметр:
  1. зберігати у `thermal_control` + `persistRuntimeSettingsLocked()`
  2. віддавати/приймати через `/api/settings`
  3. підключати в Slint bridge і web UI.

## 8) Діагностика типових проблем

- Web зміни не видно: не прошитий LittleFS або кеш браузера.
- `409` на autotune: дивитись `error` причину (`fault_active`, `already_firing`, `autotune_active`, `sensor_unhealthy`).
- Несинхронність UI: перевірити, що значення проходить ланцюгом `Web/Slint -> /api/settings -> thermalCtrl -> /api/settings -> Slint/Web`.

## 9) Що покращити далі (професійний рівень)

### 9.1 Надійність і безпека

- Додати role-based доступ (viewer/operator/admin) до web API.
- Додати audit log критичних команд (start/stop/autotune/fault clear/settings). ✅ Реалізовано (`/littlefs/logs/audit.log`)
- Ввести rate-limit на критичні endpoint-и (`/api/start`, `/api/autotune`, `/api/ota/update`).

### 9.2 Якість керування

- Профілі PID по зонах температур (low/mid/high) з авто-вибором.
- Автоматичний preflight-check перед стартом випалу (сенсор, fault, fan, maxC, RTC час). ✅ Реалізовано
- Детальні метрики autotune (якість коливань, валідні цикли, confidence). ✅ Реалізовано

### 9.3 Продуктовість і сервіс

- Експорт/імпорт повного бекапу конфігів і програм в один файл. ✅ Реалізовано (`/api/backup`, `/api/backup/import`)
- Версіонування структури `config.json` з міграціями. ✅ Реалізовано (`schema_version=2`)
- Diagnostic bundle (`logs + settings + status snapshot`) для швидкого сапорту. ✅ Реалізовано (`/api/diagnostics/bundle`)

### 9.4 Інфраструктура розробки

- CI-пайплайн: build + статичні перевірки + базові API-тести.
- Контрактні тести JSON API (schema/compatibility).
- Автоматична перевірка, що web і firmware підтримують однакові ключі налаштувань.

## 10) Мінімальний operational checklist перед релізом

- Build firmware: OK.
- Build/flash LittleFS: OK.
- Перевірка `/api/status`, `/api/settings`, `/api/autotune`: OK.
- Перевірка синхронізації значень між Slint і web UI: OK.
- Smoke test fault/clear/reboot сценаріїв: OK.

## 11) Аналіз конкурентів і цільові фічі

### 11.1 Що ринок вже очікує

- **Bartlett Genesis + KilnAid**: стабільний mobile-first remote monitoring, мульти-піч, простий onboarding.
- **Orton AutoFire / Sentry клас**: зрозумілі профілі випалу, передбачувана поведінка, мінімум “магії”.
- **Комерційні kiln/cloud рішення**: історія сесій, графіки, сервісна діагностика, журнал критичних дій.

### 11.2 Де ми вже на рівні або вище

- Локальний екран + web UI + API + WS у єдиному runtime.
- Автоматичний preflight start-check і розширені autotune-метрики.
- Backup/import, diagnostic bundle, audit log у firmware без зовнішніх агентів.

### 11.3 Що дасть реальний “професійний” стрибок

- **Operator roles + PIN/2FA для критичних дій**: розмежування прав безпеки.
- **Timeline firings + аналітика деградації нагрівача**: прогноз обслуговування.
- **Remote approvals**: start/stop/autotune через підтвердження з таймаутом.
- **Calibration wizard**: покроковий майстер сенсора/offset/fan/touch з протоколом.
- **Fleet mode**: порівняння кількох печей, health-score, централізовані алерти.

### 11.4 Рекомендований порядок впровадження

1. RBAC + PIN для критичних API
2. Графіки/аналітика firing з quality KPI
3. Fleet dashboard + централізовані сповіщення
4. Напівавтоматичний сервісний майстер і predictive maintenance

## 12) Структурне дослідження готових рішень (сенсорні контролери муфельних печей)

### 12.1 Bartlett Genesis + KilnAid

- Сенсорний UI, до 30 програм, сегментні профілі, remote monitoring через KilnAid.
- Модель продукту: простий onboarding, мульти-піч, статуси/алерти, freemium mobile app.
- Джерела:
  - <https://www.bartlettinstrument.com/product/genesis-2-0>
  - <https://www.bartlettinstrument.com/kilnaid>

### 12.2 Orton AutoFire (Slide/4000/4X + RMS)

- Сильна сторона: стабільний PID-контроль і зріле програмування профілів.
- Remote історично більше про desktop RMS (аналіз temperature-time), ніж mobile-first remote control.
- Джерела:
  - <https://www.ortonceramic.com/autofire-controllers>
  - <https://www.ortonceramic.com/remote-monitoring-software>

### 12.3 Nabertherm Controller Series 500 + MyNabertherm

- Сенсорний контролер + app з фокусом на remote monitoring/control, графіки, fault visibility.
- Сильна сторона: екосистемність (app + controller + індустріальні сценарії).
- Джерело:
  - <https://nabertherm.com/en/mynabertherm-app>

### 12.4 TAP (SDS Industries) + Mobile App

- Mobile-first підхід: remote start/adjust, графіки, push, diagnostics і preventive maintenance alerts.
- Сильна сторона: app як центр операцій.
- Джерела:
  - <https://www.kilncontrol.com/>
  - <https://www.kilncontrol.com/tap-kiln-control-mobile/>

### 12.4.1 TAP II / TAP II Pro (додатково)

- Позиціювання: сенсорний контролер із фокусом на mobile remote control та “операції з телефона”.
- Практичні фічі, які виділяють TAP II:
  - remote start/abort/skip + редагування налаштувань і розкладів через app,
  - графічні firing logs і live indicators,
  - preventive maintenance alerts (ресурс relay/thermocouple/elements),
  - diagnostics/error reporting і push-сповіщення,
  - multi-zone сценарії в TAP II Pro.
- Продуктова модель: freemium/premium mobile-функцій (частина remote-функцій у платному плані).
- Висновок для нас: найсильніший референс не лише по PID, а по **сервісній екосистемі** (maintenance + alerts + mobile-first UX).
- Джерела:
  - <https://www.kilncontrol.com/controller/digital/tap-ii-controller/>
  - <https://www.kilncontrol.com/controller/digital/tap-ii-pro/>
  - <https://www.kilncontrol.com/support/faq/>

### 12.5 Висновок по ринку

- Ринок очікує не тільки “контролер + екран”, а повний стек:
  1. безпечний remote access,
  2. аналітика сесій,
  3. сервісні алерти/maintenance,
  4. робота з декількома печами.

## 13) Gap-аналіз нашого проєкту і що змінити далі

### 13.1 Що вже закрито

- Preflight-check старту (sensor/fault/fan/maxC/RTC).
- Autotune quality metrics (valid cycles/CV/quality/confidence).
- Backup/import + diagnostics bundle.
- Audit log критичних команд.

### 13.2 Ключові прогалини до “проф рівня”

- Відсутній RBAC/PIN-gating на критичних API.
- Немає повноцінного role/session шару для web.
- Немає fleet orchestration (мульти-піч dashboard + policy alerts).
- KPI є, але ще немає повного trends/dashboard (періодні порівняння, health score).
- Немає service wizard з протоколом перевірок та контрольними картками.

### 13.3 Точкові технічні зміни (куди вносити)

- **RBAC + PIN**
  - API-guard middleware в `main/net/wifi_server.cpp` (перевірка ролі + step-up PIN).
  - Сховище auth-конфігу в `components/kiln_config/config_store.cpp` (`auth` секція, hash+salt, policy).
  - UI в `data/*` + `slint_ui/*` для ролей і підтверджень.
- **Графіки/KPI dashboard**
  - Розширити `history` API агрегатами за період (доба/тиждень/місяць).
  - Додати endpoint типу `/api/analytics/kpi`.
  - Відмалювати тренди в web UI (без правки згенерованого `assets/*`, через source/bundle pipeline).
- **Fleet + централізовані сповіщення**
  - Використати `remote_access.cpp` як transport base (MQTT topics + signed commands).
  - Додати cloud/fleet-схему: device registry, last-seen, alert routing.
  - Нормалізувати payload подій до стабільного контракту.
- **Service wizard + predictive maintenance**
  - Додати `maintenance` модель у config/history.
  - Вести лічильники ресурсу relay/element/thermocouple.
  - API: `/api/maintenance/*` + чеклісти, рекомендації, due alerts.

### 13.4 Пріоритет впровадження (оновлений)

пропустити крок( додати температуру , додати час) зробити це також на екрані з підтвердженням

на екрані під іконкою вайфай залиш тільки налаштування підключення до вайфаю  решту налаштувань перенеси до іконки шестерні праавіше від цтх іконок(тобто змісти іх ліввіше)

 годинник на екрані можна натискати для того щоб вписати вручну дату і час якшо немає вайфай зєднання

додай основну сторінку завантаження контроллера де буде великий логотип kiln pro на весь екран

додай режим  standby якшо не натискати на екран  контроллера більше 3 хв то він переходить в цей режим і міняє відображення на велике цифрове значення температури і логоти Kiln PRO щоб вийти зцього режиму треба просто натиснути на екран

 додати датчик струму як у scutt для того щоб проводити діагностику обриву спіралі, визначати реальну потужність яку споживає піч і ввести розрахунки витрат acs758lcb 100b

 ацп ads1115 lkz p,skmityyz rskmrjcns gsyd gslrk.xtyyz

## 14) ESP-IDF v6.0 migration checklist

### 14.1 Статус

- Build/flash на ESP-IDF v6.0: ✅
- RTC DS3231 мігровано на `driver/i2c_master.h`: ✅
- Legacy include `driver/i2c.h` у `config.h` прибрано: ✅
- Crypto API для remote/OTA (PSA): ✅

### 14.2 Залишкові warning-и (не блокують)

- Rust/Slint лінкер-попередження про `chown` (ESP-платформа не підтримує): informational.
- Linker warning про `.note.GNU-stack` із toolchain/libm: зовнішній тулчейн-артефакт.
- Попередження esptool `--after hard_reset` (deprecated): бажано перевести на `hard-reset`.

### 14.3 Далі для “чистого” CI

- Додати окрему CI-конфігурацію під IDF 6.0 з fail-on-new-warnings policy.
- Зафіксувати baseline warning log і контролювати тільки нові регресії.


# Codex Agent Instructions (trae_controller)

Цей файл — “контракт” для агента/асистента у цьому репозиторії. Мета: мінімум токенів, максимум корисних змін у коді, з фокусом на **ESP32‑S3 контролер муфельної печі** на **ESP‑IDF 5+** та UI на **Slint** (LVGL може бути присутній як спадщина/фолбек).

## 1) Принципи роботи (коротко)
- Працюй інкрементально: маленькі PR‑подібні зміни, що збираються.
- Мінімізуй токени: короткі відповіді, менше “теорії”, більше конкретних дифів/команд.
- Перед змінами: швидко знайди “де живе функція/модуль” (див. мапу нижче).
- Після змін: по можливості перевір `idf.py build` (або точкові тести/збірку).
- Не роби “рефактор заради рефактору”. Лише те, що потрібно задачі.

## 2) Безпека (муфельна піч = high‑stakes)
- Завжди продумуй **fail‑safe**: відмова датчика → вимкнути нагрів, сигнал/лог, safe state.
- Обов’язково враховуй: **over‑temperature**, обрив/КЗ термопари, зависання UI/мережі, watchdog.
- Будь‑які зміни в керуванні нагрівом/SSR/реле мають мати явні межі, таймаути, стан “heater off”.
- Логи й телеметрія важливі: події fault/overtemp мають бути видимі (UI + мережа, якщо є).

## 3) Репозиторій: розділи та призначення
- `firmware/` — **ESP‑IDF** прошивка (основний фокус для ESP32‑S3).
- `frontend/` — веб/UI частина (якщо використовується для налаштувань/дашборду).
- `backend/` — серверна частина (API/збереження/інтеграції), якщо потрібна.
- `data/` — дані/артефакти (перевіряй, що саме тут зберігається перед змінами).

## 4) Firmware (ESP‑IDF) — “де що лежить”
ESP‑IDF проєкт: `firmware/CMakeLists.txt`, конфіг: `firmware/sdkconfig*`, розділи: `firmware/partitions.csv`.

Головний компонент застосунку: `firmware/main/`
- `firmware/main/app/app_main.cpp` — точка входу застосунку (ініціалізація, цикли).
- `firmware/main/drivers/display_driver.cpp` — дисплей + touch (XPT2046) + NVS калібрування.
- `firmware/main/ui/slint_bridge.cpp` + `firmware/main/include/ui/slint_bridge.h` — місток C++ ↔ Slint.
- `firmware/main/net/wifi_connection.cpp` — Wi‑Fi (STA/AP), NVS креденшали, scan task.
- `firmware/main/net/wifi_server.cpp` — HTTP/WS сервер (керування/телеметрія, captive portal).
- `firmware/main/net/dns_server.cpp` — DNS (для captive portal).

Компоненти (ESP‑IDF): `firmware/components/`
- `firmware/components/kiln_control/` — сервіс керування нагрівом (ThermalController + PID/MAX6675).
- `firmware/components/kiln_config/` — єдина конфігурація плати/пінів/лімітів (`config.h`).

UI:
- `firmware/slint_ui/` — Rust + `.slint` (рантайм UI на Slint).
- `firmware/data/` — LittleFS артефакти (в т.ч. `touch_calibration.html`).

## 5) Правила структурування коду (рекомендований поділ)
Коли додаєш/переносиш функції — тримай шари:
- `hal/` (або компонент) — GPIO/I2C/SPI/ADC, датчики, SSR/реле (без бізнес‑логіки).
- `drivers/` — дисплей, сенсори, NVS (thin wrappers).
- `services/` — PID/профілі випалу, стани, safety/fault manager, планувальник.
- `ui/` — Slint екрани й presenter‑логіка (без прямого керування GPIO).
- `net/` — HTTP/WS, протоколи, серіалізація, OTA (за потреби).
- `app/` — glue/ініціалізація, tasks, залежності.

Мінімальна вимога: **не змішувати** UI/мережу з керуванням нагрівом напряму; все через service API.

## 6) Конвенції
- Мова: як у проєкті зараз (C++ для `main/*.cpp`, Rust для `firmware/slint_ui/`).
- Імена: чіткі, без “utils2”, краще `*_service`, `*_driver`, `*_manager`.
- Конфіг: використовуй `sdkconfig.defaults`/Kconfig (де доречно), не хардкодь піни/параметри.
- Логи: через ESP‑IDF logging (узгоджені TAG), без спаму в tight loops.

## 7) “Пам’ять” проєкту та TODO
Тримай живий короткий стан у `memory.md` (оновлюй, коли змінюється архітектура/рішення/плани).
- Після кожної суттєвої зміни: онови **Project Map**, **Decisions**, **TODO**.
- TODO — конкретні, з чек‑пунктами і пріоритетом (P0/P1/P2).

## 8) Збірка/флеш (типово)
Усі команди запускай з `firmware/`:
- `idf.py set-target esp32s3` (один раз/за потреби)
- `idf.py build`
- `idf.py -p COMx flash monitor` (Windows)

## 9) Коли потрібна “актуальність”
Якщо питання про “найновіші” версії бібліотек/ESP‑IDF/Slint/LVGL/залежностей або API — перевіряй офіційні джерела перед порадами (не вигадуй).

---

## 10) Notes (Updated 2026-03-17)
- Touch stack: XPT2046 в RAW режимі (0..4095) + box calibration + optional affine + optional 3x3 grid warp; усе зберігається в NVS.
- Touch APIs: `/api/touch/calibration`, `/api/touch/affine`, `/api/touch/grid`, `/api/touch/raw`, `/api/touch/probe`, `/api/touch/spi`, `/touch_calibration.html` (9-point).
- Effective RAW range: XPT2046 відкидає значення біля “рейок”; практичний діапазон ~`50..4045` (використовується для clamping калібровки).
- UI renderer: Slint software renderer (MCU) **не** підтримує `Path`; іконки робимо через прямокутники/текст або glyph‑font.
- UI debug: touch debug bar прибрано; червона “точка дотику” лишається для діагностики вирівнювання.


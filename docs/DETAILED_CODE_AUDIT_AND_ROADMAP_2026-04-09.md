# Детальний code-аудит і план покращень (`trae_controller`)

Дата: 2026-04-09  
База: фактичний аудит коду firmware/UI (ESP-IDF + Slint), а не лише порівняння з зовнішніми проєктами.

## 1) Що вже реалізовано добре (підтверджено в коді)

1. **Hardware abstraction для нагріву є**
   - `heater_hal` уже відокремлений: `SSR_ZONE1_PIN` + `SAFETY_RELAY_PIN`, `heater_hal_all_off()`.
   - Файли: `firmware/components/kiln_hal_heater/heater_hal.cpp`, `.../heater_hal.h`.

2. **Базовий safety/fault контур є**
   - Latched fault state + reason + timestamp (`kiln_fault_set/get/clear`).
   - Файли: `firmware/components/kiln_safety/faults.cpp`, `.../faults.h`.

3. **Thermal safety у контролері суттєвий**
   - Контроль сенсора: `open/short/NaN/out-of-range/spike/stuck`, rise-rate, read-timeout watchdog, overtemp.
   - `applyFaultStateLocked()` примусово вимикає heater (`heater_hal_all_off()`).
   - Файл: `firmware/components/kiln_control/thermal_control.cpp`.

4. **Power-loss recovery уже є**
   - Запис recovery state у `/littlefs/recovery.json`, автоматичне відновлення.
   - Файл: `thermal_control.cpp`.

5. **Уніфіковані командні API вже запущені**
   - `device_commands` використовується в REST/WS/Slint paths.
   - Файли: `firmware/main/app/device_commands.cpp`, `firmware/main/net/wifi_server.cpp`, `firmware/main/ui/slint_bridge.cpp`.

6. **Audit і telemetry по командах уже є**
   - `command_result` WS і `audit.log`.
   - Файл: `firmware/main/net/wifi_server.cpp`.

7. **Watchdog hardening на рівні задач є**
   - UI heartbeat + NET heartbeat + emergency stop при stall.
   - Файл: `firmware/main/app/app_main.cpp`.

## 2) Що реалізовано частково / з розривами

## P0-Critical (впливає на correctness/safety/оператора)

1. **Невірне мапування статусів у Slint UI**
   - У firmware enum: `1=RUNNING, 2=HOLD, 3=COOLING, 4=COMPLETE, 5=FAULT`.
   - У Rust UI зараз: `1->PREHEAT, 2->RAMP, 3->HOLD, 4->COOL, 5->COMPLETE`.
   - Наслідок: оператор бачить неправильний стан (наприклад `COMPLETE` відображається як `COOL`).
   - Файли:
     - `firmware/components/kiln_control/include/kiln_control/thermal_control.h`
     - `firmware/slint_ui/src/lib.rs` (`apply_state_to_ui`, блок status mapping).

2. **Command API повертає `ok` навіть коли команда фактично не застосована**
   - `device_commands::load_schedule_json()` завжди `Ok` (навіть якщо schedule parse failed у `thermalCtrl.loadSchedule`).
   - `skip/add_temp/add_time/set_rate` теж повертають `Ok` без підтвердження, що команда реально виконалась.
   - Файли:
     - `firmware/main/app/device_commands.cpp`
     - `firmware/components/kiln_control/thermal_control.cpp` (методи void без status return).

3. **Rate-limit bug для `/api/setRate`**
   - Використовується `lastAddTimeCommandMs` замість окремого таймстемпу для rate-команди.
   - Файл: `firmware/main/net/wifi_server.cpp` (`api_set_rate_handler`).

4. **Fault logs у UI не покривають автоматичні safety-fault події**
   - UI читає `audit.log`.
   - Thermal safety події пишуться в `events.log` (і то лише частково), або не пишуться взагалі при `tripFaultLocked`.
   - Файли:
     - `firmware/slint_ui/src/lib.rs` (`load_fault_log_items` читає `/littlefs/logs/audit.log`)
     - `firmware/components/kiln_control/thermal_control.cpp` (`append_event_log`, `tripFaultLocked`).

5. **Автовідновлення після power-loss без policy-gates**
   - `loadRecoveryStateLocked()` автоматично резюмує випал при наявності recovery state.
   - Немає явної перевірки “safe-to-resume” (вік outage/температура/сенсор/fault).
   - Файл: `firmware/components/kiln_control/thermal_control.cpp`.

## P1-Important (стабільність та UX прозорість)

1. **`KILN_FAULT_SSR` оголошений, але фактично не тригериться**
   - Код fault є в enum, але логіки SSR-stuck detection немає.
   - Файли:
     - `firmware/components/kiln_safety/include/kiln_safety/faults.h`
     - `firmware/components/kiln_control/thermal_control.cpp`.

2. **`sensorShortStreak` практично не використовується**
   - Поле інкрементально не формує окремого сценарію, переважно скидається.
   - Можна прибрати або завершити логіку.
   - Файл: `thermal_control.cpp/.h`.

3. **Непослідовність command event для `fault_clear` по HTTP**
   - `slint_bridge_clear_fault` шле `notifyCommandResult`, а `/api/fault/clear` — ні.
   - Через це веб-клієнти мають різну реакцію в залежності від каналу.
   - Файли:
     - `firmware/main/ui/slint_bridge.cpp`
     - `firmware/main/net/wifi_server.cpp` (`api_fault_clear_handler`).

4. **Headless сценарій блокується вимогою touch calibration**
   - Start interlock вимагає `display_driver_is_touch_calibrated()`.
   - Для headless/remote mode варто зробити режимну політику, а не hard-block завжди.
   - Файл: `firmware/main/app/device_commands.cpp`.

## 3) Детальний план покращень (реалістичний, по кроках)

## Етап A (P0): Correctness contract для команд і станів

1. **Виправити status mapping UI**
   - Файл: `firmware/slint_ui/src/lib.rs`.
   - Кроки:
     - замінити мапування `state.status -> status_label` згідно enum `thermal_control.h`;
     - додати явний `PAUSED`.
   - Acceptance:
     - `KILN_COMPLETE` на backend відображається як `COMPLETE`;
     - `KILN_HOLD` відображається як `HOLD` (не `RAMP`).

2. **Зробити команди “truthful”**
   - Файли:
     - `firmware/components/kiln_control/include/.../thermal_control.h`
     - `firmware/components/kiln_control/thermal_control.cpp`
     - `firmware/main/app/device_commands.cpp`
   - Кроки:
     - змінити `loadSchedule/skip/addTemp/addTime/setRate` на повернення `bool`/`ResultCode`;
     - в `device_commands` віддавати реальний `ResultCode`, не дефолтний `Ok`.
   - Acceptance:
     - invalid schedule -> `invalid_schedule`;
     - `add_time` поза `HOLD` -> `invalid_state` (новий код або `invalid_payload` + reason).

3. **Fix rate-limit для set-rate**
   - Файли:
     - `firmware/main/include/net/wifi_server.h` (додати `lastSetRateCommandMs`);
     - `firmware/main/net/wifi_server.cpp` (`api_set_rate_handler`).
   - Acceptance:
     - `set_rate` rate-limit незалежний від `add_time`.

## Етап B (P0/P1): Unified fault/event observability

1. **Логувати fault-події в єдиний потік**
   - Файл: `thermal_control.cpp`.
   - Кроки:
     - у `tripFaultLocked()` додати `append_event_log("FAULT", reason/code)`;
     - лог `FAULT_CLEAR`, `RECOVERY_RESUME`, `RECOVERY_ABORT`.
2. **Fault Logs у Slint: читати і `audit.log`, і `events.log`**
   - Файл: `firmware/slint_ui/src/lib.rs` (`load_fault_log_items`).
   - Acceptance:
     - автоматичний sensor fault з’являється у “Fault Logs” без участі API-команди.
3. **Додати `GET /api/events`**
   - Файл: `firmware/main/net/wifi_server.cpp` (+ header).
   - Acceptance:
     - веб/мобільний клієнт може забирати fault/event timeline.

## Етап C (P0): Safe recovery policy після power-loss

1. **Resume guard**
   - Файл: `thermal_control.cpp` (`loadRecoveryStateLocked`).
   - Кроки:
     - додати критерії `safe_to_resume`:
       - сенсор healthy,
       - немає active fault,
       - температура в допустимому вікні,
       - age recovery <= configurable limit.
     - якщо fail -> не resume, перейти в `IDLE/FAULT` з поясненням.
   - Acceptance:
     - після довгого відключення контролер не відновлює випал автоматично.

## Етап D (P1): SSR fail-safe completeness

1. **SSR stuck detection**
   - Файл: `thermal_control.cpp`.
   - Кроки:
     - коли heater OFF тривалий час, але temp стабільно росте вище порогу -> `KILN_FAULT_SSR`.
     - форсувати `heater_hal_all_off`, latch fault.
   - Acceptance:
     - fault code 11 реально досяжний тестом fault injection.

## Етап E (P1): Режими запуску (panel/headless/remote)

1. **Mode-aware interlocks**
   - Файли:
     - `device_commands.cpp`,
     - config (`/littlefs/config.json` + defaults).
   - Кроки:
     - політика `require_touch_calibration_for_start` (true для panel, false для headless).
   - Acceptance:
     - headless deployment може стартувати легально без touch calibration, але з іншими safety interlocks.

## 4) Пропонований порядок реалізації (щоб мінімізувати ризик)

1. Етап A.1 + A.3 (швидкі та безпечні виправлення, максимум користі).
2. Етап A.2 (контракт команд) + частина B.1.
3. Етап C (policy для recovery) перед будь-якими новими “feature” задачами.
4. Етап B.2/B.3 (операторська діагностика).
5. Етап D/E.

## 5) Коротко: що саме було неправильно у попередньому high-level плані

- Не враховано, що `kiln_hal_heater` і `kiln_safety` у вас **вже є**.
- Недооцінено, що у вас **вже є** power-loss recovery, audit log, command_result WS, autotune pipeline.
- Головні реальні задачі зараз не “додати з нуля”, а:
  - вирівняти контракти між шарами,
  - прибрати невідповідності між backend станом і UI,
  - завершити safety-policy до production-рівня.


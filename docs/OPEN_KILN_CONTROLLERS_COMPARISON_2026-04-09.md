# Порівняння open-source kiln controller проєктів і roadmap для `trae_controller`

Дата: 2026-04-09  
Фокус: ESP32-S3 + ESP-IDF 6.0 + Slint UI (480x320), safety-critical муфельна піч.

## 1) Що порівнювали

- PyKiln (MicroPython + ESP32): https://github.com/RinthLabs/PyKiln
- kiln-controller (Raspberry Pi): https://github.com/jbruce12000/kiln-controller
- PIDKiln (ESP32, Arduino core): https://github.com/Saur0o0n/PIDKiln
- PiLN Flask (Raspberry Pi): https://github.com/fayena/PILN_Flask
- picoReflow (Raspberry Pi): https://github.com/apollo-ng/picoReflow
- Окремо переглянуто сторінку можливостей PyKiln: https://pykiln.com/features.html

## 2) Короткий бенчмарк проти нашого проєкту

### Де `trae_controller` вже сильний

- Архітектура: чіткий поділ firmware/UI/net + рух до unified command/state model.
- Safety-база: fault latch, watchdog hardening, блок старту при проблемах сенсора/калібрування touch.
- Локальний UI: Slint runtime на 480x320, уніфікація через `UiButton/UiCard`, captive-portal + touch калібрування.
- Віддалений доступ: MQTT/TLS + HMAC перевірка команд + anti-replay.
- OTA: є endpoint оновлення з перевіркою SHA-256.

### Де конкуренти мають корисні ідеї

- `kiln-controller`: mature симуляція, watcher-алерти, автоперезапуск після збоїв, розвинений tuning/debug.
- `PIDKiln`: акцент на апаратну fail-safe надмірність (SSR + додаткове механічне реле), network syslog, energy metering.
- `PyKiln`: проста UX-модель (schedules/logs/settings), import/export розкладів, remote-notifications, self-checks.
- `PiLN Flask`: зрозумілий DB-driven лог/історія + інкрементальні графіки.

### Основні gap-и для `trae_controller`

- Потрібна чітка апаратна dual-cutoff стратегія (SSR + safety relay) у стандартній схемі та в коді interlock.
- Бракує системного event/fault журналу з retention-політикою (локально + remote sink).
- Недостатньо “production” інструментів перевірки: симулятор fault-сценаріїв + hardware-in-the-loop smoke.
- UI/UX ще не повністю “operator-first”: швидкі preflight чеклісти, чіткі fault playbooks, короткі guided flows.
- Потрібна завершена історія power-loss recovery policy (resume/abort with explicit safe rules).

## 3) Пропоновані подальші кроки

## P0 (безпека/надійність, виконати першочергово)

1. **Dual-channel heater cutoff**
   - Додати формалізований `HeaterSafetyManager`: `SSR_ENABLE` + `SAFETY_RELAY` з незалежними таймаутами.
   - Ввести періодичний “stuck relay” тест (температура росте при OFF -> latch fault + hard cutoff).
2. **Power-loss policy**
   - Зафіксувати та імплементувати таблицю рішень: `resume only if` (короткий outage, без fault, температура в межах), інакше abort-safe.
   - Додати явний UX після перезапуску: `Resume / Abort / Diagnostics`.
3. **Fault/event journal v1**
   - Кільцевий буфер в LittleFS (`/littlefs/logs/events.bin|jsonl`) з кодами: `sensor_open`, `overtemp`, `wdt_ui`, `wdt_net`, `relay_stuck`, `power_loss`.
   - API: `GET /api/events?since=` + експорт останніх N записів.
4. **Preflight safety checklist**
   - Перед стартом профілю примусові перевірки: сенсор, калібрування, Wi-Fi необов’язковий, relay self-test, max temp config.
   - Окремий UI-екран “Ready to Fire” з чіткими причинами блокування.

## P1 (функціонал і UX, найбільший приріст для користувача)

1. **Розклади v2 (TAP-подібний сценарій)**
   - Нормалізована схема segment-ів: `ramp_rate`, `target_temp`, `hold_min`, `cooling_mode`, `alerts`.
   - Валідація на firmware + web + Slint (єдина схема).
2. **Guided UX для оператора**
   - Wizard: `Підготовка -> Вибір програми -> Preflight -> Запуск`.
   - In-run quick actions: `Skip`, `+Temp`, `+Time`, `Abort` з підтвердженням і причиною.
3. **Графіки та аналітика**
   - Actual vs Setpoint, rate-of-rise, duty-cycle SSR, fan effective power, fault markers на таймлайні.
   - Після випалу: короткий “run report” (тривалість, відхилення, енергооцінка, fault summary).
4. **Сповіщення**
   - Notification policy: `run start/complete`, `fault`, `stalled heating`, `power restored`.
   - Канали: WebSocket toast + MQTT event stream (вже є база).

## P2 (масштабування/продуктність)

1. **Тестова матриця та симулятор**
   - Fault-injection режим у firmware (тільки dev build) + script-driven сценарії.
   - Авто smoke-набір: start/stop/fault-clear/reconnect/OTA sanity.
2. **Підтримка багатозонності (підготовчий етап)**
   - Абстрагувати `ZoneController` (навіть якщо наразі 1 зона).
   - У payload/API передбачити `zones[]` сумісно з майбутнім розширенням.
3. **OTA hardening v2**
   - Додати підписаний маніфест (не тільки SHA-256), rollback status endpoint, план повернення dual-slot OTA.

## 4) Пріоритетний 30/60/90-денний план

### 0-30 днів

- P0.1 Dual-cutoff manager + stuck relay detection.
- P0.2 Event journal + API читання подій.
- P0.3 Preflight checklist в Slint + блокування старту за критичними умовами.

### 31-60 днів

- P1.1 Schedule schema v2 + міграція редактора.
- P1.2 Run report + fault markers на графіку.
- P1.3 Notification policy (локально + remote).

### 61-90 днів

- P2.1 Fault-injection + smoke automation.
- P2.2 OTA hardening v2 (manifest signature + rollback telemetry).
- P2.3 Zone-ready API/model refactor без зламу сумісності.

## 5) Що зробити в репозиторії одразу (конкретні задачі)

1. Створити `firmware/components/kiln_safety/` з API:
   - `safety_preflight_check()`
   - `safety_heater_cutoff(reason)`
   - `safety_record_event(code, details)`
2. Додати `firmware/main/net/events_api.cpp` (`GET /api/events`).
3. Розширити canonical state (`schema_version`): `fault_code`, `last_fault_ts`, `preflight_ok`.
4. Додати Slint екран `PreflightScreen` + `FaultDetailsScreen` з чіткими recovery-кроками.
5. Описати “електричний fail-safe reference wiring” у `docs/` (SSR + contactor/relay + manual cutoff).

## 6) Джерела

- PyKiln README: https://raw.githubusercontent.com/RinthLabs/PyKiln/main/README.md
- PyKiln Features: https://pykiln.com/features.html
- kiln-controller README: https://raw.githubusercontent.com/jbruce12000/kiln-controller/main/README.md
- PIDKiln README: https://raw.githubusercontent.com/Saur0o0n/PIDKiln/master/README.md
- PiLN Flask README: https://raw.githubusercontent.com/fayena/PILN_Flask/main/README.md
- picoReflow README: https://raw.githubusercontent.com/apollo-ng/picoReflow/master/README.md

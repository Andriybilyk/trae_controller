# trae_controller - project memory

## Context
- Target: ESP32-S3 controller for a muffle furnace (safety-critical heating control).
- Framework: ESP-IDF 5+.
- UI: Slint (Rust toolchain) for runtime UI; LVGL retained only for fallback.

## Project Map (quick)
- Firmware (ESP-IDF): `firmware/`
  - App entry: `firmware/main/app/app_main.cpp`
  - Thermal control (component): `firmware/components/kiln_control/thermal_control.cpp`
  - UI bridge: `firmware/main/ui/slint_bridge.cpp`, `firmware/main/include/ui/slint_bridge.h`
  - Slint UI (Rust): `firmware/slint_ui/`
  - Slint component glue: `firmware/components/slint_ui/`
  - LVGL UI (archived, out of build): `firmware/legacy/lvgl_ui/main/ui/ui_manager.cpp`
  - Wi-Fi: `firmware/main/net/wifi_connection.cpp`
  - Server: `firmware/main/net/wifi_server.cpp`
  - Display+Touch: `firmware/main/drivers/display_driver.cpp`
  - DNS: `firmware/main/net/dns_server.cpp`
  - Config (component): `firmware/components/kiln_config/include/kiln_config/config.h`
- Frontend: `frontend/`
- Backend: `backend/`

## Decisions (keep short)
- Safety first: sensor fault/overtemp => heater OFF + visible fault.
- Avoid direct UI/NET -> GPIO; go through service APIs.
- Net telemetry reads via `ThermalController` getters (no cross-module globals).
- Cross-device sync: broadcast WS event `{"event":"schedules_changed","rev":...}` on schedule save/delete; web + LVGL refresh from revision.
- Persist runtime tuning/settings in `/littlefs/config.json` (temp offset + PID tunings).
- Runtime UI migrated to Slint; LVGL config backed up at `firmware/main/include/lv_conf.h.lvgl_backup_2026-03-15`.
- Slint fonts/images are embedded for the software renderer via `EmbedForSoftwareRenderer` in `firmware/slint_ui/build.rs`.
- Windows build: `firmware/components/slint_ui/CMakeLists.txt` injects `RUSTFLAGS=-Cdebuginfo=0 -Clink-arg=/PDB:NONE` to avoid `LNK1140` during Rust host/build-script link.

## TODO
- P0: Verify Slint embedded build for `xtensa-esp32s3-espidf` and confirm Slint software renderer + touch input on hardware.
- P0: Safety/fault manager: latched faults + single `heater_off()` path (sensor open/NaN, overtemp, rise-rate, watchdog).
- P0: Heater HAL: isolate GPIO control (SSR + safety relay) behind a small API; no direct GPIO from UI/NET.
- P0: Sensor diagnostics: explicit fault causes (thermocouple open/NaN, out-of-range, stuck value) + rate-of-rise limit (use `MAX_TEMP_RISE_RATE`).
- P1: Firing profiles: strict model (segments ramp/hold), validation + versioned JSON schema, persistence in NVS.
- P1: Clear state machine: `IDLE/FIRING/COMPLETE/ERROR` with sub-states; avoid duplicating `status` vs `isFiring`.
- P1: FreeRTOS tasks: split `control/ui/net` loops with defined rates + watchdog strategy. (DONE: moved loops to dedicated tasks; main task deletes itself)
- P1: Config hygiene: remove WiFi credentials from headers; defaults via Kconfig, runtime via NVS.
- P1: Sync: add WS events for history changes + "active schedule loaded" so multiple web clients/LVGL stay aligned.
- P1: Autotune UX: show progress/result in Settings/Dashboard (cycles, Ku/Pu, final PID) + add STOP button calling `/api/autotune/stop`.
- P1: PID controls: `GET /api/pid` + `POST /api/pid/reset` in UI with clear "default vs tuned" indicator.
- P2: API schema: add `schema_version`, `fw_version`, `fault_code`, `uptime_ms` to endpoints for compatibility.
- P2: Persistent event log: ring-buffer faults/start/stop/overtemp into `/littlefs/logs`.
- P2: OTA updates: safe OTA + rollback.

## Notes
- Update this file when adding modules, moving files, or changing architecture.

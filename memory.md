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
- Touch input uses XPT2046 RAW coordinates + calibrated mapping (box; optional affine; optional 3x3 grid warp), all persisted in NVS and configurable via HTTP API.
- Touch calibration uses the effective XPT2046 range (~50..4045) because samples near ADC rails are rejected by the driver.
- Slint MCU/software renderer does not support `Path` items; icons must be implemented with rectangles/text or glyph-fonts.
- Slint UI (panel): Wi‑Fi icon opens an on-device Settings screen; language toggle is placed next to the time; editor uses full-screen text/numeric keyboards for touch input; fan manual override is available from the panel UI.
- Slint editor keyboard opening is deferred by one UI tick to avoid re-entrant overlay creation during touch release on ESP32-S3; text keyboard rows are sized to stay within 480x320 without overlap.
- Runtime safety interlock added for start: start is blocked when thermocouple sensor is unhealthy or touch calibration is missing (`display_driver_is_touch_calibrated()`).
- Watchdog hardening added: UI heartbeat (Rust -> C++ bridge) + NET heartbeat monitored by control task; stall triggers emergency stop and forced `heater_off` path.
- Command debounce/rate-limit added on both sides: Slint start/stop/keyboard anti-bounce plus HTTP API rate-limit for start/stop/skip/add-temp/add-time.
- OTA endpoint added: `POST /api/ota/update` writes to next OTA partition and requires SHA-256 header (`X-Firmware-Sha256` or alias) before boot-partition switch + reboot.
- Frontend Settings now has OTA upload flow: file pick, browser SHA-256 calculation, upload to `/api/ota/update` with checksum header.
- Fan control extended: manual PWM + automatic temperature-based curve (`temp_min/max` -> `power_min/max`) with runtime update in control loop.
- Fan APIs added for web sync/control: `GET/POST /api/fan`; status payload now includes `fan_manual`, `fan_auto`, `fan_power`, `fan_effective_power`.
- Web Dashboard fan card is now the primary fan control (double-width card, icon tap toggles ON/OFF, manual +/- speed, PCB temperature readout); fan controls and simulator route were removed from web Settings.
- Fan runtime policy: during firing, automatic fan output has a minimum floor of 60% (board sensor driven) to protect SSR area cooling.

## TODO
- P0: Verify Slint embedded build for `xtensa-esp32s3-espidf` and confirm Slint software renderer + touch input on hardware.
- P0: Touch accuracy: finalize a repeatable calibration flow (web 9‑point) and document the recommended reset/apply order (calibration/affine/grid).
- P0: Safety/fault manager: latched faults + single `heater_off()` path (sensor open/NaN, overtemp, rise-rate, watchdog).
- P0: Validate watchdog thresholds (`UI/NET 4s`, TWDT 12s) on real hardware under Wi-Fi loss, UI heavy input, and OTA reboot scenarios.
- P0: Heater HAL: isolate GPIO control (SSR + safety relay) behind a small API; no direct GPIO from UI/NET.
- P0: Sensor diagnostics: explicit fault causes (thermocouple open/NaN, out-of-range, stuck value) + rate-of-rise limit (use `MAX_TEMP_RISE_RATE`).
- P1: Firing profiles: strict model (segments ramp/hold), validation + versioned JSON schema, persistence in NVS.
- P1: Clear state machine: `IDLE/FIRING/COMPLETE/ERROR` with sub-states; avoid duplicating `status` vs `isFiring`.
- P1: FreeRTOS tasks: split `control/ui/net` loops with defined rates + watchdog strategy. (DONE: moved loops to dedicated tasks; main task deletes itself)
- P1: Config hygiene: remove WiFi credentials from headers; defaults via Kconfig, runtime via NVS.
- P1: Sync: add WS events for history changes + "active schedule loaded" so multiple web clients/LVGL stay aligned.
- P1: Autotune UX: show progress/result in Settings/Dashboard (cycles, Ku/Pu, final PID) + add STOP button calling `/api/autotune/stop`.
- P1: PID controls: `GET /api/pid` + `POST /api/pid/reset` in UI with clear "default vs tuned" indicator.
- P1: UI polish: consistent typography sizes/contrast on the 480x320 panel; unify icon set (glyph-font based) across target + simulator; remove any debug-only overlays from release UI.
- P1: Re-test editor input on hardware after UI changes: numeric keypad open/close, long program names, UA/EN keyboard rows, and touch-release edge cases.
- P1: Persist fan curve + mode at boot explicitly (load config before fan init or defer fan init until LittleFS config is applied).
- P2: API schema: add `schema_version`, `fw_version`, `fault_code`, `uptime_ms` to endpoints for compatibility.
- P2: Persistent event log: ring-buffer faults/start/stop/overtemp into `/littlefs/logs`.
- P2: OTA updates: add signed manifest validation (cryptographic signature) and rollback-status reporting endpoint.

## Notes
- Update this file when adding modules, moving files, or changing architecture.

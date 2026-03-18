# Unified Control Model Plan

## Goal
Build one shared control/state model for:
- Slint on-device UI
- Web UI (mobile + desktop)
- Future iOS app

This removes duplicated logic, simplifies sync, and makes transport (WS/HTTP/BLE bootstrap) a thin layer.

## Core Idea (Single Source of Truth)
Create one canonical state schema and one command schema in firmware.

- `DeviceState` (read model): full runtime snapshot and revision counters
- `DeviceCommand` (write model): validated actions from any client
- `DeviceEvent` (stream model): push updates and change notifications

All clients map UI widgets to these same fields, instead of custom per-client variables.

## Target Architecture
- Firmware service layer:
  - Owns kiln state, wifi state, fan state, safety state, revisions.
  - Exposes state/commands through a single adapter used by Slint bridge + web server.
- Slint UI:
  - Reads/writes via bridge functions only.
- Web UI:
  - Uses WS as primary realtime channel.
  - Uses HTTP as fallback and for idempotent operations.
- iOS app (next step):
  - BLE for discovery/bootstrap only.
  - Main control channel over local IP (same schema as web).

## Data Contract v1 (Proposed)
- `DeviceState`
  - `ts_ms`, `fw_version`, `schema_version`
  - kiln: `temp`, `target`, `status`, `firing`, `step`, `totalSteps`, `timeRemaining`
  - safety: `fault_active`, `fault_code`, `fault_reason`, `sensor_ok`
  - fan: `fan_manual`, `fan_auto`, `fan_power`, `fan_effective_power`, `pcbTemp`
  - wifi: `wifi_connected`, `wifi_mode`, `ap_ssid`, `sta_ssid`, `server_url`
  - sync: `schedules_rev`, `settings_rev`
- `DeviceCommand`
  - kiln: `start(schedule_id|schedule_json)`, `stop`, `skip`, `add_time`, `add_temp`
  - fan: `set_fan_mode`, `set_fan_power`, `set_fan_curve`
  - wifi: `scan`, `connect`, `disconnect_forget`
  - system: `clear_fault`, `ota_start`
- `DeviceEvent`
  - `state` (full or delta)
  - `schedules_changed`
  - `settings_changed`
  - `fault_changed`
  - `autotune_state`

## Implementation Plan

### Phase 0 - Contract and Adapter (Firmware)
- [ ] Define `schema_version` and stable JSON keys.
- [ ] Introduce a single state builder in firmware (shared by `/api/status` and WS broadcast).
- [ ] Introduce a single command dispatcher in firmware (used by API handlers and bridge wrappers).
- [ ] Keep backward compatibility for existing endpoints during migration.

Definition of done:
- One source function for state serialization.
- One source function for command validation/dispatch.

### Phase 1 - Slint Alignment
- [ ] Map all Slint variables to canonical fields.
- [ ] Remove duplicated local transformations where possible.
- [ ] Add revision-based refresh hooks only (avoid ad-hoc reloads).

Definition of done:
- Slint screen reflects exactly canonical `DeviceState`.
- Slint write actions call canonical commands only.

### Phase 2 - Web Alignment (Mobile + Desktop)
- [ ] Create a shared TS `DeviceState`/`DeviceCommand` model in frontend.
- [ ] Switch dashboard/control components to canonical model fields.
- [ ] Use WS `state` frames as primary update path; HTTP polling as fallback.
- [ ] Remove legacy per-page field assumptions and duplicated mapping logic.

Definition of done:
- Mobile and desktop views render from the same data shape.
- Same user action on either client emits the same command payload.

### Phase 3 - Sync and Reliability
- [ ] Add monotonic `ts_ms` + optional `seq` to WS frames.
- [ ] Add stale-frame protection on clients.
- [ ] Add retry/backoff and reconnect behavior standardization.
- [ ] Add integration tests for multi-client sync (screen + web mobile + web desktop).

Definition of done:
- Deterministic sync under reconnect, AP/STA transitions, and fast user input.

### Phase 4 - iOS Bootstrap Design (Next)
- [ ] Define BLE GATT service for discovery/bootstrap payload:
  - controller id
  - friendly name
  - last known IP
  - AP fallback SSID
  - optional signed token / pairing nonce
- [ ] iOS flow:
  1. Discover known controller over BLE.
  2. Resolve network endpoint (IP/mDNS/cache).
  3. Connect over IP to same command/state API.
- [ ] Implement "trusted controllers" store in iOS app.
- [ ] Add secure pairing and re-auth strategy.

Definition of done:
- iOS can reconnect to previously paired controllers with minimal user steps.

## Recommended iOS Connectivity Strategy
- Use BLE for:
  - discovery
  - first pairing
  - local endpoint bootstrap
- Use Wi-Fi/IP for:
  - full telemetry
  - control commands
  - OTA and heavy data

Reason:
- BLE is robust for finding devices but weak for high-rate telemetry/control UX.
- Reusing Wi-Fi protocol avoids maintaining two full control stacks.

## Security and Safety Requirements
- [ ] Command auth/session for non-AP mode.
- [ ] Pairing trust model (controller whitelist).
- [ ] Safety guard remains firmware-local and cannot be bypassed by client.
- [ ] Rate-limit critical commands (`start/stop/skip`) across all transports.

## Project Checklist (Execution Order)
- [ ] Add canonical schema header and docs.
- [ ] Refactor firmware state serializer to single source.
- [ ] Refactor firmware command handling to dispatcher.
- [ ] Align Slint bridge with dispatcher/state model.
- [ ] Align frontend models and components.
- [ ] Add multi-client sync tests.
- [ ] Define BLE GATT bootstrap spec for iOS.
- [ ] Implement iOS app MVP (discover -> connect -> monitor -> control).

## Notes for Development
- Keep existing endpoints working until all clients are migrated.
- Use revision counters plus state stream for conflict-free refresh.
- Prefer additive changes first, cleanup/removal after validation.


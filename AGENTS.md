# AGENTS.md - Slint UI Architecture and Flicker Elimination Guide

## Scope

This document defines mandatory engineering rules for AI agents working on this repository.

Primary objective:
- Eliminate visible screen flash/blink during boot, Wi-Fi connect, start/stop firing, save program, and settings updates.

Secondary objective:
- Keep UI responsive while storage, network, and control logic run in background tasks.

This guide is based on lessons from:
- `catorendal-a11y/DIY-Welding-Positioner-ESP32-P4` (LVGL task architecture and failure analysis)
- ESP32-P4 + external PSRAM display constraints

---

## Non-Negotiable Rules

1. Single UI owner thread
- Only the Slint event loop thread may mutate Slint properties/models.
- Worker/control/storage/network tasks must never call Slint setters directly.
- Use `invoke_from_event_loop` or `upgrade_in_event_loop` with weak handles only.

2. No heavy work in UI callbacks
- Slint callbacks must only enqueue `UiCommand` and return.
- Forbidden in callbacks: flash writes, JSON parse/serialize of large payloads, blocking IO, sleep/delay, control state machine transitions.

3. Root window created once
- Never destroy/recreate root component for normal flow.
- Screen changes = property switch only (`current_screen`).

4. No temporary empty model states
- Never clear model then refill on screen.
- Build next data snapshot first, then apply once.

5. No loading screen for short actions
- Start/Stop/Save/setting edits must stay on current screen.
- Use small inline status indicator, not full-screen transition.

6. One render submit path at runtime
- Do not mix multiple framebuffer submit paths during one run (for example direct path + fallback path switching per event).
- Choose one stable path per board profile and keep it fixed.

7. Display and touch access serialized
- Touch read and display updates must run under one ownership model; avoid concurrent hardware access from multiple tasks.
- If one task owns touch/display hardware, others communicate via queue/flags only.

8. During flash write: never trigger full UI churn
- No screen recreate, no model rebuild, no theme reinit, no backlight changes while writing flash.
- Flash write must be debounced and executed in storage worker.

---

## Required Runtime Pipeline

```text
Slint callback
  -> UiCommand queue
  -> Control/Storage/Network worker
  -> AppState reducer
  -> UiPatch (minimal diff)
  -> single event-loop apply
  -> render
```

Do not bypass this pipeline.

---

## Required Structures

Use equivalent structures if names differ.

```rust
enum UiCommand {
    StartFiring,
    StopFiringTap,
    StopFiringConfirm,
    SaveProgram(Program),
    DeleteProgram(u32),
    ChangeSetting(SettingChange),
    Navigate(ScreenId),
    WifiScan,
    WifiConnect,
}

struct UiPatch {
    screen: Option<ScreenId>,
    kiln_state: Option<KilnState>,
    status_text: Option<String>,
    error_text: Option<String>,
    saving: Option<bool>,
    wifi_connected: Option<bool>,
    // add only small fields that can be applied atomically
}

struct PendingWrites {
    settings_save_pending: bool,
    programs_save_pending: bool,
    last_settings_change_ms: u64,
    last_program_change_ms: u64,
}
```

---

## Apply UI in One Batch

Each user action must map to one `UiPatch` application callback.

Bad:
- 10 separate `invoke_from_event_loop` calls for one action.

Good:
- One callback applying all changed fields.

```rust
fn apply_ui_patch(weak: slint::Weak<MainWindow>, patch: UiPatch) {
    weak.upgrade_in_event_loop(move |w| {
        if let Some(v) = patch.status_text { w.set_status_text(v.into()); }
        if let Some(v) = patch.kiln_state  { w.set_kiln_state(v as i32); }
        if let Some(v) = patch.screen      { w.set_current_screen(v as i32); }
        if let Some(v) = patch.saving      { w.set_is_saving(v); }
    }).ok();
}
```

---

## Storage Rules (Critical)

1. Debounce writes
- Settings flush: 500-1000 ms after last change.
- Program flush: 500-1000 ms after last change.

2. Storage worker only
- All NVS/LittleFS writes happen in storage worker/task.

3. Copy-under-lock, write-outside-lock
- If shared structures require mutex, copy data under lock, release lock, then serialize/write.
- Do not hold shared mutex across flash commit.

4. Avoid self-induced reload
- After local successful save, do not immediately force global schedule reload.
- Reload only if external revision changed and snapshot differs.

5. Flash-write guard
- Expose `flash_write_in_progress` flag.
- While true: block expensive UI rebuild/reinit operations.

---

## Model Update Rules

1. Change detection first
- Before applying schedule/program model, compare against current snapshot.
- If unchanged, skip model update.

2. Atomic model refresh
- Build rows in RAM, set model once.

3. No full-screen invalidation for local field edits
- For settings and status changes, patch only affected properties.

---

## Boot/Backlight Sequence (Mandatory)

```text
1) Backlight OFF/low
2) Init panel and Slint root once
3) Apply initial AppState and models
4) Render first stable main frame
5) Set final brightness
```

Forbidden:
- Boot -> blank/blue -> Boot -> Main
- brightness 100 -> 20 bounce during boot unless hardware-specific and documented

---

## Wi-Fi Flow Rules

1. Autoconnect must start once UI is ready.
2. Wi-Fi scan and connect are async commands only.
3. On `STA_GOT_IP`, patch only Wi-Fi icon/text/time fields.
4. Never switch screen due to Wi-Fi state changes.

---

## Start/Stop Firing Rules

1. Start tap
- Enqueue `StartFiring`.
- Validate in control worker.
- If blocked (sensor/fault), emit inline error patch only.

2. Stop flow
- Tap shows confirm popup only.
- Confirm sends command; keep current screen stable.

3. No model clears/reloads on start/stop.

---

## Screen Navigation Rules

- One root + `current_screen` state.
- If saving from editor, navigation target must be deterministic:
  - `EditorSave` -> `ProgramLibrary` immediately in UI patch.
- No hidden delayed navigation from unrelated events.

---

## Render Stability Rules (ESP32-P4)

1. Keep frame pacing fixed (target FPS) to avoid jitter bursts.
2. Avoid submitting partial garbage regions:
- Submit region only when dirty bounds are valid.
- Never present frame when there are no dirty updates.

3. If direct framebuffer mode is used:
- ensure begin/present/cancel is strictly paired.
- do not fall back to alternate blit path in same runtime mode.

4. Backlight control must be decoupled from frequent UI updates.
- No repeated brightness writes for unchanged value.

---

## Required Logs

Agents must keep these log families and use them to validate fixes:

- `UI_FLOW`
- `SCREEN_STATE`
- `MODEL_UPDATE`
- `STORAGE_WRITE`
- `RENDER_PROFILE`
- `BACKLIGHT`

Minimum examples:

```text
UI_FLOW: button=start pressed
UI_FLOW: command queued StartFiring
UI_FLOW: event FiringStartBlocked reason=thermocouple_missing
SCREEN_STATE: view=ProgramLibrary
MODEL_UPDATE: schedules rows=12 changed=true
MODEL_UPDATE: schedules changed=false skip=true
STORAGE_WRITE: programs flush begin
STORAGE_WRITE: programs flush done ok=true
RENDER_PROFILE: reason=save_program frame_ms=14 dirty=...
BACKLIGHT: set old=75 new=62 req=60
```

---

## Definition of Done for Any UI Change

All must be true:

- Root window is never recreated in normal operation.
- Save/start/stop/settings do not cause screen blink.
- No full-screen redraw caused by unchanged model reload.
- Flash writes are debounced and off UI thread.
- UI remains responsive during storage/network operations.
- Required logs confirm command -> state -> patch flow.

If any point fails, task is not complete.

---

## Explicit Anti-Patterns (Do Not Implement)

```text
on_click:
  show_loading_screen()
  write_flash_blocking()
  clear_model()
  recreate_root()
  show_main()
```

Do not fix flicker with arbitrary sleeps.
Do not hide hardware/sensor errors by forcing start.
Do not mutate Slint from worker threads.

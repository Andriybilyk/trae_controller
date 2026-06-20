# Debug Session: settings-freeze

- Status: OPEN
- Symptom: controller freezes after staying on the Settings screen for some time
- Scope: runtime debugging with instrumentation first, no business-logic changes before evidence
- Notes:
  - User reports freeze only after some time on `Settings`
  - Recent work changed Settings layout and info updates
  - Need to determine whether freeze is caused by LVGL layout churn, periodic tick work, memory pressure, or watchdog starvation

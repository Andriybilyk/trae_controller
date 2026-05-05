# Board Migration Tracker (Legacy S3 -> New P4)

## Goal
- Keep one firmware codebase with two hardware profiles:
  - `legacy_s3` (active development board, current behavior baseline)
  - `new_p4` (final production display module)

## Current Status
- Phase 1 foundation: **in progress**
- Added board profile Kconfig and runtime bridge plumbing.
- Added board-specific sdkconfig defaults files.

## Completed (this iteration)
- Added board profile config:
  - `firmware/main/Kconfig.projbuild`
  - `CONFIG_TC_BOARD_LEGACY_S3`, `CONFIG_TC_BOARD_NEW_P4`
  - `CONFIG_TC_DISPLAY_WIDTH`, `CONFIG_TC_DISPLAY_HEIGHT`
- Added board profile runtime API:
  - `firmware/main/include/config/board_profile.h`
  - `firmware/main/app/board_profile.cpp`
- Exposed board info to Slint bridge/Rust:
  - `slint_bridge_get_board_profile()`
  - `slint_bridge_get_display_width()`
  - `slint_bridge_get_display_height()`
- Bound Slint window size to board profile values at startup.
- Added board defaults files:
  - `firmware/sdkconfig.defaults.board_legacy_s3`
  - `firmware/sdkconfig.defaults.board_new_p4`

## Next (Phase 1.1)
- Move hardcoded `480x320` UI constants to root-driven tokens (`root.width/root.height`).
- Add `display/touch` HAL interfaces and select implementation by board profile.
- Keep legacy drivers as default path until new P4 driver bring-up is complete.

## Build Commands
- Legacy:
  - `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.board_legacy_s3" reconfigure build`
- New P4 profile (early scaffold):
  - `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.board_new_p4" reconfigure build`


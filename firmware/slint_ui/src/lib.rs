use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType, Rgb565Pixel, LineBufferProvider};
use slint::platform::{Platform, PointerEventButton, WindowEvent};
use slint::{Color, Image, ModelRc, Rgb8Pixel, SharedPixelBuffer, Timer, VecModel};
use slint::Model;
use serde::Deserialize;
use serde::de::Deserializer;
use encoding_rs::WINDOWS_1251;
use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::rc::Rc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// Fonts are embedded via @font-face in the .slint files (compile-time), so we don't
// need any runtime font registration here.
const UI_TARGET_FPS: u64 = 30;
const TOUCH_HOLD_DEADZONE_PX: i32 = 1;
const TOUCH_FILTER_NUM: i32 = 1;
const TOUCH_FILTER_DEN: i32 = 2;
const TOUCH_FAST_MOVE_PX: i32 = 6;
const TOUCH_DEBUG_MIN_Z: u16 = 140;
const TOUCH_DEBUG_DEADZONE_PX: i32 = 2;
const TOUCH_DEBUG_UPDATE_MIN_MS: u64 = 16;
const STANDBY_TIMEOUT_MS: u64 = 180_000;
const UI_STATE_APPLY_PERIOD_MS: u64 = 220;

slint::include_modules!();

#[repr(C)]
#[derive(Clone, Copy)]
struct SlintKilnState {
    current_temp: f32,
    target_temp: f32,
    status: i32,
    is_firing: bool,
    time_remaining: i32,
    current_step: i32,
    total_steps: i32,
    error_msg: [u8; 96],
    fault_active: bool,
    fault_reason: [u8; 96],
    pzem_voltage: f32,
    pzem_current: f32,
    pzem_power: f32,
    pzem_ok: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SlintCommandResult {
    rev: u32,
    ts_ms: u64,
    ok: bool,
    action: [u8; 24],
    source: [u8; 16],
    code: [u8; 32],
    message: [u8; 96],
}

impl Default for SlintCommandResult {
    fn default() -> Self {
        Self {
            rev: 0,
            ts_ms: 0,
            ok: false,
            action: [0; 24],
            source: [0; 16],
            code: [0; 32],
            message: [0; 96],
        }
    }
}

impl Default for SlintKilnState {
    fn default() -> Self {
        Self {
            current_temp: 0.0,
            target_temp: 0.0,
            status: 0,
            is_firing: false,
            time_remaining: 0,
            current_step: 0,
            total_steps: 0,
            error_msg: [0; 96],
            fault_active: false,
            fault_reason: [0; 96],
            pzem_voltage: 0.0,
            pzem_current: 0.0,
            pzem_power: 0.0,
            pzem_ok: false,
        }
    }
}

extern "C" {
    fn slint_bridge_display_init();
    fn slint_bridge_get_state(out: *mut SlintKilnState);
    fn slint_bridge_start_schedule_json(json: *const c_char) -> bool;
    fn slint_bridge_load_schedule_json(json: *const c_char) -> bool;
    fn slint_bridge_stop();
    fn slint_bridge_skip_step();
    fn slint_bridge_add_temp(delta_c: f32);
    fn slint_bridge_add_time(delta_minutes: f32);
    fn slint_bridge_set_rate(rate_c_per_min: f32) -> bool;
    fn slint_bridge_start_autotune(target_c: f32) -> bool;
    fn slint_bridge_stop_autotune();
    fn slint_bridge_get_user_max_temp_c() -> f32;
    fn slint_bridge_set_user_max_temp_c(max_c: f32) -> bool;
    fn slint_bridge_get_temperature_offset_c() -> f32;
    fn slint_bridge_set_temperature_offset_c(offset_c: f32) -> bool;
    fn slint_bridge_get_kiln_wattage() -> i32;
    fn slint_bridge_set_kiln_wattage(watts: i32) -> bool;
    fn slint_bridge_get_autotune_target_c() -> f32;
    fn slint_bridge_set_autotune_target_c(target_c: f32) -> bool;
    fn slint_bridge_clear_fault() -> bool;

    fn slint_bridge_set_fan_manual(enabled: bool);
    fn slint_bridge_set_fan_power(percent: i32);
    fn slint_bridge_wifi_is_connected() -> bool;
    fn slint_bridge_wifi_connect(ssid: *const c_char, password: *const c_char) -> bool;
    fn slint_bridge_wifi_disconnect();
    fn slint_bridge_wifi_scan_start();
    fn slint_bridge_wifi_scan_ready() -> bool;
    fn slint_bridge_wifi_scan_copy_results(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_wifi_server_url_copy(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_wifi_ap_ssid_copy(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_schedules_revision() -> u32;
    fn slint_bridge_get_command_result_revision() -> u32;
    fn slint_bridge_copy_command_result(out: *mut SlintCommandResult) -> bool;
    fn slint_bridge_notify_schedules_changed();
    fn slint_bridge_ui_heartbeat();
    fn slint_bridge_get_time_str(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_date_str(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_firmware_version_copy(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_device_id_copy(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_uptime_seconds() -> u64;
    fn slint_bridge_get_free_heap_bytes() -> u32;
    fn slint_bridge_get_total_heap_bytes() -> u32;
    fn slint_bridge_get_free_psram_bytes() -> u32;
    fn slint_bridge_get_total_psram_bytes() -> u32;
    fn slint_bridge_get_chip_temp_c(out_c: *mut f32) -> bool;
    fn slint_bridge_get_board_profile() -> u8;
    fn slint_bridge_get_display_width() -> i32;
    fn slint_bridge_get_display_height() -> i32;
    fn slint_bridge_get_language_is_ua() -> bool;
    fn slint_bridge_set_language_is_ua(is_ua: bool);
    fn slint_bridge_get_temp_unit() -> u8;
    fn slint_bridge_set_temp_unit(unit: u8);
    fn slint_bridge_get_time_format() -> u8;
    fn slint_bridge_set_time_format(fmt: u8);
    fn slint_bridge_get_date_format() -> u8;
    fn slint_bridge_set_date_format(fmt: u8);
    fn slint_bridge_get_display_brightness() -> u8;
    fn slint_bridge_set_display_brightness(percent: u8);
    fn slint_bridge_set_time_hm(hour: u8, minute: u8) -> bool;
    fn slint_bridge_set_date_ymd(year: u16, month: u8, day: u8) -> bool;

    fn display_driver_touch_probe(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_touch_probe_raw(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_set_touch_calibration(enabled: bool, left: u16, right: u16, top: u16, bottom: u16) -> bool;
    fn display_driver_set_touch_affine(enabled: bool, a: f32, b: f32, c: f32, d: f32, e: f32, f: f32) -> bool;
    fn display_driver_set_touch_grid(enabled: bool, dx: *const f32, dy: *const f32) -> bool;
    fn display_driver_blit_rgb565(x: i32, y: i32, w: i32, h: i32, data: *const u16) -> bool;
    fn display_driver_p4_begin_frame(out_fb: *mut *mut u16, out_w: *mut i32, out_h: *mut i32) -> bool;
    fn display_driver_p4_present_frame() -> bool;
    fn display_driver_p4_present_frame_region(x: i32, y: i32, w: i32, h: i32) -> bool;
    fn display_driver_p4_cancel_frame();
    fn slint_bridge_wifi_last_got_ip_ms() -> u64;
}

#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct ScheduleStepFile {
    #[serde(rename = "type")]
    step_type: String,
    #[serde(default)]
    rate: Option<i32>,
    #[serde(default)]
    target: Option<i32>,
    #[serde(rename = "holdTime", default)]
    hold_time: Option<i32>,
    #[serde(default)]
    id: Option<serde_json::Value>,
    #[serde(default)]
    fan: Option<bool>,
    #[serde(flatten, default)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct ScheduleFile {
    #[serde(default)]
    id: String,
    name: String,
    #[serde(rename = "type", default)]
    schedule_type: String,
    #[serde(default)]
    created: String,
    #[serde(rename = "stepsCount", default)]
    steps_count: Option<i32>,
    #[serde(rename = "glassDetails", default)]
    glass_details: Option<serde_json::Value>,
    #[serde(default)]
    steps: Vec<ScheduleStepFile>,
    #[serde(flatten, default)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Clone, serde::Deserialize, Default)]
struct WifiScanAp {
    #[serde(default)]
    ssid: String,
    #[serde(default)]
    rssi: i32,
}

#[derive(Clone, serde::Deserialize, Default)]
struct HistorySample {
    #[serde(default, deserialize_with = "de_i64_any")]
    timestamp: i64,
    #[serde(alias = "temperature", alias = "actual", default, deserialize_with = "de_f32_any")]
    temp: f32,
    #[serde(alias = "setpoint", alias = "planned", default, deserialize_with = "de_f32_any")]
    target: f32,
    #[serde(default, deserialize_with = "de_f32_any")]
    voltage: f32,
    #[serde(default, deserialize_with = "de_f32_any")]
    current: f32,
    #[serde(default, deserialize_with = "de_f32_any")]
    power: f32,
}

#[allow(dead_code)]
#[derive(Clone, serde::Deserialize, Default)]
struct HistorySummary {
    #[serde(default, deserialize_with = "de_string_any")]
    id: String,
    #[serde(rename = "scheduleName", default, deserialize_with = "de_string_any")]
    schedule_name: String,
    #[serde(default, deserialize_with = "de_i64_any")]
    duration: i64,
    #[serde(default, deserialize_with = "de_string_any")]
    status: String,
    #[serde(rename = "statusCode", default, deserialize_with = "de_string_any")]
    status_code: String,
    #[serde(rename = "totalSteps", default, deserialize_with = "de_i64_any")]
    total_steps: i64,
    #[serde(rename = "completedSteps", default, deserialize_with = "de_i64_any")]
    completed_steps: i64,
    #[serde(rename = "startTime", default, deserialize_with = "de_string_any")]
    start_time: String,
    #[serde(rename = "endTime", default, deserialize_with = "de_string_any")]
    end_time: String,
    #[serde(rename = "startTs", default, deserialize_with = "de_i64_any")]
    start_ts: i64,
    #[serde(rename = "endTs", default, deserialize_with = "de_i64_any")]
    end_ts: i64,
    #[serde(rename = "energyKwh", default, deserialize_with = "de_f32_any")]
    energy_kwh: f32,
    #[serde(rename = "energyWh", default, deserialize_with = "de_f32_any")]
    energy_wh: f32,
    #[serde(rename = "cost", default, deserialize_with = "de_f32_any")]
    cost: f32,
    #[serde(rename = "peakPower", default, deserialize_with = "de_f32_any")]
    peak_power: f32,
    #[serde(rename = "peakCurrent", default, deserialize_with = "de_f32_any")]
    peak_current: f32,
    #[serde(rename = "peakTemp", default, deserialize_with = "de_f32_any")]
    peak_temp: f32,
    #[serde(rename = "faultReason", default, deserialize_with = "de_string_any")]
    fault_reason: String,
}

#[derive(Clone, serde::Deserialize, Default)]
struct HistoryDetail {
    #[serde(default)]
    summary: HistorySummary,
    #[serde(default)]
    data: Vec<HistorySample>,
    #[serde(default)]
    planned: Vec<HistoryCurvePoint>,
    #[serde(default)]
    actual: Vec<HistoryCurvePoint>,
    #[serde(default)]
    changes: Vec<HistoryChange>,
}

#[derive(Clone, serde::Deserialize, Default)]
struct HistoryCurvePoint {
    #[serde(alias = "t", alias = "time", alias = "x", default, deserialize_with = "de_f32_any")]
    t: f32,
    #[serde(alias = "temp", alias = "y", alias = "target", alias = "value", default, deserialize_with = "de_f32_any")]
    temp: f32,
    #[serde(alias = "timestamp", default, deserialize_with = "de_i64_any")]
    timestamp: i64,
}

#[derive(Clone, serde::Deserialize, Default)]
struct HistoryChange {
    #[serde(rename = "ts_ms", alias = "timestamp", default)]
    ts_ms: i64,
    #[serde(default)]
    step: i64,
    #[serde(default)]
    action: String,
    #[serde(default)]
    field: String,
    #[serde(default)]
    before: f32,
    #[serde(default)]
    after: f32,
    #[serde(default)]
    delta: f32,
}

#[derive(Clone)]
struct GraphTapPoint {
    x: i32,
    y: i32,
    temp_label: String,
    time_label: String,
}

struct AppState {
    schedules: Vec<ScheduleFile>,
    selected_index: Option<usize>,
    editing_index: Option<usize>,
    graph_index: Option<usize>,
    graph_zoom_level: i32,
    graph_points: Vec<GraphTapPoint>,
    history_graph_id: Option<String>,
    history_detail_id: Option<String>,
    editor_steps: Vec<EditorStepRow>,
    display_temp_c: f32,
    display_temp_valid: bool,
    temp_unit: TempUnit,
    time_format: TimeFormat,
    date_format: DateFormat,
}

#[derive(Clone, Copy, PartialEq)]
enum TempUnit {
    C,
    F,
}

#[derive(Clone, Copy, PartialEq)]
enum TimeFormat {
    H24,
    H12,
}

#[derive(Clone, Copy, PartialEq)]
enum DateFormat {
    Dmy,
    Mdy,
    Ymd,
}

fn de_string_any<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: Deserializer<'de>,
{
    let v = serde_json::Value::deserialize(deserializer)?;
    Ok(match v {
        serde_json::Value::String(s) => s,
        serde_json::Value::Number(n) => n.to_string(),
        serde_json::Value::Bool(b) => {
            if b { "true".to_string() } else { "false".to_string() }
        }
        _ => String::new(),
    })
}

fn de_i64_any<'de, D>(deserializer: D) -> Result<i64, D::Error>
where
    D: Deserializer<'de>,
{
    let v = serde_json::Value::deserialize(deserializer)?;
    Ok(match v {
        serde_json::Value::Number(n) => n.as_i64().or_else(|| n.as_f64().map(|f| f as i64)).unwrap_or(0),
        serde_json::Value::String(s) => s.trim().parse::<i64>().ok().unwrap_or(0),
        serde_json::Value::Bool(b) => if b { 1 } else { 0 },
        _ => 0,
    })
}

fn de_f32_any<'de, D>(deserializer: D) -> Result<f32, D::Error>
where
    D: Deserializer<'de>,
{
    let v = serde_json::Value::deserialize(deserializer)?;
    Ok(match v {
        serde_json::Value::Number(n) => n.as_f64().map(|f| f as f32).unwrap_or(0.0),
        serde_json::Value::String(s) => s.trim().parse::<f32>().ok().unwrap_or(0.0),
        serde_json::Value::Bool(b) => if b { 1.0 } else { 0.0 },
        _ => 0.0,
    })
}

fn c_to_f(c: f32) -> f32 {
    c * 9.0 / 5.0 + 32.0
}

fn delta_temp_to_display_i32(delta_c: f32, unit: TempUnit) -> i32 {
    match unit {
        TempUnit::C => delta_c.round() as i32,
        TempUnit::F => (delta_c * 9.0 / 5.0).round() as i32,
    }
}

fn delta_temp_to_c_from_display(delta_display: i32, unit: TempUnit) -> f32 {
    match unit {
        TempUnit::C => delta_display as f32,
        TempUnit::F => delta_display as f32 * 5.0 / 9.0,
    }
}

fn f_to_c(f: f32) -> f32 {
    (f - 32.0) * 5.0 / 9.0
}

fn temp_to_display(value_c: f32, unit: TempUnit) -> f32 {
    match unit {
        TempUnit::C => value_c,
        TempUnit::F => c_to_f(value_c),
    }
}

fn temp_to_display_i32(value_c: i32, unit: TempUnit) -> i32 {
    temp_to_display(value_c as f32, unit).round() as i32
}

fn temp_to_c_from_display(value: i32, unit: TempUnit) -> i32 {
    match unit {
        TempUnit::C => value,
        TempUnit::F => f_to_c(value as f32).round() as i32,
    }
}

fn rate_to_display(value_c: i32, unit: TempUnit) -> i32 {
    match unit {
        TempUnit::C => value_c,
        TempUnit::F => (value_c as f32 * 9.0 / 5.0).round() as i32,
    }
}

fn rate_to_c_from_display(value: i32, unit: TempUnit) -> i32 {
    match unit {
        TempUnit::C => value,
        TempUnit::F => (value as f32 * 5.0 / 9.0).round() as i32,
    }
}

fn update_unit_labels(ui: &AppWindow, unit: TempUnit) {
    ui.set_temp_unit(match unit { TempUnit::C => "C", TempUnit::F => "F" }.into());
    ui.set_temp_unit_label(match unit { TempUnit::C => "\u{00B0}C", TempUnit::F => "\u{00B0}F" }.into());
    ui.set_rate_unit_label(match unit { TempUnit::C => "\u{00B0}C/h", TempUnit::F => "\u{00B0}F/h" }.into());
    ui.set_rate_unit_label_min(match unit { TempUnit::C => "\u{00B0}C/min", TempUnit::F => "\u{00B0}F/min" }.into());
}

fn apply_temp_unit_change(ui: &AppWindow, state: &mut AppState, new_unit: TempUnit) {
    if state.temp_unit == new_unit {
        return;
    }
    let old_unit = state.temp_unit;

    for step in &mut state.editor_steps {
        let rate_c = rate_to_c_from_display(step.rate, old_unit);
        step.rate = rate_to_display(rate_c, new_unit);
        let target_c = temp_to_c_from_display(step.target, old_unit);
        step.target = temp_to_display_i32(target_c, new_unit);
    }
    refresh_editor_model(ui, state);

    let temp_c = match old_unit {
        TempUnit::C => ui.get_temp(),
        TempUnit::F => f_to_c(ui.get_temp()),
    };
    let target_c = temp_to_c_from_display(ui.get_target(), old_unit);
    let seg_target_c = temp_to_c_from_display(ui.get_active_segment_target(), old_unit);
    let max_target_c = temp_to_c_from_display(ui.get_schedule_target_max_c(), old_unit);
    let autotune_c = temp_to_c_from_display(ui.get_autotune_target_c(), old_unit);
    let run_temp_c = temp_to_c_from_display(ui.get_running_edit_add_temp(), old_unit);
    let run_rate_c = rate_to_c_from_display(ui.get_running_edit_rate(), old_unit);
    let offset_c = delta_temp_to_c_from_display(ui.get_thermocouple_offset(), old_unit);

    ui.set_temp(temp_to_display(temp_c, new_unit));
    ui.set_temp_label(format!("{:.1}", temp_to_display(temp_c, new_unit)).into());
    ui.set_target(temp_to_display_i32(target_c, new_unit));
    ui.set_active_segment_target(temp_to_display_i32(seg_target_c, new_unit));
    ui.set_schedule_target_max_c(temp_to_display_i32(max_target_c, new_unit));
    ui.set_autotune_target_c(temp_to_display_i32(autotune_c, new_unit));
    ui.set_running_edit_add_temp(temp_to_display_i32(run_temp_c, new_unit));
    ui.set_running_edit_rate(rate_to_display(run_rate_c, new_unit));
    ui.set_thermocouple_offset(delta_temp_to_display_i32(offset_c, new_unit));

    state.temp_unit = new_unit;
    update_unit_labels(ui, new_unit);
    refresh_schedule_graph_model(ui, state);
}

struct EspPlatform {
    window: Rc<MinimalSoftwareWindow>,
    start: Instant,
}

impl Platform for EspPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn slint::platform::WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        self.start.elapsed()
    }
}

struct DisplayLineBuffer<'a> {
    line_buffer: &'a mut [Rgb565Pixel],
}

impl<'a> LineBufferProvider for DisplayLineBuffer<'a> {
    type TargetPixel = Rgb565Pixel;

    fn process_line(
        &mut self,
        line: usize,
        range: core::ops::Range<usize>,
        render_fn: impl FnOnce(&mut [Self::TargetPixel]),
    ) {
        let start = range.start.min(self.line_buffer.len());
        let end = range.end.min(self.line_buffer.len());
        if end <= start {
            return;
        }
        render_fn(&mut self.line_buffer[start..end]);
        unsafe {
            let ptr = self.line_buffer[start..end].as_ptr() as *const u16;
            let _ = display_driver_blit_rgb565(
                start as i32,
                line as i32,
                (end - start) as i32,
                1,
                ptr,
            );
        }
    }
}

struct BufferedDisplayLineBuffer<'a> {
    line_buffer: &'a mut [Rgb565Pixel],
    frame_buffer: &'a mut [Rgb565Pixel],
    frame_width: usize,
    had_updates: &'a mut bool,
    dirty_min_x: &'a mut i32,
    dirty_min_y: &'a mut i32,
    dirty_max_x: &'a mut i32,
    dirty_max_y: &'a mut i32,
}

impl<'a> LineBufferProvider for BufferedDisplayLineBuffer<'a> {
    type TargetPixel = Rgb565Pixel;

    fn process_line(
        &mut self,
        line: usize,
        range: core::ops::Range<usize>,
        render_fn: impl FnOnce(&mut [Self::TargetPixel]),
    ) {
        let start = range.start.min(self.line_buffer.len());
        let end = range.end.min(self.line_buffer.len());
        if end <= start {
            return;
        }

        render_fn(&mut self.line_buffer[start..end]);

        let row_start = line.saturating_mul(self.frame_width);
        let dst_start = row_start.saturating_add(start);
        let dst_end = row_start.saturating_add(end);
        if dst_end <= self.frame_buffer.len() && dst_end > dst_start {
            self.frame_buffer[dst_start..dst_end].copy_from_slice(&self.line_buffer[start..end]);
            *self.had_updates = true;
            let x1 = start as i32;
            let x2 = end.saturating_sub(1) as i32;
            let y = line as i32;
            if x1 < *self.dirty_min_x { *self.dirty_min_x = x1; }
            if y < *self.dirty_min_y { *self.dirty_min_y = y; }
            if x2 > *self.dirty_max_x { *self.dirty_max_x = x2; }
            if y > *self.dirty_max_y { *self.dirty_max_y = y; }
        }
    }
}

struct P4RotatedLineBuffer<'a> {
    line_buffer: &'a mut [Rgb565Pixel],
    frame_buffer: &'a mut [u16],
    frame_width: usize,
    src_width: usize,
    src_height: usize,
    had_updates: &'a mut bool,
    dirty_min_x: &'a mut i32,
    dirty_min_y: &'a mut i32,
    dirty_max_x: &'a mut i32,
    dirty_max_y: &'a mut i32,
}

impl<'a> LineBufferProvider for P4RotatedLineBuffer<'a> {
    type TargetPixel = Rgb565Pixel;

    fn process_line(
        &mut self,
        line: usize,
        range: core::ops::Range<usize>,
        render_fn: impl FnOnce(&mut [Self::TargetPixel]),
    ) {
        if line >= self.src_height || line >= self.frame_width {
            return;
        }
        let start = range.start.min(self.line_buffer.len());
        let end = range.end.min(self.line_buffer.len());
        if end <= start {
            return;
        }

        render_fn(&mut self.line_buffer[start..end]);

        let src_width = self.src_width.max(1);
        let end = end.min(src_width);
        if start >= end {
            return;
        }

        let raw_line = unsafe {
            std::slice::from_raw_parts(self.line_buffer.as_ptr() as *const u16, self.line_buffer.len())
        };

        for x in start..end {
            let dst_x = line;
            let dst_y = src_width - 1 - x;
            let dst_index = dst_y.saturating_mul(self.frame_width).saturating_add(dst_x);
            if dst_index < self.frame_buffer.len() {
                self.frame_buffer[dst_index] = raw_line[x];
                *self.had_updates = true;
            }
        }
        let px = line as i32;
        let py1 = (src_width.saturating_sub(end)) as i32;
        let py2 = (src_width.saturating_sub(1 + start)) as i32;
        if px < *self.dirty_min_x { *self.dirty_min_x = px; }
        if py1 < *self.dirty_min_y { *self.dirty_min_y = py1; }
        if px > *self.dirty_max_x { *self.dirty_max_x = px; }
        if py2 > *self.dirty_max_y { *self.dirty_max_y = py2; }
    }
}

fn format_minutes_to_hm(minutes: i32) -> String {
    if minutes < 0 {
        return "--:--".to_string();
    }
    let hours = minutes / 60;
    let mins = minutes % 60;
    format!("{:02}:{:02}", hours, mins)
}

fn translate_fault_reason(reason: &str, is_ua: bool) -> String {
    if !is_ua {
        return reason.to_string();
    }
    match reason {
        "Autotune sensor fault" => "\u{041F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430} \u{043F}\u{0456}\u{0434} \u{0447}\u{0430}\u{0441} \u{0430}\u{0432}\u{0442}\u{043E}\u{043D}\u{0430}\u{043B}\u{0430}\u{0448}\u{0442}\u{0443}\u{0432}\u{0430}\u{043D}\u{043D}\u{044F}".to_string(),
        "Autotune timeout" => "\u{0410}\u{0432}\u{0442}\u{043E}\u{043D}\u{0430}\u{043B}\u{0430}\u{0448}\u{0442}\u{0443}\u{0432}\u{0430}\u{043D}\u{043D}\u{044F}: \u{0442}\u{0430}\u{0439}\u{043C}\u{0430}\u{0443}\u{0442}".to_string(),
        "Autotune insufficient data" => "\u{0410}\u{0432}\u{0442}\u{043E}\u{043D}\u{0430}\u{043B}\u{0430}\u{0448}\u{0442}\u{0443}\u{0432}\u{0430}\u{043D}\u{043D}\u{044F}: \u{043D}\u{0435}\u{0434}\u{043E}\u{0441}\u{0442}\u{0430}\u{0442}\u{043D}\u{044C}\u{043E} \u{0434}\u{0430}\u{043D}\u{0438}\u{0445}".to_string(),
        "Sensor comm/no-data" => "\u{041D}\u{0435}\u{043C}\u{0430}\u{0454} \u{0434}\u{0430}\u{043D}\u{0438}\u{0445} \u{0432}\u{0456}\u{0434} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Sensor open" => "\u{041E}\u{0431}\u{0440}\u{0438}\u{0432} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Sensor short" => "\u{041A}\u{043E}\u{0440}\u{043E}\u{0442}\u{043A}\u{0435} \u{0437}\u{0430}\u{043C}\u{0438}\u{043A}\u{0430}\u{043D}\u{043D}\u{044F} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Sensor NaN/open" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0435} \u{0437}\u{043D}\u{0430}\u{0447}\u{0435}\u{043D}\u{043D}\u{044F} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Sensor out of range" => "\u{0422}\u{0435}\u{043C}\u{043F}\u{0435}\u{0440}\u{0430}\u{0442}\u{0443}\u{0440}\u{0430} \u{043F}\u{043E}\u{0437}\u{0430} \u{0434}\u{0456}\u{0430}\u{043F}\u{0430}\u{0437}\u{043E}\u{043D}\u{043E}\u{043C}".to_string(),
        "Sensor spike/out of range" => "\u{0421}\u{0442}\u{0440}\u{0438}\u{0431}\u{043E}\u{043A} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430} \u{043F}\u{043E}\u{0437}\u{0430} \u{043C}\u{0435}\u{0436}\u{0430}\u{043C}\u{0438}".to_string(),
        "Sensor stuck" => "\u{0421}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440} \u{0437}\u{0430}\u{0432}\u{0438}\u{0441} \u{043D}\u{0430} \u{043E}\u{0434}\u{043D}\u{043E}\u{043C}\u{0443} \u{0437}\u{043D}\u{0430}\u{0447}\u{0435}\u{043D}\u{043D}\u{0456}".to_string(),
        "Sensor not stable yet" => "\u{0421}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440} \u{0449}\u{0435} \u{043D}\u{0435} \u{0441}\u{0442}\u{0430}\u{0431}\u{0456}\u{043B}\u{0456}\u{0437}\u{0443}\u{0432}\u{0430}\u{0432}\u{0441}\u{044F}".to_string(),
        "Sensor comm fault" => "\u{041F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430} \u{0437}\u{0432}'\u{044F}\u{0437}\u{043A}\u{0443} \u{0437} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{043E}\u{043C}".to_string(),
        "Sensor open fault" => "\u{041E}\u{0431}\u{0440}\u{0438}\u{0432} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Sensor short fault" => "\u{041A}\u{043E}\u{0440}\u{043E}\u{0442}\u{043A}\u{0435} \u{0437}\u{0430}\u{043C}\u{0438}\u{043A}\u{0430}\u{043D}\u{043D}\u{044F} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}\u{0430}".to_string(),
        "Temperature read timeout" => "\u{0422}\u{0430}\u{0439}\u{043C}\u{0430}\u{0443}\u{0442} \u{0437}\u{0447}\u{0438}\u{0442}\u{0443}\u{0432}\u{0430}\u{043D}\u{043D}\u{044F} \u{0442}\u{0435}\u{043C}\u{043F}\u{0435}\u{0440}\u{0430}\u{0442}\u{0443}\u{0440}\u{0438}".to_string(),
        "Temperature runaway" => "\u{0410}\u{0432}\u{0430}\u{0440}\u{0456}\u{0439}\u{043D}\u{0438}\u{0439} \u{043F}\u{0435}\u{0440}\u{0435}\u{0433}\u{0440}\u{0456}\u{0432}".to_string(),
        "Unknown fault" => "\u{041D}\u{0435}\u{0432}\u{0456}\u{0434}\u{043E}\u{043C}\u{0430} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        _ => reason.to_string(),
    }
}

fn translate_command_message(message: &str, is_ua: bool) -> String {
    if !is_ua {
        return message.to_string();
    }
    match message {
        "ok" => "\u{0443}\u{0441}\u{043F}\u{0456}\u{0448}\u{043D}\u{043E}".to_string(),
        "error" => "\u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        "internal" => "\u{0432}\u{043D}\u{0443}\u{0442}\u{0440}\u{0456}\u{0448}\u{043D}\u{044F} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        "started" => "\u{0437}\u{0430}\u{043F}\u{0443}\u{0449}\u{0435}\u{043D}\u{043E}".to_string(),
        "stopped" => "\u{0437}\u{0443}\u{043F}\u{0438}\u{043D}\u{0435}\u{043D}\u{043E}".to_string(),
        "loaded" => "\u{0437}\u{0430}\u{0432}\u{0430}\u{043D}\u{0442}\u{0430}\u{0436}\u{0435}\u{043D}\u{043E}".to_string(),
        "cleared" => "\u{0441}\u{043A}\u{0438}\u{043D}\u{0443}\u{0442}\u{043E}".to_string(),
        "clear_rejected" => "\u{0441}\u{043A}\u{0438}\u{0434}\u{0430}\u{043D}\u{043D}\u{044F} \u{0432}\u{0456}\u{0434}\u{0445}\u{0438}\u{043B}\u{0435}\u{043D}\u{043E}".to_string(),
        "busy" => "\u{0437}\u{0430}\u{0447}\u{0435}\u{043A}\u{0430}\u{0439}\u{0442}\u{0435}, \u{043A}\u{043E}\u{043C}\u{0430}\u{043D}\u{0434}\u{0430} \u{0432}\u{0438}\u{043A}\u{043E}\u{043D}\u{0443}\u{0454}\u{0442}\u{044C}\u{0441}\u{044F}".to_string(),
        "invalid_payload" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0456} \u{0434}\u{0430}\u{043D}\u{0456}".to_string(),
        "invalid_schedule" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0430} \u{043F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0430}".to_string(),
        "no_schedule" => "\u{041F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0443} \u{043D}\u{0435} \u{0437}\u{0430}\u{0432}\u{0430}\u{043D}\u{0442}\u{0430}\u{0436}\u{0435}\u{043D}\u{043E}".to_string(),
        "sensor_invalid" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0438}\u{0439} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}".to_string(),
        "touch_not_calibrated" => "\u{0422}\u{0430}\u{0447} \u{043D}\u{0435} \u{0432}\u{0456}\u{0434}\u{043A}\u{0430}\u{043B}\u{0456}\u{0431}\u{0440}\u{043E}\u{0432}\u{0430}\u{043D}\u{0438}\u{0439}".to_string(),
        "fault_active" => "\u{0410}\u{043A}\u{0442}\u{0438}\u{0432}\u{043D}\u{0430} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        "fan_unsafe" => "\u{041D}\u{0435}\u{0431}\u{0435}\u{0437}\u{043F}\u{0435}\u{0447}\u{043D}\u{0430} \u{043A}\u{043E}\u{043D}\u{0444}\u{0456}\u{0433}\u{0443}\u{0440}\u{0430}\u{0446}\u{0456}\u{044F} \u{0432}\u{0435}\u{043D}\u{0442}\u{0438}\u{043B}\u{044F}\u{0442}\u{043E}\u{0440}\u{0430}".to_string(),
        "schedule_over_max_temp" => "\u{041F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0430} \u{043F}\u{0435}\u{0440}\u{0435}\u{0432}\u{0438}\u{0449}\u{0443}\u{0454} maxC".to_string(),
        "clock_not_set" => "\u{0413}\u{043E}\u{0434}\u{0438}\u{043D}\u{043D}\u{0438}\u{043A} RTC/\u{0441}\u{0438}\u{0441}\u{0442}\u{0435}\u{043C}\u{043D}\u{0438}\u{0439} \u{0447}\u{0430}\u{0441} \u{043D}\u{0435} \u{0432}\u{0441}\u{0442}\u{0430}\u{043D}\u{043E}\u{0432}\u{043B}\u{0435}\u{043D}\u{043E}".to_string(),
        "rate_limited" => "\u{0417}\u{0430}\u{0431}\u{0430}\u{0433}\u{0430}\u{0442}\u{043E} \u{0437}\u{0430}\u{043F}\u{0438}\u{0442}\u{0456}\u{0432}".to_string(),
        "Sensor invalid" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0438}\u{0439} \u{0441}\u{0435}\u{043D}\u{0441}\u{043E}\u{0440}".to_string(),
        "Invalid payload" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0456} \u{0434}\u{0430}\u{043D}\u{0456}".to_string(),
        "Invalid schedule" => "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0430} \u{043F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0430}".to_string(),
        "No schedule loaded" => "\u{041F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0443} \u{043D}\u{0435} \u{0437}\u{0430}\u{0432}\u{0430}\u{043D}\u{0442}\u{0430}\u{0436}\u{0435}\u{043D}\u{043E}".to_string(),
        "Touch not calibrated" => "\u{0422}\u{0430}\u{0447} \u{043D}\u{0435} \u{0432}\u{0456}\u{0434}\u{043A}\u{0430}\u{043B}\u{0456}\u{0431}\u{0440}\u{043E}\u{0432}\u{0430}\u{043D}\u{0438}\u{0439}".to_string(),
        "Fault active" => "\u{0410}\u{043A}\u{0442}\u{0438}\u{0432}\u{043D}\u{0430} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        "Fan configuration unsafe" => "\u{041D}\u{0435}\u{0431}\u{0435}\u{0437}\u{043F}\u{0435}\u{0447}\u{043D}\u{0430} \u{043A}\u{043E}\u{043D}\u{0444}\u{0456}\u{0433}\u{0443}\u{0440}\u{0430}\u{0446}\u{0456}\u{044F} \u{0432}\u{0435}\u{043D}\u{0442}\u{0438}\u{043B}\u{044F}\u{0442}\u{043E}\u{0440}\u{0430}".to_string(),
        "Schedule exceeds max temperature" => "\u{041F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0430} \u{043F}\u{0435}\u{0440}\u{0435}\u{0432}\u{0438}\u{0449}\u{0443}\u{0454} maxC".to_string(),
        "RTC/system clock not set" => "\u{0413}\u{043E}\u{0434}\u{0438}\u{043D}\u{043D}\u{0438}\u{043A} RTC/\u{0441}\u{0438}\u{0441}\u{0442}\u{0435}\u{043C}\u{043D}\u{0438}\u{0439} \u{0447}\u{0430}\u{0441} \u{043D}\u{0435} \u{0432}\u{0441}\u{0442}\u{0430}\u{043D}\u{043E}\u{0432}\u{043B}\u{0435}\u{043D}\u{043E}".to_string(),
        "Too many requests" => "\u{0417}\u{0430}\u{0431}\u{0430}\u{0433}\u{0430}\u{0442}\u{043E} \u{0437}\u{0430}\u{043F}\u{0438}\u{0442}\u{0456}\u{0432}".to_string(),
        "Internal error" => "\u{0412}\u{043D}\u{0443}\u{0442}\u{0440}\u{0456}\u{0448}\u{043D}\u{044F} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string(),
        _ => message.to_string(),
    }
}

fn load_schedules() -> Vec<ScheduleFile> {
    let path = "/littlefs/schedules.json";
    let Ok(data) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    let mut schedules: Vec<ScheduleFile> = serde_json::from_str(&data).unwrap_or_default();
    for s in &mut schedules {
                    s.name = sanitize_display_text(&s.name);
    }
    schedules
}

fn save_schedules(schedules: &[ScheduleFile]) -> bool {
    let path = "/littlefs/schedules.json";
    let mut normalized = schedules.to_vec();
    for s in &mut normalized {
        s.steps_count = Some(s.steps.len() as i32);
        if s.id.is_empty() {
            s.id = make_safe_id(&s.name);
        }
        if s.schedule_type.is_empty() {
            s.schedule_type = "Custom".to_string();
        }
    }
    let Ok(json) = serde_json::to_string(&normalized) else {
        return false;
    };
    std::fs::write(path, json).is_ok()
}

fn truncate_preview(mut text: String, max_chars: usize) -> String {
    if text.chars().count() > max_chars {
        text = text.chars().take(max_chars).collect::<String>();
        text.push_str("\n...");
    }
    text
}

fn compact_duration_label(seconds: i64, _is_ua: bool) -> String {
    if seconds <= 0 {
        return "duration unknown".to_string();
    }
    let hours = seconds / 3600;
    let minutes = (seconds % 3600) / 60;
    if hours > 0 {
        format!("{}h {}m", hours, minutes)
    } else {
        format!("{}m", minutes.max(1))
    }
}

fn unix_seconds_to_ymdhm_utc(ts_sec: i64) -> Option<(i32, u32, u32, u32, u32)> {
    if ts_sec < 0 {
        return None;
    }
    let days = ts_sec.div_euclid(86_400);
    let sec_of_day = ts_sec.rem_euclid(86_400);
    let hour = (sec_of_day / 3_600) as u32;
    let minute = ((sec_of_day % 3_600) / 60) as u32;

    // Civil date from days since Unix epoch (UTC).
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let mut y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = mp + if mp < 10 { 3 } else { -9 };
    y += if m <= 2 { 1 } else { 0 };

    Some((y as i32, m as u32, d as u32, hour, minute))
}

fn current_unix_ms() -> Option<i64> {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?;
    Some(now.as_millis() as i64)
}

fn normalize_history_start_ms(item: &serde_json::Value) -> Option<i64> {
    let mut start_ms = item.get("startTime").and_then(|v| v.as_i64()).unwrap_or(0);
    if start_ms <= 0 {
        start_ms = item
            .get("startTs")
            .or_else(|| item.get("startTimestamp"))
            .and_then(|v| v.as_i64())
            .unwrap_or(0);
    }
    if start_ms <= 0 {
        return None;
    }

    // Convert seconds to milliseconds when needed.
    if start_ms > 0 && start_ms < 10_000_000_000 {
        start_ms *= 1000;
    }

    // Valid Unix time in ms.
    if start_ms >= 1_700_000_000_000 {
        return Some(start_ms);
    }

    // Older records may store uptime-ms; recover approximate Unix time.
    let uptime_ms = unsafe { slint_bridge_get_uptime_seconds() }.saturating_mul(1000) as i64;
    let now_ms = current_unix_ms().unwrap_or(0);
    if now_ms >= 1_700_000_000_000 && uptime_ms > 0 && start_ms <= uptime_ms {
        return Some(now_ms.saturating_sub(uptime_ms.saturating_sub(start_ms)));
    }

    None
}

fn load_history_items_sorted() -> Option<Vec<serde_json::Value>> {
    let data = std::fs::read_to_string("/littlefs/history.json").ok()?;
    let value = serde_json::from_str::<serde_json::Value>(data.trim()).ok()?;
    let items = value.as_array()?;
    let mut sorted: Vec<serde_json::Value> = items.clone();
    sorted.sort_by(|a, b| {
        let a_ts = normalize_history_start_ms(a).unwrap_or(0);
        let b_ts = normalize_history_start_ms(b).unwrap_or(0);
        b_ts.cmp(&a_ts)
    });
    Some(sorted)
}

fn history_start_label(item: &serde_json::Value, is_ua: bool) -> Option<String> {
    let start_ms = normalize_history_start_ms(item)?;
    let (y, m, d, hh, mm) = unix_seconds_to_ymdhm_utc(start_ms / 1000)?;
    let dt = format!("{:04}-{:02}-{:02} {:02}:{:02}", y, m, d, hh, mm);
    Some(if is_ua {
        format!("\u{0421}\u{0442}\u{0430}\u{0440}\u{0442} {}", dt)
    } else {
        format!("Start {}", dt)
    })
}

fn ui_clean_text(raw: &str, fallback: &str) -> String {
    let cleaned: String = raw
        .chars()
        .filter(|c| !c.is_control())
        .collect();
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        fallback.to_string()
    } else {
        trimmed.to_string()
    }
}

fn ui_clean_multiline(raw: &str, fallback: &str) -> String {
    let cleaned: String = raw
        .chars()
        .filter(|c| *c == '\n' || !c.is_control())
        .collect();
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        fallback.to_string()
    } else {
        trimmed.to_string()
    }
}

fn history_status_label(status: &str, _is_ua: bool) -> String {
    match status {
        "COMPLETE" => "COMPLETE".to_string(),
        "STOPPED" => "STOPPED".to_string(),
        "ERROR" => "ERROR".to_string(),
        "COOL" => "COOL".to_string(),
        other if !other.is_empty() => other.to_string(),
        _ => "UNKNOWN".to_string(),
    }
}

fn history_status_code(raw: &str) -> String {
    raw.trim().to_ascii_uppercase()
}

fn load_history_items(is_ua: bool) -> Vec<HistoryPreviewCard> {
    let Ok(data) = std::fs::read_to_string("/littlefs/history.json") else {
        return vec![HistoryPreviewCard {
            id: "".into(),
            title: (if is_ua { "\u{0406}\u{0441}\u{0442}\u{043E}\u{0440}\u{0456}\u{044F} \u{0432}\u{0456}\u{0434}\u{043F}\u{0430}\u{043B}\u{0456}\u{0432} \u{043F}\u{043E}\u{043A}\u{0438} \u{043F}\u{043E}\u{0440}\u{043E}\u{0436}\u{043D}\u{044F}." } else { "Firing history is empty." }).into(),
            status: "".into(),
            status_code: "".into(),
            subtitle: "".into(),
        }];
    };
    let trimmed = data.trim();
    if trimmed.is_empty() || trimmed == "[]" {
        return vec![HistoryPreviewCard {
            id: "".into(),
            title: (if is_ua { "\u{0406}\u{0441}\u{0442}\u{043E}\u{0440}\u{0456}\u{044F} \u{0432}\u{0456}\u{0434}\u{043F}\u{0430}\u{043B}\u{0456}\u{0432} \u{043F}\u{043E}\u{043A}\u{0438} \u{043F}\u{043E}\u{0440}\u{043E}\u{0436}\u{043D}\u{044F}." } else { "Firing history is empty." }).into(),
            status: "".into(),
            status_code: "".into(),
            subtitle: "".into(),
        }];
    }
    if let Some(items) = load_history_items_sorted() {
        if !items.is_empty() {
            let mut cards = Vec::new();
            for item in &items {
                let name = item
                    .get("name")
                    .or_else(|| item.get("scheduleName"))
                    .or_else(|| item.get("program"))
                    .and_then(|v| v.as_str())
                    .unwrap_or(if is_ua { "\u{0411}\u{0435}\u{0437} \u{043D}\u{0430}\u{0437}\u{0432}\u{0438}" } else { "Untitled" });
                let name = sanitize_display_text(name);
                let raw_status = item
                    .get("status")
                    .or_else(|| item.get("result"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let status_code = history_status_code(raw_status);
                let id = item
                    .get("id")
                    .or_else(|| item.get("runId"))
                    .or_else(|| item.get("historyId"))
                    .map(|v| {
                        v.as_str()
                            .map(|s| s.to_string())
                            .unwrap_or_else(|| v.to_string().trim_matches('"').to_string())
                    })
                    .unwrap_or_default();
                let duration = item.get("duration").and_then(|v| v.as_i64()).unwrap_or(0);
                let peak_temp = item.get("peakTemp").and_then(|v| v.as_f64()).unwrap_or(0.0);
                let total_steps = item.get("totalSteps").and_then(|v| v.as_i64()).unwrap_or(0);
                let completed_steps = item.get("completedSteps").and_then(|v| v.as_i64()).unwrap_or(0);

                let mut subtitle_parts = Vec::new();
                if let Some(start_label) = history_start_label(item, is_ua) {
                    subtitle_parts.push(start_label);
                }
                subtitle_parts.push(compact_duration_label(duration, is_ua));
                if peak_temp > 0.0 {
                    subtitle_parts.push(format!("Tmax {:.0}\u{00B0}C", peak_temp));
                }
                if total_steps > 0 {
                    subtitle_parts.push(format!("steps {}/{}", completed_steps.max(0), total_steps));
                }

                cards.push(HistoryPreviewCard {
                    id: id.into(),
                    title: truncate_preview(name, 48).into(),
                    status: history_status_label(raw_status, is_ua).into(),
                    status_code: status_code.into(),
                    subtitle: truncate_preview(subtitle_parts.join(" | "), 72).into(),
                });
            }
            if !cards.is_empty() {
                return cards;
            }
        }
    }
    vec![HistoryPreviewCard {
        id: "".into(),
        title: truncate_preview(trimmed.to_string(), 64).into(),
        status: "".into(),
        status_code: "".into(),
        subtitle: "".into(),
    }]
}

fn load_history_item_id_by_preview_index(index: usize) -> Option<String> {
    let items = load_history_items_sorted()?;
    let item = items.get(index)?;
    item.get("id")
        .or_else(|| item.get("runId"))
        .or_else(|| item.get("historyId"))
        .map(|v| {
            v.as_str()
                .map(|s| s.to_string())
                .unwrap_or_else(|| v.to_string().trim_matches('"').to_string())
        })
        .filter(|id| !id.is_empty())
}

fn history_summary_from_item(item: &serde_json::Value) -> HistorySummary {
    let get_str = |keys: &[&str]| -> String {
        keys.iter()
            .find_map(|k| item.get(*k).and_then(|v| v.as_str()))
            .unwrap_or("")
            .to_string()
    };
    let get_i64 = |keys: &[&str]| -> i64 {
        keys.iter()
            .find_map(|k| item.get(*k).and_then(|v| v.as_i64()))
            .unwrap_or(0)
    };
    let get_f32 = |keys: &[&str]| -> f32 {
        keys.iter()
            .find_map(|k| item.get(*k).and_then(|v| v.as_f64()))
            .unwrap_or(0.0) as f32
    };
    let status = get_str(&["status", "result"]);
    let status_code = if !get_str(&["statusCode"]).is_empty() {
        get_str(&["statusCode"])
    } else {
        history_status_code(&status)
    };
    HistorySummary {
        id: get_str(&["id", "runId", "historyId"]),
        schedule_name: sanitize_display_text(&get_str(&["scheduleName", "name", "program", "title"])),
        duration: get_i64(&["duration"]),
        status,
        status_code,
        total_steps: get_i64(&["totalSteps", "stepsCount"]),
        completed_steps: get_i64(&["completedSteps"]),
        start_time: sanitize_display_text(&get_str(&["startTime"])),
        end_time: sanitize_display_text(&get_str(&["endTime"])),
        start_ts: get_i64(&["startTs", "startTimestamp", "startTime"]),
        end_ts: get_i64(&["endTs", "endTimestamp", "endTime"]),
        energy_kwh: get_f32(&["energyKwh", "energy_kwh"]),
        energy_wh: get_f32(&["energyWh", "energy_wh"]),
        cost: get_f32(&["cost"]),
        peak_power: get_f32(&["peakPower", "maxPower"]),
        peak_current: get_f32(&["peakCurrent", "maxCurrent"]),
        peak_temp: get_f32(&["peakTemp", "maxTemp"]),
        fault_reason: sanitize_display_text(&get_str(&["faultReason", "error", "reason"])),
    }
}

fn history_start_label_from_summary(summary: &HistorySummary, is_ua: bool) -> Option<String> {
    let mut ts = 0_i64;
    if !summary.start_time.is_empty() {
        let s = summary.start_time.trim();
        if s.chars().all(|c| c.is_ascii_digit()) {
            ts = s.parse::<i64>().unwrap_or(0);
        } else {
            return Some(summary.start_time.clone());
        }
    }
    if ts <= 0 {
        ts = summary.start_ts;
    }
    if ts <= 0 {
        return None;
    }
    if ts < 10_000_000_000 {
        ts *= 1000;
    }
    if ts < 1_700_000_000_000 {
        let uptime_ms = unsafe { slint_bridge_get_uptime_seconds() }.saturating_mul(1000) as i64;
        let now_ms = current_unix_ms().unwrap_or(0);
        if now_ms >= 1_700_000_000_000 && uptime_ms > 0 && ts <= uptime_ms {
            ts = now_ms.saturating_sub(uptime_ms.saturating_sub(ts));
        }
    }
    let (y, m, d, hh, mm) = unix_seconds_to_ymdhm_utc(ts / 1000)?;
    let dt = format!("{:04}-{:02}-{:02} {:02}:{:02}", y, m, d, hh, mm);
    Some(if is_ua { dt } else { dt })
}

fn history_end_label_from_summary(summary: &HistorySummary, _is_ua: bool) -> Option<String> {
    let mut ts = 0_i64;
    if !summary.end_time.is_empty() {
        let s = summary.end_time.trim();
        if s.chars().all(|c| c.is_ascii_digit()) {
            ts = s.parse::<i64>().unwrap_or(0);
        } else {
            return Some(summary.end_time.clone());
        }
    }
    if ts <= 0 {
        ts = summary.end_ts;
    }
    if ts <= 0 {
        return None;
    }
    if ts < 10_000_000_000 {
        ts *= 1000;
    }
    if ts < 1_700_000_000_000 {
        let uptime_ms = unsafe { slint_bridge_get_uptime_seconds() }.saturating_mul(1000) as i64;
        let now_ms = current_unix_ms().unwrap_or(0);
        if now_ms >= 1_700_000_000_000 && uptime_ms > 0 && ts <= uptime_ms {
            ts = now_ms.saturating_sub(uptime_ms.saturating_sub(ts));
        }
    }
    let (y, m, d, hh, mm) = unix_seconds_to_ymdhm_utc(ts / 1000)?;
    Some(format!("{:04}-{:02}-{:02} {:02}:{:02}", y, m, d, hh, mm))
}

fn duration_seconds_to_minutes(seconds: i64) -> i32 {
    if seconds <= 0 {
        0
    } else {
        ((seconds as f32) / 60.0).ceil() as i32
    }
}

fn load_history_summary_by_preview_index(index: usize) -> Option<HistorySummary> {
    let items = load_history_items_sorted()?;
    let item = items.get(index)?;
    Some(history_summary_from_item(item))
}

fn load_history_detail(id: &str) -> Option<HistoryDetail> {
    let path = format!("/littlefs/history_{}.json", id);
    let data = std::fs::read_to_string(path).ok()?;
    serde_json::from_str::<HistoryDetail>(data.trim()).ok()
}

fn history_status_verbose(summary: &HistorySummary, is_ua: bool) -> String {
    let code = if !summary.status_code.is_empty() {
        summary.status_code.to_ascii_uppercase()
    } else {
        summary.status.to_ascii_uppercase()
    };
    if code.contains("COMPLETE") || code == "OK" {
        if is_ua { "\u{0423}\u{0441}\u{043F}\u{0456}\u{0448}\u{043D}\u{043E}".to_string() } else { "Completed".to_string() }
    } else if code.contains("STOP") {
        if is_ua { "\u{041F}\u{0435}\u{0440}\u{0435}\u{0440}\u{0432}\u{0430}\u{043D}\u{043E} \u{043A}\u{043E}\u{0440}\u{0438}\u{0441}\u{0442}\u{0443}\u{0432}\u{0430}\u{0447}\u{0435}\u{043C}".to_string() } else { "Stopped by user".to_string() }
    } else if code.contains("ERROR") || code.contains("FAULT") {
        let reason = if !summary.fault_reason.is_empty() {
            translate_fault_reason(&summary.fault_reason, is_ua)
        } else if is_ua {
            "\u{043D}\u{0435}\u{0432}\u{0456}\u{0434}\u{043E}\u{043C}\u{0430} \u{043F}\u{0440}\u{0438}\u{0447}\u{0438}\u{043D}\u{0430}".to_string()
        } else {
            "unknown reason".to_string()
        };
        if is_ua {
            format!("\u{041F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430} ({})", reason)
        } else {
            format!("Error ({})", reason)
        }
    } else if summary.status.is_empty() {
        if is_ua { "\u{041D}\u{0435}\u{0432}\u{0456}\u{0434}\u{043E}\u{043C}\u{043E}".to_string() } else { "Unknown".to_string() }
    } else {
        summary.status.clone()
    }
}

fn compute_energy_kwh(detail: &HistoryDetail) -> f32 {
    if detail.summary.energy_kwh > 0.0 {
        return detail.summary.energy_kwh;
    }
    if detail.summary.energy_wh > 0.0 {
        return detail.summary.energy_wh / 1000.0;
    }
    if detail.data.len() < 2 {
        return 0.0;
    }
    let ts_div = detect_timestamp_seconds_divisor(&detail.data) as f64;
    let mut wh = 0.0_f64;
    for i in 1..detail.data.len() {
        let prev = &detail.data[i - 1];
        let curr = &detail.data[i];
        let dt_sec = ((curr.timestamp - prev.timestamp).max(0) as f64) / ts_div;
        if dt_sec <= 0.0 {
            continue;
        }
        let avg_w = ((prev.power.max(0.0) + curr.power.max(0.0)) as f64) * 0.5;
        wh += avg_w * dt_sec / 3600.0;
    }
    (wh / 1000.0) as f32
}

fn compute_voltage_stability(detail: &HistoryDetail, is_ua: bool) -> String {
    let mut min_v = f32::MAX;
    let mut max_v = 0.0_f32;
    for s in &detail.data {
        if s.voltage > 1.0 {
            min_v = min_v.min(s.voltage);
            max_v = max_v.max(s.voltage);
        }
    }
    if max_v <= 0.0 || min_v == f32::MAX {
        return if is_ua { "\u{041D}/\u{0414}".to_string() } else { "N/A".to_string() };
    }
    let span = (max_v - min_v).max(0.0);
    if is_ua {
        format!("{:.0}-{:.0} \u{0412} (\u{00B1}{:.1}\u{0412})", min_v, max_v, span * 0.5)
    } else {
        format!("{:.0}-{:.0} V (+/-{:.1}V)", min_v, max_v, span * 0.5)
    }
}

fn build_step_plan_actual_text(detail: &HistoryDetail, is_ua: bool) -> String {
    if detail.data.len() < 2 {
        return if is_ua { "\u{041D}/\u{0414}".to_string() } else { "N/A".to_string() };
    }
    let ts_div = detect_timestamp_seconds_divisor(&detail.data) as f64;
    let mut boundaries: Vec<usize> = vec![0];
    for i in 1..detail.data.len() {
        if (detail.data[i].target - detail.data[i - 1].target).abs() >= 0.5 {
            boundaries.push(i);
        }
    }
    if *boundaries.last().unwrap_or(&0) != detail.data.len() - 1 {
        boundaries.push(detail.data.len() - 1);
    }
    let mut out = Vec::new();
    for step_idx in 0..boundaries.len().saturating_sub(1) {
        let a = boundaries[step_idx];
        let b = boundaries[step_idx + 1];
        let dt_min = (((detail.data[b].timestamp - detail.data[a].timestamp).max(0) as f64) / ts_div / 60.0).round() as i32;
        let target = detail.data[a].target.round() as i32;
        let plan_lbl = format_minutes_label(dt_min.max(0), is_ua);
        let act_lbl = format_minutes_label(dt_min.max(0), is_ua);
        out.push(if is_ua {
            format!("\u{041A}\u{0440}\u{043E}\u{043A} {}: \u{0446}\u{0456}\u{043B}\u{044C} {}\u{00B0}, \u{043F}\u{043B}\u{0430}\u{043D} {}, \u{0444}\u{0430}\u{043A}\u{0442} {}", step_idx + 1, target, plan_lbl, act_lbl)
        } else {
            format!("Step {}: target {}\u{00B0}, plan {}, actual {}", step_idx + 1, target, plan_lbl, act_lbl)
        });
    }
    if out.is_empty() {
        if is_ua { "\u{041D}/\u{0414}".to_string() } else { "N/A".to_string() }
    } else {
        out.join("\n")
    }
}

fn build_change_diff_text(detail: &HistoryDetail, is_ua: bool) -> String {
    if detail.changes.is_empty() {
        return if is_ua { "\u{041D}/\u{0414}".to_string() } else { "N/A".to_string() };
    }
    let mut lines = Vec::new();
    for (idx, ch) in detail.changes.iter().enumerate() {
        if idx >= 14 {
            lines.push(if is_ua {
                format!("... \u{0449}\u{0435} {} \u{0437}\u{043C}\u{0456}\u{043D}", detail.changes.len() - idx)
            } else {
                format!("... {} more changes", detail.changes.len() - idx)
            });
            break;
        }
        let action = match ch.action.as_str() {
            "add_temp" => if is_ua { "\u{0414}\u{043E}\u{0434}\u{0430}\u{043D}\u{043E} \u{0442}\u{0435}\u{043C}\u{043F}\u{0435}\u{0440}\u{0430}\u{0442}\u{0443}\u{0440}\u{0443}" } else { "Added temperature" },
            "add_time" => if is_ua { "\u{0414}\u{043E}\u{0434}\u{0430}\u{043D}\u{043E} \u{0447}\u{0430}\u{0441} \u{0432}\u{0438}\u{0442}\u{0440}\u{0438}\u{043C}\u{043A}\u{0438}" } else { "Added hold time" },
            "set_rate" => if is_ua { "\u{0417}\u{043C}\u{0456}\u{043D}\u{0435}\u{043D}\u{043E} \u{0448}\u{0432}\u{0438}\u{0434}\u{043A}\u{0456}\u{0441}\u{0442}\u{044C}" } else { "Changed rate" },
            "skip_step" => if is_ua { "\u{041F}\u{0440}\u{043E}\u{043F}\u{0443}\u{0441}\u{043A} \u{043A}\u{0440}\u{043E}\u{043A}\u{0443}" } else { "Skipped step" },
            _ => if is_ua { "\u{0417}\u{043C}\u{0456}\u{043D}\u{0430}" } else { "Change" },
        };
        let field = if ch.field.is_empty() {
            if is_ua { "\u{043F}\u{0430}\u{0440}\u{0430}\u{043C}\u{0435}\u{0442}\u{0440}" } else { "field" }
        } else {
            ch.field.as_str()
        };
        lines.push(if is_ua {
            format!(
                "#{} [{}] \u{043A}\u{0440}\u{043E}\u{043A} {}: {}: {:.1} -> {:.1} (\u{0394}{:+.1})",
                idx + 1,
                uptime_label(ch.ts_ms.max(0) as u64),
                ch.step.max(0),
                field,
                ch.before,
                ch.after,
                ch.delta
            )
        } else {
            format!(
                "#{} [{}] step {}: {}: {:.1} -> {:.1} (\u{041E}\"{:+.1})",
                idx + 1,
                uptime_label(ch.ts_ms.max(0) as u64),
                ch.step.max(0),
                field,
                ch.before,
                ch.after,
                ch.delta
            )
        });
        if lines.last().is_some() {
            let last = lines.pop().unwrap_or_default();
            lines.push(format!("{} | {}", action, last));
        }
    }
    lines.join("\n")
}

fn build_step_and_change_text(detail: &HistoryDetail, is_ua: bool) -> String {
    let base = build_step_plan_actual_text(detail, is_ua);
    let changes = build_change_diff_text(detail, is_ua);
    if changes == "N/A" || changes == "\u{041D}/\u{0414}" {
        return base;
    }
    if base == "N/A" || base == "\u{041D}/\u{0414}" {
        if is_ua {
            return format!("\u{0417}\u{043C}\u{0456}\u{043D}\u{0438} \u{043F}\u{0456}\u{0434} \u{0447}\u{0430}\u{0441} \u{0432}\u{0438}\u{043F}\u{0430}\u{043B}\u{0443}:\n{}", changes);
        }
        return format!("Changes during firing:\n{}", changes);
    }
    if is_ua {
        format!("{}\n\n\u{0417}\u{043C}\u{0456}\u{043D}\u{0438} \u{043F}\u{0456}\u{0434} \u{0447}\u{0430}\u{0441} \u{0432}\u{0438}\u{043F}\u{0430}\u{043B}\u{0443}:\n{}", base, changes)
    } else {
        format!("{}\n\nChanges during firing:\n{}", base, changes)
    }
}

fn fault_status_label(ok: bool, is_ua: bool) -> String {
    if ok {
        "OK".to_string()
    } else if is_ua {
        "\u{0417}\u{0411}\u{0406}\u{0419}".to_string()
    } else {
        "FAULT".to_string()
    }
}

fn uptime_label(ms: u64) -> String {
    let total_seconds = ms / 1000;
    let minutes = total_seconds / 60;
    let seconds = total_seconds % 60;
    if minutes > 0 {
        format!("t+{}m {}s", minutes, seconds)
    } else {
        format!("t+{}s", seconds)
    }
}

#[allow(dead_code)]
fn prettify_token(token: &str) -> String {
    token
        .split('_')
        .filter(|part| !part.is_empty())
        .map(|part| {
            let mut chars = part.chars();
            match chars.next() {
                Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
                None => String::new(),
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

fn fault_title_from_fields(code: &str, message: &str, action: &str, is_ua: bool) -> String {
    let mut candidate = String::new();
    if !code.is_empty() && code != "ok" {
        candidate = translate_command_message(code, is_ua);
    } else if !message.is_empty() && message != "ok" && message != "settings_changed" {
        candidate = if message.contains("fault")
            || message.contains("Sensor")
            || message.contains("sensor")
            || message.contains("Temperature")
        {
            translate_fault_reason(message, is_ua)
        } else {
            translate_command_message(message, is_ua)
        };
    } else if !action.is_empty() {
        candidate = translate_command_message(action, is_ua);
    }

    let trimmed = candidate.trim();
    if trimmed.is_empty() {
        if is_ua {
            "\u{041D}\u{0435}\u{0432}\u{0456}\u{0434}\u{043E}\u{043C}\u{0430} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430}".to_string()
        } else {
            "Unknown fault".to_string()
        }
    } else {
        trimmed.to_string()
    }
}

fn format_uptime_label(seconds: u64, is_ua: bool) -> String {
    let days = seconds / 86_400;
    let hours = (seconds % 86_400) / 3_600;
    let mins = (seconds % 3_600) / 60;
    if is_ua {
        format!("{}\u{0434} {}\u{0433} {}\u{0445}\u{0432}", days, hours, mins)
    } else {
        format!("{}d {}h {}m", days, hours, mins)
    }
}

fn format_bytes_short(bytes: u64) -> String {
    const KB: f64 = 1024.0;
    const MB: f64 = KB * 1024.0;
    const GB: f64 = MB * 1024.0;
    let b = bytes as f64;
    if b >= GB {
        format!("{:.2} GB", b / GB)
    } else if b >= MB {
        format!("{:.1} MB", b / MB)
    } else if b >= KB {
        format!("{:.0} KB", b / KB)
    } else {
        format!("{} B", bytes)
    }
}

fn format_used_total_label(used: u64, total: u64) -> String {
    if total == 0 {
        return "N/A".to_string();
    }
    format!("{} / {}", format_bytes_short(used), format_bytes_short(total))
}

fn parse_history_total_heating_minutes() -> i64 {
    let path = "/littlefs/history.json";
    let Ok(content) = std::fs::read_to_string(path) else {
        return 0;
    };
    let Ok(items) = serde_json::from_str::<Vec<serde_json::Value>>(&content) else {
        return 0;
    };
    items.iter().fold(0_i64, |acc, item| {
        let add = item
            .get("summary")
            .and_then(|s| s.get("duration"))
            .and_then(|d| d.as_i64())
            .unwrap_or(0)
            .max(0);
        acc + add
    })
}

fn format_storage_label(schedule_count: usize, schedules_bytes: u64, _is_ua: bool) -> String {
    const LITTLEFS_TOTAL_BYTES: u64 = 0x350000;
    let used = schedules_bytes.min(LITTLEFS_TOTAL_BYTES);
    format!("{} ({})", format_used_total_label(used, LITTLEFS_TOTAL_BYTES), schedule_count)
}

fn update_controller_info_ui(ui: &AppWindow, state: &SlintKilnState, is_ua: bool) {
    let mut buf = vec![0u8; 96];
    let fw = if unsafe { slint_bridge_get_firmware_version_copy(buf.as_mut_ptr() as *mut c_char, buf.len() as i32) } {
        c_buf_to_string(&buf)
    } else {
        String::from("--")
    };
    let mut mac_buf = vec![0u8; 64];
    let device_id = if unsafe { slint_bridge_get_device_id_copy(mac_buf.as_mut_ptr() as *mut c_char, mac_buf.len() as i32) } {
        c_buf_to_string(&mac_buf)
    } else {
        String::from("--")
    };
    let heap_free = unsafe { slint_bridge_get_free_heap_bytes() } as u64;
    let heap_total = unsafe { slint_bridge_get_total_heap_bytes() } as u64;
    let heap_used = heap_total.saturating_sub(heap_free);
    let ram_free = unsafe { slint_bridge_get_free_psram_bytes() } as u64;
    let ram_total = unsafe { slint_bridge_get_total_psram_bytes() } as u64;
    let ram_used = ram_total.saturating_sub(ram_free);
    let uptime_s = unsafe { slint_bridge_get_uptime_seconds() };

    let schedules = load_schedules();
    let schedules_bytes = std::fs::metadata("/littlefs/schedules.json").map(|m| m.len()).unwrap_or(0);
    let storage = format_storage_label(schedules.len(), schedules_bytes, is_ua);

    let total_heating_minutes = parse_history_total_heating_minutes().max(0);
    let heating_label = if is_ua {
                    format!("{:.1} \u{0433}\u{043E}\u{0434}", total_heating_minutes as f32 / 60.0)
    } else {
        format!("{:.1} h", total_heating_minutes as f32 / 60.0)
    };

    let mut chip_temp_c: f32 = 0.0;
    let chip_temp = if unsafe { slint_bridge_get_chip_temp_c(&mut chip_temp_c as *mut f32) } {
        format!("{:.1} \u{00B0}C", chip_temp_c)
    } else if is_ua {
        "\u{041D}/\u{0414}".to_string()
    } else {
        "N/A".to_string()
    };
    let pzem = if state.pzem_ok {
        if is_ua {
            format!("OK | {:.0} \u{0412}", state.pzem_voltage.max(0.0))
        } else {
            format!("OK | {:.0} V", state.pzem_voltage.max(0.0))
        }
    } else if is_ua {
        "\u{041D}\u{0435}\u{043C}\u{0430}\u{0454} \u{0437}\u{0432}'\u{044F}\u{0437}\u{043A}\u{0443}".to_string()
    } else {
        "No link".to_string()
    };

    ui.set_controller_info_fw(fw.into());
    ui.set_controller_info_heap(format_used_total_label(heap_used, heap_total).into());
    ui.set_controller_info_ram(format_used_total_label(ram_used, ram_total).into());
    ui.set_controller_info_storage(storage.into());
    ui.set_controller_info_device_id(device_id.into());
    ui.set_controller_info_uptime(format_uptime_label(uptime_s, is_ua).into());
    ui.set_controller_info_heating_hours(heating_label.into());
    ui.set_controller_info_chip_temp(chip_temp.into());
    ui.set_controller_info_pzem(pzem.into());
}

fn load_fault_log_items(is_ua: bool) -> Vec<FaultLogPreviewCard> {
    let mut entries: Vec<(u64, FaultLogPreviewCard)> = Vec::new();
    let label_action = if is_ua { "\u{0414}\u{0456}\u{044F}" } else { "Action" };
    let label_source = if is_ua { "\u{0414}\u{0436}\u{0435}\u{0440}\u{0435}\u{043B}\u{043E}" } else { "Source" };
    let label_code = if is_ua { "\u{041A}\u{043E}\u{0434}" } else { "Code" };
    let label_message = if is_ua { "\u{041F}\u{043E}\u{0432}\u{0456}\u{0434}\u{043E}\u{043C}\u{043B}\u{0435}\u{043D}\u{043D}\u{044F}" } else { "Message" };
    let label_timestamp = if is_ua { "\u{0427}\u{0430}\u{0441}" } else { "Timestamp" };
    let label_event = if is_ua { "\u{041F}\u{043E}\u{0434}\u{0456}\u{044F}" } else { "Event" };

    for path in ["/littlefs/logs/audit.log", "/littlefs/logs/audit.log.1"] {
        let Ok(data) = std::fs::read_to_string(path) else { continue };
        for line in data.lines().filter(|line| !line.trim().is_empty()) {
            if let Ok(value) = serde_json::from_str::<serde_json::Value>(line) {
                let ok = value.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
                let action = value.get("action").and_then(|v| v.as_str()).unwrap_or("");
                let source = value.get("source").and_then(|v| v.as_str()).unwrap_or("");
                let code = value.get("code").and_then(|v| v.as_str()).unwrap_or("");
                let message = value.get("message").and_then(|v| v.as_str()).unwrap_or("");
                let ts_ms = value.get("ts_ms").and_then(|v| v.as_u64()).unwrap_or(0);
                let kind = value.get("kind").and_then(|v| v.as_str()).unwrap_or("");

                let fault_like = !ok
                    || action.contains("fault")
                    || code.contains("fault")
                    || code.contains("sensor")
                    || message.contains("fault")
                    || message.contains("Sensor")
                    || message.contains("sensor")
                    || kind == "autotune";
                if !fault_like {
                    continue;
                }

                let title = fault_title_from_fields(code, message, action, is_ua);
                let mut subtitle_parts = Vec::new();
                let mut detail_parts = Vec::new();
                if !action.is_empty() {
                    let translated = translate_command_message(action, is_ua);
                    subtitle_parts.push(translated.clone());
                    detail_parts.push(format!("{}: {}", label_action, translated));
                }
                if !source.is_empty() {
                    let src = source.to_uppercase();
                    subtitle_parts.push(src.clone());
                    detail_parts.push(format!("{}: {}", label_source, src));
                }
                if !code.is_empty() && code != "ok" {
                    let translated = translate_command_message(code, is_ua);
                    subtitle_parts.push(translated.clone());
                    detail_parts.push(format!("{}: {}", label_code, translated));
                }
                if !message.is_empty() && message != "ok" && message != "settings_changed" {
                    let translated = translate_fault_reason(message, is_ua);
                    subtitle_parts.push(translated.clone());
                    detail_parts.push(format!("{}: {}", label_message, translated));
                }
                if ts_ms > 0 {
                    let ts = uptime_label(ts_ms);
                    subtitle_parts.push(ts.clone());
                    detail_parts.push(format!("{}: {}", label_timestamp, ts));
                }

                entries.push((
                    ts_ms,
                    FaultLogPreviewCard {
                        title: truncate_preview(title, 44).into(),
                        status: fault_status_label(ok, is_ua).into(),
                        subtitle: truncate_preview(subtitle_parts.join(" | "), 76).into(),
                        details: detail_parts.join("\n").into(),
                    },
                ));
            } else {
                entries.push((
                    0,
                    FaultLogPreviewCard {
                        title: truncate_preview(line.trim().to_string(), 44).into(),
                        status: if is_ua { "\u{0417}\u{0411}\u{0406}\u{0419}".into() } else { "FAULT".into() },
                        subtitle: "".into(),
                        details: line.trim().to_string().into(),
                    },
                ));
            }
        }
    }

    for path in ["/littlefs/logs/events.log", "/littlefs/logs/events.log.1"] {
        let Ok(data) = std::fs::read_to_string(path) else { continue };
        for line in data.lines().filter(|line| !line.trim().is_empty()) {
            let mut parts = line.trim().splitn(3, ' ');
            let ts_ms = parts.next().and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            let event_type = parts.next().unwrap_or("").trim();
            let message = parts.next().unwrap_or("").trim();
            if event_type.is_empty() {
                continue;
            }

            let event_type_upper = event_type.to_ascii_uppercase();
            let message_lower = message.to_ascii_lowercase();
            let fault_like = event_type_upper == "FAULT"
                || event_type_upper == "FAULT_CLEAR"
                || event_type_upper == "RECOVERY_RESUME"
                || event_type_upper == "RECOVERY_ABORT"
                || event_type_upper.contains("FAULT")
                || message_lower.contains("fault")
                || message_lower.contains("sensor");
            if !fault_like {
                continue;
            }

            let ok = event_type_upper == "FAULT_CLEAR" || event_type_upper == "RECOVERY_RESUME";
            let title = fault_title_from_fields(event_type, message, event_type, is_ua);
            let mut subtitle_parts = Vec::new();
            let mut detail_parts = Vec::new();
            subtitle_parts.push(event_type_upper.clone());
            detail_parts.push(format!("{}: {}", label_event, event_type_upper));
            if !message.is_empty() {
                let translated = translate_fault_reason(message, is_ua);
                subtitle_parts.push(translated.clone());
                detail_parts.push(format!("{}: {}", label_message, translated));
            }
            if ts_ms > 0 {
                let ts = uptime_label(ts_ms);
                subtitle_parts.push(ts.clone());
                detail_parts.push(format!("{}: {}", label_timestamp, ts));
            }

            entries.push((
                ts_ms,
                FaultLogPreviewCard {
                    title: truncate_preview(title, 44).into(),
                    status: fault_status_label(ok, is_ua).into(),
                    subtitle: truncate_preview(subtitle_parts.join(" | "), 76).into(),
                    details: detail_parts.join("\n").into(),
                },
            ));
        }
    }

    entries.sort_by(|a, b| b.0.cmp(&a.0));
    let cards: Vec<FaultLogPreviewCard> = entries.into_iter().map(|(_, card)| card).collect();

    if cards.is_empty() {
        vec![FaultLogPreviewCard {
            title: (if is_ua { "\u{041B}\u{043E}\u{0433}\u{0438} \u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043E}\u{043A} \u{043F}\u{043E}\u{043A}\u{0438} \u{043F}\u{043E}\u{0440}\u{043E}\u{0436}\u{043D}\u{0456}." } else { "Fault logs are empty." }).into(),
            status: "".into(),
            subtitle: "".into(),
            details: "".into(),
        }]
    } else {
        cards
    }
}
fn schedule_to_row(schedule: &ScheduleFile, selected: bool) -> ScheduleRow {
    let steps_count = schedule.steps_count.unwrap_or(schedule.steps.len() as i32);
    ScheduleRow {
        name: schedule.name.clone().into(),
        steps_count,
        selected,
    }
}

fn schedule_steps_count(schedule: &ScheduleFile) -> i32 {
    schedule.steps_count.unwrap_or(schedule.steps.len() as i32)
}

fn to_editor_steps(schedule: &ScheduleFile, unit: TempUnit) -> Vec<EditorStepRow> {
    schedule
        .steps
        .iter()
        .map(|s| EditorStepRow {
            step_type: s.step_type.clone().into(),
            rate: rate_to_display(s.rate.unwrap_or(0), unit),
            target: temp_to_display_i32(s.target.unwrap_or(0), unit),
            hold: s.hold_time.unwrap_or(0),
        })
        .collect()
}

fn to_schedule_steps(
    existing: &[ScheduleStepFile],
    editor_steps: &[EditorStepRow],
    unit: TempUnit,
) -> Vec<ScheduleStepFile> {
    let mut out = Vec::new();
    for (i, s) in editor_steps.iter().enumerate() {
        let mut step = existing.get(i).cloned().unwrap_or_else(|| ScheduleStepFile {
            step_type: "ramp".to_string(),
            rate: Some(100),
            target: Some(0),
            hold_time: Some(0),
            id: None,
            fan: None,
            extra: serde_json::Map::new(),
        });

        let mut step_type = s.step_type.to_string();
        if step_type.is_empty() {
            step_type = "ramp".to_string();
        }
        step.step_type = step_type;
        if step.step_type == "hold" {
            step.rate = None;
            step.target = None;
            step.hold_time = Some(s.hold);
        } else {
            step.rate = Some(rate_to_c_from_display(s.rate, unit));
            step.target = Some(temp_to_c_from_display(s.target, unit));
            step.hold_time = Some(s.hold);
        }
        out.push(step);
    }
    out
}

fn schedule_effective_target_for_step(steps: &[ScheduleStepFile], step_index: usize, fallback: i32) -> i32 {
    if steps.is_empty() {
        return fallback;
    }
    let idx = step_index.min(steps.len().saturating_sub(1));
    for j in (0..=idx).rev() {
        if let Some(t) = steps[j].target {
            return t;
        }
    }
    fallback
}

fn running_segment_target(schedule: &ScheduleFile, step_index: i32, fallback: i32) -> i32 {
    if step_index < 0 {
        return fallback;
    }
    schedule_effective_target_for_step(&schedule.steps, step_index as usize, fallback)
}

fn is_hold_step(step: &ScheduleStepFile) -> bool {
    step.step_type.eq_ignore_ascii_case("hold")
}

fn estimate_remaining_minutes(
    schedule: &ScheduleFile,
    current_step: i32,
    current_temp_c: f32,
    time_remaining_min: i32,
) -> (i32, i32) {
    if schedule.steps.is_empty() || current_step < 0 {
        return (-1, -1);
    }
    let idx = (current_step as usize).min(schedule.steps.len().saturating_sub(1));

    let future_holds_sum: i32 = schedule
        .steps
        .iter()
        .skip(idx + 1)
        .filter(|s| is_hold_step(s))
        .map(|s| s.hold_time.unwrap_or(0).max(0))
        .sum();

    let mut step_remaining = -1_i32;
    let mut total_remaining = 0.0_f32;
    let mut sim_temp = if current_temp_c.is_finite() { current_temp_c } else { 0.0 };

    for (i, step) in schedule.steps.iter().enumerate().skip(idx) {
        if is_hold_step(step) {
            let hold_full = step.hold_time.unwrap_or(0).max(0);
            let hold_rem = if i == idx && time_remaining_min >= 0 {
                (time_remaining_min - future_holds_sum).clamp(0, hold_full)
            } else {
                hold_full
            };
            if i == idx {
                step_remaining = hold_rem;
            }
            total_remaining += hold_rem as f32;
            continue;
        }

        let target_c = step
            .target
            .unwrap_or_else(|| sim_temp.round() as i32) as f32;
        // Program step rate is stored/displayed as degC/hour.
        let rate_c_per_hour = step.rate.unwrap_or(0).max(0) as f32;
        let safe_rate_h = if rate_c_per_hour.is_finite() && rate_c_per_hour > 0.01 {
            rate_c_per_hour
        } else {
            9999.0
        };

        let from_temp = if i == idx { sim_temp } else { sim_temp };
        let ramp_rem = (((target_c - from_temp).abs() * 60.0) / safe_rate_h).ceil().max(0.0) as i32;
        let step_rem = ramp_rem;

        if i == idx {
            step_remaining = step_rem;
        }
        total_remaining += step_rem as f32;
        sim_temp = target_c;
    }

    (step_remaining.max(0), total_remaining.ceil().max(0.0) as i32)
}

fn apply_runtime_step_delta(old_steps: &[ScheduleStepFile], new_steps: &[ScheduleStepFile], step_index: usize) {
    if old_steps.is_empty() || new_steps.is_empty() || step_index >= old_steps.len() || step_index >= new_steps.len() {
        return;
    }
    let old_step = &old_steps[step_index];
    let new_step = &new_steps[step_index];

    let old_target = schedule_effective_target_for_step(old_steps, step_index, 0);
    let new_target = schedule_effective_target_for_step(new_steps, step_index, old_target);
    let d_target = new_target - old_target;
    if d_target != 0 {
        unsafe { slint_bridge_add_temp(d_target as f32) };
    }

    if new_step.step_type != "hold" {
        let old_rate = old_step.rate.unwrap_or(0);
        let new_rate = new_step.rate.unwrap_or(old_rate);
        if new_rate > 0 && new_rate != old_rate {
            let _ = unsafe { slint_bridge_set_rate(new_rate as f32) };
        }
    }

    if new_step.step_type == "hold" {
        let old_hold = old_step.hold_time.unwrap_or(0);
        let new_hold = new_step.hold_time.unwrap_or(old_hold);
        let d_hold = new_hold - old_hold;
        if d_hold != 0 {
            unsafe { slint_bridge_add_time(d_hold as f32) };
        }
    }
}

fn make_safe_id(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    for ch in name.chars() {
        if ch.is_ascii_alphanumeric() || ch == '_' || ch == '-' {
            out.push(ch);
        } else if ch.is_whitespace() {
            out.push('_');
        }
    }
    if out.is_empty() {
        out = "schedule".to_string();
    }
    out
}

fn normalize_schedule_name(input: &str) -> String {
    let repaired = repair_mojibake_ua(input);
    let trimmed = repaired.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let mut out = String::with_capacity(trimmed.len());
    let mut prev_underscore = false;
    for ch in trimmed.chars() {
        if ch.is_whitespace() {
            if !prev_underscore {
                out.push('_');
                prev_underscore = true;
            }
        } else {
            out.push(ch);
            prev_underscore = ch == '_';
        }
    }
    out.trim_matches('_').to_string()
}

fn repair_mojibake_ua(input: &str) -> String {
    let s = input.trim();
    if s.is_empty() {
        return String::new();
    }

    let suspicious = [
        '\u{0402}', '\u{0403}', '\u{0409}', '\u{040A}', '\u{040B}', '\u{040C}', '\u{040F}',
        '\u{0452}', '\u{0453}', '\u{0459}', '\u{045A}', '\u{045B}', '\u{045C}', '\u{045F}',
        '\u{2018}', '\u{2019}', '\u{201C}', '\u{201D}', '\u{2020}', '\u{2030}',
    ];

    let has_latin_mojibake = s.contains('\u{00D0}') || s.contains('\u{00D1}') || s.contains('\u{00C3}') || s.contains('\u{00C2}') || s.contains('\u{FFFD}');
    let has_legacy_markers = s.contains("\u{0413}\u{0452}") || s.contains("\u{0413}\u{2018}");
    if !s.chars().any(|c| suspicious.contains(&c)) && !has_legacy_markers && !has_latin_mojibake {
        return s.to_string();
    }

    if has_latin_mojibake {
        let mut bytes = Vec::with_capacity(s.len());
        for ch in s.chars() {
            let cp = ch as u32;
            if cp > 0xFF {
                bytes.clear();
                break;
            }
            bytes.push(cp as u8);
        }
        if !bytes.is_empty() {
            if let Ok(decoded) = std::str::from_utf8(&bytes) {
                let decoded = decoded.trim();
                if !decoded.is_empty() {
                    let marker_count = decoded
                        .chars()
                        .filter(|c| *c == '\u{00D0}' || *c == '\u{00D1}' || *c == '\u{00C3}' || *c == '\u{00C2}' || *c == '\u{FFFD}')
                        .count();
                    let decoded_cyr = decoded.chars().filter(|c| ('\u{0400}'..='\u{04FF}').contains(c)).count();
                    if marker_count == 0 && decoded_cyr > 0 {
                        return decoded.to_string();
                    }
                }
            }
        }
    }

    let (bytes, _, had_errors) = WINDOWS_1251.encode(s);
    if had_errors {
        return s.to_string();
    }
    let Ok(decoded) = std::str::from_utf8(bytes.as_ref()) else {
        return s.to_string();
    };
    let decoded = decoded.trim();
    if decoded.is_empty() {
        return s.to_string();
    }

    let decoded_cyr = decoded.chars().filter(|c| ('\u{0400}'..='\u{04FF}').contains(c)).count();
    let source_cyr = s.chars().filter(|c| ('\u{0400}'..='\u{04FF}').contains(c)).count();

    if decoded_cyr >= source_cyr / 2 {
        decoded.to_string()
    } else {
        s.to_string()
    }
}

fn sanitize_display_text(input: &str) -> String {
    let repaired = repair_mojibake_ua(input);
    repaired.replace('\u{FFFD}', "").trim().to_string()
}
fn make_unique_schedule_name(schedules: &[ScheduleFile], base_name: &str) -> String {
    let base = normalize_schedule_name(base_name);
    let mut max_index: u32 = 0;

    for s in schedules {
        let name = normalize_schedule_name(&s.name);
        if name == base {
            continue;
        }
        if let Some(suffix) = name
            .strip_prefix(&base)
            .and_then(|rest| rest.strip_prefix('_').or_else(|| rest.strip_prefix(' ')))
        {
            if let Ok(n) = suffix.trim().parse::<u32>() {
                if n > max_index {
                    max_index = n;
                }
            }
        }
    }

    format!("{} {}", base, max_index + 1)
}

fn parse_int_with_clamp(text: &str, min: i32, max: i32, fallback: i32) -> i32 {
    match text.trim().parse::<i32>() {
        Ok(v) => v.clamp(min, max),
        Err(_) => fallback,
    }
}

fn parse_wifi_scan_rows(json: &str) -> Vec<WifiNetworkRow> {
    let mut rows: Vec<WifiScanAp> = serde_json::from_str(json).unwrap_or_default();
    rows.retain(|r| !r.ssid.trim().is_empty());
    rows.sort_by(|a, b| b.rssi.cmp(&a.rssi));

    rows.into_iter()
        .map(|r| WifiNetworkRow {
            ssid: r.ssid.into(),
            rssi: r.rssi,
        })
        .collect()
}

const CONTROLLER_AP_SSID_FALLBACK: &str = "TRAE_KILN_SETUP";
const CONTROLLER_AP_PORTAL_URL: &str = "http://192.168.4.1";

fn wifi_qr_payload(wifi_ok: bool, server_url: &str, ap_ssid: &str) -> String {
    if wifi_ok {
        server_url.to_string()
    } else {
        format!("WIFI:T:nopass;S:{};;", ap_ssid)
    }
}

fn build_qr_image(payload: &str) -> Image {
    let qr = match qrcodegen::QrCode::encode_text(payload, qrcodegen::QrCodeEcc::Medium) {
        Ok(q) => q,
        Err(_) => {
            let fallback: SharedPixelBuffer<Rgb8Pixel> =
                SharedPixelBuffer::clone_from_slice(&[255u8, 255, 255], 1, 1);
            return Image::from_rgb8(fallback);
        }
    };

    let size = qr.size();
    let border: i32 = 2;
    let scale: i32 = 4;
    let img_size = ((size + border * 2) * scale) as u32;
    let mut data = vec![255u8; (img_size * img_size * 3) as usize];

    for y in 0..img_size as i32 {
        for x in 0..img_size as i32 {
            let qx = x / scale - border;
            let qy = y / scale - border;
            let dark = qx >= 0 && qx < size && qy >= 0 && qy < size && qr.get_module(qx, qy);
            let v = if dark { 0u8 } else { 255u8 };
            let idx = ((y as u32 * img_size + x as u32) * 3) as usize;
            data[idx] = v;
            data[idx + 1] = v;
            data[idx + 2] = v;
        }
    }

    let buf: SharedPixelBuffer<Rgb8Pixel> = SharedPixelBuffer::clone_from_slice(&data, img_size, img_size);
    Image::from_rgb8(buf)
}

fn refresh_schedule_model(ui: &AppWindow, state: &AppState) {
    let rows: Vec<ScheduleRow> = state
        .schedules
        .iter()
        .enumerate()
        .map(|(i, s)| schedule_to_row(s, state.selected_index == Some(i)))
        .collect();
    ui.set_schedules(ModelRc::new(VecModel::from(rows)));
}

fn refresh_editor_model(ui: &AppWindow, state: &AppState) {
    ui.set_editor_steps(ModelRc::new(VecModel::from(state.editor_steps.clone())));
}

fn format_minutes_label(total_minutes: i32, _is_ua: bool) -> String {
    let mins = total_minutes.max(0);
    let hours = mins / 60;
    let rem = mins % 60;
    if hours > 0 {
        format!("{}h {}m", hours, rem)
    } else {
        format!("{}m", rem)
    }
}

fn format_axis_hour_minute_label(total_minutes: i32, _is_ua: bool) -> String {
    let mins = total_minutes.max(0);
    let hours = mins / 60;
    let rem = mins % 60;
    if rem == 0 {
        format!("{}h", hours)
    } else {
        format!("{}h {}m", hours, rem)
    }
}

fn build_dynamic_tick_minutes(points: &[(f32, f32)], max_ticks: usize) -> Vec<i32> {
    let mut values: Vec<i32> = points
        .iter()
        .map(|(h, _)| (*h * 60.0).round().max(0.0) as i32)
        .collect();
    values.sort_unstable();
    values.dedup();

    if values.is_empty() {
        return vec![0];
    }
    if values.len() <= max_ticks || max_ticks < 2 {
        return values;
    }

    let stride = ((values.len() + max_ticks - 1) / max_ticks).max(1);
    let mut reduced: Vec<i32> = Vec::with_capacity(max_ticks);
    let mut i = 0usize;
    while i < values.len() {
        reduced.push(values[i]);
        i += stride;
    }
    if let Some(&last) = values.last() {
        if reduced.last().copied() != Some(last) {
            reduced.push(last);
        }
    }
    reduced
}

#[allow(dead_code)]
fn format_graph_hhmm(total_minutes: i32) -> String {
    let mins = total_minutes.max(0);
    let h = mins / 60;
    let m = mins % 60;
    format!("{:02}:{:02}", h, m)
}

fn detect_timestamp_seconds_divisor(data: &[HistorySample]) -> f32 {
    if data.len() < 2 {
        return 1.0;
    }
    let mut min_positive_delta: i64 = i64::MAX;
    for window in data.windows(2) {
        let dt = window[1].timestamp - window[0].timestamp;
        if dt > 0 && dt < min_positive_delta {
            min_positive_delta = dt;
        }
    }
    if min_positive_delta == i64::MAX {
        return 1.0;
    }
    // Millisecond logs usually have per-sample deltas >> 100.
    if min_positive_delta > 100 {
        1000.0
    } else {
        1.0
    }
}

fn sample_graph_line(rects: &mut Vec<ScheduleGraphRect>, x0: i32, y0: i32, x1: i32, y1: i32, color: Color) {
    // Keep the line visually continuous for long ramps/holds.
    // The previous cap (64) produced dotted-looking long segments.
    let steps = (x1 - x0).abs().max((y1 - y0).abs()).clamp(1, 2048);
    for i in 0..=steps {
        let t = i as f32 / steps as f32;
        let x = (x0 as f32 + (x1 - x0) as f32 * t).round() as i32;
        let y = (y0 as f32 + (y1 - y0) as f32 * t).round() as i32;
        rects.push(ScheduleGraphRect {
            x: (x - 1).max(0),
            y: (y - 1).max(0),
            width: 3,
            height: 3,
            color: color.into(),
        });
    }
}

fn downsample_graph_points(points: &[(f32, f32)], max_points: usize) -> Vec<(f32, f32)> {
    if points.len() <= max_points || max_points < 3 {
        return points.to_vec();
    }
    let mut out = Vec::with_capacity(max_points);
    out.push(points[0]);
    let inner = max_points - 2;
    let span = points.len() - 2;
    for i in 0..inner {
        let idx = 1 + (i * span) / inner;
        out.push(points[idx]);
    }
    out.push(*points.last().unwrap_or(&points[0]));
    out
}

fn build_planned_points_from_schedule(
    schedule: &ScheduleFile,
    start_temp_c: f32,
) -> Vec<(f32, f32)> {
    let mut points: Vec<(f32, f32)> = vec![(0.0, start_temp_c.max(0.0))];
    let mut current_time_h = 0.0f32;
    let mut current_temp_c = start_temp_c.max(0.0);

    for step in &schedule.steps {
        let target_c = step.target.unwrap_or(current_temp_c.round() as i32) as f32;
        let rate_c_per_hour = step.rate.unwrap_or(0).abs() as f32;
        let hold_minutes = step.hold_time.unwrap_or(0).max(0) as f32;

        let diff = (target_c - current_temp_c).abs();
        let ramp_duration_h = diff / if rate_c_per_hour > 0.0 { rate_c_per_hour } else { 100.0 };
        current_time_h += ramp_duration_h;
        current_temp_c = target_c.max(0.0);
        points.push((current_time_h, current_temp_c));

        if hold_minutes > 0.0 {
            current_time_h += hold_minutes / 60.0;
            points.push((current_time_h, current_temp_c));
        }
    }

    points
}

fn find_schedule_for_history<'a>(name: &str, schedules: &'a [ScheduleFile]) -> Option<&'a ScheduleFile> {
    let normalized_name = normalize_schedule_name(name);
    if normalized_name.is_empty() {
        return None;
    }
    schedules.iter().find(|s| normalize_schedule_name(&s.name) == normalized_name)
}

fn apply_schedule_graph_zoom(ui: &AppWindow, zoom_level: i32) {
    let level = zoom_level.clamp(0, 2);
    let is_p4_profile = ui.get_board_profile_id() == 1;
    // Larger zoom levels allocate more space to the plot rectangle.
    let (x, w, h) = if is_p4_profile {
        match level {
            1 => (62, 656, 315),
            2 => (50, 670, 324),
            _ => (72, 642, 304),
        }
    } else {
        match level {
            1 => (32, 434, 212),
            2 => (26, 440, 218),
            _ => (40, 426, 206),
        }
    };
    ui.set_schedule_graph_zoom_level(level);
    ui.set_schedule_graph_plot_x_px(x);
    ui.set_schedule_graph_plot_w_px(w);
    ui.set_schedule_graph_plot_h_px(h);
}

fn set_schedule_graph_tip(ui: &AppWindow, graph_w: i32, graph_h: i32, point: &GraphTapPoint) {
    let display_w = unsafe { slint_bridge_get_display_width() }.max(1) as f32;
    let display_h = unsafe { slint_bridge_get_display_height() }.max(1) as f32;
    let density = (display_w / 480.0).min(display_h / 320.0).max(1.0);
    let char_px = (6.8_f32 * density).ceil() as i32;
    let line_h_temp = (14.0_f32 * density).ceil() as i32;
    let line_h_time = (13.0_f32 * density).ceil() as i32;
    let pad_x = (8.0_f32 * density).ceil() as i32;
    let pad_y = (6.0_f32 * density).ceil() as i32;
    let gap = (4.0_f32 * density).ceil() as i32;

    let max_chars = point
        .temp_label
        .chars()
        .count()
        .max(point.time_label.chars().count()) as i32;
    let mut tip_w = (max_chars * char_px + pad_x * 2).clamp(124, graph_w.saturating_sub(4).max(124));
    if tip_w > graph_w {
        tip_w = graph_w.max(80);
    }

    let avail_chars = ((tip_w - pad_x * 2) / char_px).max(1);
    let temp_lines = ((point.temp_label.chars().count() as i32 + avail_chars - 1) / avail_chars).max(1);
    let time_lines = ((point.time_label.chars().count() as i32 + avail_chars - 1) / avail_chars).max(1);
    let tip_h = (pad_y + temp_lines * line_h_temp + gap + time_lines * line_h_time + pad_y)
        .clamp(46, graph_h.saturating_sub(4).max(46));

    ui.set_schedule_graph_tip_w_px(tip_w);
    ui.set_schedule_graph_tip_h_px(tip_h);
    let tip_x = (point.x + 8).clamp(0, (graph_w - tip_w).max(0));
    let tip_y = (point.y - tip_h - 8).clamp(0, (graph_h - tip_h).max(0));
    ui.set_schedule_graph_tip_x_px(tip_x);
    ui.set_schedule_graph_tip_y_px(tip_y);
    ui.set_schedule_graph_tip_temp(point.temp_label.as_str().into());
    ui.set_schedule_graph_tip_time(point.time_label.as_str().into());
    ui.set_schedule_graph_tip_visible(true);
}

fn refresh_schedule_graph_model(ui: &AppWindow, state: &mut AppState) {
    let graph_w = ui.get_schedule_graph_plot_w_px().max(120);
    let graph_h = ui.get_schedule_graph_plot_h_px().max(120);
    const START_TEMP_C: f32 = 0.0;
    const MIN_PEAK_TEMP_C: f32 = 100.0;

    let mut clear_graph = || {
        state.graph_points.clear();
        ui.set_schedule_graph_rects(ModelRc::new(VecModel::from(Vec::<ScheduleGraphRect>::new())));
        ui.set_schedule_graph_markers(ModelRc::new(VecModel::from(Vec::<ScheduleGraphMarker>::new())));
        ui.set_schedule_graph_points(ModelRc::new(VecModel::from(Vec::<ScheduleGraphPoint>::new())));
        ui.set_schedule_graph_peak_temp(0);
        ui.set_schedule_graph_mid_temp(0);
        ui.set_schedule_graph_half_label("0 min".into());
        ui.set_schedule_graph_total_label("0 min".into());
        ui.set_schedule_graph_tip_visible(false);
    };

    let draw_series = |rects: &mut Vec<ScheduleGraphRect>,
                       points: &[(f32, f32)],
                       map_x: &dyn Fn(f32) -> i32,
                       map_y: &dyn Fn(f32) -> i32,
                       color: Color| {
        for window in points.windows(2) {
            let (start_h, start_temp_c) = window[0];
            let (end_h, end_temp_c) = window[1];
            let x0 = map_x(start_h).clamp(0, graph_w - 1);
            let y0 = map_y(start_temp_c).clamp(0, graph_h - 1);
            let x1 = map_x(end_h).clamp(0, graph_w - 1);
            let y1 = map_y(end_temp_c).clamp(0, graph_h - 1);

            if y0 == y1 {
                rects.push(ScheduleGraphRect {
                    x: x0.min(x1),
                    y: (y0 - 1).max(0),
                    width: (x1 - x0).abs().max(3),
                    height: 3,
                    color: color.into(),
                });
            } else {
                sample_graph_line(rects, x0, y0, x1, y1, color);
            }
        }
    };

    if let Some(history_id) = state.history_graph_id.as_deref() {
        let Some(detail) = load_history_detail(history_id) else {
            clear_graph();
            return;
        };
        if detail.data.is_empty() && detail.planned.is_empty() && detail.actual.is_empty() {
            clear_graph();
            return;
        }

        let first_ts = detail.data.first().map(|p| p.timestamp).unwrap_or(0);
        let ts_divisor = detect_timestamp_seconds_divisor(&detail.data);
        let start_temp_from_data = detail.data.first().map(|p| p.temp).unwrap_or(START_TEMP_C);
        let planned_from_schedule = find_schedule_for_history(&detail.summary.schedule_name, &state.schedules)
            .map(|s| build_planned_points_from_schedule(s, start_temp_from_data))
            .unwrap_or_default();
        let (raw_planned_points, raw_actual_points): (Vec<(f32, f32)>, Vec<(f32, f32)>) = if !detail.data.is_empty() {
            let planned = if planned_from_schedule.len() > 1 {
                planned_from_schedule
            } else {
                detail
                    .data
                    .iter()
                    .map(|p| ((((p.timestamp - first_ts).max(0) as f32) / ts_divisor) / 3600.0, p.target.max(0.0)))
                    .collect()
            };
            let actual = detail
                .data
                .iter()
                .map(|p| ((((p.timestamp - first_ts).max(0) as f32) / ts_divisor) / 3600.0, p.temp.max(0.0)))
                .collect();
            (planned, actual)
        } else {
            let planned = if planned_from_schedule.len() > 1 {
                planned_from_schedule
            } else {
                detail
                    .planned
                    .iter()
                    .filter(|p| p.t.is_finite() && p.temp.is_finite())
                    .map(|p| {
                        let hour = if p.timestamp > 0 {
                            ((p.timestamp as f32) / 3600.0).max(0.0)
                        } else {
                            (p.t / 60.0).max(0.0)
                        };
                        (hour, p.temp.max(0.0))
                    })
                    .collect::<Vec<_>>()
            };
            let actual = detail
                .actual
                .iter()
                .filter(|p| p.t.is_finite() && p.temp.is_finite())
                .map(|p| {
                    let hour = if p.timestamp > 0 {
                        ((p.timestamp as f32) / 3600.0).max(0.0)
                    } else {
                        (p.t / 60.0).max(0.0)
                    };
                    (hour, p.temp.max(0.0))
                })
                .collect::<Vec<_>>();
            (planned, actual)
        };
        let mut raw_actual_points = raw_actual_points;
        if detail.summary.duration > 0 {
            let max_actual_h = (detail.summary.duration as f32 / 3600.0).max(0.0) + 1.0 / 60.0;
            raw_actual_points.retain(|(h, _)| *h <= max_actual_h);
        }
        let mut planned_points: Vec<(f32, f32)> = Vec::new();
        for (i, point) in raw_planned_points.iter().copied().enumerate() {
            let prev = if i > 0 { Some(raw_planned_points[i - 1]) } else { None };
            let next = if i + 1 < raw_planned_points.len() {
                Some(raw_planned_points[i + 1])
            } else {
                None
            };
            let first_or_last = i == 0 || i + 1 == raw_planned_points.len();
            let y_changed_from_prev = prev.map(|p| (point.1 - p.1).abs() >= 0.1).unwrap_or(false);
            let y_changed_to_next = next.map(|p| (p.1 - point.1).abs() >= 0.1).unwrap_or(false);
            // Keep both sides of breakpoints so hold shelves keep exact start/end.
            if first_or_last || y_changed_from_prev || y_changed_to_next {
                planned_points.push(point);
            }
        }
        if planned_points.is_empty() {
            planned_points = raw_planned_points.clone();
        }

        let actual_points = downsample_graph_points(&raw_actual_points, 120);

        let last_hour = planned_points
            .last()
            .map(|(h, _)| *h)
            .unwrap_or(0.0)
            .max(actual_points.last().map(|(h, _)| *h).unwrap_or(0.0));
        let peak_temp_c = planned_points
            .iter()
            .map(|(_, t)| *t)
            .fold(0.0_f32, f32::max)
            .max(actual_points.iter().map(|(_, t)| *t).fold(0.0_f32, f32::max));
        let _total_minutes = if detail.summary.duration > 0 {
            duration_seconds_to_minutes(detail.summary.duration)
        } else {
            (last_hour * 60.0).ceil().max(1.0) as i32
        };
        let peak_display = temp_to_display_i32(peak_temp_c.max(MIN_PEAK_TEMP_C) as i32, state.temp_unit);
        let peak_scale_c = peak_temp_c.max(MIN_PEAK_TEMP_C);

        // Keep labels outside the plotted area: reserve paddings like in web chart.
        let plot_left = 19i32;
        let plot_right = (graph_w - 6).max(plot_left + 1);
        let plot_top = 8i32;
        let plot_bottom = (graph_h - 24).max(plot_top + 1);
        let plot_w = (plot_right - plot_left).max(1) as f32;
        let plot_h = (plot_bottom - plot_top).max(1) as f32;
        let x_scale = if last_hour <= 0.0 { 0.0 } else { plot_w / last_hour };
        let y_scale = if peak_scale_c <= 0.0 { 0.0 } else { plot_h / peak_scale_c };
        let map_x = |hour: f32| -> i32 { plot_left + (hour * x_scale).round() as i32 };
        let map_y = |temp_c: f32| -> i32 { plot_bottom - (temp_c * y_scale).round() as i32 };

        let target_line = Color::from_rgb_u8(0x63, 0x66, 0xF1);
        let target_point = Color::from_rgb_u8(0x81, 0x8C, 0xF8);
        let actual_line = Color::from_rgb_u8(0x10, 0xB9, 0x81);
        let actual_fill = Color::from_argb_u8(0x1A, 0x10, 0xB9, 0x81);

        let mut rects: Vec<ScheduleGraphRect> = Vec::new();
        let mut axis_markers: Vec<ScheduleGraphMarker> = Vec::new();
        let mut point_markers: Vec<ScheduleGraphPoint> = Vec::new();
        state.graph_points.clear();

        for window in actual_points.windows(2) {
            let (start_h, start_temp_c) = window[0];
            let (end_h, end_temp_c) = window[1];
            let x0 = map_x(start_h).clamp(plot_left, plot_right);
            let y0 = map_y(start_temp_c).clamp(plot_top, plot_bottom);
            let x1 = map_x(end_h).clamp(plot_left, plot_right);
            let y1 = map_y(end_temp_c).clamp(plot_top, plot_bottom);
            if x0 == x1 {
                let y = y0.min(y1);
                rects.push(ScheduleGraphRect { x: x0, y, width: 1, height: (plot_bottom - y + 1).max(1), color: actual_fill.into() });
                continue;
            }
            for x in x0.min(x1)..=x0.max(x1) {
                let t = (x - x0) as f32 / (x1 - x0) as f32;
                let y = ((y0 as f32 + (y1 - y0) as f32 * t).round() as i32).clamp(plot_top, plot_bottom);
                rects.push(ScheduleGraphRect { x, y, width: 1, height: (plot_bottom - y + 1).max(1), color: actual_fill.into() });
            }
        }

        draw_series(&mut rects, &actual_points, &map_x, &map_y, actual_line);

        for window in planned_points.windows(2) {
            let (sh, st) = window[0];
            let (eh, et) = window[1];
            let x0 = map_x(sh).clamp(0, graph_w - 1);
            let y0 = map_y(st).clamp(0, graph_h - 1);
            let x1 = map_x(eh).clamp(0, graph_w - 1);
            let y1 = map_y(et).clamp(0, graph_h - 1);
            let steps = (x1 - x0).abs().max((y1 - y0).abs()).clamp(1, 96);
            for i in 0..=steps {
                // Dash pattern 5 on / 5 off
                if (i / 5) % 2 != 0 {
                    continue;
                }
                let t = i as f32 / steps as f32;
                let x = (x0 as f32 + (x1 - x0) as f32 * t).round() as i32;
                let y = (y0 as f32 + (y1 - y0) as f32 * t).round() as i32;
                rects.push(ScheduleGraphRect { x: (x - 1).max(0), y: (y - 1).max(0), width: 3, height: 3, color: target_line.into() });
            }
        }

        let unit_label = match state.temp_unit {
            TempUnit::C => "\u{00B0}C",
            TempUnit::F => "\u{00B0}F",
        };
        for (hour, temp_c) in planned_points.iter().copied() {
            let x = map_x(hour).clamp(0, graph_w - 1);
            let y = map_y(temp_c).clamp(0, graph_h - 1);
            let temp_label = format!("{}{}", temp_to_display_i32(temp_c.round() as i32, state.temp_unit), unit_label);
            let time_label = format_axis_hour_minute_label((hour * 60.0).round().max(0.0) as i32, ui.get_is_ua());
            point_markers.push(ScheduleGraphPoint {
                x,
                y,
                temp_label: temp_label.as_str().into(),
                time_label: time_label.as_str().into(),
                color: target_point.into(),
            });
            state.graph_points.push(GraphTapPoint {
                x,
                y,
                temp_label,
                time_label,
            });
        }

        let start_minutes = 0_i32;
        let end_minutes = (last_hour * 60.0).round().max(0.0) as i32;
        for minutes in [start_minutes, end_minutes] {
            let hour = (minutes as f32) / 60.0;
            let x = map_x(hour).clamp(plot_left, plot_right);
            axis_markers.push(ScheduleGraphMarker {
                x,
                y: plot_bottom + 2,
                label: format_axis_hour_minute_label(minutes, ui.get_is_ua()).into(),
                color: Color::from_rgb_u8(0x71, 0x71, 0x7A).into(),
            });
        }

        ui.set_schedule_graph_rects(ModelRc::new(VecModel::from(rects)));
        ui.set_schedule_graph_markers(ModelRc::new(VecModel::from(axis_markers)));
        ui.set_schedule_graph_points(ModelRc::new(VecModel::from(point_markers)));
        ui.set_schedule_graph_peak_temp(peak_display);
        ui.set_schedule_graph_mid_temp(temp_to_display_i32((peak_scale_c * 0.5).round() as i32, state.temp_unit));
        ui.set_schedule_graph_half_label(format_axis_hour_minute_label(0, ui.get_is_ua()).into());
        ui.set_schedule_graph_total_label(format_axis_hour_minute_label((last_hour * 60.0).round().max(0.0) as i32, ui.get_is_ua()).into());
        ui.set_schedule_graph_tip_visible(false);
        return;
    }

    let Some(idx) = state.graph_index else {
        clear_graph();
        return;
    };

    let Some(schedule) = state.schedules.get(idx) else {
        clear_graph();
        return;
    };

    let mut points: Vec<(f32, f32)> = vec![(0.0, START_TEMP_C)];
    let mut current_time_h = 0.0f32;
    let mut current_temp_c = START_TEMP_C;
    let mut peak_temp_c = START_TEMP_C;

    for step in &schedule.steps {
        let target_c = step.target.unwrap_or(current_temp_c.round() as i32) as f32;
        let rate_c_per_hour = step.rate.unwrap_or(0).abs() as f32;
        let hold_minutes = step.hold_time.unwrap_or(0).max(0) as f32;

        let diff = (target_c - current_temp_c).abs();
        let ramp_duration_h = diff / if rate_c_per_hour > 0.0 { rate_c_per_hour } else { 100.0 };
        current_time_h += ramp_duration_h;
        current_temp_c = target_c;
        peak_temp_c = peak_temp_c.max(current_temp_c);
        points.push((current_time_h, current_temp_c));

        if hold_minutes > 0.0 {
            current_time_h += hold_minutes / 60.0;
            points.push((current_time_h, current_temp_c));
        }
    }

    let _total_minutes = (current_time_h * 60.0).ceil().max(1.0) as i32;
    let peak_display = temp_to_display_i32(peak_temp_c.max(MIN_PEAK_TEMP_C) as i32, state.temp_unit);
    let peak_scale_c = peak_temp_c.max(MIN_PEAK_TEMP_C);

    // Keep the schedule graph geometry aligned with the web editor chart:
    // we reserve paddings so labels/ticks stay outside the plotted area.
    let plot_left = 19i32;
    let plot_right = (graph_w - 6).max(plot_left + 1);
    let plot_top = 8i32;
    let plot_bottom = (graph_h - 24).max(plot_top + 1);
    let plot_w = (plot_right - plot_left).max(1) as f32;
    let plot_h = (plot_bottom - plot_top).max(1) as f32;

    let x_scale = if current_time_h <= 0.0 { 0.0 } else { plot_w / current_time_h };
    let y_scale = if peak_scale_c <= 0.0 { 0.0 } else { plot_h / peak_scale_c };

    let map_x = |hour: f32| -> i32 { plot_left + (hour * x_scale).round() as i32 };
    let map_y = |temp_c: f32| -> i32 { plot_bottom - (temp_c * y_scale).round() as i32 };

    let profile_line_color = Color::from_rgb_u8(0x10, 0xB9, 0x81);
    let profile_fill_color = Color::from_argb_u8(0x1A, 0x10, 0xB9, 0x81);
    let point_color = Color::from_rgb_u8(0x81, 0x8C, 0xF8);

    let mut rects: Vec<ScheduleGraphRect> = Vec::new();
    let mut axis_markers: Vec<ScheduleGraphMarker> = Vec::new();
    let mut point_markers: Vec<ScheduleGraphPoint> = Vec::new();
    state.graph_points.clear();

    for window in points.windows(2) {
        let (start_h, start_temp_c) = window[0];
        let (end_h, end_temp_c) = window[1];
        let x0 = map_x(start_h).clamp(plot_left, plot_right);
        let y0 = map_y(start_temp_c).clamp(plot_top, plot_bottom);
        let x1 = map_x(end_h).clamp(plot_left, plot_right);
        let y1 = map_y(end_temp_c).clamp(plot_top, plot_bottom);
        if x0 == x1 {
            let y = y0.min(y1);
            rects.push(ScheduleGraphRect {
                x: x0,
                y,
                width: 1,
                height: (plot_bottom - y + 1).max(1),
                color: profile_fill_color.into(),
            });
            continue;
        }
        for x in x0.min(x1)..=x0.max(x1) {
            let t = (x - x0) as f32 / (x1 - x0) as f32;
            let y = ((y0 as f32 + (y1 - y0) as f32 * t).round() as i32).clamp(plot_top, plot_bottom);
            rects.push(ScheduleGraphRect {
                x,
                y,
                width: 1,
                height: (plot_bottom - y + 1).max(1),
                color: profile_fill_color.into(),
            });
        }
    }

    draw_series(
        &mut rects,
        &points,
        &map_x,
        &map_y,
        profile_line_color,
    );

    let unit_label = match state.temp_unit {
        TempUnit::C => "\u{00B0}C",
        TempUnit::F => "\u{00B0}F",
    };
    for (hour, temp_c) in points.iter().copied() {
        let x = map_x(hour).clamp(0, graph_w - 1);
        let y = map_y(temp_c).clamp(0, graph_h - 1);

        let temp_label = format!(
            "{}{}",
            temp_to_display_i32(temp_c.round() as i32, state.temp_unit),
            unit_label
        );
        let time_label = format_axis_hour_minute_label((hour * 60.0).round().max(0.0) as i32, ui.get_is_ua());
        point_markers.push(ScheduleGraphPoint {
            x,
            y,
            temp_label: temp_label.as_str().into(),
            time_label: time_label.as_str().into(),
            color: point_color.into(),
        });
        state.graph_points.push(GraphTapPoint {
            x,
            y,
            temp_label,
            time_label,
        });
    }

    let tick_minutes = build_dynamic_tick_minutes(&points, 8);
    for minutes in tick_minutes {
        let hour = (minutes as f32) / 60.0;
        let x = map_x(hour).clamp(plot_left, plot_right);
        axis_markers.push(ScheduleGraphMarker {
            x,
            y: plot_bottom + 2,
            label: format_axis_hour_minute_label(minutes, ui.get_is_ua()).into(),
            color: Color::from_rgb_u8(0x71, 0x71, 0x7A).into(),
        });
    }

    ui.set_schedule_graph_rects(ModelRc::new(VecModel::from(rects)));
    ui.set_schedule_graph_markers(ModelRc::new(VecModel::from(axis_markers)));
    ui.set_schedule_graph_points(ModelRc::new(VecModel::from(point_markers)));
    ui.set_schedule_graph_peak_temp(peak_display);
    ui.set_schedule_graph_mid_temp(temp_to_display_i32((peak_scale_c * 0.5).round() as i32, state.temp_unit));
    ui.set_schedule_graph_half_label(format_axis_hour_minute_label(0, ui.get_is_ua()).into());
    ui.set_schedule_graph_total_label(
        format_axis_hour_minute_label((current_time_h * 60.0).round().max(0.0) as i32, ui.get_is_ua()).into(),
    );
    ui.set_schedule_graph_tip_visible(false);
}

fn c_buf_to_string(buf: &[u8]) -> String {
    let nul = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..nul]).trim().to_string()
}

fn is_thermocouple_disconnected(reason: &str) -> bool {
    let lower = reason.to_ascii_lowercase();
    lower.contains("sensor open")
        || lower.contains("sensor nan/open")
        || lower.contains("sensor comm/no-data")
        || lower.contains("sensor open fault")
        || lower.contains("thermocouple not initialized")
        || lower.contains("thermocouple read timeout")
}

fn apply_state_to_ui(ui: &AppWindow, state: SlintKilnState, app_state: &mut AppState) {
    let status_label = if state.fault_active {
        "ERROR"
    } else {
        match state.status {
            // Keep mapping aligned with firmware enum KilnStatus:
            // 0=IDLE, 1=RUNNING, 2=HOLD, 3=COOLING, 4=COMPLETE, 5=FAULT, 6=PAUSED, 7=TUNING
            1 => "RUNNING",
            2 => "HOLD",
            3 => "COOLING",
            4 => "COMPLETE",
            5 => "FAULT",
            6 => "PAUSED",
            7 => "TUNING",
            _ => "IDLE",
        }
    };
    let is_idle = !state.is_firing;

    let unit = app_state.temp_unit;
    let mut shown_temp_c = state.current_temp;
    if state.current_temp.is_finite() {
        if !app_state.display_temp_valid || !app_state.display_temp_c.is_finite() {
            app_state.display_temp_c = state.current_temp;
            app_state.display_temp_valid = true;
        } else {
            const ALPHA: f32 = 0.55;
            const DEAD_BAND_C: f32 = 0.12;
            let prev = app_state.display_temp_c;
            let blended = prev * (1.0 - ALPHA) + state.current_temp * ALPHA;
            app_state.display_temp_c = if (blended - prev).abs() < DEAD_BAND_C { prev } else { blended };
        }
        shown_temp_c = app_state.display_temp_c;
    } else {
        app_state.display_temp_valid = false;
    }

    let mut fault_reason = c_buf_to_string(&state.fault_reason);
    if fault_reason.is_empty() {
        fault_reason = c_buf_to_string(&state.error_msg);
    }
    let is_ua = ui.get_is_ua();
    let thermocouple_disconnected =
        is_thermocouple_disconnected(&fault_reason) && (!state.current_temp.is_finite() || state.fault_active);

    let shown_temp = temp_to_display(shown_temp_c, unit);
    // Avoid frequent micro-updates that cause visible refresh pulses on some MIPI panels.
    if (ui.get_temp() - shown_temp).abs() > 0.15 {
        ui.set_temp(shown_temp);
    }
    if thermocouple_disconnected {
        let next_temp_label = "--.-".to_string();
        let next_hint = if is_ua {
            "\u{0422}\u{0435}\u{0440}\u{043C}\u{043E}\u{043F}\u{0430}\u{0440}\u{0430} \u{043D}\u{0435} \u{043F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0435}\u{043D}\u{0430}".to_string()
        } else {
            "Thermocouple not connected".to_string()
        };
        if ui.get_temp_label().to_string() != next_temp_label && (ui.get_temp() - shown_temp).abs() > 0.15 {
            ui.set_temp_label(next_temp_label.into());
        }
        if ui.get_temp_hint().to_string() != next_hint {
            ui.set_temp_hint(next_hint.into());
        }
    } else {
        let next_temp_label = format!("{:.1}", shown_temp);
        if ui.get_temp_label().to_string() != next_temp_label && (ui.get_temp() - shown_temp).abs() > 0.15 {
            ui.set_temp_label(next_temp_label.into());
        }
        if !ui.get_temp_hint().is_empty() {
            ui.set_temp_hint("".into());
        }
    }
    let fallback_target = state.target_temp.round() as i32;
    let selected_schedule_target = if let Some(idx) = app_state.selected_index {
        if let Some(schedule) = app_state.schedules.get(idx) {
            schedule_effective_target_for_step(&schedule.steps, 0, fallback_target)
        } else {
            fallback_target
        }
    } else {
        fallback_target
    };
    let ui_target_c = if is_idle { selected_schedule_target } else { fallback_target };
    let next_target = temp_to_display_i32(ui_target_c, unit);
    if ui.get_target() != next_target {
        ui.set_target(next_target);
    }
    let segment_target = if let Some(idx) = app_state.selected_index {
        if let Some(schedule) = app_state.schedules.get(idx) {
            let step_for_target = if is_idle { 0 } else { state.current_step };
            running_segment_target(schedule, step_for_target, selected_schedule_target)
        } else {
            selected_schedule_target
        }
    } else {
        selected_schedule_target
    };
    let next_segment_target = temp_to_display_i32(segment_target, unit);
    if ui.get_active_segment_target() != next_segment_target {
        ui.set_active_segment_target(next_segment_target);
    }
    if ui.get_status_label().to_string() != status_label {
        ui.set_status_label(status_label.into());
    }
    if ui.get_is_idle() != is_idle {
        ui.set_is_idle(is_idle);
    }
    if ui.get_step_index() != state.current_step as i32 {
        ui.set_step_index(state.current_step as i32);
    }
    if ui.get_total_steps() != state.total_steps as i32 {
        ui.set_total_steps(state.total_steps as i32);
    }
    let next_time_remaining = format_minutes_to_hm(state.time_remaining);
    if ui.get_time_remaining().to_string() != next_time_remaining {
        ui.set_time_remaining(next_time_remaining.into());
    }
    let (step_remaining_min, firing_remaining_min) = if let Some(idx) = app_state.selected_index {
        if let Some(schedule) = app_state.schedules.get(idx) {
            estimate_remaining_minutes(schedule, state.current_step, state.current_temp, state.time_remaining)
        } else {
            (-1, state.time_remaining)
        }
    } else {
        (-1, state.time_remaining)
    };
    let next_step_time_remaining = format_minutes_to_hm(step_remaining_min);
    if ui.get_step_time_remaining().to_string() != next_step_time_remaining {
        ui.set_step_time_remaining(next_step_time_remaining.into());
    }
    let next_firing_time_remaining = format_minutes_to_hm(firing_remaining_min);
    if ui.get_firing_time_remaining().to_string() != next_firing_time_remaining {
        ui.set_firing_time_remaining(next_firing_time_remaining.into());
    }

    let prev_fault_active = ui.get_fault_active();
    if !state.fault_active {
        if ui.get_fault_popup_hidden() {
            ui.set_fault_popup_hidden(false);
        }
    } else if !prev_fault_active {
        if ui.get_fault_popup_hidden() {
            ui.set_fault_popup_hidden(false);
        }
    }
    if ui.get_fault_active() != state.fault_active {
        ui.set_fault_active(state.fault_active);
    }
    let next_fault_reason = translate_fault_reason(&fault_reason, is_ua);
    if ui.get_fault_reason().to_string() != next_fault_reason {
        ui.set_fault_reason(next_fault_reason.into());
    }

    let wifi_ok = unsafe { slint_bridge_wifi_is_connected() };
    if ui.get_header_wifi_ok() != wifi_ok {
        ui.set_header_wifi_ok(wifi_ok);
    }

    // Avoid global redraw side-effects on non-Wi-Fi screens:
    // keep the heavyweight Wi-Fi model in sync only when Wi-Fi UI is actually visible.
    if (ui.get_view() == View::Settings && ui.get_settings_page() == "wifi") || ui.get_wifi_qr_visible() {
        if ui.get_wifi_ok() != wifi_ok {
            ui.set_wifi_ok(wifi_ok);
        }
        let mut raw_url = vec![0 as c_char; 96];
        let server_url = if unsafe { slint_bridge_wifi_server_url_copy(raw_url.as_mut_ptr(), raw_url.len() as i32) } {
            unsafe { CStr::from_ptr(raw_url.as_ptr()) }.to_string_lossy().into_owned()
        } else {
            "http://192.168.4.1".to_string()
        };
        let old_url = ui.get_wifi_server_url().to_string();
        if old_url != server_url {
            ui.set_wifi_server_url(server_url.into());
        }
    }
    // Keep hot-path updates lightweight: settings values are refreshed elsewhere.

    let pzem_ok = state.pzem_ok && state.pzem_voltage.is_finite() && state.pzem_current.is_finite() && state.pzem_power.is_finite();
    let power_label = if pzem_ok {
        format!("{:.0} W", state.pzem_power.max(0.0))
    } else {
        "-- W".to_string()
    };
    let current_label = if pzem_ok {
        format!("{:.2} A", state.pzem_current.max(0.0))
    } else {
        "-- A".to_string()
    };
    let voltage_label = if pzem_ok {
        format!("{:.0} V", state.pzem_voltage.max(0.0))
    } else {
        "-- V".to_string()
    };
    let phase_label = if state.is_firing {
        if pzem_ok && state.pzem_current > 0.20 {
            if is_ua { "\u{0424}\u{0430}\u{0437}\u{0430}: OK".to_string() } else { "Phase: OK".to_string() }
        } else {
            if is_ua { "\u{0424}\u{0430}\u{0437}\u{0430}: \u{041D}\u{0406}".to_string() } else { "Phase: NO".to_string() }
        }
    } else {
        if is_ua { "\u{0424}\u{0430}\u{0437}\u{0430}: --".to_string() } else { "Phase: --".to_string() }
    };
    if ui.get_pzem_power_label().to_string() != power_label {
        ui.set_pzem_power_label(power_label.into());
    }
    if ui.get_pzem_current_label().to_string() != current_label {
        ui.set_pzem_current_label(current_label.into());
    }
    if ui.get_pzem_voltage_label().to_string() != voltage_label {
        ui.set_pzem_voltage_label(voltage_label.into());
    }
    if ui.get_pzem_phase_label().to_string() != phase_label {
        ui.set_pzem_phase_label(phase_label.into());
    }
    let next_standby_power = state.pzem_power.round().max(0.0) as i32;
    if ui.get_standby_power_watts() != next_standby_power {
        ui.set_standby_power_watts(next_standby_power);
    }
}

fn apply_command_result_to_ui(ui: &AppWindow, cmd: SlintCommandResult, is_ua: bool) {
    let action = c_buf_to_string(&cmd.action).to_lowercase();
    if action == "start" || action == "stop" || cmd.ok {
        // Keep success path redraw-minimal on NewP4:
        // for start/stop and other successful commands rely on state widgets update only.
        if ui.get_command_feedback_visible() {
            ui.set_command_feedback_visible(false);
        }
        return;
    }

    let mut message = c_buf_to_string(&cmd.message);
    if message.is_empty() {
        message = if cmd.ok { "ok".to_string() } else { "error".to_string() };
    }
    message = translate_command_message(&message, is_ua);
    let prefix = if is_ua { "\u{041A}\u{043E}\u{043C}\u{0430}\u{043D}\u{0434}\u{0430}" } else { "Command" };
    let label = if action.is_empty() {
        format!("{}: {}", prefix, message)
    } else {
        format!("{} {}: {}", prefix, action, message)
    };
    ui.set_command_feedback(label.into());
    ui.set_command_feedback_ok(cmd.ok);
    ui.set_command_feedback_visible(true);
}

fn state_poll() -> SlintKilnState {
    let mut out = SlintKilnState::default();
    unsafe { slint_bridge_get_state(&mut out as *mut SlintKilnState) };
    out
}

fn state_changed_for_ui(prev: SlintKilnState, cur: SlintKilnState) -> bool {
    let prev_cur_t = prev.current_temp.round() as i32;
    let cur_cur_t = cur.current_temp.round() as i32;
    let prev_tgt_t = prev.target_temp.round() as i32;
    let cur_tgt_t = cur.target_temp.round() as i32;
    if prev_cur_t != cur_cur_t || prev_tgt_t != cur_tgt_t {
        return true;
    }
    if prev.status != cur.status
        || prev.is_firing != cur.is_firing
        || prev.time_remaining != cur.time_remaining
        || prev.current_step != cur.current_step
        || prev.total_steps != cur.total_steps
        || prev.fault_active != cur.fault_active
        || prev.pzem_ok != cur.pzem_ok
    {
        return true;
    }
    let prev_v = prev.pzem_voltage.round() as i32;
    let cur_v = cur.pzem_voltage.round() as i32;
    let prev_i = (prev.pzem_current * 10.0).round() as i32;
    let cur_i = (cur.pzem_current * 10.0).round() as i32;
    let prev_p = prev.pzem_power.round() as i32;
    let cur_p = cur.pzem_power.round() as i32;
    if prev_v != cur_v || prev_i != cur_i || prev_p != cur_p {
        return true;
    }

    prev.error_msg != cur.error_msg || prev.fault_reason != cur.fault_reason
}

const CALIB_STEPS: usize = 9;

#[derive(Clone, Copy)]
struct CalibPoints {
    pts: [Option<(u16, u16)>; CALIB_STEPS],
}

impl Default for CalibPoints {
    fn default() -> Self {
        Self { pts: [None; CALIB_STEPS] }
    }
}

#[derive(Clone, Copy, Default)]
struct TouchCalibration {
    active: bool,
    step: u8,
    points: CalibPoints,
}

fn calib_target_for_step(step: u8) -> (i32, i32) {
    // Use margins so the user can comfortably tap the dot.
    // NOTE: app.slint assumes the same geometry.
    match step {
        0 => (20, 20),   // top-left
        1 => (240, 20),  // top-middle
        2 => (459, 20),  // top-right
        3 => (20, 160),  // mid-left
        4 => (240, 160), // mid
        5 => (459, 160), // mid-right
        6 => (20, 299),  // bottom-left
        7 => (240, 299), // bottom-middle
        8 => (459, 299), // bottom-right
        _ => (240, 160),
    }
}

fn calib_hint_for_step(is_ua: bool, step: u8) -> &'static str {
    match (is_ua, step) {
        (true, 0) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0432}\u{0435}\u{0440}\u{0445}-\u{043B}\u{0456}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (true, 1) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0432}\u{0435}\u{0440}\u{0445}-\u{0446}\u{0435}\u{043D}\u{0442}\u{0440})",
        (true, 2) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0432}\u{0435}\u{0440}\u{0445}-\u{043F}\u{0440}\u{0430}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (true, 3) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0446}\u{0435}\u{043D}\u{0442}\u{0440}-\u{043B}\u{0456}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (true, 4) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0446}\u{0435}\u{043D}\u{0442}\u{0440})",
        (true, 5) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{0446}\u{0435}\u{043D}\u{0442}\u{0440}-\u{043F}\u{0440}\u{0430}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (true, 6) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{043D}\u{0438}\u{0437}-\u{043B}\u{0456}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (true, 7) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{043D}\u{0438}\u{0437}-\u{0446}\u{0435}\u{043D}\u{0442}\u{0440})",
        (true, 8) => "\u{0422}\u{043E}\u{0440}\u{043A}\u{043D}\u{0456}\u{0442}\u{044C}\u{0441}\u{044F} \u{0442}\u{043E}\u{0447}\u{043A}\u{0438} (\u{043D}\u{0438}\u{0437}-\u{043F}\u{0440}\u{0430}\u{0432}\u{043E}\u{0440}\u{0443}\u{0447})",
        (false, 0) => "Tap the dot (top-left)",
        (false, 1) => "Tap the dot (top-middle)",
        (false, 2) => "Tap the dot (top-right)",
        (false, 3) => "Tap the dot (mid-left)",
        (false, 4) => "Tap the dot (center)",
        (false, 5) => "Tap the dot (mid-right)",
        (false, 6) => "Tap the dot (bottom-left)",
        (false, 7) => "Tap the dot (bottom-middle)",
        (false, 8) => "Tap the dot (bottom-right)",
        _ => "",
    }
}

fn update_calib_ui(ui: &AppWindow, calib: &TouchCalibration) {
    let is_ua = ui.get_is_ua();
    ui.set_view(View::Calibration);
    ui.set_calib_step(calib.step as i32);
    ui.set_calib_hint(calib_hint_for_step(is_ua, calib.step).into());
    let (tx, ty) = calib_target_for_step(calib.step);
    ui.set_calib_target_x(tx as i32);
    ui.set_calib_target_y(ty as i32);
}

fn compute_calibration(points: CalibPoints) -> Option<(u16, u16, u16, u16)> {
    // Convert 4 taps into a (left,right,top,bottom) calibration box used by apply_touch_calibration().
    let (tlx, tly) = points.pts[0]?;
    let (trx, try_) = points.pts[2]?;
    let (brx, bry) = points.pts[8]?;
    let (blx, bly) = points.pts[6]?;

    let x_l = ((tlx as u32 + blx as u32) / 2) as i32;
    let x_r = ((trx as u32 + brx as u32) / 2) as i32;
    let y_t = ((tly as u32 + try_ as u32) / 2) as i32;
    let y_b = ((bry as u32 + bly as u32) / 2) as i32;

    // Targets must match calib_target_for_step().
    const W: i32 = 480;
    const H: i32 = 320;
    // Keep calibration targets close to edges to avoid large extrapolation that can
    // create "dead borders" where the pointer can't reach the screen edge.
    const X_MARGIN: i32 = 20;
    const Y_MARGIN: i32 = 20;
    const X_SPAN_PIX: i32 = (W - 1) - 2 * X_MARGIN; // distance between left/right dot in pixels
    const Y_SPAN_PIX: i32 = (H - 1) - 2 * Y_MARGIN;
    if X_SPAN_PIX <= 0 || Y_SPAN_PIX <= 0 {
        return None;
    }

    let x_span_raw = x_r - x_l;
    let y_span_raw = y_b - y_t;
    if x_span_raw.abs() < 20 || y_span_raw.abs() < 20 {
        return None;
    }

    // Extrapolate raw bounds that should map to 0..W-1 and 0..H-1.
    // Derivation is explained in the handoff summary.
    let right_minus_left = (x_span_raw * (W - 1)) / X_SPAN_PIX;
    let bottom_minus_top = (y_span_raw * (H - 1)) / Y_SPAN_PIX;

    let left = x_l - (X_MARGIN * right_minus_left) / (W - 1);
    let top = y_t - (Y_MARGIN * bottom_minus_top) / (H - 1);
    let right = left + right_minus_left;
    let bottom = top + bottom_minus_top;

    // XPT2046 driver discards samples close to ADC rails (see xpt2046_read_data()).
    // Use the effective range so box calibration doesn't target unreachable values.
    const RAW_MIN: i32 = 50;
    const RAW_MAX: i32 = 4095 - 50;
    let clamp_u16 = |v: i32| -> u16 { v.clamp(RAW_MIN, RAW_MAX) as u16 };
    let left_u = clamp_u16(left);
    let right_u = clamp_u16(right);
    let top_u = clamp_u16(top);
    let bottom_u = clamp_u16(bottom);

    Some((left_u, right_u, top_u, bottom_u))
}

fn solve_affine_ls(points: &[(f32, f32)], targets: &[f32]) -> Option<(f32, f32, f32)> {
    if points.len() != targets.len() || points.len() < 3 {
        return None;
    }

    // Solve least squares for t ~= a*x + b*y + c.
    // Normal equations: (X^T X) theta = X^T t, where row = [x y 1].
    let mut s_xx = 0.0f32;
    let mut s_xy = 0.0f32;
    let mut s_x1 = 0.0f32;
    let mut s_yy = 0.0f32;
    let mut s_y1 = 0.0f32;
    let mut s_11 = 0.0f32;
    let mut s_xt = 0.0f32;
    let mut s_yt = 0.0f32;
    let mut s_1t = 0.0f32;

    for (i, &(x, y)) in points.iter().enumerate() {
        let t = targets[i];
        s_xx += x * x;
        s_xy += x * y;
        s_x1 += x;
        s_yy += y * y;
        s_y1 += y;
        s_11 += 1.0;
        s_xt += x * t;
        s_yt += y * t;
        s_1t += t;
    }

    // Matrix:
    // [ s_xx s_xy s_x1 ] [a]   [s_xt]
    // [ s_xy s_yy s_y1 ] [b] = [s_yt]
    // [ s_x1 s_y1 s_11 ] [c]   [s_1t]
    let m00 = s_xx;
    let m01 = s_xy;
    let m02 = s_x1;
    let m10 = s_xy;
    let m11 = s_yy;
    let m12 = s_y1;
    let m20 = s_x1;
    let m21 = s_y1;
    let m22 = s_11;

    let det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    if det.abs() < 1e-6 {
        return None;
    }

    // Cramer's rule (3x3) for small system.
    let inv_det = 1.0f32 / det;

    let det_a = s_xt * (m11 * m22 - m12 * m21) - m01 * (s_yt * m22 - m12 * s_1t) + m02 * (s_yt * m21 - m11 * s_1t);
    let det_b = m00 * (s_yt * m22 - m12 * s_1t) - s_xt * (m10 * m22 - m12 * m20) + m02 * (m10 * s_1t - s_yt * m20);
    let det_c = m00 * (m11 * s_1t - s_yt * m21) - m01 * (m10 * s_1t - s_yt * m20) + s_xt * (m10 * m21 - m11 * m20);

    let a = det_a * inv_det;
    let b = det_b * inv_det;
    let c = det_c * inv_det;
    Some((a, b, c))
}

fn compute_affine_after_box(
    points: CalibPoints,
    cal: (u16, u16, u16, u16),
) -> Option<(f32, f32, f32, f32, f32, f32)> {
    // IMPORTANT: firmware applies affine AFTER the (left/right/top/bottom) box scaling.
    // So we must solve affine in that same coordinate space, otherwise it will over-scale and clamp.
    let mut mapped: Vec<(f32, f32)> = Vec::with_capacity(CALIB_STEPS);
    let mut tx: Vec<f32> = Vec::with_capacity(CALIB_STEPS);
    let mut ty: Vec<f32> = Vec::with_capacity(CALIB_STEPS);

    for i in 0..CALIB_STEPS {
        let p = points.pts[i]?;
        let (mx, my) = map_point(p, cal, None);
        let (tix, tiy) = calib_target_for_step(i as u8);
        mapped.push((mx, my));
        tx.push(tix as f32);
        ty.push(tiy as f32);
    }

    let (a, b, c) = solve_affine_ls(&mapped, &tx)?;
    let (d, e, f) = solve_affine_ls(&mapped, &ty)?;
    Some((a, b, c, d, e, f))
}

fn map_point(
    p: (u16, u16),
    cal: (u16, u16, u16, u16),
    aff: Option<(f32, f32, f32, f32, f32, f32)>,
) -> (f32, f32) {
    let (l, r, t, b) = cal;
    let (px, py) = (p.0 as f32, p.1 as f32);
    let w = 480.0f32;
    let h = 320.0f32;
    let denom_x = (r as f32 - l as f32).max(1.0);
    let denom_y = (b as f32 - t as f32).max(1.0);

    let mut x = (px - l as f32) * (w - 1.0) / denom_x;
    let mut y = (py - t as f32) * (h - 1.0) / denom_y;
    x = x.clamp(0.0, w - 1.0);
    y = y.clamp(0.0, h - 1.0);

    if let Some((a, bb, c, d, e, f)) = aff {
        let xa = a * x + bb * y + c;
        let ya = d * x + e * y + f;
        x = xa.clamp(0.0, w - 1.0);
        y = ya.clamp(0.0, h - 1.0);
    }

    (x, y)
}

fn compute_grid_residuals(
    points: CalibPoints,
    cal: (u16, u16, u16, u16),
    aff: Option<(f32, f32, f32, f32, f32, f32)>,
) -> Option<([f32; CALIB_STEPS], [f32; CALIB_STEPS])> {
    let mut dx = [0.0f32; CALIB_STEPS];
    let mut dy = [0.0f32; CALIB_STEPS];

    for i in 0..CALIB_STEPS {
        let p = points.pts[i]?;
        let (tx, ty) = calib_target_for_step(i as u8);
        let (mx, my) = map_point(p, cal, aff);
        dx[i] = tx as f32 - mx;
        dy[i] = ty as f32 - my;
    }

    Some((dx, dy))
}

#[no_mangle]
pub extern "C" fn slint_ui_run() {
    unsafe {
        slint_bridge_display_init();
    }

    let board_profile_id = unsafe { slint_bridge_get_board_profile() } as i32;
    // IMPORTANT: call the WindowAdapter::set_size() implementation so the renderer sees the correct size.
    let physical_w = unsafe { slint_bridge_get_display_width() }.max(1);
    let physical_h = unsafe { slint_bridge_get_display_height() }.max(1);
    let is_new_p4_display = board_profile_id == 1 || physical_w >= 800 || physical_h >= 480;
    // Stable path: keep a reused software buffer and push complete frame with draw_bitmap.
    let repaint_type = RepaintBufferType::ReusedBuffer;
    let window = MinimalSoftwareWindow::new(repaint_type);
    slint::platform::set_platform(Box::new(EspPlatform {
        window: window.clone(),
        start: Instant::now(),
    }))
    .expect("set_platform failed");

    // For the NewP4 we render at the panel's native resolution (800x480) without relying on Slint's HiDPI scale
    // factor. This keeps display coordinates and touch coordinates 1:1 and avoids "letterboxing" artifacts when our
    // platform backend forwards raw pixel coordinates to the driver.
    let ui_scale: f32 = 1.0;
    let logical_w = physical_w;
    let logical_h = physical_h;

    let _ = window.dispatch_event(WindowEvent::ScaleFactorChanged { scale_factor: ui_scale });
    // The MinimalSoftwareWindow also has a convenience `set_size()` method, but that does not update the adapter size.
    slint::platform::WindowAdapter::set_size(
        window.as_ref(),
        slint::PhysicalSize::new(physical_w as u32, physical_h as u32).into(),
    );

    let ui = AppWindow::new().expect("Failed to create UI");
    ui.set_board_profile_id(board_profile_id);
    ui.set_board_display_w(logical_w);
    ui.set_board_display_h(logical_h);
    ui.window().show().unwrap_or(());
    ui.set_boot_splash_visible(true);
    {
        let ui_weak = ui.as_weak();
        Timer::single_shot(Duration::from_millis(3500), move || {
            if let Some(ui) = ui_weak.upgrade() {
                ui.set_boot_splash_visible(false);
            }
        });
    }
    let lang_is_ua = unsafe { slint_bridge_get_language_is_ua() };
    ui.set_is_ua(lang_is_ua);
    let temp_unit = match unsafe { slint_bridge_get_temp_unit() } {
        1 => TempUnit::F,
        _ => TempUnit::C,
    };
    let time_format = match unsafe { slint_bridge_get_time_format() } {
        1 => TimeFormat::H12,
        _ => TimeFormat::H24,
    };
    let date_format = match unsafe { slint_bridge_get_date_format() } {
        1 => DateFormat::Mdy,
        2 => DateFormat::Ymd,
        _ => DateFormat::Dmy,
    };
    ui.set_temp_unit(match temp_unit { TempUnit::C => "C", TempUnit::F => "F" }.into());
    ui.set_time_format(match time_format { TimeFormat::H24 => "24", TimeFormat::H12 => "12" }.into());
    ui.set_date_format(match date_format { DateFormat::Dmy => "DMY", DateFormat::Mdy => "MDY", DateFormat::Ymd => "YMD" }.into());
    let mut init_brightness = unsafe { slint_bridge_get_display_brightness() } as i32;
    if init_brightness < 50 { init_brightness = 50; }
    if init_brightness > 100 { init_brightness = 100; }
    ui.set_display_brightness(init_brightness);
    ui.set_ui_dim(((100 - init_brightness).clamp(0, 100) as f32) / 100.0);
    ui.set_temp_unit_label(match temp_unit { TempUnit::C => "\u{00B0}C", TempUnit::F => "\u{00B0}F" }.into());
    ui.set_rate_unit_label(match temp_unit { TempUnit::C => "\u{00B0}C/h", TempUnit::F => "\u{00B0}F/h" }.into());
    ui.set_rate_unit_label_min(match temp_unit { TempUnit::C => "\u{00B0}C/min", TempUnit::F => "\u{00B0}F/min" }.into());
    ui.set_wifi_networks(ModelRc::new(VecModel::from(Vec::<WifiNetworkRow>::new())));
    ui.set_wifi_qr_image(build_qr_image(&wifi_qr_payload(false, "", CONTROLLER_AP_SSID_FALLBACK)));
    ui.set_wifi_qr_hint(
        if lang_is_ua {
            "\u{0412}\u{0456}\u{0434}\u{0441}\u{043A}\u{0430}\u{043D}\u{0443}\u{0439}\u{0442}\u{0435}, \u{0449}\u{043E}\u{0431} \u{043F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0438}\u{0442}\u{0438}\u{0441}\u{044F} \u{0434}\u{043E} Wi-Fi \u{043A}\u{043E}\u{043D}\u{0442}\u{0440}\u{043E}\u{043B}\u{0435}\u{0440}\u{0430}"
        } else {
            "Scan to join controller Wi-Fi"
        }
        .into(),
    );

    let mut state = AppState {
        schedules: load_schedules(),
        selected_index: None,
        editing_index: None,
        graph_index: None,
        graph_zoom_level: 0,
        graph_points: Vec::new(),
        history_graph_id: None,
        history_detail_id: None,
        editor_steps: Vec::new(),
        display_temp_c: 0.0,
        display_temp_valid: false,
        temp_unit,
        time_format,
        date_format,
    };
    apply_schedule_graph_zoom(&ui, state.graph_zoom_level);

    if !state.schedules.is_empty() {
        state.selected_index = Some(0);
        let selected = &state.schedules[0];
        ui.set_selected_schedule_name(selected.name.clone().into());
        ui.set_selected_steps_count(schedule_steps_count(selected));

        if let Ok(json) = serde_json::to_string(selected) {
            let c_json = CString::new(json).ok();
            if let Some(cstr) = c_json {
                unsafe {
                    let _ = slint_bridge_load_schedule_json(cstr.as_ptr());
                }
            }
        }
    }

    refresh_schedule_model(&ui, &state);

    let app_state = Rc::new(RefCell::new(state));
    let ui_weak = ui.as_weak();
    let calib_state: Rc<RefCell<TouchCalibration>> = Rc::new(RefCell::new(TouchCalibration::default()));
    let start_cooldown_at = Rc::new(RefCell::new(Instant::now() - Duration::from_secs(5)));
    let stop_cooldown_at = Rc::new(RefCell::new(Instant::now() - Duration::from_secs(5)));
    let clear_fault_cooldown_at = Rc::new(RefCell::new(Instant::now() - Duration::from_secs(5)));
    let clear_fault_inflight = Rc::new(RefCell::new(false));
    let kb_key_last = Rc::new(RefCell::new((String::new(), Instant::now() - Duration::from_secs(5))));
    let wifi_scan_in_progress = Rc::new(RefCell::new(false));
    let wifi_connect_pending_deadline = Rc::new(RefCell::new(None::<Instant>));
    let wifi_connect_target_ssid = Rc::new(RefCell::new(String::new()));
    let render_suppress_until = Rc::new(RefCell::new(Instant::now()));
    let command_feedback_until = Rc::new(RefCell::new(Instant::now()));
    let fault_log_cache: Rc<RefCell<Vec<FaultLogPreviewCard>>> = Rc::new(RefCell::new(Vec::new()));
    // 0=none, 1=start tap, 2=stop tap, 3=stop confirm, 4=wifi got ip, 5=cmd start, 6=cmd stop
    let render_reason = Rc::new(RefCell::new((0u8, Instant::now() - Duration::from_secs(5))));

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_library(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            // Defensive: ensure no modal/overlay is left active when navigating.
            ui.set_standby_visible(false);
            ui.set_boot_splash_visible(false);
            ui.set_fault_log_detail_visible(false);
            ui.set_show_stop_confirm(false);
            ui.set_show_skip_confirm(false);
            ui.set_show_running_edit_confirm(false);
            ui.set_show_touch_confirm(false);
            ui.set_delete_confirm_visible(false);
            ui.set_kb_visible(false);
            ui.set_command_feedback_visible(false);
            ui.set_view(View::Schedules);
            let state = app_state.borrow();
            refresh_schedule_model(&ui, &state);
            // Guardrail: if the schedule list is empty (load failure or storage not ready),
            // don't leave the user on a dead/blank page with no actionable UI.
            if ui.get_schedules().row_count() == 0 {
                ui.set_command_feedback(
                    if ui.get_is_ua() {
                        "\u{041D}\u{0435}\u{043C}\u{0430}\u{0454} \u{043F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C} \u{0430}\u{0431}\u{043E} \u{043D}\u{0435} \u{0432}\u{0434}\u{0430}\u{043B}\u{043E}\u{0441}\u{044F} \u{0437}\u{0430}\u{0432}\u{0430}\u{043D}\u{0442}\u{0430}\u{0436}\u{0438}\u{0442}\u{0438}".into()
                    } else {
                        "No programs found / load failed".into()
                    }
                );
                ui.set_command_feedback_ok(false);
                ui.set_command_feedback_visible(true);
                ui.set_view(View::Dashboard);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_selected_program(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(idx) = state.selected_index {
                if idx < state.schedules.len() {
                    state.editing_index = Some(idx);
                    let schedule = state.schedules[idx].clone();
                    state.editor_steps = to_editor_steps(&schedule, state.temp_unit);
                    ui.set_selected_schedule_name(schedule.name.clone().into());
                    refresh_editor_model(&ui, &state);
                    ui.set_view(View::Editor);
                } else {
                    ui.set_view(View::Schedules);
                }
            } else {
                ui.set_view(View::Schedules);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_running_editor(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(idx) = state.selected_index {
                if idx < state.schedules.len() {
                    state.editing_index = Some(idx);
                    let schedule = state.schedules[idx].clone();
                    state.editor_steps = to_editor_steps(&schedule, state.temp_unit);
                    ui.set_selected_schedule_name(schedule.name.clone().into());
                    refresh_editor_model(&ui, &state);
                    ui.set_view(View::Editor);
                    return;
                }
            }
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_open_history(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_history_items(ModelRc::new(VecModel::from(load_history_items(ui.get_is_ua()))));
            ui.set_settings_page("history".into());
            ui.set_view(View::Settings);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let fault_log_cache = fault_log_cache.clone();
        ui.on_open_fault_logs(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let items = load_fault_log_items(ui.get_is_ua());
            *fault_log_cache.borrow_mut() = items.clone();
            ui.set_fault_log_items(ModelRc::new(VecModel::from(items)));
            ui.set_fault_log_detail_visible(false);
            ui.set_settings_page("fault_logs".into());
            ui.set_view(View::Settings);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let fault_log_cache = fault_log_cache.clone();
        ui.on_open_fault_log_detail(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let idx = index as usize;
            let cache = fault_log_cache.borrow();
            let Some(item) = cache.get(idx) else { return; };
            ui.set_fault_log_detail_title(item.title.clone());
            ui.set_fault_log_detail_status(item.status.clone());
            ui.set_fault_log_detail_text(item.details.clone());
            ui.set_fault_log_detail_visible(true);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_close_fault_log_detail(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_fault_log_detail_visible(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_schedule_graph(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let idx = index as usize;
            if idx < state.schedules.len() {
                state.selected_index = Some(idx);
                state.graph_index = None;
                state.history_graph_id = None;
                let schedule = state.schedules[idx].clone();
                state.graph_index = Some(idx);
                ui.set_selected_schedule_name(schedule.name.clone().into());
                ui.set_selected_steps_count(schedule_steps_count(&schedule));
                ui.set_schedule_graph_back_to_history(false);
                state.graph_zoom_level = 0;
                apply_schedule_graph_zoom(&ui, state.graph_zoom_level);
                refresh_schedule_graph_model(&ui, &mut state);
                ui.set_view(View::ScheduleGraph);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_history_graph(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let idx = index as usize;
            let Some(id) = load_history_item_id_by_preview_index(idx) else { return; };
            let detail = load_history_detail(&id);
            let mut state = app_state.borrow_mut();
            state.graph_index = None;
            state.history_graph_id = Some(id.clone());
            ui.set_schedule_graph_back_to_history(true);
            if let Some(detail) = detail {
                let title = if detail.summary.schedule_name.is_empty() {
                    if ui.get_is_ua() { "\u{0412}\u{0438}\u{043F}\u{0430}\u{043B}" } else { "Firing" }
                } else {
                    detail.summary.schedule_name.as_str()
                };
                ui.set_selected_schedule_name(title.into());
                ui.set_selected_steps_count(detail.summary.total_steps.max(0) as i32);
            } else {
                ui.set_selected_schedule_name((if ui.get_is_ua() { "\u{0412}\u{0438}\u{043F}\u{0430}\u{043B}" } else { "Firing" }).into());
                ui.set_selected_steps_count(0);
            }
            apply_schedule_graph_zoom(&ui, state.graph_zoom_level);
            refresh_schedule_graph_model(&ui, &mut state);
            ui.set_view(View::ScheduleGraph);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_schedule_graph_zoom_in(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if !ui.get_schedule_graph_back_to_history() {
                return;
            }
            let mut state = app_state.borrow_mut();
            state.graph_zoom_level = (state.graph_zoom_level + 1).clamp(0, 2);
            apply_schedule_graph_zoom(&ui, state.graph_zoom_level);
            refresh_schedule_graph_model(&ui, &mut state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_schedule_graph_zoom_out(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if !ui.get_schedule_graph_back_to_history() {
                return;
            }
            let mut state = app_state.borrow_mut();
            state.graph_zoom_level = (state.graph_zoom_level - 1).clamp(0, 2);
            apply_schedule_graph_zoom(&ui, state.graph_zoom_level);
            refresh_schedule_graph_model(&ui, &mut state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_schedule_graph_zoom_reset(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if !ui.get_schedule_graph_back_to_history() {
                return;
            }
            let mut state = app_state.borrow_mut();
            state.graph_zoom_level = 0;
            apply_schedule_graph_zoom(&ui, state.graph_zoom_level);
            refresh_schedule_graph_model(&ui, &mut state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_schedule_graph_point_tapped(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if index < 0 {
                ui.set_schedule_graph_tip_visible(false);
                return;
            }
            let state = app_state.borrow();
            let idx = index as usize;
            let Some(point) = state.graph_points.get(idx) else {
                ui.set_schedule_graph_tip_visible(false);
                return;
            };
            let graph_w = ui.get_schedule_graph_plot_w_px().max(120);
            let graph_h = ui.get_schedule_graph_plot_h_px().max(120);
            set_schedule_graph_tip(&ui, graph_w, graph_h, point);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_history_detail(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let idx = index as usize;
            let summary_fallback = load_history_summary_by_preview_index(idx);
            let id = load_history_item_id_by_preview_index(idx).or_else(|| {
                summary_fallback
                    .as_ref()
                    .map(|s| s.id.clone())
                    .filter(|s| !s.is_empty())
            });
            let detail = id.as_ref().and_then(|detail_id| load_history_detail(detail_id));
            let mut state = app_state.borrow_mut();
            state.history_detail_id = id;
            if let Some(detail) = detail {
                let title = if detail.summary.schedule_name.is_empty() {
                    if ui.get_is_ua() { "\u{0412}\u{0438}\u{043F}\u{0430}\u{043B}" } else { "Firing" }
                } else {
                    detail.summary.schedule_name.as_str()
                };
                ui.set_history_detail_title(title.into());
                ui.set_history_detail_status(history_status_label(&detail.summary.status, ui.get_is_ua()).into());
                let duration_label = format_minutes_label(duration_seconds_to_minutes(detail.summary.duration), ui.get_is_ua());
                ui.set_history_detail_duration(duration_label.into());
                ui.set_history_detail_steps(detail.summary.total_steps.max(0).to_string().into());
                let energy_kwh = compute_energy_kwh(&detail);
                let energy_label = if energy_kwh > 0.0 {
                    if ui.get_is_ua() {
                        format!("{:.2} \u{043A}\u{0412}\u{0442}\u{00B7}\u{0433}\u{043E}\u{0434}", energy_kwh)
                    } else {
                        format!("{:.2} kWh", energy_kwh)
                    }
                } else if ui.get_is_ua() {
                    "-- \u{043A}\u{0412}\u{0442}\u{00B7}\u{0433}\u{043E}\u{0434}".to_string()
                } else {
                    "-- kWh".to_string()
                };
                ui.set_history_detail_energy(energy_label.into());
                let step_label = if detail.summary.total_steps > 0 && detail.summary.duration > 0 {
                    let avg = ((detail.summary.duration as f32 / 60.0) / detail.summary.total_steps as f32).round() as i32;
                    if ui.get_is_ua() {
                        format!("\u{2248}{} \u{0445}\u{0432}/\u{043A}\u{0440}\u{043E}\u{043A}", avg)
                    } else {
                        format!("\u{2248}{} min/step", avg)
                    }
                } else {
                    if ui.get_is_ua() { "-- \u{0445}\u{0432}/\u{043A}\u{0440}\u{043E}\u{043A}" } else { "-- min/step" }.to_string()
                };
                ui.set_history_detail_step_durations(step_label.into());

                let text_fallback = if ui.get_is_ua() { "\u{041D}/\u{0414}" } else { "N/A" };
                ui.set_history_detail_program_name(ui_clean_text(title, text_fallback).into());
                let start_label = history_start_label_from_summary(&detail.summary, ui.get_is_ua())
                    .unwrap_or_else(|| text_fallback.to_string());
                ui.set_history_detail_start_time(ui_clean_text(&start_label, text_fallback).into());
                let end_label = history_end_label_from_summary(&detail.summary, ui.get_is_ua())
                    .unwrap_or_else(|| text_fallback.to_string());
                ui.set_history_detail_end_time(ui_clean_text(&end_label, text_fallback).into());
                let status_verbose = history_status_verbose(&detail.summary, ui.get_is_ua());
                ui.set_history_detail_status_verbose(ui_clean_text(&status_verbose, "--").into());
                let cost_label = if detail.summary.cost > 0.0 {
                    if ui.get_is_ua() {
                        format!("{:.2} \u{0433}\u{0440}\u{043D}", detail.summary.cost)
                    } else {
                        format!("{:.2} UAH", detail.summary.cost)
                    }
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414} (\u{0442}\u{0430}\u{0440}\u{0438}\u{0444} \u{043D}\u{0435} \u{0437}\u{0430}\u{0434}\u{0430}\u{043D}\u{043E})".to_string()
                } else {
                    "N/A (tariff not set)".to_string()
                };
                ui.set_history_detail_energy_cost(ui_clean_text(&cost_label, "--").into());
                let peak_power = detail
                    .data
                    .iter()
                    .map(|s| s.power.max(0.0))
                    .fold(detail.summary.peak_power.max(0.0), f32::max);
                let peak_current = detail
                    .data
                    .iter()
                    .map(|s| s.current.max(0.0))
                    .fold(detail.summary.peak_current.max(0.0), f32::max);
                let max_load = if peak_power > 0.0 || peak_current > 0.0 {
                    if ui.get_is_ua() {
                        format!("{:.0} \u{0412}\u{0442} / {:.1} A", peak_power, peak_current)
                    } else {
                        format!("{:.0} W / {:.1} A", peak_power, peak_current)
                    }
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414}".to_string()
                } else {
                    "N/A".to_string()
                };
                ui.set_history_detail_max_load(ui_clean_text(&max_load, "--").into());
                let peak_temp = detail
                    .data
                    .iter()
                    .map(|s| s.temp.max(0.0))
                    .fold(detail.summary.peak_temp.max(0.0), f32::max);
                let peak_temp_label = if peak_temp > 0.0 {
                    format!("{:.1}{}", peak_temp, ui.get_temp_unit_label())
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414}".to_string()
                } else {
                    "N/A".to_string()
                };
                ui.set_history_detail_peak_temp(ui_clean_text(&peak_temp_label, "--").into());
                let voltage_stability = compute_voltage_stability(&detail, ui.get_is_ua());
                ui.set_history_detail_voltage_stability(ui_clean_text(&voltage_stability, "--").into());
                let step_plan_actual = build_step_and_change_text(&detail, ui.get_is_ua());
                ui.set_history_detail_step_plan_actual(ui_clean_multiline(&step_plan_actual, "--").into());
            } else if let Some(summary) = summary_fallback {
                let title = if summary.schedule_name.is_empty() {
                    if ui.get_is_ua() { "\u{0412}\u{0438}\u{043F}\u{0430}\u{043B}" } else { "Firing" }
                } else {
                    summary.schedule_name.as_str()
                };
                ui.set_history_detail_title(title.into());
                ui.set_history_detail_status(history_status_label(&summary.status, ui.get_is_ua()).into());
                ui.set_history_detail_duration(format_minutes_label(duration_seconds_to_minutes(summary.duration), ui.get_is_ua()).into());
                ui.set_history_detail_steps(summary.total_steps.max(0).to_string().into());
                let energy_kwh = if summary.energy_kwh > 0.0 {
                    summary.energy_kwh
                } else if summary.energy_wh > 0.0 {
                    summary.energy_wh / 1000.0
                } else {
                    0.0
                };
                let energy_label = if energy_kwh > 0.0 {
                    if ui.get_is_ua() {
                        format!("{:.2} \u{043A}\u{0412}\u{0442}\u{00B7}\u{0433}\u{043E}\u{0434}", energy_kwh)
                    } else {
                        format!("{:.2} kWh", energy_kwh)
                    }
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414}".to_string()
                } else {
                    "N/A".to_string()
                };
                ui.set_history_detail_energy(energy_label.into());
                let step_label = if summary.total_steps > 0 && summary.duration > 0 {
                    let avg = ((summary.duration as f32 / 60.0) / summary.total_steps as f32).round() as i32;
                    if ui.get_is_ua() {
                        format!("\u{2248}{} \u{0445}\u{0432}/\u{043A}\u{0440}\u{043E}\u{043A}", avg)
                    } else {
                        format!("\u{2248}{} min/step", avg)
                    }
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414}".to_string()
                } else {
                    "N/A".to_string()
                };
                ui.set_history_detail_step_durations(step_label.into());
                let text_fallback = if ui.get_is_ua() { "\u{041D}/\u{0414}" } else { "N/A" };
                ui.set_history_detail_program_name(ui_clean_text(title, text_fallback).into());
                let start_label = history_start_label_from_summary(&summary, ui.get_is_ua())
                    .unwrap_or_else(|| text_fallback.to_string());
                ui.set_history_detail_start_time(ui_clean_text(&start_label, text_fallback).into());
                let end_label = history_end_label_from_summary(&summary, ui.get_is_ua())
                    .unwrap_or_else(|| text_fallback.to_string());
                ui.set_history_detail_end_time(ui_clean_text(&end_label, text_fallback).into());
                let status_verbose = history_status_verbose(&summary, ui.get_is_ua());
                ui.set_history_detail_status_verbose(ui_clean_text(&status_verbose, "--").into());
                ui.set_history_detail_energy_cost(
                    if summary.cost > 0.0 {
                        if ui.get_is_ua() { format!("{:.2} \u{0433}\u{0440}\u{043D}", summary.cost) } else { format!("{:.2} UAH", summary.cost) }
                    } else if ui.get_is_ua() {
                        "\u{041D}/\u{0414} (\u{0442}\u{0430}\u{0440}\u{0438}\u{0444} \u{043D}\u{0435} \u{0437}\u{0430}\u{0434}\u{0430}\u{043D}\u{043E})".to_string()
                    } else {
                        "N/A (tariff not set)".to_string()
                    }
                    .into(),
                );
                let max_load = if summary.peak_power > 0.0 || summary.peak_current > 0.0 {
                    if ui.get_is_ua() {
                        format!("{:.0} \u{0412}\u{0442} / {:.1} A", summary.peak_power.max(0.0), summary.peak_current.max(0.0))
                    } else {
                        format!("{:.0} W / {:.1} A", summary.peak_power.max(0.0), summary.peak_current.max(0.0))
                    }
                } else if ui.get_is_ua() {
                    "\u{041D}/\u{0414}".to_string()
                } else {
                    "N/A".to_string()
                };
                ui.set_history_detail_max_load(ui_clean_text(&max_load, "--").into());
                ui.set_history_detail_peak_temp(
                    if summary.peak_temp > 0.0 {
                        format!("{:.1}{}", summary.peak_temp, ui.get_temp_unit_label())
                    } else if ui.get_is_ua() {
                        "\u{041D}/\u{0414}".to_string()
                    } else {
                        "N/A".to_string()
                    }
                    .into(),
                );
                ui.set_history_detail_voltage_stability((if ui.get_is_ua() { "\u{041D}/\u{0414}" } else { "N/A" }).into());
                ui.set_history_detail_step_plan_actual((if ui.get_is_ua() { "\u{041D}/\u{0414}" } else { "N/A" }).into());
            } else {
                ui.set_history_detail_title((if ui.get_is_ua() { "\u{0412}\u{0438}\u{043F}\u{0430}\u{043B}" } else { "Firing" }).into());
                ui.set_history_detail_status("--".into());
                ui.set_history_detail_duration("--".into());
                ui.set_history_detail_steps("--".into());
                ui.set_history_detail_energy(if ui.get_is_ua() { "-- \u{043A}\u{0412}\u{0442}\u{00B7}\u{0433}\u{043E}\u{0434}" } else { "-- kWh" }.into());
                ui.set_history_detail_step_durations(if ui.get_is_ua() { "-- \u{0445}\u{0432}/\u{043A}\u{0440}\u{043E}\u{043A}" } else { "-- min/step" }.into());
                ui.set_history_detail_program_name("--".into());
                ui.set_history_detail_start_time("--".into());
                ui.set_history_detail_end_time("--".into());
                ui.set_history_detail_status_verbose("--".into());
                ui.set_history_detail_energy_cost("--".into());
                ui.set_history_detail_max_load("--".into());
                ui.set_history_detail_peak_temp("--".into());
                ui.set_history_detail_voltage_stability("--".into());
                ui.set_history_detail_step_plan_actual("--".into());
            }
            ui.set_view(View::HistoryDetail);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let fault_log_cache = fault_log_cache.clone();
        ui.on_toggle_language(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let is_ua = !ui.get_is_ua();
            ui.set_is_ua(is_ua);
            unsafe { slint_bridge_set_language_is_ua(is_ua) };
            if ui.get_settings_page().to_string() == "fault_logs" {
                let items = load_fault_log_items(is_ua);
                *fault_log_cache.borrow_mut() = items.clone();
                ui.set_fault_log_items(ModelRc::new(VecModel::from(items)));
                ui.set_fault_log_detail_visible(false);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_set_temp_unit(move |unit| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let new_unit = if unit.to_string() == "F" { TempUnit::F } else { TempUnit::C };
            let mut state = app_state.borrow_mut();
            apply_temp_unit_change(&ui, &mut state, new_unit);
            unsafe { slint_bridge_set_temp_unit(if new_unit == TempUnit::F { 1 } else { 0 }) };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_set_time_format(move |fmt| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let new_fmt = if fmt.to_string() == "12" { TimeFormat::H12 } else { TimeFormat::H24 };
            ui.set_time_format(match new_fmt { TimeFormat::H24 => "24", TimeFormat::H12 => "12" }.into());
            app_state.borrow_mut().time_format = new_fmt;
            unsafe { slint_bridge_set_time_format(if new_fmt == TimeFormat::H12 { 1 } else { 0 }) };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_set_date_format(move |fmt| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let new_fmt = match fmt.to_string().as_str() {
                "MDY" => DateFormat::Mdy,
                "YMD" => DateFormat::Ymd,
                _ => DateFormat::Dmy,
            };
            ui.set_date_format(match new_fmt { DateFormat::Dmy => "DMY", DateFormat::Mdy => "MDY", DateFormat::Ymd => "YMD" }.into());
            app_state.borrow_mut().date_format = new_fmt;
            unsafe {
                let v = match new_fmt { DateFormat::Dmy => 0, DateFormat::Mdy => 1, DateFormat::Ymd => 2 };
                slint_bridge_set_date_format(v);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_display_brightness_changed(move |value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut v = value;
            if v < 50 { v = 50; }
            if v > 100 { v = 100; }
            ui.set_display_brightness(v);
            ui.set_ui_dim(((100 - v) as f32) / 100.0);
            unsafe { slint_bridge_set_display_brightness(v as u8) };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let calib_state = calib_state.clone();
        ui.on_start_touch_calibration(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut calib = calib_state.borrow_mut();
            *calib = TouchCalibration {
                active: true,
                step: 0,
                points: CalibPoints::default(),
            };
            update_calib_ui(&ui, &calib);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let calib_state = calib_state.clone();
        ui.on_cancel_touch_calibration(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut calib = calib_state.borrow_mut();
            calib.active = false;
            ui.set_view(View::Dashboard);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_fan_manual_changed(move |enabled| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_fan_manual(enabled);
            unsafe { slint_bridge_set_fan_manual(enabled) };
            unsafe { slint_bridge_set_fan_power(ui.get_fan_power()) };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_fan_power_changed(move |percent| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_fan_power(percent);
            unsafe { slint_bridge_set_fan_power(percent) };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_autotune_start(move |target| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let unit = app_state.borrow().temp_unit;
            let min_disp = temp_to_display_i32(100, unit);
            let max_disp = temp_to_display_i32(1200, unit);
            let clamped = target.clamp(min_disp, max_disp);
            ui.set_autotune_target_c(clamped);
            unsafe {
                let clamped_c = temp_to_c_from_display(clamped, unit);
                let _ = slint_bridge_set_autotune_target_c(clamped_c as f32);
            }
            unsafe {
                let clamped_c = temp_to_c_from_display(clamped, unit);
                let _ = slint_bridge_start_autotune(clamped_c as f32);
            }
        });
    }

    {
        ui.on_autotune_stop(move || {
            unsafe { slint_bridge_stop_autotune() };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let clear_fault_cooldown_at = clear_fault_cooldown_at.clone();
        let clear_fault_inflight = clear_fault_inflight.clone();
        ui.on_clear_fault(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            {
                let mut inflight = clear_fault_inflight.borrow_mut();
                if *inflight {
                    return;
                }
                let mut last = clear_fault_cooldown_at.borrow_mut();
                if last.elapsed() < Duration::from_millis(700) {
                    return;
                }
                *inflight = true;
                *last = Instant::now();
            }
            let ok = unsafe { slint_bridge_clear_fault() };
            *clear_fault_inflight.borrow_mut() = false;
            if ok {
                ui.set_fault_popup_hidden(false);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let wifi_scan_in_progress = wifi_scan_in_progress.clone();
        ui.on_wifi_open_picker(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_wifi_picker_visible(true);
            ui.set_wifi_scan_busy(true);
            ui.set_wifi_selected_ssid("".into());
            ui.set_wifi_networks(ModelRc::new(VecModel::from(Vec::<WifiNetworkRow>::new())));
            *wifi_scan_in_progress.borrow_mut() = true;
            unsafe { slint_bridge_wifi_scan_start() };
        });
    }

    {
        ui.on_wifi_disconnect(move || {
            unsafe { slint_bridge_wifi_disconnect() };
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_wifi_qr_toggle(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_wifi_qr_visible(!ui.get_wifi_qr_visible());
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        let start_cooldown_at = start_cooldown_at.clone();
        let render_suppress_until = render_suppress_until.clone();
        let command_feedback_until = command_feedback_until.clone();
        let render_reason = render_reason.clone();
        ui.on_start_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            *render_reason.borrow_mut() = (1u8, Instant::now());
            if ui.get_fault_active() {
                let msg = if ui.get_is_ua() {
                    "Активна помилка. Спочатку скиньте помилку".to_string()
                } else {
                    "Active fault. Clear fault first".to_string()
                };
                if !ui.get_command_feedback_visible() || ui.get_command_feedback().to_string() != msg {
                    ui.set_command_feedback(msg.into());
                    ui.set_command_feedback_ok(false);
                    ui.set_command_feedback_visible(true);
                    if let Ok(mut until) = command_feedback_until.try_borrow_mut() {
                        *until = Instant::now() + Duration::from_millis(2600);
                    }
                }
                return;
            }
            if let Ok(mut last) = start_cooldown_at.try_borrow_mut() {
                if last.elapsed() < Duration::from_millis(700) {
                    return;
                }
                *last = Instant::now();
            }
            let schedule_json: Option<String> = {
                if let Ok(state) = app_state.try_borrow() {
                    state
                        .selected_index
                        .and_then(|idx| state.schedules.get(idx))
                        .and_then(|schedule| serde_json::to_string(schedule).ok())
                } else {
                    None
                }
            };

            if let Some(json) = schedule_json {
                if let Ok(c_json) = CString::new(json) {
                    let started = unsafe { slint_bridge_start_schedule_json(c_json.as_ptr()) };
                    if started {
                        if let Ok(mut until) = render_suppress_until.try_borrow_mut() {
                            *until = Instant::now();
                        }
                    }
                    if !started {
                        ui.set_command_feedback(
                            if ui.get_is_ua() {
                                "\u{041D}\u{0435} \u{0432}\u{0434}\u{0430}\u{043B}\u{043E}\u{0441}\u{044F} \u{043F}\u{043E}\u{0447}\u{0430}\u{0442}\u{0438} \u{0432}\u{0438}\u{043F}\u{0430}\u{043B}. \u{041F}\u{0435}\u{0440}\u{0435}\u{0432}\u{0456}\u{0440} \u{0441}\u{0442}\u{0430}\u{043D} \u{0442}\u{0435}\u{0440}\u{043C}\u{043E}\u{043F}\u{0430}\u{0440}\u{0438}/\u{043F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0438}".into()
                            } else {
                                "Start failed. Check thermocouple/fault state".into()
                            }
                        );
                        ui.set_command_feedback_ok(false);
                        ui.set_command_feedback_visible(true);
                        if let Ok(mut until) = command_feedback_until.try_borrow_mut() {
                            *until = Instant::now() + Duration::from_millis(4200);
                        }
                    }
                }
            } else {
                ui.set_command_feedback(
                    if ui.get_is_ua() {
                        "\u{041D}\u{0435} \u{0432}\u{0438}\u{0431}\u{0440}\u{0430}\u{043D}\u{043E} \u{043F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0443} \u{0430}\u{0431}\u{043E} \u{043A}\u{0440}\u{043E}\u{043A}\u{0438}".into()
                    } else {
                        "No program/steps selected".into()
                    }
                );
                ui.set_command_feedback_ok(false);
                ui.set_command_feedback_visible(true);
                if let Ok(mut until) = command_feedback_until.try_borrow_mut() {
                    *until = Instant::now() + Duration::from_millis(3200);
                }
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let stop_cooldown_at = stop_cooldown_at.clone();
        let render_reason = render_reason.clone();
        ui.on_stop_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            *render_reason.borrow_mut() = (2u8, Instant::now());
            {
                let last = stop_cooldown_at.borrow();
                if last.elapsed() < Duration::from_millis(500) {
                    return;
                }
            }
            ui.set_show_stop_confirm(true);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_stop_cancel(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_stop_confirm(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_skip_step(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_skip_confirm(true);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_skip_cancel(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_skip_confirm(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_skip_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            unsafe { slint_bridge_skip_step(); }
            ui.set_show_skip_confirm(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let stop_cooldown_at = stop_cooldown_at.clone();
        let render_reason = render_reason.clone();
        ui.on_stop_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            *render_reason.borrow_mut() = (3u8, Instant::now());
            {
                let mut last = stop_cooldown_at.borrow_mut();
                if last.elapsed() < Duration::from_millis(500) {
                    return;
                }
                *last = Instant::now();
            }
            unsafe { slint_bridge_stop() };
            ui.set_show_stop_confirm(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_running_edit_back(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Dashboard);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_running_edit_apply(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_running_edit_confirm(true);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_running_edit_cancel(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_running_edit_confirm(false);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_running_edit_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let unit = app_state.borrow().temp_unit;
            let d_temp_disp = ui.get_running_edit_add_temp();
            let d_time = ui.get_running_edit_add_time() as f32;
            let rate_disp = ui.get_running_edit_rate();
            if rate_disp > 0 {
                let rate_c = rate_to_c_from_display(rate_disp, unit) as f32;
                let _ = unsafe { slint_bridge_set_rate(rate_c) };
            }
            if d_temp_disp != 0 {
                let d_temp_c = temp_to_c_from_display(d_temp_disp, unit) as f32;
                unsafe { slint_bridge_add_temp(d_temp_c); }
            }
            if d_time != 0.0 {
                unsafe { slint_bridge_add_time(d_time); }
            }
            ui.set_show_running_edit_confirm(false);
            ui.set_running_edit_add_temp(0);
            ui.set_running_edit_add_time(0);
            ui.set_view(View::Dashboard);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_select_schedule(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let idx = index as usize;
            if idx < state.schedules.len() {
                state.selected_index = Some(idx);
                state.history_graph_id = None;
                let schedule = state.schedules[idx].clone();
                ui.set_selected_schedule_name(schedule.name.clone().into());
                ui.set_selected_steps_count(schedule_steps_count(&schedule));
                if let Ok(json) = serde_json::to_string(&schedule) {
                    if let Ok(c_json) = CString::new(json) {
                        unsafe {
                            let _ = slint_bridge_load_schedule_json(c_json.as_ptr());
                        }
                    }
                }
                refresh_schedule_model(&ui, &state);
                ui.set_view(View::Dashboard);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_edit_schedule(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let idx = index as usize;
            if idx < state.schedules.len() {
                state.editing_index = Some(idx);
                state.graph_index = None;
                state.history_graph_id = None;
                let schedule = state.schedules[idx].clone();
                state.editor_steps = to_editor_steps(&schedule, state.temp_unit);
                ui.set_selected_schedule_name(schedule.name.clone().into());
                refresh_editor_model(&ui, &state);
                ui.set_view(View::Editor);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_delete_schedule(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let idx = index as usize;
            if idx >= state.schedules.len() {
                return;
            }

            let removed_selected = state.selected_index == Some(idx);
            state.schedules.remove(idx);

            state.selected_index = match state.selected_index {
                Some(sel) if sel == idx => {
                    if state.schedules.is_empty() {
                        None
                    } else {
                        Some(idx.min(state.schedules.len() - 1))
                    }
                }
                Some(sel) if sel > idx => Some(sel - 1),
                other => other,
            };

            state.editing_index = match state.editing_index {
                Some(edit) if edit == idx => None,
                Some(edit) if edit > idx => Some(edit - 1),
                other => other,
            };

            state.graph_index = match state.graph_index {
                Some(graph) if graph == idx => None,
                Some(graph) if graph > idx => Some(graph - 1),
                other => other,
            };

            if let Some(sel) = state.selected_index {
                if let Some(schedule) = state.schedules.get(sel) {
                    ui.set_selected_schedule_name(schedule.name.clone().into());
                    ui.set_selected_steps_count(schedule_steps_count(schedule));
                    if removed_selected {
                        if let Ok(json) = serde_json::to_string(schedule) {
                            if let Ok(c_json) = CString::new(json) {
                                unsafe {
                                    let _ = slint_bridge_load_schedule_json(c_json.as_ptr());
                                }
                            }
                        }
                    }
                }
            } else {
                ui.set_selected_schedule_name("".into());
                ui.set_selected_steps_count(0);
            }

            let _ = save_schedules(&state.schedules);
            unsafe { slint_bridge_notify_schedules_changed() };
            refresh_schedule_model(&ui, &state);
        });
    }
    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_new_schedule(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let base_name = if ui.get_is_ua() { "\u{041D}\u{043E}\u{0432}\u{0430}_\u{043F}\u{0440}\u{043E}\u{0433}\u{0440}\u{0430}\u{043C}\u{0430}" } else { "New_Program" };
            let name = make_unique_schedule_name(&state.schedules, base_name);
            let id = make_safe_id(&name);
            state.graph_index = None;
            state.history_graph_id = None;
            state.schedules.push(ScheduleFile {
                id,
                name: name.clone(),
                schedule_type: "Custom".to_string(),
                created: String::new(),
                steps_count: Some(0),
                glass_details: None,
                steps: Vec::new(),
                extra: serde_json::Map::new(),
            });
            let idx = state.schedules.len() - 1;
            state.editing_index = Some(idx);
            state.editor_steps = Vec::new();
            ui.set_selected_schedule_name(name.into());
            refresh_schedule_model(&ui, &state);
            refresh_editor_model(&ui, &state);
            ui.set_view(View::Editor);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_editor_back(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_kb_visible(false);
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_save(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_kb_visible(false);
            let mut state = app_state.borrow_mut();
            if let Some(idx) = state.editing_index {
                let was_firing = !ui.get_is_idle();
                let current_step = ui.get_step_index().max(0) as usize;
                let selected_for_run = state.selected_index;
                let old_steps = state.schedules.get(idx).map(|s| s.steps.clone()).unwrap_or_default();
                let name = normalize_schedule_name(&ui.get_selected_schedule_name().to_string());
                let steps = to_schedule_steps(&state.schedules[idx].steps, &state.editor_steps, state.temp_unit);
                if let Some(schedule) = state.schedules.get_mut(idx) {
                    schedule.name = name.clone();
                    schedule.steps = steps;
                    schedule.steps_count = Some(schedule.steps.len() as i32);
                    schedule.id = make_safe_id(&schedule.name);
                    if schedule.schedule_type.is_empty() {
                        schedule.schedule_type = "Custom".to_string();
                    }
                }
                ui.set_selected_schedule_name(name.into());
                let _ = save_schedules(&state.schedules);
                if was_firing && selected_for_run == Some(idx) {
                    if let Some(updated) = state.schedules.get(idx) {
                        apply_runtime_step_delta(&old_steps, &updated.steps, current_step);
                    }
                }
                unsafe { slint_bridge_notify_schedules_changed() };
                refresh_schedule_model(&ui, &state);
                ui.set_view(if was_firing && selected_for_run == Some(idx) {
                    View::Dashboard
                } else {
                    View::Schedules
                });
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_add_step(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let target = state.editor_steps.last().map(|s| s.target).unwrap_or(100);
            let rate_default = rate_to_display(100, state.temp_unit);
            state.editor_steps.push(EditorStepRow {
                step_type: "ramp".into(),
                rate: rate_default,
                target,
                hold: 0,
            });
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_rate_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let unit = state.temp_unit;
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                let max_disp = rate_to_display(9999, unit);
                let v = parse_int_with_clamp(value.as_str(), 0, max_disp, step.rate);
                step.rate = v;
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_target_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                let max_disp = ui.get_schedule_target_max_c().clamp(100, 3000);
                let v = parse_int_with_clamp(value.as_str(), 0, max_disp, step.target);
                step.target = v;
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_hold_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                let v = parse_int_with_clamp(value.as_str(), 0, 999, step.hold);
                step.hold = v;
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_kb_open(move |field, step_index, value| {
            let ui_weak = ui_weak.clone();
            let field = field.to_string();
            let value = value.to_string();
            // Defer keyboard open until the tap/release sequence is fully finished.
            // This avoids re-entrant UI updates on MCU touch input that can trigger a reboot.
            Timer::single_shot(Duration::from_millis(60), move || {
                let Some(ui) = ui_weak.upgrade() else { return; };
                if ui.get_kb_visible()
                    && ui.get_kb_step_index() == step_index
                    && ui.get_kb_field().to_string() == field
                {
                    return;
                }
                ui.set_kb_step_index(step_index);
                ui.set_kb_field(field.clone().into());
                let sanitized = if field == "clock_time" || field == "clock_date" {
                    value.chars().filter(|c| c.is_ascii_digit()).collect::<String>()
                } else {
                    value.clone()
                };
                ui.set_kb_value(sanitized.into());
                if field == "name" || field == "wifi_pass" {
                    ui.set_kb_mode("alpha".into());
                    ui.set_kb_caps(false);
                } else {
                    ui.set_kb_mode("num".into());
                }
                ui.set_kb_visible(true);
            });
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        let kb_key_last = kb_key_last.clone();
        let wifi_connect_pending_deadline = wifi_connect_pending_deadline.clone();
        let wifi_connect_target_ssid = wifi_connect_target_ssid.clone();
        let command_feedback_until = command_feedback_until.clone();
        ui.on_kb_key(move |key| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let key = key.to_string();
            {
                let mut last = kb_key_last.borrow_mut();
                if last.0 == key && last.1.elapsed() < Duration::from_millis(70) {
                    return;
                }
                *last = (key.clone(), Instant::now());
            }

            let field = ui.get_kb_field().to_string();
            if field == "name" || field == "wifi_pass" {
                let text_max_chars = if field == "name" { 24usize } else { 64usize };
                match key.as_str() {
                    "CANCEL" => {
                        ui.set_kb_visible(false);
                    }
                    "CLR" => {
                        ui.set_kb_value("".into());
                    }
                    "BS" => {
                        let mut v = ui.get_kb_value().to_string();
                        v.pop();
                        ui.set_kb_value(v.into());
                    }
                    "SPACE" => {
                        let mut v = ui.get_kb_value().to_string();
                        if v.chars().count() < text_max_chars {
                            v.push(' ');
                            ui.set_kb_value(v.into());
                        }
                    }
                    "OK" => {
                        let candidate = ui.get_kb_value().to_string();
                        if field == "name" {
                            let normalized = normalize_schedule_name(&candidate);
                            if !normalized.is_empty() {
                                ui.set_selected_schedule_name(normalized.into());
                            }
                        } else if field == "wifi_pass" {
                            ui.set_wifi_password(candidate.clone().into());
                            let ssid = ui.get_wifi_selected_ssid().to_string();
                            let ssid_trimmed = ssid.trim();
                            if !ssid_trimmed.is_empty() {
                                unsafe { slint_bridge_wifi_disconnect(); }
                                if let (Ok(c_ssid), Ok(c_pass)) = (CString::new(ssid_trimmed), CString::new(candidate)) {
                                    let started = unsafe { slint_bridge_wifi_connect(c_ssid.as_ptr(), c_pass.as_ptr()) };
                                    if started {
                                        *wifi_connect_target_ssid.borrow_mut() = ssid_trimmed.to_string();
                                        *wifi_connect_pending_deadline.borrow_mut() = Some(Instant::now() + Duration::from_secs(18));
                                        ui.set_command_feedback(
                                            if ui.get_is_ua() {
                                                "\u{041F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0435}\u{043D}\u{043D}\u{044F} \u{0434}\u{043E} Wi-Fi...".into()
                                            } else {
                                                "Connecting to Wi-Fi...".into()
                                            }
                                        );
                                        ui.set_command_feedback_ok(true);
                                        ui.set_command_feedback_visible(true);
                                        *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(3000);
                                    } else {
                                        *wifi_connect_pending_deadline.borrow_mut() = None;
                                        ui.set_command_feedback(
                                            if ui.get_is_ua() {
                                                "\u{041D}\u{0435} \u{0432}\u{0434}\u{0430}\u{043B}\u{043E}\u{0441}\u{044F} \u{0437}\u{0430}\u{043F}\u{0443}\u{0441}\u{0442}\u{0438}\u{0442}\u{0438} \u{043F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0435}\u{043D}\u{043D}\u{044F}".into()
                                            } else {
                                                "Failed to start Wi-Fi connection".into()
                                            }
                                        );
                                        ui.set_command_feedback_ok(false);
                                        ui.set_command_feedback_visible(true);
                                        *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(4200);
                                    }
                                } else {
                                    *wifi_connect_pending_deadline.borrow_mut() = None;
                                    ui.set_command_feedback(
                                        if ui.get_is_ua() {
                                            "\u{041D}\u{0435}\u{043A}\u{043E}\u{0440}\u{0435}\u{043A}\u{0442}\u{043D}\u{0456} \u{0434}\u{0430}\u{043D}\u{0456} Wi-Fi".into()
                                        } else {
                                            "Invalid Wi-Fi credentials data".into()
                                        }
                                    );
                                    ui.set_command_feedback_ok(false);
                                    ui.set_command_feedback_visible(true);
                                    *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(4200);
                                }
                            } else {
                                ui.set_command_feedback(
                                    if ui.get_is_ua() {
                                        "\u{0421}\u{043F}\u{043E}\u{0447}\u{0430}\u{0442}\u{043A}\u{0443} \u{043E}\u{0431}\u{0435}\u{0440}\u{0456}\u{0442}\u{044C} Wi-Fi \u{043C}\u{0435}\u{0440}\u{0435}\u{0436}\u{0443}".into()
                                    } else {
                                        "Select Wi-Fi network first".into()
                                    }
                                );
                                ui.set_command_feedback_ok(false);
                                ui.set_command_feedback_visible(true);
                                *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(3000);
                            }
                        }
                        ui.set_kb_visible(false);
                    }
                    other => {
                        if other.chars().count() == 1 {
                            let mut v = ui.get_kb_value().to_string();
                            if v.chars().count() < text_max_chars {
                                v.push_str(other);
                                ui.set_kb_value(v.into());
                            }
                        }
                    }
                }
                return;
            }

            let unit = app_state.borrow().temp_unit;
            let schedule_target_max = ui.get_schedule_target_max_c().clamp(100, 3000);
            let autotune_min = temp_to_display_i32(100, unit);
            let autotune_max = temp_to_display_i32(1200, unit);
            let max_temp_min = temp_to_display_i32(100, unit);
            let max_temp_max = temp_to_display_i32(1300, unit);
            let offset_min = delta_temp_to_display_i32(-100.0, unit);
            let offset_max = delta_temp_to_display_i32(100.0, unit);
            let run_temp_min = temp_to_display_i32(-300, unit);
            let run_temp_max = temp_to_display_i32(300, unit);
            let run_rate_min = rate_to_display(1, unit);
            let run_rate_max = rate_to_display(9999, unit);
            let rate_len = if unit == TempUnit::F { 5usize } else { 4usize };
            let (min, max, max_len) = match field.as_str() {
                "rate" => (0, rate_to_display(9999, unit), rate_len),
                "target" => (0, schedule_target_max, 4usize),
                "hold" => (0, 999, 3usize),
                "fan" => (0, 100, 3usize),
                "autotune" => (autotune_min, autotune_max, 4usize),
                "max_temp" => (max_temp_min, max_temp_max, 4usize),
                "thermocouple_offset" => (offset_min, offset_max, 4usize),
                "kiln_wattage" => (100, 50000, 5usize),
                "run_rate" => (run_rate_min, run_rate_max, rate_len),
                "run_temp" => (run_temp_min, run_temp_max, 4usize),
                "run_time" => (-300, 300, 4usize),
                "clock_time" => (0, 2359, 4usize),
                "clock_date" => (0, 99999999, 8usize),
                _ => (0, 9999, 5usize),
            };

            match key.as_str() {
                "CANCEL" => {
                    ui.set_kb_visible(false);
                }
                "CLR" => {
                    ui.set_kb_value("".into());
                }
                "BS" => {
                    let mut v = ui.get_kb_value().to_string();
                    v.pop();
                    ui.set_kb_value(v.into());
                }
                "OK" => {
                    let idx = ui.get_kb_step_index();
                    let value = ui.get_kb_value().to_string();
                    if idx >= 0 {
                        let mut state = app_state.borrow_mut();
                        if let Some(step) = state.editor_steps.get_mut(idx as usize) {
                            let fallback = match field.as_str() {
                                "rate" => step.rate,
                                "target" => step.target,
                                "hold" => step.hold,
                                _ => step.rate,
                            };
                            let parsed = parse_int_with_clamp(&value, min, max, fallback);
                            match field.as_str() {
                                "rate" => step.rate = parsed,
                                "target" => step.target = parsed,
                                "hold" => step.hold = parsed,
                                _ => {}
                            }
                        }
                        refresh_editor_model(&ui, &state);
                    } else if field == "clock_time" {
                        let digits: String = value.chars().filter(|c| c.is_ascii_digit()).collect();
                        if digits.len() == 4 {
                            let hour = digits.get(0..2).and_then(|v| v.parse::<u8>().ok()).unwrap_or(255);
                            let minute = digits.get(2..4).and_then(|v| v.parse::<u8>().ok()).unwrap_or(255);
                            if hour < 24 && minute < 60 {
                                let ok = unsafe { slint_bridge_set_time_hm(hour, minute) };
                                if ok {
                                    let mut buf = vec![0u8; 16];
                                    let _ = unsafe { slint_bridge_get_time_str(buf.as_mut_ptr() as *mut c_char, buf.len() as i32) };
                                    let label = c_buf_to_string(&buf);
                                    if !label.is_empty() {
                                        ui.set_current_time(label.into());
                                    }
                                }
                            }
                        }
                    } else if field == "clock_date" {
                        let digits: String = value.chars().filter(|c| c.is_ascii_digit()).collect();
                        if digits.len() == 8 {
                            let fmt = ui.get_date_format().to_string();
                            let (year_s, month_s, day_s) = if fmt == "YMD" {
                                (digits.get(0..4), digits.get(4..6), digits.get(6..8))
                            } else if fmt == "MDY" {
                                (digits.get(4..8), digits.get(0..2), digits.get(2..4))
                            } else {
                                (digits.get(4..8), digits.get(2..4), digits.get(0..2))
                            };
                            let year = year_s.and_then(|v| v.parse::<u16>().ok()).unwrap_or(0);
                            let month = month_s.and_then(|v| v.parse::<u8>().ok()).unwrap_or(0);
                            let day = day_s.and_then(|v| v.parse::<u8>().ok()).unwrap_or(0);
                            if year >= 2020 && year <= 2099 && (1..=12).contains(&month) && (1..=31).contains(&day) {
                                let ok = unsafe { slint_bridge_set_date_ymd(year, month, day) };
                                if ok {
                                    let mut buf = vec![0u8; 16];
                                    let _ = unsafe { slint_bridge_get_date_str(buf.as_mut_ptr() as *mut c_char, buf.len() as i32) };
                                    let label = c_buf_to_string(&buf);
                                    if !label.is_empty() {
                                        ui.set_current_date(label.into());
                                    }
                                }
                            }
                        }
                    } else if field == "fan" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_fan_power());
                        ui.set_fan_power(parsed);
                        unsafe { slint_bridge_set_fan_power(parsed) };
                    } else if field == "autotune" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_autotune_target_c());
                        ui.set_autotune_target_c(parsed);
                        unsafe {
                            let parsed_c = temp_to_c_from_display(parsed, unit);
                            let _ = slint_bridge_set_autotune_target_c(parsed_c as f32);
                        }
                    } else if field == "max_temp" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_schedule_target_max_c());
                        ui.set_schedule_target_max_c(parsed);
                        unsafe {
                            let parsed_c = temp_to_c_from_display(parsed, unit);
                            let _ = slint_bridge_set_user_max_temp_c(parsed_c as f32);
                        }
                    } else if field == "thermocouple_offset" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_thermocouple_offset());
                        ui.set_thermocouple_offset(parsed);
                        unsafe {
                            let parsed_c = delta_temp_to_c_from_display(parsed, unit);
                            let _ = slint_bridge_set_temperature_offset_c(parsed_c);
                        }
                    } else if field == "kiln_wattage" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_kiln_wattage());
                        ui.set_kiln_wattage(parsed);
                        unsafe {
                            let _ = slint_bridge_set_kiln_wattage(parsed);
                        }
                    } else if field == "run_rate" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_running_edit_rate());
                        ui.set_running_edit_rate(parsed);
                    } else if field == "run_temp" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_running_edit_add_temp());
                        ui.set_running_edit_add_temp(parsed);
                    } else if field == "run_time" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_running_edit_add_time());
                        ui.set_running_edit_add_time(parsed);
                    }
                    ui.set_kb_visible(false);
                }
                digit if digit.len() == 1 && (digit.chars().all(|c| c.is_ascii_digit()) || digit == "-") => {
                    let mut v = ui.get_kb_value().to_string();
                    if digit == "-" {
                        if !matches!(field.as_str(), "run_temp" | "run_time" | "thermocouple_offset") {
                            return;
                        }
                        if v.starts_with('-') {
                            v = v.trim_start_matches('-').to_string();
                        } else if v.is_empty() || v == "0" {
                            v = "-".to_string();
                        } else {
                            v = format!("-{}", v);
                        }
                        ui.set_kb_value(v.into());
                        return;
                    }
                    if v == "0" {
                        v.clear();
                    }
                    if v == "-" {
                        if v.len() < max_len {
                            v.push_str(digit);
                            ui.set_kb_value(v.into());
                        }
                        return;
                    }
                    if v.len() < max_len {
                        v.push_str(digit);
                        ui.set_kb_value(v.into());
                    }
                }
                _ => {}
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_remove(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if (index as usize) < state.editor_steps.len() {
                state.editor_steps.remove(index as usize);
            }
            refresh_editor_model(&ui, &state);
        });
    }

    let line_pixels = usize::max(physical_w as usize, 1);
    let frame_pixels = usize::max((physical_w as usize).saturating_mul(physical_h as usize), 1);
    let mut line_buffer = vec![Rgb565Pixel::default(); line_pixels];
    let mut frame_buffer = vec![Rgb565Pixel::default(); frame_pixels];
    // Use full-frame buffering to avoid visible top-to-bottom redraw on state transitions.
    let use_full_frame_flush = true;
    // NewP4: avoid direct DPI framebuffer submit path; use stable draw_bitmap pipeline.
    let allow_p4_direct_framebuffer = !is_new_p4_display;
    let force_p4_no_blit_fallback = false;
    let mut last_touch = false;
    let mut debug_pressed = false;
    let mut debug_x_i32: i32 = 0;
    let mut debug_y_i32: i32 = 0;
    let mut debug_last_update = Instant::now();
    let mut last_x: u16 = 0;
    let mut last_y: u16 = 0;
    let mut filtered_x: i32 = 0;
    let mut filtered_y: i32 = 0;
    let mut last_sent_x: i32 = 0;
    let mut last_sent_y: i32 = 0;
    let mut locked_scroll_x: i32 = 0;
    let mut have_last_sent = false;
    let mut last_state_update = Instant::now();
    let mut sta_got_ip_guard_until = Instant::now();
    let mut wifi_transition_guard_until = Instant::now();
    let mut heavy_ui_guard_until = Instant::now();
    let mut last_sta_got_ip_ms: u64 = 0;
    let mut last_touch_dbg_x: i32 = i32::MIN;
    let mut last_touch_dbg_y: i32 = i32::MIN;
    let mut last_touch_dbg_pressed: bool = false;
    let mut last_wifi_ok_value: Option<bool> = None;
    let mut last_ui_heartbeat = Instant::now();
    let mut last_clock_update = Instant::now();
    let mut last_controller_info_update = Instant::now() - Duration::from_secs(10);
    let mut last_settings_sync = Instant::now() - Duration::from_secs(10);
    let mut last_polled_state = SlintKilnState::default();
    let mut has_last_polled_state = false;
    let mut last_schedules_revision = unsafe { slint_bridge_get_schedules_revision() };
    let mut last_command_result_revision = unsafe { slint_bridge_get_command_result_revision() };
    let mut last_qr_payload = String::new();
    let mut last_touch_activity = Instant::now();
    let target_fps = UI_TARGET_FPS;
    let frame_budget = if target_fps > 0 {
        Some(Duration::from_millis(1000 / target_fps))
    } else {
        None
    };
    let mut next_frame_at = Instant::now();

    loop {
        slint::platform::update_timers_and_animations();

        if last_ui_heartbeat.elapsed() >= Duration::from_millis(200) {
            last_ui_heartbeat = Instant::now();
            unsafe { slint_bridge_ui_heartbeat() };
        }

        if last_clock_update.elapsed() >= Duration::from_millis(1000) {
            last_clock_update = Instant::now();
            let mut buf = vec![0u8; 16];
            let _ = unsafe { slint_bridge_get_time_str(buf.as_mut_ptr() as *mut c_char, buf.len() as i32) };
            let label = c_buf_to_string(&buf);
            if let Some(ui) = ui_weak.upgrade() {
                ui.set_current_time(label.into());
                let _ = unsafe { slint_bridge_get_date_str(buf.as_mut_ptr() as *mut c_char, buf.len() as i32) };
                let date_label = c_buf_to_string(&buf);
                if !date_label.is_empty() {
                    ui.set_current_date(date_label.into());
                }
            }
        }

        if let Some(ui) = ui_weak.upgrade() {
            let need_info_refresh = ui.get_settings_page() == "controller_info";
            let period = if need_info_refresh {
                Duration::from_millis(1000)
            } else {
                Duration::from_millis(5000)
            };
            if last_controller_info_update.elapsed() >= period {
                last_controller_info_update = Instant::now();
                let st = state_poll();
                update_controller_info_ui(&ui, &st, ui.get_is_ua());
            }
        }

        let command_revision = unsafe { slint_bridge_get_command_result_revision() };
        if command_revision != 0 && command_revision != last_command_result_revision {
            let mut cmd = SlintCommandResult::default();
            if unsafe { slint_bridge_copy_command_result(&mut cmd as *mut SlintCommandResult) } {
                let action = c_buf_to_string(&cmd.action);
                if action == "start" || action == "stop" {
                    heavy_ui_guard_until = Instant::now() + Duration::from_millis(220);
                    *render_reason.borrow_mut() = (if action == "start" { 5u8 } else { 6u8 }, Instant::now());
                }
                if let Some(ui) = ui_weak.upgrade() {
                    apply_command_result_to_ui(&ui, cmd, ui.get_is_ua());
                }
                *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(2600);
            }
            last_command_result_revision = command_revision;
        }
        if Instant::now() > *command_feedback_until.borrow() {
            if let Some(ui) = ui_weak.upgrade() {
                if ui.get_command_feedback_visible() {
                    ui.set_command_feedback_visible(false);
                }
            }
        }
        if let Some(ui) = ui_weak.upgrade() {
            let wifi_now_ok = unsafe { slint_bridge_wifi_is_connected() };
            if last_wifi_ok_value != Some(wifi_now_ok) {
                if ui.get_header_wifi_ok() != wifi_now_ok {
                    ui.set_header_wifi_ok(wifi_now_ok);
                }
                let wifi_ui_visible =
                    (ui.get_view() == View::Settings && ui.get_settings_page() == "wifi")
                    || ui.get_wifi_qr_visible();
                if wifi_ui_visible && ui.get_wifi_ok() != wifi_now_ok {
                    ui.set_wifi_ok(wifi_now_ok);
                }
                last_wifi_ok_value = Some(wifi_now_ok);
            }
            let pending_deadline = *wifi_connect_pending_deadline.borrow();
            if let Some(deadline) = pending_deadline {
                if wifi_now_ok {
                    let ssid = wifi_connect_target_ssid.borrow().clone();
                    ui.set_command_feedback(
                        if ui.get_is_ua() {
                            if ssid.is_empty() {
                                "\u{041F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0435}\u{043D}\u{043E} \u{0434}\u{043E} Wi-Fi".into()
                            } else {
                                format!("\u{041F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0435}\u{043D}\u{043E}: {}", ssid).into()
                            }
                        } else {
                            if ssid.is_empty() {
                                "Connected to Wi-Fi".into()
                            } else {
                                format!("Connected: {}", ssid).into()
                            }
                        }
                    );
                    ui.set_command_feedback_ok(true);
                    ui.set_command_feedback_visible(true);
                    ui.set_wifi_picker_visible(false);
                    *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(3200);
                    *wifi_connect_pending_deadline.borrow_mut() = None;
                } else if Instant::now() >= deadline {
                    ui.set_command_feedback(
                        if ui.get_is_ua() {
                            "\u{041D}\u{0435}\u{0432}\u{0456}\u{0440}\u{043D}\u{0438}\u{0439} \u{043F}\u{0430}\u{0440}\u{043E}\u{043B}\u{044C} \u{0430}\u{0431}\u{043E} \u{043D}\u{0435} \u{0432}\u{0434}\u{0430}\u{043B}\u{043E}\u{0441}\u{044F} \u{043F}\u{0456}\u{0434}\u{043A}\u{043B}\u{044E}\u{0447}\u{0438}\u{0442}\u{0438}\u{0441}\u{044F}".into()
                        } else {
                            "Wrong password or failed to connect".into()
                        }
                    );
                    ui.set_command_feedback_ok(false);
                    ui.set_command_feedback_visible(true);
                    *command_feedback_until.borrow_mut() = Instant::now() + Duration::from_millis(4200);
                    *wifi_connect_pending_deadline.borrow_mut() = None;
                }
            }
        }

        // Touch input
        let (calib_active, _calib_step) = {
            let calib = calib_state.borrow();
            (calib.active, calib.step)
        };

        let mut x: u16 = 0;
        let mut y: u16 = 0;
        let mut z: u16 = 0;
        let raw_pressed = unsafe {
            if calib_active || is_new_p4_display {
                display_driver_touch_probe_raw(&mut x, &mut y, &mut z)
            } else {
                display_driver_touch_probe(&mut x, &mut y, &mut z)
            }
        };
        if is_new_p4_display && raw_pressed {
            // NewP4 GT911 reports portrait coordinates (480x800). Convert to the UI landscape space (800x480).
            let px = x.min(479) as i32;
            let py = y.min(799) as i32;
            x = (799 - py).clamp(0, 799) as u16;
            y = px.clamp(0, 479) as u16;
        }
        let pressed = raw_pressed;

        if let Some(ui) = ui_weak.upgrade() {
            // Keep raw touch telemetry updates only for calibration view.
            // Updating these properties on every tap in normal UI causes extra full redraws.
            if ui.get_view() == View::Calibration {
                let tx = x as i32;
                let ty = y as i32;
                let tz = z as i32;
                if ui.get_touch_x() != tx { ui.set_touch_x(tx); }
                if ui.get_touch_y() != ty { ui.set_touch_y(ty); }
                if ui.get_touch_z() != tz { ui.set_touch_z(tz); }
                if ui.get_touch_pressed() != pressed { ui.set_touch_pressed(pressed); }
            }
            if !calib_active && ui.get_view() == View::Calibration {
                ui.set_view(View::Dashboard);
            }

            if pressed {
                last_touch_activity = Instant::now();
                if ui.get_standby_visible() && !last_touch {
                    // First tap only wakes the screen, without forwarding the touch to controls.
                    ui.set_standby_visible(false);
                    // Do not block the next tap on Start by the anti-bounce cooldown.
                    {
                        let mut last_start = start_cooldown_at.borrow_mut();
                        *last_start = Instant::now() - Duration::from_secs(5);
                    }
                    last_touch = true;
                    last_x = x;
                    last_y = y;
                    have_last_sent = false;
                    continue;
                }
            } else if !ui.get_standby_visible()
                && !ui.get_boot_splash_visible()
                && !ui.get_kb_visible()
                && !ui.get_show_stop_confirm()
                && !ui.get_show_skip_confirm()
                && !ui.get_show_running_edit_confirm()
                && ui.get_board_profile_id() != 1
                && last_touch_activity.elapsed() >= Duration::from_millis(STANDBY_TIMEOUT_MS)
            {
                ui.set_standby_visible(true);
            }
        }

        // Calibration: record the next point on every fresh tap.
        if calib_active && pressed && !last_touch {
            let mut calib = calib_state.borrow_mut();
            let idx = calib.step as usize;
            if idx < CALIB_STEPS {
                calib.points.pts[idx] = Some((x, y));
            }

            if idx + 1 < CALIB_STEPS {
                calib.step += 1;
                if let Some(ui) = ui_weak.upgrade() {
                    update_calib_ui(&ui, &calib);
                }
            } else {
                let result = compute_calibration(calib.points);
                let ok = if let Some((l, r, t, b)) = result {
                    let cal_ok = unsafe { display_driver_set_touch_calibration(true, l, r, t, b) };

                    let affine = compute_affine_after_box(calib.points, (l, r, t, b));
                    if let Some((a, b2, c, d, e, f)) = affine {
                        let _ = unsafe { display_driver_set_touch_affine(true, a, b2, c, d, e, f) };
                    } else {
                        let _ = unsafe { display_driver_set_touch_affine(false, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0) };
                    }

                    let grid_ok = if let Some((dx, dy)) = compute_grid_residuals(calib.points, (l, r, t, b), affine) {
                        unsafe { display_driver_set_touch_grid(true, dx.as_ptr(), dy.as_ptr()) }
                    } else {
                        unsafe { display_driver_set_touch_grid(false, std::ptr::null(), std::ptr::null()) }
                    };

                    cal_ok && grid_ok
                } else {
                    let _ = unsafe { display_driver_set_touch_affine(false, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0) };
                    let _ = unsafe { display_driver_set_touch_grid(false, std::ptr::null(), std::ptr::null()) };
                    false
                };

                calib.active = false;
                if let Some(ui) = ui_weak.upgrade() {
                    if ok {
                        ui.set_calib_hint(if ui.get_is_ua() { "\u{0413}\u{043E}\u{0442}\u{043E}\u{0432}\u{043E}" } else { "Done" }.into());
                    } else {
                        ui.set_calib_hint(if ui.get_is_ua() { "\u{041F}\u{043E}\u{043C}\u{0438}\u{043B}\u{043A}\u{0430} \u{043A}\u{0430}\u{043B}\u{0456}\u{0431}\u{0440}\u{0443}\u{0432}\u{0430}\u{043D}\u{043D}\u{044F}" } else { "Calibration failed" }.into());
                    }
                    ui.set_view(View::Dashboard);
                }
            }
        }

        let active_view = if let Some(ui) = ui_weak.upgrade() {
            ui.get_view()
        } else {
            View::Dashboard
        };

        if pressed {
            let xi = x as i32;
            let yi = y as i32;
            if !last_touch {
                filtered_x = xi;
                filtered_y = yi;
                locked_scroll_x = xi;
            } else if !calib_active {
                // Smooth touch jitter during hold/drag to avoid scroll shake on noisy ADC samples.
                let dx_raw = (xi - filtered_x).abs();
                let dy_raw = (yi - filtered_y).abs();
                if dx_raw >= TOUCH_FAST_MOVE_PX || dy_raw >= TOUCH_FAST_MOVE_PX {
                    // On faster swipes, avoid filter lag and keep scrolling responsive.
                    filtered_x = xi;
                    filtered_y = yi;
                } else {
                    filtered_x = (filtered_x * TOUCH_FILTER_NUM + xi) / TOUCH_FILTER_DEN;
                    filtered_y = (filtered_y * TOUCH_FILTER_NUM + yi) / TOUCH_FILTER_DEN;
                }
            } else {
                filtered_x = xi;
                filtered_y = yi;
            }

            let debug_min_z = if is_new_p4_display { 0 } else { TOUCH_DEBUG_MIN_Z };
            if z >= debug_min_z {
                let (dbg_x, dbg_y) = if have_last_sent {
                    (last_sent_x, last_sent_y)
                } else {
                    (filtered_x, filtered_y)
                };
                let now = Instant::now();
                let dx = (dbg_x - debug_x_i32).abs();
                let dy = (dbg_y - debug_y_i32).abs();
                if dx >= TOUCH_DEBUG_DEADZONE_PX
                    || dy >= TOUCH_DEBUG_DEADZONE_PX
                    || debug_last_update.elapsed() >= Duration::from_millis(TOUCH_DEBUG_UPDATE_MIN_MS)
                {
                    debug_x_i32 = dbg_x;
                    debug_y_i32 = dbg_y;
                    debug_last_update = now;
                }
            }
            debug_pressed = pressed && z >= debug_min_z;

            let mut send_x_i32 = filtered_x;
            if !calib_active && active_view == View::Settings {
                // Settings list should scroll only vertically.
                send_x_i32 = locked_scroll_x;
            }

            let send_x = send_x_i32 as u16;
            let send_y = filtered_y as u16;
            last_x = send_x;
            last_y = send_y;
            // Touch coordinates arrive in physical pixels; convert to Slint logical coordinates when scaling is used.
            let pos = slint::LogicalPosition::new((send_x as f32) / ui_scale, (send_y as f32) / ui_scale);
            if !last_touch {
                let _ = window.dispatch_event(WindowEvent::PointerPressed { position: pos, button: PointerEventButton::Left });
                let _ = window.dispatch_event(WindowEvent::PointerMoved { position: pos });
                last_sent_x = filtered_x;
                last_sent_y = filtered_y;
                have_last_sent = true;
            } else {
                let dx = (filtered_x - last_sent_x).abs();
                let dy = (filtered_y - last_sent_y).abs();
                if !have_last_sent || dx > TOUCH_HOLD_DEADZONE_PX || dy > TOUCH_HOLD_DEADZONE_PX {
                    let _ = window.dispatch_event(WindowEvent::PointerMoved { position: pos });
                    last_sent_x = filtered_x;
                    last_sent_y = filtered_y;
                    have_last_sent = true;
                }
            }
        } else if last_touch {
            let pos = slint::LogicalPosition::new((last_x as f32) / ui_scale, (last_y as f32) / ui_scale);
            let _ = window.dispatch_event(WindowEvent::PointerReleased { position: pos, button: PointerEventButton::Left });
            have_last_sent = false;
        }
        last_touch = pressed;
        if !pressed {
            debug_pressed = false;
        }

        if let Some(ui) = ui_weak.upgrade() {
            if ui.get_view() == View::Calibration {
                if debug_x_i32 != last_touch_dbg_x {
                    ui.set_touch_debug_x(debug_x_i32);
                    last_touch_dbg_x = debug_x_i32;
                }
                if debug_y_i32 != last_touch_dbg_y {
                    ui.set_touch_debug_y(debug_y_i32);
                    last_touch_dbg_y = debug_y_i32;
                }
                if debug_pressed != last_touch_dbg_pressed {
                    ui.set_touch_debug_pressed(debug_pressed);
                    last_touch_dbg_pressed = debug_pressed;
                }
            }
        }

        // Status update
        if last_state_update.elapsed() >= Duration::from_millis(UI_STATE_APPLY_PERIOD_MS) {
            last_state_update = Instant::now();
            let got_ip_ms = unsafe { slint_bridge_wifi_last_got_ip_ms() };
            if got_ip_ms != 0 && got_ip_ms != last_sta_got_ip_ms {
                last_sta_got_ip_ms = got_ip_ms;
                *render_reason.borrow_mut() = (4u8, Instant::now());
                sta_got_ip_guard_until = Instant::now() + Duration::from_millis(120);
                wifi_transition_guard_until = Instant::now() + Duration::from_millis(300);
                heavy_ui_guard_until = Instant::now() + Duration::from_millis(180);
                if let Ok(mut until) = render_suppress_until.try_borrow_mut() {
                    *until = Instant::now();
                }
            }
            let s = state_poll();
            let should_apply_state = !has_last_polled_state || state_changed_for_ui(last_polled_state, s);
            let in_wifi_transition_dashboard =
                Instant::now() < wifi_transition_guard_until && ui.get_view() == View::Dashboard;
            if in_wifi_transition_dashboard || Instant::now() < heavy_ui_guard_until {
                // During short STA_GOT_IP transition, keep dashboard stable and only let
                // Wi-Fi icon update via the lightweight path above.
                last_polled_state = s;
                has_last_polled_state = true;
            } else if should_apply_state && Instant::now() >= sta_got_ip_guard_until {
                {
                    let mut state = app_state.borrow_mut();
                    apply_state_to_ui(&ui, s, &mut state);
                }
                last_polled_state = s;
                has_last_polled_state = true;
            }
            let max_temp = unsafe { slint_bridge_get_user_max_temp_c() };
            if max_temp.is_finite() {
                let unit = app_state.borrow().temp_unit;
                let max_disp = temp_to_display(max_temp.clamp(100.0, 1300.0), unit).round() as i32;
                if ui.get_schedule_target_max_c() != max_disp {
                    ui.set_schedule_target_max_c(max_disp);
                }
            }
            let autotune_target_c = unsafe { slint_bridge_get_autotune_target_c() };
            if autotune_target_c.is_finite() {
                let unit = app_state.borrow().temp_unit;
                let autotune_disp = temp_to_display(autotune_target_c.clamp(100.0, 1200.0), unit).round() as i32;
                if ui.get_autotune_target_c() != autotune_disp {
                    ui.set_autotune_target_c(autotune_disp);
                }
            }
            if ui.get_settings_page() == "autotune" && last_settings_sync.elapsed() >= Duration::from_secs(3) {
                last_settings_sync = Instant::now();
                let unit = app_state.borrow().temp_unit;
                let offset_c = unsafe { slint_bridge_get_temperature_offset_c() };
                if offset_c.is_finite() {
                    ui.set_thermocouple_offset(delta_temp_to_display_i32(offset_c, unit));
                }
                let watts = unsafe { slint_bridge_get_kiln_wattage() };
                if watts > 0 {
                    ui.set_kiln_wattage(watts);
                }
                let max_temp = unsafe { slint_bridge_get_user_max_temp_c() };
                if max_temp.is_finite() {
                    let max_disp = temp_to_display(max_temp.clamp(100.0, 1300.0), unit).round() as i32;
                    if ui.get_schedule_target_max_c() != max_disp {
                        ui.set_schedule_target_max_c(max_disp);
                    }
                }
            }
            if (ui.get_view() == View::Settings && ui.get_settings_page() == "wifi") || ui.get_wifi_qr_visible() {
            let wifi_ok = ui.get_wifi_ok();
            let url = ui.get_wifi_server_url().to_string();
            let mut raw_ap_ssid = [0 as c_char; 64];
            let ap_ssid = if unsafe { slint_bridge_wifi_ap_ssid_copy(raw_ap_ssid.as_mut_ptr(), raw_ap_ssid.len() as i32) } {
                unsafe { CStr::from_ptr(raw_ap_ssid.as_ptr()) }
                    .to_string_lossy()
                    .trim()
                    .to_string()
            } else {
                CONTROLLER_AP_SSID_FALLBACK.to_string()
            };
            let ap_ssid = if ap_ssid.is_empty() {
                CONTROLLER_AP_SSID_FALLBACK.to_string()
            } else {
                ap_ssid
            };
            let ap_ssid = if ap_ssid.starts_with("TRAE_KILN_SETUP") {
                ap_ssid
            } else {
                CONTROLLER_AP_SSID_FALLBACK.to_string()
            };
            let qr_payload = wifi_qr_payload(wifi_ok, &url, &ap_ssid);
            if qr_payload != last_qr_payload {
                last_qr_payload = qr_payload.clone();
                ui.set_wifi_qr_image(build_qr_image(&qr_payload));
                if wifi_ok {
                    let hint = if ui.get_is_ua() {
                        format!("\u{0412}\u{0456}\u{0434}\u{043A}\u{0440}\u{0438}\u{0442}\u{0438} \u{0432}\u{0435}\u{0431}-\u{0456}\u{043D}\u{0442}\u{0435}\u{0440}\u{0444}\u{0435}\u{0439}\u{0441}: {}", url)
                    } else {
                        format!("Open web interface: {}", url)
                    };
                    ui.set_wifi_qr_hint(hint.into());
                } else {
                    let hint = if ui.get_is_ua() {
                        format!(
                            "1) Скануйте QR і підключіться до {}, 2) captive portal відкриє Wi-Fi manager (або відкрийте {})",
                            ap_ssid,
                            CONTROLLER_AP_PORTAL_URL
                        )
                    } else {
                        format!(
                            "1) Scan QR to join {}, 2) captive portal should open Wi-Fi manager (or open {})",
                            ap_ssid,
                            CONTROLLER_AP_PORTAL_URL
                        )
                    };
                    ui.set_wifi_qr_hint(hint.into());
                }
            }
            }

            let schedules_revision = unsafe { slint_bridge_get_schedules_revision() };
            if schedules_revision != last_schedules_revision {
                last_schedules_revision = schedules_revision;
                if let Some(ui) = ui_weak.upgrade() {
                    // Avoid clobbering unsaved edits while the editor is open.
                    if ui.get_view() != View::Editor {
                        let mut state = app_state.borrow_mut();
                        let previous_name = ui.get_selected_schedule_name().to_string();
                        let previous_id = state
                            .selected_index
                            .and_then(|idx| state.schedules.get(idx))
                            .map(|s| s.id.clone())
                            .unwrap_or_default();
                        state.schedules = load_schedules();
                        state.editing_index = None;
                        state.editor_steps.clear();

                        if state.schedules.is_empty() {
                            state.selected_index = None;
                            ui.set_selected_schedule_name("".into());
                            ui.set_selected_steps_count(0);
                        } else {
                            let selected_idx = state
                                .schedules
                                .iter()
                                .position(|s| !previous_id.is_empty() && s.id == previous_id)
                                .or_else(|| state.schedules.iter().position(|s| s.name == previous_name))
                                .unwrap_or(0);
                            state.selected_index = Some(selected_idx);
                            let selected = state.schedules[selected_idx].clone();
                            ui.set_selected_schedule_name(selected.name.clone().into());
                            ui.set_selected_steps_count(schedule_steps_count(&selected));
                        }

                        refresh_schedule_model(&ui, &state);
                    }
                }
            }
        }

        if *wifi_scan_in_progress.borrow() && unsafe { slint_bridge_wifi_scan_ready() } {
            let mut raw = vec![0 as c_char; 4096];
            let ok = unsafe { slint_bridge_wifi_scan_copy_results(raw.as_mut_ptr(), raw.len() as i32) };
            let json = if ok {
                unsafe { CStr::from_ptr(raw.as_ptr()) }.to_string_lossy().into_owned()
            } else {
                "[]".to_string()
            };

            let rows = parse_wifi_scan_rows(&json);
            if let Some(ui) = ui_weak.upgrade() {
                ui.set_wifi_networks(ModelRc::new(VecModel::from(rows)));
                ui.set_wifi_scan_busy(false);
            }
            *wifi_scan_in_progress.borrow_mut() = false;
        }

        // Render
        if use_full_frame_flush {
            let frame_start = Instant::now();
            let mut had_updates = false;
            let mut used_direct_fb = false;
            let mut dirty_min_x = i32::MAX;
            let mut dirty_min_y = i32::MAX;
            let mut dirty_max_x = -1;
            let mut dirty_max_y = -1;
            let mut submit_w: i32 = 0;
            let mut submit_h: i32 = 0;
            window.draw_if_needed(|renderer| {
                if allow_p4_direct_framebuffer && is_new_p4_display {
                    let mut p4_fb_ptr: *mut u16 = core::ptr::null_mut();
                    let mut p4_fb_w: i32 = 0;
                    let mut p4_fb_h: i32 = 0;
                    let direct_ok = unsafe {
                        display_driver_p4_begin_frame(&mut p4_fb_ptr, &mut p4_fb_w, &mut p4_fb_h)
                            && !p4_fb_ptr.is_null()
                            && p4_fb_w > 0
                            && p4_fb_h > 0
                    };
                    if direct_ok {
                        used_direct_fb = true;
                        let fb_pixels = (p4_fb_w as usize).saturating_mul(p4_fb_h as usize).max(1);
                        let fb_slice = unsafe { std::slice::from_raw_parts_mut(p4_fb_ptr, fb_pixels) };
                        let buffer = P4RotatedLineBuffer {
                            line_buffer: &mut line_buffer,
                            frame_buffer: fb_slice,
                            frame_width: p4_fb_w as usize,
                            src_width: physical_w as usize,
                            src_height: physical_h as usize,
                            had_updates: &mut had_updates,
                            dirty_min_x: &mut dirty_min_x,
                            dirty_min_y: &mut dirty_min_y,
                            dirty_max_x: &mut dirty_max_x,
                            dirty_max_y: &mut dirty_max_y,
                        };
                        renderer.render_by_line(buffer);
                        return;
                    }
                    // On NewP4 never mix direct-framebuffer and fallback blit paths in the same run:
                    // mixed submit modes can produce visible flash/overdraw during state transitions.
                    return;
                }
                let buffer = BufferedDisplayLineBuffer {
                    line_buffer: &mut line_buffer,
                    frame_buffer: &mut frame_buffer,
                    frame_width: physical_w as usize,
                    had_updates: &mut had_updates,
                    dirty_min_x: &mut dirty_min_x,
                    dirty_min_y: &mut dirty_min_y,
                    dirty_max_x: &mut dirty_max_x,
                    dirty_max_y: &mut dirty_max_y,
                };
                renderer.render_by_line(buffer);
            });
            if had_updates {
                if used_direct_fb {
                    let x1 = dirty_min_x.clamp(0, physical_w.saturating_sub(1));
                    let y1 = dirty_min_y.clamp(0, physical_h.saturating_sub(1));
                    let x2 = dirty_max_x.clamp(0, physical_w.saturating_sub(1));
                    let y2 = dirty_max_y.clamp(0, physical_h.saturating_sub(1));
                    if x2 >= x1 && y2 >= y1 {
                        let w = x2 - x1 + 1;
                        let h = y2 - y1 + 1;
                        submit_w = w;
                        submit_h = h;
                        unsafe {
                            let _ = display_driver_p4_present_frame_region(x1, y1, w, h);
                        }
                    } else {
                        // Nothing dirty in this frame; keep backbuffer and skip submit.
                        unsafe { display_driver_p4_cancel_frame(); }
                    }
                } else {
                    if force_p4_no_blit_fallback {
                        return;
                    }
                    let x1 = dirty_min_x.clamp(0, physical_w.saturating_sub(1));
                    let y1 = dirty_min_y.clamp(0, physical_h.saturating_sub(1));
                    let x2 = dirty_max_x.clamp(0, physical_w.saturating_sub(1));
                    let y2 = dirty_max_y.clamp(0, physical_h.saturating_sub(1));
                    if x2 >= x1 && y2 >= y1 {
                        let w = x2 - x1 + 1;
                        let h = y2 - y1 + 1;
                        submit_w = w;
                        submit_h = h;
                        if x1 == 0 && y1 == 0 && w == physical_w && h == physical_h {
                            // Full-frame dirty region: avoid extra temporary region allocation/copy.
                            unsafe {
                                let _ = display_driver_blit_rgb565(
                                    0,
                                    0,
                                    physical_w,
                                    physical_h,
                                    frame_buffer.as_ptr() as *const u16,
                                );
                            }
                        } else {
                            let src_w = physical_w as usize;
                            let rw = w as usize;
                            let rh = h as usize;
                            let mut region: Vec<Rgb565Pixel> = vec![Rgb565Pixel::default(); rw.saturating_mul(rh)];
                            for row in 0..rh {
                                let src_row = (y1 as usize).saturating_add(row);
                                let src_start = src_row.saturating_mul(src_w).saturating_add(x1 as usize);
                                let src_end = src_start.saturating_add(rw);
                                let dst_start = row.saturating_mul(rw);
                                let dst_end = dst_start.saturating_add(rw);
                                if src_end <= frame_buffer.len() && dst_end <= region.len() {
                                    region[dst_start..dst_end].copy_from_slice(&frame_buffer[src_start..src_end]);
                                }
                            }
                            unsafe {
                                let _ = display_driver_blit_rgb565(
                                    x1,
                                    y1,
                                    w,
                                    h,
                                    region.as_ptr() as *const u16,
                                );
                            }
                        }
                    }
                }
                let frame_ms = frame_start.elapsed().as_millis();
                let (reason, reason_at) = *render_reason.borrow();
                if reason != 0 && reason_at.elapsed() <= Duration::from_millis(900) {
                    let reason_label = match reason {
                        1 => "start_tap",
                        2 => "stop_tap",
                        3 => "stop_confirm",
                        4 => "wifi_got_ip",
                        5 => "cmd_start",
                        6 => "cmd_stop",
                        _ => "unknown",
                    };
                    println!(
                        "RENDER_PROFILE reason={} frame_ms={} submit={}x{} dirty=({},{})->({},{}) direct={}",
                        reason_label,
                        frame_ms,
                        submit_w,
                        submit_h,
                        dirty_min_x,
                        dirty_min_y,
                        dirty_max_x,
                        dirty_max_y,
                        used_direct_fb
                    );
                }
            } else if used_direct_fb {
                unsafe { display_driver_p4_cancel_frame(); }
            }
        } else {
            window.draw_if_needed(|renderer| {
                let buffer = DisplayLineBuffer {
                    line_buffer: &mut line_buffer,
                };
                renderer.render_by_line(buffer);
            });
        }

        // Keep a stable frame cadence to reduce micro-stutter on MCU rendering.
        if let Some(frame_budget) = frame_budget {
            let now = Instant::now();
            if now < next_frame_at {
                std::thread::sleep(next_frame_at - now);
                next_frame_at += frame_budget;
            } else {
                next_frame_at = now + frame_budget;
            }
        }
    }
}


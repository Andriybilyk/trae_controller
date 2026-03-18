use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType, Rgb565Pixel, LineBufferProvider};
use slint::platform::{Platform, PointerEventButton, WindowEvent};
use slint::{Image, ModelRc, Rgb8Pixel, SharedPixelBuffer, Timer, VecModel};
use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::rc::Rc;
use std::time::{Duration, Instant};

// Fonts are embedded via @font-face in the .slint files (compile-time), so we don't
// need any runtime font registration here.
const UI_TARGET_FPS: u64 = 30;
const TOUCH_HOLD_DEADZONE_PX: i32 = 1;
const TOUCH_FILTER_NUM: i32 = 1;
const TOUCH_FILTER_DEN: i32 = 2;
const TOUCH_FAST_MOVE_PX: i32 = 6;

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
        }
    }
}

extern "C" {
    fn slint_bridge_display_init();
    fn slint_bridge_get_state(out: *mut SlintKilnState);
    fn slint_bridge_start_schedule_json(json: *const c_char) -> bool;
    fn slint_bridge_load_schedule_json(json: *const c_char) -> bool;
    fn slint_bridge_stop();

    fn slint_bridge_set_fan_manual(enabled: bool);
    fn slint_bridge_set_fan_power(percent: i32);
    fn slint_bridge_wifi_is_connected() -> bool;
    fn slint_bridge_wifi_connect(ssid: *const c_char, password: *const c_char) -> bool;
    fn slint_bridge_wifi_disconnect();
    fn slint_bridge_wifi_scan_start();
    fn slint_bridge_wifi_scan_ready() -> bool;
    fn slint_bridge_wifi_scan_copy_results(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_wifi_server_url_copy(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_schedules_revision() -> u32;
    fn slint_bridge_get_command_result_revision() -> u32;
    fn slint_bridge_copy_command_result(out: *mut SlintCommandResult) -> bool;
    fn slint_bridge_notify_schedules_changed();
    fn slint_bridge_ui_heartbeat();
    fn slint_bridge_get_time_str(out: *mut c_char, out_len: i32) -> bool;
    fn slint_bridge_get_language_is_ua() -> bool;
    fn slint_bridge_set_language_is_ua(is_ua: bool);

    fn display_driver_touch_probe(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_touch_probe_raw(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_set_touch_calibration(enabled: bool, left: u16, right: u16, top: u16, bottom: u16) -> bool;
    fn display_driver_set_touch_affine(enabled: bool, a: f32, b: f32, c: f32, d: f32, e: f32, f: f32) -> bool;
    fn display_driver_set_touch_grid(enabled: bool, dx: *const f32, dy: *const f32) -> bool;
    fn display_driver_blit_rgb565(x: i32, y: i32, w: i32, h: i32, data: *const u16) -> bool;
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

struct AppState {
    schedules: Vec<ScheduleFile>,
    selected_index: Option<usize>,
    editing_index: Option<usize>,
    editor_steps: Vec<EditorStepRow>,
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
        render_fn(&mut self.line_buffer[range.clone()]);
        unsafe {
            let ptr = self.line_buffer[range.clone()].as_ptr() as *const u16;
            let _ = display_driver_blit_rgb565(
                range.start as i32,
                line as i32,
                (range.end - range.start) as i32,
                1,
                ptr,
            );
        }
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

fn load_schedules() -> Vec<ScheduleFile> {
    let path = "/littlefs/schedules.json";
    let Ok(data) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    serde_json::from_str(&data).unwrap_or_default()
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

fn to_editor_steps(schedule: &ScheduleFile) -> Vec<EditorStepRow> {
    schedule
        .steps
        .iter()
        .map(|s| EditorStepRow {
            step_type: s.step_type.clone().into(),
            rate: s.rate.unwrap_or(0),
            target: s.target.unwrap_or(0),
            hold: s.hold_time.unwrap_or(0),
        })
        .collect()
}

fn to_schedule_steps(existing: &[ScheduleStepFile], editor_steps: &[EditorStepRow]) -> Vec<ScheduleStepFile> {
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
            step.rate = Some(s.rate);
            step.target = Some(s.target);
            step.hold_time = Some(s.hold);
        }
        out.push(step);
    }
    out
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

const CONTROLLER_AP_SSID: &str = "TRAE_KILN_SETUP";

fn wifi_qr_payload(wifi_ok: bool, server_url: &str) -> String {
    if wifi_ok {
        server_url.to_string()
    } else {
        format!("WIFI:T:nopass;S:{};;", CONTROLLER_AP_SSID)
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

fn c_buf_to_string(buf: &[u8]) -> String {
    let nul = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..nul]).trim().to_string()
}

fn apply_state_to_ui(ui: &AppWindow, state: SlintKilnState) {
    let status_label = match state.status {
        1 => "PREHEAT",
        2 => "RAMP",
        3 => "HOLD",
        4 => "COOL",
        5 => "COMPLETE",
        6 => "ERROR",
        7 => "TUNING",
        _ => "IDLE",
    };
    let is_idle = !state.is_firing;

    ui.set_temp(state.current_temp as f32);
    ui.set_temp_label(format!("{:.1}", state.current_temp).into());
    ui.set_target(state.target_temp as i32);
    ui.set_status_label(status_label.into());
    ui.set_is_idle(is_idle);
    ui.set_step_index(state.current_step as i32);
    ui.set_total_steps(state.total_steps as i32);
    ui.set_time_remaining(format_minutes_to_hm(state.time_remaining).into());

    let mut fault_reason = c_buf_to_string(&state.fault_reason);
    if fault_reason.is_empty() {
        fault_reason = c_buf_to_string(&state.error_msg);
    }
    ui.set_fault_active(state.fault_active);
    ui.set_fault_reason(fault_reason.into());

    let wifi_ok = unsafe { slint_bridge_wifi_is_connected() };
    ui.set_wifi_ok(wifi_ok);

    let mut raw_url = vec![0 as c_char; 96];
    let server_url = if unsafe { slint_bridge_wifi_server_url_copy(raw_url.as_mut_ptr(), raw_url.len() as i32) } {
        unsafe { CStr::from_ptr(raw_url.as_ptr()) }.to_string_lossy().into_owned()
    } else {
        "http://192.168.4.1".to_string()
    };
    ui.set_wifi_server_url(server_url.into());
}

fn apply_command_result_to_ui(ui: &AppWindow, cmd: SlintCommandResult, is_ua: bool) {
    let mut message = c_buf_to_string(&cmd.message);
    if message.is_empty() {
        message = if cmd.ok { "ok".to_string() } else { "error".to_string() };
    }
    let action = c_buf_to_string(&cmd.action);
    let prefix = if is_ua { "Команда" } else { "Command" };
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
        0 => (40, 60),   // top-left
        1 => (240, 60),  // top-middle
        2 => (439, 60),  // top-right
        3 => (40, 160),  // mid-left
        4 => (240, 160), // mid
        5 => (439, 160), // mid-right
        6 => (40, 259),  // bottom-left
        7 => (240, 259), // bottom-middle
        8 => (439, 259), // bottom-right
        _ => (240, 160),
    }
}

fn calib_hint_for_step(is_ua: bool, step: u8) -> &'static str {
    match (is_ua, step) {
        (true, 0) => "Торкніться точки (верх-ліворуч)",
        (true, 1) => "Торкніться точки (верх-центр)",
        (true, 2) => "Торкніться точки (верх-праворуч)",
        (true, 3) => "Торкніться точки (центр-ліворуч)",
        (true, 4) => "Торкніться точки (центр)",
        (true, 5) => "Торкніться точки (центр-праворуч)",
        (true, 6) => "Торкніться точки (низ-ліворуч)",
        (true, 7) => "Торкніться точки (низ-центр)",
        (true, 8) => "Торкніться точки (низ-праворуч)",
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

    let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);

    slint::platform::set_platform(Box::new(EspPlatform {
        window: window.clone(),
        start: Instant::now(),
    }))
    .expect("set_platform failed");

    // IMPORTANT: call the WindowAdapter::set_size() implementation so the renderer sees the correct size.
    // The MinimalSoftwareWindow also has a convenience `set_size()` method, but that does not update the adapter size.
    slint::platform::WindowAdapter::set_size(
        window.as_ref(),
        slint::PhysicalSize::new(480, 320).into(),
    );

    let ui = AppWindow::new().expect("Failed to create UI");
    ui.window().show().unwrap_or(());
    let lang_is_ua = unsafe { slint_bridge_get_language_is_ua() };
    ui.set_is_ua(lang_is_ua);
    ui.set_wifi_networks(ModelRc::new(VecModel::from(Vec::<WifiNetworkRow>::new())));
    ui.set_wifi_qr_image(build_qr_image(&format!("WIFI:T:nopass;S:{};;", CONTROLLER_AP_SSID)));
    ui.set_wifi_qr_hint("Scan to join controller Wi-Fi".into());

    let mut state = AppState {
        schedules: load_schedules(),
        selected_index: None,
        editing_index: None,
        editor_steps: Vec::new(),
    };

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
    let kb_key_last = Rc::new(RefCell::new((String::new(), Instant::now() - Duration::from_secs(5))));
    let wifi_scan_in_progress = Rc::new(RefCell::new(false));

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_open_library(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Schedules);
            let state = app_state.borrow();
            refresh_schedule_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_toggle_language(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let is_ua = !ui.get_is_ua();
            ui.set_is_ua(is_ua);
            unsafe { slint_bridge_set_language_is_ua(is_ua) };
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
        ui.on_start_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            {
                let mut last = start_cooldown_at.borrow_mut();
                if last.elapsed() < Duration::from_millis(700) {
                    return;
                }
                *last = Instant::now();
            }
            let state = app_state.borrow();
            if let Some(idx) = state.selected_index {
                if let Some(schedule) = state.schedules.get(idx) {
                    if let Ok(json) = serde_json::to_string(schedule) {
                        if let Ok(c_json) = CString::new(json) {
                            unsafe {
                                let _ = slint_bridge_start_schedule_json(c_json.as_ptr());
                            }
                        }
                    }
                    ui.set_view(View::Dashboard);
                    return;
                }
            }
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        ui.on_stop_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
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
        let stop_cooldown_at = stop_cooldown_at.clone();
        ui.on_stop_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
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
        let app_state = app_state.clone();
        ui.on_select_schedule(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let idx = index as usize;
            if idx < state.schedules.len() {
                state.selected_index = Some(idx);
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
                let schedule = state.schedules[idx].clone();
                state.editor_steps = to_editor_steps(&schedule);
                ui.set_selected_schedule_name(schedule.name.clone().into());
                refresh_editor_model(&ui, &state);
                ui.set_view(View::Editor);
            }
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_new_schedule(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            let name = if ui.get_is_ua() { "Нова програма" } else { "New Program" };
            let id = make_safe_id(name);
            state.schedules.push(ScheduleFile {
                id,
                name: name.to_string(),
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
                let name = ui.get_selected_schedule_name().to_string();
                let steps = to_schedule_steps(&state.schedules[idx].steps, &state.editor_steps);
                if let Some(schedule) = state.schedules.get_mut(idx) {
                    schedule.name = name;
                    schedule.steps = steps;
                    schedule.steps_count = Some(schedule.steps.len() as i32);
                    if schedule.id.is_empty() {
                        schedule.id = make_safe_id(&schedule.name);
                    }
                    if schedule.schedule_type.is_empty() {
                        schedule.schedule_type = "Custom".to_string();
                    }
                }
                let _ = save_schedules(&state.schedules);
                unsafe { slint_bridge_notify_schedules_changed() };
                refresh_schedule_model(&ui, &state);
                ui.set_view(View::Schedules);
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
            state.editor_steps.push(EditorStepRow {
                step_type: "ramp".into(),
                rate: 100,
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
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                let v = parse_int_with_clamp(value.as_str(), 0, 9999, step.rate);
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
                let v = parse_int_with_clamp(value.as_str(), 0, 2000, step.target);
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
                ui.set_kb_value(value.clone().into());
                if field == "name" || field == "wifi_pass" {
                    ui.set_kb_mode("alpha".into());
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
                            let trimmed = candidate.trim();
                            if !trimmed.is_empty() {
                                ui.set_selected_schedule_name(trimmed.into());
                            }
                        } else if field == "wifi_pass" {
                            ui.set_wifi_password(candidate.clone().into());
                            let ssid = ui.get_wifi_selected_ssid().to_string();
                            let ssid_trimmed = ssid.trim();
                            if !ssid_trimmed.is_empty() {
                                if let (Ok(c_ssid), Ok(c_pass)) = (CString::new(ssid_trimmed), CString::new(candidate)) {
                                    unsafe {
                                        let _ = slint_bridge_wifi_connect(c_ssid.as_ptr(), c_pass.as_ptr());
                                    }
                                }
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

            let (min, max, max_len) = match field.as_str() {
                "rate" => (0, 9999, 4usize),
                "target" => (0, 2000, 4usize),
                "hold" => (0, 999, 3usize),
                "fan" => (0, 100, 3usize),
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
                    } else if field == "fan" {
                        let parsed = parse_int_with_clamp(&value, min, max, ui.get_fan_power());
                        ui.set_fan_power(parsed);
                        unsafe { slint_bridge_set_fan_power(parsed) };
                    }
                    ui.set_kb_visible(false);
                }
                digit if digit.len() == 1 && digit.chars().all(|c| c.is_ascii_digit()) => {
                    let mut v = ui.get_kb_value().to_string();
                    if v == "0" {
                        v.clear();
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

    let mut line_buffer = vec![Rgb565Pixel::default(); 480];
    let mut last_touch = false;
    let mut last_x: u16 = 0;
    let mut last_y: u16 = 0;
    let mut filtered_x: i32 = 0;
    let mut filtered_y: i32 = 0;
    let mut last_sent_x: i32 = 0;
    let mut last_sent_y: i32 = 0;
    let mut locked_scroll_x: i32 = 0;
    let mut have_last_sent = false;
    let mut last_state_update = Instant::now();
    let mut last_ui_heartbeat = Instant::now();
    let mut last_clock_update = Instant::now();
    let mut last_schedules_revision = unsafe { slint_bridge_get_schedules_revision() };
    let mut last_command_result_revision = unsafe { slint_bridge_get_command_result_revision() };
    let mut command_feedback_until = Instant::now();
    let mut last_qr_payload = String::new();
    let frame_budget = Duration::from_millis(1000 / UI_TARGET_FPS);
    let mut next_frame_at = Instant::now() + frame_budget;

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
            }
        }

        let command_revision = unsafe { slint_bridge_get_command_result_revision() };
        if command_revision != 0 && command_revision != last_command_result_revision {
            let mut cmd = SlintCommandResult::default();
            if unsafe { slint_bridge_copy_command_result(&mut cmd as *mut SlintCommandResult) } {
                if let Some(ui) = ui_weak.upgrade() {
                    apply_command_result_to_ui(&ui, cmd, ui.get_is_ua());
                }
                command_feedback_until = Instant::now() + Duration::from_millis(2600);
            }
            last_command_result_revision = command_revision;
        }
        if Instant::now() > command_feedback_until {
            if let Some(ui) = ui_weak.upgrade() {
                if ui.get_command_feedback_visible() {
                    ui.set_command_feedback_visible(false);
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
        let pressed = unsafe {
            if calib_active {
                display_driver_touch_probe_raw(&mut x, &mut y, &mut z)
            } else {
                display_driver_touch_probe(&mut x, &mut y, &mut z)
            }
        };

        if let Some(ui) = ui_weak.upgrade() {
            ui.set_touch_x(x as i32);
            ui.set_touch_y(y as i32);
            ui.set_touch_z(z as i32);
            ui.set_touch_pressed(pressed);
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
                        ui.set_calib_hint(if ui.get_is_ua() { "Готово" } else { "Done" }.into());
                    } else {
                        ui.set_calib_hint(if ui.get_is_ua() { "Помилка калібрування" } else { "Calibration failed" }.into());
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

            let mut send_x_i32 = filtered_x;
            if !calib_active && active_view == View::Settings {
                // Settings list should scroll only vertically.
                send_x_i32 = locked_scroll_x;
            }

            let send_x = send_x_i32 as u16;
            let send_y = filtered_y as u16;
            last_x = send_x;
            last_y = send_y;
            let pos = slint::LogicalPosition::new(send_x as f32, send_y as f32);
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
            let pos = slint::LogicalPosition::new(last_x as f32, last_y as f32);
            let _ = window.dispatch_event(WindowEvent::PointerReleased { position: pos, button: PointerEventButton::Left });
            have_last_sent = false;
        }
        last_touch = pressed;

        // Status update
        if last_state_update.elapsed() >= Duration::from_millis(500) {
            last_state_update = Instant::now();
            let s = state_poll();
            apply_state_to_ui(&ui, s);
            let wifi_ok = ui.get_wifi_ok();
            let url = ui.get_wifi_server_url().to_string();
            let qr_payload = wifi_qr_payload(wifi_ok, &url);
            if qr_payload != last_qr_payload {
                last_qr_payload = qr_payload.clone();
                ui.set_wifi_qr_image(build_qr_image(&qr_payload));
                if wifi_ok {
                    let hint = if ui.get_is_ua() {
                        format!("Відкрити веб-інтерфейс: {}", url)
                    } else {
                        format!("Open web interface: {}", url)
                    };
                    ui.set_wifi_qr_hint(hint.into());
                } else {
                    let hint = if ui.get_is_ua() {
                        format!("1) Скануйте QR ({}), 2) відкрийте http://192.168.4.1", CONTROLLER_AP_SSID)
                    } else {
                        format!("1) Scan QR ({}), 2) open http://192.168.4.1", CONTROLLER_AP_SSID)
                    };
                    ui.set_wifi_qr_hint(hint.into());
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
                                .position(|s| s.name == previous_name)
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
        window.draw_if_needed(|renderer| {
            let buffer = DisplayLineBuffer {
                line_buffer: &mut line_buffer,
            };
            renderer.render_by_line(buffer);
        });

        // Keep a stable frame cadence to reduce micro-stutter on MCU rendering.
        let now = Instant::now();
        if now < next_frame_at {
            std::thread::sleep(next_frame_at - now);
            next_frame_at += frame_budget;
        } else {
            next_frame_at = now + frame_budget;
        }
    }
}



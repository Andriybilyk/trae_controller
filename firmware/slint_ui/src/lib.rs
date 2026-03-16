use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType, Rgb565Pixel, LineBufferProvider};
use slint::platform::{Platform, PointerEventButton, WindowEvent, WindowAdapter};
use slint::{ModelRc, VecModel};
use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;
use std::rc::Rc;
use std::time::{Duration, Instant};

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

    fn display_driver_touch_probe(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_touch_probe_raw(x: *mut u16, y: *mut u16, z: *mut u16) -> bool;
    fn display_driver_set_touch_calibration(enabled: bool, left: u16, right: u16, top: u16, bottom: u16) -> bool;
    fn display_driver_blit_rgb565(x: i32, y: i32, w: i32, h: i32, data: *const u16) -> bool;
}

#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct ScheduleStepFile {
    #[serde(rename = "type")]
    step_type: String,
    #[serde(default)]
    rate: i32,
    #[serde(default)]
    target: i32,
    #[serde(rename = "holdTime", default)]
    hold_time: i32,
}

#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct ScheduleFile {
    name: String,
    #[serde(default)]
    steps: Vec<ScheduleStepFile>,
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
    let Ok(json) = serde_json::to_string(schedules) else {
        return false;
    };
    std::fs::write(path, json).is_ok()
}

fn schedule_to_row(schedule: &ScheduleFile, selected: bool) -> ScheduleRow {
    ScheduleRow {
        name: schedule.name.clone().into(),
        steps_count: schedule.steps.len() as i32,
        selected,
    }
}

fn to_editor_steps(schedule: &ScheduleFile) -> Vec<EditorStepRow> {
    schedule
        .steps
        .iter()
        .map(|s| EditorStepRow {
            is_hold: s.step_type == "hold",
            target: s.target,
            hold: s.hold_time,
        })
        .collect()
}

fn to_schedule_steps(editor_steps: &[EditorStepRow]) -> Vec<ScheduleStepFile> {
    editor_steps
        .iter()
        .map(|s| {
            if s.is_hold {
                ScheduleStepFile {
                    step_type: "hold".to_string(),
                    rate: 0,
                    target: 0,
                    hold_time: s.hold,
                }
            } else {
                ScheduleStepFile {
                    step_type: "ramp".to_string(),
                    rate: 100,
                    target: s.target,
                    hold_time: 0,
                }
            }
        })
        .collect()
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
        2 => "RAMP",
        3 => "HOLD",
        5 => "COMPLETE",
        6 => "ERROR",
        7 => "TUNING",
        _ => "IDLE",
    };
    let is_idle = status_label == "IDLE" || status_label == "COMPLETE" || status_label == "ERROR";

    ui.set_temp(state.current_temp as f32);
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

    // TODO: bridge Wi-Fi status (for now, keep the dot as an "OK" indicator + calibration entry point).
    ui.set_wifi_ok(true);
}

fn state_poll() -> SlintKilnState {
    let mut out = SlintKilnState::default();
    unsafe { slint_bridge_get_state(&mut out as *mut SlintKilnState) };
    out
}

#[derive(Clone, Copy, Default)]
struct CalibPoints {
    tl: Option<(u16, u16)>,
    tr: Option<(u16, u16)>,
    br: Option<(u16, u16)>,
    bl: Option<(u16, u16)>,
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
        1 => (439, 60),  // top-right
        2 => (439, 259), // bottom-right
        3 => (40, 259),  // bottom-left
        _ => (240, 160),
    }
}

fn calib_hint_for_step(is_ua: bool, step: u8) -> &'static str {
    match (is_ua, step) {
        (true, 0) => "Торкніться точки (верх-ліворуч)",
        (true, 1) => "Торкніться точки (верх-праворуч)",
        (true, 2) => "Торкніться точки (низ-праворуч)",
        (true, 3) => "Торкніться точки (низ-ліворуч)",
        (false, 0) => "Tap the dot (top-left)",
        (false, 1) => "Tap the dot (top-right)",
        (false, 2) => "Tap the dot (bottom-right)",
        (false, 3) => "Tap the dot (bottom-left)",
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
    let (tlx, tly) = points.tl?;
    let (trx, try_) = points.tr?;
    let (brx, bry) = points.br?;
    let (blx, bly) = points.bl?;

    let x_l = ((tlx as u32 + blx as u32) / 2) as i32;
    let x_r = ((trx as u32 + brx as u32) / 2) as i32;
    let y_t = ((tly as u32 + try_ as u32) / 2) as i32;
    let y_b = ((bry as u32 + bly as u32) / 2) as i32;

    // Targets must match calib_target_for_step().
    const W: i32 = 480;
    const H: i32 = 320;
    const X_MARGIN: i32 = 40;
    const Y_MARGIN: i32 = 60;
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

    let clamp_u16 = |v: i32, max: i32| -> u16 { v.clamp(0, max) as u16 };
    let left_u = clamp_u16(left, W - 1);
    let right_u = clamp_u16(right, W - 1);
    let top_u = clamp_u16(top, H - 1);
    let bottom_u = clamp_u16(bottom, H - 1);

    Some((left_u, right_u, top_u, bottom_u))
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
        ui.set_selected_steps_count(selected.steps.len() as i32);

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
        let app_state = app_state.clone();
        ui.on_start_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
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
        ui.on_stop_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
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
                ui.set_selected_steps_count(schedule.steps.len() as i32);
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
            let name = if ui.get_is_ua() { "Нова програма" } else { "New Schedule" };
            state.schedules.push(ScheduleFile {
                name: name.to_string(),
                steps: Vec::new(),
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
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_save(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(idx) = state.editing_index {
                let steps = to_schedule_steps(&state.editor_steps);
                if let Some(schedule) = state.schedules.get_mut(idx) {
                    schedule.steps = steps;
                }
                let _ = save_schedules(&state.schedules);
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
            state.editor_steps.push(EditorStepRow {
                is_hold: false,
                target: 500,
                hold: 30,
            });
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_toggle_step(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                step.is_hold = !step.is_hold;
                if step.is_hold && step.hold == 0 {
                    step.hold = 30;
                }
                if !step.is_hold && step.target == 0 {
                    step.target = 500;
                }
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_target_plus(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                step.target = (step.target + 10).min(2000);
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_target_minus(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                step.target = (step.target - 10).max(0);
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_hold_plus(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                step.hold = (step.hold + 1).min(999);
            }
            refresh_editor_model(&ui, &state);
        });
    }

    {
        let ui_weak = ui_weak.clone();
        let app_state = app_state.clone();
        ui.on_editor_hold_minus(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut state = app_state.borrow_mut();
            if let Some(step) = state.editor_steps.get_mut(index as usize) {
                step.hold = (step.hold - 1).max(0);
            }
            refresh_editor_model(&ui, &state);
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
    let mut last_state_update = Instant::now();

    loop {
        slint::platform::update_timers_and_animations();

        // Touch input
        let (calib_active, calib_step) = {
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

        // Calibration: accept taps close to the shown dot (prevents random taps / cancel button from advancing steps).
        if calib_active && pressed && !last_touch {
            const R: i32 = 80;
            let (tx, ty) = calib_target_for_step(calib_step);
            let dx = x as i32 - tx;
            let dy = y as i32 - ty;
            if dx * dx + dy * dy <= R * R {
                let mut calib = calib_state.borrow_mut();
                match calib.step {
                    0 => calib.points.tl = Some((x, y)),
                    1 => calib.points.tr = Some((x, y)),
                    2 => calib.points.br = Some((x, y)),
                    3 => calib.points.bl = Some((x, y)),
                    _ => {}
                }

                if calib.step < 3 {
                    calib.step += 1;
                    if let Some(ui) = ui_weak.upgrade() {
                        update_calib_ui(&ui, &calib);
                    }
                } else {
                    // Finalize
                    let result = compute_calibration(calib.points);
                    let ok = if let Some((l, r, t, b)) = result {
                        unsafe { display_driver_set_touch_calibration(true, l, r, t, b) }
                    } else {
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
        }

        if pressed {
            let pos = slint::LogicalPosition::new(x as f32, y as f32);
            let _ = window.dispatch_event(WindowEvent::PointerMoved { position: pos });
            if !last_touch {
                let _ = window.dispatch_event(WindowEvent::PointerPressed { position: pos, button: PointerEventButton::Left });
            }
        } else if last_touch {
            let pos = slint::LogicalPosition::new(x as f32, y as f32);
            let _ = window.dispatch_event(WindowEvent::PointerReleased { position: pos, button: PointerEventButton::Left });
        }
        last_touch = pressed;

        // Status update
        if last_state_update.elapsed() >= Duration::from_millis(500) {
            last_state_update = Instant::now();
            let s = state_poll();
            apply_state_to_ui(&ui, s);
        }

        // Render
        window.draw_if_needed(|renderer| {
            let mut buffer = DisplayLineBuffer {
                line_buffer: &mut line_buffer,
            };
            renderer.render_by_line(buffer);
        });

        std::thread::sleep(Duration::from_millis(5));
    }
}



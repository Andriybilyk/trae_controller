use slint::{ModelRc, VecModel, Timer};
use std::cell::RefCell;
use std::rc::Rc;
use chrono::Timelike;

slint::include_modules!();

fn make_schedule(name: &str, steps: i32, selected: bool) -> ScheduleRow {
    ScheduleRow {
        name: name.into(),
        steps_count: steps,
        selected,
    }
}

fn make_editor_step(rate: i32, target: i32, hold: i32) -> EditorStepRow {
    EditorStepRow {
        step_type: "ramp".into(),
        rate,
        target,
        hold,
    }
}

fn make_graph_rect(x: i32, y: i32, width: i32, height: i32) -> ScheduleGraphRect {
    ScheduleGraphRect { x, y, width, height, color: slint::Color::from_rgb_u8(52, 223, 172) }
}

fn make_graph_marker(x: i32, y: i32, label: &str) -> ScheduleGraphMarker {
    ScheduleGraphMarker { x, y, label: label.into(), color: slint::Color::from_rgb_u8(255, 255, 255) }
}

fn parse_int(text: &str, min: i32, max: i32, fallback: i32) -> i32 {
    match text.trim().parse::<i32>() {
        Ok(v) => v.clamp(min, max),
        Err(_) => fallback,
    }
}

fn main() {
    // Match MCU renderer to avoid layout differences vs. device.
    std::env::set_var("SLINT_RENDERER", "software");
    std::env::set_var("SLINT_SCALE_FACTOR", "1");

    let ui = AppWindow::new().expect("Failed to create UI");
    ui.window().set_size(slint::LogicalSize::new(480.0, 320.0));

    ui.set_boot_splash_visible(false);
    ui.set_standby_visible(false);
    ui.set_temp(24.5);
    ui.set_temp_label("24.5".into());
    ui.set_target(0);
    ui.set_status_label("IDLE".into());
    ui.set_is_idle(true);
    ui.set_step_index(0);
    ui.set_total_steps(0);
    ui.set_time_remaining("--:--".into());
    ui.set_selected_schedule_name("".into());
    ui.set_selected_steps_count(0);
    ui.set_wifi_ok(true);
    ui.set_fault_active(false);
    ui.set_fault_reason("".into());
    ui.set_schedule_graph_peak_temp(760);
    ui.set_schedule_graph_total_label("4h 20m".into());
    ui.set_schedule_graph_rects(ModelRc::new(VecModel::from(vec![
        make_graph_rect(0, 56, 40, 3),
        make_graph_rect(38, 44, 3, 3),
        make_graph_rect(44, 34, 3, 3),
        make_graph_rect(50, 24, 3, 3),
        make_graph_rect(56, 14, 3, 3),
        make_graph_rect(62, 8, 42, 3),
        make_graph_rect(104, 8, 3, 3),
        make_graph_rect(112, 20, 3, 3),
        make_graph_rect(120, 32, 3, 3),
        make_graph_rect(128, 44, 3, 3),
        make_graph_rect(136, 56, 50, 3),
    ])));
    ui.set_schedule_graph_markers(ModelRc::new(VecModel::from(vec![
        make_graph_marker(40, 56, "1"),
        make_graph_marker(104, 8, "2"),
        make_graph_marker(186, 56, "3"),
    ])));

    let schedules_state = Rc::new(RefCell::new(vec![
        make_schedule("Glass 90 polish", 8, false),
        make_schedule("Glass 96 slump", 8, false),
        make_schedule("Custom", 3, false),
    ]));
    let editor_state = Rc::new(RefCell::new(vec![
        make_editor_step(100, 500, 0),
        make_editor_step(0, 0, 30),
        make_editor_step(200, 25, 0),
    ]));

    let selected_index: Rc<RefCell<Option<usize>>> = Rc::new(RefCell::new(None));

    let refresh_schedules = |ui: &AppWindow, schedules: &Rc<RefCell<Vec<ScheduleRow>>>| {
        ui.set_schedules(ModelRc::new(VecModel::from(schedules.borrow().clone())));
    };
    let refresh_editor = |ui: &AppWindow, steps: &Rc<RefCell<Vec<EditorStepRow>>>| {
        ui.set_editor_steps(ModelRc::new(VecModel::from(steps.borrow().clone())));
    };

    refresh_schedules(&ui, &schedules_state);
    refresh_editor(&ui, &editor_state);

    // Toggle language
    {
        let ui_weak = ui.as_weak();
        ui.on_toggle_language(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_is_ua(!ui.get_is_ua());
        });
    }

    // Open library
    {
        let ui_weak = ui.as_weak();
        ui.on_open_library(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Schedules);
        });
    }

    // Select schedule
    {
        let ui_weak = ui.as_weak();
        let schedules = schedules_state.clone();
        let selected = selected_index.clone();
        ui.on_select_schedule(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut s = schedules.borrow_mut();
            for item in s.iter_mut() {
                item.selected = false;
            }
            let idx = index as usize;
            if let Some(item) = s.get_mut(idx) {
                item.selected = true;
                *selected.borrow_mut() = Some(idx);
                ui.set_selected_schedule_name(item.name.clone());
                ui.set_selected_steps_count(item.steps_count);
                ui.set_view(View::Dashboard);
            }
            ui.set_schedules(ModelRc::new(VecModel::from(s.clone())));
        });
    }

    // Edit schedule
    {
        let ui_weak = ui.as_weak();
        ui.on_edit_schedule(move |_index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Editor);
        });
    }

    {
        let ui_weak = ui.as_weak();
        let schedules = schedules_state.clone();
        ui.on_open_schedule_graph(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if let Some(item) = schedules.borrow().get(index as usize) {
                ui.set_selected_schedule_name(item.name.clone());
                ui.set_selected_steps_count(item.steps_count);
            }
            ui.set_view(View::ScheduleGraph);
        });
    }

    // New schedule
    {
        let ui_weak = ui.as_weak();
        let schedules = schedules_state.clone();
        let selected = selected_index.clone();
        ui.on_new_schedule(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let name = if ui.get_is_ua() { "\u{41d}\u{43e}\u{432}\u{430}\u{20}\u{43f}\u{440}\u{43e}\u{433}\u{440}\u{430}\u{43c}\u{430}" } else { "New Program" };
            schedules.borrow_mut().push(make_schedule(name, 0, false));
            *selected.borrow_mut() = None;
            ui.set_selected_schedule_name(name.into());
            ui.set_selected_steps_count(0);
            ui.set_view(View::Editor);
            ui.set_schedules(ModelRc::new(VecModel::from(schedules.borrow().clone())));
        });
    }

    // Editor callbacks
    {
        let ui_weak = ui.as_weak();
        ui.on_editor_back(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui.as_weak();
        ui.on_editor_save(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_view(View::Schedules);
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_editor_add_step(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut steps = editor.borrow_mut();
            steps.push(make_editor_step(100, 500, 0));
            ui.set_editor_steps(ModelRc::new(VecModel::from(steps.clone())));
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_editor_rate_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if let Some(step) = editor.borrow_mut().get_mut(index as usize) {
                step.rate = parse_int(value.as_str(), 0, 9999, step.rate);
            }
            ui.set_editor_steps(ModelRc::new(VecModel::from(editor.borrow().clone())));
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_editor_target_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if let Some(step) = editor.borrow_mut().get_mut(index as usize) {
                step.target = parse_int(value.as_str(), 0, 2000, step.target);
            }
            ui.set_editor_steps(ModelRc::new(VecModel::from(editor.borrow().clone())));
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_editor_hold_changed(move |index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            if let Some(step) = editor.borrow_mut().get_mut(index as usize) {
                step.hold = parse_int(value.as_str(), 0, 999, step.hold);
            }
            ui.set_editor_steps(ModelRc::new(VecModel::from(editor.borrow().clone())));
        });
    }

    {
        let ui_weak = ui.as_weak();
        ui.on_kb_open(move |field, step_index, value| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_kb_step_index(step_index);
            ui.set_kb_field(field);
            ui.set_kb_value(value);
            ui.set_kb_visible(true);
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_kb_key(move |key| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let key = key.to_string();
            let field = ui.get_kb_field().to_string();

            if field == "name" {
                match key.as_str() {
                    "CANCEL" => ui.set_kb_visible(false),
                    "CLR" => ui.set_kb_value("".into()),
                    "BS" => {
                        let mut v = ui.get_kb_value().to_string();
                        v.pop();
                        ui.set_kb_value(v.into());
                    }
                    "SPACE" => {
                        let mut v = ui.get_kb_value().to_string();
                        v.push(' ');
                        ui.set_kb_value(v.into());
                    }
                    "OK" => {
                        ui.set_selected_schedule_name(ui.get_kb_value());
                        ui.set_kb_visible(false);
                    }
                    other if other.chars().count() == 1 => {
                        let mut v = ui.get_kb_value().to_string();
                        v.push_str(other);
                        ui.set_kb_value(v.into());
                    }
                    _ => {}
                }
                return;
            }

            match key.as_str() {
                "CANCEL" => ui.set_kb_visible(false),
                "CLR" => ui.set_kb_value("".into()),
                "BS" => {
                    let mut v = ui.get_kb_value().to_string();
                    v.pop();
                    ui.set_kb_value(v.into());
                }
                "OK" => {
                    let idx = ui.get_kb_step_index() as usize;
                    let value = ui.get_kb_value().to_string();
                    let mut steps = editor.borrow_mut();
                    if let Some(step) = steps.get_mut(idx) {
                        match field.as_str() {
                            "rate" => step.rate = parse_int(&value, 0, 9999, step.rate),
                            "target" => step.target = parse_int(&value, 0, 2000, step.target),
                            "hold" => step.hold = parse_int(&value, 0, 999, step.hold),
                            _ => {}
                        }
                    }
                    ui.set_editor_steps(ModelRc::new(VecModel::from(steps.clone())));
                    ui.set_kb_visible(false);
                }
                digit if digit.len() == 1 && digit.chars().all(|c| c.is_ascii_digit()) => {
                    let mut v = ui.get_kb_value().to_string();
                    v.push_str(digit);
                    ui.set_kb_value(v.into());
                }
                _ => {}
            }
        });
    }

    {
        let ui_weak = ui.as_weak();
        let editor = editor_state.clone();
        ui.on_editor_remove(move |index| {
            let Some(ui) = ui_weak.upgrade() else { return; };
            let mut steps = editor.borrow_mut();
            if (index as usize) < steps.len() {
                steps.remove(index as usize);
            }
            ui.set_editor_steps(ModelRc::new(VecModel::from(steps.clone())));
        });
    }

    // Start/Stop simulation
    {
        let ui_weak = ui.as_weak();
        ui.on_start_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_is_idle(false);
            ui.set_status_label("RAMP".into());
            ui.set_target(700);
            ui.set_step_index(1);
            ui.set_total_steps(8);
            ui.set_time_remaining("01:20".into());
            ui.set_view(View::Dashboard);
        });
    }

    {
        let ui_weak = ui.as_weak();
        ui.on_stop_firing(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_stop_confirm(true);
        });
    }

    {
        let ui_weak = ui.as_weak();
        ui.on_stop_cancel(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_stop_confirm(false);
        });
    }

    {
        let ui_weak = ui.as_weak();
        ui.on_stop_confirm(move || {
            let Some(ui) = ui_weak.upgrade() else { return; };
            ui.set_show_stop_confirm(false);
            ui.set_is_idle(true);
            ui.set_status_label("IDLE".into());
        });
    }

    // Update time in header
    let timer = Timer::default();
    let ui_weak = ui.as_weak();
    timer.start(slint::TimerMode::Repeated, std::time::Duration::from_secs(1), move || {
        let Some(ui) = ui_weak.upgrade() else { return; };
        let now = chrono::Local::now();
        ui.set_current_time(format!("{:02}:{:02}", now.hour(), now.minute()).into());
    });

    ui.run().unwrap();
}


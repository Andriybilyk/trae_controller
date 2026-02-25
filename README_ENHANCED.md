# TRAE KILN CONTROLLER (Enhanced Edition)

This is a complete rewrite of the open-source Kiln Controller, designed to rival the **TAP II Pro** in features and reliability, while keeping the hardware cost under $50.

## Key Features
*   **Multi-Zone Support**: 1-3 independent zones with PID control.
*   **Cone Fire Mode**: Built-in Orton Cone schedules (022-10) with Slow/Medium/Fast speeds.
*   **Glass Wizard**: Automated schedules for COE 90/96 (Full Fuse, Tack, Slump, Anneal).
*   **Smart Schedules**: Unlimited steps, Ramp/Hold/Cool, live editing.
*   **Modern UI**: React-based dashboard with real-time graphs, dark mode, and mobile support.
*   **Safety First**: Over-temp shutdown, watchdog timer, thermocouple failure detection.
*   **Connectivity**: WiFi, REST API, WebSocket, MQTT-ready.

## System Architecture
*   **Firmware (`firmware/`)**: ESP32-S3 code using PlatformIO. Handles real-time control, safety, and API.
*   **Backend (`backend/`)**: Node.js server for data logging, schedule generation logic, and serving the frontend (optional, can run on PC).
*   **Frontend (`frontend/`)**: React/Vite application for the user interface.

## Hardware BOM (Budget < $50)

| Component | Description | Est. Price | Link Example |
|-----------|-------------|------------|--------------|
| **MCU** | ESP32-S3 DevKitC-1 (USB-C) | $6.00 | AliExpress |
| **Display** | 3.5" or 4.3" TFT SPI (ILI9488/ILI9341) with Touch | $12.00 | AliExpress |
| **Thermocouple** | MAX31855 K-Type Module + Probe | $5.00 ea | Amazon/Ali |
| **Relay** | SSR-40DA (Solid State Relay) | $5.00 ea | Amazon |
| **Power** | 5V 2A USB Power Supply | $3.00 | Local |
| **Misc** | Wires, Breadboard/PCB, Case | $5.00 | - |
| **Safety** | Door Switch (Mechanical Limit Switch) | $1.00 | - |

**Total for 1-Zone System: ~$37.00**

## Wiring Guide (1-Zone)

*   **MAX31855**:
    *   VCC -> 3.3V
    *   GND -> GND
    *   CLK -> GPIO 18
    *   CS -> GPIO 5
    *   DO -> GPIO 19
*   **SSR**:
    *   (+) -> GPIO 15
    *   (-) -> GND
*   **Display (SPI)**:
    *   MOSI -> GPIO 23
    *   MISO -> GPIO 19
    *   CLK -> GPIO 18
    *   CS -> GPIO 5 (Shared with MAX, use different CS in real build!) -> *Note: Config uses distinct pins if possible, check `config.h`*
    *   DC -> GPIO 2
    *   RST -> GPIO 4

*Note: In `config.h`, ensure CS pins for Display and Thermocouple are unique. Default config shares SPI bus but needs unique CS.*

## Installation Instructions

### 1. Firmware
1.  Install VS Code and **PlatformIO** extension.
2.  Open the `firmware` folder.
3.  Edit `include/config.h` to match your pinout.
4.  Connect ESP32-S3 via USB.
5.  Run "PlatformIO: Upload".

### 2. Backend (Optional/Dev)
1.  Install Node.js.
2.  Open `backend` folder.
3.  Run `npm install`.
4.  Run `npm start` to launch the API server at `http://localhost:3000`.

### 3. Frontend
1.  Open `frontend` folder.
2.  Run `npm install`.
3.  Run `npm run dev` to start the UI at `http://localhost:5173`.
4.  Configure the UI to point to your ESP32 IP address (in `Dashboard.tsx`).

## Usage
1.  **Dashboard**: View current temp and status.
2.  **Schedules**: Create custom ramp/hold schedules.
3.  **Wizards**: Use "Cone Wizard" for ceramics or "Glass Wizard" for fusing.
4.  **Start**: Click "Start Firing" and monitor the graph.

## Safety Warnings
*   **High Voltage**: Kilns use dangerous voltages (110/220V). Always disconnect power before working on hardware.
*   **Unattended Operation**: Never leave a firing kiln unattended.
*   **Thermal Runaway**: Ensure your SSR fails "Open". Use a mechanical backup contactor if possible.

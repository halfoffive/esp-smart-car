# ESP32 Smart Car - Knowledge Base

**Project:** ESP32 智能车控制系统
**Stack:** Arduino/C++ (ESP32), Rust (Axum), Vue 3 (Vite + TailwindCSS)
**Architecture:** 嵌入式固件 + 桌面端控制界面

## Structure

```
esp-smart-car/
├── firmware/              # Embedded firmware (Arduino IDE)
│   ├── car_controller/     # Vehicle controller (ESP32-C6)
│   ├── camera_module/     # Camera module (ESP32-S3 CAM)
│   └── receiver_dongle/   # USB receiver (ESP32-C6)
├── desktop/               # Desktop control interface
│   ├── backend/           # Rust backend (Axum + WebSocket)
│   └── frontend/          # Vue frontend (Web UI)
└── docs/                  # Hardware documentation
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| Add motor control logic | `firmware/car_controller/motor_control.h` | Functional programming style |
| Add servo control logic | `firmware/car_controller/servo_control.h` | Smooth movement algorithm |
| Modify wireless protocol | `firmware/car_controller/wireless.h` | ESP-NOW protocol |
| Add camera resolution | `firmware/camera_module/camera_config.h` | OV2640 configuration |
| Add video streaming | `firmware/camera_module/video_stream.h` | Frame packetization |
| Add serial communication | `desktop/backend/src/serial.rs` | USB serial port |
| Add WebSocket handlers | `desktop/backend/src/websocket.rs` | Real-time video |
| Add REST API endpoints | `desktop/backend/src/api.rs` | HTTP API |
| Add UI components | `desktop/frontend/src/components/` | Vue SFC components |
| Add keyboard controls | `desktop/frontend/src/composables/useKeyboard.ts` | WASD mapping |
| Hardware wiring | `docs/hardware.md` | Complete pinout diagram |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `MotorControl` | namespace | `motor_control.h` | 4-motor differential drive |
| `ServoControl` | namespace | `servo_control.h` | 2-servo gimbal control |
| `WirelessProtocol` | namespace | `wireless.h` | ESP-NOW communication |
| `CameraConfig` | struct | `camera_config.h` | OV2640 configuration |
| `VideoStream` | namespace | `video_stream.h` | Frame transmission |
| `SerialManager` | struct | `serial.rs` | USB serial communication |
| `WebSocketManager` | struct | `websocket.rs` | Client connection management |
| `AppState` | struct | `main.rs` | Shared application state |

## Conventions

### Embedded (C++ - Arduino)
- **Functional programming style**: Heavy use of `const`, pure functions, immutable state
- **Chinese comments**: All functions, structs, enums have detailed Chinese comments
- **Type safety**: Use `enum class` instead of `enum` to prevent implicit conversions
- **Namespace organization**: Use `namespace` for each module (e.g., `PinConfig`, `SG90Config`)
- **Struct-based state**: All state stored in const structs, new state created via functions
- **Hardware abstraction**: Pure functions for logic, separate `apply_*` functions for side effects

### Backend (Rust)
- **Tokio async**: All I/O operations are async
- **Arc + Mutex**: Shared state via `Arc<Mutex<T>>`
- **Error handling**: Use `anyhow::Result` for error propagation
- **Tracing**: Structured logging with `tracing` crate
- **Modular design**: Separate modules for serial, websocket, API

### Frontend (Vue + TypeScript)
- **Composition API**: Use `<script setup>` and composables
- **Pinia**: State management (pre-configured)
- **TailwindCSS**: Utility-first styling with custom theme
- **Functional components**: Composables for reusable logic
- **WebSocket**: Real-time communication via custom composable

## Anti-Patterns

- **Never use `as any` or `@ts-ignore`**: Type safety is enforced
- **Never suppress errors**: Empty catch blocks are forbidden
- **Never delete failing tests**: Fix the code, not the tests
- **Never use global mutable state** in embedded: All state must be explicit
- **Never mix concerns**: Motor logic separate from servo logic separate from wireless
- **Never use implicit conversions** in C++: Always explicit type casting

## Unique Styles

- **Functional C++**: All data structures are immutable, state changes via function returns
- **Binary protocol**: Custom 8-byte packet format for ESP-NOW communication
- **Frame packetization**: Video frames split into 128-byte chunks for wireless transmission
- **Differential steering**: Left/right motor speed differential for turning
- **Smooth servo movement**: Gradual angle changes with configurable speed steps

## Commands

```bash
# Backend (Rust)
cd desktop/backend
cargo build        # Build
cargo run          # Run server (serves frontend at http://localhost:8080)

# Frontend (Vue + Bun)
cd desktop/frontend
bun install        # Install dependencies
bun run dev        # Development server (port 3000)
bun run build      # Production build (outputs to ../backend/frontend/dist)

# Firmware (Arduino IDE)
# Open .ino files in Arduino IDE
# Select board: ESP32C6 or ESP32S3
# Upload to respective devices
```

## Hardware Wiring

See `docs/hardware.md` for complete pinout diagram.

Key connections:
- **L298N #1**: GPIO 4,5,6 (left motors)
- **L298N #2**: GPIO 7,8,9 (right motors)
- **Servo H**: GPIO 14 (horizontal gimbal)
- **Servo V**: GPIO 15 (vertical gimbal)
- **Camera**: ESP32-S3 standard CAM pins

## Notes

- **Power isolation**: Motor power and logic power must be separate
- **Ground common**: All devices must share common ground
- **Baud rate**: 921600 for USB serial (high-speed video)
- **ESP-NOW channel**: Fixed channel 1 for all devices
- **Video buffer**: 4096 bytes for frame reassembly
- **Timeout protection**: 1-second auto-stop if no commands received
- **Calibration**: Servo angles may need adjustment based on physical mounting

## 额外要求

- **编程风格**: 函数式编程，大量中文注释。
- **当完成修改**: 更新"AGENTS.md","CHANGELOG","README"，然后提交并推送git。
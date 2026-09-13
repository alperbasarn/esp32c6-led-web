# Architecture

## Current runtime

The firmware is currently implemented in `main/app_main.cpp`. It combines the
embedded web UI, LED renderer, Wi-Fi/captive DNS services, Matter callbacks,
NVS persistence, and OTA lifecycle.

The MQTT link to the QNOB HMI is the first module split out of that translation
unit: `main/mqtt_link.cpp` (broker client, NVS settings, paired controllers,
pairing side effects) over `main/mqtt_proto.cpp` (pure topic/JSON/pairing/
coalescer logic, host-testable with `test/host/run_tests.sh`). It reaches the
LED core only through `main/led_control.h`, never through app_main's globals.

```mermaid
flowchart LR
    UI[Embedded web UI] --> HTTP[HTTP API]
    HTTP --> State[LED and configuration state]
    Matter[Matter controller] --> MatterCB[Matter callbacks]
    MatterCB --> State
    State --> Effect[Effect task snapshot]
    Effect --> LED[LED renderer]
    State --> NVS[NVS persistence]
    WiFi[SoftAP and STA] --> HTTP
    WiFi --> Matter
    OTA[Published or manual OTA] --> Probation[OTA probation]
    Probation --> NVS
```

## State ownership

- `s_state_mutex` protects the main LED/configuration state and update status.
- `s_led_mutex` serializes access to the physical LED strip.
- `s_ota_mutex` prevents simultaneous OTA operations.
- Matter callbacks update the canonical LED state and persist Matter color
  trackers (`matter_x`, `matter_y`, and `matter_temp`).
- The effect task is the only runtime renderer. It renders from a copied state
  snapshot, wakes on state changes, and does not hold `s_state_mutex` while
  touching the LED strip.
- The effect task also owns a private "displayed" state (`s_disp_*`) that eases
  brightness and per-channel color toward the target snapshot each frame
  (exponential ease, `kEaseTauMs`; snaps within `kEaseEpsilon`). Only the effect
  task touches these fields — the eased values are display-only and never feed
  back into the canonical state or Matter. Power is derived from the eased
  brightness so on/off fades run to true black.
- Final output is gamma-corrected through `s_gamma_lut`, a 256-entry LUT built
  once at boot by `init_gamma_lut()` (exponent `kGammaExponent`) before the
  effect task starts. Gamma is applied last, per channel, folding effect
  modulation and the eased brightness/color into a single pass.
- The task's notify wait is transition-aware: it keeps rendering at ~40 ms while
  the effect animates OR while an ease is still in flight, and blocks
  indefinitely (`portMAX_DELAY`) only once both settled and non-animated. Any
  control/Matter/schedule change calls `notify_effect_task()` to wake it.
- Pairing feedback (the blink code, then a green or red flash) is a mode flag
  the effect task copies inside the same `s_state_mutex` critical section as the
  snapshot and renders afterwards through `apply_solid_frame()`. The MQTT link
  only sets the flag, so it never touches the strip or blocks on it.
- `s_mutex` inside `mqtt_link.cpp` guards the link's own state. It is never held
  across a publish or a `led_control_*` call, and `led_control_*` never calls
  back into the link, so the two locks cannot invert. A Matter callback marking
  the link dirty is a flag write plus a task notify.

### Control paths

`led_control_apply_json()` in `app_main.cpp` is the single validation and
clamping path: `POST /api/control` calls it with the full-tuple requirement and
an immediate NVS write, the MQTT `set` handler calls it with partial updates and
a 2 s debounced write that the schedule task flushes
(`led_control_persist_tick()`). Both then notify the effect task and schedule
the Matter sync.

## Update flows

### Web control

1. The browser submits `/api/control`.
2. The request is validated and clamped.
3. State is persisted to NVS and the effect task is notified.
4. Matter attributes are synchronized asynchronously.

### Matter control

1. Matter invokes the attribute callback.
2. The callback converts Matter color values into the canonical RGB state.
3. The LED state is persisted and the effect task is notified.
4. Web clients see the result on their next state request.

### Published OTA

1. The device downloads and verifies the signed manifest.
2. The firmware image is downloaded to the inactive OTA partition.
3. Image size and SHA-256 are checked.
4. An OTA probation marker is written.
5. The device reboots and waits for Wi-Fi plus Matter readiness.
6. The marker is cleared on success; repeated failures roll back.

### Manual OTA

Manual uploads use the same probation lifecycle but intentionally do not use
the signed release-manifest trust chain. This path is for development and
recovery only.

## Network boundaries

- The SoftAP is the setup and administration boundary.
- LAN clients may inspect status and control LEDs.
- Configuration, OTA, reboot, revert, and factory reset require a SoftAP
  client. So do the MQTT broker settings (`mqtt_*` on `POST /api/config`) and
  `POST /api/mqtt/unpair`; the broker password is never returned by
  `GET /api/state`, and host/port/user are returned to SoftAP clients only.
- Controllers on the broker are a separate, weaker boundary: pairing decides
  which HMI may drive this strip, but any client with broker credentials can
  publish. Per-device credentials and ACLs remain future work.
- Matter onboarding codes are returned only to SoftAP clients while the Matter
  commissioning window is open.

The current SoftAP boundary is a network-location check, not a replacement for
authenticated HTTPS sessions. A future production hardening pass should add
authenticated sessions and TLS.

## Target module layout

The long-term target is to split the current translation unit into:

```text
main/
  app_main.cpp
  led_engine.cpp/.h
  matter_controller.cpp/.h
  network_manager.cpp/.h
  web_server.cpp/.h
  ota_manager.cpp/.h
  storage.cpp/.h
  mqtt_link.cpp/.h      (done)
  mqtt_proto.cpp/.h     (done, host-tested)
  led_control.h         (done: the LED core's public surface)
  web/
    index.html
    app.js
    styles.css
```

Each module should expose events or snapshots rather than directly modifying
another module's globals. The LED driver should have one owning task, and NVS
writes should be queued/debounced rather than performed inside Matter update
callbacks.

## Required verification

- Build with the pinned ESP-IDF and esp-matter revisions.
- Test manual and published OTA with power interruption.
- Test rollback after repeated self-test failure.
- Commission with Apple Home over BLE and after fabric removal.
- Verify LAN clients cannot perform administration actions.
- Verify changing LED count clears pixels outside the new count.
- Verify RGB, HS, XY, and color-temperature changes survive reboot.
- Run `test/host/run_tests.sh` (protocol layer) before touching the MQTT
  contract, and verify on hardware: broker connect, retained `info`/`status`/
  `state`/`controllers`, a `set` from a paired and from an unpaired controller,
  the blink-code pairing flow, and that factory reset clears both the broker
  settings and the controller list.

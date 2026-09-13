<!--
Mirrored from the HMI repository (homio-round-hmi:
screen/SW/ESP/dev_notes/led_mqtt_contract.md). Sections 1-6 below are that
document verbatim — it is the shared spec, so edit it there first and copy the
result here. How this firmware implements it is at the end of this file.
-->

# LED instances over MQTT — contract v1 (draft, 2026-09-13)

Owner decisions: the LED device is `alperbasarn/esp32c6-led-web`; the transport between the round
HMI and every LED instance is **MQTT** through the existing broker; the LED firmware may change;
the HMI must **pair, unpair, monitor and control** any number of instances.

This document is the single spec both firmwares implement. The HMI side is tracked in the WBS as
`LED-01`..`LED-08`; the LED side is the task list at the end (its own repository, its own release
train). Nothing here is implemented yet.

## 1. Identity

- `<id>`: the LED device id, lower case, derived from the STA MAC the same way the SoftAP name is
  (`led-` + the last three MAC bytes, e.g. `led-a1b2c3`). Stable across reboots and firmware.
- `<ctl>`: the controller id of an HMI unit, `qnob-` + its last three STA MAC bytes.
- Names are user-facing labels (24 bytes max), never identities.

## 2. Topics

All under the prefix `homio/led/`. Retained topics are marked **R**.

| Topic | Dir | R | QoS | Payload |
|---|---|---|---|---|
| `homio/led/<id>/info` | LED -> HMI | R | 1 | `{"id","model":"esp32c6-led-web","fw","name","max_leds","ip"}` published on every connect |
| `homio/led/<id>/status` | LED -> HMI | R | 1 | `online` on connect; `offline` as the **LWT** (keepalive 30 s, so an unplugged strip reads offline within 45 s) |
| `homio/led/<id>/state` | LED -> HMI | R | 1 | `{"power","brightness","color","effect","effect_params","effect_color","count","src","seq"}`; the same fields and ranges as `GET /api/state` (brightness 0-255, `color` `#RRGGBB`, `effect` name, five 0-255 params, `count` read-only); coalesced to at most 10 per second, always the full tuple |
| `homio/led/<id>/set` | HMI -> LED | - | 1 | any subset of `power`, `brightness`, `color`, `effect`, `effect_params`, `effect_color` plus `"from":"<ctl>"` and `"seq"`; the LED applies, persists (debounced), then publishes `state` with `src` = that `from` |
| `homio/led/<id>/pair` | HMI -> LED | - | 1 | `{"controller":"<ctl>","name":"<label>"}` opens the flow; `{"controller","code":N}` confirms it (section 4) |
| `homio/led/<id>/unpair` | HMI -> LED | - | 1 | `{"controller":"<ctl>"}` from a paired controller |
| `homio/led/<id>/controllers` | LED -> HMI | R | 1 | `{"paired":[{"id":"<ctl>","name":"..."}]}` after every change |

Discovery is a subscription to `homio/led/+/info` and `homio/led/+/status`; a retained `info`
arriving without a paired entry is an **available** instance, one with a paired entry is **mine**.

Retired on the HMI side once this lands: `ledRing/brightnessControl`, `ledRing/colorControl`,
`ledRing/modeControl`, `esp32/light/*` (WBS Q8 and Q7 are answered: one broker, one client).

## 3. Control and monitoring rules (HMI)

- Outbound `set` is a `LightModel` observer with a 100 ms coalescing window and a trailing flush
  (the MDL-12 light limiter moves from 10 ms to 100 ms); a knob drag never exceeds 10 publishes/s.
- Echo suppression like the media path: a `state` whose `src` equals `<ctl>` and whose `seq` is
  at or below the last sent one is applied without re-triggering a publish; anything else is a
  remote change (web page, Matter, another HMI) and wins.
- Monitoring is passive: retained `state`/`status` on subscribe, then pushes. No polling.
- Presence: `status` offline or no `state` for 120 s marks the instance offline; commands to an
  offline instance are refused locally (no queueing).
- Limits: 8 instances in the registry, fixed arrays, no heap on the path (MDL-01 budget +1.2 KB).

## 4. Pairing (why it exists, and the flow)

Pairing is **ownership, not security**. Every client on the broker account can publish anything;
per-device broker credentials and ACLs are the security boundary and stay a later step (the
current credentials are public and awaiting rotation, PCKNOB-02b). What pairing gives:

- the HMI knows which of the announcing instances are *its* strips (a household with two HMIs and
  four strips needs this);
- the LED honours `set` only from controllers in its paired list (`from` is checked; unpaired
  senders get a `state` with `"rejected":true` and nothing changes), which stops a mis-tap on a
  neighbour's HMI on the same account from changing the wrong strip;
- proof of presence at pairing time without a display or button on the strip.

Flow: the HMI publishes `pair` with its id. The LED opens a 60 s window, blinks a **code**
(1-6 blinks, repeated), and waits. The user dials the blink count on the knob and confirms; the
HMI publishes `pair` with `code`. On a match the LED stores the controller (NVS, max 4), publishes
`controllers`, and flashes green once; on a mismatch or timeout it flashes red and stays unpaired.
A factory-fresh LED (no controllers stored) accepts the first `pair` without a code so the very
first setup is one tap. Unpair is immediate from a paired controller or through the web factory
reset. The HMI keeps its own list (id, name) in NVS through the single settings writer (APP-29)
and re-posts it as the registry's first records at boot.

## 5. Failure modes covered

| Case | Behaviour |
|---|---|
| LED loses WiFi | LWT flips `status` to `offline`; the HMI badge goes grey within 45 s; retained `state` stays for the last-known view |
| Broker down | both sides reconnect with backoff; the HMI shows every instance as unknown, not offline |
| Two HMIs turn the same knob | last `set` wins; each sees the other's `state` (different `src`) and follows it |
| LED changed from the web page or Matter | `state` with `src` `web`/`matter`; the HMI follows, no echo |
| LED flash budget (C6, 4 MB, two 1.94 MB slots, already trimmed) | esp-mqtt over the mbedtls already linked for HTTPS; measure before merging (LED task 1); fallback is plain MQTT to a LAN broker |

## 6. LED-side tasks (esp32c6-led-web)

1. Measure: add esp-mqtt (TLS, cert bundle already present) and report flash and free heap with
   Matter up; decide TLS vs LAN-plain from the numbers.
2. MQTT client with broker settings in NVS (host, port, user, password; set from the SoftAP-gated
   config page), LWT, reconnect with backoff.
3. Publish `info`, `status`, `state` (coalesced 10/s, `src`/`seq`), `controllers`.
4. Subscribe `set`, `pair`, `unpair`; apply through the existing `control` validation path
   (same clamps as `/api/control`); persist debounced.
5. Pairing window, blink code, controller list in NVS (max 4), first-pair-free rule.
6. Reject `set` from unpaired controllers with a `rejected` state.
7. Web page: MQTT status and the paired controller list; factory reset clears controllers.
8. Docs: this contract mirrored in the LED repository.

## 7. How this firmware implements it

| Contract piece | Where |
|---|---|
| Topic building, JSON encode/decode, pairing state machine, publish coalescer | [`main/mqtt_proto.cpp`](../main/mqtt_proto.cpp) — pure, no ESP dependency, covered by [`test/host/run_tests.sh`](../test/host/run_tests.sh) |
| Broker client, NVS settings, controller list, reconnect backoff, LWT | [`main/mqtt_link.cpp`](../main/mqtt_link.cpp) |
| The single validation/clamping path shared with `POST /api/control` | `led_control_apply_json()` in `main/app_main.cpp`, declared in [`main/led_control.h`](../main/led_control.h) |
| Blink code and green/red flashes | the effect task in `main/app_main.cpp` (`led_control_indicator_*`) |
| Broker settings UI, link status, paired list, unpair | the Configuration tab of the embedded page; `POST /api/config` and `POST /api/mqtt/unpair`, both SoftAP-gated |

Device-side decisions that the HMI should know about:

- **`<id>`** is `led-` plus the last three bytes of the **station** MAC, lower case. The
  SoftAP SSID is built from the SoftAP MAC, which on this chip is the station MAC with the
  last byte incremented — so the two can differ by one. The Configuration tab prints the
  exact id under **MQTT Link → Device Id**; trust that, not the SSID.
- **`name`** in `info` is the SoftAP SSID (for example `ESP32C6-LED-A1B2C3`). There is no
  separate device-name setting in this firmware yet.
- **`src`** on locally originated `state` publishes is `web` (page or `/api/config`),
  `matter`, `schedule` (fixed schedules and the sleep/wake timer) or `device` (the retained
  publish on every connect). `seq` for those is a device-local counter; for a `set` it is
  the sender's own `seq`, echoed back.
- A `set` from a **paired** controller always produces a `state` publish, even when a field
  was out of range and got clamped or ignored, so the controller can correct its optimistic
  view. A `set` from an **unpaired** controller changes nothing and produces a `state` with
  `"rejected":true` and `src` set to the offender.
- `count` is read-only over MQTT, exactly as on `POST /api/control`; it is a SoftAP-gated
  configuration item.
- Persistence for MQTT `set` is debounced by 2 s (one flash write per burst); the web and
  Matter paths still write through immediately. A power cut within that window loses the
  last MQTT change.
- Pairing is limited to 4 controllers. A `pair` from a controller that is already paired is
  idempotent (it refreshes the label and flashes green); a `pair` that would exceed the
  limit flashes red.
- Broker settings are accepted only on the SoftAP-gated `POST /api/config`
  (`mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass`, `mqtt_tls`). An empty `mqtt_host`
  disables the link. The password is never returned by `GET /api/state`.

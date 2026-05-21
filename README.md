# ESP32-C6 Matter WS2812B Controller

This is a small ESP-IDF + ESP-Matter test project for an ESP32-C6 driving a WS2812B LED strip.

It exposes the strip in two ways:

- A local web page for LED count, color, brightness, effects, OTA, and reset actions
- A Matter extended color light endpoint for Apple Home
- Local OTA firmware upload from the same web page (manual `.bin` install)
- Periodic background checks against the latest published GitHub release; user-triggered install with a signed-manifest verification chain
- A firmware revert action that boots back into the other OTA slot
- A factory reset action that clears Matter pairing, Wi-Fi AP config, and saved LED settings
- Built-in LED effects: `glow`, `rainbow`, `chase`, `sparkle`, `wave`
- Per-effect controls in the web UI with unique meanings for each animation

The firmware starts a Wi-Fi SoftAP, serves the control page directly from the board, and includes captive-portal style redirects plus a small DNS responder so phones and laptops are more likely to open the page automatically. After the device joins your normal Wi-Fi through Matter commissioning, the same web UI is also reachable on its LAN IP.

## Defaults

- SoftAP SSID/password: generated per device on first boot and stored in NVS
- Web UI: `http://192.168.4.1`
- LED data GPIO: `17` (`D7` on XIAO ESP32C6) — configurable via Kconfig
- Maximum strip length compiled in: `256` — configurable via Kconfig
- Auto-update check interval: 6 h, with `[0, 30 min]` first-poll jitter — configurable via Kconfig
- Matter device type: extended color light

## Wiring

- `XIAO ESP32C6 D7 / GPIO17` -> `DIN` on WS2812B strip
- `GND` -> strip ground
- External `5V` -> strip power
- Share ground between the ESP32-C6 and LED power supply

For anything beyond a few LEDs, use an external 5V supply.

## Build and flash

You need ESP-IDF `v5.4.1` and esp-matter at the commit pinned in [`.github/workflows/publish-firmware.yml`](.github/workflows/publish-firmware.yml) (`ESP_MATTER_REF`). For full setup instructions see the [Espressif ESP-IDF guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32c6/get-started/index.html) and the [esp-matter README](https://github.com/espressif/esp-matter).

Once your environment is set up:

```bash
. ~/esp/esp-idf-5.4.1/export.sh
export ESP_MATTER_PATH=~/esp/esp-matter-pinned
cd ~/esp32c6-led-web
idf.py set-target esp32c6 build
idf.py -p /dev/ttyACM0 flash monitor          # Linux/macOS
idf.py -p COM6 flash monitor                  # Windows
```

**The first install needs a USB flash.** After that, the device can pull new firmware over the air — either by uploading a `.bin` you built locally (Configuration → Install From File) or, more usually, by waiting for the next published GitHub release and pressing Install Update.

For first-time USB flash, the CI workflow publishes `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` (you'll generate this locally), and `esp32c6_led_web.bin` to every GitHub release. You can flash all four with esptool directly without a local toolchain:

```bash
python -m esptool --chip esp32c6 -p COM6 -b 460800 --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
    0x0 bootloader.bin \
    0xc000 partition-table.bin \
    0x1d000 ota_data_initial.bin \
    0x20000 esp32c6_led_web.bin
```

## Web UI layout

The device page is split into three tabs:

- `Overview`: Matter state, AP and LAN web UI URLs, current firmware version, running slot, revert target, latest available release, update status
- `Configuration`: LED count, SoftAP SSID/password, the Firmware Update card (Current vs. Available version, **Install Update** button, Check For Updates button, and a manual **Install From File** path), revert button, factory reset, reboot
- `Control`: brightness, color, and one sub-tab per effect with its own parameters

The SoftAP credentials shown in `Configuration` are the credentials hosted by the ESP32-C6 itself for the local setup page. On a fresh device they are generated automatically and printed to the serial log when the AP starts.

## Pair with Apple Home

1. Flash the firmware and open the serial monitor.
2. Watch for the Matter onboarding output.
3. Open Apple Home on your iPhone or iPad.
4. Tap `+`, then `Add Accessory`.
5. Scan the Matter QR code from the serial log, or enter the manual setup code.
6. Finish commissioning while the phone is connected over BLE and the ESP32-C6 has network access.

The local web page also shows the current Matter state, manual setup code, QR URL, and both web UI addresses.

## OTA updates

There are two OTA paths in the firmware:

**Install From File (dev escape hatch).** Build a `.bin` locally with `idf.py build`, open the device web UI, in the `Firmware Update` card choose your file, click `Install From File`. The device writes the bytes to the inactive OTA slot and reboots into it. No signature verification on this path — it bypasses the manifest-trust chain by design so you can recover a stuck device or test pre-release builds. **Do not use this path to ship firmware to anyone but yourself.**

**Install Update (published-release flow).** The device periodically (every `CONFIG_APP_UPDATE_INTERVAL_HOURS`, default 6 h) fetches `manifest.json` from the latest GitHub release and verifies its ECDSA P-256 signature. If a newer version is available, the Firmware Update card shows the version and enables an `Install Update` button. Pressing the button downloads the binary, SHA-256-verifies it against the manifest, switches the boot partition, reboots, and runs a 2-strike self-test before promoting the image to permanent. See the next section.

After at least one successful update, the `Revert To Previous Firmware` button in `Configuration` can boot the device back into the other application slot without erasing settings.

Both paths update the application partition only. Bootloader and partition table changes still require USB flashing.

## Published auto-updates

The firmware watches the latest GitHub release for this repository (set via `CONFIG_APP_RELEASE_REPO`, default `alperbasarn/esp32c6-led-web`) and installs newer published builds once the device has Wi-Fi access.

Each release publishes a signed `manifest.json` to the GitHub release. The device:

1. Fetches `https://github.com/<repo>/releases/latest/download/manifest.json`.
2. Fetches `manifest.json.sig` (detached ECDSA P-256 DER signature over the manifest bytes).
3. Verifies the signature against the public key embedded in [`main/include/release_pubkey.h`](main/include/release_pubkey.h). Rejects the release if the signature is missing or invalid.
4. Compares `manifest.version` against the running firmware. Skips if not newer.
5. Cohort-gates: a stable hash of the device MAC % 100 must be less than `manifest.rollout.percent`. Devices outside the cohort surface an "available, waiting" status without installing.
6. Downloads the binary via `esp_https_ota`, then reads the freshly written OTA partition back and compares its SHA-256 against `manifest.app.sha256`. On mismatch the boot partition is reverted before reboot.
7. After reboot, an early-boot probation step writes a strike counter to NVS (`ota_health` namespace) and neutralizes the bootloader's built-in 1-strike rollback by calling `esp_ota_mark_app_valid_cancel_rollback()`. We're now in charge of probation.
8. A `self_test_task` waits up to `CONFIG_APP_UPDATE_SELF_TEST_TIMEOUT_S` (default 90 s) for **both Wi-Fi STA up AND Matter ready**. On success it clears the NVS marker — the image is permanent. On timeout it `esp_restart()`s; the next boot increments the strike count.
9. After `APP_OTA_MAX_STRIKES` (= 2) consecutive failed attempts, the early-boot probation step rolls back: `esp_ota_set_boot_partition(previous_slot)` + clear marker + restart.

So a broken release has three layers of defense:

| Failure                                               | Caught by                |
|-------------------------------------------------------|--------------------------|
| Bytes don't match `manifest.app.sha256`                | SHA-256 check, no reboot |
| Boots, but Wi-Fi/Matter never come up                  | self-test timeout (×2)   |
| Panics or hangs the watchdog before self-test runs    | next boot's strike count |

Each device's first poll is jittered randomly in `[0, CONFIG_APP_UPDATE_INITIAL_JITTER_MIN]` minutes after Wi-Fi up. The `Check For Updates` button bypasses the jitter. The check is automatic and periodic; install is always manual.

### Publishing a release

**Rule:** any `.bin` that signs a manifest and ships to devices in the field must come from CI, never a local build. Local builds are for development and your own USB-flashed test board.

To cut a release:

1. Bump `PROJECT_VER` in [`CMakeLists.txt`](CMakeLists.txt). The new value is what the device's `esp_app_get_description()->version` will report.
2. Commit and push to `main`. The push triggers the `Publish Firmware` workflow which builds + warms the CI cache (no release artifacts attached on push).
3. `gh release create vX.Y --generate-notes --title "X.Y"`. This fires the workflow on the `release: published` event.
4. The workflow builds the app (`idf.py set-target esp32c6 build`), then runs [`scripts/generate-manifest.sh`](scripts/generate-manifest.sh) which produces `manifest.json` + `manifest.json.sig` and uploads:
   - `esp32c6_led_web.bin` — the OTA application image
   - `bootloader.bin`, `partition-table.bin` — needed for first-time USB flashes
   - `manifest.json`, `manifest.json.sig` — what devices read

Pinned versions live in workflow env (`IDF_VERSION`, `ESP_MATTER_REF`). If you upgrade the Matter stack or IDF, bump them in [`.github/workflows/publish-firmware.yml`](.github/workflows/publish-firmware.yml) so CI matches what you build locally.

ESP-IDF is cached in CI under the `main` branch scope (no per-tag re-clone). esp-matter is shallow-cloned each run because a full recursive clone exceeds GitHub's 10 GB per-repo cache budget; shallow clone takes ~10 min per release. Total release CI runtime: ~20 min.

### Signing key management

The release manifest is signed by `scripts/generate-manifest.sh` using an ECDSA P-256 private key passed via the `MANIFEST_SIGNING_KEY` repository secret. The matching public key is embedded in firmware at [`main/include/release_pubkey.h`](main/include/release_pubkey.h).

To rotate keys:

```bash
openssl ecparam -name prime256v1 -genkey -noout -out manifest-signing.key
openssl ec -in manifest-signing.key -pubout -outform PEM > release_pubkey.pem
# 1. Paste release_pubkey.pem into kReleasePubKeyPem in main/include/release_pubkey.h
# 2. Commit, build, and roll out the firmware update so devices learn the new pubkey
# 3. Only AFTER devices have updated, replace the MANIFEST_SIGNING_KEY secret with the new private key
# 4. Cut the next release with the new key
```

Devices that have not yet picked up the new public-key firmware will refuse manifests signed by the new private key. Plan the rotation in two release waves.

To bootstrap a fresh fork: generate a new keypair as above, replace the placeholder PEM in `main/include/release_pubkey.h`, add `MANIFEST_SIGNING_KEY` (contents of `manifest-signing.key`) as a repo secret. Never commit the private key.

## Reset options

- `Reset`: reboots the device and applies any saved SoftAP credential changes
- `Factory Reset`: clears the app settings namespace, then runs the Matter factory reset flow so pairing data and local settings are removed cleanly
- `Revert To Previous Firmware`: switches boot to the other OTA app slot without erasing settings

## Notes

- The web page stores settings in NVS, so they survive reboot.
- LED count, AP credentials, and effects stay local to the web UI. Matter and Apple Home sync power, color, and brightness.
- Each effect now has its own saved parameter set. Examples:
- `glow`: pulse speed, glow floor, pulse depth
- `rainbow`: drift speed, rainbow length, color blend, start offset, contrast
- `chase`: chase speed, tail length, tail sharpness
- `sparkle`: spark density, base glow, twinkle speed
- `wave`: wave speed, wavelength, wave depth
- The LED count set in the page is clamped to the compiled-in maximum.
- LED GPIO and maximum pixel count are build-time settings. Run `idf.py menuconfig`, navigate to **ESP32-C6 LED Web — Hardware**, and adjust `APP_LED_GPIO` / `APP_LED_MAX_PIXELS`. For a one-off override, put `CONFIG_APP_LED_GPIO=18` in `sdkconfig.local` (gitignored) before building. The default is GPIO 17, which is D7 on a XIAO ESP32C6.
- The serial monitor is the most reliable place to get the first Matter pairing codes after boot.

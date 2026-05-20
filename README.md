# ESP32-C6 Matter WS2812B Controller

This is a small ESP-IDF + ESP-Matter test project for an ESP32-C6 driving a WS2812B LED strip.

It exposes the strip in two ways:

- A local web page for LED count, color, brightness, effects, OTA, and reset actions
- A Matter extended color light endpoint for Apple Home
- A local OTA firmware upload flow from the same web page
- Automatic update checks against the latest published GitHub release
- A firmware revert action that boots back into the other OTA slot
- A factory reset action that clears Matter pairing, Wi-Fi AP config, and saved LED settings
- Built-in LED effects: `glow`, `rainbow`, `chase`, `sparkle`, `wave`
- Per-effect controls in the web UI with unique meanings for each animation

The firmware starts a Wi-Fi SoftAP, serves the control page directly from the board, and includes captive-portal style redirects plus a small DNS responder so phones and laptops are more likely to open the page automatically. After the device joins your normal Wi-Fi through Matter commissioning, the same web UI is also reachable on its LAN IP.

## Defaults

- SoftAP SSID/password: generated per device on first boot and stored in NVS
- Web UI: `http://192.168.4.1`
- LED data GPIO: `17` (`D7` on XIAO ESP32C6)
- Maximum strip length compiled in: `120`
- Matter device type: extended color light

## Wiring

- `XIAO ESP32C6 D7 / GPIO17` -> `DIN` on WS2812B strip
- `GND` -> strip ground
- External `5V` -> strip power
- Share ground between the ESP32-C6 and LED power supply

For anything beyond a few LEDs, use an external 5V supply.

## Build and flash

```bash
cd ~/esp32c6-led-web
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The first install still needs USB flashing. After that, you can upload new firmware from the web UI using the generated app binary:

```bash
~/esp32c6-led-web/build/esp32c6_led_web.bin
```

## Web UI layout

The device page is split into three tabs:

- `Overview`: Matter state, AP and LAN web UI URLs, firmware version, running slot, revert target
- `Configuration`: LED count, SoftAP SSID/password, auto-update toggle, OTA upload, published update check, revert button, factory reset, reboot
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

1. Build a new firmware image with `idf.py build`.
2. Open the device web UI at `http://192.168.4.1` or its LAN IP after commissioning.
3. In the `Firmware Update` section, choose `build/esp32c6_led_web.bin`.
4. Click `Install OTA Update`.
5. Wait for the board to reboot into the other OTA slot.

After at least one successful OTA update, the `Revert To Previous Firmware` button in `Configuration` can boot the device back into the other application slot.

This OTA path updates the application partition only. Bootloader and partition table changes still require USB flashing.

## Published auto-updates

The firmware watches the latest GitHub release for this repository (set via `CONFIG_APP_RELEASE_REPO`, default `alperbasarn/esp32c6-led-web`) and installs newer published builds once the device has Wi-Fi access.

Each release publishes a signed `manifest.json` to the GitHub release. The device:

1. Fetches `https://github.com/<repo>/releases/latest/download/manifest.json`.
2. Fetches `manifest.json.sig` (detached ECDSA P-256 DER signature over the manifest bytes).
3. Verifies the signature against the public key embedded in [`main/include/release_pubkey.h`](main/include/release_pubkey.h). Rejects the release if the signature is missing or invalid.
4. Compares `manifest.version` against the running firmware. Skips if not newer.
5. Cohort-gates: a stable hash of the device MAC % 100 must be less than `manifest.rollout.percent`. Devices outside the cohort surface an "available, waiting" status without installing.
6. Downloads the binary via `esp_https_ota`, then reads the freshly written OTA partition back and compares its SHA-256 against `manifest.app.sha256`. On mismatch the boot partition is reverted before reboot.
7. After reboot, a self-test task waits for Wi-Fi STA + the HTTP server to come up within `CONFIG_APP_UPDATE_SELF_TEST_TIMEOUT_S` and calls `esp_ota_mark_app_valid_cancel_rollback()`. If the deadline passes, the device rolls back automatically.

Each device's first poll is jittered randomly in `[0, CONFIG_APP_UPDATE_INITIAL_JITTER_MIN]` minutes after Wi-Fi up. The manual "Check Published Update Now" button bypasses the jitter.

### Publishing a release

1. Tag and publish a release on GitHub (`gh release create v1.5.0 --generate-notes`).
2. The `Publish Firmware` workflow builds the app, then runs [`scripts/generate-manifest.sh`](scripts/generate-manifest.sh) which produces `manifest.json` + `manifest.json.sig` and uploads:
   - `esp32c6_led_web.bin` — the OTA application image
   - `bootloader.bin`, `partition-table.bin` — needed for first-time USB flashes
   - `manifest.json`, `manifest.json.sig` — what devices read

The workflow is pinned to the `esp-matter` revision in `ESP_MATTER_REF`. If you upgrade the Matter stack, bump that env in [`.github/workflows/publish-firmware.yml`](.github/workflows/publish-firmware.yml) so CI matches local builds.

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

#!/usr/bin/env bash
# Enforces the one rule that makes main/model/ worth having: a pure model
# translation unit depends on nothing but the C++ standard library.
#
# The payoff of the MVC refactor is that model code compiles and runs on the
# host, so its behaviour can be pinned by tests in CI with no device attached.
# That property is easy to destroy by accident — one convenient #include of an
# esp_* header and the module is welded to the firmware again, silently, with
# the firmware build still green. This check makes that a red build instead of
# something a reviewer has to notice.
#
# Run with no arguments; exits non-zero and names the offending file and line.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
model_dir="$root/main/model"

if [ ! -d "$model_dir" ]; then
    echo "check_pure: no main/model directory; nothing to check"
    exit 0
fi

# Includes a pure model TU may never pull in.
FORBIDDEN_INCLUDE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](esp_|freertos/|driver/|nvs|soc/|hal/|sdkconfig|lwip/|mdns|esp-matter|app/|platform/|led_strip)'
# Identifiers that mean the TU has reached for firmware state or RTOS services.
FORBIDDEN_SYMBOL='(ESP_LOG[EWIDV]|esp_err_t|esp_timer_|esp_random|xTaskCreate|xSemaphore|vTaskDelay|portMAX_DELAY|nvs_open|chip::|esp_matter)'

# Strip // and /* */ comments while preserving line numbering, so a file is
# free to *describe* the rule (this one does) without tripping it, and so a
# reported line number still points at the real offending line.
strip_comments() {
    awk '
    {
        line = $0
        out = ""
        i = 1
        n = length(line)
        while (i <= n) {
            c = substr(line, i, 1)
            d = substr(line, i, 2)
            if (inblock) {
                if (d == "*/") { inblock = 0; i += 2 } else { i += 1 }
                continue
            }
            if (d == "/*") { inblock = 1; i += 2; continue }
            if (d == "//") { break }
            out = out c
            i += 1
        }
        print out
    }' "$1"
}

status=0
checked=0

while IFS= read -r -d '' f; do
    checked=$((checked + 1))
    rel="${f#"$root"/}"
    code="$(strip_comments "$f")"

    if hits=$(printf '%s\n' "$code" | grep -nE "$FORBIDDEN_INCLUDE"); then
        echo "check_pure: FORBIDDEN INCLUDE in $rel" >&2
        echo "$hits" | sed 's/^/    /' >&2
        echo "    main/model/ is compiled by test/host/run_tests.sh with plain g++." >&2
        echo "    Keep the ESP-IDF dependency in the caller and pass the value in." >&2
        status=1
    fi

    if hits=$(printf '%s\n' "$code" | grep -nE "$FORBIDDEN_SYMBOL"); then
        echo "check_pure: FORBIDDEN SYMBOL in $rel" >&2
        echo "$hits" | sed 's/^/    /' >&2
        echo "    A pure model is a map from arguments to results: no logging," >&2
        echo "    no RTOS, no NVS, no globals. Return a decision; let the caller act." >&2
        status=1
    fi
done < <(find "$model_dir" -type f \( -name '*.cpp' -o -name '*.h' \) -print0)

if [ "$status" -eq 0 ]; then
    echo "check_pure: OK — $checked file(s) in main/model/ are ESP-IDF-free"
fi
exit "$status"

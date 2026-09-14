#!/usr/bin/env bash
# Compiles and runs the host test suites with plain g++ — no ESP-IDF toolchain,
# no device, no flashing.
#
# Adding a module is a line of DATA in the MODULES table below, not a change to
# this script's logic. Each row is:
#
#     <name>|<needs_cjson>|<sources relative to the repo root, space separated>
#
# Only modules whose `needs_cjson` is 1 require an ESP-IDF checkout (cJSON is
# pulled straight out of the IDF tree, or from CJSON_DIR). Modules that need
# nothing beyond the standard library run anywhere, which is the point of
# main/model/ — see tools/check_pure.sh, which enforces that those translation
# units never acquire an ESP-IDF dependency in the first place.
#
#   ./test/host/run_tests.sh                    # every module it can build
#   ./test/host/run_tests.sh color_model        # just one
#   IDF_PATH=~/esp/esp-idf-5.4.1 ./test/host/run_tests.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/esp32c6-led-web-host-tests"

# name | needs_cjson | sources
MODULES=(
    "mqtt_proto|1|main/mqtt_proto.cpp"
    "color_model|0|main/model/color_model.cpp"
)

want="${1:-}"

cjson_dir="${CJSON_DIR:-}"
if [ -z "$cjson_dir" ]; then
    for candidate in "${IDF_PATH:-}" "$HOME/esp/esp-idf-5.4.1" "$HOME/esp/esp-idf"; do
        [ -n "$candidate" ] || continue
        if [ -f "$candidate/components/json/cJSON/cJSON.c" ]; then
            cjson_dir="$candidate/components/json/cJSON"
            break
        fi
    done
fi

mkdir -p "$out"
ran=0
skipped=0
failed=0

for row in "${MODULES[@]}"; do
    IFS='|' read -r name needs_cjson sources <<< "$row"
    [ -z "$want" ] || [ "$want" = "$name" ] || continue

    extra_inc=()
    extra_src=()
    if [ "$needs_cjson" = "1" ]; then
        if [ -z "$cjson_dir" ] || [ ! -f "$cjson_dir/cJSON.c" ]; then
            echo "SKIP $name (needs cJSON; set IDF_PATH or CJSON_DIR)" >&2
            skipped=$((skipped + 1))
            continue
        fi
        extra_inc=(-I "$cjson_dir")
        extra_src=("$cjson_dir/cJSON.c")
    fi

    # shellcheck disable=SC2206
    srcs=($sources)
    prefixed=()
    for s in "${srcs[@]}"; do prefixed+=("$root/$s"); done

    echo "=== $name ==="
    g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -I "$root/main" "${extra_inc[@]+"${extra_inc[@]}"}" \
        "$here/test_$name.cpp" "${prefixed[@]}" "${extra_src[@]+"${extra_src[@]}"}" \
        -o "$out/test_$name"

    if "$out/test_$name"; then
        ran=$((ran + 1))
    else
        failed=$((failed + 1))
    fi
done

if [ -n "$want" ] && [ "$ran" -eq 0 ] && [ "$skipped" -eq 0 ] && [ "$failed" -eq 0 ]; then
    echo "no such module: $want" >&2
    exit 2
fi

echo "host tests: $ran passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]

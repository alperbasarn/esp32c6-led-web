#!/usr/bin/env bash
# Compiles and runs the host tests for main/mqtt_proto.cpp with plain g++.
#
# The protocol layer has no ESP-IDF dependency beyond cJSON, which is pulled
# straight out of the IDF tree (or from CJSON_DIR if you keep it elsewhere).
#
#   ./test/host/run_tests.sh
#   IDF_PATH=~/esp/esp-idf-5.4.1 ./test/host/run_tests.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/esp32c6-led-web-host-tests"

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

if [ -z "$cjson_dir" ] || [ ! -f "$cjson_dir/cJSON.c" ]; then
    echo "cJSON sources not found. Set IDF_PATH to an ESP-IDF checkout or CJSON_DIR to a cJSON source directory." >&2
    exit 1
fi

mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I "$root/main" -I "$cjson_dir" \
    "$here/test_mqtt_proto.cpp" "$root/main/mqtt_proto.cpp" "$cjson_dir/cJSON.c" \
    -o "$out/test_mqtt_proto"

"$out/test_mqtt_proto"

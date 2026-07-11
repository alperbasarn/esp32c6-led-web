#!/usr/bin/env bash
# Produces a signed release manifest for the device's OTA flow.
#
# Inputs (env):
#   RELEASE_TAG     — e.g. "v1.5.0" or "1.5.0"
#   REPO_SLUG       — "owner/name", typically $GITHUB_REPOSITORY
#   IDF_REF         — e.g. "v5.4.1"
#   MATTER_REF      — e.g. "c6f7672…"
#   APP_BIN         — path to esp32c6_led_web.bin
#   APP_VERSION     — optional version from the built app; when set, it must
#                      match RELEASE_TAG after an optional leading "v" is removed
#   SIGNING_KEY_PEM — contents of the ECDSA P-256 private key in PEM form
#                     (typically from a repo secret)
#   ROLLOUT_PERCENT — optional staged-rollout percentage (integer 0..100);
#                     defaults to 100. Set to 0 to halt an in-flight release.
# Outputs (current dir):
#   manifest.json
#   manifest.json.sig   (raw DER-encoded ECDSA signature over the manifest bytes)

set -euo pipefail

: "${RELEASE_TAG:?must be set}"
: "${REPO_SLUG:?must be set}"
: "${APP_BIN:?must be set}"

if [ -z "${SIGNING_KEY_PEM:-}" ]; then
  echo "ERROR: SIGNING_KEY_PEM must be set for a release manifest." >&2
  exit 1
fi

if [[ ! "$RELEASE_TAG" =~ ^v?[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z-]+)*$ ]]; then
  echo "ERROR: RELEASE_TAG must be a version tag such as v1.6 or 1.6.0-rc.1: $RELEASE_TAG" >&2
  exit 1
fi

if [[ ! "$REPO_SLUG" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
  echo "ERROR: REPO_SLUG must have the form owner/name: $REPO_SLUG" >&2
  exit 1
fi

if [ ! -s "$APP_BIN" ]; then
  echo "APP_BIN not found: $APP_BIN" >&2
  exit 1
fi

rollout_percent="${ROLLOUT_PERCENT:-100}"
if [[ ! "$rollout_percent" =~ ^[0-9]+$ ]] || [ "$rollout_percent" -lt 0 ] || [ "$rollout_percent" -gt 100 ]; then
  echo "ERROR: ROLLOUT_PERCENT must be an integer from 0 through 100: $rollout_percent" >&2
  exit 1
fi

version="${RELEASE_TAG#v}"
if [ -n "${APP_VERSION:-}" ] && [ "$version" != "$APP_VERSION" ]; then
  echo "ERROR: release version $version does not match built app version $APP_VERSION" >&2
  exit 1
fi

for required_command in sha256sum stat openssl; do
  if ! command -v "$required_command" >/dev/null 2>&1; then
    echo "ERROR: required command not found: $required_command" >&2
    exit 1
  fi
done

sha256=$(sha256sum "$APP_BIN" | awk '{print $1}')
size=$(stat -c%s "$APP_BIN")
asset_name=$(basename "$APP_BIN")
asset_url="https://github.com/${REPO_SLUG}/releases/download/${RELEASE_TAG}/${asset_name}"

python_bin="${PYTHON_BIN:-}"
if [ -z "$python_bin" ]; then
  if command -v python3 >/dev/null 2>&1; then
    python_bin=python3
  elif command -v python >/dev/null 2>&1; then
    python_bin=python
  else
    echo "ERROR: python3 or python is required to write manifest.json safely." >&2
    exit 1
  fi
fi

"$python_bin" - "$version" "$REPO_SLUG" "$asset_url" "${IDF_REF:-}" "${MATTER_REF:-}" \
  "$sha256" "$size" "$rollout_percent" <<'PY'
import datetime
import json
import sys

version, repo_slug, asset_url, idf_ref, matter_ref, sha256, size, rollout_percent = sys.argv[1:]

manifest = {
    "version": version,
    "chip": "esp32c6",
    "released_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "idf_ref": idf_ref,
    "matter_ref": matter_ref,
    "app": {
        "url": asset_url,
        "sha256": sha256,
        "size": int(size),
    },
    "rollout": {
        "percent": int(rollout_percent),
        "cohorts": ["*"],
    },
}

with open("manifest.json", "w", encoding="utf-8", newline="\n") as output:
    json.dump(manifest, output, indent=2)
    output.write("\n")
PY

echo "manifest.json:" >&2
cat manifest.json >&2

tmpkey=$(mktemp)
tmppubkey=$(mktemp)
trap 'rm -f "$tmpkey" "$tmppubkey"' EXIT
printf '%s' "$SIGNING_KEY_PEM" > "$tmpkey"

if ! openssl pkey -in "$tmpkey" -check -noout >/dev/null 2>&1; then
  echo "ERROR: SIGNING_KEY_PEM is not a valid EC private key." >&2
  exit 1
fi

curve=$(openssl pkey -in "$tmpkey" -text -noout 2>/dev/null | awk -F': ' '/ASN1 OID/ { print $2; exit }')
if [ "$curve" != "prime256v1" ]; then
  echo "ERROR: SIGNING_KEY_PEM must use the ECDSA P-256 (prime256v1) curve; got: ${curve:-unknown}" >&2
  exit 1
fi

# ECDSA P-256 + SHA-256, DER-encoded signature.
openssl dgst -sha256 -sign "$tmpkey" -out manifest.json.sig manifest.json
openssl pkey -in "$tmpkey" -pubout -out "$tmppubkey" >/dev/null 2>&1
openssl dgst -sha256 -verify "$tmppubkey" -signature manifest.json.sig manifest.json >/dev/null
echo "manifest.json.sig: $(stat -c%s manifest.json.sig) bytes" >&2

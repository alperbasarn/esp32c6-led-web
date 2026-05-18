#!/usr/bin/env bash
# Produces a signed release manifest for the device's OTA flow.
#
# Inputs (env):
#   RELEASE_TAG     — e.g. "v1.5.0" or "1.5.0"
#   REPO_SLUG       — "owner/name", typically $GITHUB_REPOSITORY
#   IDF_REF         — e.g. "v5.4.1"
#   MATTER_REF      — e.g. "c6f7672…"
#   APP_BIN         — path to esp32c6_led_web.bin
#   SIGNING_KEY_PEM — contents of the ECDSA P-256 private key in PEM form
#                     (typically from a repo secret)
# Outputs (current dir):
#   manifest.json
#   manifest.json.sig   (raw DER-encoded ECDSA signature over the manifest bytes)

set -euo pipefail

: "${RELEASE_TAG:?must be set}"
: "${REPO_SLUG:?must be set}"
: "${APP_BIN:?must be set}"

if [ ! -f "$APP_BIN" ]; then
  echo "APP_BIN not found: $APP_BIN" >&2
  exit 1
fi

version="${RELEASE_TAG#v}"
sha256=$(sha256sum "$APP_BIN" | awk '{print $1}')
size=$(stat -c%s "$APP_BIN")
asset_name=$(basename "$APP_BIN")
released_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
asset_url="https://github.com/${REPO_SLUG}/releases/download/${RELEASE_TAG}/${asset_name}"

cat > manifest.json <<EOF
{
  "version": "${version}",
  "chip": "esp32c6",
  "released_at": "${released_at}",
  "idf_ref": "${IDF_REF:-}",
  "matter_ref": "${MATTER_REF:-}",
  "app": {
    "url": "${asset_url}",
    "sha256": "${sha256}",
    "size": ${size}
  },
  "rollout": {
    "percent": 100,
    "cohorts": ["*"]
  }
}
EOF

echo "manifest.json:" >&2
cat manifest.json >&2

if [ -n "${SIGNING_KEY_PEM:-}" ]; then
  tmpkey=$(mktemp)
  trap 'rm -f "$tmpkey"' EXIT
  printf '%s' "$SIGNING_KEY_PEM" > "$tmpkey"
  # ECDSA P-256 + SHA-256, DER-encoded signature.
  openssl dgst -sha256 -sign "$tmpkey" -out manifest.json.sig manifest.json
  echo "manifest.json.sig: $(stat -c%s manifest.json.sig) bytes" >&2
else
  echo "WARNING: SIGNING_KEY_PEM not set — no manifest.json.sig produced." >&2
  echo "Devices with CONFIG_APP_OTA_SIG_VERIFY=y will refuse this release." >&2
fi

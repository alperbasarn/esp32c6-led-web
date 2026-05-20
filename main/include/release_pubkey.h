// SPDX-License-Identifier: Apache-2.0
//
// ECDSA P-256 public key for manifest signature verification.
//
// To regenerate this keypair:
//   openssl ecparam -name prime256v1 -genkey -noout -out manifest-signing.key
//   openssl ec -in manifest-signing.key -pubout -outform PEM > release_pubkey.pem
//
// Then paste the contents of release_pubkey.pem into kReleasePubKeyPem below.
//
// IMPORTANT: the matching PRIVATE key (manifest-signing.key) must be uploaded
// as the `MANIFEST_SIGNING_KEY` repository secret. NEVER commit the private
// key — keep it offline or, at most, in the GitHub secret store.
//
// If kReleasePubKeyPem is empty *and* CONFIG_APP_OTA_SIG_VERIFY=n, signature
// verification is skipped (with a runtime warning). Production builds must
// have both a real key here AND CONFIG_APP_OTA_SIG_VERIFY=y.

#pragma once

#include <stddef.h>

// PEM-encoded ECDSA P-256 public key, NUL-terminated.
static const char kReleasePubKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAERfgYAcJAW5VI7/4pE/QqAiJXeziR\n"
    "Lzj031sPE+goALYzPDlwgP25hpnS7Hj3aSj9W/UA3JhHXL9ieBXFNQCzQQ==\n"
    "-----END PUBLIC KEY-----\n";
static const size_t kReleasePubKeyPemLen = sizeof(kReleasePubKeyPem) - 1;

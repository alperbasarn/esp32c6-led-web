// Wi-Fi diagnostic text — pure model, no ESP-IDF.
//
// Part of main/model/: compiled both into the firmware and by the host suite
// with plain g++, so it may not include an esp_* header or touch a global.
// tools/check_pure.sh enforces that.
//
// The reason codes here are the numeric values ESP-IDF's wifi_err_reason_t
// reports. They are written as plain integers deliberately: including
// esp_wifi_types.h to name them would weld this table to the SDK and cost it
// its host-testability, for no behavioural gain. The mapping is pinned by
// test/host/test_net_model.cpp against a frozen copy of the original.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Short, stable slug for a Wi-Fi disconnect reason, for the Wi-Fi State card
// and the /api/state document. Never returns nullptr: an unmapped code yields
// "unknown", so callers may use the result directly.
//
// 17 codes are mapped; with the "unknown" fallback that is 18 distinct
// strings. The count is asserted in the host test so the table cannot shrink
// unnoticed.
const char *net_wifi_disconnect_reason_text(uint16_t reason);

#ifdef __cplusplus
}
#endif

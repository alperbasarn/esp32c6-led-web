// Matter colour-space conversions — pure model, no ESP-IDF.
//
// This translation unit is part of main/model/ and is compiled BOTH into the
// firmware and by test/host/run_tests.sh with plain g++. It must therefore
// never include an esp_*, freertos/, driver/, nvs_ or esp_matter header, and
// must never touch a global: every function here is a pure map from its
// arguments to its out-parameters. tools/check_pure.sh enforces the include
// rule in CI, so an accidental ESP-IDF include is a red build, not a review
// comment.
//
// The conversions moved here verbatim from app_main.cpp. Equivalence with the
// pre-move implementation is pinned by test/host/test_color_model.cpp, which
// carries a frozen copy of the original code and compares against it across
// the full input domain.
//
// Out-parameters are all optional: passing nullptr for a channel skips it,
// matching the original call sites.
#pragma once

#include <stdint.h>

// The Matter ColorControl CurrentX/CurrentY reported before any colour command
// has been received. Also the fallback used when an RGB triple is too dark to
// yield a meaningful chromaticity.
//
// constexpr rather than `extern const` deliberately: these seed file-scope
// statics in app_main.cpp, and an extern would make that dynamic
// initialisation with a cross-translation-unit ordering dependency.
#ifdef __cplusplus
constexpr uint16_t kColorDefaultCurrentX = 0x616b;
constexpr uint16_t kColorDefaultCurrentY = 0x607d;
#else
#define kColorDefaultCurrentX ((uint16_t) 0x616b)
#define kColorDefaultCurrentY ((uint16_t) 0x607d)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- RGB -> Matter ---------------------------------------------------------

// 8-bit RGB to the Matter hue/saturation pair (both on the 0..254 scale that
// ColorControl uses, NOT 0..255).
void color_rgb_to_matter_hs(uint8_t red, uint8_t green, uint8_t blue,
                            uint8_t *matter_hue, uint8_t *matter_saturation);

// 8-bit RGB to CIE xyY chromaticity as Matter CurrentX/CurrentY (0..65535).
// An input that sums to ~zero falls back to kColorDefaultCurrentX/Y rather
// than dividing by zero.
void color_rgb_to_matter_xy(uint8_t red, uint8_t green, uint8_t blue,
                            uint16_t *current_x, uint16_t *current_y);

// --- Matter -> RGB ---------------------------------------------------------

// Matter hue/saturation (0..254) to 8-bit RGB at full value.
void color_matter_hs_to_rgb(uint8_t matter_hue, uint8_t matter_saturation,
                            uint8_t *red, uint8_t *green, uint8_t *blue);

// Matter CurrentX/CurrentY (0..65535) to 8-bit RGB, via XYZ, sRGB gamma and a
// normalising pass that scales back any channel driven above 1.0. A y at or
// below 0.0001 yields white rather than a division by zero.
void color_matter_xy_to_rgb(uint16_t current_x, uint16_t current_y,
                            uint8_t *red, uint8_t *green, uint8_t *blue);

// Matter ColorTemperatureMireds to 8-bit RGB (Tanner Helland approximation).
// mireds is clamped away from zero, and the derived temperature is clamped to
// 1000..40000 K before the piecewise fit is applied.
void color_temp_mireds_to_rgb(uint16_t mireds,
                              uint8_t *red, uint8_t *green, uint8_t *blue);

#ifdef __cplusplus
}
#endif

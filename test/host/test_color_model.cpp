// Characterization tests for main/model/color_model.cpp.
//
// These conversions were moved out of app_main.cpp during the MVC refactor.
// A Matter controller sees their output directly, so "no observable change"
// has to be proven rather than asserted.
//
// The `reference` namespace below is a FROZEN, verbatim copy of the five
// functions as they stood in app_main.cpp immediately before the move (dev @
// 82c1c36, lines 2028-2226). Do not "clean it up" and do not re-sync it with
// color_model.cpp — its whole value is that it is an independent snapshot of
// the old behaviour. Every test here asserts that the extracted module agrees
// with it exactly, over the full or a densely sampled input domain.
//
// If a future change to color_model.cpp is a deliberate behaviour change, that
// is a Matter-visible change: update the reference AND say so in the commit.

#include "model/color_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace reference {

// ---- frozen snapshot of app_main.cpp, pre-extraction -----------------------

static constexpr uint16_t kDefaultCurrentX = 0x616b;
static constexpr uint16_t kDefaultCurrentY = 0x607d;

static inline uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

static inline uint16_t clamp_u16(int value, int min_value, int max_value)
{
    return static_cast<uint16_t>(std::clamp(value, min_value, max_value));
}

static void rgb_to_matter_hs(uint8_t red, uint8_t green, uint8_t blue, uint8_t *matter_hue, uint8_t *matter_saturation)
{
    double r = static_cast<double>(red) / 255.0;
    double g = static_cast<double>(green) / 255.0;
    double b = static_cast<double>(blue) / 255.0;

    double max_value = std::max({r, g, b});
    double min_value = std::min({r, g, b});
    double delta = max_value - min_value;

    double hue = 0.0;
    if (delta > 0.0) {
        if (max_value == r) {
            hue = 60.0 * std::fmod((g - b) / delta, 6.0);
        } else if (max_value == g) {
            hue = 60.0 * (((b - r) / delta) + 2.0);
        } else {
            hue = 60.0 * (((r - g) / delta) + 4.0);
        }
    }
    if (hue < 0.0) {
        hue += 360.0;
    }

    double saturation = max_value <= 0.0 ? 0.0 : (delta / max_value);
    if (matter_hue) {
        *matter_hue = clamp_u8(static_cast<int>(std::lround((hue / 360.0) * 254.0)));
    }
    if (matter_saturation) {
        *matter_saturation = clamp_u8(static_cast<int>(std::lround(saturation * 254.0)));
    }
}

static void matter_hs_to_rgb(uint8_t matter_hue, uint8_t matter_saturation, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    double hue = (static_cast<double>(matter_hue) / 254.0) * 360.0;
    double saturation = static_cast<double>(matter_saturation) / 254.0;
    double value = 1.0;
    double chroma = value * saturation;
    double x = chroma * (1.0 - std::fabs(std::fmod(hue / 60.0, 2.0) - 1.0));
    double match = value - chroma;
    double rf = 0.0;
    double gf = 0.0;
    double bf = 0.0;

    if (hue < 60.0) {
        rf = chroma;
        gf = x;
    } else if (hue < 120.0) {
        rf = x;
        gf = chroma;
    } else if (hue < 180.0) {
        gf = chroma;
        bf = x;
    } else if (hue < 240.0) {
        gf = x;
        bf = chroma;
    } else if (hue < 300.0) {
        rf = x;
        bf = chroma;
    } else {
        rf = chroma;
        bf = x;
    }

    if (red) {
        *red = clamp_u8(static_cast<int>(std::lround((rf + match) * 255.0)));
    }
    if (green) {
        *green = clamp_u8(static_cast<int>(std::lround((gf + match) * 255.0)));
    }
    if (blue) {
        *blue = clamp_u8(static_cast<int>(std::lround((bf + match) * 255.0)));
    }
}

static void matter_xy_to_rgb(uint16_t current_x, uint16_t current_y, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    double x = static_cast<double>(current_x) / 65535.0;
    double y = static_cast<double>(current_y) / 65535.0;
    if (y <= 0.0001) {
        if (red) {
            *red = 255;
        }
        if (green) {
            *green = 255;
        }
        if (blue) {
            *blue = 255;
        }
        return;
    }

    double z = std::max(0.0, 1.0 - x - y);
    double luminance = 1.0;
    double X = (luminance / y) * x;
    double Y = luminance;
    double Z = (luminance / y) * z;

    double rf = X * 1.656492 - Y * 0.354851 - Z * 0.255038;
    double gf = -X * 0.707196 + Y * 1.655397 + Z * 0.036152;
    double bf = X * 0.051713 - Y * 0.121364 + Z * 1.011530;

    rf = std::max(0.0, rf);
    gf = std::max(0.0, gf);
    bf = std::max(0.0, bf);

    auto gamma_correct = [](double value) {
        if (value <= 0.0031308) {
            return 12.92 * value;
        }
        return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    };

    rf = gamma_correct(rf);
    gf = gamma_correct(gf);
    bf = gamma_correct(bf);

    double max_value = std::max({rf, gf, bf});
    if (max_value > 1.0) {
        rf /= max_value;
        gf /= max_value;
        bf /= max_value;
    }

    if (red) {
        *red = clamp_u8(static_cast<int>(std::lround(rf * 255.0)));
    }
    if (green) {
        *green = clamp_u8(static_cast<int>(std::lround(gf * 255.0)));
    }
    if (blue) {
        *blue = clamp_u8(static_cast<int>(std::lround(bf * 255.0)));
    }
}

static void rgb_to_matter_xy(uint8_t red, uint8_t green, uint8_t blue, uint16_t *current_x, uint16_t *current_y)
{
    auto to_linear = [](uint8_t value) {
        double srgb = static_cast<double>(value) / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
    };

    double r = to_linear(red);
    double g = to_linear(green);
    double b = to_linear(blue);
    double x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375;
    double y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750;
    double z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041;
    double sum = x + y + z;

    if (sum <= 0.000001) {
        x = static_cast<double>(kDefaultCurrentX) / 65535.0;
        y = static_cast<double>(kDefaultCurrentY) / 65535.0;
        sum = 1.0;
    }

    if (current_x) {
        *current_x = clamp_u16(static_cast<int>(std::lround((x / sum) * 65535.0)), 0, 65535);
    }
    if (current_y) {
        *current_y = clamp_u16(static_cast<int>(std::lround((y / sum) * 65535.0)), 0, 65535);
    }
}

static void color_temp_to_rgb(uint16_t mireds, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    double kelvin = 1000000.0 / std::max<uint16_t>(mireds, 1);
    double temp = std::clamp(kelvin / 100.0, 10.0, 400.0);

    double rf;
    double gf;
    double bf;

    if (temp <= 66.0) {
        rf = 255.0;
        gf = 99.4708025861 * std::log(temp) - 161.1195681661;
        if (temp <= 19.0) {
            bf = 0.0;
        } else {
            bf = 138.5177312231 * std::log(temp - 10.0) - 305.0447927307;
        }
    } else {
        rf = 329.698727446 * std::pow(temp - 60.0, -0.1332047592);
        gf = 288.1221695283 * std::pow(temp - 60.0, -0.0755148492);
        bf = 255.0;
    }

    if (red) {
        *red = clamp_u8(static_cast<int>(std::lround(rf)));
    }
    if (green) {
        *green = clamp_u8(static_cast<int>(std::lround(gf)));
    }
    if (blue) {
        *blue = clamp_u8(static_cast<int>(std::lround(bf)));
    }
}

}  // namespace reference

// ---- harness ---------------------------------------------------------------

static long g_checks = 0;
static long g_failures = 0;

static void fail(const char *what, const char *detail)
{
    if (g_failures < 10) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, detail);
    }
    ++g_failures;
}

#define EXPECT_EQ_U(what, got, want, detail)                 \
    do {                                                     \
        ++g_checks;                                          \
        if ((got) != (want)) {                               \
            char buf[256];                                   \
            std::snprintf(buf, sizeof(buf), "%s got=%d want=%d", (detail), (int) (got), (int) (want)); \
            fail((what), buf);                               \
        }                                                    \
    } while (0)

// ---- equivalence tests -----------------------------------------------------

// Exhaustive: all 256x256 Matter hue/saturation pairs.
static void test_hs_to_rgb_exhaustive()
{
    for (int h = 0; h <= 255; ++h) {
        for (int s = 0; s <= 255; ++s) {
            uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
            reference::matter_hs_to_rgb((uint8_t) h, (uint8_t) s, &r0, &g0, &b0);
            color_matter_hs_to_rgb((uint8_t) h, (uint8_t) s, &r1, &g1, &b1);
            char d[64];
            std::snprintf(d, sizeof(d), "hue=%d sat=%d", h, s);
            EXPECT_EQ_U("hs_to_rgb.r", r1, r0, d);
            EXPECT_EQ_U("hs_to_rgb.g", g1, g0, d);
            EXPECT_EQ_U("hs_to_rgb.b", b1, b0, d);
        }
    }
}

// Exhaustive: every representable mireds value, including 0 (the divide guard).
static void test_color_temp_exhaustive()
{
    for (int m = 0; m <= 65535; ++m) {
        uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
        reference::color_temp_to_rgb((uint16_t) m, &r0, &g0, &b0);
        color_temp_mireds_to_rgb((uint16_t) m, &r1, &g1, &b1);
        char d[64];
        std::snprintf(d, sizeof(d), "mireds=%d", m);
        EXPECT_EQ_U("temp.r", r1, r0, d);
        EXPECT_EQ_U("temp.g", g1, g0, d);
        EXPECT_EQ_U("temp.b", b1, b0, d);
    }
}

// Dense grid over CurrentX/CurrentY, including the y<=0.0001 white branch.
static void test_xy_to_rgb_grid()
{
    for (int xi = 0; xi <= 65535; xi += 257) {
        for (int yi = 0; yi <= 65535; yi += 257) {
            uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
            reference::matter_xy_to_rgb((uint16_t) xi, (uint16_t) yi, &r0, &g0, &b0);
            color_matter_xy_to_rgb((uint16_t) xi, (uint16_t) yi, &r1, &g1, &b1);
            char d[64];
            std::snprintf(d, sizeof(d), "x=%d y=%d", xi, yi);
            EXPECT_EQ_U("xy_to_rgb.r", r1, r0, d);
            EXPECT_EQ_U("xy_to_rgb.g", g1, g0, d);
            EXPECT_EQ_U("xy_to_rgb.b", b1, b0, d);
        }
    }
}

// Dense RGB grid for both RGB->Matter directions, plus exhaustive greys and
// exhaustive single-channel ramps (where the hue branch selection flips).
static void test_rgb_to_matter_grid()
{
    auto check = [](int r, int g, int b) {
        uint8_t h0 = 0, s0 = 0, h1 = 0, s1 = 0;
        reference::rgb_to_matter_hs((uint8_t) r, (uint8_t) g, (uint8_t) b, &h0, &s0);
        color_rgb_to_matter_hs((uint8_t) r, (uint8_t) g, (uint8_t) b, &h1, &s1);
        uint16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        reference::rgb_to_matter_xy((uint8_t) r, (uint8_t) g, (uint8_t) b, &x0, &y0);
        color_rgb_to_matter_xy((uint8_t) r, (uint8_t) g, (uint8_t) b, &x1, &y1);
        char d[64];
        std::snprintf(d, sizeof(d), "rgb=%d,%d,%d", r, g, b);
        EXPECT_EQ_U("rgb_to_hs.hue", h1, h0, d);
        EXPECT_EQ_U("rgb_to_hs.sat", s1, s0, d);
        EXPECT_EQ_U("rgb_to_xy.x", x1, x0, d);
        EXPECT_EQ_U("rgb_to_xy.y", y1, y0, d);
    };

    for (int r = 0; r <= 255; r += 5) {
        for (int g = 0; g <= 255; g += 5) {
            for (int b = 0; b <= 255; b += 5) {
                check(r, g, b);
            }
        }
    }
    for (int v = 0; v <= 255; ++v) {
        check(v, v, v);  // greys, incl. the sum<=1e-6 fallback at 0,0,0
        check(v, 0, 0);
        check(0, v, 0);
        check(0, 0, v);
        check(255, v, 0);
        check(0, 255, v);
        check(v, 0, 255);
    }
}

// The nullptr-tolerance the original call sites rely on.
static void test_null_out_params()
{
    color_matter_hs_to_rgb(120, 200, nullptr, nullptr, nullptr);
    color_matter_xy_to_rgb(20000, 30000, nullptr, nullptr, nullptr);
    color_temp_mireds_to_rgb(370, nullptr, nullptr, nullptr);
    color_rgb_to_matter_hs(10, 20, 30, nullptr, nullptr);
    color_rgb_to_matter_xy(10, 20, 30, nullptr, nullptr);
    ++g_checks;  // reaching here without a crash is the assertion
}

// The defaults exported by the header must match the frozen originals.
static void test_default_xy_constants()
{
    EXPECT_EQ_U("kColorDefaultCurrentX", kColorDefaultCurrentX, reference::kDefaultCurrentX, "const");
    EXPECT_EQ_U("kColorDefaultCurrentY", kColorDefaultCurrentY, reference::kDefaultCurrentY, "const");
}

int main()
{
    test_default_xy_constants();
    test_hs_to_rgb_exhaustive();
    test_color_temp_exhaustive();
    test_xy_to_rgb_grid();
    test_rgb_to_matter_grid();
    test_null_out_params();

    std::printf("color_model: %ld checks, %ld failures\n", g_checks, g_failures);
    if (g_failures > 10) {
        std::fprintf(stderr, "(%ld further failures suppressed)\n", g_failures - 10);
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

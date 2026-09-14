// Matter colour-space conversions. See color_model.h for the contract.
//
// Moved verbatim from app_main.cpp; the arithmetic is deliberately unchanged,
// including the exact constant literals and the lround/clamp ordering, because
// any drift here is directly visible to a Matter controller. Equivalence is
// pinned exhaustively by test/host/test_color_model.cpp.
//
// Pure model TU: standard library only, no ESP-IDF headers, no globals.

#include "color_model.h"

#include <algorithm>
#include <cmath>

namespace {

// Local copies of app_main.cpp's clamp helpers. Duplicated rather than shared
// so this TU keeps a zero-dependency surface; both are static inline and
// vanish into their call sites, so there is no symbol or size cost.
inline uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

inline uint16_t clamp_u16(int value, int min_value, int max_value)
{
    return static_cast<uint16_t>(std::clamp(value, min_value, max_value));
}

}  // namespace

void color_rgb_to_matter_hs(uint8_t red, uint8_t green, uint8_t blue,
                            uint8_t *matter_hue, uint8_t *matter_saturation)
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

void color_matter_hs_to_rgb(uint8_t matter_hue, uint8_t matter_saturation,
                            uint8_t *red, uint8_t *green, uint8_t *blue)
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

void color_matter_xy_to_rgb(uint16_t current_x, uint16_t current_y,
                            uint8_t *red, uint8_t *green, uint8_t *blue)
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

void color_rgb_to_matter_xy(uint8_t red, uint8_t green, uint8_t blue,
                            uint16_t *current_x, uint16_t *current_y)
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
        x = static_cast<double>(kColorDefaultCurrentX) / 65535.0;
        y = static_cast<double>(kColorDefaultCurrentY) / 65535.0;
        sum = 1.0;
    }

    if (current_x) {
        *current_x = clamp_u16(static_cast<int>(std::lround((x / sum) * 65535.0)), 0, 65535);
    }
    if (current_y) {
        *current_y = clamp_u16(static_cast<int>(std::lround((y / sum) * 65535.0)), 0, 65535);
    }
}

void color_temp_mireds_to_rgb(uint16_t mireds,
                              uint8_t *red, uint8_t *green, uint8_t *blue)
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

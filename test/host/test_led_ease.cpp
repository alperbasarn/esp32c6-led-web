// Host tests for main/model/led_ease.cpp.
//
// `reference` is the advance rule exactly as it stood inline in effect_task(),
// with the four function-static doubles turned into parameters -- that was the
// only way to call it at all. The order of operations is what matters here and
// is preserved verbatim: step all four channels, THEN test for settling against
// the stepped values, THEN snap.
//
// What this gets under test for the first time: the frame-delta cap (without
// which the first frame after an idle block jumps most of the way to the
// target, reinstating the hard step the easing exists to remove), the
// all-four-channels-together settle rule, and the fact that power is derived
// from displayed rather than target brightness so a power-off fade is not cut
// to black on its first frame.

#include "../../main/model/led_ease.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

static long g_checks = 0;
static long g_failures = 0;

static void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures < 20) {
            std::printf("FAIL  %s\n", what);
        }
    }
}

// ---------------------------------------------------------------------------
// Frozen pre-move rule, lifted from effect_task().
// ---------------------------------------------------------------------------
namespace reference {

static constexpr double kEaseTauMs = 220.0;
static constexpr double kEaseEpsilon = 0.5;

static double ease_alpha(double dt_ms, double tau_ms)
{
    if (tau_ms <= 0.0) {
        return 1.0;
    }
    return 1.0 - std::exp(-dt_ms / tau_ms);
}

static double ease_step(double cur, double tgt, double alpha)
{
    return cur + (tgt - cur) * alpha;
}

static bool advance(double *s_disp_brightness, double *s_disp_r, double *s_disp_g,
                    double *s_disp_b, double bri_target, double r_target,
                    double g_target, double b_target, double dt_ms)
{
    dt_ms = std::clamp(dt_ms, 1.0, 60.0);
    double alpha = ease_alpha(dt_ms, kEaseTauMs);
    *s_disp_brightness = ease_step(*s_disp_brightness, bri_target, alpha);
    *s_disp_r = ease_step(*s_disp_r, r_target, alpha);
    *s_disp_g = ease_step(*s_disp_g, g_target, alpha);
    *s_disp_b = ease_step(*s_disp_b, b_target, alpha);

    bool settled =
        std::fabs(*s_disp_brightness - bri_target) < kEaseEpsilon &&
        std::fabs(*s_disp_r - r_target) < kEaseEpsilon &&
        std::fabs(*s_disp_g - g_target) < kEaseEpsilon &&
        std::fabs(*s_disp_b - b_target) < kEaseEpsilon;
    if (settled) {
        *s_disp_brightness = bri_target;
        *s_disp_r = r_target;
        *s_disp_g = g_target;
        *s_disp_b = b_target;
    }
    return settled;
}

}  // namespace reference

// ---------------------------------------------------------------------------

static uint32_t g_seed = 0xb7e15163u;
static uint32_t rnd()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static double rnd255()
{
    return static_cast<double>(rnd() % 256001) / 1000.0;  // 0..256 with decimals
}

int main()
{
    // --- the two helpers, against the frozen copies -------------------------
    for (int i = 0; i < 40000; ++i) {
        double dt = static_cast<double>(rnd() % 500000) / 1000.0;
        double tau = static_cast<double>(rnd() % 2000);
        ++g_checks;
        if (led_ease_alpha(dt, tau) != reference::ease_alpha(dt, tau)) {
            ++g_failures;
            std::printf("FAIL  led_ease_alpha(%f,%f) diverged\n", dt, tau);
            break;
        }
        double cur = rnd255(), tgt = rnd255();
        double a = led_ease_alpha(dt, tau);
        ++g_checks;
        if (led_ease_step(cur, tgt, a) != reference::ease_step(cur, tgt, a)) {
            ++g_failures;
            std::printf("FAIL  led_ease_step diverged\n");
            break;
        }
    }

    // alpha's contract
    check(led_ease_alpha(0.0, 220.0) == 0.0, "zero elapsed time advances nothing");
    check(led_ease_alpha(10.0, 0.0) == 1.0, "tau 0 means jump straight to target");
    check(led_ease_alpha(10.0, -5.0) == 1.0, "negative tau also means jump");
    for (double dt = 0.0; dt <= 200.0; dt += 0.5) {
        double a = led_ease_alpha(dt, kLedEaseTauMs);
        check(a >= 0.0 && a <= 1.0, "alpha stays within [0,1]");
    }
    {
        // Monotonic in dt: a longer frame must never advance less.
        double prev = -1.0;
        bool monotonic = true;
        for (double dt = 0.0; dt <= 300.0; dt += 0.25) {
            double a = led_ease_alpha(dt, kLedEaseTauMs);
            if (a < prev) {
                monotonic = false;
            }
            prev = a;
        }
        check(monotonic, "alpha is monotonic in the frame delta");
    }
    // The documented time constant: 63% of a step in one tau, ~95% in three.
    check(std::fabs(led_ease_alpha(220.0, 220.0) - 0.6321) < 0.001,
          "one tau covers about 63% of the step");
    check(std::fabs(led_ease_alpha(660.0, 220.0) - 0.9502) < 0.001,
          "three tau covers about 95% of the step");

    // step never overshoots for alpha in [0,1]
    for (int i = 0; i < 20000; ++i) {
        double cur = rnd255(), tgt = rnd255();
        double a = static_cast<double>(rnd() % 1001) / 1000.0;
        double out = led_ease_step(cur, tgt, a);
        double lo = std::min(cur, tgt), hi = std::max(cur, tgt);
        check(out >= lo - 1e-9 && out <= hi + 1e-9,
              "a step never leaves the interval between current and target");
    }

    // --- the frame-delta cap -------------------------------------------------
    check(led_ease_clamp_dt(0.0) == kLedEaseMinDtMs, "a zero delta is raised to the floor");
    check(led_ease_clamp_dt(-50.0) == kLedEaseMinDtMs, "a negative delta is raised too");
    check(led_ease_clamp_dt(40.0) == 40.0, "a normal frame passes through untouched");
    check(led_ease_clamp_dt(60.0) == 60.0, "the bound itself passes through");
    check(led_ease_clamp_dt(1e9) == kLedEaseMaxDtMs, "an idle gap is capped");
    {
        // The reason the cap exists: after settling, the task blocks on
        // portMAX_DELAY, so the next measured delta is the whole idle gap. With
        // the cap, the first frame of a fresh fade is a small step. Without it,
        // the fade would be over before it was visible.
        led_ease_rgb_t cur{0.0, 0.0, 0.0, 0.0};
        const led_ease_rgb_t tgt{255.0, 255.0, 255.0, 255.0};
        led_ease_advance(&cur, &tgt, 600000.0, kLedEaseTauMs);  // ten minutes idle
        check(cur.brightness < 90.0,
              "the first frame after a long idle is still a small step, not a jump");
        double uncapped = led_ease_step(0.0, 255.0, led_ease_alpha(600000.0, kLedEaseTauMs));
        check(uncapped > 254.0,
              "...and without the cap that same frame would have arrived at the target");
    }

    // --- the composed advance rule, against the frozen inline version -------
    for (int trial = 0; trial < 6000; ++trial) {
        led_ease_rgb_t mine{rnd255(), rnd255(), rnd255(), rnd255()};
        led_ease_rgb_t tgt{rnd255(), rnd255(), rnd255(), rnd255()};
        double rb = mine.brightness, rr = mine.red, rg = mine.green, rbl = mine.blue;

        // Mix normal frames, idle gaps and degenerate deltas.
        static const double kDeltas[] = {0.0, 1.0, 13.0, 40.0, 60.0, 61.0, 5000.0, -3.0};
        double dt = kDeltas[trial % 8];

        bool mine_settled = led_ease_advance(&mine, &tgt, dt, kLedEaseTauMs);
        bool ref_settled = reference::advance(&rb, &rr, &rg, &rbl, tgt.brightness,
                                              tgt.red, tgt.green, tgt.blue, dt);
        ++g_checks;
        if (mine.brightness != rb || mine.red != rr || mine.green != rg ||
            mine.blue != rbl || mine_settled != ref_settled) {
            ++g_failures;
            if (g_failures < 20) {
                std::printf("FAIL  advance diverged at trial %d (dt=%f)\n", trial, dt);
            }
        }
    }

    // --- the settle test must run AFTER the step, not before ----------------
    // Found by mutation testing. Reordering the rule is invisible to random
    // trials: with alpha 0.166 at a 40 ms frame, the two orderings disagree only
    // when the pre-step difference lands in [0.5, 0.5997) -- a 0.1-wide window
    // that four independent random channels effectively never occupy at once.
    // Tested directly instead.
    {
        const led_ease_rgb_t tgt{100.0, 100.0, 100.0, 100.0};
        led_ease_rgb_t cur{100.58, 100.58, 100.58, 100.58};  // 0.58 out: inside the window
        check(std::fabs(cur.brightness - tgt.brightness) >= kLedEaseEpsilon,
              "the fixture starts OUTSIDE the snap threshold");
        bool settled = led_ease_advance(&cur, &tgt, 40.0, kLedEaseTauMs);
        check(settled,
              "a step that crosses the threshold settles on THIS frame, not the next");
        check(cur.brightness == tgt.brightness && cur.red == tgt.red &&
                  cur.green == tgt.green && cur.blue == tgt.blue,
              "...and snaps exactly, leaving no residue a later frame has to carry");
    }

    // --- the snap threshold is half a code, not a whole one -----------------
    // Also from mutation testing: widening the threshold only makes fades settle
    // marginally early, which no convergence test notices. A very large tau
    // makes alpha negligible, so the post-step difference is whatever was set
    // up -- which isolates the threshold from the step.
    {
        const double kFrozen = 1e9;  // alpha ~ 1e-9: the step is a no-op
        const led_ease_rgb_t tgt{100.0, 100.0, 100.0, 100.0};

        led_ease_rgb_t just_inside{100.49, 100.0, 100.0, 100.0};
        check(led_ease_advance(&just_inside, &tgt, 1.0, kFrozen),
              "0.49 of a code away counts as settled");

        led_ease_rgb_t just_outside{100.51, 100.0, 100.0, 100.0};
        check(!led_ease_advance(&just_outside, &tgt, 1.0, kFrozen),
              "0.51 of a code away does NOT count as settled");

        led_ease_rgb_t three_quarters{100.75, 100.0, 100.0, 100.0};
        check(!led_ease_advance(&three_quarters, &tgt, 1.0, kFrozen),
              "three quarters of a code is not settled -- the threshold is 0.5, not 1.0");

        // Each channel is held to the same threshold, not just brightness.
        for (int ch = 0; ch < 4; ++ch) {
            led_ease_rgb_t c{100.0, 100.0, 100.0, 100.0};
            double *field = ch == 0 ? &c.brightness : ch == 1 ? &c.red
                            : ch == 2 ? &c.green : &c.blue;
            *field = 100.75;
            check(!led_ease_advance(&c, &tgt, 1.0, kFrozen),
                  "every channel is held to the same snap threshold");
        }
    }

    // --- tau == 0 with dt == 0: the short-circuit prevents a NaN ------------
    // The third mutation testing found. For any positive dt, dropping the
    // tau <= 0 guard is harmless: -dt/0 is -inf, exp gives 0, and alpha is 1
    // either way. At dt == 0 it is NOT harmless -- 0/0 is NaN, and a NaN alpha
    // would poison every eased channel permanently, since NaN propagates
    // through the step and never compares within the snap threshold again.
    check(led_ease_alpha(0.0, 0.0) == 1.0, "alpha(0, 0) is 1, not NaN");
    check(!std::isnan(led_ease_alpha(0.0, 0.0)), "alpha(0, 0) is not NaN");
    check(!std::isnan(led_ease_alpha(0.0, -1.0)), "alpha(0, negative tau) is not NaN");
    {
        led_ease_rgb_t cur{10.0, 20.0, 30.0, 40.0};
        const led_ease_rgb_t tgt{200.0, 200.0, 200.0, 200.0};
        led_ease_advance(&cur, &tgt, 0.0, 0.0);
        check(!std::isnan(cur.brightness) && !std::isnan(cur.red) &&
                  !std::isnan(cur.green) && !std::isnan(cur.blue),
              "a degenerate frame never leaves NaN in the display state");
        // The eased state is a running value; one NaN would never recover.
        for (int i = 0; i < 5; ++i) {
            led_ease_advance(&cur, &tgt, 40.0, kLedEaseTauMs);
        }
        check(!std::isnan(cur.brightness), "and later frames stay finite");
    }

    // --- settling is all-or-nothing -----------------------------------------
    {
        const led_ease_rgb_t tgt{100.0, 100.0, 100.0, 100.0};
        // Three channels already there, one just outside the threshold.
        led_ease_rgb_t cur{100.0, 100.0, 100.0, 100.0 - 40.0};
        bool settled = led_ease_advance(&cur, &tgt, 1.0, kLedEaseTauMs);
        check(!settled, "one lagging channel keeps the whole fade in flight");
        check(cur.blue != tgt.blue, "...and that channel is not snapped");

        // All four inside the threshold: snap exactly, and report settled.
        led_ease_rgb_t near{100.4, 99.6, 100.2, 99.8};
        settled = led_ease_advance(&near, &tgt, 1.0, kLedEaseTauMs);
        check(settled, "all channels within half a code settles");
        check(near.brightness == tgt.brightness && near.red == tgt.red &&
                  near.green == tgt.green && near.blue == tgt.blue,
              "settling snaps exactly onto the target, leaving no residue");
    }

    // --- a fade actually converges, and then stays put -----------------------
    {
        led_ease_rgb_t cur{0.0, 0.0, 0.0, 0.0};
        const led_ease_rgb_t tgt{255.0, 200.0, 150.0, 100.0};
        int frames = 0;
        bool settled = false;
        while (!settled && frames < 10000) {
            settled = led_ease_advance(&cur, &tgt, 40.0, kLedEaseTauMs);
            ++frames;
        }
        check(settled, "a fade converges rather than creeping forever");
        check(frames < 100, "and converges in a plausible number of 40 ms frames");
        // Once settled it must stay settled, or the task would never idle.
        for (int i = 0; i < 10; ++i) {
            check(led_ease_advance(&cur, &tgt, 40.0, kLedEaseTauMs),
                  "a settled fade stays settled");
        }
    }

    // --- monotonic approach: no oscillation ---------------------------------
    {
        led_ease_rgb_t cur{0.0, 0.0, 0.0, 0.0};
        const led_ease_rgb_t tgt{255.0, 255.0, 255.0, 255.0};
        double prev = cur.brightness;
        bool monotonic = true;
        for (int i = 0; i < 200; ++i) {
            led_ease_advance(&cur, &tgt, 40.0, kLedEaseTauMs);
            if (cur.brightness < prev) {
                monotonic = false;
            }
            prev = cur.brightness;
        }
        check(monotonic, "a rising fade never dips");

        led_ease_rgb_t down{255.0, 255.0, 255.0, 255.0};
        const led_ease_rgb_t dark{0.0, 0.0, 0.0, 0.0};
        prev = down.brightness;
        monotonic = true;
        for (int i = 0; i < 200; ++i) {
            led_ease_advance(&down, &dark, 40.0, kLedEaseTauMs);
            if (down.brightness > prev) {
                monotonic = false;
            }
            prev = down.brightness;
        }
        check(monotonic, "a falling fade never rises");
    }

    // --- NULL handling: never claim settled ---------------------------------
    {
        led_ease_rgb_t cur{1.0, 2.0, 3.0, 4.0};
        const led_ease_rgb_t tgt{5.0, 6.0, 7.0, 8.0};
        check(!led_ease_advance(nullptr, &tgt, 40.0, kLedEaseTauMs),
              "NULL current does not report settled");
        check(!led_ease_advance(&cur, nullptr, 40.0, kLedEaseTauMs),
              "NULL target does not report settled");
        check(cur.brightness == 1.0, "a NULL target leaves the current values alone");
    }

    // --- power derived from DISPLAYED brightness ----------------------------
    check(!led_ease_power_from_brightness(0), "displayed brightness 0 is off");
    check(led_ease_power_from_brightness(1), "displayed brightness 1 is still on");
    check(led_ease_power_from_brightness(255), "full brightness is on");
    for (int v = 1; v <= 255; ++v) {
        check(led_ease_power_from_brightness((uint8_t) v),
              "every non-zero displayed brightness is on");
    }
    {
        // The whole point: a power-off fade must keep rendering frames until the
        // displayed brightness truly reaches zero. If power were derived from
        // the target it would read false on frame one and cut the strip to
        // black, discarding the fade.
        led_ease_rgb_t cur{255.0, 255.0, 255.0, 255.0};
        const led_ease_rgb_t off{0.0, 0.0, 0.0, 0.0};
        int lit_frames = 0;
        for (int i = 0; i < 500; ++i) {
            led_ease_advance(&cur, &off, 40.0, kLedEaseTauMs);
            uint8_t displayed = (uint8_t) std::clamp(std::lround(cur.brightness), 0L, 255L);
            if (led_ease_power_from_brightness(displayed)) {
                ++lit_frames;
            }
        }
        check(lit_frames > 5, "a power-off fade stays lit for several frames");
        check(cur.brightness == 0.0, "and ends at exactly zero, the settled off state");
    }

    std::printf("led_ease: %ld checks, %ld failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

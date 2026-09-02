#include "ClipProgressPolicy.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

namespace {
    [[nodiscard]] bool Near(float a_left, float a_right) {
        return std::fabs(a_left - a_right) < 0.0001f;
    }
}

int main() {
    using MTB::ClipProgressPolicy::ActivationSample;
    using MTB::ClipProgressPolicy::AdvanceAfterSyntheticStep;
    using MTB::ClipProgressPolicy::CaptureRemainingGraphSeconds;

    ActivationSample generator{
        .duration = 1.25f,
        .cropEnd = 0.10f,
        .localTime = 0.20f,
        .playbackSpeed = 2.0f,
    };

    // The activation hook is the last point at which the generator is known
    // to be alive. Pending state must be a scalar snapshot that remains valid
    // after the engine destroys or recycles that generator.
    const float captured = CaptureRemainingGraphSeconds(generator);
    generator = {
        .duration = -999.0f,
        .cropEnd = 999.0f,
        .localTime = 999.0f,
        .playbackSpeed = 0.0f,
    };
    CHECK(Near(captured, 0.475f));

    // Every synthetic graph step consumes the snapshot without consulting the
    // dead generator, and clamps at the transition boundary.
    CHECK(Near(AdvanceAfterSyntheticStep(captured, 1.0f / 30.0f),
               0.475f - (1.0f / 30.0f)));
    CHECK(Near(AdvanceAfterSyntheticStep(0.02f, 1.0f / 30.0f), 0.0f));

    // A replacement clip publishes a fresh snapshot. Accounting for the pump
    // step after UpdateAnimation intentionally consumes the replacement too:
    // at worst this holds one step early, never one step late across the idle.
    const float replacement = CaptureRemainingGraphSeconds({
        .duration = 0.90f,
        .cropEnd = 0.0f,
        .localTime = 0.0f,
        .playbackSpeed = 1.0f,
    });
    CHECK(Near(AdvanceAfterSyntheticStep(replacement, 1.0f / 30.0f),
               0.90f - (1.0f / 30.0f)));

    // Unreadable animation data fails closed at the boundary.
    CHECK(Near(CaptureRemainingGraphSeconds({}), 0.0f));
    CHECK(Near(CaptureRemainingGraphSeconds({
                   .duration = 1.0f,
                   .playbackSpeed = 0.0f,
               }),
               0.0f));

    if (g_failures == 0) {
        std::printf("clip progress policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

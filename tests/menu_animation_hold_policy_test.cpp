#include "MenuAnimationHoldPolicy.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

int main() {
    using MTB::MenuAnimationHoldPolicy::ShouldHold;

    // Existing arm-edge and live-transition holds remain intact.
    CHECK(ShouldHold({
        .armEdgeHeld = true,
    }));
    CHECK(ShouldHold({
        .freezeDrawSheathe = true,
        .equipClipInFlight = true,
    }));

    // Regression: once a paused-menu equip has occurred, do not resume the
    // graph later in the same menu and let it select an idle against stale
    // framework state. Selection belongs to the first live gameplay frame.
    CHECK(ShouldHold({
        .freezeDrawSheathe = true,
        .equipOccurredThisSession = true,
    }));

    CHECK(!ShouldHold({
        .freezeDrawSheathe = false,
        .equipOccurredThisSession = true,
    }));

    CHECK(!ShouldHold({}));

    // A rider is held because nothing steps the animal under her. Field
    // 2026-08-13: "the horse is not moving but the player is idle riding".
    CHECK(ShouldHold({
        .mounted = true,
    }));

    // ⚠ AND IT DOES NOT DEPEND ON THE EQUIP SETTINGS, which is the point of
    // putting it here rather than beside them. A rider holds with the
    // draw/sheathe freeze switched off and with no equip in the session,
    // because the reason is the statue underneath her, not a clip in flight.
    CHECK(ShouldHold({
        .freezeDrawSheathe = false,
        .equipClipInFlight = false,
        .equipOccurredThisSession = false,
        .mounted = true,
    }));

    // On foot nothing changes, so the standing menu keeps breathing.
    CHECK(!ShouldHold({
        .mounted = false,
    }));

    if (g_failures == 0) {
        std::printf("menu animation hold policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

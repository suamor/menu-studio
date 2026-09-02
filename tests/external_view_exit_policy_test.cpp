#include "ExternalViewExitPolicy.h"

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
    using namespace MTB::ExternalViewExitPolicy;

    // Regression: close-event listeners run serially. If Menu Studio snaps
    // inline and SPII restores its target afterward, current and target differ
    // for the first gameplay render and the camera visibly glides/snaps.
    const auto timing = ChooseDisarmTiming({
        .armed = true,
        .ownViewActive = false,
    });
    CHECK(timing == ReconcileTiming::kNextFrame);

    float currentZoom = 0.0f;
    float targetZoom = 0.0f;
    bool pending = false;

    if (timing == ReconcileTiming::kInline) {
        currentZoom = targetZoom;
    } else if (timing == ReconcileTiming::kNextFrame) {
        pending = true;
    }

    // A later MenuOpenClose sink restores SPII's pre-menu gameplay target.
    targetZoom = 1.0f;

    if (ShouldReconcile({
            .pending = pending,
            .menuOpen = false,
        })) {
        currentZoom = targetZoom;
    }
    CHECK(currentZoom == targetZoom);

    // SPII restores freeRotation but deliberately does not save/restore
    // currentYaw or targetYaw. SmoothCam owns live gameplay yaw after SPII
    // releases control. Writing targetYaw here therefore publishes the yaw
    // that was present on menu entry for one frame before SmoothCam corrects
    // it—the exact field symptom.
    float currentYaw = 1.0f;  // live gameplay/SmoothCam direction
    float targetYaw = 0.0f;   // stale direction captured on menu entry
    const auto fields = ChooseReconcileFields();
    CHECK(fields.zoom);
    CHECK(!fields.yaw);
    if (fields.yaw) {
        currentYaw = targetYaw;
    }
    CHECK(currentYaw == 1.0f);

    // A close followed by another menu open is a switch. Do not reconcile the
    // old menu's exit after the new menu provider has framed its camera.
    CHECK(!ShouldReconcile({
        .pending = true,
        .menuOpen = true,
    }));

    CHECK(ChooseDisarmTiming({
              .armed = false,
              .ownViewActive = false,
          }) == ReconcileTiming::kNone);
    CHECK(ChooseDisarmTiming({
              .armed = true,
              .ownViewActive = true,
          }) == ReconcileTiming::kNone);

    if (g_failures == 0) {
        std::printf("external view exit policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

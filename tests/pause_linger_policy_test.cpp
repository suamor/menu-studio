#include "PauseLingerPolicy.h"

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
    using namespace std::chrono_literals;
    using MTB::PauseLingerPolicy::Choose;
    using MTB::PauseLingerPolicy::ChooseClose;
    using MTB::PauseLingerPolicy::CloseDecision;
    using MTB::PauseLingerPolicy::Clock;
    using MTB::PauseLingerPolicy::Decision;
    using MTB::PauseLingerPolicy::kMenuSwitchLinger;

    const Clock::time_point closedAt{};
    const auto deadline = closedAt + kMenuSwitchLinger;

    CHECK(Choose({
              .holdActive = true,
              .lingerArmed = true,
              .now = closedAt + 84ms,
              .lingerUntil = deadline,
          }) == Decision::kKeep);
    CHECK(Choose({
              .holdActive = true,
              .lingerArmed = true,
              .now = closedAt + 85ms,
              .lingerUntil = deadline,
          }) == Decision::kReleaseElapsedLinger);

    // Regression: release is elapsed-time based, not render-frame based. A
    // thousand checks during the first 50 ms do not consume the linger.
    for (int frame = 0; frame < 1000; ++frame) {
        CHECK(Choose({
                  .holdActive = true,
                  .lingerArmed = true,
                  .now = closedAt + 50ms,
                  .lingerUntil = deadline,
              }) == Decision::kKeep);
    }

    CHECK(Choose({
              .holdActive = true,
              .lingerArmed = false,
              .now = closedAt,
              .lingerUntil = deadline,
          }) == Decision::kReleaseOrphanedHold);
    CHECK(Choose({
              .coveredMenuOpen = true,
              .holdActive = true,
              .lingerArmed = true,
              .now = closedAt + 1s,
              .lingerUntil = deadline,
          }) == Decision::kKeep);

    // Real close with SPII + SmoothCam, using the timings captured from the
    // field log: SPII releases camera control 4 ms after its close notice and
    // the first render is ~16 ms later. If Menu Studio keeps freezeTime through
    // the 85 ms switch bridge, SmoothCam cannot publish its gameplay FOV before
    // that render, so the restored vanilla/menu-entry FOV is exposed.
    const auto externalClose = ChooseClose({
        .lastCoveredMenuClosed = true,
        .holdActive = true,
        .externalCameraHandoff = true,
    });
    CHECK(externalClose == CloseDecision::kReleaseExternalCamera);
    const bool worldFrozenAfterClose =
        externalClose != CloseDecision::kReleaseExternalCamera;
    constexpr auto spiiReleaseAt = 4ms;
    constexpr auto firstRenderAt = 16ms;
    const bool smoothCamCanPublish =
        !worldFrozenAfterClose && spiiReleaseAt < firstRenderAt;
    constexpr float restoredFallbackFov = 60.0f;
    constexpr float smoothCamGameplayFov = 80.0f;
    const float visibleFov =
        smoothCamCanPublish ? smoothCamGameplayFov : restoredFallbackFov;
    CHECK(visibleFov == smoothCamGameplayFov);

    // Menu Studio-owned framing still uses the switch bridge.
    CHECK(ChooseClose({
              .lastCoveredMenuClosed = true,
              .holdActive = true,
              .externalCameraHandoff = false,
          }) == CloseDecision::kArmSwitchLinger);

    if (g_failures == 0) {
        std::printf("pause linger policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

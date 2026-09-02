#include "CameraDebtPolicy.h"

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
    namespace P = MTB::CameraDebtPolicy;

    // ── The hot path ───────────────────────────────────────────────────────
    // Disarm() runs every frame while no menu is open, and on all of those
    // frames there is nothing to decide.
    {
        CHECK(P::ChooseTeardown({}) == P::Teardown::kNothingOwed);
        CHECK(P::ChooseTeardown({ .haveCapture = false,
                                  .menusCounted = true,
                                  .uiHoldsAMenu = true }) ==
              P::Teardown::kNothingOwed);
    }

    // ── The genuine close ──────────────────────────────────────────────────
    // Nobody says a menu is up any more. Pay and retire the reading; this is
    // the behaviour that was already correct and must not move.
    {
        CHECK(P::ChooseTeardown({ .haveCapture = true,
                                  .menusCounted = false,
                                  .uiHoldsAMenu = false }) ==
              P::Teardown::kPayAndClear);
    }

    // ── THE REGRESSION ─────────────────────────────────────────────────────
    // Every one of these used to clear the capture. The next tick re-armed,
    // found the flags false, and re-read the camera the framing owned.
    {
        // A settings save inside the panel. Field 2026-08-30: the teardown ran
        // at 01:57:45.764 and the close event arrived at 01:57:47.535, so the
        // count had already been reconciled to zero while the menu was still on
        // screen. The UI is the only witness left on this route, and it is
        // enough on its own.
        CHECK(P::ChooseTeardown({ .haveCapture = true,
                                  .menusCounted = false,
                                  .uiHoldsAMenu = true }) ==
              P::Teardown::kPayAndKeep);

        // The dormancy latch on an unpaused menu - the "especially during
        // fights" half of the report, because a menu opened in combat under
        // Skyrim Souls is live and latches dormant. Here our own count is the
        // witness and the UI may lag it.
        CHECK(P::ChooseTeardown({ .haveCapture = true,
                                  .menusCounted = true,
                                  .uiHoldsAMenu = false }) ==
              P::Teardown::kPayAndKeep);

        // Both agree the menu is up: a transient missing-3D frame, a menu
        // switch, anything else that reaches Disarm mid-session.
        CHECK(P::ChooseTeardown({ .haveCapture = true,
                                  .menusCounted = true,
                                  .uiHoldsAMenu = true }) ==
              P::Teardown::kPayAndKeep);
    }

    // ⚠ EITHER WITNESS IS ENOUGH, AND THAT ASYMMETRY IS THE POINT. Keeping a
    // capture one frame past the close costs nothing - the next Disarm frame
    // retires it, and Disarm runs every frame out in the world. Dropping one a
    // frame early costs the player their camera until they force a rebuild. So
    // the rule is OR, never AND, and no reachable pair of witnesses may clear.
    {
        for (int witnesses = 0; witnesses < 4; ++witnesses) {
            const bool counted = (witnesses & 1) != 0;
            const bool ui = (witnesses & 2) != 0;
            const auto action = P::ChooseTeardown({ .haveCapture = true,
                                                    .menusCounted = counted,
                                                    .uiHoldsAMenu = ui });
            CHECK((action == P::Teardown::kPayAndClear) == (!counted && !ui));
        }
    }

    // ── Taking the reading ─────────────────────────────────────────────────
    {
        // First armed tick of a session: nothing framed, nothing held.
        CHECK(P::ChooseCapture({ .haveCapture = false,
                                 .captureAlreadyTaken = false }) ==
              P::Capture::kTake);

        // The re-arm after a mid-menu teardown, once the teardown rule above
        // keeps the reading. This is the case that used to poison it.
        CHECK(P::ChooseCapture({ .haveCapture = true,
                                 .captureAlreadyTaken = true }) ==
              P::Capture::kAlreadyHeld);
        CHECK(P::ChooseCapture({ .haveCapture = true,
                                 .captureAlreadyTaken = false }) ==
              P::Capture::kAlreadyHeld);

        // The belt and braces: a framing has run and the capture is gone
        // anyway, so some route cleared it that the teardown rule does not
        // cover. Reading the camera here would hand the framing to gameplay.
        CHECK(P::ChooseCapture({ .haveCapture = false,
                                 .captureAlreadyTaken = true }) ==
              P::Capture::kRefuseFramingLive);
    }

    // ── Whose camera the reading describes ──────────────
    // Field 2026-09-02: through Tab, the tween menu is up and holding the camera
    // (kTween, fov 90) when the covered menu's open event reaches us, so the
    // reading taken there was the tween's: 4 of 4 on the reporter's rig against
    // 22 of 22 'tween menu closed' on the rig that could not reproduce it. The
    // tween's own open edge now takes a reading of the player's camera, and a
    // covered menu opening over a live tween carries THAT instead of reading.
    {
        // The hotkey route: no tween, read the camera. Every proven run.
        CHECK(P::ChooseReadingSource({ .tweenMenuUp = false,
                                       .preTweenReadingValid = false }) ==
              P::ReadingSource::kLiveCamera);
        // A stale slot with no tween up must never be carried. The tween's
        // close drops the slot; this is the guard for the day it does not.
        CHECK(P::ChooseReadingSource({ .tweenMenuUp = false,
                                       .preTweenReadingValid = true }) ==
              P::ReadingSource::kLiveCamera);
        // The Tab route, reading in hand: carry it.
        CHECK(P::ChooseReadingSource({ .tweenMenuUp = true,
                                       .preTweenReadingValid = true }) ==
              P::ReadingSource::kPreTweenReading);
        // The Tab route with nothing in hand (armed mid-tween): read live, and
        // let the return-state policy refuse kTween on its own.
        CHECK(P::ChooseReadingSource({ .tweenMenuUp = true,
                                       .preTweenReadingValid = false }) ==
              P::ReadingSource::kLiveCamera);

        // The tween's open edge samples only where a covered menu opening now
        // would count as session-fresh. A tween opening over a live session (a
        // menu switch through Tab) must not overwrite the reading it holds.
        CHECK(P::ChooseTweenSample(true) == P::TweenSample::kTake);
        CHECK(P::ChooseTweenSample(false) == P::TweenSample::kIgnore);
    }

    // Both decisions are compile-time, which is what keeps them out of the
    // per-frame cost of a Disarm that has nothing to do.
    {
        static_assert(P::ChooseTeardown({}) == P::Teardown::kNothingOwed);
        static_assert(P::ChooseTeardown({ .haveCapture = true,
                                          .menusCounted = false,
                                          .uiHoldsAMenu = true }) ==
                      P::Teardown::kPayAndKeep);
        static_assert(P::ChooseCapture({ .haveCapture = false,
                                         .captureAlreadyTaken = true }) ==
                      P::Capture::kRefuseFramingLive);
    }

    if (g_failures == 0) {
        std::printf("camera_debt_policy_test: all checks passed\n");
        return 0;
    }
    std::printf("camera_debt_policy_test: %d check(s) FAILED\n", g_failures);
    return 1;
}

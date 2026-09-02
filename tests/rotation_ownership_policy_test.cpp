#include "RotationOwnershipPolicy.h"

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
    using MTB::RotationOwnershipPolicy::ChooseReassert;
    using MTB::RotationOwnershipPolicy::ChooseScalarRestore;

    // Regression: SPIM's input-time Update3DPosition must be neutralised even
    // before Menu Studio has accumulated any visible yaw.
    const auto ownedAtZero = ChooseReassert({
        .armed = true,
        .spinBasisValid = true,
        .previewSpin = true,
        .spimPresent = true,
        .overrideSpimRotation = true,
        .menuOpen = true,
        .spinYaw = 0.0f,
    });
    CHECK(ownedAtZero.reassertRoot);
    CHECK(ownedAtZero.restorePlayerHeading);
    CHECK(ownedAtZero.restoreCameraPark);
    CHECK(ownedAtZero.neutralizeBeforeOriginal);
    CHECK(ownedAtZero.propagateSceneGraph);

    // Minimal call-order replay:
    //   SPIM writes heading -> Update3DPosition publishes it to `world`
    //   -> Menu Studio composes its root-local yaw.
    // The old hook stopped after the local write, leaving `world` at the
    // pinned/front heading until a later frame.
    float playerHeading = 0.9f;  // SPIM's attempted turn
    float worldYaw = -1.0f;
    constexpr float armedHeading = 0.0f;
    constexpr float menuStudioYaw = 0.6f;
    if (ownedAtZero.neutralizeBeforeOriginal) {
        playerHeading = armedHeading;
    }
    worldYaw = playerHeading;  // original Update3DPosition
    float rootLocalYaw = worldYaw;
    if (ownedAtZero.reassertRoot) {
        rootLocalYaw = menuStudioYaw;
    }
    if (ownedAtZero.propagateSceneGraph) {
        worldYaw = rootLocalYaw;
    }
    CHECK(worldYaw == menuStudioYaw);

    // Without ownership, the existing non-zero spin reassert remains, but
    // Menu Studio does not alter SPIM's heading or camera.
    const auto unownedSpin = ChooseReassert({
        .armed = true,
        .spinBasisValid = true,
        .previewSpin = true,
        .spimPresent = true,
        .overrideSpimRotation = false,
        .menuOpen = true,
        .spinYaw = 0.25f,
    });
    CHECK(unownedSpin.reassertRoot);
    CHECK(!unownedSpin.restorePlayerHeading);
    CHECK(!unownedSpin.restoreCameraPark);
    CHECK(!unownedSpin.neutralizeBeforeOriginal);
    CHECK(unownedSpin.propagateSceneGraph);

    const auto unownedAtZero = ChooseReassert({
        .armed = true,
        .spinBasisValid = true,
        .previewSpin = true,
        .spimPresent = true,
        .overrideSpimRotation = false,
        .menuOpen = true,
        .spinYaw = 0.0f,
    });
    CHECK(!unownedAtZero.reassertRoot);

    const auto inactive = ChooseReassert({
        .armed = true,
        .spinBasisValid = true,
        .raceMenuOpen = true,
        .previewSpin = true,
        .spimPresent = true,
        .overrideSpimRotation = true,
        .menuOpen = true,
    });
    CHECK(!inactive.reassertRoot);
    CHECK(!inactive.restorePlayerHeading);
    CHECK(!inactive.restoreCameraPark);
    CHECK(!inactive.neutralizeBeforeOriginal);
    CHECK(!inactive.propagateSceneGraph);

    // ── The heading pin dies with the MENU, not with the arm ───────────────
    // User report 2026-08-16, SPIM only: "when we exit the player can end up
    // with a different rotation to before they opened the menu". SPIM turns the
    // PLAYER to face the camera at its framing and puts the heading back from
    // its own saved copy on close. SPII swings the CAMERA instead and never
    // touches the body, which is why only SPIM shows this.
    //
    // Our armedHeading_ is sampled a tick after SPIM has already turned the
    // body, so it holds the TURNED heading. Pinning to it is correct while the
    // menu is up and is how SPIM's per-drag rotation is neutralised. Past the
    // close edge it is the last writer over SPIM's correct restore, and we hold
    // no copy of the real heading to repair it with.
    {
        using MTB::RotationOwnershipPolicy::PinHeading;

        // In the menu, with the override on: we own the heading, as before.
        const auto inMenu = ChooseReassert({
            .armed = true,
            .spinBasisValid = true,
            .previewSpin = true,
            .spimPresent = true,
            .overrideSpimRotation = true,
            .menuOpen = true,
            .spinYaw = 0.0f,
        });
        CHECK(inMenu.restorePlayerHeading);

        // The exit window: `armed` is still true because the teardown is
        // deferred, but the menu is gone and SPIM has already restored. The
        // heading write must stop, and ONLY that write - the camera park and
        // the root spin still have a frame to finish on.
        const auto exiting = ChooseReassert({
            .armed = true,
            .spinBasisValid = true,
            .previewSpin = true,
            .spimPresent = true,
            .overrideSpimRotation = true,
            .menuOpen = false,
            .spinYaw = 0.0f,
        });
        CHECK(!exiting.restorePlayerHeading);
        CHECK(exiting.restoreCameraPark);
        CHECK(exiting.reassertRoot);
        CHECK(exiting.neutralizeBeforeOriginal);

        // The term is a plain AND, and an open menu cannot conjure ownership
        // we do not have: without SPIM this was never our heading to pin.
        CHECK(PinHeading(true, true));
        CHECK(!PinHeading(true, false));
        CHECK(!PinHeading(false, true));
        CHECK(!PinHeading(false, false));

        // SPII: the override never engages, so no heading write exists to
        // stop, in the menu or out of it. The report's "not an issue with
        // SPII" is this line.
        const auto spii = ChooseReassert({
            .armed = true,
            .spinBasisValid = true,
            .previewSpin = true,
            .spimPresent = false,
            .overrideSpimRotation = true,
            .menuOpen = true,
            .spinYaw = 0.0f,
        });
        CHECK(!spii.restorePlayerHeading);
    }

    // ── Handing a single framed value back ─────────────────────────────────
    {
        using MTB::RotationOwnershipPolicy::kFovEpsilon;
        using MTB::RotationOwnershipPolicy::kPitchEpsilon;

        // The measured pitch: entered 0.058, a framing wrote 0.100.
        const auto pitch = ChooseScalarRestore(true, 0.058f, 0.100f, kPitchEpsilon);
        CHECK(pitch.restore);
        CHECK(pitch.value == 0.058f);

        // The measured FOV: gameplay 90, the inventory framing wrote 60, and
        // the editor was entered on the 60 because nobody had handed it back.
        const auto fov = ChooseScalarRestore(true, 90.0f, 60.0f, kFovEpsilon);
        CHECK(fov.restore);
        CHECK(fov.value == 90.0f);

        // Nothing wrote it, so nothing is owed.
        CHECK(!ChooseScalarRestore(true, 0.058f, 0.058f, kPitchEpsilon).restore);
        CHECK(!ChooseScalarRestore(true, 90.0f, 90.0f, kFovEpsilon).restore);

        // No entry value captured (a teardown with no arm behind it) cannot
        // invent one.
        CHECK(!ChooseScalarRestore(false, 0.0f, 0.100f, kPitchEpsilon).restore);

        // Float noise is not a framing, and the two epsilons are scaled to
        // their own units: a hundredth of a degree is noise, a hundredth of a
        // radian is a real pitch change.
        CHECK(!ChooseScalarRestore(true, 0.058f, 0.0585f, kPitchEpsilon).restore);
        CHECK(!ChooseScalarRestore(true, 90.0f, 90.01f, kFovEpsilon).restore);
        CHECK(ChooseScalarRestore(true, 0.058f, 0.068f, kPitchEpsilon).restore);

        // Downward is a change too - the sign is not the question.
        CHECK(ChooseScalarRestore(true, 0.400f, 0.100f, kPitchEpsilon).restore);
        CHECK(ChooseScalarRestore(true, 60.0f, 90.0f, kFovEpsilon).restore);
    }

    // ── The stale-park guard ───────────────────────────────────────────────
    // Field 2026-08-11, "Space around you = Off": the deferred teardown pays
    // the park at ~+149 ms, AFTER the view mod's own restore at ~+110 ms. On a
    // rig where the park's capture lost the race at the open (Souls-late arm),
    // that payment re-applied the framing: animCam TRUE, freeRotEnabled TRUE,
    // yaw works, pitch locked. The guard: a field that changed between the
    // close edge and the payment was restored by its owner - stand aside.
    {
        using MTB::RotationOwnershipPolicy::ParkMayRestore;
        using MTB::RotationOwnershipPolicy::kPitchEpsilon;

        // The repro: at close the flag still held the framing (true); by
        // payment time the provider had restored it (false). A poisoned park
        // (preMenu true) must NOT get the last word.
        CHECK(!ParkMayRestore(true, /*atClose*/ true, /*live*/ false));

        // Untouched since the close: the park is the only payer left - pay.
        CHECK(ParkMayRestore(true, true, true));
        CHECK(ParkMayRestore(true, false, false));

        // No close-edge sample (the load boundary, a ForceReset): pay as
        // before - the guard must not invent a decline.
        CHECK(ParkMayRestore(false, true, false));
        CHECK(ParkMayRestore(false, false, true));

        // Scalars, same rule with the field's own epsilon. freeRotation.x:
        // framed 2.642 at close, provider restored -0.060 in the window.
        CHECK(!ParkMayRestore(true, 2.642f, -0.060f, kPitchEpsilon));
        // Hands-off exit: still exactly the framed value - the park pays.
        CHECK(ParkMayRestore(true, 2.642f, 2.642f, kPitchEpsilon));
        // Noise is not a writer.
        CHECK(ParkMayRestore(true, 2.642f, 2.6425f, kPitchEpsilon));
        // No sample -> pay.
        CHECK(ParkMayRestore(false, 0.0f, 9.0f, kPitchEpsilon));

        // The in-dispatch payment (Void/Dressing room) samples and pays in
        // the same call stack, so live == atClose by construction and the
        // guard is a proven no-op on every path that was already healthy.
        CHECK(ParkMayRestore(true, 0.100f, 0.100f, kPitchEpsilon));
    }

    // ── The two flags that belong to the stance, not to the session ────────
    // Field 2026-08-09 and again after the 0.8.1 stale-park fix: "I can look
    // left and right but not up and down after closing the inventory", and
    // "it can still happen for some people rarely in combat". The stale-park
    // guard above cannot reach this one: the park's value was never the right
    // value to write. freeRotationEnabled and toggleAnimCam are derived by the
    // engine from whether the weapon is out (ThirdPersonState carries
    // ProcessWeaponDrawnChange and SetFreeRotationMode), and a menu session is
    // where that stance changes underneath the capture.
    {
        using MTB::RotationOwnershipPolicy::ChooseStanceFlags;
        using MTB::RotationOwnershipPolicy::FreeRotationForStance;
        using MTB::RotationOwnershipPolicy::ParkMayRestore;
        using MTB::RotationOwnershipPolicy::StanceFlags;
        using MTB::RotationOwnershipPolicy::kAnimCamHandBack;

        // The healthy 90%: sheathed in, sheathed out. Nothing about the old
        // behaviour may change here, and this is the case that proves it.
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = false,
                                  .stanceKnownNow = true,
                                  .drawnNow = false }) == StanceFlags::kHandBackCapture);
        // Drawn in, drawn out - equally unchanged.
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = true,
                                  .stanceKnownNow = true,
                                  .drawnNow = true }) == StanceFlags::kHandBackCapture);

        // The repro. The weapon preview drew on open and the deferred sheathe
        // lands after the camera restore, or combat started mid-menu and the
        // gate sheathed there. Either way the capture describes a stance the
        // player is no longer in, and handing it back is the dead pitch.
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = false,
                                  .stanceKnownNow = true,
                                  .drawnNow = true }) == StanceFlags::kEngineDerived);
        // And the other direction: drawn when the menu opened in combat,
        // sheathed by the time the teardown pays.
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = true,
                                  .stanceKnownNow = true,
                                  .drawnNow = false }) == StanceFlags::kEngineDerived);

        // ⚠ Only kSheathed and kDrawn are terminal. A reading taken mid-clip
        // cannot be compared to anything, and the transition still in flight
        // will end with the engine recomputing both flags itself - so an
        // unknown stance keeps the old behaviour rather than inventing a
        // third one.
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = false,
                                  .drawnAtCapture = false,
                                  .stanceKnownNow = true,
                                  .drawnNow = true }) == StanceFlags::kHandBackCapture);
        CHECK(ChooseStanceFlags({ .haveCapture = true,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = false,
                                  .stanceKnownNow = false,
                                  .drawnNow = true }) == StanceFlags::kHandBackCapture);

        // Nothing captured is nothing owed, whatever the stance says.
        CHECK(ChooseStanceFlags({ .haveCapture = false,
                                  .stanceKnownAtCapture = true,
                                  .drawnAtCapture = false,
                                  .stanceKnownNow = true,
                                  .drawnNow = true }) == StanceFlags::kLeaveAlone);

        // SetFreeRotationMode takes `a_weaponSheathed`, so free rotation IS
        // the sheathed mode: drawn locks the character to the camera.
        CHECK(FreeRotationForStance(false));
        CHECK(!FreeRotationForStance(true));

        // ── The Show Player In Menus half ──────────────────────────────────
        // User report 2026-08-15: the lock happens with SPIM installed, not
        // SPII. SPIM's own ResetCamera hardcodes toggleAnimCam = false rather
        // than restoring a saved copy (Event.cpp:586), because false is the
        // only value worth leaving behind. With SPIM present our OwnView never
        // arms (MenuCovered answers true for every menu), so the park is the
        // only restorer and its stale `true` is the whole bug.
        //
        // The value handed back is therefore a constant, not a decision: there
        // is no combination of capture and stance that should ever put the anim
        // cam back on after a menu.
        CHECK(!kAnimCamHandBack);

        // ⚠ AND THE STALE-PARK GUARD CANNOT SUBSTITUTE FOR IT. This is the
        // SPIM ordering: SPIM's close handler clears the flag BEFORE our
        // close-edge sample is taken, so both readings are false, the guard
        // reports a field nobody touched, and it would wave a poisoned `true`
        // straight through. Load order decides which sink runs first, which is
        // what makes the report "rarely, for some people".
        CHECK(ParkMayRestore(true, /*atClose*/ false, /*live*/ false));
    }

    if (g_failures == 0) {
        std::printf("rotation ownership policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

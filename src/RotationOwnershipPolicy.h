#pragma once

namespace MTB::RotationOwnershipPolicy {

    struct ReassertInput {
        bool  armed{ false };
        bool  spinBasisValid{ false };
        bool  raceMenuOpen{ false };
        bool  previewSpin{ false };
        bool  mounted{ false };
        bool  spimPresent{ false };
        bool  overrideSpimRotation{ false };
        // ⚠⚠ THE HEADING PIN IS A MENU-LIFETIME WRITE, NOT AN ARM-LIFETIME ONE,
        // AND THAT DISTINCTION IS THE 2026-08-16 EXIT-ROTATION BUG. `armed`
        // stays true through the deferred exit, which is LIVE GAMEPLAY: the
        // menu is gone, SPIM has already put the player's heading back, and the
        // engine is calling Update3DPosition again - which is our hook. One
        // more pin from there and the player snaps back to facing the camera
        // with nothing left to correct it. See PinHeading below.
        bool  menuOpen{ false };
        float spinYaw{ 0.0f };
    };

    struct ReassertPlan {
        bool neutralizeBeforeOriginal{ false };
        bool reassertRoot{ false };
        bool restorePlayerHeading{ false };
        bool restoreCameraPark{ false };
        bool propagateSceneGraph{ false };
    };

    [[nodiscard]] constexpr bool OwnsSpimRotation(bool a_spimPresent,
                                                   bool a_overrideSpimRotation) {
        return a_spimPresent && a_overrideSpimRotation;
    }

    // ── Who owns the player's heading, and for how long ────────────────────
    //
    // Only SPIM ever makes this a question. It turns the PLAYER to face the
    // camera at its framing (`SetRotationZ(angle.z -= angleChange)` in its
    // RotatePlayer, roughly a half turn) and puts the heading back from its own
    // `m_playerRotation` on close. SPII does the opposite: it leaves the body
    // alone and swings the CAMERA around it, so our pin writes the heading the
    // player already had and none of this can arise. That is the whole of "not
    // an issue with SPII".
    //
    // Because SPIM turns the body BEFORE our first armed tick, `armedHeading_`
    // is the TURNED heading, not the one the player walked in with. Pinning to
    // it is right while the menu is up: it holds the character where the
    // framing put them, and it is how SPIM's per-drag rotation is neutralised
    // so our own spin owns the character. It is wrong the moment the menu is
    // gone, and we have no record of the real heading to correct it with.
    // SPIM does.
    //
    // ⚠⚠ `armed` IS NOT THE SAME QUESTION AS "a menu is up". It stays true
    // through the deferred exit, which is live gameplay - SPIM has restored the
    // heading by then and the engine is calling Update3DPosition again, which
    // is our hook. One more pin from there and the player snaps back to facing
    // the camera with nothing left to correct it.
    //
    // ⚠ SO THE PIN STOPS AT THE CLOSE EDGE AND NO RESTORE IS ADDED. SPIM's own
    // reset is already correct and already runs; the only thing that broke it
    // was us writing after it. Two painters of one appearance is what caused
    // this, so the fix is one fewer writer, not one more.
    [[nodiscard]] constexpr bool PinHeading(bool a_ownsSpim, bool a_menuOpen) {
        return a_ownsSpim && a_menuOpen;
    }

    // SPIM changes player heading and third-person free rotation, then calls
    // Update3DPosition in the input event itself. When Menu Studio owns the
    // drag, both writes must be undone in that same call—even when our spin is
    // still exactly zero—or one render can expose both rotations.
    [[nodiscard]] constexpr ReassertPlan ChooseReassert(const ReassertInput& a_input) {
        const bool active = a_input.armed && a_input.spinBasisValid &&
                            !a_input.raceMenuOpen && a_input.previewSpin &&
                            !a_input.mounted;
        if (!active) {
            return {};
        }

        const bool ownsSpim =
            OwnsSpimRotation(a_input.spimPresent, a_input.overrideSpimRotation);
        const bool reassertRoot = a_input.spinYaw != 0.0f || ownsSpim;
        return {
            .neutralizeBeforeOriginal = ownsSpim,
            .reassertRoot = reassertRoot,
            .restorePlayerHeading = PinHeading(ownsSpim, a_input.menuOpen),
            .restoreCameraPark = ownsSpim,
            .propagateSceneGraph = reassertRoot,
        };
    }


    [[nodiscard]] constexpr float AbsF(float a_v) {
        return a_v < 0.0f ? -a_v : a_v;
    }

    // ── Handing the third-person state back at teardown ────────────────────
    //
    // ⚠ THERE IS NO DECISION LEFT TO MAKE HERE, AND THAT IS A FINDING RATHER
    // THAN AN OMISSION. A ChooseFreeRotationRestore used to live at this spot.
    // It asked whether the live value still sat on the park we had been
    // re-asserting before writing anything back, so as not to trample a view
    // mod that had restored first.
    //
    // The field killed it. The player ORBITS during the editor, which moves
    // freeRotation off the park by design, so the test read "somebody else
    // owns this" for precisely the case the restore exists to cover, and
    // declined: "freeRotation left at 0.512 (parked 2.642)". The conflict it
    // guarded against cannot arise anyway, because a view mod's copy and ours
    // are both the pre-menu value and whoever writes last writes the same
    // number.
    //
    // So the bubble restores the whole third-person state it found, in
    // Bubble::Disarm, with no test - the two FLAGS included, which the first
    // cut missed by fixing only numbers. Nothing there can be the player's
    // own doing: a menu holds their input for the whole session, so every
    // difference between what we found and what is there at teardown was
    // written by a framing.
    //
    // ⚠ THAT REASONING IS TRUE OF THE SESSION AND FALSE OF THE WINDOW AFTER
    // IT, and the difference is the 2026-08-11 pitch lock. With "Space around
    // you" at Void the payment runs inside the close dispatch; at Off/Scene
    // view it is deferred ~150 ms into LIVE gameplay - a window in which the
    // view mod that framed the menu legitimately restores its own writes
    // (measured: SPII at +110 ms, park at +149 ms). A park whose capture had
    // lost the race at the open (the arm lands "a few frames later" on a
    // Souls rig, and the framing beats it) then re-applies animCam=true /
    // freeRotEnabled=true ON TOP of that restore, as the last writer. Yaw
    // works, pitch is dead, and nothing ever heals it.
    //
    // The guard below is NOT the removed ownership test in new clothes. That
    // test compared the live value against the PARK (what we kept
    // re-asserting), so a player orbiting during the editor read as "somebody
    // else owns this" and the restore was wrongly declined. This one compares
    // against the CLOSE EDGE: a field that moved since the close was paid by
    // its owner in the window, and our copy - whether clean or poisoned - is
    // stale either way. When the payment runs in the close dispatch itself,
    // live == atClose by construction, so every already-healthy path is
    // untouched.

    // A field is only the park's to restore if nobody wrote it between the
    // close edge and the payment.
    [[nodiscard]] constexpr bool ParkMayRestore(bool a_haveAtClose,
                                                bool a_atClose, bool a_live) {
        return !a_haveAtClose || a_live == a_atClose;
    }

    [[nodiscard]] constexpr bool ParkMayRestore(bool a_haveAtClose,
                                                float a_atClose, float a_live,
                                                float a_epsilon) {
        return !a_haveAtClose || AbsF(a_live - a_atClose) <= a_epsilon;
    }

    // ── Handing a single framed value back ─────────────────────────────────
    //
    // The pitch and the field of view both come through here. Neither lives on
    // the third-person state restored wholesale above: pitch is on the ACTOR
    // and the FOV is on the camera singleton, so each is a write to something
    // the rest of the game is using and "did a framing touch this at all" is
    // worth asking before making it.
    //
    // Measured 2026-08-05. Pitch entered at 0.058 and left at 0.100, 0.1 being
    // the literal in the Show Player In Inventory recipe, with the field
    // reporting a camera that would not look up or down. The FOV is the same
    // story one field over: the editor was entered already reading 60, because
    // the INVENTORY session applied a framing at FOV 60 and nothing handed the
    // gameplay value back, so the report is a view that stays too narrow
    // afterwards.
    struct ScalarRestore {
        bool  restore = false;
        float value = 0.0f;
    };

    // Radians for a pitch. Tighter than any framing offset and looser than the
    // noise of a float copied through a save and back.
    inline constexpr float kPitchEpsilon = 0.001f;
    // Degrees for a field of view, where the framings in play are tens apart.
    inline constexpr float kFovEpsilon = 0.05f;

    [[nodiscard]] constexpr ScalarRestore ChooseScalarRestore(
        bool a_haveEntry, float a_entryValue, float a_liveValue,
        float a_epsilon) {
        if (!a_haveEntry) {
            return {};  // never captured one, so nothing is owed
        }
        if (AbsF(a_liveValue - a_entryValue) <= a_epsilon) {
            return {};  // already where it started
        }
        return { .restore = true, .value = a_entryValue };
    }

    // ── The two flags that belong to the stance, not to the session ────────
    //
    // ⚠⚠ `freeRotationEnabled` AND `toggleAnimCam` ARE NOT SAVED SETTINGS.
    // The engine derives them from whether the weapon is out: ThirdPersonState
    // carries `ProcessWeaponDrawnChange(bool a_drawn)` and
    // `SetFreeRotationMode(bool a_weaponSheathed)` as virtuals, and those are
    // the only things that recompute them. Nothing re-derives them per frame,
    // so a value written over the engine's own stands until the player next
    // draws or sheathes. That is why the field report is a camera that never
    // heals on its own: same family as bHeadTracking being engine-driven
    // OUTPUT, and as [[engine-recomputes-it-does-not-store]].
    //
    // A teardown genuinely does owe both flags back, because every framing in
    // play writes them - OwnView's own recipe and Show Player In Inventory's
    // alike. The bug is WHICH value it owes. Handing the capture back is right
    // only while the stance it was taken in still stands, and a menu session is
    // precisely where that stops being true:
    //   - the weapon preview draws on open and sheathes on close, and the
    //     sheathe is DEFERRED past the camera restore (Bubble.cpp, F-26 r2);
    //   - WeaponPreviewGate returns kRestore the moment combat starts, so the
    //     sheathe can land in the middle of the menu;
    //   - the inventory transition itself reports a sheathed actor briefly
    //     (Fitting Room's WeaponPreview records the same reading).
    // All three are combat-flavoured, which is the "only in combat" that both
    // reporters kept naming and that nothing in the camera code explained.
    //
    // ⚠ THE STALE-PARK GUARD ABOVE CANNOT REACH THIS. It asks whether somebody
    // else wrote the field between the close edge and the payment. Here nobody
    // did - our own copy is simply the wrong answer, and on the in-dispatch
    // payment live == atClose by construction, so the guard waves it through.
    enum class StanceFlags {
        kLeaveAlone,       // nothing captured, so nothing is owed
        kHandBackCapture,  // the stance still stands - restore as before
        kEngineDerived,    // the stance moved under us - re-derive instead
    };

    struct StanceInput {
        bool haveCapture = false;
        bool stanceKnownAtCapture = false;
        bool drawnAtCapture = false;
        bool stanceKnownNow = false;
        bool drawnNow = false;
    };

    // ⚠ ONLY kSheathed AND kDrawn ARE TERMINAL, which is why both readings
    // carry a "known" bit rather than a bare bool. A reading taken mid-clip
    // cannot be compared to anything, and a transition still in flight ends
    // with the engine recomputing both flags itself - so an unknown stance
    // keeps the old behaviour instead of inventing a third one. The same
    // reasoning the post-close weapon-state watch already applies in Bubble.cpp.
    [[nodiscard]] constexpr StanceFlags ChooseStanceFlags(const StanceInput& a_in) {
        if (!a_in.haveCapture) {
            return StanceFlags::kLeaveAlone;
        }
        if (!a_in.stanceKnownAtCapture || !a_in.stanceKnownNow) {
            return StanceFlags::kHandBackCapture;
        }
        return a_in.drawnAtCapture == a_in.drawnNow
                   ? StanceFlags::kHandBackCapture
                   : StanceFlags::kEngineDerived;
    }

    // `SetFreeRotationMode` takes `a_weaponSheathed`, so free rotation IS the
    // sheathed mode: drawn locks the character to the camera, sheathed lets the
    // camera orbit around them.
    [[nodiscard]] constexpr bool FreeRotationForStance(bool a_drawn) {
        return !a_drawn;
    }

    // ── toggleAnimCam is never handed back, it is handed off ───────────────
    //
    // ⚠⚠ THIS FLAG IS NOT RESTORED FROM THE CAPTURE AT ALL, AND THAT IS THE
    // WHOLE FIX FOR SHOW PLAYER IN MENUS. Read the two providers side by side:
    //
    //   Show Player In Inventory saves the flag and writes its copy back
    //   (MenuCamera.cpp: saved at 257, re-applied at 142), the same shape we had.
    //
    //   Show Player In Menus does NOT. Its ResetCamera hardcodes
    //   `thirdState->toggleAnimCam = false` (Event.cpp:586) and never consults a
    //   saved value, because false is the only value worth leaving behind.
    //
    // With SPIM installed our OwnView never arms at all - MenuCovered returns
    // true for every menu, so ShouldOwn declines - which leaves the park as the
    // single restorer. SPIM turns the flag ON and OFF repeatedly WITHIN one
    // menu session, gated on IsWeaponDrawn (Event.cpp:218 re-arms it on a
    // drawn-weapon rotate, 253 clears it on a move), so what the park captured
    // is a snapshot of a dance rather than a setting. Then SPIM clears the flag
    // correctly on close and the park writes its snapshot back on top.
    //
    // ⚠ AND THE STALE-PARK GUARD IS BLIND TO EXACTLY THAT. It compares the
    // close edge against the payment. When SPIM's own close handler runs BEFORE
    // our close-edge sample, both readings are already false, the guard sees a
    // field nobody touched, and it waves our stale `true` through. Which sink
    // runs first is load order, which is why this is "rarely, for some people".
    //
    // So: hand the flag off to the value that lets the player look up and down,
    // every time, exactly as SPIM does. A pre-menu TRUE cannot be worth
    // restoring - an animated camera that was running when the menu opened has
    // long finished by the time it closes, and the engine sets the flag again
    // whenever it next wants it. There is no reading of the capture that beats
    // this, so the capture is not read.
    //
    // ⚠ freeRotationEnabled gets the OPPOSITE treatment on purpose, and the
    // reason is True Directional Movement: it leaves that flag true as a normal
    // gameplay state, so deriving it from the stance would fight TDM on every
    // teardown. That one keeps the capture whenever the stance held, and is
    // re-derived only when the stance provably moved.
    inline constexpr bool kAnimCamHandBack = false;

}  // namespace MTB::RotationOwnershipPolicy

#pragma once

// Who owns the pre-menu camera capture, and when a teardown is allowed to
// retire it.
//
// ⚠⚠ THE CAPTURE WAS BEING RE-TAKEN FROM A CAMERA THE FRAMING ALREADY OWNED,
// AND THAT IS THE FROZEN CAMERA. The park holds one reading per menu session:
// the field of view, the pitch, the third-person free rotation and its two
// flags, taken at the MENU-OPEN EVENT, before any framing runs. The close
// hands that reading back. `Bubble::Disarm` ends by clearing it, which is
// correct at a close and wrong everywhere else - and Disarm is reached WITH THE
// MENU STILL ON SCREEN by several routes (a settings save inside the panel, the
// dormancy latch under an unpaused menu, a frame with no player 3D, a menu
// switch). Disarm also clears `armedLastFrame_`, so the very next tick re-enters
// the arm path, finds the capture flags false, and takes a fresh reading OFF THE
// FRAMED CAMERA. From that moment the capture holds the menu's own numbers, and
// the close writes the menu's numbers into live gameplay.
//
// ⚠ MEASURED, NOT ARGUED. `MenuStudio.log` 2026-08-31 10:13:30, a MagicMenu
// teardown:
//
//     camera debt: SPENT. 'MagicMenu' ... Held vs live: fov 60.0 vs 80.0,
//     freeRot 2.642 vs 3.196, pitch 0.100 vs 0.063, animCam true vs false
//
// The "held" column is the framing: 60 is the framed field of view, 0.100 is the
// literal in the Show Player In Inventory recipe, animCam true is a camera
// locked to the animation. The "live" column is the player's own, and the next
// session's open watch prints exactly it: `fov=80.0 ... animCam=false
// ... pitch=0.063`. The capture had been re-taken.
//
// ⚠⚠ WHY IT NEVER HEALS. `freeRotationEnabled` and `toggleAnimCam` are engine
// OUTPUT, recomputed only when the player draws or sheathes
// (ProcessWeaponDrawnChange / SetFreeRotationMode). A wrong value written over
// them stands until the next draw. Free rotation on with the angle standing
// still is a camera that does not turn with the character: the field reports it
// as a camera stuck in mid-air while the character walks out of shot. A forced
// camera rebuild (reading an Elder Scroll) clears it, and the next spell equip
// puts it straight back - which is the 2026-08-30 report, exactly.
//
// ⚠ AND WHY THREE EARLIER REPAIRS EACH HELD AND NONE ENDED IT. All three worked
// on the PAYMENT: what to hand back, and whether it was still ours to hand.
// None of them asked whether the value being handed back was the player's in the
// first place. The stale-park guard in RotationOwnershipPolicy.h is the reason
// this rig only saw it intermittently - it withholds a payment whose field moved
// since the close sample, so it blocks a poisoned capture BY ACCIDENT, and only
// on the routes that took a close sample at all. A mid-menu teardown never takes
// one, so nothing stands between the poison and the camera.

namespace MTB::CameraDebtPolicy {

    // ── The teardown ───────────────────────────────────────────────────────

    enum class Teardown {
        // No capture stands. Disarm() runs every frame while no menu is open,
        // so this is the overwhelmingly common answer and it must be free.
        kNothingOwed,
        // A menu we cover is still on screen. Hand back what the guards allow -
        // if the framing is still live it simply overwrites us again a frame
        // later, and if the session went dormant this is the restore - but KEEP
        // the reading, because the close still has to pay it and there is no
        // second chance to read the player's own values.
        kPayAndKeep,
        // The menu is genuinely gone. Pay and retire the reading.
        kPayAndClear,
    };

    struct TeardownFacts {
        // Any of the three valid flags: free rotation, pitch, field of view.
        bool haveCapture = false;
        // Our own count. ⚠ NOT SUFFICIENT ALONE, and the field is why: the r19c
        // self-heal reconciles menusOpen_ to zero against the UI, and a settings
        // save tore a session down 1.77 s before its close event arrived, so the
        // count read zero with the menu still up and still framing.
        bool menusCounted = false;
        // The UI map, asked about the menus we actually counted. The second
        // opinion the count needs. Either one saying "still there" is enough:
        // keeping a capture one frame too long costs nothing (the next Disarm
        // retires it), and dropping one a frame too early costs the camera.
        bool uiHoldsAMenu = false;
    };

    [[nodiscard]] constexpr Teardown ChooseTeardown(TeardownFacts a_facts) {
        if (!a_facts.haveCapture) {
            return Teardown::kNothingOwed;
        }
        if (a_facts.menusCounted || a_facts.uiHoldsAMenu) {
            return Teardown::kPayAndKeep;
        }
        return Teardown::kPayAndClear;
    }

    // ── Taking the reading ─────────────────────────────────────────────────

    enum class Capture {
        // The session-fresh menu-open event, nothing framed yet. Read it. Also
        // the first armed tick when no such open ever ran, which is the menu
        // that was already up when the plugin armed.
        kTake,
        // One already stands. The healthy no-op, and the whole point of the
        // teardown rule above: while the capture survives a mid-menu teardown,
        // the re-arm that follows lands here instead of on a framed camera.
        kAlreadyHeld,
        // ⚠ THE POISONING, CAUGHT AT THE OTHER END. A framing has run in this
        // menu session and there is no capture to show for it, so whatever this
        // tick reads belongs to the framing. Refusing leaves the park with
        // nothing to hand back, which is bad; taking it hands the framing to
        // gameplay, which is the frozen camera. Refuse, and say so loudly - a
        // reading of this is a route into Disarm that the teardown rule above
        // does not cover, and it wants finding rather than absorbing.
        kRefuseFramingLive,
    };

    struct CaptureFacts {
        bool haveCapture = false;
        // Has this menu session already read one? ⚠ THIS IS THE SAME STATEMENT
        // AS "A FRAMING HAS RUN SINCE", and that equivalence is what makes it
        // answerable. The reading is taken at the menu-open event, immediately
        // BEFORE the framing goes on; so if a session has taken one, the camera
        // it would read now belongs to whatever framed the menu - ours (OwnView)
        // or a view mod's, and the capture cannot tell whose numbers it is
        // looking at. Asking "did we already read one" needs a latch we set;
        // asking "is a framing applied" would need us to canvass every mod that
        // might have applied it.
        bool captureAlreadyTaken = false;
    };

    [[nodiscard]] constexpr Capture ChooseCapture(CaptureFacts a_facts) {
        if (a_facts.haveCapture) {
            return Capture::kAlreadyHeld;
        }
        if (a_facts.captureAlreadyTaken) {
            return Capture::kRefuseFramingLive;
        }
        return Capture::kTake;
    }

    // ── Whose camera the reading describes ──────────────
    //
    // ⚠⚠ A READING TAKEN UNDER THE TWEEN MENU IS A READING OF THE TWEEN MENU.
    // Tab opens the tween menu first, and the tween menu takes the camera:
    // kTween, at its own field of view. The covered menu opens OVER it, so the
    // reading at that menu's open event, correct on every hotkey route, holds
    // the tween's numbers on the Tab route. Field 2026-09-02: 4 of 4 captures
    // 'tween menu UP' on the reporter's rig, 22 of 22 'closed' on the rig that
    // never reproduced it. So the tween menu's OWN open edge takes a reading of
    // the player's camera, and a covered menu opening over a live tween carries
    // that reading instead of taking one.
    enum class ReadingSource {
        kLiveCamera,       // read the camera at this open event, as before
        kPreTweenReading,  // carry the reading the tween's open edge took
    };

    struct ReadingSourceFacts {
        bool tweenMenuUp = false;
        bool preTweenReadingValid = false;
    };

    [[nodiscard]] constexpr ReadingSource ChooseReadingSource(ReadingSourceFacts a_facts) {
        if (a_facts.tweenMenuUp && a_facts.preTweenReadingValid) {
            return ReadingSource::kPreTweenReading;
        }
        return ReadingSource::kLiveCamera;
    }

    // The tween menu's open edge: sample only where a covered menu opening now
    // would count as session-fresh. A tween opening over a live session (a
    // menu switch through Tab) must not overwrite the reading it holds.
    enum class TweenSample { kIgnore, kTake };

    [[nodiscard]] constexpr TweenSample ChooseTweenSample(bool a_sessionFresh) {
        return a_sessionFresh ? TweenSample::kTake : TweenSample::kIgnore;
    }

}  // namespace MTB::CameraDebtPolicy

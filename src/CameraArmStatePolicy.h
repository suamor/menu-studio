#pragma once

// Which camera the arm found, and what it is allowed to do about it.
//
// ⚠ THE ARM USED TO ASK THE WRONG QUESTION, AND IT ASKED IT TWICE. The test was
// `firstPerson || mounted` - an allowlist of two states - when the question the
// framing actually needs answered is "can what I am about to write be SEEN from
// here". Everything we write lands on the third-person state object and the
// over-shoulder INI settings, so the answer is yes from kThirdPerson and no from
// every other state in the engine.
//
// The failure is silent by construction. A state outside the allowlist skipped
// the switch, applied the whole framing to a camera nobody was looking through,
// and logged itself as an "unmanaged third-person arm" - a name that assumes the
// thing it never checked. On screen: no close-up, the character from behind,
// centred, exactly where the previous camera had them. That is the 2026-08-11
// report ("if an animation calls for a camera change it can get stuck and fail
// to trigger the close-up"), and it is the r52 mounted empty void a second time.
// r52 fixed that one by adding kMount to the allowlist rather than by inverting
// it, so the next state to come along broke the same way.
//
// Which state it happens to be is a detail of who is driving: an animation
// camera (kAnimated), sitting furniture (kFurniture), a mid-POV-swap frame
// (kPCTransition), a vanity spin (kAutoVanity). The reported case reached us
// through the BARTER menu, which is the one menu we cover that opens out of a
// CONVERSATION, and a conversation is where camera mods live.

namespace MTB::CameraArmStatePolicy {

    // ⚠ MIRRORS RE::CameraState SO THE DECISION CAN BE TESTED WITHOUT THE ENGINE,
    // and Bubble.cpp static_asserts every one of these against the real enum. A
    // silent drift here would re-point the whole table at the wrong states, so it
    // is a compile error instead.
    inline constexpr int kFirstPerson = 0;
    inline constexpr int kAutoVanity = 1;
    inline constexpr int kVATS = 2;
    inline constexpr int kFree = 3;
    inline constexpr int kIronSights = 4;
    inline constexpr int kFurniture = 5;
    inline constexpr int kPCTransition = 6;
    inline constexpr int kTween = 7;
    inline constexpr int kAnimated = 8;
    inline constexpr int kThirdPerson = 9;
    inline constexpr int kMount = 10;
    inline constexpr int kBleedout = 11;
    inline constexpr int kDragon = 12;
    inline constexpr int kTotal = 13;

    struct ArmPlan {
        // Apply the framing at all. False means stand down completely: no
        // framing, no INI writes, nothing to hand back.
        bool frame = false;
        // Switch to third person first, remembering what we left so the close
        // can put it back (the r52 savedStateId machinery, which already
        // restores an arbitrary state rather than a hardcoded first person).
        bool forceThird = false;
        // The rider raise/boom/pitch, which belong to kMount alone. A dragon is
        // also a mount in the plain-English sense and deliberately does NOT get
        // these: the offsets were fitted to a horse and nothing has measured a
        // dragon.
        bool mountOffsets = false;
    };

    [[nodiscard]] constexpr ArmPlan ChooseArm(int a_stateId) {
        // Already where the framing can be seen. No switch, and nothing to
        // restore at the close either.
        if (a_stateId == kThirdPerson) {
            return { .frame = true, .forceThird = false, .mountOffsets = false };
        }
        // ⚠ THE ONE STATE WE STAND ASIDE FOR, and it is a deliberate exception
        // rather than a gap. kFree is somebody driving the camera on purpose:
        // the console's tfc, a screenshot tool, another mod's cinematic. Taking
        // it would fight them, and handing it back at the close would snap them
        // out of a shot they were composing. Note this stands down COMPLETELY,
        // which is already better than the old fall-through: that one applied
        // the framing anyway, invisibly, and still owed a restore for it.
        if (a_stateId == kFree) {
            return {};
        }
        // Everything else: take the camera, remember what it was.
        return { .frame = true,
                 .forceThird = true,
                 .mountOffsets = a_stateId == kMount };
    }

    // ── What the close hands back ─────────────────────
    //
    // ⚠⚠ THE STATE THE ARM FOUND IS NOT ALWAYS THE STATE TO RETURN TO. Field
    // 2026-09-02, a 1.1.5 reporter's diag3 log: every menu she opens comes
    // through Tab, so the engine is already in kTween, at the tween menu's own
    // field of view, when the inventory's open event reaches us. The arm found
    // kTween, ApplyFraming remembered it, and the close did SetState(kTween)
    // and wrote the tween's field of view back as the player's. kTween is a
    // state the engine leaves only when the tween menu closes, and that menu
    // was already gone. In combat nothing else moves the camera either, and
    // the watch read STILL WRONG at +8.01s. That is "the camera bugs out and
    // you cannot see your character, but you can move".
    //
    // The 22 sessions on the rig that could not reproduce it all opened on a
    // hotkey, tween closed, and found kFirstPerson or kThirdPerson: states
    // that ARE the player's, and were rightly handed back.
    struct ReturnFacts {
        // What the arm found live.
        int liveState = kFirstPerson;
        // A reading taken at the tween menu's own open edge, before the tween
        // took the camera. Bubble carries it into the session's capture.
        bool readingValid = false;
        int  readingState = kFirstPerson;
    };

    // ⚠ WHEN NOTHING KNOWS, THIRD PERSON. The engine keeps a first/third mode
    // bit on the player, but it sits in a per-runtime layout block that the
    // multi-runtime build compiles away, so there is no portable way to ask.
    // Third is the coherent blind answer: the framing already switched the
    // camera to third person, and every value the close writes back lives on
    // the third-person state object, so that state is the one those values
    // describe. A first-person player is one keypress from home; a player
    // handed kTween was stuck for good.
    [[nodiscard]] constexpr int ChooseReturnState(ReturnFacts a_facts) {
        if (a_facts.liveState != kTween) {
            return a_facts.liveState;
        }
        if (a_facts.readingValid && a_facts.readingState != kTween) {
            return a_facts.readingState;
        }
        return kThirdPerson;
    }

}  // namespace MTB::CameraArmStatePolicy

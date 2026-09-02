#pragma once

namespace MTB::WeaponStance {

    // ⚠ ONE READER FOR ONE ANSWER. Both restore sites - OwnView's own framing
    // and the Bubble's rotation park - have to agree about which stance the
    // player is in, and they run at different points of the teardown. Two
    // hand-rolled copies of "is the weapon out" would drift in that gap, which
    // is the shape [[two-readers-of-one-answer-drift-in-the-gap]] names.
    //
    // The engine types stop here on purpose: the DECISION taken from this
    // reading lives in RotationOwnershipPolicy.h, which stays free of RE:: so
    // it can be tested without Skyrim running.

    struct Reading {
        bool known = false;  // the state machine is parked, so this is comparable
        bool drawn = false;
    };

    // ⚠ ONLY kSheathed AND kDrawn ARE TERMINAL. WEAPON_STATE is a small state
    // machine and the four states between them are a clip in flight - a draw or
    // a sheathe that has started and not landed. A reading taken there answers
    // about a moment rather than a stance, and comparing two of them would call
    // a stance "changed" simply because the menu opened mid-draw. The engine
    // finishes those transitions itself and recomputes the camera flags on the
    // way out, so the honest answer mid-clip is "I cannot tell", and the caller
    // keeps whatever it was already doing.
    //
    // Same reasoning the post-close weapon-state watch in Bubble.cpp already
    // applies to its own verdict.
    [[nodiscard]] inline Reading Read(RE::Actor* a_actor) {
        auto* const state = a_actor ? a_actor->AsActorState() : nullptr;
        if (!state) {
            return {};
        }
        switch (state->GetWeaponState()) {
        case RE::WEAPON_STATE::kSheathed:
            return { .known = true, .drawn = false };
        case RE::WEAPON_STATE::kDrawn:
            return { .known = true, .drawn = true };
        default:
            return {};
        }
    }

}  // namespace MTB::WeaponStance

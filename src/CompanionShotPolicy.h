#pragma once

#include <cmath>

// When are the player and a framed companion ONE shot?
//
// Three callers ask this and they must not answer it differently. StudioRig asks
// so it can light a pair instead of a soloist, Declutter asks so it can stand
// the player down and give her the frame, and the public API asks on behalf of
// another mod offering the user a choice of who to frame. A rig lighting a
// midpoint between her and a player who is not on screen is the exact failure
// that splitting these apart would produce.
namespace MTB::CompanionShotPolicy {

    // ⚠ HORIZONTAL AND VERTICAL ARE SEPARATE QUESTIONS, and treating them as one
    // distance was a shipped bug: a follower one floor up passed a 250-unit
    // test on height alone, so the editor framed someone standing in the air
    // above the player, outside the studio space entirely. Field-reported
    // 2026-07-31 as "they can be out of the bubble and below or above".
    //
    // They are not interchangeable. Two people standing 240 units apart on the
    // same ground are a wide two-shot, which is a composition. Two people 240
    // units apart vertically are on different floors, which is not a
    // composition at any horizontal distance.
    struct Separation {
        float horizontal{ 0.0f };  // XY only
        float vertical{ 0.0f };    // absolute, so above and below read alike
    };

    [[nodiscard]] inline Separation SeparationOf(float a_dx, float a_dy, float a_dz) {
        return { std::sqrt(a_dx * a_dx + a_dy * a_dy), a_dz < 0.0f ? -a_dz : a_dz };
    }

    // How far apart two subjects may stand and still be one shot. The design was
    // worked against the measured 166 (player to follower in the editor); past
    // this the pair is not a composition, it is two people in different parts of
    // a room.
    //
    // ⚠ THIS IS ALSO THE GUARD AGAINST A STALE HANDLE. Declutter deliberately
    // never auto-clears the companion. That was harmless while the handle only
    // bought a cull exemption. It stopped being harmless when the handle began
    // to hide the PLAYER: without a bound, a follower left set and then walked
    // away from would take the player off screen in every later menu of the
    // session, on a shot the user believes is solo.
    inline constexpr float kSpreadEnter = 250.0f;
    inline constexpr float kSpreadLeave = 320.0f;

    // The vertical bound is much tighter than the horizontal one and that is the
    // point of having two. A character stands about 120 units tall. Half of that
    // covers a step, a dais or a sloping floor, where both subjects still read
    // as standing on the same ground. A full body height means her feet are
    // near the player's head, which is a different storey rather than a wider
    // shot, and interiors put roughly 200 units between floors.
    inline constexpr float kRiseEnter = 72.0f;
    inline constexpr float kRiseLeave = 108.0f;

    // ⚠ TWO THRESHOLDS PER AXIS, NOT ONE, AND THAT IS THE WHOLE POINT. A single
    // bound on a noisy signal strobes: a follower parked near it by
    // follow-distance AI or an idle shuffle crosses back and forth every frame.
    // For the rig each crossing teleports three lights and swings their radii
    // several-fold; for the player hide it blinks a character in and out of the
    // frame, which is worse than either state. Enter the two-shot below kEnter,
    // leave it only above kLeave, and the band between is dead air.
    //
    // A paused menu freezes the separation and cannot strobe on its own. Skyrim
    // Souls is why this is not merely belt and braces: under it the world runs
    // while the menu is open and both subjects really do move, stairs included.
    // Split so a caller can say WHICH bound turned a pair down. "Too far apart"
    // and "on different floors" are the same rejection to this code and
    // completely different things to whoever is reading the log, and the
    // vertical case shipped undetected precisely because nothing could say it.
    [[nodiscard]] constexpr bool CloseEnough(const Separation& a_sep, bool a_latched) {
        return a_sep.horizontal <= (a_latched ? kSpreadLeave : kSpreadEnter);
    }

    [[nodiscard]] constexpr bool SameLevel(const Separation& a_sep, bool a_latched) {
        return a_sep.vertical <= (a_latched ? kRiseLeave : kRiseEnter);
    }

    [[nodiscard]] constexpr bool InShot(const Separation& a_sep, bool a_latched) {
        return CloseEnough(a_sep, a_latched) && SameLevel(a_sep, a_latched);
    }

}  // namespace MTB::CompanionShotPolicy

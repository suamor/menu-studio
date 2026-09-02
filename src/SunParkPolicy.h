#pragma once

namespace MTB::SunParkPolicy {

    enum class Action {
        kPark,
        kRelease,
    };

    struct Input {
        // bCutSunLight. The player's own opt-out, and it wins over everything
        // below: someone who turned this off asked for their own daylight.
        bool enabled;

        // ⚠⚠ THE VOID FAMILY, AND DELIBERATELY *NOT* Settings::CellLightAllowed().
        // That gate also answers true under bStudioLightWithoutSpace, which is
        // the F-24 switch that frees the cell override into Off and Scene view
        // so the room keeps rendering while its look is standardized. Doing the
        // same to the sun is not the same act: rewriting a room restyles what
        // you can see, but taking the sun away while the world is still on
        // screen puts the whole of Skyrim in the dark. F-24's own note recorded
        // this hazard for bCutCellLights and resolved it structurally, by
        // leaving that cut inside a branch the visible modes never reach. This
        // is the same rule written as a term, because this one is reached.
        bool worldHidden;

        // Interiors are already handled and were reported working: Declutter's
        // r33 cut self-culls the cell's own NiLights and StudioLight rewrites
        // INTERIOR_DATA around them. The sun is neither of those things, so it
        // needs its own answer, and only out here.
        bool exterior;
    };

    [[nodiscard]] constexpr Action Decide(const Input& a_input) {
        return a_input.enabled && a_input.worldHidden && a_input.exterior
                   ? Action::kPark
                   : Action::kRelease;
    }

}  // namespace MTB::SunParkPolicy

#pragma once

namespace MTB::DeclutterRefPolicy {

    struct Input {
        bool isPlayer{ false };
        bool isPlayerMount{ false };
        bool isOccupiedFurniture{ false };
        bool isDisabled{ false };
        // A second actor the shot is deliberately framing (a follower being
        // dressed in another mod's outfit editor). Same standing as the mount:
        // the shot is about them, so the sweep must not hide them.
        bool isFramedCompanion{ false };
    };

    // Only inspect scene data after cheap lifetime/ownership exclusions. In
    // particular, disabled temporary references can be torn down while
    // effect-heavy armor is rebuilding the player's 3D.
    [[nodiscard]] constexpr bool ShouldInspect3D(const Input& a_input) {
        return !a_input.isPlayer &&
               !a_input.isPlayerMount &&
               !a_input.isOccupiedFurniture &&
               !a_input.isDisabled &&
               !a_input.isFramedCompanion;
    }

}  // namespace MTB::DeclutterRefPolicy

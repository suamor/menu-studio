#pragma once

namespace MTB::MenuAnimationHoldPolicy {

    struct Input {
        bool armEdgeHeld{ false };
        bool freezeDrawSheathe{ false };
        bool equipClipInFlight{ false };
        bool equipOccurredThisSession{ false };
        bool mounted{ false };
    };

    // A paused actor cannot finish the framework state changes associated with
    // an equip. Once an equip clip has activated in this menu, keep the body
    // graph on its caught frame until the world resumes. Releasing the hold
    // merely because the clip's wall-clock cap elapsed lets the next menu tick
    // select an idle against stale state and latches the lunge loop.
    //
    // ⚠ AND A RIDER IS HELD BECAUSE THE ANIMAL UNDER HER IS NOT TICKED. Nothing
    // in this plugin steps a mount's graph, so the horse is a statue for the
    // whole menu. Stepping the rider on top of that plays her mounted idle -
    // the sway, the rein hand, the settle - against a horse that never moves,
    // and the field reported exactly that: "the horse is not moving but the
    // player is idle riding", which reads worse than either being still.
    //
    // ⚠ THE HOLD IS ON THE BODY GRAPH ONLY, the same rule the framed companion
    // follows. Her face keeps ticking, so a held rider still blinks rather than
    // becoming a mannequin.
    //
    // This is the cheap half of the pair. Ticking the mount instead would keep
    // both alive and is the better answer, but it steps a second actor's graph
    // under a frozen Papyrus VM, which is what cost this project the 0.7.1
    // lunge loop. Holding costs nothing and cannot loop.
    [[nodiscard]] constexpr bool ShouldHold(const Input& a_input) {
        return a_input.armEdgeHeld || a_input.mounted ||
               (a_input.freezeDrawSheathe &&
                (a_input.equipClipInFlight || a_input.equipOccurredThisSession));
    }

}  // namespace MTB::MenuAnimationHoldPolicy

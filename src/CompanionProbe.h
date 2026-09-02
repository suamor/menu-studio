#pragma once

namespace RE {
    class Actor;
}

// Why the framed companion is a statue, measured rather than guessed.
//
// The 0.7.5 field run proved her behaviour graph is STEPPING and that she is
// not held by the player's equip latch, and she is still motionless. Between a
// stepping graph and a moving character sit three links:
//
//     graph  ->  bone LOCAL transforms  ->  bone WORLD transforms  ->  skinned mesh
//
// Nothing in the log says which one is cut. This samples one named bone on both
// sides of the middle link every armed tick and reports once, at disarm, with
// the interpretation written into the log line.
//
// It also measures the shot's geometry, because "the player obscures her" is
// currently an eyeball report and the fix depends on WHY: a player standing on
// the camera-to-her axis is a bearing problem, a player well off that axis is
// not, and those two want opposite fixes.
namespace MTB::CompanionProbe {

    // Once per armed tick, after the graph step and the propagation pass.
    void Sample(RE::Actor* a_actor);

    // One line at disarm, then forget. Silent if nothing was ever sampled.
    void Report();

    // Forget without reporting (game load).
    void Reset();

}  // namespace MTB::CompanionProbe

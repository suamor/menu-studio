#pragma once

namespace MTB::CameraCollisionPolicy {

    enum class Action {
        kRunOriginal,
        kBypass,
    };

    struct Input {
        bool enabled;
        bool bubbleActive;
        // ⚠⚠ THE ARM EDGE, AND IT IS A SEPARATE TERM BECAUSE THE PAUSE HAS NOT
        // LANDED YET WHEN IT MATTERS MOST. bubbleActive asks whether the world
        // is frozen, and the freeze is a UI queue post the engine publishes a
        // frame later than we ask for it. The arm block re-runs
        // TESCamera::Update precisely to rebuild the orbit position after a
        // wall clamped it (B-4's own fix), and it did that with the gate
        // reading FALSE, so the smoother clamped the rebuild too. The camera
        // then FREEZES there for the rest of a paused menu, because nothing
        // recomputes a third-person camera while the world is stopped.
        //
        // Measured, not inferred: the position builder (SE id 49975) derives
        // the boom from currentZoomOffset every call and the pull-in lives
        // entirely inside the smoother's obstruction test (id 49980), so ONE
        // uncollided update is all it takes to put the camera back where it
        // belongs, and one collided one is all it takes to lose it.
        //
        // Field 2026-08-16, in scene view and with declutter off alike: open a
        // menu with your back to a wall or against furniture and the camera is
        // at your face until you drag, which is the studio camera taking over
        // and writing its own transform.
        bool armWindow;
    };

    [[nodiscard]] constexpr Action Decide(const Input& a_input) {
        // ⚠ enabled STILL GATES BOTH. A player who turned the bypass off asked
        // for vanilla walls, and the arm edge is not an exception to that.
        return a_input.enabled && (a_input.bubbleActive || a_input.armWindow)
                   ? Action::kBypass
                   : Action::kRunOriginal;
    }

}  // namespace MTB::CameraCollisionPolicy

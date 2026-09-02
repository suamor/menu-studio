#pragma once

namespace MTB {
    // Vanilla third-person camera collision: ThirdPersonState's position
    // builder (RVA 0x850260, ID 49975) finishes by calling the collision
    // smoother (0x850A80, ID 49980), which runs the obstruction check
    // (0x84C870) and LERPs the camera from the last unobstructed spot toward
    // the corrected one - the visible pull-in when orbiting near walls in a
    // bubble menu (SPIM runs camera-orbit mode; declutter hides the wall's
    // RENDER but its havok stays). While the bubble is active we skip that
    // single call on SE. AE inlines the smoother, so its surviving obstruction
    // query call is gated instead and the collision memory is settled at the
    // raw orbit translation. Both leave the camera where the orbit math put it.
    // Gameplay is untouched - outside the bubble the original always runs.
    namespace CameraGate {
        void Install();  // SKSEPlugin_Load, after AllocTrampoline

        // ⚠⚠ HOLD THE GATE OPEN ACROSS ONE DELIBERATE CAMERA REBUILD, because
        // the arm edge cannot wait for the pause it just asked for. The freeze
        // is a UI queue post the engine publishes a frame later, so the arm
        // block's own TESCamera::Update - the B-4 re-extend, whose entire job is
        // to rebuild an orbit position a wall had clamped - ran with the gate
        // reading false and got clamped in turn. A third-person camera is not
        // recomputed while the world is stopped, so that one clamped rebuild is
        // what the player looks at for the whole menu: the camera at their face,
        // until a drag hands the shot to StudioCamera (field 2026-08-16, scene
        // view and declutter off alike).
        //
        // Scoped rather than a flag anyone can leave set: Held() is what the
        // policy reads, and the guard puts it back on every path.
        class ForcedBypass {
        public:
            ForcedBypass();
            ~ForcedBypass();
            ForcedBypass(const ForcedBypass&) = delete;
            ForcedBypass& operator=(const ForcedBypass&) = delete;
        };

        [[nodiscard]] bool ForcedBypassHeld();
    }
}

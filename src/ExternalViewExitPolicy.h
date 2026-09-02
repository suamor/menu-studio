#pragma once

namespace MTB::ExternalViewExitPolicy {

    enum class ReconcileTiming {
        kNone,
        kInline,
        kNextFrame,
    };

    struct DisarmInput {
        bool armed{ false };
        bool ownViewActive{ false };
    };

    struct ReconcileInput {
        bool pending{ false };
        bool menuOpen{ false };
    };

    struct ReconcileFields {
        bool zoom{ true };
        // External camera providers own live yaw. SPII restores freeRotation,
        // while SmoothCam resumes yaw after camera control is released; their
        // targetYaw is not a saved gameplay value.
        bool yaw{ false };
    };

    [[nodiscard]] constexpr ReconcileTiming ChooseDisarmTiming(
        const DisarmInput& a_input) {
        if (!a_input.armed || a_input.ownViewActive) {
            return ReconcileTiming::kNone;
        }
        // Other MenuOpenClose sinks restore their camera later in the same
        // event dispatch. Reconcile on the next frame, after every sink has
        // published its final gameplay target.
        return ReconcileTiming::kNextFrame;
    }

    [[nodiscard]] constexpr bool ShouldReconcile(const ReconcileInput& a_input) {
        return a_input.pending && !a_input.menuOpen;
    }

    [[nodiscard]] constexpr ReconcileFields ChooseReconcileFields() {
        return {};
    }

}  // namespace MTB::ExternalViewExitPolicy

#pragma once

#include <chrono>

namespace MTB::PauseLingerPolicy {

    using Clock = std::chrono::steady_clock;

    // Field-measured menu switches spend 52-71 ms with no covered menu open.
    // Keep a small wall-clock margin without making low-frame-rate users wait
    // several rendered frames before gameplay and camera updates resume.
    inline constexpr auto kMenuSwitchLinger = std::chrono::milliseconds{ 85 };

    enum class Decision {
        kKeep,
        kReleaseElapsedLinger,
        kReleaseOrphanedHold
    };

    enum class CloseDecision {
        kNoChange,
        kArmSwitchLinger,
        kReleaseExternalCamera
    };

    struct CloseInput {
        bool lastCoveredMenuClosed{ false };
        bool holdActive{ false };
        bool externalCameraHandoff{ false };
    };

    struct Input {
        bool              coveredMenuOpen{ false };
        bool              holdActive{ false };
        bool              lingerArmed{ false };
        Clock::time_point now{};
        Clock::time_point lingerUntil{};
    };

    [[nodiscard]] constexpr Decision Choose(const Input& a_input) {
        if (a_input.coveredMenuOpen) {
            return Decision::kKeep;
        }
        if (a_input.lingerArmed) {
            return a_input.now < a_input.lingerUntil ?
                       Decision::kKeep :
                       Decision::kReleaseElapsedLinger;
        }
        return a_input.holdActive ?
                   Decision::kReleaseOrphanedHold :
                   Decision::kKeep;
    }

    [[nodiscard]] constexpr CloseDecision ChooseClose(const CloseInput& a_input) {
        if (!a_input.lastCoveredMenuClosed || !a_input.holdActive) {
            return CloseDecision::kNoChange;
        }
        if (a_input.externalCameraHandoff) {
            // Rendering continues while freezeTime is held, but SmoothCam's
            // gameplay camera cannot advance. Once SPII releases control, an
            // 85 ms linger therefore exposes its restored fallback FOV.
            return CloseDecision::kReleaseExternalCamera;
        }
        return CloseDecision::kArmSwitchLinger;
    }

}  // namespace MTB::PauseLingerPolicy

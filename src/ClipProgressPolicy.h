#pragma once

namespace MTB::ClipProgressPolicy {

    // Values copied while hkbClipGenerator::Activate still guarantees that
    // the generator, its binding and its animation are alive.
    struct ActivationSample {
        float duration = 0.0f;
        float cropEnd = 0.0f;
        float localTime = 0.0f;
        float playbackSpeed = 0.0f;
    };

    // Return a self-contained graph-time snapshot. Zero deliberately means
    // "do not advance": unreadable animation data must fail closed before an
    // equip transition can cross into idle selection.
    [[nodiscard]] constexpr float CaptureRemainingGraphSeconds(
        const ActivationSample& a_sample) noexcept {
        if (!(a_sample.duration > 0.0f) || !(a_sample.playbackSpeed > 0.01f)) {
            return 0.0f;
        }
        const float clipEnd = a_sample.duration - a_sample.cropEnd;
        const float remaining =
            (clipEnd - a_sample.localTime) / a_sample.playbackSpeed;
        return (remaining > 0.0f) ? remaining : 0.0f;
    }

    // Synthetic UpdateAnimation calls advance graph time without advancing
    // wall time, so consume the stored snapshot explicitly after each call.
    [[nodiscard]] constexpr float AdvanceAfterSyntheticStep(
        float a_remaining, float a_step) noexcept {
        if (!(a_remaining > 0.0f)) {
            return 0.0f;
        }
        if (!(a_step > 0.0f)) {
            return a_remaining;
        }
        const float remaining = a_remaining - a_step;
        return (remaining > 0.0f) ? remaining : 0.0f;
    }

}  // namespace MTB::ClipProgressPolicy

#pragma once

#include <cstddef>
#include <cstdint>

// The readout logic for the post-racemenu camera watch. Engine-free on
// purpose, like every other policy header here: the part of a probe that is
// easy to get wrong is not the sampling, it is the schedule that decides when
// to speak and the sentence it finally prints. Both are tested.
//
// The question this exists to answer, and it is deliberately a narrow one:
// after the character editor closes, is ANYTHING still driving the camera
// root? Three reasoned attempts at the stranded-view exit have now failed in
// the same way (see docs/STATUS.md, 2026-08-04), which says the cause is not
// where it was being looked for. So this forms no fourth hypothesis. It
// measures who drives the node, and the findings below lead to different
// searches.
namespace MTB::CameraCloseProbePolicy {

    // Every door into the camera root this plugin can see, plus the one it
    // cannot. The first four are the sites StudioCamera already hooks, so the
    // attribution costs nothing beyond a compare - we are already standing in
    // each doorway.
    enum class Door : std::uint8_t {
        kVtable = 0,   // PlayerCamera::Update through the vtable slot
        kPlayerSite,   // the devirtualized player-update call site (AE only)
        kMasterSite,   // the devirtualized master-update call site (AE only)
        kEditor,       // RaceSexCamera::Update through ITS OWN vtable
        kForeign,      // the node moved and none of the above claimed it
        kCount
    };

    inline constexpr std::size_t kDoorCount = static_cast<std::size_t>(Door::kCount);

    [[nodiscard]] constexpr const char* DoorName(Door a_door) {
        switch (a_door) {
        case Door::kVtable:     return "vtable";
        case Door::kPlayerSite: return "player site";
        case Door::kMasterSite: return "master site";
        case Door::kEditor:     return "editor camera";
        case Door::kForeign:    return "FOREIGN";
        default:                return "?";
        }
    }

    // What the watch counted between the close edge and the verdict.
    //
    // ⚠ RAN AND WROTE ARE DIFFERENT QUESTIONS, and the first cut only asked
    // the second. A camera that ticks every frame and re-asserts the SAME
    // transform changes nothing, so a change-based detector cannot see it at
    // all - and "actively held at the editor's framing" is exactly the shape
    // this probe was built to catch. Counting thunk entries costs one
    // increment and closes that blind spot.
    struct Ledger {
        std::uint32_t ran[kDoorCount]{};     // the thunk executed
        std::uint32_t writes[kDoorCount]{};  // ...and it changed the node
        // Frames sampled, and frames on which the node moved at all. The pair
        // matters: 0 moved frames out of 88 is a completely different finding
        // from 88 out of 88, and both are silent in a per-door count.
        std::uint32_t frames = 0;
        std::uint32_t movedFrames = 0;

        constexpr void NoteRan(Door a_door) {
            const auto i = static_cast<std::size_t>(a_door);
            if (i < kDoorCount) {
                ++ran[i];
            }
        }

        constexpr void Note(Door a_door) {
            const auto i = static_cast<std::size_t>(a_door);
            if (i < kDoorCount) {
                ++writes[i];
            }
        }

        [[nodiscard]] constexpr std::uint32_t Count(Door a_door) const {
            const auto i = static_cast<std::size_t>(a_door);
            return i < kDoorCount ? writes[i] : 0u;
        }

        [[nodiscard]] constexpr std::uint32_t Ran(Door a_door) const {
            const auto i = static_cast<std::size_t>(a_door);
            return i < kDoorCount ? ran[i] : 0u;
        }

        // The three PlayerCamera doors together. They are one mechanism split
        // across SE and AE call shapes, not three findings.
        [[nodiscard]] constexpr std::uint32_t PlayerCameraWrites() const {
            return Count(Door::kVtable) + Count(Door::kPlayerSite) +
                   Count(Door::kMasterSite);
        }

        [[nodiscard]] constexpr std::uint32_t Total() const {
            return PlayerCameraWrites() + Count(Door::kEditor) +
                   Count(Door::kForeign);
        }
    };

    // ⚠ A HANDFUL OF WRITES IS AN EVENT, NOT A WRITER, and getting this wrong
    // already produced one false verdict. The 2026-08-04 control run counted
    // ONE master-site write across 88 frames with the node frozen the whole
    // time, and the first cut of this classifier called that "the engine is
    // recomputing normally, the camera is being rebuilt every frame" - a
    // sentence its own ledger contradicted on the same line. The prose was
    // always claiming a rate, so the code has to test one.
    inline constexpr std::uint32_t kSustainedDenominator = 4;  // a quarter of frames

    [[nodiscard]] constexpr bool Sustained(std::uint32_t a_writes,
                                           std::uint32_t a_frames) {
        return a_frames > 0 && a_writes * kSustainedDenominator >= a_frames;
    }

    // ⚠ FIVE FINDINGS, FIVE DIFFERENT NEXT SEARCHES. This ranking is the whole
    // value of the probe, so the order is argued rather than arbitrary:
    //
    //   kEditorStillActive - the editor's camera outlives its menu and is
    //       still being ticked. Ours to fix, most specific, and it explains
    //       the symptom exactly ("stranded in the framing that menu was
    //       using"). Ranked first, and keyed on the thunk RUNNING rather than
    //       writing, because a camera re-asserting one transform is the most
    //       likely shape and changes nothing a diff could see.
    //       ⚠ ELIMINATED BY THE 2026-08-04 BUBBLED RUN: 500 stand-downs while
    //       the menu was open, ran 0 after the close. Kept because the probe
    //       outlives one bug hunt and a regression here should still be named.
    //   kForeignWriter - the node moves and no door we stand in claimed it, so
    //       there is a writer this plugin cannot currently see. Next search is
    //       finding it, not arguing about restores.
    //   kParkedInputs - ⚠ WHAT THE 2026-08-04 BUBBLED RUN ACTUALLY MEASURED,
    //       and the first cut had no name for it: the PlayerCamera update body
    //       ran 171 times across 91 frames and moved the node ONCE. The
    //       update is alive and recomputing every frame; the recompute just
    //       yields the same value, because the camera STATE it reads from is
    //       parked at the menu framing and nothing at close restores those
    //       inputs. The node is derived - writing it back was always going to
    //       lose, which is why three node-side exit attempts failed the same
    //       way. Next search is what restores the STATE'S inputs, not the node.
    //   kEngineRecomputing - the ordinary PlayerCamera doors wrote on a real
    //       fraction of frames. The camera IS being rebuilt into new values,
    //       so neither the node nor the inputs are stale; if the shot still
    //       looks wrong, the fault is in WHAT the inputs hold, not that they
    //       are frozen.
    //   kNothingWrote - nobody even runs. The transform on screen is the last
    //       one anybody left there. ⚠ THE 2026-08-04 CONTROL LOOKED LIKE THIS
    //       from the write ledger alone (177 frames frozen, then input), but
    //       it ran on the first cut with no ran counters, so it cannot be told
    //       apart from kParkedInputs in hindsight. With ran counters the two
    //       separate cleanly, and this one now means the update body itself is
    //       not being called - a different search again (who stopped calling
    //       it), and one no run has confirmed yet.
    //
    // The verdict line prints the FULL ledger regardless, so ranking picks the
    // headline and hides nothing.
    enum class Finding : std::uint8_t {
        kEditorStillActive,
        kForeignWriter,
        kParkedInputs,
        kEngineRecomputing,
        kNothingWrote,
    };

    // The three PlayerCamera doors' RAN total, mirroring PlayerCameraWrites.
    [[nodiscard]] constexpr std::uint32_t PlayerCameraRan(const Ledger& a_ledger) {
        return a_ledger.Ran(Door::kVtable) + a_ledger.Ran(Door::kPlayerSite) +
               a_ledger.Ran(Door::kMasterSite);
    }

    [[nodiscard]] constexpr Finding Classify(const Ledger& a_ledger) {
        if (a_ledger.Ran(Door::kEditor) > 0) {
            return Finding::kEditorStillActive;
        }
        if (a_ledger.Count(Door::kForeign) > 0) {
            return Finding::kForeignWriter;
        }
        if (Sustained(a_ledger.PlayerCameraWrites(), a_ledger.frames)) {
            return Finding::kEngineRecomputing;
        }
        // Ran at a real rate without writing: alive, recomputing, parked.
        if (Sustained(PlayerCameraRan(a_ledger), a_ledger.frames)) {
            return Finding::kParkedInputs;
        }
        return Finding::kNothingWrote;
    }

    struct Schedule {
        // Per-frame and unconditional for this many frames. "The first few
        // gameplay frames" from the STATUS note - the close, the restores, and
        // the first rendered frames the player actually sees.
        std::uint32_t denseFrames = 12;
        // ⚠ EARLY, BECAUSE THE PLAYER IS THE CLOCK. The symptom's own
        // workaround is moving the camera, which both fixes the strand and
        // destroys the evidence, so the verdict has to land before a player
        // who reaches for the mouse immediately
        // ([[probe-fuse-must-outlive-the-symptom]], third clock). The control
        // run bore that out: input arrived at +1.74s.
        float verdictAt = 1.0f;
        // Long enough to catch that corrective input as a change line, so the
        // log shows what recovery looked like as well as what the strand did.
        float expireAt = 8.0f;
        // ⚠ THE VERDICT MUST NOT SPEAK DURING THE TEARDOWN, and verdictAt alone
        // cannot promise that. On the shipped defaults the teardown lands in the
        // close's own call stack, so 1.0s is comfortably clear of it - but the
        // legacy exit (any menu whose EFFECTIVE view mode is 0, which is every
        // menu in sMenus but not in sSpaceMenus) defers the view restore by at
        // least 85 ms and then runs the dissolve, and fExitHoldSeconds is a
        // user-settable delay in front of the sleek cut. Grading the state
        // mid-teardown answers a question nobody asked.
        //
        // So the verdict waits for the teardown signal plus this settle, and
        // verdictAt becomes the FLOOR rather than the trigger. A teardown that
        // never arrives still gets a verdict at the ceiling below, and says so:
        // a teardown that never ran is itself the finding.
        float settleAfterTeardown = 0.5f;
        // The backstop when no teardown signal ever comes. Well inside expireAt
        // so the verdict is still a timed line and not an expiry afterthought.
        float verdictCeiling = 3.0f;
        // A live writer with a moving value would emit a change line every
        // frame. Sixty is a second of that, which is plenty to establish a
        // rate and short of drowning the log.
        std::uint32_t emitBudget = 60;
    };

    struct TickInput {
        std::uint32_t framesSinceClose = 0;
        float         secondsSinceClose = 0.0f;
        bool          changed = false;      // node moved since the last sample
        bool          verdictDone = false;
        std::uint32_t emitted = 0;
        // Has the teardown signalled, and how long ago? teardownSeconds is only
        // read when teardownSeen is true.
        bool  teardownSeen = false;
        float teardownSeconds = 0.0f;
    };

    struct TickPlan {
        bool sample = false;
        bool verdict = false;
        bool expire = false;
    };

    [[nodiscard]] constexpr TickPlan PlanTick(const TickInput& a_in,
                                              const Schedule& a_sched = {}) {
        TickPlan plan{};
        if (a_in.secondsSinceClose >= a_sched.expireAt) {
            plan.expire = true;
            // A watch that reaches its fuse without ever having spoken is the
            // failure mode this whole file exists to avoid. Pay the verdict on
            // the way out if the timed one somehow never landed.
            plan.verdict = !a_in.verdictDone;
            return plan;
        }
        // ⚠ TWO CLOCKS, AND THE LATER ONE WINS. verdictAt is the floor (the
        // player is the other clock and reaches for the mouse early), the
        // teardown settle is the correctness gate, and verdictCeiling is what
        // stops a session whose teardown never signals from going quiet.
        if (!a_in.verdictDone) {
            const bool pastFloor = a_in.secondsSinceClose >= a_sched.verdictAt;
            const bool settled =
                a_in.teardownSeen &&
                a_in.secondsSinceClose >=
                    a_in.teardownSeconds + a_sched.settleAfterTeardown;
            const bool ceiling = a_in.secondsSinceClose >= a_sched.verdictCeiling;
            if ((pastFloor && settled) || ceiling) {
                plan.verdict = true;
            }
        }
        // ⚠ THE VERDICT IS DELIBERATELY OUTSIDE THE BUDGET. It is one line and
        // it is the line the next session greps for; a flood of change samples
        // must never be able to eat it.
        const bool dense = a_in.framesSinceClose < a_sched.denseFrames;
        if ((dense || a_in.changed) && a_in.emitted < a_sched.emitBudget) {
            plan.sample = true;
        }
        return plan;
    }

    // Translation compare in Skyrim units. The camera root moves by whole
    // units when anything real happens to it; this only has to be tighter than
    // that and looser than float noise from a recompose.
    inline constexpr float kMoveEpsilon = 0.01f;

    [[nodiscard]] constexpr bool Moved(float a_ax, float a_ay, float a_az,
                                       float a_bx, float a_by, float a_bz,
                                       float a_epsilon = kMoveEpsilon) {
        const float dx = a_ax - a_bx;
        const float dy = a_ay - a_by;
        const float dz = a_az - a_bz;
        // Squared, to keep this constexpr and free of <cmath>.
        return (dx * dx + dy * dy + dz * dz) > (a_epsilon * a_epsilon);
    }

    // ⚠ THE FOV IS A SAMPLE TRIGGER OF ITS OWN, for the same reason the UI
    // snapshot became one. The field report that put FOV on this probe is a
    // camera AND an FOV left wrong after the editor, and a translation-only
    // change test sleeps through an FOV that moves while the node holds still.
    // A trigger that cannot see half the symptom cannot measure it.
    //
    // Degrees. The framings in play here are tens of degrees apart (own view
    // arms at 90 against a menu default of 60), so a twentieth of a degree is
    // far below anything real and far above recompose noise.
    inline constexpr float kFovEpsilon = 0.05f;

    // Radians, for the angle fields (pitch, heading, free rotation). Tighter
    // than any framing offset and looser than the noise of a float copied
    // through a save and back.
    inline constexpr float kAngleEpsilon = 0.001f;

    // Zoom offsets, which are coarse.
    inline constexpr float kZoomEpsilon = 0.01f;

    // ⚠ NAMED FOR WHAT IT DOES, NOT FOR ITS FIRST CALLER. This was FovChanged,
    // and every one of its callers but one was comparing a pitch, a heading, a
    // free-rotation offset or a zoom. A widened reader would take the name at
    // face value and assume the angle comparisons were unguarded.
    [[nodiscard]] constexpr bool ScalarChanged(float a_now, float a_before,
                                               float a_epsilon) {
        const float d = a_now - a_before;
        return (d < 0.0f ? -d : d) > a_epsilon;
    }

    [[nodiscard]] constexpr bool FovChanged(float a_now, float a_before,
                                            float a_epsilon = kFovEpsilon) {
        return ScalarChanged(a_now, a_before, a_epsilon);
    }

}  // namespace MTB::CameraCloseProbePolicy

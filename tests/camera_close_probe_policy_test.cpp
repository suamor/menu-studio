#include "CameraCloseProbePolicy.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

int main() {
    using namespace MTB::CameraCloseProbePolicy;

    // ── The verdict ranking ────────────────────────────────────────────────
    //
    // An empty ledger is not "no data", it is the finding that nothing wrote -
    // which is the one that says the transform on screen is simply stale and
    // the search is why the camera stopped updating.
    CHECK(Classify(Ledger{}) == Finding::kNothingWrote);

    {
        // A real rate through the ordinary doors is the engine rebuilding the
        // shot. Half the frames, so comfortably sustained.
        Ledger l{};
        l.frames = 80;
        l.movedFrames = 40;
        for (int i = 0; i < 40; ++i) {
            l.Note(Door::kVtable);
        }
        CHECK(Classify(l) == Finding::kEngineRecomputing);
        // The three PlayerCamera doors are one mechanism across SE and AE call
        // shapes, so the rate is measured on their sum.
        Ledger m{};
        m.frames = 80;
        for (int i = 0; i < 14; ++i) {
            m.Note(Door::kMasterSite);
            m.Note(Door::kPlayerSite);
        }
        CHECK(m.PlayerCameraWrites() == 28);
        CHECK(Classify(m) == Finding::kEngineRecomputing);
    }

    {
        // ⚠ REGRESSION, FROM THE FIELD. The 2026-08-04 control run: 88 frames,
        // the node frozen the whole time, and ONE master-site write right at
        // the close. The first cut classified this as kEngineRecomputing and
        // printed "the camera is being rebuilt every frame" - a sentence its
        // own ledger contradicted on the same line. One write in 88 frames is
        // an event, not a writer.
        Ledger control{};
        control.frames = 88;
        control.movedFrames = 0;
        control.NoteRan(Door::kMasterSite);
        control.Note(Door::kMasterSite);
        CHECK(Classify(control) == Finding::kNothingWrote);
        CHECK(!Sustained(control.PlayerCameraWrites(), control.frames));
    }

    {
        // ⚠ REGRESSION, FROM THE FIELD, ROUND TWO. The 2026-08-04 BUBBLED run:
        // the master-update body ran 171 times across 91 frames and moved the
        // node exactly once. The update is alive and recomputing every frame;
        // the recompute just keeps producing the parked value, because the
        // camera state it reads is still holding the menu framing. The first
        // cut printed "no door ran at a rate that could be holding it" above a
        // ledger reading ran=171 - the sentence and the numbers on one line
        // again. This shape now has its own name.
        Ledger bubbled{};
        bubbled.frames = 91;
        bubbled.movedFrames = 0;
        for (int i = 0; i < 171; ++i) {
            bubbled.NoteRan(Door::kMasterSite);
        }
        bubbled.Note(Door::kMasterSite);
        CHECK(Classify(bubbled) == Finding::kParkedInputs);
        // The boundary logic: ran sustained, writes not.
        CHECK(Sustained(PlayerCameraRan(bubbled), bubbled.frames));
        CHECK(!Sustained(bubbled.PlayerCameraWrites(), bubbled.frames));
        // And a sustained WRITER outranks parked inputs - writes imply ran, so
        // the write test must be asked first or it could never fire.
        Ledger writer{};
        writer.frames = 80;
        for (int i = 0; i < 80; ++i) {
            writer.NoteRan(Door::kMasterSite);
        }
        for (int i = 0; i < 40; ++i) {
            writer.Note(Door::kMasterSite);
        }
        CHECK(Classify(writer) == Finding::kEngineRecomputing);
    }

    {
        // The rate boundary itself, since the whole verdict now turns on it.
        CHECK(!Sustained(21, 88));   // just under a quarter
        CHECK(Sustained(22, 88));    // exactly a quarter
        CHECK(!Sustained(0, 88));
        // No frames means no rate to speak of, and must not divide by zero
        // into a true.
        CHECK(!Sustained(0, 0));
        CHECK(!Sustained(5, 0));
    }

    {
        // A foreign write outranks the ordinary engine ones: the engine
        // recomputing is normal, a writer we cannot see is not.
        Ledger l{};
        l.frames = 80;
        for (int i = 0; i < 40; ++i) {
            l.Note(Door::kVtable);
        }
        l.Note(Door::kForeign);
        CHECK(Classify(l) == Finding::kForeignWriter);
    }

    {
        // ⚠ THE RANKING THAT MATTERS. If the editor's camera is still being
        // ticked after its menu closed, that is the headline even when foreign
        // and engine writes are also present: most specific, ours, and it
        // explains the reported symptom exactly.
        Ledger l{};
        l.frames = 80;
        for (int i = 0; i < 40; ++i) {
            l.Note(Door::kVtable);
        }
        l.Note(Door::kForeign);
        l.NoteRan(Door::kEditor);
        l.Note(Door::kEditor);
        CHECK(Classify(l) == Finding::kEditorStillActive);
        // ...and nothing is hidden by the ranking: the counts survive for the
        // line that prints them.
        CHECK(l.Count(Door::kEditor) == 1);
        CHECK(l.Count(Door::kForeign) == 1);
        CHECK(l.PlayerCameraWrites() == 40);
    }

    {
        // ⚠ THE BLIND SPOT THE RAN COUNTER EXISTS TO CLOSE. A camera that
        // ticks every frame and re-asserts the SAME transform changes nothing,
        // so every write counter reads zero and the node never moves - yet it
        // is precisely the shape that would hold a framing in place. Keyed on
        // the thunk running, this is still caught.
        Ledger held{};
        held.frames = 88;
        held.movedFrames = 0;
        for (int i = 0; i < 88; ++i) {
            held.NoteRan(Door::kEditor);
        }
        CHECK(held.Count(Door::kEditor) == 0);  // wrote nothing...
        CHECK(held.Ran(Door::kEditor) == 88);   // ...but never stopped running
        CHECK(Classify(held) == Finding::kEditorStillActive);
    }

    // ── The schedule ───────────────────────────────────────────────────────
    const Schedule sched{};

    {
        // Frame 0 samples with nothing changed and no history. The first
        // sample must be at +0.0s: a probe whose first line lands after the
        // player has already reached for the mouse measures the recovery
        // instead of the strand.
        const auto plan = PlanTick({ .framesSinceClose = 0,
                                     .secondsSinceClose = 0.0f,
                                     .changed = false },
                                   sched);
        CHECK(plan.sample);
        CHECK(!plan.verdict);
        CHECK(!plan.expire);
    }

    {
        // Past the dense window an unchanged frame is silent...
        const auto quiet = PlanTick({ .framesSinceClose = sched.denseFrames,
                                      .secondsSinceClose = 0.3f,
                                      .changed = false },
                                    sched);
        CHECK(!quiet.sample);
        // ...and a changed one still speaks, which is how the player's
        // corrective camera move shows up in the log.
        const auto moved = PlanTick({ .framesSinceClose = sched.denseFrames,
                                      .secondsSinceClose = 0.3f,
                                      .changed = true },
                                    sched);
        CHECK(moved.sample);
    }

    {
        // The verdict lands once, at its time, and a done flag silences it.
        // ⚠ A TEARDOWN IS SUPPLIED, AND IT HAS TO BE. On the shipped defaults
        // the teardown runs in the close's own call stack (t=0), so this is the
        // ordinary case, not a contrivance. Without one the verdict waits for
        // the ceiling - see the teardown block below.
        const auto due = PlanTick({ .framesSinceClose = 60,
                                    .secondsSinceClose = sched.verdictAt,
                                    .verdictDone = false,
                                    .teardownSeen = true,
                                    .teardownSeconds = 0.0f },
                                  sched);
        CHECK(due.verdict);
        const auto again = PlanTick({ .framesSinceClose = 61,
                                      .secondsSinceClose = sched.verdictAt + 0.5f,
                                      .verdictDone = true,
                                      .teardownSeen = true,
                                      .teardownSeconds = 0.0f },
                                    sched);
        CHECK(!again.verdict);
    }

    {
        // ⚠ THE VERDICT MUST NOT SPEAK DURING THE TEARDOWN. The old policy
        // fired on a fixed one-second timer, which is clear of the default
        // exit but not of the legacy one (the view restore is deferred at least
        // 85 ms and the dissolve runs after it) nor of a user-raised
        // fExitHoldSeconds. Grading the third-person state mid-restore reports
        // a fault that is about to fix itself.
        const auto midTeardown = PlanTick({ .framesSinceClose = 60,
                                            .secondsSinceClose = sched.verdictAt,
                                            .verdictDone = false,
                                            .teardownSeen = true,
                                            .teardownSeconds = 0.9f },
                                          sched);
        CHECK(!midTeardown.verdict);
        // ...and speaks once the settle has passed.
        const auto settled =
            PlanTick({ .framesSinceClose = 90,
                       .secondsSinceClose = 0.9f + sched.settleAfterTeardown + 0.01f,
                       .verdictDone = false,
                       .teardownSeen = true,
                       .teardownSeconds = 0.9f },
                     sched);
        CHECK(settled.verdict);
    }

    {
        // ⚠ THE FLOOR SURVIVES A TEARDOWN THAT SETTLES EARLY. The player is the
        // other clock: the symptom's own workaround is moving the camera, which
        // destroys the evidence, so the verdict may not be brought forward just
        // because the teardown was quick.
        const auto tooEarly = PlanTick({ .framesSinceClose = 5,
                                         .secondsSinceClose = 0.1f,
                                         .verdictDone = false,
                                         .teardownSeen = true,
                                         .teardownSeconds = 0.0f },
                                       sched);
        CHECK(!tooEarly.verdict);
    }

    {
        // ⚠ A TEARDOWN THAT NEVER SIGNALS MUST NOT SILENCE THE PROBE. That is
        // itself the finding, and a watch that goes quiet because it is waiting
        // for a signal nobody sends is the exact failure this file exists to
        // avoid. The ceiling pays it, well inside the expiry.
        const auto waiting = PlanTick({ .framesSinceClose = 100,
                                        .secondsSinceClose =
                                            sched.verdictCeiling - 0.1f,
                                        .verdictDone = false },
                                      sched);
        CHECK(!waiting.verdict);
        const auto ceiling = PlanTick({ .framesSinceClose = 120,
                                        .secondsSinceClose = sched.verdictCeiling,
                                        .verdictDone = false },
                                      sched);
        CHECK(ceiling.verdict);
        // And the ceiling is inside the window, so it is a timed verdict rather
        // than an expiry afterthought.
        CHECK(sched.verdictCeiling < sched.expireAt);
    }

    {
        // ⚠ REGRESSION: a live writer emits a change line every frame and
        // would exhaust the budget in about a second. The budget must stop the
        // SAMPLES and never the verdict - the verdict is the line the next
        // session greps for, and a flood eating it would waste the field run.
        const auto plan = PlanTick({ .framesSinceClose = 200,
                                     .secondsSinceClose = sched.verdictAt,
                                     .changed = true,
                                     .verdictDone = false,
                                     .emitted = sched.emitBudget,
                                     .teardownSeen = true,
                                     .teardownSeconds = 0.0f },
                                   sched);
        CHECK(!plan.sample);
        CHECK(plan.verdict);
    }

    {
        // ⚠ REGRESSION: reaching the fuse having never spoken is the failure
        // this probe was written to avoid. Expiry pays an unpaid verdict on
        // the way out...
        const auto silent = PlanTick({ .framesSinceClose = 500,
                                       .secondsSinceClose = sched.expireAt,
                                       .verdictDone = false },
                                     sched);
        CHECK(silent.expire);
        CHECK(silent.verdict);
        // ...and does not print a second one when it already landed.
        const auto spoken = PlanTick({ .framesSinceClose = 500,
                                       .secondsSinceClose = sched.expireAt + 2.0f,
                                       .verdictDone = true },
                                     sched);
        CHECK(spoken.expire);
        CHECK(!spoken.verdict);
        CHECK(!spoken.sample);
    }

    // ── A run, rather than a set of isolated answers ───────────────────────
    {
        // Drive the schedule the way the frame loop will: a strand, where
        // nothing moves at all, then the player nudges the camera at ~2s.
        std::uint32_t emitted = 0;
        std::uint32_t verdicts = 0;
        bool          verdictDone = false;
        bool          watching = true;
        std::uint32_t denseSamples = 0;

        for (std::uint32_t frame = 0; watching && frame < 1000; ++frame) {
            const float seconds = static_cast<float>(frame) / 60.0f;
            const bool  nudged = (frame == 120);  // the corrective mouse move
            const auto  plan = PlanTick({ .framesSinceClose = frame,
                                          .secondsSinceClose = seconds,
                                          .changed = nudged,
                                          .verdictDone = verdictDone,
                                          .emitted = emitted },
                                        sched);
            if (plan.sample) {
                ++emitted;
                if (frame < sched.denseFrames) {
                    ++denseSamples;
                }
            }
            if (plan.verdict) {
                ++verdicts;
                verdictDone = true;
            }
            if (plan.expire) {
                watching = false;
            }
        }

        // Every dense frame spoke, the verdict landed exactly once, the
        // player's nudge was captured, and the watch closed itself.
        CHECK(denseSamples == sched.denseFrames);
        CHECK(verdicts == 1);
        CHECK(emitted == sched.denseFrames + 1);  // dense window + the nudge
        CHECK(!watching);
    }

    // ── The movement compare ───────────────────────────────────────────────
    {
        // Float noise from a recompose is not a write.
        CHECK(!Moved(10.0f, 20.0f, 30.0f, 10.0f, 20.0f, 30.0f));
        CHECK(!Moved(10.0f, 20.0f, 30.0f, 10.0f, 20.0f, 30.001f));
        // A real camera move is whole units.
        CHECK(Moved(10.0f, 20.0f, 30.0f, 10.0f, 20.0f, 31.0f));
        // Each axis counts on its own.
        CHECK(Moved(10.0f, 20.0f, 30.0f, 11.0f, 20.0f, 30.0f));
        CHECK(Moved(10.0f, 20.0f, 30.0f, 10.0f, 21.0f, 30.0f));
    }

    // ── The FOV compare ────────────────────────────────────────────────────
    {
        // A recompose that lands on the same value is not a change.
        CHECK(!FovChanged(60.0f, 60.0f));
        CHECK(!FovChanged(60.0f, 60.01f));
        // Symmetric: which way it moved is not the question.
        CHECK(!FovChanged(60.01f, 60.0f));
        // The two framings this probe exists to tell apart.
        CHECK(FovChanged(90.0f, 60.0f));
        CHECK(FovChanged(60.0f, 90.0f));
    }

    if (g_failures == 0) {
        std::printf("camera_close_probe_policy_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

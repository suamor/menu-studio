#include "MountedSubjectPolicy.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

static bool Near(float a_a, float a_b, float a_tol = 0.05f) {
    return std::fabs(a_a - a_b) <= a_tol;
}

int main() {
    namespace P = MTB::MountedSubjectPolicy;
    using P::Inputs;
    using P::Measure;

    // ── On foot, which must not move ────────────────────────────────────────
    // Measured 2026-08-12 20:37:43 on the dev rig, character 'Umbrael':
    // crown z 181.8, body height 117.5. The origin is the ground she stands on,
    // so the column is just crown minus origin and nothing here has an opinion.
    {
        const auto column = Measure(Inputs{ .riderOriginZ = 64.3f,
                                            .crownZ = 181.8f,
                                            .hasMount = false });
        CHECK(Near(column.groundZ, 64.3f));
        CHECK(Near(column.height, 117.5f));
        CHECK(column.plausible);
        CHECK(!column.fromMount);
        CHECK(column.mountDeclined == nullptr);
    }

    // ── The defect, measured ────────────────────────────────────────────────
    // Same session, 20:38:16, same character on a Wild Horse. GetPosition()
    // answers the SADDLE, so crown minus origin is 50.9 where the same body
    // read 117.5 on foot five minutes earlier. ⚠ 50.9 clears the 32..512 accept
    // range, so the old measurement declined nothing and reported nothing. This
    // case exists to pin that the halving is a silent pass, which is why it
    // needed a field run rather than a warning to find.
    {
        const auto column = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                            .crownZ = -3210.1f,
                                            .hasMount = false });
        CHECK(Near(column.height, 50.9f));
        CHECK(column.plausible);  // ⚠ the bug: nothing rejects it
    }

    // ── The fix ─────────────────────────────────────────────────────────────
    // ⚠ THE MOUNT ORIGIN BELOW IS CONSTRUCTED, NOT MEASURED. The 12 August run
    // read the rider's origin and never read the horse's, so the drop from
    // saddle to hooves is still an unknown this project has not put a number
    // on. These cases test the arithmetic and the refusals, not a horse.
    {
        const auto column = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                            .crownZ = -3210.1f,
                                            .hasMount = true,
                                            .mountOriginKnown = true,
                                            .mountOriginZ = -3355.0f });
        CHECK(Near(column.groundZ, -3355.0f));
        CHECK(Near(column.height, 144.9f));  // the whole column, hooves to crown
        CHECK(column.plausible);
        CHECK(column.fromMount);
        CHECK(column.mountDeclined == nullptr);
    }

    // ── A mount that answers with nonsense ──────────────────────────────────
    // A saddle sits ABOVE the feet of the thing carrying it. An origin level
    // with the rider's or above it is not a ground, it is a stale ref, a
    // handle to the wrong actor, or a mount whose position has not been
    // updated this frame. Refuse it and say so rather than composing a shot
    // around a negative or zero-length column.
    {
        constexpr float kBadOrigins[] = { -3261.0f, -3200.0f, 0.0f };
        for (const float bad : kBadOrigins) {
            const auto column = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                                .crownZ = -3210.1f,
                                                .hasMount = true,
                                                .mountOriginKnown = true,
                                                .mountOriginZ = bad });
            CHECK(!column.fromMount);
            CHECK(column.mountDeclined != nullptr);
            CHECK(Near(column.groundZ, -3261.0f));  // back to the rider's origin
        }
    }

    // A mount far enough below to put the column outside the accept range is
    // the same class of wrong answer, and it must not be allowed through just
    // because it is on the correct side. 512 is the existing ceiling.
    {
        const auto column = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                            .crownZ = -3210.1f,
                                            .hasMount = true,
                                            .mountOriginKnown = true,
                                            .mountOriginZ = -4000.0f });
        CHECK(!column.fromMount);
        CHECK(column.mountDeclined != nullptr);
        CHECK(Near(column.groundZ, -3261.0f));
    }

    // ── Mounted, and the animal could not be asked ──────────────────────────
    // Mid mount-up, an empty handle, or `IsOnMount()` and `GetMount()`
    // disagreeing, which nothing in this codebase has ever checked. ⚠ THIS MUST
    // NOT LOOK LIKE THE ON-FOOT CASE. Both measure from the rider's origin and
    // both come out plausible; only one of them is right to. Without the
    // refusal there is no way to tell a rider on the ground from a rider whose
    // horse went unread, which is the T2 trap in one line: a physically mounted
    // player measured with no mount term and nothing saying so.
    {
        const auto unread = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                           .crownZ = -3210.1f,
                                           .hasMount = true,
                                           .mountOriginKnown = false });
        const auto onFoot = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                            .crownZ = -3210.1f,
                                            .hasMount = false });
        CHECK(Near(unread.height, onFoot.height));
        CHECK(unread.plausible && onFoot.plausible);
        CHECK(!unread.fromMount);
        CHECK(unread.mountDeclined != nullptr);   // the one difference
        CHECK(onFoot.mountDeclined == nullptr);
    }

    // ⚠ AND THE FALLBACK IS NOT A SUCCESS. Falling back to the rider's origin
    // on a mounted subject gives the halved column the whole fix exists to
    // remove, and it passes `plausible` while doing it. The caller has to be
    // able to tell "measured from the ground" from "measured from the saddle
    // because the mount refused", which is what mountDeclined carries.
    {
        const auto column = Measure(Inputs{ .riderOriginZ = -3261.0f,
                                            .crownZ = -3210.1f,
                                            .hasMount = true,
                                            .mountOriginKnown = true,
                                            .mountOriginZ = 0.0f });
        CHECK(column.plausible);
        CHECK(!column.fromMount);
        CHECK(column.mountDeclined != nullptr);
    }

    // ── The existing proportion gate, unchanged either side of a mount ──────
    {
        const auto tiny = Measure(Inputs{ .riderOriginZ = 0.0f, .crownZ = 31.0f });
        CHECK(!tiny.plausible);
        const auto huge = Measure(Inputs{ .riderOriginZ = 0.0f, .crownZ = 513.0f });
        CHECK(!huge.plausible);
        const auto low = Measure(Inputs{ .riderOriginZ = 0.0f, .crownZ = 32.0f });
        CHECK(low.plausible);
        const auto high = Measure(Inputs{ .riderOriginZ = 0.0f, .crownZ = 512.0f });
        CHECK(high.plausible);
    }

    // A crown below the origin is a broken skeleton read, not a short
    // character. Negative lengths must never reach the composition.
    {
        const auto inverted =
            Measure(Inputs{ .riderOriginZ = 100.0f, .crownZ = 40.0f });
        CHECK(!inverted.plausible);
    }

    // ── The mounted aim ─────────────────────────────────────────────────────
    // Measured 2026-08-13, dev rig, defaults: over-shoulder Z 96.3, boom 195,
    // base pitch 0.1, and the rider's crown 50.8 above her ref.
    {
        const float sz = 96.3f;
        const float boom = 195.0f;
        const float base = 0.1f;
        const float lens = sz + P::kOverShoulderBaseZ;  // 85.3 above the ref
        const float crown = 50.8f;

        // Where an aim of a given total pitch lands, in units above the ref.
        const auto aimZ = [&](float a_totalPitch) {
            return lens - std::tan(a_totalPitch) * boom;
        };

        // ⚠ THE DEFECT, PINNED. With no mounted pitch at all the shot aims
        // ABOVE her head, which is the empty sky in both field screenshots.
        CHECK(aimZ(base) > crown);
        CHECK(Near(aimZ(base), 65.7f, 0.2f));

        // The fix lands the aim exactly where it was asked to.
        const float extra = P::AimPitch(sz, boom, base);
        CHECK(Near(aimZ(base + extra), P::kRiderAimAboveRef, 0.05f));
        CHECK(aimZ(base + extra) < crown);  // on the column, not over her head
        CHECK(Near(extra, 0.472f, 0.005f));
        CHECK(extra > 0.0f);  // down, never up

        // ⚠ AND IT AIMS BELOW THE SADDLE, WHICH LOOKS WRONG AND IS NOT. The
        // subject is the rider AND the animal, a column running from the ground
        // 129.6 below the ref up to her crown 48.5 above it. Its middle is
        // under her. An aim on the rider alone leaves the horse eating the
        // bottom of the frame and her pinned near the floor of it, which is
        // what the field saw after the first cut.
        CHECK(P::kRiderAimAboveRef < 0.0f);
        CHECK(Near(P::kRiderAimAboveRef, (48.5f - 129.6f) * 0.5f, 0.1f));

        // ⚠ AND IT SELF-CORRECTS, WHICH IS THE POINT. Raising the mount raise
        // is on the do-not-retry list because it used to push the subject
        // further out of frame. It cannot any more: a higher lens pitches
        // further and the aim stays on her.
        constexpr float kRaises[] = { 96.3f, 140.0f, 200.0f, 260.0f };
        for (const float raised : kRaises) {
            const float e = P::AimPitch(raised, boom, base);
            const float landed = (raised + P::kOverShoulderBaseZ) -
                                 std::tan(base + e) * boom;
            CHECK(Near(landed, P::kRiderAimAboveRef, 0.05f));
        }
        CHECK(P::AimPitch(200.0f, boom, base) > P::AimPitch(96.3f, boom, base));

        // A longer boom needs less angle for the same drop.
        CHECK(P::AimPitch(sz, 400.0f, base) < P::AimPitch(sz, 195.0f, base));

        // A boom that cannot describe a shot asks for no change rather than
        // pointing the camera at the dirt.
        CHECK(Near(P::AimPitch(sz, 0.0f, base), 0.0f));
        CHECK(Near(P::AimPitch(sz, -50.0f, base), 0.0f));
    }

    // ── What a widening shot gives way to ───────────────────────────────────
    // Measured 2026-08-13 on a head focus: column 180 tall, so half is 90; the
    // column's middle sat at 0.49 of the frame while her eye line sat at 0.93.
    {
        const float half = 90.0f;
        const float columnMid = -3297.0f;
        const float riderMid = columnMid + 64.0f;  // crown 48.5 up, half of it

        // A frame that can hold the whole column gives way to the column. That
        // is the widest stop, and it is the one shot where both belong.
        CHECK(Near(P::GiveWayTarget(riderMid, columnMid, half, half), columnMid));
        CHECK(Near(P::GiveWayTarget(riderMid, columnMid, 2.0f * half, half),
                   columnMid));

        // A frame that can hold none of it gives way to the rider.
        CHECK(Near(P::GiveWayTarget(riderMid, columnMid, 0.0f, half), riderMid));

        // ⚠ AND EVERY SHOT IN BETWEEN LEANS ON HER, WHICH IS THE WHOLE POINT.
        // Half a frame was the case that read as "the camera is quite low": the
        // aim used to sit on the column's middle there.
        const float mid = P::GiveWayTarget(riderMid, columnMid, 0.5f * half, half);
        CHECK(mid > columnMid);              // above the horse
        CHECK(mid < riderMid);               // not all the way to her yet
        CHECK(Near(mid, columnMid + 32.0f));  // exactly half the way

        // Monotone, so widening never jerks the aim back down.
        float previous = riderMid + 1.0f;
        for (float span = 0.0f; span <= half + 1e-3f; span += 0.05f * half) {
            const float target =
                P::GiveWayTarget(riderMid, columnMid, span, half);
            CHECK(target <= previous + 1e-3f);
            previous = target;
        }

        // A subject with no measured height cannot be leaned toward.
        CHECK(Near(P::GiveWayTarget(riderMid, columnMid, 50.0f, 0.0f), columnMid));
    }

    // ── The face anchor ─────────────────────────────────────────────────────
    // Not a policy call: the rule lives in FocusAnchorZ because it needs the
    // subject's ref. Pinned here as arithmetic because it took two wrong shapes
    // to find, and both of them looked reasonable written down.
    //
    // Measured 2026-08-13: mounted, the column reads 178.2 with the ground 129.6
    // below her ref, so her own span above the saddle is 48.6. On foot the same
    // character measures 118.2.
    {
        const float column = 178.2f;
        const float drop = 129.6f;
        const float seated = column - drop;
        const float face = 0.93f;  // fCameraTrackFaceHeight

        CHECK(Near(seated, 48.6f, 0.1f));

        // The rule: the face sits `face` of the way up the subject's own span
        // above the surface they are on. Ref at 0 keeps the arithmetic plain.
        const float mounted = face * seated;                 // above the saddle
        const float onFoot = face * 118.2f;                  // above the ground
        CHECK(Near(mounted, 45.2f, 0.1f));
        CHECK(Near(onFoot, 109.9f, 0.1f));

        // ⚠ BOTH EARLIER SHAPES PINNED, so neither comes back. Read against the
        // whole column the anchor lands 12.5 under the crown, on her chin; the
        // rule above lands 3.4 under it, which is where her eyes actually are.
        const float crownAboveRef = seated;
        CHECK(Near(crownAboveRef - face * column, -117.1f, 0.5f));  // absurd
        CHECK(Near(crownAboveRef - mounted, 3.4f, 0.1f));

        // And it cannot be dragged by the animal: a taller horse changes the
        // column and the drop together, leaving her span and her face alone.
        const float tallerSeated = (column + 40.0f) - (drop + 40.0f);
        CHECK(Near(tallerSeated, seated));
        CHECK(Near(face * tallerSeated, mounted));
    }

    if (g_failures == 0) {
        std::printf("mounted_subject_policy_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

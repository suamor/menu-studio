#pragma once

#include <cmath>

// Where the framed subject's ground is, and how tall the thing being framed
// therefore is.
//
// ⚠⚠ A MOUNTED RIDER'S GetPosition() IS THE SADDLE, NOT THE GROUND. Measured on
// the dev rig 2026-08-12, one character, five minutes apart:
//
//     on foot   crown z  181.8   body height 117.5   standing   5.9
//     on a horse crown z -3210.1  body height  50.9   standing -46.6
//
// Every length in the studio camera is crown minus origin, so an origin at the
// seat does not shift the composition, it HALVES the character. The shot is then
// built for a 51-unit subject: the eye line lands on the saddle, the body middle
// on the horse's back, the pan reach comes out half, and the arm opens pegged at
// the far stop with nothing left to pull. Those read as five defects and they are
// one wrong number.
//
// ⚠ THE TRAP THAT KEPT THIS HIDDEN IS THAT 50.9 IS A LEGAL ANSWER. The accept
// range is 32 to 512 and always has been, so the measurement declined nothing,
// warned about nothing, and logged a body height that looked like a body height.
// It took a field reading to see it, and the reading had never been taken. The
// range is kept here unchanged for exactly that reason: widening or narrowing it
// was never the fix.
//
// The mount's own ref is on the ground the way any actor's is, so a mounted
// subject's ground comes from the mount. ⚠ THAT IS THE ONE CLAIM HERE NOBODY HAS
// MEASURED. The 12 August run read the rider's origin and never read the horse's,
// so the drop from saddle to hooves has no number against it yet. The refusals
// below are shaped for that: a mount origin is used only when it is on the
// correct side of the rider and yields a column the existing range accepts, and
// when it is refused the caller is told, because falling back to the rider's
// origin quietly restores the very halving this exists to remove.
//
// This answers "is there a horse under the player and where does it stand",
// which is a question about the world. It deliberately does NOT key on the
// camera state: `kMount` answers "which camera must I force out of and hand
// back", a different question, and only the actor side can produce the mount
// whose position this needs. See the T2 note in the mounted handoff.

namespace MTB::MountedSubjectPolicy {

    // The proportions a measurement has to fall inside to be believed. Unchanged
    // from the range the studio camera has always used; see the header note on
    // why widening it is not the fix.
    inline constexpr float kMinHeight = 32.0f;
    inline constexpr float kMaxHeight = 512.0f;

    struct Inputs {
        // The rider's own ref origin. On foot this is the ground; mounted it is
        // the saddle, which is the whole reason this file exists.
        float riderOriginZ = 0.0f;
        // The top of the subject, already resolved from bones by the caller.
        float crownZ = 0.0f;
        // Actor state, not camera state. True whenever there is an animal under
        // the subject, whether or not anything could be read off it.
        bool hasMount = false;
        // False when there IS a mount and its position could not be read: the
        // handle came back empty, or the actor is mid mount-up. ⚠ THAT CASE HAS
        // TO BE TOLD APART FROM "no mount", because both end up measuring from
        // the rider's origin and only one of them is correct to do so.
        bool mountOriginKnown = false;
        // Only read when both flags above are set.
        float mountOriginZ = 0.0f;
    };

    struct Column {
        // What the composition should treat as the subject's feet.
        float groundZ = 0.0f;
        // groundZ to crown. This is the whole column that has to fit in frame,
        // so mounted it spans hooves to the rider's head rather than saddle to
        // head.
        float height = 0.0f;
        // Inside kMinHeight..kMaxHeight. A caller that gets false should hold
        // its previous measurement rather than compose against this one.
        bool plausible = false;
        // True only when the mount supplied the ground. False on foot AND on a
        // refused mount, so it is not a "was mounted" flag; it is a "this
        // measurement is off the ground" flag.
        bool fromMount = false;
        // Non-null only when a mount was present and its origin was refused.
        // ⚠ A refusal leaves a MOUNTED subject measured from the saddle, which
        // is the original defect wearing the fix's clothes. It is reported so
        // the log can say so instead of showing a plausible-looking half body.
        const char* mountDeclined = nullptr;
    };

    // ── The mounted framing's pitch ─────────────────────────────────────────
    //
    // ⚠ THE RAISE MOVES THE CAMERA AND LEAVES THE AIM LEVEL, WHICH IS WHY THE
    // RIDER SITS LOW WITH A SKY ABOVE HER. `fOwnViewMountRaise` is a camera
    // POSITION offset, so +120 lifts the lens and the subject falls toward the
    // bottom of the frame. Two field screenshots show exactly that, cropped at
    // the shoulders with empty air over the head, and the raise is on the
    // do-not-retry list precisely because dialling it up makes it worse.
    //
    // Measured on the dev rig 2026-08-13, mounted, defaults:
    //
    //     camera height above the rider's ref   ~85   (sz 96.3, so the
    //                                                  over-shoulder base is
    //                                                  about -11)
    //     rider's crown above the same ref       50.8
    //     boom                                  195
    //     pitch applied                          0.1 rad
    //
    // 0.1 rad over a 195 boom drops the aim about 19 units, landing it at +66
    // when the crown is at +50.8. The shot is aimed a head and a half ABOVE
    // her. Nothing was wrong with the raise; the aim never followed it.
    //
    // So the pitch is derived from where the lens actually is rather than
    // dialled: aim at a point on the rider's upper body and let the geometry
    // say what angle that is. It self-corrects, which is the part that matters
    // - change the raise or the boom and the aim stays on her, instead of the
    // knob quietly breaking the framing the way it does today.
    //
    // The over-shoulder Z is not measured from the ref: it is an offset from
    // the third-person camera's own default perch, so the base below is the
    // measured difference between the two rather than a guess.
    inline constexpr float kOverShoulderBaseZ = -11.0f;
    // Where the shot should land, in units above the rider's ref. NEGATIVE:
    // the aim belongs BELOW the saddle.
    //
    // ⚠ THE FIRST CUT AIMED AT HER UPPER BODY AND SHE WAS STILL LOW IN FRAME.
    // The field moved by the 32 units that cut predicted, which is how we know
    // the mechanism was right and the target was wrong. The subject here is not
    // a rider. It is a rider AND the animal under her, one column 178.6 units
    // tall, and a column is framed at its middle.
    //
    // Measured 2026-08-13: the ground is 129.6 below the ref and her crown is
    // 48.5 above it, so the middle of what has to fit sits at
    // (48.5 - 129.6) / 2, a little over 40 below the saddle she is sitting on.
    inline constexpr float kRiderAimAboveRef = -40.6f;

    // ⚠ THE FACE ANCHOR IS NOT IN THIS FILE, AND ONE ROUND WAS SPENT PUTTING IT
    // HERE. It went through two wrong shapes first, both of which invented a
    // number: a fraction of the whole rider-and-horse column, which slid the
    // anchor down to her chin, and then a drop below the crown scaled by a
    // recovered standing height, which assumed the crown is the top of her head.
    // It is not. This skeleton's head cluster answers around her EYES, measured
    // 2026-08-13: her eyes sat 9.5 units ABOVE an anchor placed 8.4 under the
    // measured crown.
    //
    // The rule that works needs no constant at all, because the standing case
    // already had it: the face sits a stated fraction of the way up the
    // subject's own span above whatever they are standing on. On foot that
    // surface is the ground; mounted it is the saddle. See FocusAnchorZ.

    // Half a seated rider, in units. Her crown measured 48.5 above her ref on
    // 2026-08-13 and she is sitting on that ref, so her own middle is about
    // half of that above it.
    inline constexpr float kRiderSeatedHalfHeight = 24.0f;

    // What a widening shot gives way to, mounted.
    //
    // ⚠ THE WIDE SHOT AND THE CLOSE SHOT WANT DIFFERENT SUBJECTS, AND USING ONE
    // FOR BOTH IS WHAT PUT HER FACE ON THE TOP EDGE. ComposeShot lets the aim
    // sit away from the body's middle only as far as the frame has room for,
    // which is right, but mounted "the body" was taken to be the whole
    // rider-and-horse column. Measured on a head focus: the column's middle sat
    // at 0.49 of the frame and her eye line at 0.93. The shot was centred, on
    // the horse.
    //
    // A frame that cannot hold the column is not framing a column, it is
    // framing the rider, so that is what it gives way to. The target slides to
    // the column's middle exactly as the frame grows able to hold it, which
    // leaves the widest stop showing both and every closer shot on her.
    [[nodiscard]] inline float GiveWayTarget(float a_riderMid, float a_columnMid,
                                             float a_halfSpan,
                                             float a_halfHeight) {
        if (a_halfHeight <= 0.0f) {
            return a_columnMid;
        }
        const float held = a_halfSpan / a_halfHeight;
        const float w = held < 0.0f ? 0.0f : (held > 1.0f ? 1.0f : held);
        return a_riderMid + (a_columnMid - a_riderMid) * w;
    }

    // The EXTRA pitch the mounted framing needs, in radians, on top of the
    // standing look's own base. Positive is down.
    //
    // ⚠ IT RETURNS A DELTA, NOT A TOTAL, BECAUSE THE CALLER ALREADY APPLIES A
    // BASE. Returning the total and letting it be added would double the base
    // in: 0.1 on top of a computed 0.25 aims about 22 units under the mark,
    // which is the same class of error as the one this fixes, pointed the
    // other way.
    [[nodiscard]] inline float AimPitch(float a_overShoulderZ, float a_boom,
                                        float a_basePitch) {
        // A boom that is zero or negative cannot describe a shot. Asking for
        // no change is the safe direction: a wrong pitch points the camera at
        // the sky or the dirt, where none merely repeats today's framing.
        if (a_boom <= 1.0f) {
            return 0.0f;
        }
        const float rise = a_overShoulderZ + kOverShoulderBaseZ - kRiderAimAboveRef;
        return std::atan2(rise, a_boom) - a_basePitch;
    }

    [[nodiscard]] constexpr Column Measure(const Inputs& a_in) {
        const auto measureFrom = [&a_in](float a_groundZ) {
            const float height = a_in.crownZ - a_groundZ;
            return Column{ .groundZ = a_groundZ,
                           .height = height,
                           .plausible =
                               height >= kMinHeight && height <= kMaxHeight,
                           .fromMount = false,
                           .mountDeclined = nullptr };
        };

        if (!a_in.hasMount) {
            return measureFrom(a_in.riderOriginZ);
        }

        // Mounted, but the animal could not be asked. Mid mount-up, a handle
        // that resolved to nothing, or `IsOnMount()` and `GetMount()`
        // disagreeing, which this codebase has assumed they never do without
        // ever checking. The measurement below is the saddle-based one either
        // way; the difference is that this one knows it and says so.
        if (!a_in.mountOriginKnown) {
            Column fallback = measureFrom(a_in.riderOriginZ);
            fallback.mountDeclined = "its position could not be read";
            return fallback;
        }

        // A saddle sits above the feet of whatever carries it. An origin level
        // with the rider's or above it is not a ground: it is a stale ref, the
        // wrong actor, or a mount whose position has not been updated this
        // frame. None of those should reach a composition.
        if (!(a_in.mountOriginZ < a_in.riderOriginZ)) {
            Column fallback = measureFrom(a_in.riderOriginZ);
            fallback.mountDeclined = "its origin is not below the rider's";
            return fallback;
        }

        Column mounted = measureFrom(a_in.mountOriginZ);
        // Far enough below to break the proportions is the same class of wrong
        // answer as being above, and being on the correct side does not earn it
        // a pass.
        if (!mounted.plausible) {
            Column fallback = measureFrom(a_in.riderOriginZ);
            fallback.mountDeclined = "the column it gives is out of proportion";
            return fallback;
        }
        mounted.fromMount = true;
        return mounted;
    }

}  // namespace MTB::MountedSubjectPolicy

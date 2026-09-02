#pragma once

#include <algorithm>
#include <cmath>

// The pivot camera's arithmetic, with no engine types in it so the whole thing
// can be tested off-engine.
//
// The camera holds a world point and sits on a sphere around it. Zooming toward
// a face and orbiting a subject are then the same operation with different
// inputs, and neither cares what the character underneath is doing. Driving the
// engine's own boom cannot express either: that boom runs along the
// over-shoulder axis, which is anchored to the player's heading, so it slides
// rather than orbits and the frame swings off the subject the moment they turn.
namespace MTB::StudioCameraPolicy {

    struct Vec3 {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    // Row-major, column vectors, matching NiMatrix3 so the engine layer can copy
    // entries straight across: result[i] = sum_j m[i][j] * p[j].
    struct Mat3 {
        float m[3][3]{};
    };

    // A ball in some frame. Used for "everything hanging off this node",
    // measured once and then carried by the node's own transform.
    struct Sphere {
        Vec3  centre;
        float radius{ 0.0f };
    };

    // The smallest sphere containing both, with a zero radius meaning "nothing
    // measured yet" so a fold can start from Sphere{}.
    //
    // ⚠ THE CONTAINMENT TESTS COME FIRST AND THEY ARE ALSO THE ZERO-DISTANCE
    // GUARD. Two geometries authored about the same origin, which a weapon and
    // its own scabbard routinely are, give a distance of zero; the containment
    // branch answers before anything divides by it.
    [[nodiscard]] inline Sphere UnionSpheres(const Sphere& a_a, const Sphere& a_b) {
        if (a_a.radius <= 0.0f) {
            return a_b;
        }
        if (a_b.radius <= 0.0f) {
            return a_a;
        }
        const Vec3  d{ a_b.centre.x - a_a.centre.x, a_b.centre.y - a_a.centre.y,
                       a_b.centre.z - a_a.centre.z };
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist + a_b.radius <= a_a.radius) {
            return a_a;
        }
        if (dist + a_a.radius <= a_b.radius) {
            return a_b;
        }
        const float r = (dist + a_a.radius + a_b.radius) * 0.5f;
        const float t = (r - a_a.radius) / dist;
        return Sphere{ { a_a.centre.x + d.x * t, a_a.centre.y + d.y * t,
                         a_a.centre.z + d.z * t },
                       r };
    }

    // Where the camera sits relative to the pivot. Skyrim is X east, Y north,
    // Z up, and yaw is measured from north so it composes with the game's own
    // heading convention without a correction term.
    struct Orbit {
        float distance{ 0.0f };
        float yaw{ 0.0f };    // radians, about Z
        float pitch{ 0.0f };  // radians, positive looks down from above
    };

    // ⚠ THE PITCH CLAMP IS NOT A TASTE SETTING. At exactly +-90 degrees the
    // view direction is parallel to world up, the cross product that builds the
    // camera's right vector collapses to zero length, and the basis below is
    // undefined. Stopping short of it is what keeps the matrix well formed, so
    // this bound belongs here rather than in the settings.
    inline constexpr float kPitchLimit = 1.35f;  // ~77 degrees

    // Near bound keeps the lens outside the head it is pointed at; far bound is
    // generous because a dressing room shot wants a full-length option.
    inline constexpr float kMinDistance = 25.0f;
    inline constexpr float kMaxDistance = 900.0f;

    [[nodiscard]] inline float Length(const Vec3& a_v) {
        return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
    }

    [[nodiscard]] inline Vec3 Cross(const Vec3& a, const Vec3& b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    [[nodiscard]] inline Vec3 Normalized(const Vec3& a_v) {
        const float len = Length(a_v);
        if (len <= 1e-6f) {
            return { 0.0f, 1.0f, 0.0f };
        }
        return { a_v.x / len, a_v.y / len, a_v.z / len };
    }

    // The same angle written so it sits within half a turn of a reference.
    // ⚠ THE EASING IS PLAIN ARITHMETIC AND KNOWS NOTHING ABOUT ANGLES: told
    // to go from 3.0 to -3.0 radians it walks the long way round, spinning
    // the shot most of a full turn to reach a heading a hair away. Anything
    // that sets a yaw target from a world direction has to pass through
    // here first.
    [[nodiscard]] inline float NearestAngle(float a_target, float a_reference) {
        constexpr float kPi = 3.14159265f;
        constexpr float kTwoPi = 6.28318531f;
        float           d = a_target - a_reference;
        while (d > kPi) {
            d -= kTwoPi;
        }
        while (d < -kPi) {
            d += kTwoPi;
        }
        return a_reference + d;
    }

    [[nodiscard]] inline float ClampPitch(float a_pitch) {
        return (std::max)(-kPitchLimit, (std::min)(kPitchLimit, a_pitch));
    }

    [[nodiscard]] inline float ClampDistance(float a_distance) {
        return (std::max)(kMinDistance, (std::min)(kMaxDistance, a_distance));
    }

    // Read the opening state off whatever the engine already built, rather than
    // imposing one. That is what lets the layer arm invisibly: the shot the
    // player knows stays exactly as it was until they touch an input.
    [[nodiscard]] inline Orbit OrbitFromOffset(const Vec3& a_offset) {
        const float dist = Length(a_offset);
        if (dist <= 1e-6f) {
            return {};
        }
        Orbit o;
        o.distance = dist;
        o.pitch = std::asin((std::max)(-1.0f, (std::min)(1.0f, a_offset.z / dist)));
        o.yaw = std::atan2(a_offset.x, a_offset.y);
        return o;
    }

    [[nodiscard]] inline Vec3 OffsetFromOrbit(const Orbit& a_o) {
        const float horizontal = a_o.distance * std::cos(a_o.pitch);
        return { horizontal * std::sin(a_o.yaw), horizontal * std::cos(a_o.yaw),
                 a_o.distance * std::sin(a_o.pitch) };
    }

    // An orthonormal frame for a camera sitting at a_offset from its pivot and
    // looking back at it.
    //
    // ⚠ THE AXIS ORDER HERE IS ARBITRARY AND THAT IS DELIBERATE. Nothing in this
    // file needs to know which column the engine treats as forward, because the
    // only consumer is DeltaRotation, where one basis is multiplied by the
    // transpose of another built the same way and the convention cancels. Trying
    // to match the engine's own camera convention would mean guessing at it, and
    // a guess that is wrong by a swapped axis produces a camera that looks
    // plausible until it is asked to pitch.
    [[nodiscard]] inline Mat3 BasisFromOffset(const Vec3& a_offset) {
        const Vec3 forward = Normalized({ -a_offset.x, -a_offset.y, -a_offset.z });
        const Vec3 worldUp{ 0.0f, 0.0f, 1.0f };
        const Vec3 right = Normalized(Cross(worldUp, forward));
        const Vec3 up = Cross(forward, right);
        Mat3 basis;
        basis.m[0][0] = right.x;    basis.m[0][1] = forward.x;  basis.m[0][2] = up.x;
        basis.m[1][0] = right.y;    basis.m[1][1] = forward.y;  basis.m[1][2] = up.y;
        basis.m[2][0] = right.z;    basis.m[2][1] = forward.z;  basis.m[2][2] = up.z;
        return basis;
    }

    [[nodiscard]] inline Mat3 Multiply(const Mat3& a, const Mat3& b) {
        Mat3 out;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                              a.m[i][2] * b.m[2][j];
            }
        }
        return out;
    }

    [[nodiscard]] inline Mat3 Transposed(const Mat3& a_m) {
        Mat3 out;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out.m[i][j] = a_m.m[j][i];
            }
        }
        return out;
    }

    // The rotation that carries the opening view direction onto the current one.
    // Left-multiplied onto the engine's own camera rotation, it turns the camera
    // by exactly the amount the position moved, so the pivot stays centred.
    //
    // With no input applied the two bases are identical and this is the identity,
    // which is the property that makes an untouched arm invisible.
    [[nodiscard]] inline Mat3 DeltaRotation(const Vec3& a_from, const Vec3& a_to) {
        return Multiply(BasisFromOffset(a_to), Transposed(BasisFromOffset(a_from)));
    }

    // Frame-rate independent approach, the same shape and rate the preview spin
    // already uses. One easing rate across the mod rather than a second one to
    // tune against the first.
    [[nodiscard]] inline float Ease(float a_current, float a_target, float a_dt,
                                    float a_rate = 14.0f) {
        return a_current + (a_target - a_current) *
                               (std::min)(1.0f, (std::max)(0.0f, a_dt * a_rate));
    }

    // The pivot follows a live skeleton and needs the same treatment as the
    // angles: eased, never snapped. The head node sways with breathing and
    // head-tracking and swings in a circle when the preview spin turns the
    // character, and a camera bolted to the raw reading carries every one of
    // those wobbles into the shot.
    [[nodiscard]] inline Vec3 Ease(const Vec3& a_current, const Vec3& a_target,
                                   float a_dt, float a_rate = 14.0f) {
        return { Ease(a_current.x, a_target.x, a_dt, a_rate),
                 Ease(a_current.y, a_target.y, a_dt, a_rate),
                 Ease(a_current.z, a_target.z, a_dt, a_rate) };
    }

    // How far past the soft boundary the wheel may push before the rubber band
    // wins outright. Generous enough that the give is obvious rather than a
    // twitch, short enough that the character never becomes a speck.
    inline constexpr float kOvershoot = 1.35f;

    // How quickly an overshot distance is drawn back to the boundary. Slower
    // than the main easing on purpose: at the same rate the pull is
    // indistinguishable from the zoom simply refusing to go, which is the hard
    // stop this exists to replace.
    inline constexpr float kRecoilRate = 3.5f;

    // The far limit, and the give around it.
    //
    // A hard clamp tells the player nothing: the wheel turns and the picture
    // stops changing, which reads as broken rather than as a boundary. Letting
    // the shot travel a little past and then drawing it back says "this is as
    // far as the room goes" with no text and no sound.
    [[nodiscard]] inline float ClampToOvershoot(float a_distance, float a_softMax) {
        const float ceiling = (std::min)(kMaxDistance, a_softMax * kOvershoot);
        return (std::max)(kMinDistance, (std::min)(ceiling, a_distance));
    }

    // One step of the pull back toward the boundary. Returns a_distance
    // untouched when it is inside, so nothing is spent while the player is
    // within bounds and the band cannot fight ordinary zooming.
    // ⚠ THE RATE IS PASSED IN SO IT CAN STAY BELOW THE MAIN EASING. With a
    // fixed rate and a smoothing slider on the main one, a floaty setting would
    // make the pull FASTER than the motion it interrupts, and the band would
    // snatch the shot back instead of easing it.
    [[nodiscard]] inline float Recoil(float a_distance, float a_softMax, float a_dt,
                                      float a_rate = kRecoilRate) {
        if (a_distance <= a_softMax) {
            return a_distance;
        }
        return Ease(a_distance, a_softMax, a_dt, a_rate);
    }

    // What the band should run at, given whatever the main easing is set to.
    [[nodiscard]] inline float RecoilRateFor(float a_mainRate) {
        return (std::min)(kRecoilRate, a_mainRate * 0.4f);
    }

    // A 0-to-1 taste dial turned into an easing rate. 0 follows the hand almost
    // exactly, 1 drifts after it.
    //
    // Exposed as smoothing rather than as the rate itself because the rate runs
    // BACKWARDS from what it describes: a bigger number is a stiffer camera,
    // which is the opposite of what a slider called "smoothing" should do as it
    // moves right.
    inline constexpr float kRateStiff = 22.0f;
    inline constexpr float kRateLoose = 3.0f;

    [[nodiscard]] inline float RateFromSmoothing(float a_smoothing) {
        const float s = (std::max)(0.0f, (std::min)(1.0f, a_smoothing));
        return kRateStiff + (kRateLoose - kRateStiff) * s;
    }

    // The zoom track (spec: docs/specs/2026-08-05-zoom-track-camera.md). One
    // parameter t in [0, 1] is the camera's position along the track: 0 the
    // near stop, 1 the far stop. The wheel moves t by a fixed step, distance
    // is a log-lerp along the track, and the pivot height is keyframed over
    // the SAME t - so equal wheel steps make equal distance RATIOS, which is
    // how zoom reads to a hand, and height can never fall out of phase with
    // distance. Framing is AUTHORED as compositions rather than derived from
    // optics; one field night spent four rounds proving the deriving
    // approach trades one complaint for another whichever way it is patched.

    [[nodiscard]] inline float Clamp01(float a_v) {
        return (std::max)(0.0f, (std::min)(1.0f, a_v));
    }

    // distance(t) = dMin * (dMax / dMin)^t. t is clamped at the near stop
    // (hard) but NOT at the far one, so the overshoot band past t = 1 shows
    // up in distance and the recoil has something to pull back.
    [[nodiscard]] inline float TrackDistance(float a_t, float a_dMin, float a_dMax) {
        if (a_dMin <= 0.0f || a_dMax <= a_dMin) {
            return (std::max)(a_dMin, kMinDistance);  // no travel to speak of
        }
        return a_dMin * std::pow(a_dMax / a_dMin, (std::max)(0.0f, a_t));
    }

    // The inverse, for reading a found shot onto the track at capture:
    // t0 = log(D0 / dMin) / log(dMax / dMin), clamped to the stops. A shot
    // closer than the floor reads as the near stop, one past the boundary as
    // the far stop.
    [[nodiscard]] inline float TrackT(float a_distance, float a_dMin, float a_dMax) {
        if (a_dMin <= 0.0f || a_dMax <= a_dMin || a_distance <= 0.0f) {
            return 0.0f;
        }
        return Clamp01(std::log(a_distance / a_dMin) / std::log(a_dMax / a_dMin));
    }

    // The far-end overshoot band expressed in t: the same distance ceiling
    // ClampToOvershoot holds (the soft boundary times the overshoot, with
    // the absolute maximum still winning), carried through the track's own
    // log scale. The near stop stays hard at 0 and gets no band.
    [[nodiscard]] inline float TrackCeiling(float a_dMin, float a_dMax) {
        if (a_dMin <= 0.0f || a_dMax <= a_dMin) {
            return 1.0f;
        }
        const float top = (std::min)(kMaxDistance, a_dMax * kOvershoot);
        return 1.0f + std::log((std::max)(1.0f, top / a_dMax)) /
                          std::log(a_dMax / a_dMin);
    }

    [[nodiscard]] inline float ClampTrack(float a_t, float a_ceiling) {
        return (std::max)(0.0f, (std::min)(a_ceiling, a_t));
    }

    // The pan band: the same give-and-recoil the wheel's far stop has, for
    // the middle-drag pan. Inside the band nothing happens; past either edge
    // the target is drawn back to it, so the character can be framed
    // off-centre but never lost.
    //
    // ⚠ THE TWO EDGES ARE SEPARATE BECAUSE THE REACH IS NOT SYMMETRIC. The
    // pan composes onto the track's pivot, which sits near the eye line at
    // the close stop - from there the crown is a head away and the shoes are
    // nearly a whole body away. One shared bound generous enough to reach
    // the feet would let the shot fly a body's height into the sky, and one
    // tight enough to keep the sky sane could not reach the shoes. The field
    // met the second half of that: "sometimes I can't reach the shoes".
    [[nodiscard]] inline float ClampBand(float a_v, float a_low, float a_high) {
        return (std::max)(a_low, (std::min)(a_high, a_v));
    }

    [[nodiscard]] inline float RecoilBand(float a_v, float a_low, float a_high,
                                          float a_dt, float a_rate = kRecoilRate) {
        if (a_v > a_high) {
            return Ease(a_v, a_high, a_dt, a_rate);
        }
        if (a_v < a_low) {
            return Ease(a_v, a_low, a_dt, a_rate);
        }
        return a_v;
    }

    // The symmetric case, for the sideways axis where both directions really
    // are the same distance from the subject.
    [[nodiscard]] inline float ClampAbs(float a_v, float a_bound) {
        return ClampBand(a_v, -a_bound, a_bound);
    }

    [[nodiscard]] inline float RecoilAbs(float a_v, float a_bound, float a_dt,
                                         float a_rate = kRecoilRate) {
        return RecoilBand(a_v, -a_bound, a_bound, a_dt, a_rate);
    }

    // How tall the picture is at a place on the track, measured in the same
    // world units as the anchors: half the frame's height at that distance.
    //
    // ⚠ THE LENS SLOPE IS STATED, NOT READ. NiFrustum's fTop really is the
    // tan of half the vertical FOV and reading it live would also work, but
    // deriving framing from a live frustum is what failed four times in one
    // night, and the failure that cost the most was a misread of exactly
    // this number. The menus run FOV 60, measured, which is a slope of
    // 0.325, and it is a setting for anyone whose setup differs. Nothing
    // else here consults the optics.
    [[nodiscard]] inline float HalfSpanAt(float a_t, float a_dMin, float a_dMax,
                                          float a_lensSlope) {
        return (std::max)(0.0f, a_lensSlope) *
               TrackDistance(a_t, a_dMin, a_dMax);
    }

    // How far back the camera has to be for the frame to be exactly tall
    // enough to hold a subject of this height. The inverse of HalfSpanAt: the
    // half frame at a distance is lensSlope times it, and holding the whole
    // subject needs two of those.
    //
    // ⚠ THIS EXISTS BECAUSE THE FAR STOP WAS NEVER ASKED WHETHER IT COULD HOLD
    // THE SUBJECT. It is a setting, and a setting chosen while looking at a
    // person on foot is not a statement about a person on a horse. Measured
    // 2026-08-13: a mounted column is 180.3 units, which needs 277.4, against a
    // shipped boundary of 200 that holds 130. The arm opened pegged at the far
    // stop with nothing left to pull, every time, and no amount of wheel could
    // fit the pair in frame because the track had already ended.
    [[nodiscard]] inline float DistanceThatHolds(float a_height,
                                                 float a_lensSlope) {
        if (a_height <= 0.0f || a_lensSlope <= 0.0f) {
            return 0.0f;
        }
        return a_height / (2.0f * a_lensSlope);
    }

    // Where on the track a measured sphere wants to sit: the place whose half
    // frame is the sphere's radius divided by a_fill, so a_fill of 0.8 leaves
    // a fifth of the frame as margin round the thing being looked at.
    //
    // ⚠ THE LENS SLOPE IS THE CALLER'S SETTING, NOT A FRUSTUM READ, for the
    // reason HalfSpanAt states above. This is that same stated number used the
    // same way, which is why it is here beside it rather than in the engine
    // layer where a live NiFrustum would be in reach.
    //
    // ⚠ AND A DEGENERATE INPUT ANSWERS WIDE. A radius of zero is a measurement
    // that found nothing, and answering with the near stop would put the
    // camera inside the character. TrackT already clamps the real answers to
    // the stops, so nothing here can leave the boundary either way.
    [[nodiscard]] inline float AttachmentTrackT(float a_radius, float a_fill,
                                                float a_lensSlope, float a_dMin,
                                                float a_dMax) {
        if (a_radius <= 0.0f || a_fill <= 0.0f || a_lensSlope <= 0.0f) {
            return 1.0f;
        }
        return TrackT(a_radius / (a_fill * a_lensSlope), a_dMin, a_dMax);
    }

    // The composition at a place on the track: where the shot aims, and how
    // much of a focused part's sideways offset it carries.
    //
    // ⚠ THIS IS THE WHOLE MODEL, AND IT REPLACED A KEYFRAME SCHEDULE THAT
    // COULD NOT WORK. Three authored keyframes with the anchor gliding
    // between them moved the aim point on a clock, with nothing tying that
    // clock to whether the part being framed was still on screen. Every
    // field round hit the same wall from a new side: a face leaving the top
    // mid-travel, boots leaving the bottom, and finally both at once,
    // because the drift starts the moment the wheel does. Tuning the
    // schedule only moved which end broke.
    //
    // The rule instead: AIM AT WHAT WAS ASKED FOR, AND GIVE WAY TO THE BODY
    // ONLY AS FAST AS THE GROWING FRAME FORCES YOU TO. As the shot widens
    // the frame runs out of body to fill, so the aim point has less freedom
    // to sit away from the middle; that shrinking freedom IS the schedule,
    // and it lands on the body middle exactly when the frame first holds
    // the whole character. The three compositions the spec authored still
    // come out of it - the part up close, the part with room around it in
    // the middle, the whole character far out - but now they are reached
    // because the picture demanded it rather than because a clock said so.
    //
    // The slack lets the frame hang a little past the body at the near end,
    // which is what keeps a boot off the bottom edge instead of exactly on
    // it. It shrinks with the same freedom, so the far stop still holds the
    // whole character with no floor or sky to spare.
    struct Composition {
        float anchorZ{ 0.0f };
        float lateral{ 0.0f };  // 0 to 1, how much sideways offset to carry
    };

    [[nodiscard]] inline Composition ComposeShot(float a_focusZ, float a_feetZ,
                                                 float a_crownZ, float a_bodyMidZ,
                                                 float a_halfSpan, float a_slack) {
        const float height = a_crownZ - a_feetZ;
        if (height <= 1e-3f) {
            return { a_focusZ, 1.0f };  // no proportions: hold the focus
        }
        const float half = 0.5f * height;
        const float room = (std::max)(0.0f, half - (std::max)(0.0f, a_halfSpan)) *
                           (1.0f + (std::max)(0.0f, a_slack));
        const float low = a_bodyMidZ - room;
        const float high = a_bodyMidZ + room;
        return { (std::max)(low, (std::min)(high, a_focusZ)), Clamp01(room / half) };
    }

    // Where on screen the player may start a camera drag, as fractions of the
    // display. A stated region rather than a derived one.
    //
    // ⚠ ASKING THE MENU INSTEAD WAS TRIED AND REFUTED. GFxMovieView::HitTest
    // with kButtonEvents answered "over the UI" for 53% of samples on the item
    // list and 57% over the character, measured 2026-08-04 across 823 samples,
    // because SkyUI carries button-enabled objects over the character view and
    // an invisible button is still a button. The other three test conditions
    // answer "over" almost everywhere, since the menu background spans the
    // screen. See the design doc; do not re-run it.
    struct Zone {
        float left{ 0.0f };
        float top{ 0.0f };
        float right{ 1.0f };
        float bottom{ 1.0f };
    };

    // a_w and a_h are the cursor space's own extent, so this works whether the
    // caller counts in screen pixels or Scaleform stage units. Degenerate extents
    // answer false rather than dividing: a drag taken on a divide-by-zero is a
    // camera swing the player did not ask for.
    [[nodiscard]] inline bool InZone(float a_x, float a_y, float a_w, float a_h,
                                     const Zone& a_zone) {
        if (a_w <= 0.0f || a_h <= 0.0f) {
            return false;
        }
        const float u = a_x / a_w;
        const float v = a_y / a_h;
        return u >= a_zone.left && u <= a_zone.right && v >= a_zone.top &&
               v <= a_zone.bottom;
    }

}  // namespace MTB::StudioCameraPolicy

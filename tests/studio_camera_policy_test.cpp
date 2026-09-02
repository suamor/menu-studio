// Unit tests for the pivot camera's arithmetic. No engine, no Skyrim: the
// policy takes plain floats so the invariants that decide whether a camera is
// sane can be checked on the desk instead of in a menu.
#include "StudioCameraPolicy.h"

#include <cmath>
#include <cstdio>

using namespace MTB::StudioCameraPolicy;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    bool Near(float a, float b, float tol = 1e-3f) {
        const float d = a - b;
        return (d < 0.0f ? -d : d) <= tol;
    }

    bool NearVec(const Vec3& a, const Vec3& b, float tol = 1e-3f) {
        return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
    }

    bool IsIdentity(const Mat3& m, float tol = 1e-4f) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (!Near(m.m[i][j], i == j ? 1.0f : 0.0f, tol)) {
                    return false;
                }
            }
        }
        return true;
    }

    Vec3 Apply(const Mat3& m, const Vec3& v) {
        return { m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                 m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                 m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z };
    }

    // Whatever offset goes in must come back out. If this drifts, the camera
    // creeps every time the state is read back and re-applied.
    void RoundTrip() {
        const Vec3 cases[]{
            { 0.0f, 150.0f, 0.0f },      { 0.0f, -150.0f, 0.0f },
            { 120.0f, 0.0f, 40.0f },     { -80.0f, 60.0f, -30.0f },
            { 10.0f, 10.0f, 200.0f },    { -5.0f, -240.0f, 90.0f },
        };
        for (const auto& c : cases) {
            CHECK(NearVec(OffsetFromOrbit(OrbitFromOffset(c)), c));
        }
    }

    // The property the whole "arm invisibly" claim rests on: with nothing
    // touched yet, the correction applied to the engine's camera is the
    // identity, so the shot the player already had is byte-for-byte kept.
    void UntouchedArmIsIdentity() {
        const Vec3 offsets[]{
            { 0.0f, 155.0f, 20.0f }, { -46.7f, 145.0f, -20.0f }, { 90.0f, -90.0f, 5.0f },
        };
        for (const auto& o : offsets) {
            CHECK(IsIdentity(DeltaRotation(o, o)));
        }
    }

    // A delta rotation has to actually be a rotation: orthonormal, and it must
    // carry the old view direction onto the new one. If it does not, the camera
    // moves but keeps looking where it was, which reads as the subject sliding
    // out of frame.
    void DeltaCarriesTheView() {
        const Orbit from{ 150.0f, 0.0f, 0.2f };
        const Orbit to{ 150.0f, 1.1f, -0.4f };
        const Vec3  a = OffsetFromOrbit(from);
        const Vec3  b = OffsetFromOrbit(to);
        const Mat3  d = DeltaRotation(a, b);

        CHECK(NearVec(Apply(d, Normalized(a)), Normalized(b)));

        const Mat3 shouldBeIdentity = Multiply(d, Transposed(d));
        CHECK(IsIdentity(shouldBeIdentity));
    }

    // Distance must not change when only the angles do, or "orbit" quietly
    // becomes "orbit and dolly".
    void OrbitPreservesDistance() {
        const Orbit o{ 200.0f, 2.4f, -0.9f };
        CHECK(Near(Length(OffsetFromOrbit(o)), 200.0f, 1e-2f));
    }

    // The clamp exists so the basis stays defined, not for taste. Straight up
    // and straight down are exactly where the cross product collapses.
    void PitchClampKeepsTheBasisSane() {
        CHECK(ClampPitch(3.0f) <= kPitchLimit);
        CHECK(ClampPitch(-3.0f) >= -kPitchLimit);
        CHECK(Near(ClampPitch(0.5f), 0.5f));

        const Orbit top{ 150.0f, 0.0f, ClampPitch(99.0f) };
        const Mat3  basis = BasisFromOffset(OffsetFromOrbit(top));
        CHECK(IsIdentity(Multiply(basis, Transposed(basis))));
    }

    // A yaw target taken from a world direction must be rewritten near the
    // angle the shot is already at, or the easing unwinds the long way and
    // the camera swings most of a turn to reach a heading beside it.
    void AngleTakesTheShortWay() {
        constexpr float pi = 3.14159265f;
        constexpr float twoPi = 6.28318531f;

        // The case that would spin: 3.0 to -3.0 is a fifth of a turn apart.
        const float near = NearestAngle(-3.0f, 3.0f);
        CHECK(near > 3.0f && near < 3.0f + pi);
        CHECK(Near(std::fmod(near + twoPi * 4.0f, twoPi),
                   std::fmod(-3.0f + twoPi * 4.0f, twoPi), 1e-3f));

        // Already close: nothing moves.
        CHECK(Near(NearestAngle(0.5f, 0.4f), 0.5f));
        CHECK(Near(NearestAngle(-0.5f, -0.4f), -0.5f));

        // Whatever comes in, the result is the same heading and within half
        // a turn of the reference.
        for (float ref = -9.0f; ref <= 9.0f; ref += 0.7f) {
            for (float t = -9.0f; t <= 9.0f; t += 0.7f) {
                const float a = NearestAngle(t, ref);
                CHECK(a - ref <= pi + 1e-3f && a - ref >= -pi - 1e-3f);
                const float turns = (a - t) / twoPi;
                CHECK(Near(turns, std::round(turns), 1e-3f));  // same heading
            }
        }
    }

    void DistanceClamp() {
        CHECK(Near(ClampDistance(5.0f), kMinDistance));
        CHECK(Near(ClampDistance(99999.0f), kMaxDistance));
        CHECK(Near(ClampDistance(150.0f), 150.0f));
    }

    // Easing must converge and must never overshoot, including on the long
    // frame a menu open produces. An unclamped lerp factor past 1 oscillates.
    void EasingConvergesAndNeverOvershoots() {
        float v = 0.0f;
        for (int i = 0; i < 200; ++i) {
            v = Ease(v, 100.0f, 1.0f / 60.0f);
            CHECK(v <= 100.0f + 1e-3f);
        }
        CHECK(Near(v, 100.0f, 0.5f));

        CHECK(Near(Ease(0.0f, 100.0f, 10.0f), 100.0f));   // huge dt lands, no overshoot
        CHECK(Near(Ease(50.0f, 50.0f, 0.016f), 50.0f));   // already there, stays
        CHECK(Near(Ease(0.0f, 100.0f, -1.0f), 0.0f));     // a negative dt moves nothing
    }

    void ZoneAcceptsTheViewportAndRefusesTheList() {
        const Zone right{ 0.55f, 0.0f, 1.0f, 1.0f };  // the character side
        CHECK(InZone(2000.0f, 700.0f, 2560.0f, 1440.0f, right));
        CHECK(!InZone(400.0f, 700.0f, 2560.0f, 1440.0f, right));  // the item list
        CHECK(InZone(1408.0f, 0.0f, 2560.0f, 1440.0f, right));    // exactly on the edge

        // Works in stage units as well as screen pixels, since the caller
        // supplies the extent it is counting in.
        CHECK(InZone(1000.0f, 350.0f, 1280.0f, 720.0f, right));

        // A degenerate extent must refuse rather than divide. A drag taken on a
        // divide by zero is a camera swing nobody asked for.
        CHECK(!InZone(100.0f, 100.0f, 0.0f, 1440.0f, right));
        CHECK(!InZone(100.0f, 100.0f, 2560.0f, 0.0f, right));
    }

    // The far boundary has give in it. A hard stop reads as broken, so the
    // wheel may travel past and is then drawn back.
    void FarBoundaryHasGiveAndRecovers() {
        constexpr float soft = 500.0f;

        // Past the boundary is allowed, up to the overshoot ceiling.
        CHECK(Near(ClampToOvershoot(520.0f, soft), 520.0f));
        CHECK(Near(ClampToOvershoot(99999.0f, soft), soft * kOvershoot));
        CHECK(Near(ClampToOvershoot(200.0f, soft), 200.0f));  // inside, untouched

        // ⚠ The absolute ceiling still wins, or a huge soft limit would let the
        // overshoot multiply it past the point the arithmetic is sane.
        CHECK(ClampToOvershoot(99999.0f, 5000.0f) <= kMaxDistance);

        // Inside the boundary the band must do NOTHING, or it would fight
        // ordinary zooming every frame.
        CHECK(Near(Recoil(200.0f, soft, 1.0f / 60.0f), 200.0f));
        CHECK(Near(Recoil(soft, soft, 1.0f / 60.0f), soft));

        // Outside, it pulls back and settles ON the boundary, never through it.
        float d = soft * kOvershoot;
        for (int i = 0; i < 400; ++i) {
            d = Recoil(d, soft, 1.0f / 60.0f);
            CHECK(d >= soft - 1e-3f);  // never undershoots
        }
        CHECK(Near(d, soft, 0.5f));

        // And it is SLOWER than the main easing, which is the whole point: at
        // the same rate the pull is indistinguishable from a hard stop.
        float band = soft * kOvershoot;
        float plain = soft * kOvershoot;
        band = Recoil(band, soft, 1.0f / 60.0f);
        plain = Ease(plain, soft, 1.0f / 60.0f);
        CHECK(band > plain);
    }

    // The smoothing dial must run the right way round and stay bounded, and the
    // rubber band must never outrun the motion it interrupts.
    void SmoothingDialAndBandOrdering() {
        CHECK(Near(RateFromSmoothing(0.0f), kRateStiff));
        CHECK(Near(RateFromSmoothing(1.0f), kRateLoose));
        CHECK(RateFromSmoothing(0.0f) > RateFromSmoothing(1.0f));  // right way round
        CHECK(Near(RateFromSmoothing(-5.0f), kRateStiff));         // clamped
        CHECK(Near(RateFromSmoothing(99.0f), kRateLoose));

        // Higher smoothing must actually move the camera less in one frame.
        const float stiff = Ease(0.0f, 100.0f, 1.0f / 60.0f, RateFromSmoothing(0.0f));
        const float loose = Ease(0.0f, 100.0f, 1.0f / 60.0f, RateFromSmoothing(1.0f));
        CHECK(stiff > loose);

        // ⚠ The band has to stay SLOWER than the easing at every setting, or a
        // loose camera would get snatched back rather than eased back.
        for (float s = 0.0f; s <= 1.0f; s += 0.1f) {
            const float main = RateFromSmoothing(s);
            CHECK(RecoilRateFor(main) < main);
            CHECK(RecoilRateFor(main) > 0.0f);
        }

        // And it still settles on the boundary at the loosest setting.
        const float rate = RecoilRateFor(RateFromSmoothing(1.0f));
        float       d = 500.0f * kOvershoot;
        for (int i = 0; i < 2000; ++i) {
            d = Recoil(d, 500.0f, 1.0f / 60.0f, rate);
            CHECK(d >= 500.0f - 1e-3f);
        }
        CHECK(Near(d, 500.0f, 0.5f));
    }

    // The pivot ease is the scalar ease three times over; convergence and the
    // no-overshoot bound must hold per axis, including on the long frame a
    // menu open produces.
    void PivotEaseConverges() {
        Vec3       p{ 0.0f, -50.0f, 120.0f };
        const Vec3 target{ 30.0f, 10.0f, 128.0f };
        for (int i = 0; i < 400; ++i) {
            p = Ease(p, target, 1.0f / 60.0f);
        }
        CHECK(NearVec(p, target, 0.5f));

        const Vec3 snap = Ease(Vec3{ 0.0f, 0.0f, 0.0f }, target, 10.0f);
        CHECK(NearVec(snap, target));

        // A negative dt moves nothing on any axis.
        const Vec3 held = Ease(Vec3{ 1.0f, 2.0f, 3.0f }, target, -1.0f);
        CHECK(NearVec(held, Vec3{ 1.0f, 2.0f, 3.0f }));
    }

    // A zero offset has no direction to read, so it must not produce NaNs that
    // then propagate into the camera matrix.
    void DegenerateOffsetIsSafe() {
        const Orbit o = OrbitFromOffset({ 0.0f, 0.0f, 0.0f });
        CHECK(Near(o.distance, 0.0f));
        CHECK(o.yaw == o.yaw);      // not NaN
        CHECK(o.pitch == o.pitch);  // not NaN
        const Mat3 basis = BasisFromOffset({ 0.0f, 0.0f, 0.0f });
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                CHECK(basis.m[i][j] == basis.m[i][j]);
            }
        }
    }

    // ===== The zoom track =====
    // Figures throughout are the field's: floor 35 (the live INI), dome
    // boundary 251, the character with feet 0, body middle 64, face 118.

    // Spec test 1: distance(t) and t(distance) are each other's inverses
    // across the whole range, so nothing drifts when the state is read back
    // and re-applied.
    void TrackRoundTrips() {
        const float dMin = 35.0f;
        const float dMax = 420.0f;
        for (float d = dMin; d <= dMax; d *= 1.17f) {
            CHECK(Near(TrackDistance(TrackT(d, dMin, dMax), dMin, dMax), d,
                       d * 1e-4f));
        }
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.05f) {
            CHECK(Near(TrackT(TrackDistance(t, dMin, dMax), dMin, dMax), t, 1e-4f));
        }
        // The stops are the bounds themselves.
        CHECK(Near(TrackDistance(0.0f, dMin, dMax), dMin));
        CHECK(Near(TrackDistance(1.0f, dMin, dMax), dMax, 1e-2f));
        // Outside the range the inversion clamps to the stops rather than
        // extrapolating.
        CHECK(Near(TrackT(10.0f, dMin, dMax), 0.0f));
        CHECK(Near(TrackT(9999.0f, dMin, dMax), 1.0f));
        // Degenerate ranges answer sanely instead of dividing by log(1).
        CHECK(TrackT(100.0f, 100.0f, 100.0f) == 0.0f);
        CHECK(Near(TrackDistance(0.5f, 0.0f, 400.0f), kMinDistance));
        CHECK(Near(TrackDistance(0.5f, 200.0f, 100.0f), 200.0f));
    }

    // Spec test 2: equal t steps give equal distance RATIOS everywhere on
    // the track - the uniform wheel feel is a property of the construction,
    // and the default step crosses the whole range in the stated notch
    // count.
    void EqualStepsMakeEqualRatios() {
        const float dMin = 35.0f;
        const float dMax = 420.0f;
        const float step = 1.0f / 15.0f;
        const float r0 =
            TrackDistance(step, dMin, dMax) / TrackDistance(0.0f, dMin, dMax);
        CHECK(r0 > 1.0f);
        for (float t = 0.0f; t + step <= 1.0f + 1e-4f; t += step) {
            const float ratio = TrackDistance(t + step, dMin, dMax) /
                                TrackDistance(t, dMin, dMax);
            CHECK(Near(ratio, r0, r0 * 1e-4f));
        }
        // Fifteen default notches span the range exactly.
        CHECK(Near(std::pow(r0, 15.0f), dMax / dMin, dMax / dMin * 1e-3f));
    }

    // The composition rule, on the field character that drove every round:
    // 121 units tall, feet at 0, body middle 60.5, eye line 112.5, the
    // ankle 5.5, floor 35 and boundary 200 with the menus' lens slope.
    // Distances and half-spans below are the ones the track really produces
    // at those places, so a failure here is a shot the field would see.
    void CompositionAimsAtTheFocusAndYieldsToTheFrame() {
        constexpr float feet = 0.0f;
        constexpr float crown = 121.0f;
        constexpr float mid = 60.5f;
        constexpr float eye = 112.5f;
        constexpr float ankle = 5.5f;
        constexpr float slack = 0.5f;
        constexpr float dMin = 35.0f;
        constexpr float dMax = 200.0f;
        constexpr float lens = 0.325f;

        const auto shotAt = [&](float t, float focus) {
            return ComposeShot(focus, feet, crown, mid,
                               HalfSpanAt(t, dMin, dMax, lens), slack);
        };
        // Is the focused part inside the picture, with the stated margin?
        const auto framed = [&](float t, float focus, float margin) {
            const float s = HalfSpanAt(t, dMin, dMax, lens);
            const auto  c = shotAt(t, focus);
            return focus >= c.anchorZ - s + margin && focus <= c.anchorZ + s - margin;
        };

        // ⚠ THE ONE THAT FAILED IN THE FIELD. At the closeness Fitting Room
        // asks for, a boots shot sat at t 0.7 and the keyframe schedule had
        // already dragged the aim up to the knee, leaving the sole on the
        // bottom edge. Whatever was focused must stay inside the picture at
        // every place on the track, both ends of the body alike.
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.02f) {
            CHECK(framed(t, ankle, 0.0f));
            CHECK(framed(t, eye, 0.0f));
            CHECK(framed(t, mid, 0.0f));
        }

        // And while the shot is still ABOUT the part - the frame shorter
        // than the body - it must be comfortably inside, not clinging to an
        // edge. Past that the frame is closing on the whole character and
        // the feet belong near the bottom, which is what a full-length shot
        // is; asking for margin there would be asking for the wrong picture.
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.02f) {
            const float s = HalfSpanAt(t, dMin, dMax, lens);
            if (s <= 0.4f * (crown - feet)) {
                CHECK(framed(t, ankle, 0.15f * s));
                CHECK(framed(t, eye, 0.15f * s));
            }
        }

        // The field's own case, pinned: the boots at the closeness Fitting
        // Room sends. The sole must sit clear of the bottom edge, which is
        // exactly what the last build got wrong.
        {
            const float s = HalfSpanAt(0.7f, dMin, dMax, lens);
            const float bottom = shotAt(0.7f, ankle).anchorZ - s;
            CHECK(bottom < feet);                 // floor under the boot
            CHECK(feet - bottom > 0.08f * crown); // and enough of it to see
        }

        // Close up the shot aims at exactly what was asked for, whichever
        // end of the body it is.
        CHECK(Near(shotAt(0.0f, ankle).anchorZ, ankle));
        CHECK(Near(shotAt(0.0f, eye).anchorZ, eye));
        CHECK(Near(shotAt(0.2f, ankle).anchorZ, ankle));

        // At the far stop the frame holds the whole character, so there is
        // no freedom left and every focus lands on the body middle.
        CHECK(Near(shotAt(1.0f, ankle).anchorZ, mid));
        CHECK(Near(shotAt(1.0f, eye).anchorZ, mid));
        CHECK(Near(shotAt(1.0f, ankle).lateral, 0.0f));

        // In between the aim gives way, monotonically and only toward the
        // middle - a shot that wandered the other way would read as a dip.
        float prev = ankle - 1.0f;
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.02f) {
            const float z = shotAt(t, ankle).anchorZ;
            CHECK(z >= prev - 1e-3f);
            CHECK(z <= mid + 1e-3f);
            prev = z;
        }
        prev = eye + 1.0f;
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.02f) {
            const float z = shotAt(t, eye).anchorZ;
            CHECK(z <= prev + 1e-3f);
            CHECK(z >= mid - 1e-3f);
            prev = z;
        }

        // A focus already at the middle never moves at all, at any zoom.
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.05f) {
            CHECK(Near(shotAt(t, mid).anchorZ, mid));
        }

        // The sideways carry rides the same freedom: all of it up close,
        // none once the frame holds the character.
        CHECK(Near(shotAt(0.0f, ankle).lateral, 1.0f));
        CHECK(shotAt(0.5f, ankle).lateral > 0.0f);
        CHECK(shotAt(0.5f, ankle).lateral < 1.0f);
        float prevW = 2.0f;
        for (float t = 0.0f; t <= 1.0f + 1e-4f; t += 0.02f) {
            const float w = shotAt(t, ankle).lateral;
            CHECK(w <= prevW + 1e-3f && w >= -1e-3f && w <= 1.0f + 1e-3f);
            prevW = w;
        }

        // Degenerate proportions hold the focus rather than inventing a
        // composition, and a frame taller than the body pins the middle.
        CHECK(Near(ComposeShot(50.0f, 10.0f, 10.0f, 10.0f, 20.0f, slack).anchorZ,
                   50.0f));
        CHECK(Near(ComposeShot(ankle, feet, crown, mid, 999.0f, slack).anchorZ, mid));
        CHECK(Near(ComposeShot(ankle, feet, crown, mid, 999.0f, slack).lateral, 0.0f));
    }

    // Spec test 5: the capture inversion. t0 read off a found mid-range shot
    // reproduces that shot's distance, which is what lets frame one keep the
    // framing the menu opened with. The distances are ones the field
    // actually logged.
    void CaptureInversionReproducesTheShot() {
        const float dMin = 35.0f;
        const float dMax = 251.0f;
        for (const float d0 : { 44.9f, 90.0f, 163.9f, 240.0f }) {
            const float t0 = TrackT(d0, dMin, dMax);
            CHECK(t0 > 0.0f && t0 < 1.0f);
            CHECK(Near(TrackDistance(t0, dMin, dMax), d0, 0.01f));
        }
    }

    // The far-end band carried into track terms: the ceiling in t maps back
    // to exactly the distance ceiling the wheel always had, the near stop
    // stays hard, and the recoil settles the target ON the far stop.
    void OvershootBandInT() {
        const float dMin = 35.0f;
        const float dMax = 251.0f;
        const float ceiling = TrackCeiling(dMin, dMax);
        CHECK(ceiling > 1.0f);
        CHECK(Near(TrackDistance(ceiling, dMin, dMax),
                   ClampToOvershoot(99999.0f, dMax), 0.5f));

        // A boundary already at the absolute maximum leaves no band, the
        // same answer ClampToOvershoot gives.
        CHECK(Near(TrackCeiling(35.0f, kMaxDistance), 1.0f));

        // Hard at the near stop, the band's give at the far one.
        CHECK(Near(ClampTrack(-0.5f, ceiling), 0.0f));
        CHECK(Near(ClampTrack(0.4f, ceiling), 0.4f));
        CHECK(Near(ClampTrack(9.9f, ceiling), ceiling));

        // Recoil in t: untouched inside the range, settles on the far stop
        // from the band, never through it.
        CHECK(Near(Recoil(0.7f, 1.0f, 1.0f / 60.0f), 0.7f));
        float t = ceiling;
        for (int i = 0; i < 600; ++i) {
            t = Recoil(t, 1.0f, 1.0f / 60.0f);
            CHECK(t >= 1.0f - 1e-3f);
        }
        CHECK(Near(t, 1.0f, 0.01f));
    }

    // The pan band: symmetric about zero, untouched inside, settles ON the
    // boundary from either side and never through it.
    void PanBandIsSymmetric() {
        constexpr float bound = 60.0f;

        CHECK(Near(ClampAbs(20.0f, bound), 20.0f));
        CHECK(Near(ClampAbs(-20.0f, bound), -20.0f));
        CHECK(Near(ClampAbs(999.0f, bound), bound));
        CHECK(Near(ClampAbs(-999.0f, bound), -bound));

        CHECK(Near(RecoilAbs(35.0f, bound, 1.0f / 60.0f), 35.0f));
        CHECK(Near(RecoilAbs(-35.0f, bound, 1.0f / 60.0f), -35.0f));
        CHECK(Near(RecoilAbs(0.0f, bound, 1.0f / 60.0f), 0.0f));

        float high = bound * kOvershoot;
        for (int i = 0; i < 600; ++i) {
            high = RecoilAbs(high, bound, 1.0f / 60.0f);
            CHECK(high >= bound - 1e-3f);
        }
        CHECK(Near(high, bound, 0.5f));

        float low = -bound * kOvershoot;
        for (int i = 0; i < 600; ++i) {
            low = RecoilAbs(low, bound, 1.0f / 60.0f);
            CHECK(low <= -bound + 1e-3f);
        }
        CHECK(Near(low, -bound, 0.5f));
    }

    // The vertical band's two edges are separate, and the reason is
    // reachability: from the close stop's eye-line pivot the shoes must be
    // reachable while the sky stays bounded. Figures are the field's - a 129
    // unit character, eye line 0.93 of that, the shipped ranges.
    void PanBandReachesTheShoesWithoutTheSky() {
        constexpr float height = 129.0f;
        const float     up = 0.5f * height;
        const float     down = 1.0f * height;
        const float     eyeLine = 0.93f * height;

        // The whole point: panning down from the close stop's pivot reaches
        // the feet, and a symmetric band at the sideways range would not.
        CHECK(eyeLine - down <= 0.0f);
        CHECK(eyeLine - up > 0.0f);

        // And the sky stays bounded - the widest anchor is the body middle,
        // and even from there the up edge stops around the crown.
        CHECK(0.5f * height + up <= 1.05f * height);

        CHECK(Near(ClampBand(-999.0f, -down, up), -down));
        CHECK(Near(ClampBand(999.0f, -down, up), up));
        CHECK(Near(ClampBand(-70.0f, -down, up), -70.0f));  // inside, untouched

        // Both edges recoil onto themselves, never through.
        float v = -down * kOvershoot;
        for (int i = 0; i < 600; ++i) {
            v = RecoilBand(v, -down, up, 1.0f / 60.0f);
            CHECK(v <= -down + 1e-3f);
        }
        CHECK(Near(v, -down, 0.5f));

        v = up * kOvershoot;
        for (int i = 0; i < 600; ++i) {
            v = RecoilBand(v, -down, up, 1.0f / 60.0f);
            CHECK(v >= up - 1e-3f);
        }
        CHECK(Near(v, up, 0.5f));

        // Inside the band nothing is spent, or the pull would fight an
        // ordinary pan every frame.
        CHECK(Near(RecoilBand(0.0f, -down, up, 1.0f / 60.0f), 0.0f));
        CHECK(Near(RecoilBand(-100.0f, -down, up, 1.0f / 60.0f), -100.0f));
    }

    // The union that turns a subtree of authored bounds into one sphere.
    void AttachmentBoundsUnion() {
        // An empty union takes whatever it is given.
        const Sphere none{};
        const Sphere one{ { 1.0f, 0.0f, 0.0f }, 2.0f };
        CHECK(Near(UnionSpheres(none, one).radius, 2.0f));
        CHECK(NearVec(UnionSpheres(none, one).centre, one.centre));
        CHECK(Near(UnionSpheres(one, none).radius, 2.0f));

        // A sphere wholly inside another leaves it alone, both ways round.
        const Sphere big{ { 0.0f, 0.0f, 0.0f }, 10.0f };
        const Sphere small{ { 1.0f, 0.0f, 0.0f }, 2.0f };
        CHECK(Near(UnionSpheres(big, small).radius, 10.0f));
        CHECK(NearVec(UnionSpheres(big, small).centre, big.centre));
        CHECK(Near(UnionSpheres(small, big).radius, 10.0f));

        // Two disjoint spheres give one that spans both, centred between the
        // far faces. Radius 1 at x=0 and radius 1 at x=10 span 12, so r=6 and
        // the centre lands at x=5.
        const Sphere a{ { 0.0f, 0.0f, 0.0f }, 1.0f };
        const Sphere b{ { 10.0f, 0.0f, 0.0f }, 1.0f };
        const Sphere u = UnionSpheres(a, b);
        CHECK(Near(u.radius, 6.0f));
        CHECK(NearVec(u.centre, Vec3{ 5.0f, 0.0f, 0.0f }));

        // Concentric spheres cannot divide by the distance between them.
        const Sphere c1{ { 3.0f, 4.0f, 5.0f }, 1.0f };
        const Sphere c2{ { 3.0f, 4.0f, 5.0f }, 4.0f };
        CHECK(Near(UnionSpheres(c1, c2).radius, 4.0f));
        CHECK(NearVec(UnionSpheres(c1, c2).centre, c1.centre));

        // A blade: many small spheres along a line union to span the line.
        Sphere blade{};
        for (int i = 0; i <= 10; ++i) {
            blade = UnionSpheres(
                blade, Sphere{ { 0.0f, 0.0f, static_cast<float>(i) * 4.0f }, 1.0f });
        }
        CHECK(Near(blade.radius, 21.0f));
        CHECK(NearVec(blade.centre, Vec3{ 0.0f, 0.0f, 20.0f }));
    }

    // Where on the track a measured radius sends the shot.
    void AttachmentDistance() {
        const float dMin  = 50.0f;
        const float dMax  = 400.0f;
        const float slope = 0.325f;
        const float fill  = 0.8f;

        // The distance chosen is the one where the sphere spans `fill` of the
        // half-frame, so reading it back through HalfSpanAt returns the radius
        // divided by fill.
        const float radius = 30.0f;
        const float t      = AttachmentTrackT(radius, fill, slope, dMin, dMax);
        CHECK(t > 0.0f && t < 1.0f);
        CHECK(Near(HalfSpanAt(t, dMin, dMax, slope), radius / fill, 0.05f));

        // A bigger weapon stands further back.
        CHECK(AttachmentTrackT(60.0f, fill, slope, dMin, dMax) >
              AttachmentTrackT(15.0f, fill, slope, dMin, dMax));

        // Something enormous pins at the far stop rather than running past it.
        CHECK(Near(AttachmentTrackT(100000.0f, fill, slope, dMin, dMax), 1.0f));
        // Something tiny pins at the near stop.
        CHECK(Near(AttachmentTrackT(0.001f, fill, slope, dMin, dMax), 0.0f));

        // Inputs that cannot describe a shot answer with the WIDE end, never
        // the near one: too far is a shot the player can correct, too close is
        // a face full of nothing.
        CHECK(Near(AttachmentTrackT(0.0f, fill, slope, dMin, dMax), 1.0f));
        CHECK(Near(AttachmentTrackT(radius, 0.0f, slope, dMin, dMax), 1.0f));
        CHECK(Near(AttachmentTrackT(radius, fill, 0.0f, dMin, dMax), 1.0f));
    }

    // How far back a subject of a given height has to be framed from, and the
    // boundary that has to allow it.
    void FarStopHasToHoldTheSubject() {
        const float slope = 0.325f;

        // The definition: at that distance the frame is exactly the subject's
        // height, so reading it back through HalfSpanAt at the far stop gives
        // half of it. dMin is irrelevant here because t 1 IS dMax.
        const float onFoot = DistanceThatHolds(118.2f, slope);
        CHECK(Near(HalfSpanAt(1.0f, 50.0f, onFoot, slope), 118.2f * 0.5f, 0.05f));

        // ── The measured pair, 2026-08-13 ───────────────────────────────────
        // On foot the character read 118.2 and needs 181.8, which every shipped
        // boundary already clears - the dev rig's 200, the 250 default, the
        // 420 no-space fallback. That is why nothing on foot may move.
        CHECK(Near(onFoot, 181.8f, 0.1f));
        CHECK(onFoot < 200.0f);

        // Mounted the same character read 180.3 and needs 277.4, which none of
        // them clear. The far stop was a wall, not a boundary: the arm opened
        // at t 1.00 with the track spent and the wheel could not fit the pair.
        const float mounted = DistanceThatHolds(180.3f, slope);
        CHECK(Near(mounted, 277.4f, 0.1f));
        CHECK(mounted > 200.0f);
        CHECK(mounted > 250.0f);

        // ⚠ AND THE OLD INVARIANT ONLY EVER HELD FOR PEOPLE ON FOOT. The
        // composition test above asserts the whole character fits at the far
        // stop, and it runs a 121-unit subject against dMax 200. Same assertion,
        // same numbers, mounted: it fails. A boundary of 200 holds 130.
        CHECK(2.0f * HalfSpanAt(1.0f, 50.0f, 200.0f, slope) < 180.3f);
        CHECK(Near(2.0f * HalfSpanAt(1.0f, 50.0f, 200.0f, slope), 130.0f, 0.1f));
        CHECK(2.0f * HalfSpanAt(1.0f, 50.0f, mounted, slope) >= 180.3f - 0.05f);

        // A taller subject always needs more room, and the relation is linear
        // so a doubled subject stands twice as far back.
        CHECK(DistanceThatHolds(240.0f, slope) >
              DistanceThatHolds(180.0f, slope));
        CHECK(Near(DistanceThatHolds(240.0f, slope),
                   2.0f * DistanceThatHolds(120.0f, slope)));

        // A measurement that found nothing asks for nothing. Zero is the
        // caller's cue to leave its own boundary alone, which matters because
        // this is used as a FLOOR: answering wide here would widen the stop
        // for a subject nobody managed to measure.
        CHECK(Near(DistanceThatHolds(0.0f, slope), 0.0f));
        CHECK(Near(DistanceThatHolds(-10.0f, slope), 0.0f));
        CHECK(Near(DistanceThatHolds(180.3f, 0.0f), 0.0f));
        CHECK(Near(DistanceThatHolds(180.3f, -0.5f), 0.0f));
    }

}  // namespace

int main() {
    RoundTrip();
    UntouchedArmIsIdentity();
    DeltaCarriesTheView();
    OrbitPreservesDistance();
    PitchClampKeepsTheBasisSane();
    AngleTakesTheShortWay();
    DistanceClamp();
    EasingConvergesAndNeverOvershoots();
    ZoneAcceptsTheViewportAndRefusesTheList();
    FarBoundaryHasGiveAndRecovers();
    SmoothingDialAndBandOrdering();
    PivotEaseConverges();
    DegenerateOffsetIsSafe();
    TrackRoundTrips();
    EqualStepsMakeEqualRatios();
    CompositionAimsAtTheFocusAndYieldsToTheFrame();
    CaptureInversionReproducesTheShot();
    OvershootBandInT();
    PanBandIsSymmetric();
    PanBandReachesTheShoesWithoutTheSky();
    AttachmentBoundsUnion();
    AttachmentDistance();
    FarStopHasToHoldTheSubject();

    if (g_failures == 0) {
        std::printf("studio_camera_policy_test: all checks passed\n");
        return 0;
    }
    std::printf("studio_camera_policy_test: %d FAILURES\n", g_failures);
    return 1;
}

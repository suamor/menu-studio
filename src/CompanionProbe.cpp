#include "PCH.h"

#include "CompanionProbe.h"

#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace MTB::CompanionProbe {

    namespace {

        constexpr float kPi = 3.14159265f;

        // How close to the camera-to-her axis the player has to sit before the
        // word "occluding" is earned. Generous on purpose: a body is wide, and
        // the question this answers is "is the fix a bearing change or not",
        // which does not need a tight cone.
        //
        // ⚠ ANGULAR SEPARATION, NOT DISTANCE. Fitting Room spent a night on the
        // opposite mistake - a similar camera distance was read as "she is in
        // the shot" when she was 110 degrees off-axis. Distance says nothing
        // about what is in front of what.
        constexpr float kOnAxisDegrees = 25.0f;

        struct State {
            std::uint32_t owner = 0;  // form id these counters belong to

            // The bone half.
            bool          boneLookedUp = false;
            bool          boneMissing = false;  // looked up and NOT found
            std::string   boneName;
            RE::NiMatrix3 lastLocal{};
            RE::NiMatrix3 lastWorld{};
            bool          haveLast = false;
            std::uint32_t ticks = 0;
            std::uint32_t localChanged = 0;
            std::uint32_t worldChanged = 0;
            float         localMaxDelta = 0.0f;
            float         worldMaxDelta = 0.0f;

            // The geometry half. Kept as the LAST reading rather than logged per
            // tick: nothing here moves while the world is paused, and a per-tick
            // line would bury the report it exists to support.
            bool  haveGeometry = false;
            float playerToHer = 0.0f;
            float camToHer = 0.0f;
            float camToPlayer = 0.0f;
            float offAxisDeg = 0.0f;  // player's bearing from her, minus the camera's
        };
        State g_s;

        void Clear() { g_s = State{}; }

        // Largest absolute entry-by-entry difference between two rotations.
        //
        // A MAGNITUDE, not a bool. "Changed" alone cannot separate a breathing
        // idle from float noise, and this codebase has already lost a round to a
        // diagnostic that could only ever confirm its own hypothesis.
        float MaxDelta(const RE::NiMatrix3& a_lhs, const RE::NiMatrix3& a_rhs) {
            float worst = 0.0f;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    worst = (std::max)(worst,
                                       std::fabs(a_lhs.entry[r][c] - a_rhs.entry[r][c]));
                }
            }
            return worst;
        }

        // Skyrim yaw: zero points along +Y, matching StudioRig::CameraBearing.
        // The one-unit floor is that function's too - these are units where a
        // person is ~120 tall, so a sub-unit horizontal offset is atan2 on noise.
        float Bearing(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) {
            const RE::NiPoint3 d = a_to - a_from;
            if (std::fabs(d.x) < 1.0f && std::fabs(d.y) < 1.0f) {
                return 0.0f;
            }
            return std::atan2(d.x, d.y);
        }

        float WrapToPi(float a_rad) {
            while (a_rad > kPi) {
                a_rad -= 2.0f * kPi;
            }
            while (a_rad < -kPi) {
                a_rad += 2.0f * kPi;
            }
            return a_rad;
        }

        void SampleGeometry(RE::Actor* a_actor) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* camRoot = cam ? cam->cameraRoot.get() : nullptr;
            if (!player || !camRoot) {
                return;
            }
            const RE::NiPoint3 her = a_actor->GetPosition();
            const RE::NiPoint3 him = player->GetPosition();
            const RE::NiPoint3 eye = camRoot->world.translate;
            g_s.playerToHer = her.GetDistance(him);
            g_s.camToHer = eye.GetDistance(her);
            g_s.camToPlayer = eye.GetDistance(him);
            g_s.offAxisDeg =
                WrapToPi(Bearing(her, him) - Bearing(her, eye)) * (180.0f / kPi);
            g_s.haveGeometry = true;
        }

        void SampleBone(RE::Actor* a_actor, RE::NiAVObject* a_root) {
            if (!g_s.boneLookedUp) {
                g_s.boneLookedUp = true;
                g_s.boneName = Settings::GetSingleton().companionProbeBone;
            }
            auto* bone = a_root->GetObjectByName(g_s.boneName);
            if (!bone) {
                // ⚠ A MISSING BONE MUST NOT READ AS A STILL ONE. Both produce
                // zero counted changes, they mean opposite things, and telling
                // them apart after the fact is impossible - which is exactly how
                // a field run gets spent for nothing.
                g_s.boneMissing = true;
                return;
            }
            g_s.boneMissing = false;
            ++g_s.ticks;
            if (g_s.haveLast) {
                const float dLocal = MaxDelta(bone->local.rotate, g_s.lastLocal);
                const float dWorld = MaxDelta(bone->world.rotate, g_s.lastWorld);
                if (dLocal > 0.0f) {
                    ++g_s.localChanged;
                    g_s.localMaxDelta = (std::max)(g_s.localMaxDelta, dLocal);
                }
                if (dWorld > 0.0f) {
                    ++g_s.worldChanged;
                    g_s.worldMaxDelta = (std::max)(g_s.worldMaxDelta, dWorld);
                }
            }
            g_s.lastLocal = bone->local.rotate;
            g_s.lastWorld = bone->world.rotate;
            g_s.haveLast = true;
        }

        const char* BoneVerdict() {
            if (g_s.localChanged == 0 && g_s.worldChanged == 0) {
                return "NEITHER MOVED: the graph steps but writes no bone locals, so "
                       "the fault is UPSTREAM of the skeleton. Propagation is not the "
                       "explanation and the downward pass this build added is not the "
                       "fix.";
            }
            if (g_s.localChanged > 0 && g_s.worldChanged == 0) {
                return "LOCALS MOVE, WORLD DOES NOT: the pose reaches her bones and "
                       "stops there. Propagation IS the missing half, and the downward "
                       "pass this build added is not reaching this bone.";
            }
            if (g_s.localChanged == 0 && g_s.worldChanged > 0) {
                return "WORLD MOVES WITHOUT LOCALS: something other than her graph is "
                       "writing this bone (an ancestor's transform, or another mod). "
                       "Unexpected: do not fold this into either hypothesis.";
            }
            return "BOTH MOVED: the stepped pose reaches her world transforms. If she "
                   "still reads as a statue the fault is DOWNSTREAM of the scene graph "
                   "(skinning, or the render never being asked to redraw her) and both "
                   "standing hypotheses are refuted.";
        }

        const char* GeometryVerdict() {
            if (std::fabs(g_s.offAxisDeg) >= kOnAxisDegrees) {
                return "The player is well OFF the camera-to-her axis, so whatever is "
                       "spoiling the shot is not him standing in front of her; a "
                       "bearing change would fix nothing.";
            }
            return g_s.camToPlayer < g_s.camToHer
                       ? "The player is ON the axis and NEARER the camera than she is: "
                         "he is literally between the lens and the subject. A bearing "
                         "change, or standing him down, is the fix."
                       : "The player is ON the axis but BEHIND her, so he is not "
                         "occluding her; he is cluttering the frame behind the "
                         "subject.";
        }

    }  // namespace

    void Sample(RE::Actor* a_actor) {
        if (!Settings::GetSingleton().companionProbe || !a_actor) {
            return;
        }
        auto* root = a_actor->Get3D();
        if (!root) {
            return;
        }
        // Counters belong to ONE actor. Fitting Room can name a different
        // follower without the menu ever closing, and carrying counts across
        // that would average two skeletons into one unreadable number.
        if (const std::uint32_t id = a_actor->GetFormID(); id != g_s.owner) {
            Clear();
            g_s.owner = id;
        }
        SampleBone(a_actor, root);
        SampleGeometry(a_actor);
    }

    void Report() {
        if (!g_s.owner) {
            return;  // never armed with a companion - no false zero
        }
        if (g_s.boneMissing || g_s.ticks == 0) {
            spdlog::info("companion probe: bone '{}' was NOT FOUND on 0x{:08X}'s "
                         "skeleton. This run measured NOTHING. Point "
                         "sCompanionProbeBone at a bone that exists before reading "
                         "anything into the silence.",
                         g_s.boneName.empty() ? "(unset)" : g_s.boneName, g_s.owner);
            Clear();
            return;
        }
        spdlog::info("companion probe: '{}' over {} armed tick(s): local changed {}x "
                     "(max {:.5f}), world changed {}x (max {:.5f}). {}",
                     g_s.boneName, g_s.ticks, g_s.localChanged, g_s.localMaxDelta,
                     g_s.worldChanged, g_s.worldMaxDelta, BoneVerdict());
        if (g_s.haveGeometry) {
            spdlog::info("companion probe geometry: player {:.0f}u from her; camera "
                         "{:.0f}u from her and {:.0f}u from him; player sits {:.0f} deg "
                         "off the camera-to-her axis. {}",
                         g_s.playerToHer, g_s.camToHer, g_s.camToPlayer, g_s.offAxisDeg,
                         GeometryVerdict());
        }
        Clear();
    }

    void Reset() { Clear(); }

}  // namespace MTB::CompanionProbe

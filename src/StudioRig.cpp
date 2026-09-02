#include "PCH.h"

#include "StudioRig.h"

#include "CompanionShotPolicy.h"  // the one answer to "are these two one shot?"
#include "Declutter.h"  // FramedCompanion: the second subject, if there is one
#include "Offsets.h"
#include "Settings.h"
#include "Transition.h"

#include <array>
#include <cmath>
#include <cstring>

namespace {
    // po3-fork layout, byte-verified against the 1.5.97 AddLight decompile
    // (shadow branch reads +0x01 and +0x08; Tools\re\research\mtb_studiorig.c).
    // dynamic=true: static BSLights added mid-session never re-enter the
    // per-geometry light lists (round-5 field: registered fine, lit
    // nothing); dynamic lights are re-evaluated every frame - the same
    // path a torch equipped in a paused menu uses.
    struct LightCreateParams {
        bool                dynamic{ true };        // 00
        bool                shadowLight{ false };   // 01
        bool                portalStrict{ false };  // 02
        bool                affectLand{ true };     // 03
        bool                affectWater{ false };   // 04
        bool                neverFades{ true };     // 05
        float               fov{ 0.0f };            // 08
        float               falloff{ 1.0f };        // 0C
        float               nearDistance{ 5.0f };   // 10
        float               depthBias{ 1.0f };      // 14
        std::uint32_t       sceneGraphIndex{ 0 };   // 18
        RE::NiAVObject*     restrictedNode{ nullptr };  // 20
        void*               lensFlareData{ nullptr };   // 28
    };
    static_assert(sizeof(LightCreateParams) == 0x30);

    using CreatePointLight_t = RE::NiPointLight* (*)();
    using AddLight_t = RE::BSLight* (*)(RE::ShadowSceneNode*, RE::NiLight*,
                                        const LightCreateParams*);
    using RemoveLight_t = void (*)(RE::ShadowSceneNode*, RE::NiPointer<RE::BSLight>&);

    REL::Relocation<CreatePointLight_t> g_createPointLight{ MTB::Offsets::NiPointLightCreate };
    REL::Relocation<AddLight_t>         g_addLight{ MTB::Offsets::ShadowSceneAddLight };
    REL::Relocation<RemoveLight_t>      g_removeLight{ MTB::Offsets::ShadowSceneRemoveLight };

    struct RigSpec {
        const char* name;
        float       angleOffset;  // radians from player heading (0 = facing dir)
        float       distance;
        float       zOffset;
        RE::NiColor color;
        float       radius;
        float       fade;
    };
    // Player faces the camera under SPIM, so heading+0 is between character
    // and camera: key front-left-high, fill front-right-low, rim behind-high.
    constexpr std::array<RigSpec, 3> kRig{ {
        { "MTB_RigKey", 0.66f, 150.0f, 115.0f, { 1.00f, 0.95f, 0.87f }, 380.0f, 2.0f },
        { "MTB_RigFill", -0.84f, 175.0f, 55.0f, { 0.72f, 0.78f, 0.90f }, 420.0f, 1.0f },
        { "MTB_RigRim", 3.27f, 130.0f, 150.0f, { 1.00f, 1.00f, 1.00f }, 320.0f, 1.6f },
    } };

    struct ActiveLight {
        RE::NiPointer<RE::NiPointLight> node;
        RE::NiPointer<RE::BSLight>      bsLight;
    };
    std::array<ActiveLight, kRig.size()> g_active{};
    bool g_rigUp = false;

    // WHAT THE RIG IS POINTED AT. Solo is spread == 0, and every formula below
    // then reduces to exactly the arithmetic that shipped before a companion
    // existed - centre is the player, heading is the player's own facing, and
    // distance and radius are the kRig literals untouched. That equivalence is
    // the point: a solo studio shot must not change because two-shot support
    // was added.
    struct Framing {
        RE::NiPoint3 centre{};
        float        heading{ 0.0f };
        float        spread{ 0.0f };  // distance between the two subjects
    };

    Framing g_framing{};  // what the lights were last PLACED with

    // The pair's "front" is the CAMERA's, not either character's. Solo can use
    // the player's own facing because SPII turns the character to face the
    // camera, so the two nearly agree (field 2026-07-31: player heading 0.94
    // against a camera bearing of 0.73). With two subjects there is no single
    // facing to borrow, and splitting the difference between two headings would
    // key-light neither of them.
    float CameraBearing(const RE::NiPoint3& a_centre, float a_fallback) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* root = cam ? cam->cameraRoot.get() : nullptr;
        if (!root) {
            return a_fallback;
        }
        const RE::NiPoint3 d = root->world.translate - a_centre;
        // A whole world unit, not an epsilon. These are Skyrim units where a
        // person is ~120 tall, so a sub-unit horizontal offset is a camera
        // effectively overhead and atan2 on it returns a bearing built from
        // float noise, which would then swing the entire rig.
        if (std::fabs(d.x) < 1.0f && std::fabs(d.y) < 1.0f) {
            return a_fallback;
        }
        // Skyrim yaw, matching the placement below: 0 points along +Y.
        return std::atan2(d.x, d.y);
    }

    // The thresholds and the hysteresis rule live in CompanionShotPolicy, which
    // Declutter also uses to decide whether to stand the player down. They were
    // duplicated here until the player hide needed the same bound, and two
    // copies of a stale-handle guard is one copy too many.

    bool g_pairLatched = false;  // hysteresis state, reset by Apply

    // Why a reject happened, so the log can say it once rather than leaving a
    // silently-solo rig with nothing to correlate against.
    enum class Reject { kNone, kNoCompanion, kNoTarget3D, kCulled, kOtherCell, kTooFar,
                        kDifferentLevel };
    Reject g_lastReject = Reject::kNone;

    const char* RejectWhy(Reject a_r) {
        switch (a_r) {
        case Reject::kNoTarget3D: return "the actor has no 3D";
        case Reject::kCulled:     return "the actor is app-culled (not on screen)";
        case Reject::kOtherCell:  return "the actor is in a different cell";
        case Reject::kTooFar:     return "the actors are too far apart to be one shot";
        case Reject::kDifferentLevel:
            return "the actors are on different levels (one is above or below the "
                   "other), which is not a two-shot at any horizontal distance";
        default:                  return "";
        }
    }

    Framing ComputeFraming(RE::PlayerCharacter* a_player) {
        Framing f;
        f.centre = a_player->GetPosition();
        f.heading = a_player->data.angle.z;

        const auto reject = [&](Reject a_why) -> Framing {
            g_pairLatched = false;
            if (g_lastReject != a_why) {
                g_lastReject = a_why;
                if (const char* why = RejectWhy(a_why); *why) {
                    spdlog::debug("studio rig: framing the player alone: {}.", why);
                }
            }
            return f;
        };

        const auto companion = MTB::Declutter::FramedCompanion().get();
        if (!companion || companion.get() == a_player) {
            return reject(Reject::kNoCompanion);
        }
        // Nothing to light. Each of these resolves to a VALID handle, so none
        // is caught by the null check above.
        auto* other3D = companion->Get3D();
        if (!other3D) {
            return reject(Reject::kNoTarget3D);
        }
        // ⚠ APP-CULLED IS NOT A REJECT FOR A COMPANION WE GOT FROM
        // FramedCompanion(), AND REJECTING ON IT WAS OS-103. The note that used
        // to sit here had the mechanism exactly right and drew the opposite
        // conclusion from it: "the cull survives until the next sweep - which is
        // up to fifteen ticks after the companion is named". That makes the flag
        // STALE BY DESIGN in this window, describing a sweep that ran BEFORE she
        // was named rather than her actual state. Being the framed companion IS
        // the exemption, so the next sweep lifts it.
        //
        // Measured cost of trusting it, from the field log that closed OS-103:
        //
        //   04:08:07.508  declutter: framing companion 'Jenassa'
        //   04:08:07.510  studio rig: framing the player alone - app-culled
        //   04:08:07.593  declutter: the player is standing down
        //
        // The rig decided 83 ms before the sweep that owns that flag had run for
        // her, and framed the player alone at exactly the moment the whole point
        // was to stop doing that. Declutter reads the same question correctly
        // only because it evaluates AFTER its own resweep.
        //
        // Kept as a one-shot log rather than deleted: a cull here is still worth
        // seeing, because a companion who is STILL culled several sweeps later
        // is a real fault somewhere else, and the line is what would show it.
        // The original worry was that the rig would "swing for a two-shot whose
        // second subject is still invisible" - it does, for the one or two
        // frames until the sweep lands, which is a lighting blip against a
        // wrongly composed shot for as long as the menu is open.
        if (other3D->GetAppCulled() && g_lastReject != Reject::kCulled) {
            g_lastReject = Reject::kCulled;
            spdlog::debug("studio rig: the framed companion is app-culled but she is "
                          "FRAMED, so the flag is a leftover from the sweep before she "
                          "was named. Composing the two-shot anyway; the next sweep "
                          "un-culls her (OS-103).");
        }
        auto* cell = a_player->GetParentCell();
        if (!cell || companion->GetParentCell() != cell) {
            return reject(Reject::kOtherCell);
        }
        const auto other = companion->GetPosition();
        // THE PLAYER IS NOT ON SCREEN, so there is no pair to compose. Light HER
        // as the soloist: centre on her, take the bearing from her, and leave
        // spread at zero, which makes every formula below reduce to exactly the
        // one-subject arithmetic that shipped before a companion existed.
        //
        // Read from Declutter rather than re-derived, and read FIRST, so the two
        // modules cannot disagree about who the camera can see. Whatever this
        // function would otherwise have decided, a rig that lights a midpoint
        // between her and a hidden player is lighting half a shot at nobody.
        if (MTB::Declutter::PlayerHiddenForCompanion()) {
            g_pairLatched = false;
            g_lastReject = Reject::kNone;
            f.centre = other;
            f.heading = CameraBearing(other, f.heading);
            f.spread = 0.0f;
            return f;
        }
        const RE::NiPoint3 d = other - f.centre;
        const auto sep = MTB::CompanionShotPolicy::SeparationOf(d.x, d.y, d.z);
        if (!MTB::CompanionShotPolicy::SameLevel(sep, g_pairLatched)) {
            return reject(Reject::kDifferentLevel);
        }
        if (!MTB::CompanionShotPolicy::CloseEnough(sep, g_pairLatched)) {
            return reject(Reject::kTooFar);
        }
        // The full 3D distance, unchanged. This is what the light placement
        // below steps back by and scales its reach against, and the gate above
        // now bounds the vertical term tightly enough that the two agree to
        // within a few units anyway. Changing it here would be an unrelated
        // change to shipped rig arithmetic.
        const float spread = f.centre.GetDistance(other);

        g_pairLatched = true;
        g_lastReject = Reject::kNone;
        f.spread = spread;
        f.centre = RE::NiPoint3{ (f.centre.x + other.x) * 0.5f,
                                 (f.centre.y + other.y) * 0.5f,
                                 (f.centre.z + other.z) * 0.5f };
        f.heading = CameraBearing(f.centre, f.heading);
        return f;
    }

    // Step the lights back by half the separation so the pair sits where one
    // subject used to, then SCALE the reach to match.
    //
    // ⚠ THE REACH MULTIPLIES, IT DOES NOT ADD, and the first version of this
    // got it wrong. Radius is not a cutoff you either clear or miss: it is the
    // falloff normaliser, and brightness follows d/r. Adding the same constant
    // to distance and radius preserves r - d while moving d/r, so the far
    // subject slid 47% further along its own falloff curve while the near one
    // came in - the key stopped being a key in both directions at once.
    // Scaling r by (d + spread)/d holds d/r exactly at the worst-case bearing,
    // which is what "as much a key as it was" actually means.
    //
    // Stepping back and opening up is what a photographer does for a two-shot
    // rather than relighting. What it cannot do is hold the three-point RATIO
    // for both subjects at once: each one is nearer some lights than the other,
    // so A reads slightly contrastier and B slightly flatter. Two rigs is the
    // only real fix for that, and it is not worth six lights yet.
    // The one scale factor everything uses. 1.0 when solo, so every formula
    // below returns its kRig literal untouched.
    float RigScale(const RigSpec& a_spec, const Framing& a_f) {
        if (a_f.spread <= 0.0f || a_spec.distance <= 0.0f) {
            return 1.0f;
        }
        return (a_spec.distance + a_f.spread) / a_spec.distance;
    }

    RE::NiPoint3 LightWorldPos(const RigSpec& a_spec, const Framing& a_f) {
        const float a = a_f.heading + a_spec.angleOffset;
        const float d = a_spec.distance + a_f.spread * 0.5f;
        // ⚠ Z SCALES TOO, and leaving it flat was the subtler half of the same
        // mistake as the additive radius. The engine shades on 3D distance, so
        // an unscaled z is not merely a smaller share of a longer throw: it
        // drops the ELEVATION. At spread 166 the key fell from 37.5 degrees to
        // 20, and at 400 to 11.8 - which is not a key light any more, it is a
        // flat frontal wash with no nose shadow and no cheek falloff, and the
        // rim stops raking hair and starts raking the floor into the lens.
        // Scaling z by the same factor as the radius preserves the elevation
        // angle exactly AND makes the full 3D d/r invariant rather than just
        // the horizontal one.
        return RE::NiPoint3{ a_f.centre.x + std::sin(a) * d,
                             a_f.centre.y + std::cos(a) * d,
                             a_f.centre.z + a_spec.zOffset * RigScale(a_spec, a_f) };
    }

    float LightRadius(const RigSpec& a_spec, const Framing& a_f) {
        return a_spec.radius * RigScale(a_spec, a_f);
    }

    // Worth a re-place? Actors breathe and drift a unit or two, and re-placing
    // three lights every tick to chase that would be noise in the log and churn
    // in the scene. Only a real change moves them: a companion arriving or
    // leaving, or someone actually walking.
    bool FramingMoved(const Framing& a_now, const Framing& a_was) {
        // Headings WRAP. A camera bearing crossing +pi to -pi is a hair's
        // movement that a plain subtraction reads as a full turn, so a subject
        // standing on that seam would thrash the rig. Compare the short way
        // round.
        constexpr float kTwoPi = 6.2831853f;
        float dh = std::fmod(a_now.heading - a_was.heading, kTwoPi);
        if (dh > kTwoPi * 0.5f) {
            dh -= kTwoPi;
        } else if (dh < -kTwoPi * 0.5f) {
            dh += kTwoPi;
        }
        return a_now.centre.GetDistance(a_was.centre) > 8.0f ||
               std::fabs(a_now.spread - a_was.spread) > 8.0f ||
               std::fabs(dh) > 0.05f;
    }

    // r39 (field: "the 3 point light system sometimes can appear even when
    // the menu is closed"): Remove() re-derived the shadow scene node from
    // the player's 3D at TEARDOWN time - when that walk failed, the code
    // dropped our BSLight references WITHOUT deregistering them, and the
    // SSN kept the rig lighting the character in normal gameplay until the
    // next load. Hold the scene node the lights were REGISTERED with from
    // Apply to Remove instead (as NiAVObject: ShadowSceneNode's NiNode
    // base is private in NG - the r5 lesson - so the public-base pointer
    // carries the refcount and we reinterpret at the call).
    RE::NiPointer<RE::NiAVObject> g_ssnHold;

    // Live values: colors/intensities/enables flow from the effective look
    // (manual or auto time-of-day) every armed tick, so slider drags and
    // the game clock both read back in real time. The transition scalar
    // (F-12) rides the same write: the rig ramps in at arm and dissolves
    // through the teardown grace.
    void PushConfig(std::size_t a_index, RE::NiPointLight* a_light,
                    const MTB::Settings::LookValues& a_look) {
        const auto& cfg = MTB::Settings::GetSingleton();
        const auto& lc = a_index == 0 ? a_look.key
                       : a_index == 1 ? a_look.fillLight
                                      : a_look.rim;
        auto& data = a_light->GetLightRuntimeData();
        data.diffuse = RE::NiColor{ lc.color.red / 255.0f, lc.color.green / 255.0f,
                                    lc.color.blue / 255.0f };
        data.fade = lc.enabled
                        ? kRig[a_index].fade * lc.intensity * cfg.rigBrightness *
                              MTB::Transition::Value()
                        : 0.0f;
    }

    RE::ShadowSceneNode* FindShadowSceneNode(RE::NiAVObject* a_from) {
        for (auto* node = a_from ? a_from->parent : nullptr; node; node = node->parent) {
            // NG declares the NiNode base private, which blocks
            // netimmerse_cast's static_cast - match the NiRTTI by name and
            // reinterpret (single inheritance, base at offset 0).
            if (const auto* rtti = node->GetRTTI();
                rtti && rtti->name && std::strcmp(rtti->name, "ShadowSceneNode") == 0) {
                return reinterpret_cast<RE::ShadowSceneNode*>(node);
            }
        }
        return nullptr;
    }
}

namespace MTB::StudioRig {
    // Forwarder onto the file-local one the placement maths already uses, so
    // there is exactly one definition of where the front of the shot is.
    float CameraBearing(const RE::NiPoint3& a_centre, float a_fallback) {
        return ::CameraBearing(a_centre, a_fallback);
    }

    void Apply() {
        const auto& cfg = Settings::GetSingleton();
        // The rig lights the framed character in the Void and the Dressing
        // room, and in Off / Scene view too once bRigWithoutSpace is on (F-24:
        // "Natural world behind me + this mod's lighting"). Nothing below cares
        // which view is up: the lights hang off the PLAYER's own scene parent
        // and the shadow scene node walked from the player's 3D, never off
        // backdrop or void geometry, so there is no space to depend on.
        if (g_rigUp || !cfg.RigAllowed()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerRoot = player ? player->Get3D(false) : nullptr;
        auto* parent = playerRoot ? playerRoot->parent : nullptr;
        auto* ssn = FindShadowSceneNode(playerRoot);
        if (!parent || !ssn) {
            spdlog::warn("studio rig: no scene parent/shadow scene node, rig skipped.");
            return;
        }
        g_ssnHold = RE::NiPointer<RE::NiAVObject>{ reinterpret_cast<RE::NiAVObject*>(ssn) };

        g_pairLatched = false;  // fresh arm, no hysteresis state carried over
        g_lastReject = Reject::kNone;
        const Framing framing = ComputeFraming(player);
        g_framing = framing;
        if (framing.spread > 0.0f) {
            spdlog::debug("studio rig: two-shot framing: centre ({:.0f},{:.0f},{:.0f}), "
                          "spread {:.0f}, camera bearing {:.2f}.",
                          framing.centre.x, framing.centre.y, framing.centre.z,
                          framing.spread, framing.heading);
        }

        // Parent-local placement: cell roots usually carry identity
        // transforms, but never assume - invert properly.
        const auto& pw = parent->world;
        const auto pwRotInv = pw.rotate.Transpose();
        const float invScale = pw.scale != 0.0f ? 1.0f / pw.scale : 1.0f;

        int up = 0;
        for (std::size_t i = 0; i < kRig.size(); ++i) {
            const auto& spec = kRig[i];
            auto* light = g_createPointLight();
            if (!light) {
                continue;
            }
            light->name = spec.name;
            const float radius = LightRadius(spec, framing);
            auto& data = light->GetLightRuntimeData();
            data.ambient = RE::NiColor{ 0.0f, 0.0f, 0.0f };
            data.radius = RE::NiPoint3{ radius, radius, radius };
            PushConfig(i, light, cfg.CurrentLook());  // diffuse + fade, live look

            const RE::NiPoint3 world = LightWorldPos(spec, framing);
            parent->AttachChild(light, true);
            light->local.translate = (pwRotInv * (world - pw.translate)) * invScale;
            light->world.rotate = RE::NiMatrix3();
            light->world.translate = world;
            light->world.scale = 1.0f;
            // A bare-created light has a ZERO world bound - light gathering
            // intersects the light's bound against geometry, so a zero
            // bound lights nothing regardless of registration (round-6
            // field: dynamic lights up, zero photons). Give it the real
            // sphere.
            light->worldBound.center = world;
            light->worldBound.radius = radius;
            spdlog::debug("studio rig: '{}' at ({:.0f},{:.0f},{:.0f}) r={:.0f}.",
                          spec.name, world.x, world.y, world.z, radius);

            LightCreateParams params{};
            auto* bsLight = g_addLight(ssn, light, &params);
            if (!bsLight) {
                spdlog::warn("studio rig: AddLight refused '{}'.", spec.name);
                if (auto* p = light->parent) {
                    p->DetachChild2(light);
                }
                continue;
            }
            g_active[i].node = RE::NiPointer<RE::NiPointLight>{ light };
            g_active[i].bsLight = RE::NiPointer<RE::BSLight>{ bsLight };
            ++up;
        }
        g_rigUp = up > 0;
        if (g_rigUp) {
            spdlog::debug("studio rig: {} light(s) up (brightness {:.2f}).",
                          up, cfg.rigBrightness);
        }
    }

    void Tick() {
        const auto& cfg = MTB::Settings::GetSingleton();
        // Live master toggle / view-mode change: spawn/remove mid-arm.
        if (g_rigUp && !cfg.RigAllowed()) {
            MTB::StudioRig::Remove();
            return;
        }
        if (!g_rigUp && cfg.RigAllowed()) {
            MTB::StudioRig::Apply();
        }
        if (!g_rigUp) {
            return;
        }
        // FOLLOW THE FRAMING. The companion is named by another mod through
        // Declutter's API, which can land long after the rig went up - the
        // editor opens on the player and the user picks a follower seconds
        // later. Without this the lights stay keyed to a solo shot that no
        // longer exists, and the follower is visible but lit by falloff.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            const Framing now = ComputeFraming(player);
            // ⚠ SOLO NEVER RE-PLACES. Before the companion existed the rig was
            // nailed to the point where the menu opened and stayed there for
            // the whole session, and in the Souls-live path the world is
            // RUNNING - a walking player crosses the 8-unit gate in about a
            // tenth of a second, so an ungated re-place would have the solo rig
            // chasing them at frame rate. That is a change to a shipped path,
            // and it is not the change being made here.
            //
            // The second half of the test matters as much as the first: a
            // companion who just left has to be re-placed back to solo framing,
            // and that transition is spread > 0 in the OLD framing only.
            const bool pairInvolved = now.spread > 0.0f || g_framing.spread > 0.0f;
            if (pairInvolved && FramingMoved(now, g_framing)) {
                // The CAST changing, not the spread drifting. Measuring a delta
                // since the last re-place re-arms itself continuously: two
                // subjects walking apart accumulate 8 units of spread every few
                // frames, and each one is a synchronous flush_on(debug) disk
                // write. Solo-versus-paired is a state, and it changes twice.
                const bool castChanged =
                    (now.spread > 0.0f) != (g_framing.spread > 0.0f);
                for (std::size_t i = 0; i < g_active.size(); ++i) {
                    auto* light = g_active[i].node.get();
                    // The light's OWN parent, not a freshly walked
                    // player->Get3D()->parent. In the live path those two can
                    // diverge - an armour equip rebuilds the player's 3D, a load
                    // door swaps the cell root - and then local.translate would
                    // be written in one basis while world.translate is written in
                    // another. The next downward pass recomputes world from that
                    // local and throws the light somewhere unrelated.
                    auto* parent = light ? light->parent : nullptr;
                    if (!light || !parent) {
                        continue;
                    }
                    const auto& pw = parent->world;
                    const auto pwRotInv = pw.rotate.Transpose();
                    const float invScale = pw.scale != 0.0f ? 1.0f / pw.scale : 1.0f;
                    const auto& spec = kRig[i];
                    const RE::NiPoint3 world = LightWorldPos(spec, now);
                    const float radius = LightRadius(spec, now);
                    light->local.translate = (pwRotInv * (world - pw.translate)) * invScale;
                    light->world.translate = world;
                    light->worldBound.center = world;
                    light->worldBound.radius = radius;
                    light->GetLightRuntimeData().radius =
                        RE::NiPoint3{ radius, radius, radius };
                }
                g_framing = now;
                // Only when the CAST changed, never when the shot merely
                // translated. The logger runs at debug with flush_on(debug), so
                // a line per re-place is a synchronous disk write, and a
                // translating shot re-places continuously.
                if (castChanged) {
                    spdlog::debug("studio rig: re-framed: centre "
                                  "({:.0f},{:.0f},{:.0f}), spread {:.0f}, heading {:.2f}.",
                                  now.centre.x, now.centre.y, now.centre.z, now.spread,
                                  now.heading);
                }
            }
        }
        // Cheap defense: some engine passes recompute world data from local
        // or reset bounds; keep the rig's world state exactly as placed -
        // and un-culled (the declutter sweep now exempts MTB_ nodes, but a
        // pre-fix sweep entry restored on close could re-flag mid-session).
        // PushConfig makes panel color/intensity edits land the same frame.
        const auto look = cfg.CurrentLook();
        for (std::size_t i = 0; i < g_active.size(); ++i) {
            if (auto* light = g_active[i].node.get()) {
                light->worldBound.center = light->world.translate;
                light->worldBound.radius = light->GetLightRuntimeData().radius.x;
                if (light->GetAppCulled()) {
                    light->SetAppCulled(false);
                }
                PushConfig(i, light, look);
            }
        }
    }

    void LogLiveState(const char* a_why) {
        if (!g_rigUp) {
            spdlog::info("rig readback [{}]: rig NOT up (T={:.2f}).", a_why,
                         MTB::Transition::Value());
            return;
        }
        for (std::size_t i = 0; i < g_active.size(); ++i) {
            auto* light = g_active[i].node.get();
            if (!light) {
                spdlog::info("rig readback [{}]: light {} node GONE.", a_why, i);
                continue;
            }
            const auto& data = light->GetLightRuntimeData();
            spdlog::info(
                "rig readback [{}]: {} fade={:.3f} boundR={:.0f} culled={} parent={} "
                "world=({:.0f},{:.0f},{:.0f}) T={:.2f} bsLight={}",
                a_why, light->name.c_str(), data.fade, light->worldBound.radius,
                light->GetAppCulled(), light->parent ? "yes" : "NULL",
                light->world.translate.x, light->world.translate.y,
                light->world.translate.z, MTB::Transition::Value(),
                g_active[i].bsLight ? "held" : "NULL");
        }
    }

    void PushFade() {
        // Grace-window refresh (F-12): only the fade values move - no
        // Apply/Remove decisions, no transform writes; the world is live
        // again out there and this must stay a pure visual dissolve.
        if (!g_rigUp) {
            return;
        }
        const auto look = MTB::Settings::GetSingleton().CurrentLook();
        for (std::size_t i = 0; i < g_active.size(); ++i) {
            if (auto* light = g_active[i].node.get()) {
                PushConfig(i, light, look);
            }
        }
    }

    void Remove() {
        if (!g_rigUp) {
            g_ssnHold.reset();
            return;
        }
        // Deregister from the node the lights were REGISTERED with (held
        // since Apply) - never from a fresh player walk, which can fail
        // exactly when the teardown races a 3D change and used to leak the
        // rig into gameplay. The walk stays as a legacy fallback only.
        auto* ssn = g_ssnHold
            ? reinterpret_cast<RE::ShadowSceneNode*>(g_ssnHold.get())
            : FindShadowSceneNode(
                  RE::PlayerCharacter::GetSingleton()
                      ? RE::PlayerCharacter::GetSingleton()->Get3D(false)
                      : nullptr);
        int down = 0;
        int leaked = 0;
        for (auto& active : g_active) {
            if (active.bsLight) {
                if (ssn) {
                    g_removeLight(ssn, active.bsLight);
                    ++down;
                } else {
                    ++leaked;
                }
            }
            active.bsLight.reset();
            if (active.node) {
                if (auto* p = active.node->parent) {
                    p->DetachChild2(active.node.get());
                }
                active.node.reset();
            }
        }
        g_rigUp = false;
        g_ssnHold.reset();
        if (leaked > 0) {
            spdlog::warn("studio rig: {} light(s) could NOT be deregistered "
                         "(no scene node at teardown), rig-leak tripwire; "
                         "report this line with what preceded it.", leaked);
        } else {
            spdlog::debug("studio rig: {} light(s) removed.", down);
        }
    }
}

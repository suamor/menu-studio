#include "PCH.h"

#include "CompanionShotPolicy.h"
#include "CullWatch.h"
#include "Declutter.h"
#include "DeclutterRefPolicy.h"
#include "PathCullPolicy.h"
#include "Settings.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
    // Refs WE culled (only those whose 3D was visible before), to restore
    // exactly and never un-hide something another mod hid. Handles, not
    // pointers: a quickload invalidates them and resolution just fails.
    std::vector<RE::ObjectRefHandle> g_hidden;

    // Interior cell-root children we culled (solo mode). NiPointer keeps the
    // nodes alive until RestoreAll, which always runs before any unload
    // (menu close / disarm / ForceReset at kPreLoadGame).
    std::vector<RE::NiPointer<RE::NiAVObject>> g_hiddenNodes;

    // r33: cell light SOURCES cut this arm (see CutCellLightSources) - the
    // walk is once per arm, not per periodic sweep; RestoreAll re-arms it.
    bool g_cellLightsCut = false;

    // r37 F-20 VOID ENGINE: world feeders cut this arm (grass root, land
    // quadrants, LOD roots, precipitation geometry) - once per arm, the
    // paused world can't grow new ones; RestoreAll re-arms it.
    bool g_worldFeedersCut = false;
    // One arm summary per arm. Reset with the rest of the per-arm one-shots in
    // RestoreAll, so a menu switch that tears down and rebuilds says so again.
    bool g_armSummaryLogged = false;

    // F-30. THE PATH CULL'S OWN PATH, ONCE PER ARM.
    //
    // ⚠⚠ THE OLD CLIMB NEVER SAID WHERE IT WALKED, and it had THREE silent
    // early-outs on top of that. This line is what convicted it (field
    // 2026-08-20, large list): a depth-4 chain through the cell's big object
    // list reached ~1740 siblings and worked; a depth-5 chain through 'Portal
    // Shared Geometry' reached ~73 and left the inn rendering. Room-bound
    // interiors parent the player under portal geometry, so the chain's shape
    // is the whole bug - and it cleared the two suspects the correlations had
    // convicted first (an SPII sweep-order race at "19 of 19", which was 2
    // failures against 17 successes, and the Get3D first-person trap, with
    // both models measured on both sides of the failure).
    //
    // It stays in after the fix: the pass condition in the field is that
    // depth STOPS predicting the cull. Once per arm, so a 4-per-second
    // re-cull cannot bury it.
    bool g_pathDumpLogged = false;
    // F-30 round 2: the cell root's own children, named with RTTI, once per
    // arm. Exists because round 1 proved coverage was never the problem - the
    // descent culled the same containers the climb culled, the flags held,
    // and the room rendered anyway - so the next question is WHAT hangs there
    // that a container cull cannot reach.
    bool g_cellChildrenLogged = false;

    // F-30 round 3: THE PORTAL GRAPH'S OWN HOOK INTO THE SCENE, parked while
    // armed. Field 2026-08-20 05:37 closed every other door: 10840 per-node
    // culls, flags re-read true at 4 Hz for the whole arm, freeze-frame flags
    // clean - and the inn's rafters still swung in and out of frame WITH THE
    // CAMERA ANGLE. Angle-dependence is portal culling's signature: the
    // ShadowSceneNode's BSPortalGraph draws room and shared-portal content
    // directly (its portalSharedNode points at the 'Shared Portal Geometry'
    // node the engine parents the player under in a room-bound interior), so
    // that content never passes the scene traversal that honours AppCulled.
    // Cells without room bounds run with the pointer null, which is what
    // makes null a state the engine already survives. NiPointer holds the
    // node alive while parked; the graph pointer itself is IDENTITY ONLY and
    // never dereferenced at restore unless it is still the live graph.
    RE::BSPortalGraph*        g_parkedPortalGraph = nullptr;
    RE::NiPointer<RE::NiNode> g_parkedPortalNode;
    bool                      g_portalParkLogged  = false;
    std::size_t               g_portalReParks     = 0;

    // F-30 round 6 instrument: the cast's own render state, every ~2s while
    // armed. The character still blinks out by camera angle in a room-bound
    // interior (field 06:1x-15:1x, eFPS suspected), and the one question that
    // picks the next lever is whether the ENGINE WRITES HIS CULL FLAG (then a
    // pin re-asserts it clear, the OS-103 idiom) or the flag stays clear and
    // he simply is not drawn (then the occlusion evaluation is render-side
    // and flag-pinning is a dead end). ⚠ Round 5 already taught the cost of
    // guessing here: parking the graph's u58 list un-hid the ROOM, because
    // occlusion planes were doing half the void's hiding for it. Measure
    // first this time.
    std::size_t g_castProbeCalls = 0;

    // F-30 round 7: PIN THE THIRD-PERSON BODY VISIBLE WHILE ARMED. The probe
    // split the character-vanish cleanly (field 15:41): on every broken arm
    // the 3p root read culled=true with the 1p at culled=false boundR=1 - the
    // engine's FIRST-PERSON resting state - while healthy arms read the pair
    // flipped. So nothing un-culled her body on those opens (SPII or the
    // engine skips the flip on some first-person opens), and the studio
    // orbited an actor whose root was flagged hidden. The pin is the OS-103
    // idiom in the opposite direction: re-assert visible each Refresh, count
    // the re-asserts (a count near the tick rate means a per-frame writer and
    // this needs a hook instead), and hand the found state back at restore.
    // ⚠ The companion shot's deliberate player-hide (g_playerHideWanted)
    // WINS: this pin only runs when nobody asked him hidden.
    bool        g_playerPinApplied   = false;
    std::size_t g_playerPinReAsserts = 0;

    // F-30 round 8: ALWAYS-DRAW ON THE CAST WHILE ARMED. Round 7's pin held
    // (23 arms, zero re-asserts, every probe culled=false) and parts of the
    // character still vanished by camera angle - so the writer is not a flag
    // at all: the occlusion planes (vanilla + eFPS) cull her GEOMETRY at
    // render accumulation, per piece, against walls the void has hidden.
    // kAlwaysDraw is the engine's own bypass for exactly that test. Set on
    // every node of each cast subtree while armed; only nodes that LACKED the
    // bit are recorded, and the restore clears exactly those, so a mesh that
    // ships with kAlwaysDraw keeps it. ⚠ Round 5's lesson stands: this
    // touches the CAST ONLY, never the room - the planes keep hiding the
    // world for the void.
    std::vector<RE::NiPointer<RE::NiAVObject>> g_alwaysDrawSet;
    bool                                       g_alwaysDrawLogged = false;

    // F-30 round 4: THE CAST LIVES IN THE PARKED SUBTREE, so parking it took
    // the character down with the rafters (field 05:54: world mostly gone,
    // character gone too - and pre-park screenshots never showed him either,
    // because the graph, not the traversal, decided his visibility). While
    // armed, any cast root whose chain runs through the parked shared node is
    // re-parented onto cell3D - the exact shape the healthy arms already have,
    // where the traversal draws him and our culls all hold. The original
    // parent is kept alive by NiPointer and handed back at restore.
    struct ReparentedRoot {
        RE::NiPointer<RE::NiAVObject> root;
        RE::NiPointer<RE::NiNode>     oldParent;
    };
    std::vector<ReparentedRoot> g_reparented;
    std::size_t                 g_castReParents = 0;

    // ⚠ ShadowSceneNode PRIVATELY inherits NiNode in this CommonLib (the
    // ImageSpaceModifierInstance lesson again), so netimmerse_cast's
    // static_cast is inaccessible. The engine's own NiRTTI name is the
    // identity check instead, and the cast is a reinterpret over the same
    // object - private inheritance keeps the layout, only the conversion.
    RE::ShadowSceneNode* ShadowSceneAbove(RE::NiAVObject* a_cell3D) {
        for (auto* up = a_cell3D ? a_cell3D->parent : nullptr; up; up = up->parent) {
            if (const auto* rtti = up->GetRTTI();
                rtti && rtti->name && std::strcmp(rtti->name, "ShadowSceneNode") == 0) {
                return reinterpret_cast<RE::ShadowSceneNode*>(up);
            }
        }
        return nullptr;
    }

    // r37 F-22: active imagespace-modifier instances we zeroed (combat blur
    // frozen mid-ramp over the studio). RAW pointers by necessity -
    // ImageSpaceModifierInstance privately inherits NiObject in NG (the
    // ShadowSceneNode lesson again), so NiPointer won't compile. Safe
    // anyway: restore only touches instances still present in the engine's
    // own live list, so a pointer that died with a quickload is never
    // dereferenced.
    struct ImodSave {
        RE::ImageSpaceModifierInstance* inst;
        float strength;
    };
    std::vector<ImodSave> g_imodSaves;

    // r53: the player's mount stays visible in every cull path (r52 only
    // caught the actor sweep; the ref sweep re-hid the horse → "saddling
    // empty space"). One helper, used by both.
    RE::Actor* PlayerMount(RE::PlayerCharacter* a_player) {
        RE::ActorPtr mount;
        if (a_player && a_player->GetMount(mount)) {
            return mount.get();
        }
        return nullptr;
    }

    // The framed companion: a second actor the shot is deliberately about,
    // set by whoever owns the shot (Fitting Room's outfit editor, when it is
    // dressing a follower rather than the player). Empty by default, so the
    // solo studio shot is bit-for-bit what it was.
    RE::ActorHandle g_companion;

    // Whether a sweep has actually run since the companion was last set. Lets
    // a caller distinguish "she is not framed" from "she is not framed YET",
    // which are one Refresh apart and look identical from outside.
    bool g_companionSwept = false;

    // The cast changed mid-arm, so the next Refresh must restore before it
    // sweeps. Refresh only ever HIDES - it is idempotent and accumulates - so
    // without this a companion named after the first sweep stays culled for
    // the rest of the arm no matter how many times the sweep re-runs.
    // Restore-then-resweep is the same path a declutter-mode change already
    // uses, and it is the only one that also undoes the sibling cull and the
    // light cut rather than just the ref cull.
    bool g_castDirty = false;

    // THE CAST. Resolved once per sweep, because the r52/r53 mount bug is the
    // whole lesson here: an exemption honoured by ONE cull path is not an
    // exemption. The horse was skipped by the actor sweep and then re-hidden
    // by the ref sweep, and the field report was "saddling empty space".
    //
    // There are FOUR paths that can delete an actor from this shot, and a
    // companion has to survive all four:
    //   HideNearbyActors        - the process-list actor sweep
    //   HideEverythingElse      - the in-range ref sweep
    //   HideInteriorPathSiblings- culls every sibling on the player's branch,
    //                             and indoors a nearby actor IS such a sibling
    //   CutLightsUnder          - would cut her torch while keeping ours
    struct Cast {
        RE::PlayerCharacter* player{ nullptr };
        RE::Actor*           mount{ nullptr };
        RE::Actor*           companion{ nullptr };
        // ⚠⚠ BOTH player models, always (F-30 round 4b). Get3D() hands back
        // the FIRST-PERSON model whenever he stands in first person, and a
        // cast built from it alone leaves the THIRD-PERSON body - the one the
        // studio actually shows - unprotected: the room-bound rounds parked
        // and culled it with the scenery, and the frame had no character in
        // it. The documented [[player-get3d-is-first-person]] trap, caught
        // INSIDE the exemption machinery this time rather than at a hook.
        RE::NiAVObject*      playerRoot{ nullptr };     // third-person
        RE::NiAVObject*      playerRoot1st{ nullptr };  // first-person
        RE::NiAVObject*      companionRoot{ nullptr };

        [[nodiscard]] bool Has(const RE::TESObjectREFR* a_ref) const {
            return a_ref && (a_ref == player || a_ref == mount || a_ref == companion);
        }

        // Node-level membership, for the two walks that see scene nodes rather
        // than refs. Exact roots only: a descendant of the companion's root is
        // already spared because culling is checked at the root.
        [[nodiscard]] bool HasNode(const RE::NiAVObject* a_node) const {
            return a_node && (a_node == playerRoot || a_node == playerRoot1st ||
                              a_node == companionRoot);
        }
    };

    Cast ResolveCast(RE::PlayerCharacter* a_player) {
        Cast c;
        c.player        = a_player;
        c.mount         = PlayerMount(a_player);
        c.playerRoot    = a_player ? a_player->Get3D(false) : nullptr;
        c.playerRoot1st = a_player ? a_player->Get3D(true) : nullptr;
        // A companion who unloaded, died out of the process lists or was never
        // set resolves to null and the cast is simply the player and the horse.
        if (const auto ptr = g_companion.get()) {
            c.companion     = ptr.get();
            c.companionRoot = ptr->Get3D();
        }
        return c;
    }

    // Is the player currently off screen for a companion's sake? Assigned from
    // the cull flag itself every Refresh, never from intent.
    bool g_playerHidden = false;
    // OS-103(b). THE INTENT, kept apart from g_playerHidden, which is the
    // observation. Read by the Is3rdPersonVisible hook, and it must never be
    // derived from the cull flag: under the fix the engine sets that flag
    // BECAUSE of what the hook returned, so keying on it would close a loop.
    bool g_playerHideWanted = false;
    bool g_playerHideLogged = false;
    // OS-103 round 2: one-shot for the RE-hide, kept separate from the hide's
    // own latch. "We hid him" and "we had to hide him again" are different
    // facts and collapsing them would hide the second behind the first.
    bool g_playerReHideLogged = false;
    // OS-103 round 6. THE ROOT WE ACTUALLY CULLED, held for IDENTITY ONLY.
    //
    // ⚠ NEVER DEREFERENCED, and it must stay that way. SPII calls
    // Update3DPosition(true), and if that rebuilds his 3D then this pointer is
    // dangling by the next tick - comparing it is safe, reading through it is a
    // crash. It exists to answer ONE question that no flag can:
    //
    //   pointer CHANGED  -> his 3D was rebuilt, our cull went with the old root,
    //                       and re-culling the new one is the fix.
    //   pointer STABLE   -> same node, somebody clears the flag every frame, and
    //                       a per-tick race is the wrong shape of fix entirely.
    //
    // Those two have completely different answers and the cull flag alone cannot
    // tell them apart, which is why three rounds of flag-watching did not.
    const void* g_playerRootAtHide = nullptr;
    std::size_t g_playerReHides    = 0;
    std::size_t g_playerRootSwaps  = 0;

    // OS-103(b) phase probe. g_probeGapClears[i] counts the clears observed in
    // the gap ENDING at phase i, so the tally reads as "the flag died between
    // the previous sample and this one". g_probeGapSamples is the denominator:
    // without it a gap that is rarely reached looks quiet rather than clean.
    constexpr std::size_t kCullPhaseCount =
        static_cast<std::size_t>(MTB::Declutter::CullPhase::kCount);
    std::size_t g_probeGapClears[kCullPhaseCount]  = {};
    std::size_t g_probeGapSamples[kCullPhaseCount] = {};
    bool        g_probeLastCulled                  = false;
    bool        g_probeHaveLast                    = false;
    std::size_t g_probeLastPhase                   = 0;
    // ⚠ THE TALLY USED TO LAND ONLY AT RestoreAll, AND THAT COST A FIELD ROUND.
    // The 07:25 arm ran 104 seconds and produced NOTHING, because the game was
    // closed with the menu still open, so the restore never ran and the whole
    // measurement went with it. An absent tally then reads exactly like a zero
    // tally, which is the worst way for a probe to fail.
    //
    // So it also lands on a cadence, and the arm summary at restore becomes a
    // bonus rather than the only copy. ~2000 wrap samples is about twelve
    // seconds at the observed frame rate.
    constexpr std::size_t kProbeReportEvery = 2000;
    std::size_t g_probeSinceReport = 0;

    // OS-103(b) round 2: the Update3DPosition bracket. Independent of the ring
    // counters above, because this call does not fire on a fixed cadence.
    std::size_t g_u3dCalls        = 0;  // times we bracketed it while he was down
    std::size_t g_u3dCleared      = 0;  // times it turned the cull OFF across itself
    bool        g_u3dCulledBefore = false;
    bool        g_u3dArmed        = false;
    const char* const kCullPhaseNames[kCullPhaseCount] = {
        "pre-dispatch", "post-dispatch", "post-refresh", "wrap"
    };
    // Last value of the setting we actually acted on, so a mid-menu toggle can
    // trigger the restore-then-resweep that Refresh alone would never do (it
    // only ever hides, so turning the setting off cannot bring him back).
    bool g_lastPlayerHideSetting = true;

    bool g_hideLatched = false;  // CompanionShotPolicy hysteresis

    // Is she really a second subject in THIS shot? She has to be resolvable,
    // built, visible, in the same cell, and close enough to be one composition.
    // Standing the player down for a companion who is not on screen would leave
    // an empty frame, which is worse than the crowded one this is fixing, and
    // the distance bound is what stops a handle left set from taking the player
    // off screen in every later menu of the session.
    // OS-103. WHICH of the four early-outs answered "no". Fitting Room's field
    // report is that the player sometimes stays on screen with a follower as the
    // edit subject, and the success line above fired FOUR times in the very
    // session the counter-example screenshot came from, so this is intermittent
    // rather than broken. Four separate conditions can produce that one symptom
    // and nothing distinguished them, which is the whole reason a round was
    // spent guessing.
    //
    // ⚠ ONE LINE PER STEADY STATE, NOT PER REFRESH. This runs on every Refresh
    // and the menu refreshes constantly, so an unconditional log would bury the
    // answer in its own noise. Keyed on the reason CODE: a steady decline logs
    // once, and a FLAP between two reasons keeps logging, which is itself the
    // reading that separates a race from a settled no.
    int g_lastDeclineCode = 0;  // 0 == nothing declined since the last success

    void NoteDecline(int a_code, const char* a_why) {
        if (g_lastDeclineCode == a_code) {
            return;
        }
        g_lastDeclineCode = a_code;
        spdlog::info("declutter: the player is NOT standing down: {}", a_why);
    }

    bool CompanionIsOnScreen(RE::PlayerCharacter* a_player) {
        const auto ptr = g_companion.get();
        if (!ptr || ptr.get() == a_player) {
            NoteDecline(1, "no framed companion is set, or the subject IS the player. "
                           "Expected while the editor is on the player; a surprise while "
                           "it names a follower.");
            return g_hideLatched = false;
        }
        auto* root = ptr->Get3D();
        // ⚠ SPLIT FROM THE CULL TEST BELOW ON PURPOSE. They were one condition
        // and they mean opposite things: no 3D is "she is not built yet", culled
        // is "she is built and somebody hid her".
        if (!root) {
            NoteDecline(2, "the framed companion has no 3D yet.");
            return g_hideLatched = false;
        }
        if (root->GetAppCulled()) {
            NoteDecline(3, "the framed companion reads as CULLED at policy time. This is "
                           "OS-103's leading hypothesis: the actor sweep hides everyone "
                           "before Fitting Room names her, so a policy evaluation landing "
                           "in that window calls her off screen and stands the player up, "
                           "and her exemption un-culls her immediately after with the "
                           "decision already taken.");
            return g_hideLatched = false;
        }
        auto* cell = a_player->GetParentCell();
        if (!cell || ptr->GetParentCell() != cell) {
            NoteDecline(4, "the framed companion is in a different cell from the player.");
            return g_hideLatched = false;
        }
        const RE::NiPoint3 d   = ptr->GetPosition() - a_player->GetPosition();
        // auto: SeparationOf returns a CompanionShotPolicy::Separation, not a
        // float. InShot takes it by const reference.
        const auto sep = MTB::CompanionShotPolicy::SeparationOf(d.x, d.y, d.z);
        const bool in  = MTB::CompanionShotPolicy::InShot(sep, g_hideLatched);
        if (!in) {
            NoteDecline(5, "CompanionShotPolicy says she is not a second subject at this "
                           "separation, so standing the player down would leave an empty "
                           "frame.");
        } else {
            // Cleared on success so the NEXT decline logs even if it repeats the
            // previous reason. Without this a flap reads as a single event.
            g_lastDeclineCode = 0;
        }
        return g_hideLatched = in;
    }

    bool HideLoadedRef(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root) {
        if (!a_ref || !a_root || a_root->GetAppCulled()) {
            return false;
        }
        a_root->SetAppCulled(true);
        g_hidden.push_back(a_ref->GetHandle());
        return true;
    }

    bool HideRef(RE::TESObjectREFR* a_ref) {
        return HideLoadedRef(a_ref, a_ref ? a_ref->Get3D() : nullptr);
    }

    // OS-103(b) THE BUG, FOUND IN ROUND 5. The body other people see is NOT
    // Get3D().
    //
    // ⚠ FOR THE PLAYER, Get3D() IS THE FIRST PERSON SKELETON. The node census
    // caught all three of Get3D(), Get3D(true) and firstPerson3D returning one
    // pointer with the cull bit SET, while Get3D(false) returned a different
    // node with it clear and the full third person body on screen. Every round
    // of this bug culled the first person node, held it culled, and watched him
    // stand there, because Get3D() forwards to Get3D2() and PlayerCharacter
    // answers that with whichever body the camera is using.
    //
    // That also retires the "unwinnable per-frame war". The six engine writes a
    // frame that round 4 measured were the engine maintaining its own hidden
    // first person node, which is ordinary housekeeping. Nobody was fighting us.
    //
    // Named rather than inlined at the four call sites, so there is one place to
    // be right about which body this feature means.
    RE::NiAVObject* PlayerBody3D(RE::PlayerCharacter* a_player) {
        return a_player ? a_player->Get3D(false) : nullptr;
    }

    // ⚠ RECORDED AS A NODE, NOT A HANDLE, AND THAT IS THE WHOLE POINT.
    // HideLoadedRef pushes a handle into g_hidden, and RestoreAll re-derives the
    // root from it with ref->Get3D() - which for the player is the FIRST PERSON
    // node again. Riding that path would cull the third person body and then
    // un-cull the first person one, leaving an invisible player in a live save
    // on every menu exit. g_hiddenNodes restores the exact pointer it was given,
    // drains in the same RestoreAll on the same exits, and its NiPointer keeps
    // the node alive until it does.
    bool HidePlayerBody(RE::NiAVObject* a_body) {
        if (!a_body || a_body->GetAppCulled()) {
            return false;  // already down, and never un-hide what another mod hid
        }
        a_body->SetAppCulled(true);
        g_hiddenNodes.emplace_back(a_body);
        return true;
    }

    // Stand the player down so a framed companion has the shot to herself.
    //
    // ⚠ THIS GOES THROUGH HideLoadedRef ON PURPOSE, rather than culling his
    // root directly. That records his handle in g_hidden, which means the
    // release is the SAME proven path that already restores every other ref
    // this module hides, on every exit it already covers. A player left culled
    // in the live world is an invisible character in a running save, so the
    // release is the part that had to be borrowed rather than written.
    //
    // It does not touch the Cast. The four cull paths still spare his branch,
    // and sparing a branch does not un-cull a root, so his exemption staying
    // intact costs nothing here. Removing him from the Cast instead would let
    // the tree-position culls take a branch that holds BOTH of them, and take
    // her off screen with him.
    void ApplyPlayerHide(RE::PlayerCharacter* a_player) {
        if (MTB::Settings::GetSingleton().hidePlayerForCompanion &&
            CompanionIsOnScreen(a_player)) {
            // OS-103(b): raise the intent BEFORE the cull below, so the vfunc
            // hook is already answering "not visible" by the time the engine's
            // own pass reaches its decision this frame.
            g_playerHideWanted = true;
            if (auto* root = PlayerBody3D(a_player)) {
                HidePlayerBody(root);  // no-op if he is already culled
                // Read the flag BACK. If another mod culled him first we did
                // not hide him and must not claim to, but the rig still needs
                // to know he is not on screen.
                g_playerHidden = root->GetAppCulled();
                if (g_playerHidden) {
                    g_playerRootAtHide = root;  // identity only, never read
                    // OS-103(b) round 3: watch the four bytes, on the node we
                    // just culled. Re-arming on the same node is free.
                    MTB::CullWatch::Arm(root);
                }
                if (g_playerHidden && !g_playerHideLogged) {
                    g_playerHideLogged = true;
                    spdlog::info("declutter: the player is standing down while the "
                                 "framed companion is the subject "
                                 "(bHidePlayerForCompanion). The rig lights her alone.");
                }
                return;
            }
        }
        g_playerHideWanted = false;
        g_playerHidden     = false;
    }

    // AE-safe stand-in for RE::TES::ForEachReferenceInRange. CommonLib-NG's
    // version finishes by walking worldSpace->GetSkyCell() through a TES member
    // whose AE offset it flags as uncertain ("worldSpace // 140 - actual offset
    // change is somewhere near showLandBorder"). On AE exteriors that read comes
    // back as garbage (float/coordinate bytes) and GetSkyCell dereferences it,
    // so the game CTDs the instant the inventory opens outdoors (SE's layout is
    // correct, which is why SE never hit it). We reproduce the interior and
    // grid-cell sweep using only members that are stable across SE and AE
    // (interiorCell 0xC0, gridCells 0x78) and drop the sky-cell pass, which only
    // ever holds distant sky-dome art, never the local clutter this is about
    // (and outdoors the void keeps terrain and sky by design anyway).
    template <class Fn>
    void ForEachRefInRangeSafe(RE::TES* a_tes, RE::PlayerCharacter* a_origin,
                               float a_radius, Fn a_fn) {
        if (!a_tes || !a_origin || a_radius <= 0.0f) {
            return;
        }
        const auto originPos = a_origin->GetPosition();
        auto visit = [&](RE::TESObjectREFR& a_ref) -> RE::BSContainer::ForEachResult {
            return a_fn(a_ref);
        };
        if (a_tes->interiorCell) {
            a_tes->interiorCell->ForEachReferenceInRange(originPos, a_radius, visit);
            return;
        }
        auto* grid = a_tes->gridCells;
        const std::uint32_t gridLength = grid ? grid->length : 0;
        if (gridLength == 0) {
            return;
        }
        const float xPlus = originPos.x + a_radius;
        const float xMinus = originPos.x - a_radius;
        const float yPlus = originPos.y + a_radius;
        const float yMinus = originPos.y - a_radius;
        for (std::uint32_t x = 0; x < gridLength; ++x) {
            for (std::uint32_t y = 0; y < gridLength; ++y) {
                auto* cell = grid->GetCell(x, y);
                if (!cell || !cell->IsAttached()) {
                    continue;
                }
                const auto* coords = cell->GetCoordinates();
                if (!coords) {
                    continue;
                }
                const float wx = coords->worldX;
                const float wy = coords->worldY;
                if (wx < xPlus && (wx + 4096.0f) > xMinus &&
                    wy < yPlus && (wy + 4096.0f) > yMinus) {
                    cell->ForEachReferenceInRange(originPos, a_radius, visit);
                }
            }
        }
    }

    void HideNearbyActors(RE::PlayerCharacter* a_player, float a_radius) {
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return;
        }
        const auto playerPos = a_player->GetPosition();
        const Cast cast = ResolveCast(a_player);  // r52/r53 horse, plus the companion
        int hidden = 0;
        auto sweep = [&](RE::BSTArray<RE::ActorHandle>& a_handles) {
            for (auto& handle : a_handles) {
                auto actor = handle.get();
                if (!actor || cast.Has(actor.get())) {
                    continue;
                }
                if (actor->GetPosition().GetDistance(playerPos) > a_radius) {
                    continue;
                }
                if (HideRef(actor.get())) {
                    ++hidden;
                }
            }
        };
        sweep(lists->highActorHandles);
        sweep(lists->middleHighActorHandles);
        if (hidden > 0) {
            spdlog::debug("declutter: hid {} actor(s) within {:.0f} units.", hidden, a_radius);
        }
    }

    // Interiors, solo mode: nothing in the cell tree renders except the node
    // chains leading to the cast - including art that belongs to no reference
    // and shares the player's top-level branch (addon-node candle flames,
    // smoke, sparks: the field survivors of both the per-ref sweep and the
    // branch-level wholesale cull).
    //
    // F-30: computed from the CELL DOWN. The original climbed from the player
    // up, culling each level's siblings, which only took the cell down when
    // the chain happened to pass through the cell's big object list - and in
    // a room-bound interior the engine parents the player under the room's
    // portal geometry instead, so the climb culled the dozen things sharing
    // his room and left the rest of the inn rendering. Descending from cell3D
    // and culling every branch off the cast's keep path makes the tree shape
    // stop mattering. Per-node decision: PathCullPolicy.h.
    // F-30 instrument. See g_pathDumpLogged for why this exists.
    void LogPathOnce(RE::PlayerCharacter* a_player, RE::NiAVObject* a_root,
                     RE::NiAVObject* a_cell3D, const char* a_verdict) {
        if (g_pathDumpLogged) {
            return;
        }
        g_pathDumpLogged = true;

        // ⚠ THE FIRST-PERSON QUESTION, ASKED DIRECTLY. Get3D() on the player
        // hands back the FIRST-PERSON model while he is in first person, and
        // that model does not hang where the third-person one does. Naming all
        // three pointers means the answer is read rather than reasoned about.
        const auto* third = a_player->Get3D(false);
        const auto* first = a_player->Get3D(true);
        std::string chain;
        int         depth = 0;
        std::size_t siblingTotal = 0;
        for (auto* n = a_root; n && depth < 16; n = n->parent) {
            std::size_t kids = 0;
            if (auto* parent = n->parent ? n->parent->AsNode() : nullptr) {
                kids = parent->GetChildren().size();
            }
            const std::size_t sibs = kids ? kids - 1 : 0;
            siblingTotal += sibs;
            const char* nm = n->name.c_str();
            if (!chain.empty()) {
                chain += " < ";
            }
            chain += (nm && *nm) ? nm : "<unnamed>";
            chain += " (" + std::to_string(sibs) + ")";
            ++depth;
            if (n == a_cell3D || !n->parent) {
                break;
            }
        }
        spdlog::info("declutter path [{}]: start {} ({}) | third {} first {} | depth {} | {} "
                     "sibling(s) reachable | chain {}",
                     a_verdict, static_cast<const void*>(a_root),
                     a_root == first    ? "THE FIRST-PERSON MODEL"
                     : a_root == third  ? "the third-person model"
                                        : "NEITHER 1p NOR 3p",
                     static_cast<const void*>(third), static_cast<const void*>(first), depth,
                     siblingTotal, chain);
    }

    bool HideInteriorPathSiblings(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        if (!cell || !cell->IsInteriorCell()) {
            return false;
        }
        auto* loaded = cell->GetRuntimeData().loadedData;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        auto* playerRoot = a_player->Get3D();
        if (!cell3D || !playerRoot) {
            // ⚠ THESE EARLY-OUTS WERE SILENT, and a sweep that returns here
            // looks identical in the log to one that ran and found nothing.
            LogPathOnce(a_player, playerRoot, cell3D,
                        !cell3D ? "NO CELL 3D, walk never started"
                                : "NO PLAYER 3D, walk never started");
            return false;
        }

        // The player must actually live under this cell root.
        bool underCell = false;
        for (auto* n = playerRoot->parent; n; n = n->parent) {
            if (n == cell3D) {
                underCell = true;
                break;
            }
        }
        if (!underCell) {
            LogPathOnce(a_player, playerRoot, cell3D,
                        "PLAYER NOT UNDER THE CELL ROOT, walk refused");
            return false;
        }
        LogPathOnce(a_player, playerRoot, cell3D, "walking");

        const Cast cast = ResolveCast(a_player);

        // The keep-set: every node on a cast member's chain up to cell3D, and
        // separately the cast's own roots. The descent spares keep-set nodes
        // and recurses into them; everything else off the path is culled from
        // ABOVE, which is what reaches the cell's big object list even when no
        // cast chain passes through it. Chains are ~5 nodes and the cast is at
        // most two, so a vector beats a hash set here.
        //
        // Indoors a nearby actor's 3D root hangs on these exact chains, so
        // this walk would hide the framed companion even when both ref sweeps
        // have learned to skip her - which is why her root goes in as a keep
        // like the player's own. Of the four cull paths this is the one that
        // is easiest to miss, because it culls by tree position rather than by
        // anything about the reference.
        std::vector<const RE::NiAVObject*> keepRoots;
        std::vector<const RE::NiAVObject*> keepChain;
        const auto addChain = [&](RE::NiAVObject* a_root) {
            if (!a_root) {
                return;
            }
            // A cast member parented elsewhere (unloaded, mid-transition)
            // contributes nothing; the descent then simply never reaches her,
            // and the ref sweeps' exemptions still protect her everywhere else.
            bool reachesCell = false;
            for (auto* n = a_root; n; n = n->parent) {
                if (n == cell3D) {
                    reachesCell = true;
                    break;
                }
            }
            if (!reachesCell) {
                return;
            }
            keepRoots.push_back(a_root);
            for (auto* n = a_root; n && n != cell3D; n = n->parent) {
                keepChain.push_back(n);
            }
        };
        addChain(cast.playerRoot);
        addChain(cast.playerRoot1st);
        addChain(cast.companionRoot);

        const auto contains = [](const std::vector<const RE::NiAVObject*>& a_set,
                                 const RE::NiAVObject* a_node) {
            return std::find(a_set.begin(), a_set.end(), a_node) != a_set.end();
        };

        // ⚠⚠ F-30 round 2: A CULL MUST GO ALL THE WAY DOWN, not stop at the
        // branch root. Field 2026-08-20 04:33: the descent culled the same 13
        // containers the old climb culled, the flags held for the whole arm
        // (the 4-per-second re-cull logged nothing new for ten seconds), and
        // the inn rendered anyway. Meanwhile the healthy depth-4 arm culls the
        // object list's ~1740 children INDIVIDUALLY and works. So an ancestor
        // container's culled flag is not honoured by whatever renders a
        // room-bound interior (the portal graph reaches rooms directly), and
        // per-node culls are. On kCull we therefore keep descending with the
        // keep-set checks off, culling every descendant node individually -
        // the granularity the working arm proves.
        int hidden = 0;
        const auto descend = [&](auto&& a_self, RE::NiNode* a_node,
                                 bool a_offPath) -> void {
            for (auto& childPtr : a_node->GetChildren()) {
                auto* child = childPtr.get();
                if (!child) {
                    continue;
                }
                const char* nm = child->name.c_str();
                using MTB::PathCullPolicy::Action;
                switch (MTB::PathCullPolicy::Decide({
                    .isCastRoot     = !a_offPath && contains(keepRoots, child),
                    .isCastAncestor = !a_offPath && contains(keepChain, child),
                    .isOurs         = nm && std::strncmp(nm, "MTB_", 4) == 0,
                    .alreadyCulled  = child->GetAppCulled(),
                })) {
                case Action::kDescend:
                    if (auto* asNode = child->AsNode()) {
                        a_self(a_self, asNode, false);
                    }
                    break;
                case Action::kCull:
                    child->SetAppCulled(true);
                    g_hiddenNodes.emplace_back(child);
                    ++hidden;
                    if (auto* asNode = child->AsNode()) {
                        a_self(a_self, asNode, true);
                    }
                    break;
                case Action::kKeepWhole:
                case Action::kLeave:
                    // An already-culled subtree is left whole. Within one pass
                    // parents are culled before their children are visited, so
                    // our own deep cull is never cut short by its own flags: a
                    // child's flag is read before we set it.
                    break;
                }
            }
        };
        descend(descend, cell3D, false);

        // F-30 instrument, round 2: name what hangs off the cell root, once
        // per arm. RTTI is the interesting column - 'BSMultiBoundRoom' or a
        // portal node here would name the machinery that ignored the container
        // culls. Stays until F-30 is field-confirmed dead.
        if (!g_cellChildrenLogged) {
            g_cellChildrenLogged = true;
            std::size_t idx = 0;
            for (auto& childPtr : cell3D->GetChildren()) {
                if (auto* child = childPtr.get()) {
                    const auto* node = child->AsNode();
                    const char* nm = child->name.c_str();
                    spdlog::info("declutter cell child [{}]: rtti={} name='{}' "
                                 "kids={} culled={} onKeepPath={}",
                                 idx,
                                 child->GetRTTI() ? child->GetRTTI()->name : "?",
                                 (nm && *nm) ? nm : "<unnamed>",
                                 node ? node->GetChildren().size() : 0,
                                 child->GetAppCulled(),
                                 contains(keepChain, child) || contains(keepRoots, child));
                }
                ++idx;
            }
        }
        // Above the cell: the 2026-07-11 scene dump showed the survivors'
        // homes - UNNAMED siblings of cell3D under 'ObjectLODRoot' and two
        // more unnamed roots under 'shadow scene node' (global particle/FX
        // containers). Cull unnamed siblings for two levels above the cell;
        // named roots (Sky, Weather, LODRoot) stay, and the camera lives a
        // level higher still - never reached.
        RE::NiAVObject* up = cell3D;
        for (int lvl = 0; lvl < 2 && up->parent; ++lvl) {
            auto* parent = up->parent;
            for (auto& sibPtr : parent->GetChildren()) {
                auto* sib = sibPtr.get();
                if (!sib || sib == up || sib->GetAppCulled()) {
                    continue;
                }
                if (const char* nm = sib->name.c_str(); nm && *nm) {
                    continue;  // named engine roots untouched
                }
                sib->SetAppCulled(true);
                g_hiddenNodes.emplace_back(sib);
                ++hidden;
            }
            up = parent;
        }

        if (hidden > 0) {
            spdlog::debug("declutter[solo]: culled {} node(s) (branches off the "
                          "cast's path + unnamed FX roots above the cell).", hidden);
        }

        // F-30 round 3: park the portal graph's shared-geometry hook (see
        // g_parkedPortalGraph). Runs every Refresh as a RE-ASSERT, the sun
        // park's idiom: if the engine hands the graph a fresh pointer mid-arm,
        // the next sweep takes it down again and counts the re-park.
        if (auto* ssn = ShadowSceneAbove(cell3D)) {
            if (auto* graph = ssn->GetRuntimeData().portalGraph) {
                if (auto* shared = graph->portalSharedNode.get()) {
                    if (!g_parkedPortalGraph) {
                        g_parkedPortalGraph = graph;
                        g_parkedPortalNode  = graph->portalSharedNode;
                    } else {
                        ++g_portalReParks;
                    }
                    if (graph == g_parkedPortalGraph) {
                        graph->portalSharedNode = nullptr;
                        if (!g_portalParkLogged) {
                            g_portalParkLogged = true;
                            spdlog::info(
                                "declutter portal park: graph {} (cell {:08X}) "
                                "sharedNode '{}' ({} kids) -> parked | arrays "
                                "u40={} u58={} u78={} u90={}",
                                static_cast<const void*>(graph), graph->cellID,
                                shared->name.c_str() ? shared->name.c_str() : "<unnamed>",
                                shared->GetChildren().size(),
                                graph->unk40.size(), graph->unk58.size(),
                                graph->unk78.size(), graph->unk90.size());
                        }
                    } else {
                        spdlog::info("declutter portal park: a SECOND graph {} "
                                     "appeared mid-arm (parked one {}), left alone.",
                                     static_cast<const void*>(graph),
                                     static_cast<const void*>(g_parkedPortalGraph));
                    }
                }
            }
        }

        // F-30 round 4: pull the cast out of the parked subtree (see
        // g_reparented). Re-asserted every Refresh - the engine's room
        // tracking may hand a model back to the room node mid-arm, and the
        // paused world makes that cheap to undo again.
        if (auto* cellNode = cell3D->AsNode(); cellNode && g_parkedPortalNode) {
            const auto underParkedNode = [&](RE::NiAVObject* a_root) {
                for (auto* n = a_root->parent; n; n = n->parent) {
                    if (n == g_parkedPortalNode.get()) {
                        return true;
                    }
                }
                return false;
            };
            for (auto* rootConst : keepRoots) {
                auto* root = const_cast<RE::NiAVObject*>(rootConst);
                if (root->parent == cellNode || !underParkedNode(root)) {
                    continue;
                }
                auto* oldParent = root->parent;
                // The FIRST observed home is the record; a re-assert after the
                // engine re-filed him mid-arm must not overwrite it with the
                // room node it is pulling him back out of.
                const bool known = std::any_of(
                    g_reparented.begin(), g_reparented.end(),
                    [&](const ReparentedRoot& r) { return r.root.get() == root; });
                if (!known) {
                    g_reparented.push_back({ RE::NiPointer<RE::NiAVObject>(root),
                                             RE::NiPointer<RE::NiNode>(oldParent) });
                }
                ++g_castReParents;
                RE::NiPointer<RE::NiAVObject> keepAlive(root);
                oldParent->DetachChild(root);
                cellNode->AttachChild(root, true);
                spdlog::info("declutter portal park: cast root '{}' re-parented "
                             "cell-side (was under '{}').",
                             root->name.c_str() ? root->name.c_str() : "<unnamed>",
                             oldParent->name.c_str() ? oldParent->name.c_str()
                                                     : "<unnamed>");
            }
        }

        // F-30 round 7: the player pin (see g_playerPinApplied). AFTER the
        // descent and the reparent, BEFORE the probe, so the probe reads the
        // pinned state and a healthy arm logs culled=false either way.
        if (!g_playerHideWanted && cast.playerRoot &&
            cast.playerRoot->GetAppCulled()) {
            if (!g_playerPinApplied) {
                g_playerPinApplied = true;
                spdlog::info("declutter player pin: the 3p root was culled at "
                             "arm (first-person resting state), pinned "
                             "visible for the studio.");
            } else {
                ++g_playerPinReAsserts;
            }
            cast.playerRoot->SetAppCulled(false);
        }

        // F-30 round 8: kAlwaysDraw across the cast subtrees (see
        // g_alwaysDrawSet). Re-asserted per Refresh - a node the engine
        // rebuilds mid-arm (equip change) arrives without the bit and picks
        // it up on the next sweep.
        {
            std::size_t granted = 0;
            const auto grant = [&](auto&& a_self, RE::NiAVObject* a_obj) -> void {
                if (!a_obj) {
                    return;
                }
                using Flag = RE::NiAVObject::Flag;
                if (!a_obj->GetFlags().all(Flag::kAlwaysDraw)) {
                    a_obj->GetFlags().set(Flag::kAlwaysDraw);
                    g_alwaysDrawSet.emplace_back(a_obj);
                    ++granted;
                }
                if (auto* node = a_obj->AsNode()) {
                    for (auto& childPtr : node->GetChildren()) {
                        a_self(a_self, childPtr.get());
                    }
                }
            };
            grant(grant, cast.playerRoot);
            grant(grant, cast.playerRoot1st);
            grant(grant, cast.companionRoot);
            if (granted > 0 && !g_alwaysDrawLogged) {
                g_alwaysDrawLogged = true;
                spdlog::info("declutter always-draw: {} cast node(s) granted "
                             "kAlwaysDraw (occlusion bypass while armed).",
                             granted);
            }
        }

        // F-30 round 6: the cast probe (see g_castProbeCalls). Refresh runs
        // ~4x/s armed, so every 8th call is ~2s. Round 8 grew it per-part
        // eyes: the culled count over the whole subtree, with the first few
        // culled parts named, so a per-piece flag write can no longer hide
        // under a clean root.
        if (++g_castProbeCalls % 8 == 1) {
            const auto probe = [&](const char* a_tag, RE::NiAVObject* a_root) {
                if (!a_root) {
                    return;
                }
                std::size_t total = 0, culledParts = 0, drawables = 0, drawablesCulled = 0;
                std::string firstCulled;
                std::string drawn;
                const auto walk = [&](auto&& a_self, RE::NiAVObject* a_obj) -> void {
                    if (!a_obj) {
                        return;
                    }
                    ++total;
                    if (a_obj->GetAppCulled()) {
                        ++culledParts;
                        if (firstCulled.size() < 96) {
                            if (!firstCulled.empty()) {
                                firstCulled += ", ";
                            }
                            const char* nm = a_obj->name.c_str();
                            firstCulled += (nm && *nm) ? nm : "<unnamed>";
                        }
                    }
                    if (auto* node = a_obj->AsNode()) {
                        for (auto& childPtr : node->GetChildren()) {
                            a_self(a_self, childPtr.get());
                        }
                        return;
                    }
                    // ⚠⚠ A LEAF, NOT AsGeometry(). The node count alone cannot
                    // tell a bare skeleton from a dressed one, because a
                    // skeleton is hundreds of nodes on its own and the field
                    // question is "which PART is missing" (2026-08-27: a
                    // character invisible after a preset load, root NOT culled,
                    // 539 nodes for five minutes with nothing culled). Anything
                    // that draws is a leaf; AsGeometry() answers null on BS83
                    // shapes and would drop half a character in silence.
                    ++drawables;
                    if (a_obj->GetAppCulled()) {
                        ++drawablesCulled;
                    }
                    // 400, not 160: the first field round with this line cut
                    // off after thirteen names on a rig carrying fifty one, so
                    // it could say how MUCH draws and not WHAT.
                    if (drawn.size() < 400) {
                        if (!drawn.empty()) {
                            drawn += ", ";
                        }
                        const char* nm = a_obj->name.c_str();
                        drawn += (nm && *nm) ? nm : "<unnamed>";
                    }
                };
                walk(walk, a_root);
                const auto& w = a_root->world.translate;
                spdlog::info("declutter cast probe [{}]: '{}' culled={} | "
                             "subtree {} node(s), {} culled{}{} | "
                             "{} drawable(s), {} of them culled: {} | "
                             "world=({:.0f},{:.0f},{:.0f}) boundR={:.0f}",
                             a_tag,
                             a_root->name.c_str() ? a_root->name.c_str() : "<unnamed>",
                             a_root->GetAppCulled(), total, culledParts,
                             culledParts ? ": " : "",
                             culledParts ? firstCulled : "", drawables, drawablesCulled,
                             drawables ? drawn : std::string{ "nothing on this rig draws" },
                             w.x, w.y, w.z, a_root->worldBound.radius);
            };
            probe("3p", cast.playerRoot);
            probe("1p", cast.playerRoot1st);
            probe("comp", cast.companionRoot);
        }

        // One-shot structure dump kept for future diagnosis (now also shows
        // which of the culprits are marked culled).
        static bool dumpedOnce = false;
        if (!dumpedOnce) {
            dumpedOnce = true;
            int level = 0;
            for (RE::NiNode* walk = cell3D->parent; walk && level < 4; walk = walk->parent, ++level) {
                std::string line;
                for (auto& sib : walk->GetChildren()) {
                    if (auto* s = sib.get()) {
                        line += line.empty() ? "" : ", ";
                        const char* nm = s->name.c_str();
                        line += (nm && *nm) ? nm : "<unnamed>";
                        line += s->GetAppCulled() ? "(culled)" : "";
                        if (s == (level == 0 ? static_cast<RE::NiAVObject*>(cell3D) : nullptr)) {
                            line += "[cell3D]";
                        }
                    }
                }
                spdlog::debug("scene above cell3D, level {} '{}': [{}]", level,
                              walk->name.c_str() ? walk->name.c_str() : "<unnamed>", line);
            }
        }
        return true;
    }

    // r38 (field: "particles like fire embers" in the exterior void): the
    // unnamed FX containers live at the same levels above EXTERIOR cell
    // roots as the interior ones did (r13 scene dump lineage) - but the
    // above-the-cell cull only ever ran for interiors. Same rules: unnamed
    // siblings only (named engine roots - Sky, LODRoot, camera parents -
    // untouched), two levels, shared restore list. Plus the same one-shot
    // structure dump so the next leak names its own home.
    void HideExteriorAboveCell(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        if (!cell || cell->IsInteriorCell()) {
            return;
        }
        auto* loaded = cell->GetRuntimeData().loadedData;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        if (!cell3D) {
            return;
        }
        int hidden = 0;
        RE::NiAVObject* up = cell3D;
        for (int lvl = 0; lvl < 2 && up->parent; ++lvl) {
            auto* parent = up->parent;
            for (auto& sibPtr : parent->GetChildren()) {
                auto* sib = sibPtr.get();
                if (!sib || sib == up || sib->GetAppCulled()) {
                    continue;
                }
                if (const char* nm = sib->name.c_str(); nm && *nm) {
                    continue;  // named engine roots untouched
                }
                sib->SetAppCulled(true);
                g_hiddenNodes.emplace_back(sib);
                ++hidden;
            }
            up = parent;
        }
        if (hidden > 0) {
            spdlog::debug("declutter[solo]: culled {} unnamed root(s) above "
                          "the exterior cell.", hidden);
        }
        static bool dumpedOnceExt = false;
        if (!dumpedOnceExt) {
            dumpedOnceExt = true;
            int level = 0;
            for (RE::NiNode* walk = cell3D->parent; walk && level < 4;
                 walk = walk->parent, ++level) {
                std::string line;
                for (auto& sib : walk->GetChildren()) {
                    if (auto* s = sib.get()) {
                        line += line.empty() ? "" : ", ";
                        const char* nm = s->name.c_str();
                        line += (nm && *nm) ? nm : "<unnamed>";
                        line += s->GetAppCulled() ? "(culled)" : "";
                    }
                }
                spdlog::debug("scene above EXTERIOR cell3D, level {} '{}': [{}]",
                              level,
                              walk->name.c_str() ? walk->name.c_str() : "<unnamed>",
                              line);
            }
        }
    }

    // Solo mode: hide every loaded reference around the player except the
    // player, light sources (culling them would change how the player is
    // lit) and the furniture the player occupies (inventory can open while
    // sitting - the chair must not vanish under them).
    void HideEverythingElse(RE::PlayerCharacter* a_player, float a_radius) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        const auto occupied = a_player->GetOccupiedFurniture();
        const Cast cast = ResolveCast(a_player);  // r53: the horse stays, and the companion
        // Hoisted out of the lambda: this is read once per REF otherwise, and
        // the sweep visits every ref in the loaded grid.
        const bool hideLights = MTB::Settings::GetSingleton().hideLightRefs;
        int hidden = 0;
        ForEachRefInRangeSafe(
            tes, a_player, a_radius, [&](RE::TESObjectREFR& a_ref) {
                // Lifetime and ownership checks MUST precede Get3D. The 0.7.3
                // perf pass moved the scene lookup first, making every refresh
                // inspect the player and disabled refs that the old path
                // rejected before touching their 3D.
                const bool isPlayer = &a_ref == a_player;
                const bool isMount = &a_ref == cast.mount;
                const bool isCompanion = cast.companion && &a_ref == cast.companion;
                const bool exempt = isPlayer || isMount || isCompanion;
                const bool isOccupied = !exempt && a_ref.GetHandle() == occupied;
                const bool disabled = !exempt && !isOccupied && a_ref.IsDisabled();
                if (!MTB::DeclutterRefPolicy::ShouldInspect3D({
                        .isPlayer = isPlayer,
                        .isPlayerMount = isMount,
                        .isOccupiedFurniture = isOccupied,
                        .isDisabled = disabled,
                        .isFramedCompanion = isCompanion,
                    })) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* const root = a_ref.Get3D();
                if (!root || root->GetAppCulled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* base = a_ref.GetBaseObject();
                if (!base) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (base->GetFormType() == RE::FormType::Light && !hideLights) {
                    // Light refs carry flame/smoke/spark art. Hiding the mesh
                    // usually keeps illumination; bHideLightRefs=0 preserves
                    // the old skip if a setup disagrees and the player goes
                    // dark.
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (HideLoadedRef(&a_ref, root)) {
                    ++hidden;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        if (hidden > 0) {
            spdlog::debug("declutter[solo]: hid {} ref(s) within {:.0f} units.", hidden, a_radius);
        }
    }

    // r33 (user root-caused it in the field: "the extra lighting comes from
    // the other sources of light produced by the cell"): every sweep above
    // culls PARENT branches, and a parent-culled light keeps illuminating -
    // only a light whose OWN node is culled is skipped by the light
    // gathering (r8, proven both directions by the studio rig). So the void
    // was still lit by braziers/candles the player couldn't even see. Walk
    // the scene and self-cull every NiLight that isn't ours (MTB_ rig) or
    // on the player's branch (equipped torch/candlelight keeps lighting the
    // character); the shared restore list un-culls them on every exit.
    int CutLightsUnder(RE::NiAVObject* a_obj, const Cast& a_cast) {
        // Stop at any cast member's root: their equipped torch or Candlelight
        // keeps lighting them. Before the companion existed this was a single
        // pointer compare against the player, and leaving it that way would
        // have cut her torch while keeping ours.
        if (!a_obj || a_cast.HasNode(a_obj)) {
            return 0;
        }
        if (const char* nm = a_obj->name.c_str();
            nm && std::strncmp(nm, "MTB_", 4) == 0) {
            return 0;
        }
        if (auto* light = netimmerse_cast<RE::NiLight*>(a_obj)) {
            if (!light->GetAppCulled()) {
                light->SetAppCulled(true);
                g_hiddenNodes.emplace_back(light);
                return 1;
            }
            return 0;
        }
        int cut = 0;
        if (auto* node = a_obj->AsNode()) {
            for (auto& child : node->GetChildren()) {
                cut += CutLightsUnder(child.get(), a_cast);
            }
        }
        return cut;
    }

    void CutCellLightSources(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        auto* playerRoot = a_player->Get3D();
        if (!cell3D || !playerRoot) {
            return;
        }
        // Interiors: start two levels above the cell - the unnamed FX roots
        // up there (scene-dump finding) can hold light emitters too.
        // Exteriors: the player's cell root only (the sun/sky directional is
        // not a scene NiLight; StudioLight owns the ambient/directional).
        RE::NiAVObject* root = cell3D;
        if (cell->IsInteriorCell()) {
            for (int i = 0; i < 2 && root->parent; ++i) {
                root = root->parent;
            }
        }
        const Cast cast = ResolveCast(a_player);
        if (const int cut = CutLightsUnder(root, cast); cut > 0) {
            spdlog::debug("declutter[solo]: self-culled {} cell light source(s): "
                          "illumination off; the rig + studio light own the "
                          "character now.", cut);
        }
    }

    // ------------------------------------------------------------------
    // r37 F-20 VOID ENGINE. The RE verdict first (mtb_renderscenes.c /
    // mtb_menureplace.c / mtb_statsmenu2.c): the skills menu blanks the
    // world through IMenu flag kFreezeFrameBackground (1<<5, carried by the
    // TweenMenu under StatsMenu) - the render dispatcher (0x1405B1020)
    // renders the world ONCE more into a held frame, then only re-presents
    // that frozen image while the flag menu is stacked; the perk dome draws
    // through a separate menu-3D pipeline. That mechanism is binary: a
    // frozen world cannot contain OUR live, animating character, so the
    // flag is unusable here. What we adopt instead are the engine's own
    // WHOLESALE per-subsystem switches - each leak the field found gets
    // killed at its engine root, not per-ref:
    //   grass  = the grass scene ROOT node (BGSGrassManager+0x68); culling
    //            it is literally the engine's ToggleGrass ("tg") console
    //            implementation (decompile mtb_togglegrass2.c).
    //   land   = every attached cell's land quadrant meshes
    //            (cellLand->loadedData->mesh[0..3]) - terrain is not a
    //            reference, no ref sweep can reach it.
    //   LOD    = TES::lodLandRoot + TES::objLODWaterRoot (distant terrain /
    //            water impostors; the shell occludes most, these kill the
    //            rest).
    //   precip = Sky::precip current/last geometries - rain/snow already
    //            mid-air at arm stays frozen INSIDE the shell otherwise.
    // Everything lands in g_hiddenNodes, so the existing three-exit
    // restore covers it (SPEC §4 invariant 1).
    int CullNode(RE::NiAVObject* a_obj) {
        if (a_obj && !a_obj->GetAppCulled()) {
            a_obj->SetAppCulled(true);
            g_hiddenNodes.emplace_back(a_obj);
            return 1;
        }
        return 0;
    }

    RE::NiAVObject* GrassSceneRoot() {
        auto* mgr = RE::BGSGrassManager::GetSingleton();
        if (!mgr) {
            return nullptr;
        }
        // +0x68 = the grass scene root. Engine evidence: the ToggleGrass
        // handler (1.5.97 @ 0x140313600) flips exactly this node's AppCull
        // bit. Not modeled in CommonLib - raw offset, SE-only like the rest
        // of Offsets.h.
        return *reinterpret_cast<RE::NiAVObject**>(
            reinterpret_cast<std::uintptr_t>(mgr) + 0x68);
    }

    void CullLandOfCell(RE::TESObjectCELL* a_cell, int& a_quads) {
        if (!a_cell) {
            return;
        }
        auto* land = a_cell->GetRuntimeData().cellLand;
        auto* data = land ? land->loadedData : nullptr;
        if (!data) {
            return;
        }
        for (auto* mesh : data->mesh) {
            a_quads += CullNode(mesh);
        }
    }

    void CullWorldFeeders(RE::PlayerCharacter* a_player) {
        const int grass = CullNode(GrassSceneRoot());
        int quads = 0;
        int lod = 0;
        if (auto* tes = RE::TES::GetSingleton()) {
            CullLandOfCell(a_player->GetParentCell(), quads);
            if (auto* grid = tes->gridCells) {
                for (std::uint32_t x = 0; x < grid->length; ++x) {
                    for (std::uint32_t y = 0; y < grid->length; ++y) {
                        CullLandOfCell(grid->GetCell(x, y), quads);
                    }
                }
            }
            lod += CullNode(tes->lodLandRoot);
            lod += CullNode(tes->objLODWaterRoot);
            // r41 - the r38 exterior dump settled the mountain question:
            // TES::objRoot is the node NAMED 'ObjectLODRoot' and it is the
            // GRID CELL CONTAINER (the player's ancestor on every arm -
            // the r38 guard refused it 38/38 times, misleading CommonLib
            // field name and all). The distant-LOD content - object LOD
            // (the mountain silhouettes) and tree LOD - lives under the
            // NAMED 'LODRoot' sibling beneath the shadow scene node, which
            // the unnamed-sibling cull deliberately skips.
            //
            // r42 - 'Sky' and 'Weather' join the named culls, and THIS is
            // the real void switch (field: clouds + sky visible in the
            // armed void with ENB off): Sky::mode is consumed by
            // Sky::Update, WHICH NEVER RUNS WHILE THE MENU IS PAUSED - the
            // mode park was visually inert in every armed menu, and each
            // dark night sky spent five rounds masquerading as our void.
            // Culling the sky's scene BRANCH is render-side and immediate,
            // pause or not; the mode park stays for its real consumers
            // (fog ingest, and the r40 close-edge audio flow is untouched).
            for (RE::NiNode* up = a_player->Get3D() ? a_player->Get3D()->parent : nullptr;
                 up; up = up->parent) {
                for (auto& childPtr : up->GetChildren()) {
                    auto* child = childPtr.get();
                    if (child && (child->name == "LODRoot" ||
                                  child->name == "Sky" ||
                                  child->name == "Weather")) {
                        lod += CullNode(child);
                    }
                }
            }
        }
        // r45 (field: "some circle at our feet … where the voidsphere and
        // floor interact?" - exactly right): a WATER PLANE slicing the
        // shell sphere is a circle at foot height. Water was the one
        // feeder r37 consciously skipped; the live water meshes hang off
        // TESWaterSystem's object list (LOD water was already culled).
        int water = 0;
        if (auto* ws = RE::TESWaterSystem::GetSingleton()) {
            for (auto& obj : ws->waterObjects) {
                if (obj) {
                    water += CullNode(obj->shape.get());
                }
            }
        }
        if (water > 0) {
            spdlog::debug("void engine: {} water shape(s) culled.", water);
        }
        int precip = 0;
        if (auto* sky = RE::Sky::GetSingleton(); sky && sky->precip) {
            precip += CullNode(sky->precip->currentPrecip.get());
            precip += CullNode(sky->precip->lastPrecip.get());
        }
        spdlog::info("void engine: grass root {} | {} land quad(s) | {} LOD "
                     "root(s) | {} precip geom(s) culled.",
                     grass ? "culled" : "absent/off", quads, lod, precip);
    }

    // r37 F-22: a combat arm can catch a magic-projectile imagespace
    // modifier mid-ramp - the pause freezes its interpolators and the blur
    // sits over the studio for the whole menu. The engine keeps the live
    // instances in TES::activeImageSpaceModifiers; zeroing each instance's
    // strength removes its contribution, the saved value returns on exit.
    // If the field still shows blur with this log line present, the applied
    // state is baked elsewhere (ImageSpaceManager accumulation) and the
    // next lever is forcing a manager rebuild - evidence first.
    void NeutralizeActiveImods() {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        int n = 0;
        int seen = 0;
        for (auto& inst : tes->activeImageSpaceModifiers) {
            auto* raw = inst.get();
            if (!raw) {
                continue;
            }
            ++seen;
            // The note above called this exactly: "if the field still shows
            // blur with this log line present, the applied state is baked
            // elsewhere". It does (2026-07-18, Community Shaders, blur on
            // ContainerMenu) - so name every live instance before deciding
            // anything, because a COUNT cannot tell us which one survives.
            // Two things separate the candidates. The concrete type: a DOF
            // or Temp instance is not form-backed (IsForm() is null on
            // those), so a depth-of-field blur shows up here with no form id
            // at all, which is itself the answer to "vanilla imod or
            // something else". And the source form id when there is one:
            // 000434BB is the vanilla menu blur that OwnView already parks,
            // so seeing it here would mean that park is not holding.
            // No RTTI name here: this CommonLib build inherits NiObject
            // PRIVATELY on ImageSpaceModifierInstance, so GetRTTI() will not
            // compile through it. IsForm() is the public discriminator and it
            // carries the signal that matters anyway - it returns null for a
            // NON form-backed instance (DOF or Temp), and a depth-of-field
            // instance is exactly the shape a "blur" report points at.
            RE::FormID srcID = 0;
            bool       formBacked = false;
            if (auto* asForm = raw->IsForm()) {
                formBacked = true;
                if (asForm->imod) {
                    srcID = asForm->imod->GetFormID();
                }
            }
            const bool live = raw->strength != 0.0f;
            // debug, not info: the blur this was cut for turned out to be a
            // vanilla effect (field 2026-07-18), so this is now standing
            // instrumentation for the NEXT imod question rather than an open
            // investigation, and it must not cost every user two log lines per
            // menu open. bVerboseLog brings it back.
            spdlog::debug("void engine: imod #{} {} imod={:08X} "
                          "strength={:.3f} age={:.2f} -> {}",
                          seen, formBacked ? "form-backed" : "NON-form (DOF/Temp)",
                          srcID, raw->strength, raw->age,
                          live ? "ZEROED" : "left (already 0)");
            if (live) {
                g_imodSaves.push_back({ raw, raw->strength });
                raw->strength = 0.0f;
                ++n;
            }
        }
        if (seen > 0) {
            spdlog::info("void engine: zeroed {} of {} active imod instance(s) "
                         "(F-22 blur), strengths restore on exit.", n, seen);
        }
    }

    // Scene view (mode 1): the room stays; clear out furniture so the player
    // reads against the environment. Occupied furniture stays (menus open
    // while sitting), light refs stay (the cell keeps its own look).
    void HideSceneFurniture(RE::PlayerCharacter* a_player, float a_radius) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        const auto occupied = a_player->GetOccupiedFurniture();
        int hidden = 0;
        ForEachRefInRangeSafe(
            tes, a_player, a_radius, [&](RE::TESObjectREFR& a_ref) {
                if (&a_ref == a_player || a_ref.GetHandle() == occupied || a_ref.IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* base = a_ref.GetBaseObject();
                if (base && base->GetFormType() == RE::FormType::Furniture && HideRef(&a_ref)) {
                    ++hidden;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        if (hidden > 0) {
            spdlog::debug("declutter[scene]: hid {} furniture within {:.0f} units.",
                          hidden, a_radius);
        }
    }

}

namespace MTB::Declutter {
    void SetFramedCompanion(RE::ActorHandle a_actor) {
        if (g_companion == a_actor) {
            return;
        }
        g_companion = a_actor;
        g_companionSwept = false;
        g_castDirty = true;
        if (const auto ptr = g_companion.get()) {
            spdlog::info("declutter: framing companion '{}' 0x{:08X}: she is now "
                         "exempt from the actor sweep, the ref sweep, the interior "
                         "path-sibling cull and the light cut.",
                         ptr->GetName(), ptr->GetFormID());
        } else {
            spdlog::info("declutter: framed companion cleared.");
        }
    }

    RE::ActorHandle FramedCompanion() { return g_companion; }

    bool FramedCompanionTookEffect() { return g_companionSwept; }

    bool PlayerHideIntent() { return g_playerHideWanted; }

    bool PlayerHiddenForCompanion() { return g_playerHidden; }

    bool CouldFrameCompanion(RE::Actor* a_actor) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_actor || !player || a_actor == player) {
            return false;
        }
        // Built, not merely alive. An actor with no 3D has nothing to put in
        // the shot, which is the same test the framing itself makes.
        if (!a_actor->Get3D()) {
            return false;
        }
        auto* cell = player->GetParentCell();
        if (!cell || a_actor->GetParentCell() != cell) {
            return false;
        }
        // ⚠ DELIBERATELY NOT GetAppCulled. While a bubbled menu is open our own
        // sweep has already hidden everyone who is not the current companion,
        // so a cull check here would report every candidate as unframeable and
        // the answer would be "nobody", always. What is being asked is whether
        // she COULD be framed, and being hidden by us is not an obstacle to
        // that - naming her is exactly what un-hides her.
        const RE::NiPoint3 d = a_actor->GetPosition() - player->GetPosition();
        return MTB::CompanionShotPolicy::InShot(
            MTB::CompanionShotPolicy::SeparationOf(d.x, d.y, d.z), false);
    }

    void Refresh() {
        const auto& cfg = Settings::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || cfg.declutterMode == 0) {
            return;
        }
        // A mid-menu toggle needs the same restore-then-resweep a cast change
        // gets. Refresh only ever HIDES, so switching the setting off would
        // otherwise leave the player culled for the rest of the arm.
        if (g_lastPlayerHideSetting != cfg.hidePlayerForCompanion) {
            g_lastPlayerHideSetting = cfg.hidePlayerForCompanion;
            g_castDirty = true;
        }
        if (g_castDirty) {
            g_castDirty = false;
            // Everything comes back, then the sweeps below re-hide against the
            // new cast. Anyone who just left the cast is re-hidden in the same
            // pass, so this handles a swap as well as an addition.
            RestoreAll();
        }
        g_companionSwept = static_cast<bool>(g_companion.get());
        // Before the sweeps, so the shot is decided once and every path below
        // sees the same answer. Ordering is not load-bearing (the sweeps exempt
        // him either way), but reading his visibility mid-sweep would be.
        ApplyPlayerHide(player);
        if (cfg.IsVoidFamily()) {
            // r37: with the void engine on, the ref sweeps reach further -
            // the 'Editor Smoke Test Cell' arm (r33 field) had architecture
            // beyond the 4096-unit radius that no cull could touch. 16384
            // covers a full loaded exterior grid quadrant; one-shot cost,
            // paused world.
            const float radius = cfg.voidEngine
                ? std::max(cfg.soloHideRadius, 16384.0f)
                : cfg.soloHideRadius;
            // Void (2) and dressing room (3) share the solo sweep.
            // Actor sweep first: process lists cover actors whose 3D lives
            // outside the ref-in-range walk (mounts, summons mid-transition).
            HideNearbyActors(player, radius);
            // Interiors: path-sibling cull - only the player's node chain
            // renders, so ref-less emitters can't survive anywhere in the
            // cell tree. The ref sweep still runs as a safety net (and is
            // what exteriors rely on).
            HideInteriorPathSiblings(player);
            // r38: exteriors get the above-the-cell FX-root cull too (the
            // ember particles the interior version killed in r13).
            if (cfg.voidEngine) {
                HideExteriorAboveCell(player);
            }
            HideEverythingElse(player, radius);
            // r33: cut the cell's light SOURCES (illumination, not art) so
            // the character is lit by the studio alone. Once per arm - the
            // paused world can't spawn new lights mid-menu.
            if (cfg.cutCellLights && !g_cellLightsCut) {
                g_cellLightsCut = true;
                CutCellLightSources(player);
            }
            // r37 F-20/F-22: the world's non-ref feeders (grass, land, LOD,
            // precipitation) + frozen imod blur - once per arm.
            if (cfg.voidEngine && !g_worldFeedersCut) {
                g_worldFeedersCut = true;
                CullWorldFeeders(player);
                NeutralizeActiveImods();
            }
            // ⚠ ONE INFO LINE PER ARM, AND IT EXISTS BECAUSE THE FIELD KEEPS
            // REPORTING GEOMETRY THAT SURVIVED (2026-08-16: "entering the
            // inventory facing a wall leaves geometry unculled"). Every number
            // that would settle such a report is already measured and every one
            // of them goes to spdlog::debug, so a normal user's log says
            // nothing at all about the sweep that ran. Which cell kind, which
            // radius and how much was actually taken down separates "the sweep
            // never ran" from "the sweep ran and this piece is out of its
            // reach", and those have completely different fixes.
            //
            // Once per arm rather than per Refresh: Refresh is idempotent and
            // re-runs on every mode change and cast change, and a line per call
            // would be noise in the one place someone reads under pressure.
            if (!g_armSummaryLogged) {
                g_armSummaryLogged = true;
                auto* const cell = player->GetParentCell();
                spdlog::info("declutter: arm summary: mode {}, {}, radius {:.0f}, "
                             "{} ref(s) and {} node(s) held down.",
                             cfg.declutterMode,
                             cell ? (cell->IsInteriorCell() ? "interior" : "exterior")
                                  : "no cell",
                             radius, g_hidden.size(), g_hiddenNodes.size());
            }
            return;
        }
        // Scene view (mode 1): environment visible, no lighting override -
        // only actors and furniture leave the set. (The old corridor-only
        // mode retired in r16 - "less is more".)
        HideNearbyActors(player, cfg.soloHideRadius);
        HideSceneFurniture(player, cfg.soloHideRadius);
    }

    // OS-103 round 2. PIN THE PLAYER BACK DOWN EVERY ARMED TICK.
    //
    // ⚠ THE HIDE WAS NEVER THE PROBLEM; STAYING HIDDEN WAS. ApplyPlayerHide
    // culls him once per Refresh and reads the flag back, so the success line
    // it logs is TRUE at the instant it is written - and the field kept showing
    // him anyway, filling the frame in front of the follower. A cull that is
    // correct when set and wrong a frame later is not a decision problem, it is
    // a re-assert problem, and this module already had the shape of the answer
    // one function down: "Sky::Update un-culls its branch each frame; pin the
    // world-feeder culls back down".
    //
    // Show Player In Inventory exists to RENDER the player in this exact menu,
    // so something putting him back is the expected state of the world rather
    // than a surprise. Fitting Room's CameraFrame already re-asserts
    // cameraTarget on a cadence for the same reason and names the same mod.
    //
    // ⚠ SAFE TO CALL EVERY TICK. HideLoadedRef early-outs on an already-culled
    // root, so the steady state costs one flag read and pushes nothing; only a
    // root somebody actually un-culled is re-hidden and re-recorded, and the
    // restore un-culls him either way.
    void ReassertPlayerHide() {
        if (!g_playerHidden) {
            return;  // we never took him off screen, so there is nothing to hold
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const root   = PlayerBody3D(player);
        if (!root || root->GetAppCulled()) {
            return;  // still down, which is the whole point
        }
        // He came BACK. Name it once: this is the difference between "we never
        // hid him" and "we hid him and lost him", and those have completely
        // different fixes.
        ++g_playerReHides;
        const bool swapped = (g_playerRootAtHide && root != g_playerRootAtHide);
        if (swapped) {
            ++g_playerRootSwaps;
        }
        if (!g_playerReHideLogged) {
            g_playerReHideLogged = true;
            spdlog::info("declutter: the player was un-culled after we stood him down. "
                         "Root at hide {}, root now {}: {} (OS-103). Re-asserting every "
                         "armed tick from here; the per-arm tally lands at restore.",
                         g_playerRootAtHide, static_cast<const void*>(root),
                         swapped ? "REBUILT, so our cull went with the old node"
                                 : "the SAME node, so somebody is clearing the flag");
        }
        HidePlayerBody(root);
        g_playerRootAtHide = root;
    }

    // The tally, in one place because two callers now need it: the cadence
    // inside the probe and the arm summary at restore. Counters are NOT reset
    // here, so the cadence lines read as a running total and the last one
    // before a kill is still a complete measurement.
    void ReportCullProbe(const char* a_why) {
        for (std::size_t i = 0; i < kCullPhaseCount; ++i) {
            if (g_probeGapSamples[i] == 0) {
                continue;
            }
            const auto prev = (i + kCullPhaseCount - 1) % kCullPhaseCount;
            spdlog::info("declutter/probe [{}]: gap {} -> {}: cleared {} of {} sample(s) "
                         "(OS-103b).",
                         a_why, kCullPhaseNames[prev], kCullPhaseNames[i],
                         g_probeGapClears[i], g_probeGapSamples[i]);
        }
        // The suspect call, on its own line because it is a bracket rather than
        // a gap. A high ratio here NAMES Update3DPosition as the writer; zero
        // calls means SPII is not even reaching it while the editor holds her,
        // which is itself an answer.
        spdlog::info("declutter/probe [{}]: Update3DPosition cleared the cull {} of {} "
                     "bracketed call(s) (OS-103b).",
                     a_why, g_u3dCleared, g_u3dCalls);
    }

    // OS-103(b) round 5. WHICH NODE IS HE ON SCREEN THROUGH?
    //
    // ⚠ THE FLAG WE HAVE BEEN FIGHTING OVER IS NOT THE ONE. Round 4 held the
    // app-cull bit on Get3D() set at every frame boundary and at all 240000
    // phase samples, and he was visible for the whole arm. Four rounds went
    // into winning a fight over four bytes that do not govern what is drawn, so
    // before anything else gets hooked, count the nodes and read the bit off
    // each one. The node whose bit is CLEAR is the one to cull.
    //
    // The leading suspect is our own fix. Is3rdPersonVisibleHook answers false
    // while the intent holds, and the engine's first and third person paths are
    // a mirror: telling it the third person body is not visible is also telling
    // it the first person one should be. If that is what happened, firstPerson3D
    // comes back un-culled here and the fix moved the body rather than hiding
    // it.
    void ReportNodeCensus(const char* a_why) {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        // ⚠ NAMED SEPARATELY EVEN WHERE THEY SHOULD MATCH. Get3D() forwards to
        // Get3D2(), which is the vfunc the engine's own show site called, so
        // those two agreeing is the expected reading and their DISAGREEING
        // would itself be the answer. Printed rather than assumed.
        const auto say = [&](const char* a_what, RE::NiAVObject* a_node) {
            if (!a_node) {
                spdlog::info("declutter/nodes [{}]: {} is null (OS-103b).", a_why, a_what);
                return;
            }
            spdlog::info("declutter/nodes [{}]: {} is {}, flags {:#010x}, so he is {} "
                         "through it (OS-103b).",
                         a_why, a_what, static_cast<const void*>(a_node),
                         a_node->GetFlags().underlying(),
                         a_node->GetAppCulled() ? "CULLED" : "ON SCREEN");
        };
        say("Get3D()", player->Get3D());
        say("Get3D(first=true)", player->Get3D(true));
        say("Get3D(first=false)", player->Get3D(false));
        say("firstPerson3D", player->GetInfoRuntimeData().firstPerson3D.get());
        spdlog::info("declutter/nodes [{}]: Is3rdPersonVisible() answers {} and our hide "
                     "intent is {}. A false there with a live third person body is the fix "
                     "backfiring rather than the engine disagreeing (OS-103b).",
                     a_why, player->Is3rdPersonVisible(), PlayerHideIntent());
    }

    void ProbeUpdate3DBracketImpl(bool a_before) {
        if (!Settings::GetSingleton().playerCullProbe || !g_playerHidden) {
            return;
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const root   = PlayerBody3D(player);
        if (!root) {
            g_u3dArmed = false;
            return;
        }
        if (a_before) {
            g_u3dCulledBefore = root->GetAppCulled();
            g_u3dArmed        = true;
            return;
        }
        if (!g_u3dArmed) {
            return;  // the 'after' half without its 'before', so it counts nothing
        }
        g_u3dArmed = false;
        ++g_u3dCalls;
        if (g_u3dCulledBefore && !root->GetAppCulled()) {
            ++g_u3dCleared;
        }
    }

    // OS-103(b). Sample the cull flag at one fixed point in the frame and tally
    // which GAP it died in. Header carries why this exists and why the previous
    // number did not answer it.
    void ProbeUpdate3DBracket(bool a_before) { ProbeUpdate3DBracketImpl(a_before); }

    void ProbePlayerCull(CullPhase a_phase) {
        if (!Settings::GetSingleton().playerCullProbe || !g_playerHidden) {
            return;  // off, or we never stood him down, so there is no subject
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const root   = PlayerBody3D(player);
        if (!root) {
            return;
        }
        const auto idx    = static_cast<std::size_t>(a_phase);
        const bool culled = root->GetAppCulled();
        ++g_probeGapSamples[idx];
        // The transition is the measurement: culled at the previous sample,
        // un-culled at this one, so the writer ran in between. A gap that never
        // scores is a gap the writer is not in.
        if (g_probeHaveLast && g_probeLastCulled && !culled) {
            ++g_probeGapClears[idx];
        }
        g_probeLastCulled = culled;
        g_probeHaveLast   = true;
        g_probeLastPhase  = idx;
        // Once per frame, and BEFORE the cadence check so the stamp a trip
        // carries is the frame it happened in rather than the one before it.
        if (a_phase == CullPhase::kWrap) {
            CullWatch::Tick();
        }
        if (a_phase == CullPhase::kWrap && ++g_probeSinceReport >= kProbeReportEvery) {
            g_probeSinceReport = 0;
            ReportCullProbe("running");
            ReportNodeCensus("running");
            CullWatch::Report("running");
        }
        // ⚠ THE PER-FRAME RE-SET IS GONE (OS-103b). It existed only to give
        // each frame a fresh subject while we were hunting the writer, and the
        // hunt is over: the engine recomputes the cull from Is3rdPersonVisible,
        // which the vfunc hook now answers. With the fix working the flag stops
        // being cleared at all, so the healthy reading is 0 clears out of N
        // samples. A broken fix shows as ONE clear and then silence, which is
        // still unmistakable against the 24105 of 24105 this started at.
        if (false) {
            // Re-set, so the next frame has something to lose again.
            //
            // ⚠ SetAppCulled DIRECTLY rather than through HideLoadedRef, and
            // the difference matters at this rate. HideLoadedRef records the
            // handle in g_hidden on every cull it performs, and here it would
            // perform one EVERY FRAME, so a two-minute arm would push ~20000
            // duplicate handles. His handle is already in g_hidden from
            // ApplyPlayerHide, so the restore already covers him and the
            // bookkeeping would be pure waste.
            root->SetAppCulled(true);
            g_probeLastCulled = root->GetAppCulled();
        }
    }

    void ReassertWorldFeederCulls() {
        // The armed menu is paused, so the culls hold by themselves; this
        // exists for the UNPAUSED window between a close and the at-black
        // teardown (switch gaps, exit hold + dip): Sky::Update ticks there
        // with the true mode (r40) and un-culls its own branch each frame -
        // the skybox flashed over the studio mid-transition (field r43).
        // Same named walk as the arm-time cull; CullNode skips anything
        // still culled, so steady-state cost is a handful of name checks.
        if (!g_worldFeedersCut || !Settings::GetSingleton().voidEngine) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player ? player->Get3D() : nullptr;
        if (!root) {
            return;
        }
        for (RE::NiNode* up = root->parent; up; up = up->parent) {
            for (auto& childPtr : up->GetChildren()) {
                auto* child = childPtr.get();
                if (child && (child->name == "LODRoot" ||
                              child->name == "Sky" ||
                              child->name == "Weather")) {
                    CullNode(child);
                }
            }
        }
    }

    void RestoreAll() {
        // ⚠ INTENT DOWN BEFORE ANYTHING IS UN-CULLED (OS-103b). The engine
        // recomputes his cull every frame from Is3rdPersonVisible, so clearing
        // the flag below while the hook still answers "not visible" would just
        // have the engine put it straight back on the next pass. Dropping the
        // intent first also makes the restore self-healing: even if our own
        // un-cull missed him, the engine's next pass shows him again.
        g_playerHideWanted = false;
        // The player comes back with everything else: his handle is in
        // g_hidden like any other ref, so the loop below un-culls him. Clearing
        // the flag here rather than at the cull site keeps it honest across the
        // paths that restore without a Refresh following.
        // OS-103: the whole measurement in one line, per arm. A rate near the
        // tick count means a per-frame fight; a handful means occasional
        // rebuilds; zero re-hides with him still visible would mean the cull
        // was never the mechanism and this approach is finished.
        if (g_playerReHides > 0) {
            spdlog::info("declutter: player-hide held {} time(s) this arm, {} of them "
                         "after a ROOT REBUILD. {} (OS-103).",
                         g_playerReHides, g_playerRootSwaps,
                         g_playerRootSwaps == g_playerReHides
                             ? "Every one was a rebuild: Update3DPosition is the writer"
                         : g_playerRootSwaps == 0
                             ? "None were rebuilds: the flag is being cleared on a live node"
                             : "Mixed, so both mechanisms are in play");
        }
        // OS-103(b): the phase tally, one line per gap, per arm. Read it as
        // "the flag died between the previous phase and this one". The
        // pre-dispatch -> post-dispatch row is the decisive one: it brackets
        // the engine's own player dispatch, because PlayerDispatchHook calls
        // that before our OnFrame. A score there implicates the engine path; a
        // score only on the wrap -> pre-dispatch row means the writer is
        // outside our frame entirely and is somebody else's hook.
        if (Settings::GetSingleton().playerCullProbe) {
            ReportCullProbe("arm end");
            CullWatch::Report("arm end");
        }
        // ⚠ ALWAYS, not just when the probe is on: the setting can be
        // toggled mid-session and a watch left armed on a node about to be
        // freed is the one way this probe can hurt anything. Disarm is a
        // no-op when nothing is armed.
        CullWatch::Disarm();
        for (std::size_t i = 0; i < kCullPhaseCount; ++i) {
            g_probeGapClears[i]  = 0;
            g_probeGapSamples[i] = 0;
        }
        g_probeHaveLast   = false;
        g_probeLastCulled = false;
        g_probeLastPhase  = 0;
        g_probeSinceReport = 0;
        g_u3dCalls        = 0;
        g_u3dCleared      = 0;
        g_u3dArmed        = false;
        g_u3dCulledBefore = false;
        g_playerHidden = false;
        g_playerHideLogged = false;
        g_playerReHideLogged = false;
        g_playerRootAtHide = nullptr;
        g_playerReHides = 0;
        g_playerRootSwaps = 0;
        // OS-103: reset with its sibling, so each menu open reports its own
        // decline reason rather than inheriting the last menu's.
        g_lastDeclineCode = 0;
        // r37 F-22: give the imod instances their strengths back first -
        // independent of the cull lists, cheap, and a stray zero must never
        // outlive the menu. Raw saved pointers are only dereferenced when
        // the instance is STILL in the engine's live list (quickload-safe).
        if (!g_imodSaves.empty()) {
            int back = 0;
            if (auto* tes = RE::TES::GetSingleton()) {
                for (auto& live : tes->activeImageSpaceModifiers) {
                    auto* raw = live.get();
                    if (!raw) {
                        continue;
                    }
                    for (auto& save : g_imodSaves) {
                        if (save.inst == raw) {
                            raw->strength = save.strength;
                            ++back;
                            break;
                        }
                    }
                }
            }
            spdlog::debug("void engine: restored {}/{} imod strength(s).",
                          back, g_imodSaves.size());
            g_imodSaves.clear();
        }
        g_worldFeedersCut = false;
        g_armSummaryLogged = false;
        g_pathDumpLogged = false;
        g_cellChildrenLogged = false;
        // F-30 round 3: hand the portal graph its shared node back. The graph
        // pointer is identity only - it is dereferenced ONLY when the live
        // graph above the player's current cell is still the one we parked.
        // If the cell (and its graph) died before this ran, the engine rebuilt
        // both from scratch and there is nothing to hand back; the NiPointer
        // just lets go.
        if (g_parkedPortalNode) {
            RE::NiAVObject* cell3D = nullptr;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* cell = player->GetParentCell()) {
                    if (auto* loaded = cell->GetRuntimeData().loadedData) {
                        cell3D = loaded->cell3D.get();
                    }
                }
            }
            auto* ssn   = ShadowSceneAbove(cell3D);
            auto* graph = ssn ? ssn->GetRuntimeData().portalGraph : nullptr;
            if (graph && graph == g_parkedPortalGraph) {
                graph->portalSharedNode = g_parkedPortalNode;
                spdlog::info("declutter portal park: released, sharedNode "
                             "handed back to graph {} ({} re-park(s) this arm).",
                             static_cast<const void*>(graph), g_portalReParks);
            } else {
                spdlog::info("declutter portal park: parked graph {} is gone "
                             "(live graph {}), nothing to hand back.",
                             static_cast<const void*>(g_parkedPortalGraph),
                             static_cast<const void*>(graph));
            }
        }
        // F-30 round 4: hand every re-parented cast root back to the node the
        // engine had it under. Both ends are NiPointer-held, so a model whose
        // old parent died with the cell simply stays where the traversal can
        // draw it; the engine's own room tracking re-files it on the next
        // unpaused frame either way.
        for (auto& moved : g_reparented) {
            auto* root = moved.root.get();
            auto* home = moved.oldParent.get();
            if (!root || !home || root->parent == home) {
                continue;
            }
            if (auto* parent = root->parent) {
                RE::NiPointer<RE::NiAVObject> keepAlive(root);
                parent->DetachChild(root);
                home->AttachChild(root, true);
            }
        }
        if (!g_reparented.empty()) {
            spdlog::info("declutter portal park: {} cast root(s) handed back "
                         "({} re-parent(s) this arm).",
                         g_reparented.size(), g_castReParents);
        }
        g_reparented.clear();
        g_castReParents     = 0;
        g_parkedPortalGraph = nullptr;
        g_parkedPortalNode  = nullptr;
        g_portalParkLogged  = false;
        g_portalReParks     = 0;
        g_castProbeCalls    = 0;
        // F-30 round 7: hand the first-person resting state back. Only when
        // the pin actually fired - a third-person arm never pins, so nothing
        // is written. The engine's own camera-state transition recomputes
        // this on the next real view change either way; the restore just
        // never leaves the flag different from how the arm found it.
        if (g_playerPinApplied) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* root3p = player->Get3D(false)) {
                    root3p->SetAppCulled(true);
                }
            }
            spdlog::info("declutter player pin: released, 3p re-culled as "
                         "found ({} re-assert(s) this arm).",
                         g_playerPinReAsserts);
        }
        g_playerPinApplied   = false;
        g_playerPinReAsserts = 0;
        // F-30 round 8: take kAlwaysDraw back off exactly the nodes that
        // lacked it. A node freed since (equip change) is just a dead
        // NiPointer entry whose object nothing renders; clearing a live one
        // returns it to the state the arm found.
        for (auto& objPtr : g_alwaysDrawSet) {
            if (auto* obj = objPtr.get()) {
                obj->GetFlags().reset(RE::NiAVObject::Flag::kAlwaysDraw);
            }
        }
        if (!g_alwaysDrawSet.empty()) {
            spdlog::info("declutter always-draw: cleared from {} cast node(s).",
                         g_alwaysDrawSet.size());
        }
        g_alwaysDrawSet.clear();
        g_alwaysDrawLogged = false;
        if (g_hidden.empty() && g_hiddenNodes.empty()) {
            return;
        }
        int restored = 0;
        for (auto& handle : g_hidden) {
            auto ref = handle.get();
            auto* root = ref ? ref->Get3D() : nullptr;
            if (root && root->GetAppCulled()) {
                root->SetAppCulled(false);
                ++restored;
            }
        }
        int nodesRestored = 0;
        for (auto& node : g_hiddenNodes) {
            if (node && node->GetAppCulled()) {
                node->SetAppCulled(false);
                ++nodesRestored;
            }
        }
        spdlog::info("declutter: restored {}/{} ref(s) + {}/{} cell branch(es).",
                     restored, g_hidden.size(), nodesRestored, g_hiddenNodes.size());
        g_hidden.clear();
        g_hiddenNodes.clear();
        g_cellLightsCut = false;
    }
}

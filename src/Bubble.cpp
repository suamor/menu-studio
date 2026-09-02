#include "PCH.h"

#include "ActorTickProbe.h"
#include "AnimEventProbe.h"
#include "Backdrop.h"
#include "Bubble.h"
#include "CameraArmStatePolicy.h"
#include "CameraCloseProbe.h"
#include "CameraDebtPolicy.h"
#include "CameraGate.h"  // ForcedBypass, for the arm-edge camera rebuild
#include "CbpcDrive.h"
#include "ClipProbe.h"
#include "CompanionLunge.h"
#include "CompanionProbe.h"
#include "EquipNotifyGate.h"
#include "Declutter.h"
#include "ExternalViewExitPolicy.h"
#include "FaceNeutral.h"
#include "ForcePause.h"
#include "FsmpDrive.h"
#include "FootIkGate.h"
#include "ItemPreviewBroker.h"
#include "ItemPreviewPolicy.h"
#include "MenuAnimationHoldPolicy.h"
#include "MovementArmPolicy.h"
#include "Offsets.h"
#include "OwnView.h"
#include "RotationOwnershipPolicy.h"
#include "SceneTint.h"
#include "Settings.h"
#include "ShadowPause.h"
#include "ActionBar.h"  // the editor button's owed open, paid on a gameplay frame
#include "StudioCamera.h"
#include "StudioLight.h"
#include "StudioRig.h"
#include "Transition.h"
#include "VersionCheck.h"
#include "WeaponPreview.h"
#include "WeaponStance.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace {
    // Zero a menu's Scaleform root alpha, and put it back. AS2 _alpha is 0..100.
    //
    // ⚠⚠ THIS REPLACED A kHide ON HUDMenu AND THE FIELD IS WHY. The comment
    // that message carried claimed "HUDMenu is meant to stay open; hiding is
    // what the engine itself does to it". The log refuted both halves on
    // 2026-08-28: 'HUD Menu' was in the open-menu list at 19:29:26.363, the
    // kHide went out at .377, and it was in NO later list for the rest of the
    // run. The engine does not do that to it - HUD Menu is still listed while a
    // vanilla InventoryMenu is up, which is the case the claim was reasoning
    // from. A message the engine reads as a close cannot be the hide this
    // wants, and it takes every HUD mod hooked onto that movie down with it.
    //
    // The alpha write leaves the menu on the stack, so nothing is destroyed and
    // nothing has to be rebuilt. Fitting Room hides its own host menu exactly
    // this way and has shipped it for months.
    //
    // ⚠ A MENU WITH NO uiMovie IS A SILENT NO-OP, and that is the right failure:
    // the HUD stays visible, which is what a player had before this existed.
    // Never a stranded HUD.
    void SetMenuRootAlpha(std::string_view a_menu, double a_alpha) {
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto menu = ui->GetMenu(a_menu); menu && menu->uiMovie) {
                menu->uiMovie->SetVariable("_root._alpha", RE::GFxValue(a_alpha));
            }
        }
    }

    // SE 1.5.97: at Main::Update+0x28E sits the unique E8 call to the
    // per-frame player-update dispatch (ID 35578). Verified by decompile +
    // exhaustive call-site scan (docs/SPIKE-A-RE.md). IDs/offsets live in
    // Offsets.h (§3.4 table).

    // Engine frame-dt globals, updated every frame even while paused
    // (Main::UpdateTimers, RVA 0x5B3E40). Logged for telemetry only - the
    // bubble runs on its own QPC clock.
    REL::Relocation<float*> g_slowDt{ MTB::Offsets::SlowDt };
    REL::Relocation<float*> g_realDt{ MTB::Offsets::RealDt };
    REL::Relocation<float*> g_dtVariant3{ MTB::Offsets::DtVariant3 };

    // BSFaceGenAnimationData::Update(float a_dt, bool a_force) - ID 25983,
    // RVA 0x3C4030. Advances expression/modifier/phoneme keyframe ramps and
    // the blink timers; the engine's only caller (0x3D9440, render-side face
    // model update) passes (g_slowDt, true) after clearing the +0x218 pending
    // flag - mirrored exactly in Tick. Returns whether anything changed.
    using FaceGenUpdate_t = bool(__fastcall*)(RE::BSFaceGenAnimationData*, float, bool);
    REL::Relocation<FaceGenUpdate_t> g_faceGenUpdate{ MTB::Offsets::FaceGenUpdate };

    // The face MESH bake. Update above moves the DATA; this is the half that
    // reaches the geometry, and while a menu holds the pause the engine only
    // ever runs it for RaceSex Menu. See Offsets::FaceGenApplyMorphs.
    using FaceGenApplyMorphs_t = void(__fastcall*)(RE::BSFaceGenNiNode*, bool);
    REL::Relocation<FaceGenApplyMorphs_t> g_faceGenApplyMorphs{
        MTB::Offsets::FaceGenApplyMorphs
    };

    bool          g_companionTickLogged = false;

    // OS-104. Whose headtracking we turned off, so Disarm can put it back.
    //
    // ⚠ A HANDLE, NOT A POINTER, and restored from the HANDLE rather than from
    // whoever is framed at Disarm time. The framed companion can change (or
    // clear) between the hold and the exit, and restoring "the current
    // companion" would then leave the ORIGINAL follower staring at nothing for
    // the rest of the save while un-holding someone we never held.
    RE::ActorHandle g_headtrackHeld{};

    // ⚠ bHeadTracking IS A SYMPTOM, NOT THE CAUSE - DO NOT WRITE IT.
    // 2026-08-06 spent a build proving this the wrong way round. The probe
    // showed a clean correlation (true -> the head drags, FALSE -> it does
    // not), so a sampler learned the gameplay value and a release put it
    // back. The next field pass read bHeadTracking=true on all FOUR racemenu
    // opens with the head still dead, which kills the theory outright.
    //
    // Two traps came out of it, both worth keeping:
    //   - the sampler INHERITED THE FAULT. The breakage persists into
    //     gameplay, so 'the value the player plays with' read as the stuck
    //     OFF and the release faithfully put OFF back - this mod breaking
    //     the thing it was fixing. See the CameraCloseProbe note for the
    //     same lesson in its first costume.
    //   - the variable is ENGINE-DRIVEN. It reads true while the actor is
    //     actively tracking something, so forcing it means writing into an
    //     output every frame and fighting whoever owns it: 288 releases in
    //     one session, most of them during ordinary play.
    //
    // The head-state probe below stays, because it is what settled this.

    // ⚠ THE HEAD-STATE PROBE (2026-08-06), and it exists because the fault
    // turned out to be PERSISTENT. Reaching the editor through Fitting Room
    // kills RaceMenu's head drag, and it STAYS killed: leaving to gameplay
    // and opening `showracemenu` from the console is still broken, and only
    // a RACE CHANGE - which rebuilds the 3D and the behaviour graph - clears
    // it. So this is not a condition inside the menu, it is residue left on
    // the actor, and every per-menu theory tested so far has come back
    // negative.
    //
    // Fired on the racemenu OPEN edge whether or not we bubble that menu, so
    // a working open (console, fresh game) and a broken one (after Fitting
    // Room) produce the same line and can simply be diffed. Whatever differs
    // between those two prints is the bug; nothing else has to be guessed.
    // ⚠ ONE LINE PER CHANGE, NOT ONE PER CALL. Disarm() runs EVERY FRAME while
    // no menu is open (its own weapon-debt comment says so), and the disarm tag
    // is printed from there, so at the main menu this wrote hundreds of
    // identical lines with 3D=false and every field unreadable. A log that
    // buries its own evidence is worse than no log.
    //
    // Deduplicating rather than gating on armedLastFrame_ on purpose: the r50
    // zero-frame cut can reach Disarm with that flag ALREADY false (see the
    // note above the weapon-debt block), so an armed-gate would silently drop
    // real teardowns - which is the one thing this probe must never do. A
    // teardown that changed nothing prints nothing; a teardown that changed
    // anything still prints, on every path.
    std::string g_lastHeadState;

    void LogHeadState(const char* a_tag, float a_spinYaw, bool a_spinBasisValid) {
        if (!MTB::Settings::GetSingleton().diagnosticProbes) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        bool graphHeadTracking = false;
        const bool gotGraph =
            player->GetGraphVariableBool("bHeadTracking", graphHeadTracking);
        std::string slots;
        std::string tracked;
        float       timer = -1.0f;
        if (auto* process = player->GetActorRuntimeData().currentProcess) {
            if (auto* high = process->high) {
                using HT = RE::HighProcessData::HEAD_TRACK_TYPE;
                for (std::uint32_t i = 0; i < HT::kTotal; ++i) {
                    if (i != 0) {
                        slots += ' ';
                        tracked += ' ';
                    }
                    slots += std::to_string(i) + ':' +
                             (high->headTrackTarget[i]
                                  ? std::to_string(high->headTrackTarget[i].native_handle())
                                  : "-");
                    tracked += high->headTracked[i] ? '1' : '0';
                }
                timer = high->headTrackTargetTimer;
            }
        }
        // ⚠ THE SPIN IS IN HERE BECAUSE IT IS THE OTHER THING WE LEAVE ON
        // THE ACTOR. The preview spin writes the player's 3D ROOT ROTATION
        // directly and re-asserts it through a hook on
        // PlayerCharacter::Update3DPosition, so a basis left captured is
        // residue of exactly the persistent, rebuild-clearable kind the race
        // change was observed to cure.
        std::string freeze;
        for (const auto& name : MTB::Settings::GetSingleton().freezeGraphBools) {
            bool v = false;
            if (!freeze.empty()) {
                freeze += ' ';
            }
            freeze += name + '=' +
                      (player->GetGraphVariableBool(name.c_str(), v) ? (v ? "1" : "0") : "?");
        }
        auto line = fmt::format(
            "head state [{}]: bHeadTracking={}{} | slots [{}] tracked [{}] "
            "timer {:.2f} | sceneHeadTrackRotation={} | 3D={} | spin yaw {:.3f} "
            "basis {} | freeze [{}]",
            a_tag, gotGraph ? (graphHeadTracking ? "true" : "FALSE") : "unreadable",
            gotGraph ? "" : " (graph has no such variable)", slots, tracked, timer,
            player->GetActorRuntimeData().boolFlags.all(
                RE::Actor::BOOL_FLAGS::kSceneHeadTrackRotation),
            player->Get3D(false) != nullptr, a_spinYaw,
            a_spinBasisValid ? "HELD" : "clear", freeze);
        // ⚠ THE TAG IS PART OF THE LINE, so an open and a disarm reporting the
        // same state still both print. That pairing is the whole method: the
        // two prints are meant to be diffed against each other.
        if (line == g_lastHeadState) {
            return;
        }
        g_lastHeadState = line;
        spdlog::info("{}", line);
    }

    // ⚠⚠ THE PLAYER'S HEADTRACKING IS SWITCHED OFF, NOT AIMED (2026-08-24,
    // user report: "player has headtracking when in MS ... they can change
    // their head position if we rotate the character away, they don't have
    // this effect when they are just facing the front").
    //
    // B-3 through B-7 v3 pinned a head-track TARGET straight ahead of the
    // VISUAL body every armed tick, spin included:
    //     heading = player->data.angle.z + spinYaw_
    // and the arithmetic hides the defect. spinYaw_ is a rotation of the
    // player's 3D ROOT NODE (see Tick: root->local.rotate = spin *
    // rootBaseRotate_); it never reaches data.angle.z. The engine solves the
    // neck in the LOGICAL frame, so it turns the head by target minus
    // data.angle.z, which is +spinYaw_ - and then the node spin turns the
    // whole already-solved skeleton by spinYaw_ AGAIN. The head came out
    // twisted from the body by exactly the spin. At spin zero the two terms
    // are both zero, which is the "no effect facing the front" half of the
    // report, and the reason six months of front-on shots never showed it.
    //
    // Dropping the compensation would straighten the neck, but the pin was
    // only ever a workaround for OTHER mods aiming the head (TDM), and what
    // this menu wants is the thing the user asked for: no head tracking. So
    // the player now gets the treatment the framed companion has had since
    // OS-104 - the actor state bit off and the target slots emptied, the
    // prior value captured and handed back. One mechanism for both actors,
    // and nothing left aiming a neck for the spin to double.
    RE::ActorHandle g_playerHeadtrackHeld{};
    bool            g_playerHeadtrackPrior = true;
    bool            g_playerHeadtrackFailLogged = false;

    // ⚠ THE BIT IS RE-ASSERTED EVERY ARMED TICK, THE SLOTS ARE CLEARED ONCE.
    // The companion's hold is once per arm because writing a BEHAVIOUR GRAPH
    // variable per frame is a graph call per frame. This one writes a bit in
    // the actor's own state, which costs nothing, and the player is the actor
    // TDM actually drives - a hold it can re-enable on its next tick is a
    // hold the user experiences as the bug still being there. The slot clear
    // is the part that costs an engine call, so it stays on the edge; if the
    // field ever shows a target re-acquired mid-session, that is the next
    // thing to move inside, and LogHeadState already prints all six slots.
    void HoldPlayerHeadtracking(RE::Actor* a_player) {
        if (!a_player || !a_player->Get3D()) {
            return;
        }
        auto* const st = a_player->AsActorState();
        if (!st) {
            if (!g_playerHeadtrackFailLogged) {
                g_playerHeadtrackFailLogged = true;
                spdlog::warn("head hold: the player has no actor state to write, so "
                             "head tracking is left alone in this menu.");
            }
            return;
        }
        if (g_playerHeadtrackHeld) {
            st->actorState2.headTracking = 0;  // the per-tick re-assert
            return;
        }
        g_playerHeadtrackPrior = st->actorState2.headTracking != 0;
        st->actorState2.headTracking = 0;
        // ⚠ AND THE TARGETS GO WITH IT, because the bit decides whether the
        // actor ACQUIRES a target rather than whether one is already held -
        // the same reading the companion hold is built on. Through the
        // engine's own clear so the paired headTracked flag goes too.
        //
        // ⚠ THE ID IS CHECKED FIRST. An absent Address Library id resolves to
        // its NEIGHBOUR rather than failing, so an unguarded call here would
        // run some unrelated engine function on the player. The state bit
        // above is a plain write and needs no id, so a build without the clear
        // still gets the hold - it just says so.
        std::size_t cleared = 0;
        bool        clearRan = false;
        if (auto* const proc = a_player->GetActorRuntimeData().currentProcess) {
            if (auto* const high = proc->high) {
                if (MTB::VersionCheck::IdOk(RELOCATION_ID(38726, 39756))) {
                    using HT = RE::HighProcessData::HEAD_TRACK_TYPE;
                    for (std::uint32_t i = 0; i < HT::kTotal; ++i) {
                        if (high->headTrackTarget[i]) {
                            ++cleared;
                        }
                        high->ClearHeadtrackTarget(static_cast<HT>(i), false);
                    }
                    clearRan = true;
                }
            }
        }
        g_playerHeadtrackHeld = a_player->GetHandle();
        spdlog::info("head hold: the player's head tracking is OFF for this menu so "
                     "a rotated preview stops twisting its neck (was {}, {}). "
                     "Restored to that value on menu exit.",
                     g_playerHeadtrackPrior ? "on" : "off",
                     clearRan ? fmt::format("{} live target(s) dropped", cleared)
                              : "the slot clear is absent from this build's Address "
                                "Library, so any target already held stays");
    }

    // Hand the head back, to the value we found rather than to a literal - a
    // player who had head tracking off before the menu (another mod, a scene)
    // must not come out of it switched on with nothing in the log saying we
    // did it.
    void ReleasePlayerHeadtracking() {
        if (!g_playerHeadtrackHeld) {
            return;
        }
        if (const auto ptr = g_playerHeadtrackHeld.get(); ptr) {
            if (auto* const st = ptr->AsActorState()) {
                st->actorState2.headTracking = g_playerHeadtrackPrior ? 1 : 0;
            }
            spdlog::info("head hold: the player's head tracking is restored to {}.",
                         g_playerHeadtrackPrior ? "on" : "off");
        }
        g_playerHeadtrackHeld  = RE::ActorHandle{};
        g_playerHeadtrackPrior = true;
    }

    // ⚠ WHAT bHeadTracking WAS BEFORE WE TOUCHED IT. The first version of this
    // restored a hardcoded `true`, which is not a restore - it is a WRITE
    // wearing a restore's name. A follower who already had headtracking off
    // (another mod, a scene, a package) would come out of the menu with it
    // switched ON, and nothing in the log would say we did it.
    bool g_headtrackPrior = true;

    // One-shot for the CANNOT-HOLD path, so a graph without bHeadTracking says
    // so once instead of every armed tick.
    bool g_headtrackFailLogged = false;

    // OS-107. Whose HEADING we turned, and what it was. Same ownership rules as
    // the headtracking latch above and for the same reasons: a handle so the
    // restore names whoever we actually turned, and the captured angle rather
    // than a literal, because a follower put back at a guessed heading is a
    // write wearing a restore's name.
    RE::ActorHandle g_headingHeld{};
    float           g_headingPrior = 0.0f;
    std::uint32_t g_companionSettled = 0;  // form id we have already sent moveStop

    // OS-108. WAS SHE WALKING WHEN THE MENU OPENED? An arm-edge latch, for the
    // same reason the player's is one: the pause makes it unknowable afterwards,
    // because the first thing the settle does is zero the very variable the
    // answer is read from.
    bool g_companionMovingArm = false;

    // OS-104: STOP HER STARING AT THE PLAYER. Engine headtracking locks a
    // follower's head and eyes onto the nearest interesting actor, and while
    // this menu is open that is the player standing right beside her. The shot
    // is composed on HER, so the subject spends the whole menu looking out of
    // frame at someone the viewer may not even be able to see (the player is
    // stood down for exactly this shot - see bHidePlayerForCompanion).
    //
    // ⚠⚠ ITS OWN FUNCTION, AND CALLED FROM THE TICK RATHER THAN FROM INSIDE
    // TickFramedCompanion (user 2026-08-16: "make sure we disable follower
    // headtracking when they are in FR no matter what the view is"). It used to
    // live inside that function, behind its three early returns, so whether a
    // follower stared at the player was decided by bTickCompanion, by
    // bFreezeCharacter and by whether the engine was animating this menu
    // itself. None of those three is a question about where she is LOOKING:
    // they are about whether we step her graph. A hold that a live menu, a
    // frozen-character setting or a switched-off animation tick can cancel is a
    // hold the player experiences as random.
    //
    // ⚠ ONCE PER ARM, NOT PER TICK. Writing a graph variable every frame is a
    // behaviour-graph call per frame for a value that does not change. The
    // handle latch is also what makes the restore honest.
    //
    // ⚠ RESTORED IN Disarm, ALWAYS, from the handle. A follower left with
    // headtracking off in a live save is a visible bug that outlives the menu,
    // which is the same reason every other borrowed piece of state in this
    // module has an exit path rather than a hope.
    void HoldFramedCompanionHeadtracking() {
        if (g_headtrackHeld) {
            return;
        }
        const auto companion = MTB::Declutter::FramedCompanion().get();
        auto* actor = companion.get();
        // The player is the shot's own subject and looks where the camera and
        // SPII put him; nothing here is about him. No 3D means no graph to
        // write, and the next tick asks again.
        if (!actor || actor == RE::PlayerCharacter::GetSingleton() || !actor->Get3D()) {
            return;
        }
        // ⚠⚠ THE ACTOR STATE BIT, NOT THE GRAPH VARIABLE (2026-08-16). The first
        // version wrote bHeadTracking through the behaviour graph, the field log
        // proves the write LANDED ("headtracking held OFF ... (was on)"), and
        // she went on staring at the player anyway. The note at the top of this
        // file already said why, one dimension over: bHeadTracking is a graph
        // OUTPUT that reads true while the actor is tracking something. Turning
        // an output off does not turn the tracking off.
        //
        // ⚠ MEASURED IN THE BINARY RATHER THAN GUESSED. Papyrus
        // Actor.SetHeadTracking is the engine's own switch for this and its AE
        // implementation (0x1409EA380) is four instructions long:
        //     AND dword [rcx+0xCC], 0xFFFFFFF7
        //     MOVZX eax, r9b / SHL eax, 3 / OR dword [rcx+0xCC], eax
        // Bit 3 of the dword at Actor+0xCC, which is ActorState2::headTracking.
        // It touches nothing else: no graph variable and no target. So this is
        // the whole of what the game means by "stop head tracking".
        bool state = false;
        bool prior = true;
        if (auto* const st = actor->AsActorState()) {
            prior              = st->actorState2.headTracking != 0;
            st->actorState2.headTracking = 0;
            state              = true;
        }
        // ⚠ AND THE TARGET IS CLEARED BESIDE IT, because the flag decides
        // whether she ACQUIRES one, not whether she is holding one already. A
        // follower who locked onto the player one frame before the menu opened
        // would keep that lock for as long as the engine leaves the handle in
        // place, which in a paused menu is the whole session. Cleared through
        // the engine's own ClearHeadtrackTarget so the paired headTracked flag
        // goes with the handle; the AI picks a new one after the menu the way
        // it always did, so there is nothing here to restore.
        std::size_t cleared = 0;
        if (auto* const proc = actor->GetActorRuntimeData().currentProcess) {
            if (auto* const high = proc->high) {
                using HeadTrack = RE::HighProcessData::HEAD_TRACK_TYPE;
                for (std::uint32_t i = 0; i < HeadTrack::kTotal; ++i) {
                    if (high->headTrackTarget[i]) {
                        ++cleared;
                    }
                    high->ClearHeadtrackTarget(static_cast<HeadTrack>(i), false);
                }
            }
        }
        if (state) {
            g_headtrackHeld  = actor->GetHandle();
            g_headtrackPrior = prior;
            spdlog::info("companion look: '{}' 0x{:08X}: headtracking held OFF so "
                         "she faces the shot instead of the player (was {}, {} live "
                         "target(s) dropped). Restored to that value on menu exit "
                         "(OS-104).",
                         actor->GetName(), actor->GetFormID(), prior ? "on" : "off",
                         cleared);
        } else if (!g_headtrackFailLogged) {
            // ⚠ THE HOLD IS NOT CLAIMED ON THIS PATH, so Disarm writes nothing.
            // Failing loudly and touching nothing beats a restore that "puts
            // back" a value we were never able to read.
            g_headtrackFailLogged = true;
            spdlog::warn("companion look: '{}' 0x{:08X}: could NOT hold headtracking: "
                         "she has no actor state to write. Leaving it alone; she will "
                         "keep looking at the player (OS-104).",
                         actor->GetName(), actor->GetFormID());
        }
    }

    // Keep the FRAMED COMPANION as alive as the player.
    //
    // Menu Studio's whole premise is that the world stops and the SUBJECT does
    // not. Once another mod can put a second subject in the shot (Fitting Room
    // dressing a follower), she has the same claim on it, and without this she
    // is a statue standing next to a breathing character - which reads worse
    // than either would alone and defeats the point of framing her at all.
    //
    // UpdateAnimation is TESObjectREFR vfunc 0x7D, a virtual on any reference
    // rather than a player-only path, so the mechanism generalises unchanged.
    // The PlayerCharacter override additionally refreshes the 1st and 3rd person
    // graphs; a plain Actor has one graph and the base implementation steps it.
    //
    // ⚠ SHE DOES NOT INHERIT THE PLAYER'S HOLD, and an earlier version of this
    // made her do so on the theory that "both statues or both live" was at least
    // coherent. Field: "they are still static, but they advance the animation
    // when we switch a gear piece". That is the hold, exactly.
    //
    // MenuAnimationHoldPolicy holds on equipOccurredThisSession, which is a
    // SESSION LATCH: once any equip clip has fired, the graph never steps again
    // until the menu closes. In an outfit editor you equip on almost every
    // click, so it trips within a second of opening and stays tripped. The only
    // motion left was the burst the gear change itself pumps, which is precisely
    // what was reported.
    //
    // That hold is about the PLAYER's draw and sheathe clips mis-selecting an
    // idle under a frozen VM. She is being DRESSED, not drawing a weapon, and
    // armour equips carry no draw clip. Holding her for the player's reason
    // bought nothing and cost the entire feature.
    //
    // Her exposure to the same lunge-loop class is real and unguarded, because
    // every guard reads the player. bTickCompanion is the switch, and a
    // follower who occasionally picks a wrong idle is still a better answer than
    // one who is reliably a statue.
    //
    // ⚠ THE 0.7.1 LUNGE LOOP IS THE RISK THIS CARRIES: stepping a behaviour
    // graph while the Papyrus VM is frozen lets it select its next pose against
    // stale state, and every guard built for that reads the PLAYER.
    // bTickCompanion is the switch, and turning it off leaves the player's own
    // liveness untouched.
    void TickFramedCompanion(float a_dt, bool a_engineAnimates) {
        const auto& cfg = MTB::Settings::GetSingleton();
        if (!cfg.tickCompanion || a_engineAnimates || MTB::Bubble::CharacterFrozen()) {
            return;
        }
        const auto companion = MTB::Declutter::FramedCompanion().get();
        auto* actor = companion.get();
        // Nothing to animate, and the player is already handled by the caller.
        if (!actor || actor == RE::PlayerCharacter::GetSingleton() || !actor->Get3D()) {
            return;
        }
        if (!g_companionTickLogged) {
            g_companionTickLogged = true;
            // Says what will actually HAPPEN, not merely that the function was
            // reached. The previous version logged on entry and read as success
            // while the graph step below was being skipped every tick.
            spdlog::info("companion live: '{}' 0x{:08X}: graph {}, face {}. Not held "
                         "by the player's equip hold.",
                         actor->GetName(), actor->GetFormID(),
                         cfg.tickAnimation ? "STEPPING" : "off (bTickAnimation)",
                         cfg.tickFace ? "stepping" : "off (bTickFace)");
        }
        // ⚠ THE HEADTRACK HOLD IS NOT HERE ANY MORE. It moved out to
        // HoldFramedCompanionHeadtracking, called from the armed tick above
        // this function's three early returns, because where she LOOKS must not
        // depend on whether we are stepping her graph (user 2026-08-16).

        // OS-107: TURN HER TO FACE THE LENS, which is the one thing the player
        // gets for free and she does not. SPII turns the player to face the
        // camera on every open, so his bearing to it is a constant and the shot
        // reads the same every time (field 2026-07-31: player heading 0.94
        // against a camera bearing of 0.73). Nothing turns a follower, so she is
        // presented at whatever heading her AI last left her at and the same
        // follower frames front-on, in profile or from behind on successive
        // opens.
        //
        // ⚠ THE BEARING COMES FROM StudioRig, not from a second derivation here.
        // It is the same number the key light is placed from, so her body and
        // the lighting cannot disagree about which way is front.
        //
        // ⚠ ONCE PER ARM AND RESTORED IN Disarm, exactly like the headtracking
        // hold. A follower left spun to face a camera that has gone is a visible
        // bug that outlives the menu.
        if (!g_headingHeld && cfg.faceCompanionToCamera) {
            const float prior = actor->data.angle.z;
            const float want =
                MTB::StudioRig::CameraBearing(actor->GetPosition(), prior) +
                cfg.companionFacingOffset * 0.017453292f;  // degrees in, radians out
            actor->data.angle.z = want;
            // ⚠ THE DATA WRITE ALONE MOVES NOTHING WHILE THE MENU HOLDS THE
            // PAUSE. Update3DPosition is what pushes the ref's angle down into
            // the 3D, and OS-103(b) measured the engine calling it ZERO times
            // across an armed arm, so the mesh would keep the old heading and
            // only snap round on exit. Calling it here is the same hand-stepping
            // this whole bubble is built on.
            actor->Update3DPosition(true);
            g_headingHeld  = actor->GetHandle();
            g_headingPrior = prior;
            spdlog::info("companion look: '{}' 0x{:08X}: turned to face the lens, "
                         "heading {:.2f} -> {:.2f} rad (offset {:.0f} deg). Restored to "
                         "{:.2f} on menu exit (OS-107).",
                         actor->GetName(), actor->GetFormID(), prior, want,
                         cfg.companionFacingOffset, prior);
        }

        // SETTLE HER OUT OF LOCOMOTION, or ticking the graph just plays whatever
        // clip she was caught in for the whole menu - a follower walking over to
        // you when it opened would walk in place forever, which is the same
        // defect the player's own settle exists to prevent.
        //
        // Split exactly as WeaponPreview splits it for the player, for the
        // reason recorded there: the VARIABLES are idempotent and safe to
        // re-assert every tick, the moveStop EVENT is not, and re-firing it per
        // tick is the shape that produced a permanent re-pump loop. TurnDelta is
        // deliberately left alone here too.
        // ⚠ OS-108: A MOVING ARM GETS NO SETTLE AT ALL. The player already
        // learned this one and she never got it. Field 2026-07-23 on his path:
        // the stop/settle transaction "caused the visible exit slide", and the
        // fix was to hold the caught pose on moving arms, "preserve every live
        // movement bit and graph variable, and let the unpaused world resume
        // naturally" (see movementPlan.freezeCaughtPose below).
        //
        // Her path settled unconditionally, so a follower caught mid-stride had
        // moveStop fired and Speed/Direction pinned at zero for the whole menu.
        // On exit her package moves her again while the graph is still pinned,
        // and she slides with no locomotion under her until the engine's own
        // movement update re-acquires - "eventually go back to normal".
        //
        // The latch reads Speed BEFORE the zeroing below, because afterwards the
        // answer is our own write. 1.0 rather than 0.0: Speed is a blended float
        // and a standing actor does not sit at exactly zero.
        if (const std::uint32_t id = actor->GetFormID(); id != g_companionSettled) {
            g_companionSettled = id;
            float      speed = 0.0f;
            const bool got   = actor->GetGraphVariableFloat("Speed", speed);
            g_companionMovingArm = got && speed > 1.0f && !actor->IsInMidair();
            if (g_companionMovingArm) {
                spdlog::info("companion look: '{}' 0x{:08X} was moving when the menu "
                             "opened (Speed {:.1f}). Her caught pose is held and her "
                             "movement graph left untouched, so she resumes without the "
                             "exit slide (OS-108).",
                             actor->GetName(), actor->GetFormID(), speed);
            } else {
                actor->NotifyAnimationGraph("moveStop");
            }
        }
        // ⚠ NOT ON A MOVING ARM. Re-asserting these every tick is exactly the
        // "preserve every live movement bit and graph variable" the player's fix
        // forbids, and holding them at zero for the menu's length is what leaves
        // the graph with nothing to resume from.
        if (!g_companionMovingArm) {
            actor->SetGraphVariableFloat("Speed", 0.0f);
            actor->SetGraphVariableFloat("Direction", 0.0f);
        }

        // Watch her event stream for the 0.7.1 lunge loop, which the field
        // confirmed does happen to her. Synced before the step so a graph
        // rebuilt by the last gear switch is re-attached to first.
        MTB::CompanionLunge::Sync(actor);
        // ⚠ THE HOLD IS ON THE BODY GRAPH ONLY, exactly like the player's. Her
        // face keeps ticking below, so a held companion still blinks rather
        // than becoming the statue `eaaf783` was written to stop.
        // ⚠ AND THE BODY GRAPH STOPS TOO ON A MOVING ARM (OS-108). Standing
        // aside on the movement variables without also holding the pose would
        // trade the exit slide for a follower marching on the spot for the whole
        // menu, which is the defect the settle was written to prevent. Holding
        // the caught frame is the player's own answer, and the hold is on the
        // BODY graph only - her face keeps ticking below, so she still blinks.
        if (cfg.tickAnimation && !g_companionMovingArm &&
            !MTB::CompanionLunge::ShouldHold()) {
            actor->UpdateAnimation(a_dt);
        }
        if (cfg.tickFace) {
            // No blink pinning and no lid composition here, unlike the player's
            // path. All of that exists to hold a pose STILL for a portrait; she
            // is scenery in that portrait and should simply be alive.
            if (auto* face = actor->GetFaceGenAnimationData()) {
                g_faceGenUpdate(face, a_dt, true);
            }
            if (cfg.faceMeshRefresh) {
                if (auto* node = actor->GetFaceNodeSkinned()) {
                    g_faceGenApplyMorphs(node, true);
                }
            }
        }

        // PUBLISH THE POSE. Stepping the graph writes bone LOCALS; nothing
        // recomputes the world transforms they feed while the world is paused,
        // so without this the whole tick above is invisible.
        //
        // ⚠ THE PLAYER HAS ALWAYS HAD THIS and she never did - OnFrame runs the
        // same NiUpdateData pass on `player->Get3D(false)` at the end of every
        // armed tick, with a comment naming exactly this failure ("the paused
        // scene graph may never run a downward pass, leaving new local
        // transforms/morphs invisible"). The companion tick was written as a
        // sibling of the player's graph step and inherited none of its publish.
        //
        // It also explains the field clue that survived the hold being removed:
        // she moves in a burst when a gear piece is switched, because Fitting
        // Room's 3D refresh runs a pass of its own. She was never frozen - she
        // was being drawn from stale world transforms between refreshes.
        if (auto* root = actor->Get3D()) {
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
            root->Update(ctx);
        }
        // AFTER the publish, so the probe measures what the frame will actually
        // draw. It reads both sides of the link this fix spans, which is what
        // lets one field run tell "the fix worked" from "the fix ran and the
        // fault is somewhere else".
        MTB::CompanionProbe::Sample(actor);
    }

    // Game.FadeOutGame's engine core (Offsets.h: FaderMenu-driven, UI-
    // clocked ⇒ animates while paused). ABI byte-verified from the native
    // wrapper: (fadingOut, blackFade, duration→XMM2, unk=false, secsBefore).
    using FadeOutGame_t = void (*)(bool a_fadingOut, bool a_blackFade, float a_duration,
                                   bool a_unk, float a_secsBeforeFade);
    REL::Relocation<FadeOutGame_t> g_fadeOutGame{ MTB::Offsets::FadeOutGame };

    struct PlayerDispatchHook {
        static void thunk(RE::Main* a_main) {
            // OS-103(b): THE DECISIVE PAIR. func() is the engine's own player
            // dispatch and it runs BEFORE our OnFrame every frame, so bracketing
            // it here is what separates "the engine un-culls him" from "somebody
            // else's hook does". Both calls no-op unless bPlayerCullProbe is on
            // and we actually stood him down this arm.
            MTB::Declutter::ProbePlayerCull(MTB::Declutter::CullPhase::kPreDispatch);
            func(a_main);
            MTB::Declutter::ProbePlayerCull(MTB::Declutter::CullPhase::kPostDispatch);
            MTB::Bubble::GetSingleton().OnFrame(a_main);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // ⚠ THE CAMERA'S INPUT SOURCE, moved UPSTREAM of every sink. FLICK hooks
    // the pump's dispatch call and swallows the entire queue while one of its
    // windows holds the mouse (Hooks::ProcessInputQueue in its PDB), so the
    // BSTEventSource sink below it sees NOTHING of a gesture begun before its
    // passthrough opens. The field cost: in Fitting Room only wheel notches
    // rolled DURING a held drag survived - the 08:03 log's "seen 4, taken 4"
    // against a player scrolling far more than four times.
    //
    // Menu Studio loads after FUCK.dll (alphabetical), so this write_call
    // wraps FLICK's and reads the REAL queue first. The gesture handling is
    // identical to the sink path it replaces; the sink loop stays compiled as
    // the fallback for a runtime where this site cannot be located.
    struct InputTapHook {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source,
                          RE::InputEvent* const*               a_events) {
            if (a_events) {
                const auto& s = MTB::Settings::GetSingleton();
                // ⚠ THE CHARACTER EDITOR IS NOT EXCLUDED ANY MORE. It was,
                // on the reading that RaceMenu owns its own camera and two
                // systems on one mouse is a fight. The field answered that:
                // with the studio finally arming in there, the editor's
                // camera is the one thing that still does not match the rest
                // of the mod, and ours is what the player came for. The spin
                // stays out (RaceMenu turns the character itself) - only the
                // camera comes in.
                // ⚠ AND THE STUDIO SESSION JOINS THE MENU COUNT. Under
                // bWaitForOwnerContext a covered menu can be counted with the
                // studio deliberately held down, and a camera that took drags
                // in there would orbit a shot nobody built.
                if (s.studioCamera && MTB::Bubble::OpenMenuCount() > 0 &&
                    MTB::Bubble::StudioSessionEntered()) {
                    auto& bubble = MTB::Bubble::GetSingleton();
                    for (auto* e = *a_events; e; e = e->next) {
                        bubble.HandleCameraInput(e);
                    }
                }
            }
            func(a_source, a_events);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static inline bool installed = false;
    };

    // B-7 v3 stutter (field r25: "it can rotate but it stutters like
    // crazy… some logic keeps resetting the player to face the front"):
    // SPIM calls this vfunc on the player after EVERY drag event
    // (spim_input.c tail: vtable+0x1F8), and it re-syncs the node from
    // data.angle - the pinned front - AFTER our per-tick compose when the
    // input pump lands later in the frame. Re-apply the spin on top of
    // every sync; the r25 telemetry proved the accumulator itself is
    // silky (park rock-steady, spin easing cleanly).
    struct Update3DPositionHook {
        static void thunk(RE::PlayerCharacter* a_this, bool a_warp) {
            MTB::Bubble::GetSingleton().PrepareSpinReassert();
            // OS-103(b) round 2: does THIS call un-cull him? Round 6 only ever
            // asked whether it REBUILDS his 3D (it does not), and the two are
            // different acts. No-ops unless bPlayerCullProbe is on.
            MTB::Declutter::ProbeUpdate3DBracket(true);
            func(a_this, a_warp);
            MTB::Declutter::ProbeUpdate3DBracket(false);
            MTB::Bubble::GetSingleton().ReassertSpin();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // OS-103(b) THE FIX. Answer the question the engine asks before it decides
    // whether the player is on screen, instead of overwriting the answer.
    //
    // The write watch caught the engine clearing our cull at AE id 32160 +0x4D8
    // (`and dword [rax+0xF4], 0xFFFFFFFE`), gated two instructions earlier on
    // `call [rax+0x388]`, which is vfunc 0x71, TESObjectREFR::Is3rdPersonVisible.
    // The same function also SETS the flag at +0xE3. So the engine derives his
    // visibility from this virtual every single frame, which is why three
    // rounds of re-asserting the flag lost 24105 times out of 24105: we were
    // overwriting a conclusion that gets recomputed, not a stored decision.
    //
    // ⚠ INTENT, NOT THE FLAG. This reads Declutter's WANT rather than
    // PlayerHiddenForCompanion, which reads the cull flag back. Under this fix
    // that flag is set by the engine BECAUSE of what we return here, so keying
    // on it would be a hook consulting its own output.
    struct Is3rdPersonVisibleHook {
        static bool thunk(RE::PlayerCharacter* a_this) {
            if (MTB::Declutter::PlayerHideIntent()) {
                return false;
            }
            return func(a_this);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    float QpcSeconds(std::uint64_t a_from, std::uint64_t a_to) {
        static const double freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        return static_cast<float>(static_cast<double>(a_to - a_from) / freq);
    }

    std::uint64_t QpcNow() {
        LARGE_INTEGER t{};
        ::QueryPerformanceCounter(&t);
        return static_cast<std::uint64_t>(t.QuadPart);
    }

    // ⚠ THE POLICY'S TABLE IS KEYED ON THESE NUMBERS, so a CommonLib that ever
    // renumbers CameraState must break the build rather than re-point the whole
    // table at the wrong states. kTotal is asserted too: a NEW state added at
    // the end is exactly the shape that produced this bug twice, and the policy
    // decides one by falling through to "take the camera", which is the safe
    // answer but not one anybody should get without noticing.
    namespace ArmState = MTB::CameraArmStatePolicy;
    static_assert(static_cast<int>(RE::CameraState::kFirstPerson) == ArmState::kFirstPerson);
    static_assert(static_cast<int>(RE::CameraState::kAutoVanity) == ArmState::kAutoVanity);
    static_assert(static_cast<int>(RE::CameraState::kVATS) == ArmState::kVATS);
    static_assert(static_cast<int>(RE::CameraState::kFree) == ArmState::kFree);
    static_assert(static_cast<int>(RE::CameraState::kIronSights) == ArmState::kIronSights);
    static_assert(static_cast<int>(RE::CameraState::kFurniture) == ArmState::kFurniture);
    static_assert(static_cast<int>(RE::CameraState::kPCTransition) == ArmState::kPCTransition);
    static_assert(static_cast<int>(RE::CameraState::kTween) == ArmState::kTween);
    static_assert(static_cast<int>(RE::CameraState::kAnimated) == ArmState::kAnimated);
    static_assert(static_cast<int>(RE::CameraState::kThirdPerson) == ArmState::kThirdPerson);
    static_assert(static_cast<int>(RE::CameraState::kMount) == ArmState::kMount);
    static_assert(static_cast<int>(RE::CameraState::kBleedout) == ArmState::kBleedout);
    static_assert(static_cast<int>(RE::CameraState::kDragon) == ArmState::kDragon);
    static_assert(static_cast<int>(RE::CameraState::kTotal) == ArmState::kTotal);

    // The engine's name for a camera state. A field report that says "the
    // close-up never happened" is unfalsifiable without it; with it, one log
    // line names who was holding the camera.
    const char* CameraStateName(RE::CameraState a_id) {
        switch (a_id) {
        case RE::CameraState::kFirstPerson:  return "kFirstPerson";
        case RE::CameraState::kAutoVanity:   return "kAutoVanity";
        case RE::CameraState::kVATS:         return "kVATS";
        case RE::CameraState::kFree:         return "kFree";
        case RE::CameraState::kIronSights:   return "kIronSights";
        case RE::CameraState::kFurniture:    return "kFurniture";
        case RE::CameraState::kPCTransition: return "kPCTransition";
        case RE::CameraState::kTween:        return "kTween";
        case RE::CameraState::kAnimated:     return "kAnimated";
        case RE::CameraState::kThirdPerson:  return "kThirdPerson";
        case RE::CameraState::kMount:        return "kMount";
        case RE::CameraState::kBleedout:     return "kBleedout";
        case RE::CameraState::kDragon:       return "kDragon";
        default:                             return "?";
        }
    }

    RE::ThirdPersonState* GetThirdPersonState() {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* state = camera ? camera->currentState.get() : nullptr;
        if (state && state->id == RE::CameraState::kThirdPerson) {
            return static_cast<RE::ThirdPersonState*>(state);
        }
        return nullptr;
    }

    // The state SLOT, reachable from any current camera state - the same
    // object OwnView's ThirdStateObject and the close probe's SampleView
    // read. The park's PAYMENT goes through this one: the accessor above
    // returns null the moment the player switches POV, and a payment gated
    // on it was silently dropped while its debt was destroyed anyway - a
    // POV toggle inside the ~150 ms deferred-exit window ate the restore.
    RE::ThirdPersonState* ThirdPersonSlotState() {
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera) {
            return nullptr;
        }
        return static_cast<RE::ThirdPersonState*>(
            camera->cameraStates[RE::CameraState::kThirdPerson].get());
    }

    // F-14 v3: SPIM's own inspect-mode gate - the right stick belongs to
    // the ITEM while its 3D preview is zoomed (Inventory3DManager
    // zoomProgress > 0) and to the CHARACTER otherwise.
    bool ItemPreviewZoomedOut() {
        auto* inv = RE::Inventory3DManager::GetSingleton();
        return !inv || inv->GetRuntimeData().zoomProgress == 0.0f;
    }

    // TRUE when the blink machine is parked AND the lids are open, i.e. there
    // is nothing in flight on the face and it can be held still without
    // freezing a half-finished blink.
    //
    // The distinction matters. Holding the face the moment the POSE freezes
    // would recreate the shut-eyes bug this stint just fixed: whatever the
    // pause caught mid-blink would be the last thing baked. Settle first, hold
    // second - the same order the cancelled-effect lesson teaches.
    // unk200: 0 waiting / 1 closing / 2 opening / 3-4 look-holds.
    const char* WeaponStateName(int a_ws) {
        switch (a_ws) {
        case 0:  return "kSheathed";
        case 1:  return "kWantToDraw";
        case 2:  return "kDrawing";
        case 3:  return "kDrawn";
        case 4:  return "kWantToSheathe";
        case 5:  return "kSheathing";
        default: return "?";
        }
    }

    // ── POST-CLOSE WEAPON-STATE WATCH ──────────────────────────────────────
    //
    // Field 2026-07-21: "there is a chance that the character becomes stuck,
    // preventing them from drawing or sheathing the weapon entirely", after
    // opening the inventory mid-draw and swapping weapons inside it.
    //
    // ⚠ EVERY OTHER WEAPON LINE IN THIS PLUGIN IS ARMED-ONLY, so the log goes
    // silent at exactly the moment this symptom starts. A probe whose fuse
    // ends before the symptom cannot see it
    // ([[probe-fuse-must-outlive-the-symptom]]); this one OPENS at the close
    // edge and runs past it.
    //
    // WHAT IT DECIDES, in one run. WEAPON_STATE is a small state machine and
    // only kSheathed and kDrawn are terminal. Our pumps drive it by stepping
    // the graph until the state edge arrives, and they give up at a cap - so
    // a capped pump can hand the world back a player parked on kDrawing or
    // kSheathing with no clip left running to move them off it. The engine's
    // own draw toggle then has nothing to do, which IS the report.
    //   non-terminal here -> we stranded the state machine; the fix is at the
    //                        pump that capped.
    //   terminal here     -> the state machine is fine and the fault is in
    //                        which CLIP the graph latched, which is a
    //                        different search entirely.
    // A negative is as useful as a positive, which is the point.
    //
    // Costs nothing in normal play: it only runs in a bounded window after a
    // bubble menu closes, logs on CHANGE, and gives one verdict line.
    void WatchWeaponStateAfterClose(bool a_armed, std::uint64_t a_now,
                                    float (*a_since)(std::uint64_t, std::uint64_t)) {
        static bool          wasArmed{ false };
        static bool          watching{ false };
        static std::uint64_t closedAt{ 0 };
        static int           lastWs{ -1 };
        static bool          verdictDone{ false };

        const bool closedEdge = wasArmed && !a_armed;
        wasArmed              = a_armed;
        if (a_armed) {
            watching = false;  // a re-arm cancels the watch; the next close restarts it
            return;
        }
        if (closedEdge) {
            watching    = true;
            closedAt    = a_now;
            lastWs      = -1;
            verdictDone = false;
        }
        if (!watching) {
            return;
        }

        auto* const player = RE::PlayerCharacter::GetSingleton();
        // Gate on real 3D. The state machine means nothing without a graph,
        // and a member read before the 3D exists is the CTD shape in
        // [[engine-singletons-exist-before-their-arrays]].
        if (!player || !player->Get3D()) {
            return;
        }
        auto* const state = player->AsActorState();
        if (!state) {
            return;
        }

        const int   ws    = static_cast<int>(state->GetWeaponState());
        const float since = a_since(closedAt, a_now);
        if (ws != lastWs) {
            lastWs = ws;
            spdlog::info("weapon watch: +{:.2f}s after the last bubble menu closed: weapon "
                         "state {} ({}).", since, ws, WeaponStateName(ws));
        }
        // 1.5 s is past any real draw or sheathe clip, so anything still
        // mid-transition here is parked, not in progress.
        if (!verdictDone && since >= 1.5f) {
            verdictDone = true;
            if (ws == 0 || ws == 3) {
                spdlog::info("weapon watch: settled on {}. The world was handed a TERMINAL "
                             "weapon state, so a stuck draw/sheathe is not this.",
                             WeaponStateName(ws));
            } else {
                spdlog::warn("weapon watch: STILL {} ({}) 1.5s after the menu closed. The "
                             "weapon state machine was STRANDED mid-transition. The player "
                             "cannot draw or sheathe from here. Look at the last pump line "
                             "above: a CAPPED pump is the suspect.",
                             ws, WeaponStateName(ws));
            }
        }
        if (since > 6.0f) {
            watching = false;  // long past any transition; stop costing anything
        }
    }

    bool FaceAtRest(RE::BSFaceGenAnimationData* a_face) {
        if (!a_face) {
            return true;  // nothing to hold
        }
        if (a_face->unk200 != 0) {
            return false;  // mid-blink or parked in a look-hold
        }
        const auto& lids = a_face->unk100;
        if (!lids.values || lids.count < 2) {
            return true;
        }
        return lids.values[0] <= 0.01f && lids.values[1] <= 0.01f;
    }
}

namespace MTB {
    Bubble& Bubble::GetSingleton() {
        static Bubble instance;
        return instance;
    }

    void Bubble::InstallHook() {
        // The offset comes from VersionCheck, not from Offsets.h: on any build
        // other than the two we measured by hand, the hand-measured offset is
        // wrong and the site is found by matching the call's TARGET instead.
        const auto callOffset = VersionCheck::DispatchCallOffset();
        if (callOffset == 0) {
            spdlog::error("No player-dispatch call site inside Main::Update on this runtime; "
                          "Bubble NOT installed. (plugin.cpp normally refuses to load at all "
                          "in this state; reaching here means iRuntimeGate=1.)");
            return;
        }
        REL::Relocation<std::uintptr_t> site{ Offsets::MainUpdate, callOffset };

        // Never write_call a site that is no longer the vanilla E8 - another
        // mod may have claimed or patched it in an incompatible way. Kept even
        // though VersionCheck already read this byte: it re-reads it at the
        // moment of the write, so a mod that patched the site in between is
        // still caught.
        if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
            byte != 0xE8) {
            spdlog::error(
                "Main::Update+0x{:X} is 0x{:02X}, expected E8 (call). Another mod "
                "patched the player-dispatch site incompatibly. Bubble NOT installed.",
                callOffset, byte);
            return;
        }

        auto& trampoline = SKSE::GetTrampoline();
        PlayerDispatchHook::func =
            trampoline.write_call<5>(site.address(),
                                     reinterpret_cast<std::uintptr_t>(&PlayerDispatchHook::thunk));
        spdlog::info("Frame driver installed at Main::Update+0x{:X}.", callOffset);

        // The camera's input tap, outside FLICK's swallow. The byte here is an
        // E8 pointing at FLICK's trampoline rather than the engine dispatch -
        // that is the expected shape (VersionCheck's chaining case accepted
        // it), and chaining onto FLICK is the point.
        if (const auto pumpOffset = VersionCheck::InputPumpCallOffset(); pumpOffset != 0) {
            REL::Relocation<std::uintptr_t> pumpSite{ Offsets::InputPumpCaller, pumpOffset };
            if (const auto byte = *reinterpret_cast<std::uint8_t*>(pumpSite.address());
                byte == 0xE8) {
                InputTapHook::func = trampoline.write_call<5>(
                    pumpSite.address(),
                    reinterpret_cast<std::uintptr_t>(&InputTapHook::thunk));
                InputTapHook::installed = true;
                spdlog::info("Input tap installed at pump+0x{:X}: camera gestures are "
                             "read before FLICK's input block can swallow them.",
                             pumpOffset);
            } else {
                spdlog::warn("Input tap: pump+0x{:X} is 0x{:02X}, expected E8. "
                             "Skipped; the camera reads the event sink instead.",
                             pumpOffset, byte);
            }
        }

        // Update3DPosition (TESObjectREFR vfunc 0x3F) on the PLAYER vtable -
        // the spin re-assert (see Update3DPositionHook). write_vfunc, per
        // the house rule: never write_branch a non-branch site.
        REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
        Update3DPositionHook::func = playerVtbl.write_vfunc(0x3F, Update3DPositionHook::thunk);
        spdlog::info("Preview-spin re-assert installed on PlayerCharacter::Update3DPosition.");

        // OS-103(b): the player-hide fix, on the same vtable and by the same
        // mechanism. vfunc 0x71 is Is3rdPersonVisible; the engine reads it every
        // frame and writes his app-cull from the answer.
        Is3rdPersonVisibleHook::func =
            playerVtbl.write_vfunc(0x71, Is3rdPersonVisibleHook::thunk);
        spdlog::info("Player-hide installed on PlayerCharacter::Is3rdPersonVisible "
                     "(vfunc 0x71). The engine now culls him for us instead of "
                     "against us.");

        // r23 DIAGNOSTIC. Installed here with the other vtable writes rather
        // than per-tick: the hkbClipGenerator vtable is process-global and does
        // not depend on the player's 3D existing, unlike the graph SINK, which
        // does and is therefore retried every frame.
        // The equip gate is the FIX and always installs. The probes below are
        // diagnostics and install only when asked: see Settings::diagnosticProbes
        // for why they are not merely silenced.
        EquipNotifyGate::Install();
        // ⚠ THE PROBE INSTALLS USED TO LIVE HERE, AND THEY NEVER RAN.
        //
        // This function is part of plugin init; the INI is not read until the
        // save loads, ~25 s later in the field log. So `diagnosticProbes` was
        // ALWAYS false here and neither probe was ever installed from an INI
        // setting - only from a compiled-in default. Both are idempotent, so
        // they now sit in the per-frame retry beside AnimEventProbe::Install(),
        // which is the one that worked and the reason the discrepancy showed
        // up at all (its graph sink is retried per frame by necessity).
        //
        // ⚠ AND THIS INVALIDATES A MEASUREMENT THE DOCS TREAT AS SETTLED.
        // ActorTickProbe counts Actor::Update calls. With the hook never
        // installed the counter is trivially 0, and the disarm line still
        // prints "Actor::Update ran 0 time(s) ... ZERO: the actor does not
        // update while paused". That line is printed by Bubble, not by the
        // hook, so it reads identically whether the truth is "zero calls" or
        // "no hook". Any run whose log lacks `actor tick probe: hooked` proves
        // nothing about Actor::Update. Re-measure before citing it again.
    }

    void Bubble::Register() {
        // r19: register the studio's own pausing menu before the watcher, so a
        // menu that is already open at this point can still be covered.
        ShadowPause::Register();
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&GetSingleton());
            spdlog::info("Menu watcher registered.");
        } else {
            spdlog::error("UI singleton unavailable; menu watcher NOT registered.");
        }
        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink<RE::InputEvent*>(&GetSingleton());
            spdlog::info("Input watcher registered (preview spin).");
        } else {
            spdlog::error("Input device manager unavailable; preview spin has no input.");
        }
    }

    void Bubble::CancelDipIfActive() {
        // F-12 exit discipline: no exit path may leave the screen dark.
        // Only touches the fader when OUR dip is mid-flight (black cut or
        // the post-build hold) - an unconditional fade-in here would fight
        // the engine's own fades (loading screens ride the same FaderMenu).
        if (dipPhase_ != 0 || dipHoldFrames_ > 0) {
            dipPhase_ = 0;
            dipHoldFrames_ = 0;
            g_fadeOutGame(false, true, 0.15f, false, 0.0f);
            spdlog::debug("dip: cancelled (exit before the reveal), fading back in.");
        }
    }

    void Bubble::ArmOwnViewIfOurs(bool a_force) {
        // F-15: own the view when no view mod does - first-person arms (a
        // first-person camera means nothing switched it) and menus outside
        // every installed view mod's coverage. Applies the FULL SPIM
        // framing; the CLOSE EVENT restores it (r33: restoring at the
        // deferred Disarm kept the framed camera up through the grace
        // window - a jarring split-second third-person angle after
        // first-person barters). No-op when already framed (menu switch).
        // r45: a_force bypasses the coverage check - the tick-3 fallback
        // fires it when a COVERED menu is still first person and unframed
        // (the covering mod bowed out; before r45 the fallback re-ran the
        // coverage check and silently declined - the rare "first-person
        // barter, no player" field case).
        // ⚠ THE CHARACTER EDITOR IS BACK IN, AND BOTH EVICTIONS WERE
        // WRONGFUL CONVICTIONS. Two rounds blamed this framing for the
        // stranded exit (over-shoulder shot over the world, no UI, cleared
        // only by a camera move) and a comment here once called the fight
        // structural - the editor's own camera re-restoring over ours. The
        // 2026-08-05 probe convicted the real culprit by NAME: the strip's
        // editor button force-hid only the origin menu, leaving the TWEEN
        // MENU wrapper open and invisible under the whole session - HUD
        // faded, its cursor up, the camera never handed back. OwnView's
        // restore "not taking" was that stuck tween refusing to let the
        // engine resume; with the button closing the tween (ActionBar),
        // the exit hands back clean, field-confirmed with NO framing at
        // all. So the editor now gets the same opening shot as every other
        // menu no view mod claims (field: "why is the angle so different
        // from racemenu?" - SPII authors the inventory shot, the editor
        // authored its own). Session behaviour is untouched: the editor's
        // sliders own the character, our camera takes over on the first
        // drag, the same contract as every other menu.
        if (OwnView::Active()) {
            return;
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->currentState) {
            return;
        }
        const auto stateId = camera->currentState->id;
        const bool firstPerson = stateId == RE::CameraState::kFirstPerson;
        // r52 (field: "show player in inventory doesn't even work when we
        // are on the horse" - SPII declines mounted, source-confirmed, so
        // it's ours). A mounted arm's camera is the MOUNT state (kMount),
        // NOT kThirdPerson - so before r52 we neither forced third nor
        // framed the active mount camera, and the void came up EMPTY.
        //
        // ⚠ AND r52 FIXED THAT BY ADDING ONE ENTRY TO A TWO-ENTRY ALLOWLIST,
        // which is why the same defect came back through a different door on
        // 2026-08-11: an animation-driven camera at the barter menu, framed
        // invisibly and logged as an "unmanaged third-person arm". The state
        // table and its reasoning now live in CameraArmStatePolicy.h; this
        // site just asks it.
        const auto plan = CameraArmStatePolicy::ChooseArm(static_cast<int>(stateId));
        if (!a_force && !OwnView::ShouldOwn(currentMenuName_, firstPerson)) {
            return;
        }
        if (!plan.frame) {
            // kFree only: somebody is driving the camera on purpose. Stand
            // down completely rather than framing a view nobody is looking
            // through and then owing a restore for it.
            spdlog::info("own view: the camera is in {}, standing aside, "
                         "something else is driving it.",
                         CameraStateName(stateId));
            return;
        }
        const bool mounted = plan.mountOffsets;
        const bool forcedThird = plan.forceThird;
        if (forcedThird) {
            spdlog::debug("own view: {} arm, switching to third person.",
                          CameraStateName(stateId));
        }
        // ⚠⚠ WHAT THE CLOSE RETURNS TO IS DECIDED HERE, NOT READ AT THE ARM.
        // Field 2026-09-02: through Tab the arm finds kTween, and handing
        // kTween back is the stuck camera. The park's reading, carried from
        // the tween's open edge, knows the player's own state and field of
        // view; the player's mode bit is the last word when it does not.
        std::optional<OwnView::PriorCamera> prior;
        if (forcedThird) {
            const bool readingUsable = preMenuFovValid_ && preMenuCameraState_ >= 0 &&
                                       preMenuCameraState_ != CameraArmStatePolicy::kTween;
            const int returnTo = CameraArmStatePolicy::ChooseReturnState({
                .liveState = static_cast<int>(stateId),
                .readingValid = preMenuFovValid_ && preMenuCameraState_ >= 0,
                .readingState = preMenuCameraState_,
            });
            if (returnTo != static_cast<int>(stateId)) {
                // ⚠ THE FIELD OF VIEW FOLLOWS THE SAME RULE. A reading that saw
                // kTween saw the tween's field of view too, so it is no more
                // the player's than the live value is; either is the tween's
                // 90 and the rotation park's withhold is what stands between it
                // and the camera. Only a usable reading knows the real number.
                prior = OwnView::PriorCamera{
                    .stateId = returnTo,
                    .worldFOV = readingUsable ? preMenuFov_ : camera->worldFOV,
                };
                spdlog::info("own view: the arm found {} and that is not a state to hand "
                             "back; the close returns to {} at fov {:.1f} ({}).",
                             CameraStateName(stateId),
                             CameraStateName(static_cast<RE::CameraState>(returnTo)),
                             prior->worldFOV,
                             readingUsable ? "from the park's reading taken before the tween"
                                           : "blind, no reading from before the tween");
            }
        }
        // ApplyFraming does the transition itself (SPIM's direct SetState -
        // the ForceThirdPerson request path stalled on COMBAT arms, r32
        // field: "in combat we don't see the character enter the view").
        OwnView::ApplyFraming(forcedThird, mounted, prior);
        if (mounted) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto p = player ? player->GetPosition() : RE::NiPoint3{};
            spdlog::info("own view: MOUNTED arm, framing raised for the rider; "
                         "player at ({:.0f},{:.0f},{:.0f}). If the void/constellation "
                         "reads wrong here, the backdrop centers on this point.",
                         p.x, p.y, p.z);
        }
        if (auto* tps = GetThirdPersonState()) {
            freeRotArm_ = tps->freeRotation.x;  // the spin park adopts the framed view
            freeRotParked_ = true;              // ...and the teardown owes it back
        }
    }

    // ⚠ a_pinHeading IS A PARAMETER RATHER THAN A RE-DERIVED TEST, so the one
    // decision made in ChooseReassert is the one applied here. Both callers run
    // from the Update3DPosition hook at different points in the frame, and a
    // second reader of the same question would drift from the first.
    void Bubble::RestoreOwnedRotationState(RE::PlayerCharacter* a_player,
                                           bool a_pinHeading) {
        // ⚠⚠ THE PIN DIES WITH THE MENU, NOT WITH THE ARM. Past the close edge
        // SPIM has already handed the player's real heading back and this write
        // would be the last one - the player snapping round to face a camera
        // that is no longer framing them. Reasoning at PinHeading.
        if (a_player && a_pinHeading) {
            a_player->data.angle.z = armedHeading_;
        }
        if (auto* tps = GetThirdPersonState()) {
            const float delta = freeRotArm_ - tps->freeRotation.x;
            if (std::fabs(delta) > 1.0f) {
                // A menu switch re-ran SPIM's framing; keep its new park.
                freeRotArm_ = tps->freeRotation.x;
            } else {
                // Undo SPIM's camera counter-rotation before the camera can
                // consume it.
                tps->freeRotation.x = freeRotArm_;
            }
        }
    }

    void Bubble::PrepareSpinReassert() {
        const auto& settings = Settings::GetSingleton();
        if (!armedLastFrame_ || !spinBasisValid_ || raceMenuOpen_.load() ||
            !settings.previewSpin) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool mounted = player && player->IsOnMount();
        const bool spimPresent =
            settings.overrideSpimRotation && OwnView::SpimPresent();
        const auto plan = RotationOwnershipPolicy::ChooseReassert({
            .armed = true,
            .spinBasisValid = true,
            .previewSpin = true,
            .mounted = mounted,
            .spimPresent = spimPresent,
            .overrideSpimRotation = settings.overrideSpimRotation,
            // Atomic, and read rather than latched: this hook fires at its own
            // point in the frame, so countedMenus_ is not safe to touch here.
            .menuOpen = menusOpen_.load() > 0,
            .spinYaw = spinYaw_,
        });
        if (!plan.neutralizeBeforeOriginal) {
            return;
        }
        RestoreOwnedRotationState(player, plan.restorePlayerHeading);
    }

    void Bubble::ReassertSpin() {
        const auto& settings = Settings::GetSingleton();
        if (!armedLastFrame_ || !spinBasisValid_ || raceMenuOpen_.load() ||
            !settings.previewSpin) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool mounted = player && player->IsOnMount();
        const bool spimPresent =
            settings.overrideSpimRotation && OwnView::SpimPresent();
        const auto plan = RotationOwnershipPolicy::ChooseReassert({
            .armed = true,
            .spinBasisValid = true,
            .previewSpin = true,
            .mounted = mounted,
            .spimPresent = spimPresent,
            .overrideSpimRotation = settings.overrideSpimRotation,
            .menuOpen = menusOpen_.load() > 0,
            .spinYaw = spinYaw_,
        });
        if (!plan.reassertRoot) {
            return;
        }

        if (plan.restorePlayerHeading || plan.restoreCameraPark) {
            RestoreOwnedRotationState(player, plan.restorePlayerHeading);
        }

        RE::NiMatrix3 spin;
        spin.EulerAnglesToAxesZXY(0.0f, 0.0f, spinYaw_);
        // Open 3: READ the ownership the frame tick already decided, do not
        // re-derive it. This runs from SPIM's Update3DPosition vfunc, which
        // fires on the PLAYER at a different point in the frame - capturing a
        // basis from here would race the tick that owns it.
        const bool companionOwnsSpin = spinCompanionId_ != 0;
        auto* playerRoot = player ? player->Get3D(false) : nullptr;
        if (playerRoot) {
            playerRoot->local.rotate =
                companionOwnsSpin ? rootBaseRotate_ : spin * rootBaseRotate_;
        }
        if (auto* horse = spinHorseRoot_.get()) {  // r54: keep the horse with the rider
            horse->local.rotate =
                companionOwnsSpin ? horseBaseRotate_ : spin * horseBaseRotate_;
        }
        if (auto* her = spinCompanionRoot_.get()) {
            her->local.rotate = spin * companionBaseRotate_;
            // Her own publish: the propagation below is the PLAYER's node and
            // does not reach a second skeleton.
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
            her->Update(ctx);
        }
        if (plan.propagateSceneGraph && playerRoot) {
            // Update3DPosition already published SPIM's transform to `world`.
            // A local-only overwrite is not render-visible until a later
            // scene pass, producing the reported snap to the front. Publish
            // our composed transform before the vfunc returns.
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
            playerRoot->Update(ctx);
        }
    }

    // ---------------------------------------------------------------- //
    // Open 3: the framed companion is the second rotatable subject       //
    // ---------------------------------------------------------------- //
    // WHO DOES THE DRAG TURN? Her, whenever she is in the shot - not both.
    //
    // The mount is the precedent for a second subject and it settled the
    // mechanism, but not this question, because a rider and a horse are ONE
    // visual unit: a shared yaw keeps them together and the alternative is
    // absurd. A player and a follower are two people standing near each other.
    // Spinning both about their own origins does not orbit them around one
    // another, so it never shows the user a new angle on the pair - it just
    // pirouettes two characters independently, and the one being dressed can no
    // longer be turned without also sweeping the one who is in the way.
    //
    // One drag, one meaning. While she is the subject the player holds the
    // pinned heading he entered with, which is the same value the spin composes
    // on and therefore costs nothing to keep.
    //
    // With no companion named, every write below reduces to exactly what
    // shipped before: spin * base on the player, base on nobody else.
    void Bubble::CaptureSpinCompanion(RE::Actor* a_actor) {
        auto* root = a_actor->Get3D();
        if (!root) {
            return;  // no 3D yet - Fitting Room may still be un-culling her
        }
        spinCompanionRoot_ = RE::NiPointer<RE::NiAVObject>{ root };
        companionBaseRotate_ = root->local.rotate;
        spinCompanionId_ = a_actor->GetFormID();
        spdlog::info("spin: '{}' 0x{:08X} is the subject. The drag turns HER now, "
                     "and the player holds his entry heading.",
                     a_actor->GetName(), spinCompanionId_);
    }

    void Bubble::ReleaseSpinCompanion() {
        // Through the STORED pointer. NiPointer holds a strong reference, so
        // this is safe even if her 3D was rebuilt underneath us - writing a
        // detached node is harmless, and re-resolving would un-spin whoever
        // happens to be selected NOW instead of who we actually spun.
        if (auto* her = spinCompanionRoot_.get()) {
            her->local.rotate = companionBaseRotate_;
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
            her->Update(ctx);
        }
        spinCompanionRoot_.reset();
        spinCompanionId_ = 0;
    }

    bool Bubble::SyncSpinCompanion() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* actor = Declutter::FramedCompanion().get().get();
        if (actor == player) {
            actor = nullptr;  // the player is never his own companion
        }
        const std::uint32_t id = actor ? actor->GetFormID() : 0;
        if (id == spinCompanionId_) {
            // ⚠ HER 3D CAN BE REPLACED WITHOUT THE SELECTION CHANGING. Fitting
            // Room forces a refresh on her every time a gear piece is switched,
            // which is most clicks in an outfit editor. A basis captured from
            // the old node would then be composed onto a new one, and the
            // rotation would jump by whatever the two disagreed about.
            if (actor && spinCompanionRoot_ && actor->Get3D() != spinCompanionRoot_.get()) {
                ReleaseSpinCompanion();
                CaptureSpinCompanion(actor);
            }
            return spinCompanionId_ != 0;
        }
        ReleaseSpinCompanion();
        if (actor) {
            CaptureSpinCompanion(actor);
        }
        // OWNERSHIP CHANGED, so the accumulated drag belongs to nobody. Handing
        // it to the new subject would snap her by however far the last one was
        // turned; leaving it on the player when she is deselected would snap
        // HIM. Both subjects are sitting at their base rotation at this instant,
        // so zeroing here moves nothing on screen.
        spinTarget_ = 0.0f;
        spinYaw_ = 0.0f;
        return spinCompanionId_ != 0;
    }

    // B-8 v2's deferred exit-move mirror is GONE, not disabled. It fired into
    // the close→open gap of every menu SWITCH (field r23: "switch to magic menu
    // and I see the player walk for a bit", measured gaps 52-71 ms), and at
    // close time a switch is indistinguishable from a real exit. The deferral
    // was meant to wait out the gap, then the whole mirror was retired and this
    // was left as an empty body its own header note asked to remove "after field
    // confirmation". A day of field play was that confirmation.
    //
    // FireDeferredWeaponRestore below is the surviving deferral and still uses
    // the same measured 0.085 s window.

    void Bubble::FireDeferredWeaponRestore(bool a_force) {
        // F-26 r2. Pays the sheathe Disarm deferred, once the close→open gap
        // of a menu switch has passed without a bubble menu re-opening. The
        // 0.085 s window is the same field-measured switch gap the other two
        // deferrals use (B-8 v2 measured 52-71 ms). If the field ever shows a
        // switch slow enough to still flicker, THIS is the number to raise -
        // gateHoldFrames_ (~0.5 s) exists because a switch can outlast the
        // 6-frame visual grace, so a slower gap is a known possibility.
        //
        // Cost of the window on a REAL exit: the weapon stays in hand ≤85 ms
        // before the sheathe starts, under the menu-close fade. That is the
        // same trade F-15 r35 and B-8 v2 both took, and the lesser residual.
        // DIAGNOSTIC (2026-07-20). The 12:18-12:20 field log shows this debt
        // pending for 2.2 s across a close, then cancelled by the next open,
        // with NO `sheathing (restore)` line anywhere in six menu sessions -
        // so the player walked out of every one of them still drawn. Both
        // halves read correct in isolation (Disarm sets the flag at :423 then
        // clears armedLastFrame_ at :509; this runs every frame from the
        // driver), so name the guard that actually blocks instead of reasoning
        // about it a second time. On CHANGE only - this runs every frame.
        {
            const int reason =
                !pendingWeaponRestore_                                              ? 0
                : (!a_force && armedLastFrame_)                                     ? 1
                : (!a_force && QpcSeconds(weaponRestoreQpc_, QpcNow()) < 0.085f)    ? 2
                                                                                    : 3;
            static int s_lastReason = -1;
            if (reason != s_lastReason) {
                s_lastReason = reason;
                if (reason != 0) {  // "nothing pending" is the common case, stay silent
                    spdlog::debug("weapon diag: deferred restore {} (force={} armed={} debt={})",
                                  reason == 1   ? "BLOCKED by armedLastFrame_"
                                  : reason == 2 ? "waiting out the 0.085s switch window"
                                                : "FIRING NOW",
                                  a_force, armedLastFrame_, WeaponPreview::HasDebt());
                }
            }
        }
        if (!pendingWeaponRestore_) {
            return;
        }
        // Gated on "are we previewing right now", NOT on "is a menu open".
        // Review caught two cases the menu-count version got wrong, both where
        // a menu is still OPEN but the bubble is dormant: the Souls unpause
        // path and Tick's missing-3D path both Disarm with the menu up, and a
        // menu-count guard held the sheathe for the whole remaining menu
        // session - leaving a force-drawn weapon through live unpaused
        // gameplay, and breaking the forgive-on-dead-3D path Restore()
        // documents. armedLastFrame_ is the honest question: a re-open cancels
        // this before it can ever be reached with the preview live again.
        if (!a_force && armedLastFrame_) {
            return;
        }
        if (!a_force && QpcSeconds(weaponRestoreQpc_, QpcNow()) < 0.085f) {
            return;
        }
        pendingWeaponRestore_ = false;
        // Restore() forgives the debt when the player or their 3D is gone, so
        // this is safe on every teardown edge, not just the graceful ones.
        WeaponPreview::Restore(RE::PlayerCharacter::GetSingleton());
    }

    void Bubble::ForceReset() {
        menusOpen_ = 0;
        countedMenus_.clear();  // r19c: menus died with the load, no close events coming
        // The incoming save rebuilds the camera; a gameplay frame sampled
        // before the load describes the outgoing one.
        lastGameplay_ = {};
        preTween_ = {};
        raceMenuOpen_ = false;
        // The studio session goes with the menus. CLEARED rather than drained:
        // ForcePause::Reset drops its own bookkeeping for the same reason - the
        // engine's pause counter came back with the save, so there is nothing
        // left to hand back and a release here would decrement somebody else's
        // count.
        studioEntered_ = false;
        // ⚠ CLEARED WITHOUT PAYING, and for the same reason the ledger above is.
        // The movie that carried the alpha died with the save; the HUD that
        // comes back is a new one at its own default. Writing 100 into it would
        // be handing back something we are not holding, and leaving the flag set
        // would make the next session's payer do exactly that.
        hidHud_ = false;
        studioGateHeld_ = false;
        studioPauses_.Clear();
        studioEnterCount_ = 0;
        // ⚠ THE PROOF IS PER GAME SESSION, AND THIS IS WHERE THE SESSION ENDS.
        // A player who loads a save with Fitting Room uninstalled must get their
        // bubble back rather than a gate armed by a mod that is no longer there
        // to lift it. So both the live set and the proof go.
        ownerContexts_.clear();
        ownerContextProven_ = false;
        ItemPreviewBroker::Drain();
        // F-26: the player 3D dies with the load - drop the restore debt rather
        // than trying to pay it against a stale actor. Reset() hard-clears the
        // debt, so the deferred sheathe must go with it or it would fire into
        // the new game against a weapon this preview never drew.
        WeaponPreview::Reset();
        // Drops state WITHOUT reverting, deliberately: the camera stack belongs
        // to the incoming save and the node whose transform we recorded died
        // with the old one, so putting the old numbers back would write them
        // into somebody else's camera.
        StudioCamera::DropOnLoad();
        // Same reasoning one line up: the node it was watching died with the
        // old save, so every transform it is holding to compare against is
        // meaningless now. Close the watch rather than report a "move" that is
        // really a different camera.
        CameraCloseProbe::DropOnLoad();
        camOrbitDragging_ = false;
        camHoldDecided_ = false;
        camOwedLeftUp_ = false;
        camPanDragging_ = false;
        camPanTravel_ = 0.0f;
        pendingWeaponRestore_ = false;
        pendingOwnViewRestore_ = false;  // DropOnLoad below owns the globals
        pendingExternalViewReconcile_ = false;
        CancelDipIfActive();
        exitPhase_ = 0;  // r47: the exit machine is a pure hold - no fader state
        // B-7 v3: drop the spin state; the node dies with the load (if the
        // 3D is still up, un-compose so nothing spun survives into a save
        // made this frame - cheap and deterministic).
        if (spinBasisValid_) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (auto* root = player ? player->Get3D(false) : nullptr) {
                root->local.rotate = rootBaseRotate_;
            }
            spinBasisValid_ = false;
        }
        if (auto* horse = spinHorseRoot_.get()) {  // r54: un-spin the mount
            horse->local.rotate = horseBaseRotate_;
        }
        spinHorseRoot_.reset();
        ReleaseSpinCompanion();  // Open 3: the second subject goes with the load
        CompanionProbe::Reset();  // counters belong to the outgoing session
        // ⚠ MUST detach: the graph is destroyed and rebuilt across a load, and
        // the held manager reference is the only thing keeping the old one
        // alive for our sink to be removed from.
        CompanionLunge::Reset();
        spinTarget_ = 0.0f;
        spinYaw_ = 0.0f;
        // F-15: the load restores the save's own camera - put only the
        // process-global surfaces back (INI Settings survive loads).
        OwnView::DropOnLoad();
        // The graph is destroyed and rebuilt across a load, so the sink from the
        // old one is gone with it. Drop the flag so Install() re-attaches.
        AnimEventProbe::Reset();
        AnimEventProbe::SetArmed(false);
        // Same rebuild, and this one is not merely stale but DANGEROUS if left:
        // the cached hkbCharacter addresses can be recycled by the new graph,
        // and a stale match would tag another actor's clips as the player's.
        ClipProbe::Reset();
        EquipNotifyGate::SetArmed(false);
        airFrozenArm_ = false;
        preserveDirectionBitsArm_ = false;
        movingArm_ = false;
        graceFrames_ = 0;
        gateHoldFrames_ = 0;
        loggedUnpausedOnce_ = false;
        sessionDormant_ = false;
        sessionLiveOnly_ = false;  // r28g: dies with the session, like dormant
        armedLastFrame_ = false;
        // ⚠ PAY THE HALF THAT SURVIVES THE LOAD, THEN CLEAR. The park owns four
        // surfaces and only ONE of them belongs to the outgoing save: the pitch
        // is on the actor and dies with him, while freeRotation.x, toggleAnimCam
        // and freeRotationEnabled live on the process-global ThirdPersonState
        // and the FOV lives on PlayerCamera. Both of those outlive the load.
        //
        // This is exactly the split OwnView::DropOnLoad makes twenty lines up
        // ("Only the surfaces that survive a load"), and the park has to make it
        // too, because the park exists for the sessions OwnView does NOT own -
        // with a view mod covering the inventory OwnView never arms, so these
        // three latches are the only record that a debt exists at all.
        //
        // Dropping the lot was the first cut of this fix and it was wrong in the
        // dangerous direction: a quickload taken from inside a bubbled menu
        // delivers no close event, so nothing had paid yet, and clearing here
        // left toggleAnimCam TRUE with the only record of the old value gone.
        // The next session then captures the framed value as its own baseline
        // and restores it faithfully forever. That is the reported symptom,
        // manufactured by the repair.
        //
        // Below OwnView::DropOnLoad on purpose, the same park-writes-last order
        // Disarm uses.
        if (preMenuFreeRotValid_) {
            if (auto* tps = GetThirdPersonState()) {
                tps->freeRotation.x = preMenuFreeRot_;
                tps->toggleAnimCam = preMenuAnimCam_;
                tps->freeRotationEnabled = preMenuFreeRotEnabled_;
            }
        }
        if (preMenuFovValid_) {
            if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                camera->worldFOV = preMenuFov_;
            }
        }
        preMenuFreeRotValid_ = false;
        preMenuPitchValid_ = false;  // data.angle.x dies with the outgoing actor
        preMenuFovValid_ = false;
        // ⚠ THE LATCHES GO WITH THE READING, AND THIS IS THE OTHER PLACE THAT
        // MAY CLEAR THEM. Leaving captureTakenThisSession_ set across a load
        // would have the first menu of the new save refuse its own capture and
        // hand nothing back for the rest of that session.
        captureTakenThisSession_ = false;
        captureKeptLogged_ = false;
        // The debt watch is cleared HERE and nowhere else: it exists to survive
        // a teardown, so only the load boundary, which nothing survives, may
        // drop it. An unreported early spend crossing into a new save would
        // print a comparison against a camera that no longer exists.
        debtSpentEarly_ = false;
        atCloseViewValid_ = false;  // no close edge crosses a load boundary
        // Restore NOW, not via the next Disarm edge (there is none after
        // this): handles die with the load, form-side edits (cell lighting,
        // FSMP thresholds, CBPC flag) survive it and must not leak. The
        // backdrop's shader writes hit the SHARED model templates - they
        // outlive the load too.
        // F-12 exit discipline: a quickload never ramps - snap the
        // transition dead before the removes so nothing re-applies a
        // faded value on the next arm.
        Transition::Snap(0.0f);
        FaceNeutral::Drop();  // face data dies with the load - nothing to write back
        Declutter::RestoreAll();
        Backdrop::Remove();
        Backdrop::OccluderDrop();  // scene parent dies with the load - detach the resident occluder
        Settings::GetSingleton().liveStudioActive = false;  // r28, same reason as in Disarm
        StudioRig::Remove();
        StudioLight::Restore();
        SceneTint::Restore();  // guarded: no-op unless the colour filter was on
        FsmpDrive::SetRotationFreedom(false);
        CbpcDrive::SetSimulateWhilePaused(false);
        FootIkGate::SetSuppressed(false);
        // Forced-pause bookkeeping only: menu instances (and their flags)
        // die with the load; the engine owns the counter either way.
        ForcePause::Reset();
        spdlog::info("ForceReset (load/new game).");
    }

    void Bubble::ReconcileExternalViewAfterClose() {
        using namespace ExternalViewExitPolicy;

        if (!pendingExternalViewReconcile_) {
            return;
        }
        if (!ShouldReconcile({
                .pending = true,
                .menuOpen = menusOpen_.load() > 0,
            })) {
            // A new covered menu won the close->open race. Its camera provider
            // now owns the targets; applying the previous menu's exit here
            // would disturb the newly framed view.
            pendingExternalViewReconcile_ = false;
            spdlog::debug("external view: post-close reconcile cancelled (menu switch).");
            return;
        }

        pendingExternalViewReconcile_ = false;
        if (auto* camera = RE::PlayerCamera::GetSingleton()) {
            if (auto* state = camera->currentState.get();
                state && state->id == RE::CameraState::kThirdPerson) {
                auto* tps = static_cast<RE::ThirdPersonState*>(state);
                const auto fields = ChooseReconcileFields();
                if (fields.zoom) {
                    tps->currentZoomOffset = tps->targetZoomOffset;
                }
                if (fields.yaw) {
                    tps->currentYaw = tps->targetYaw;
                }
                spdlog::debug("external view: reconciled restored camera after close sinks.");
            }
        }
    }

    bool Bubble::GateHoldsStudioDown() const {
        const auto& s = Settings::GetSingleton();
        return StudioSessionPolicy::HoldsStudioDown({
            .waitForOwnerContext = s.waitForOwnerContext,
            .ownerContextProven  = ownerContextProven_,
            .anyOwnerContextLive = !ownerContexts_.empty(),
        });
    }

    // Everything the bubble OWNS comes up here. Called from the menu open when
    // the gate is not holding, and from the per-frame reconcile when an owner
    // publishes into a menu that is already up.
    //
    // ⚠ THE ORDER IS THE OPEN EVENT'S OWN ORDER, and it is load bearing.
    // Force-pause goes FIRST so the pause is live before anything below reads
    // IsBubbleActive(), which is what makes the whole arm path behave like a
    // vanilla paused menu (§3.1). The cut to black then happens in this same
    // call stack, before the frame renders, which is the r25 lesson: mutate
    // before the frame renders and there is no frame to see.
    void Bubble::EnterStudioSession(const std::string& a_menuName, const char* a_why) {
        // ⚠ THE PAUSE IS PER COUNTED MENU AND THE SCENE IS PER SESSION, AND
        // CONFLATING THE TWO DROPS A TAKE. Two counted menus can overlap - an
        // open event can beat the outgoing menu's close - and before the studio
        // session existed every one of those opens ran EnsurePaused for itself.
        // A plain "already entered, nothing to do" here would silently skip the
        // second menu, and under Skyrim Souls the outgoing close would then find
        // the covered set empty, let the linger elapse, and unfreeze the world
        // with a menu still on screen.
        //
        // So the take runs on every call, over every menu we have counted.
        // EnsurePaused is idempotent per menu on its own side and the ledger is
        // a set, so a repeat costs nothing and owes exactly one release.
        //
        // §3.1: force-pause FIRST, before anything below reads IsBubbleActive(),
        // so the whole arm path behaves like a vanilla paused menu.
        //
        // ⚠ RECORDED AS THE ASK, NOT AS THE OUTCOME. EnsurePaused declines on
        // its own terms (force-pause off, a Souls-live menu, a menu that already
        // pauses itself) and ReleaseFor is a no-op on a name nothing was taken
        // for, so recording the ask cannot over-pay - and it cannot be
        // re-derived later from settings the player can change mid-menu, which
        // is the whole point of the ledger.
        // Owner-live sessions (SetOwnerWorldLive) skip the take outright: no
        // take, no ledger entry, nothing owed at Leave. The empty ledger is
        // also what keeps PauseStillLanding from waiting on a pause nobody
        // asked for - ForcePause::Holding() stays false.
        if (OwnerWorldLiveWanted()) {
            spdlog::info(
                "studio session: an owner asked for a LIVE session: no studio "
                "force-pause taken for {} counted menu(s). bFreezeCharacter is "
                "still obeyed literally; physics is driven either way.",
                countedMenus_.size());
        } else {
            for (const auto& counted : countedMenus_) {
                ForcePause::EnsurePaused(counted);
                studioPauses_.RecordTaken(counted);
            }
        }
        if (studioEntered_.load()) {
            return;  // the scene is already up; only the take above was owed
        }
        studioEntered_ = true;
        const auto& settings = Settings::GetSingleton();
        (void)ItemPreviewBroker::SetClaim(
            "MenuStudio.Settings",
            ItemPreviewPolicy::LocalClaimWanted(settings.enabled,
                                                menusOpen_.load() > 0, true,
                                                settings.disableItemPreview3D));
        ItemPreviewBroker::Reconcile();
        ++studioEnterCount_;
        // ⚠ A FRESH ARM DECISION, AND THIS IS THE ONLY THING THAT EVER LIFTS
        // THE DORMANT LATCH. r18 latches the arm decision per session so an
        // incidental pause change cannot rebuild the scene ten times, and that
        // is untouched - what has changed is that "session" is the STUDIO
        // session, and a menu can hold more than one of those in a row.
        sessionDormant_     = false;
        loggedUnpausedOnce_ = false;
        // ⚠ AND THE ARM DECISION IS GIVEN TIME TO SEE THE PAUSE WE ARE ABOUT TO
        // ASK FOR. A mid-menu Enter runs inside OnFrame, upstream of the arm
        // decision in the SAME frame, and ShadowPause's take is a UI queue post
        // that the engine publishes a frame later. Without this window the
        // decision reads an unpaused world one line after the take and latches
        // the session dormant, which is one way - so the studio session ends
        // before its first tick. Field 2026-08-13.
        studioPauseSettleFrames_ = StudioSessionPolicy::kPauseSettleFrames;
        // r28b: baseline the toggle watcher against what THIS studio session is
        // deciding with, so its first frame cannot read the previous session's
        // value as a fresh user toggle and re-decide a session that just began.
        lastForcePause_ = Settings::GetSingleton().forcePause;
        spdlog::info("studio session ENTER #{} in {} ({}), holding {} force-pause "
                     "take(s), the camera and the scene.",
                     studioEnterCount_, a_menuName, a_why, studioPauses_.Size());
        // ⚠⚠ THE COMPASS AND THE CROSSHAIR, AND ONLY OVER A MENU THE ENGINE DOES
        // NOT ALREADY HANDLE. The four vanilla menus put the game in menu mode
        // and the HUD stands itself down, so this would be a no-op there and a
        // behaviour change nobody asked for if it ever stopped being one. A
        // third-party menu that draws its own UI (Grid Inventory) does not, and
        // the field saw the compass and crosshair sitting over the void with the
        // studio armed (2026-08-27).
        //
        // ⚠ AN ALPHA WRITE, NOT A MESSAGE. See SetMenuRootAlpha at the top of
        // this file for what the kHide this replaced actually did to HUDMenu.
        // Recorded in hidHud_ rather than re-derived, because the predicate can
        // change while the session runs.
        //
        // ⚠⚠ PAID BEFORE IT IS RE-TAKEN, AND THAT LINE IS THE BUG THIS COST.
        // What stood here was a bare `hidHud_ = false`, which is an eraser
        // asking a different question from the mark: three routes end a studio
        // session by writing studioEntered_ = false directly, so a session that
        // hid the HUD could be followed by a fresh Enter that simply forgot the
        // debt. Field 2026-08-28: hidden at 19:29:26.377, forgotten by the next
        // Enter at 19:30:53, and the player's compass never came back.
        // RestoreHudIfHidden is idempotent and no-ops when nothing is owed, so
        // this costs nothing in the ordinary case.
        RestoreHudIfHidden("a new studio session is about to take the HUD");
        if (Settings::GetSingleton().hideHudOverCustomMenus &&
            !StudioSessionPolicy::EngineHidesHud(a_menuName)) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME)) {
                SetMenuRootAlpha(RE::HUDMenu::MENU_NAME, 0.0);
                hidHud_ = true;
                spdlog::info("studio session: '{}' leaves the HUD up, so the compass and "
                             "crosshair are hidden for this session.",
                             a_menuName);
            }
        }
        // ⚠ ARMED AT THE SESSION EDGE, NOT IN THE PAUSED TICK. The tick that
        // used to do it needs the pause, which lands a few frames later, and
        // AddOrbit drops everything while unarmed - so a drag begun on the very
        // first frame was arriving and being thrown away one layer down.
        // Nothing is written to the camera by this; that still waits for a real
        // input. Idempotent, so the tick's own call is harmless.
        StudioCamera::Arm();
        // Per-menu SPACE (NymerethRole): the backdrop/void is opt-in per menu,
        // so resolve the EFFECTIVE mode for the menu this session is entering.
        // Everything else about the bubble (pause, physics, the live character)
        // is unchanged - only the space steps aside, and only for menus the user
        // left out of sSpaceMenus. Setting the effective value here means all
        // ~30 existing declutterMode / IsVoidFamily consumers get the per-menu
        // answer with no changes.
        //
        // ⚠ THIS COMMENT USED TO CLAIM "restored to the configured value at
        // Disarm" AND NOTHING HAS EVER DONE THAT. The effective value is only
        // ever written here, so a menu that opted out of the space leaves a 0
        // standing until the next session resolves it again. Harmless today -
        // every consumer is gated on the bubble being up - and left alone on
        // purpose rather than fixed in passing, but not worth carrying a false
        // sentence about.
        if (auto& s = Settings::GetSingleton();
            s.declutterModeIni != 0 && !s.MenuWantsSpace(a_menuName)) {
            if (s.declutterMode != 0) {
                spdlog::info("space: {} is not in sSpaceMenus, opening without "
                             "the backdrop (pause + physics unchanged).", a_menuName);
            }
            s.declutterMode = 0;
        } else {
            s.declutterMode = s.declutterModeIni;
        }
        // F-15 r35: a re-open inside the switch gap cancels the pending restore
        // - the framing never came down, the switch is seamless
        // (ArmOwnViewIfOurs no-ops while OwnView stays active; it still frames
        // the new menu when the previous one wasn't ours).
        if (armedLastFrame_) {
            if (pendingOwnViewRestore_) {
                pendingOwnViewRestore_ = false;
                spdlog::debug("own view: restore cancelled (menu switch), framing stays.");
            }
            ArmOwnViewIfOurs();
        }
        // F-12 v3 + declutter, in the SAME call stack, before this frame
        // renders. The r24 timed dip showed the world for a few frames under
        // the menu UI (field: "when we see the inventory UI it should already
        // have been faded in") - the cut to black happens HERE, so the first
        // rendered frame is already dark and the cull swap hides behind it; the
        // first armed tick builds the studio and starts the fade-in.
        if (IsBubbleActive() && Settings::GetSingleton().declutterMode != 0) {
            const auto& s = Settings::GetSingleton();
            // r40: the close edge hands the sky mode back so weather audio
            // survives the exit's unpaused window - a switch re-open re-parks
            // it (idempotent for fresh arms; Apply's own park then no-ops).
            // ⚠ THE EDITOR IS NO LONGER CARVED OUT HERE OR BELOW. Both
            // carve-outs were written while the editor could never be bubbled
            // at all (its name was misspelled everywhere), so neither has ever
            // run with raceMenuOpen_ actually true. They encode "the editor
            // gets less studio", which is exactly what the field asked us to
            // stop doing: without the sky parked the world shows through the
            // void that was just enabled for it.
            if (s.CellLightAllowed()) {
                StudioLight::ReparkSkyMode();
            }
            // BLUE-VOID FLASH - DEFINITIVE FIX (RE Option P). The world-feeder
            // cull is immediate, but a freshly-attached shell can't occlude
            // until the NEXT frame (one-frame publish latency of new geometry
            // into the batch renderer - attaching earlier does NOT help; RE-
            // confirmed). Un-culling an already-RESIDENT node, however, is
            // render-side and immediate (the mod's own world-culls prove it).
            // So a persistent gap occluder is kept resident + AppCulled and
            // UN-CULLED here, before the frame renders: when it is warm it
            // blocks the world the SAME frame the cull drops it → gap-free, so
            // cull now. When it had to build cold (first open per session /
            // after a cell change) it lags one frame like any fresh node, so
            // keep the no-fader interim (defer the cull to the Tick - a brief
            // real-world frame, never blue); it is warm from the next open.
            // (No fader - that captures menu input, HANDOFF l.14.) `Warm()`
            // preheats the real shell so it draws in promptly behind the
            // occluder. A menu SWITCH keeps the previous occluder up → no gap.
            if (!armedLastFrame_ && s.IsVoidFamily()) {
                Backdrop::Warm();
                const bool occWarm = Backdrop::OccluderShow();
                spdlog::debug("open: gap occluder {}, cull {}.",
                              occWarm ? "WARM (same-frame, gap-free)" : "cold (interim)",
                              occWarm ? "now" : "deferred to Tick");
                if (occWarm) {
                    Declutter::Refresh();
                }
            } else {
                Declutter::Refresh();
            }
        }
    }

    // The studio comes down while its menu stays up. Everything Enter took is
    // handed back; nothing the MENU session owns is touched, and in particular
    // menusOpen_ and countedMenus_ are not.
    void Bubble::LeaveStudioSession(const char* a_why) {
        if (!studioEntered_.load()) {
            return;
        }
        // ⚠ FALSE BEFORE THE TEARDOWN RUNS, NOT AFTER. IsBubbleActive() reads
        // this, and Disarm's callees ask it; a teardown that still answered
        // "the bubble is up" would have the strip and the mouse gates live for
        // the frames it takes to come down.
        studioEntered_ = false;
        (void)ItemPreviewBroker::SetClaim("MenuStudio.Settings", false);
        studioGateHeld_ = GateHoldsStudioDown();
        // ⚠⚠ EXACTLY WHAT THIS SESSION TOOK, FROM THE RECORD, NEVER RE-DERIVED.
        // The settings panel is drawn inside the menu this session is running
        // in, so the predicate that decided the take can have changed under us
        // - that is r19c, and it shipped once already. The ledger is drained,
        // so a second Leave in the same session pays nothing.
        const auto owed = studioPauses_.Drain();
        for (const auto& menu : owed) {
            ForcePause::ReleaseFor(menu);
        }
        // The instant cut, the same one the zero-frame sleek exit uses at the
        // close edge. There is no dissolve to run here: the menu is not going
        // anywhere, so there is no exit choreography to hide behind and a fade
        // would just be a slow reveal of the room.
        Transition::Snap(0.0f);
        if (pendingOwnViewRestore_) {
            pendingOwnViewRestore_ = false;
            OwnView::Disarm();
        }
        Disarm();  // its own last statement clears armedLastFrame_
        // The other half of the HUD hide, and only when this session took it.
        //
        // ⚠⚠ THIS IS NO LONGER THE ONLY PAYER AND IT NEVER SHOULD HAVE BEEN.
        // LeaveStudioSession is reached from ReconcileStudioSession alone, so
        // in a shipped configuration with bWaitForOwnerContext off it can go a
        // whole play session without running: the field log for 2026-08-28 has
        // seven ENTERs and not one LEAVE. Every route that ends a session pays
        // this now; see RestoreHudIfHidden.
        RestoreHudIfHidden("the studio session left");
        spdlog::info("studio session LEAVE (pair #{}, {}), handed back {} force-pause "
                     "hold(s); the menu stays open and vanilla.",
                     studioEnterCount_, a_why, owed.size());
    }

    // Give the HUD back, once, from the record. Idempotent: a caller that owes
    // nothing pays nothing, so every route that ends a studio session can call
    // this unconditionally and none of them has to know what the others did.
    //
    // ⚠⚠ THE RECORD IS THE ONLY INPUT. Not the settings, not EngineHidesHud,
    // not which menu is up now - the settings panel is drawn inside the menu
    // whose session did the hiding and the player can change either mid-menu.
    // Same doctrine as the pause ledger's Drain(), and for the same reason.
    //
    // ⚠ THE HUDMenu OPEN CHECK IS AN INSTRUMENT, NOT A GATE. The write is a
    // no-op on a menu that has gone, so the answer changes nothing; it is
    // logged because "the HUD had already left the stack when we came to give
    // it back" is the one reading that would send this back to the drawing
    // board, and no field run has produced it yet.
    void Bubble::RestoreHudIfHidden(const char* a_why) {
        if (!hidHud_) {
            return;
        }
        hidHud_ = false;
        auto* ui = RE::UI::GetSingleton();
        const bool stillOpen = ui && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME);
        SetMenuRootAlpha(RE::HUDMenu::MENU_NAME, 100.0);
        spdlog::info("studio session: the compass and crosshair are handed back ({}). "
                     "HUDMenu was {} on the stack.",
                     a_why, stillOpen ? "still" : "NO LONGER");
    }

    void Bubble::ReconcileStudioSession() {
        // ⚠ THE CACHE IS WRITTEN EVERY FRAME, NOT ONLY ON A MOVE, and it holds
        // the RAW gate answer. The action bar reads it off-thread to decide
        // whether to draw its weaker form, and a value that only refreshed on
        // transitions would be stale for exactly the frames in between. The
        // menu count and the entered flag are applied by the reader
        // (StudioHeldForOwnerContext) rather than folded in here, so there is
        // one meaning for this field and not two.
        const bool holds = GateHoldsStudioDown();
        studioGateHeld_ = holds;
        using Move = StudioSessionPolicy::Move;
        switch (StudioSessionPolicy::ChooseMove(menusOpen_.load() > 0,
                                                studioEntered_.load(), holds)) {
        case Move::kEnter:
            EnterStudioSession(currentMenuName_, "an owner published a context");
            break;
        case Move::kLeave:
            LeaveStudioSession("the last owner withdrew");
            break;
        case Move::kNothing:
            break;
        }
    }

    bool Bubble::SetOwnerContext(const char* a_ownerId, bool a_active) {
        if (!a_ownerId || !*a_ownerId) {
            spdlog::warn("owner context: a call was refused (no owner id). Nothing "
                         "changed.");
            return false;
        }
        auto&             self = GetSingleton();
        const std::string id{ a_ownerId };
        if (a_active) {
            // ⚠ THE FIRST LIVE CONTEXT OF THE GAME SESSION IS THE PROOF, and it
            // is taken whether or not the setting is on. A player who turns
            // bWaitForOwnerContext on AFTER their outfit editor has already
            // opened once gets a gate with teeth immediately, rather than one
            // that waits for another round trip to believe in itself.
            if (!self.ownerContextProven_) {
                self.ownerContextProven_ = true;
                spdlog::info("owner context: '{}' is the first owner to publish one this "
                             "game session; bWaitForOwnerContext is armed from here "
                             "(it does nothing until something proves it can lift the "
                             "studio again).",
                             id);
            }
            if (self.ownerContexts_.insert(id).second) {
                spdlog::info("owner context: '{}' opened ({} live).", id,
                             self.ownerContexts_.size());
            }
            return true;
        }
        if (self.ownerContexts_.erase(id) != 0) {
            spdlog::info("owner context: '{}' closed ({} live).", id,
                         self.ownerContexts_.size());
        }
        return true;
    }

    bool Bubble::SetOwnerWorldLive(const char* a_ownerId, bool a_live) {
        if (!a_ownerId || !*a_ownerId) {
            spdlog::warn("owner world-live: a call was refused (no owner id). Nothing "
                         "changed.");
            return false;
        }
        auto&             self = GetSingleton();
        const std::string id{ a_ownerId };
        if (a_live) {
            if (self.ownerWorldLive_.insert(id).second) {
                spdlog::info("owner world-live: '{}' asks its sessions to keep the "
                             "world running (no force-pause, no dormant latch).",
                             id);
            }
        } else if (self.ownerWorldLive_.erase(id) != 0) {
            spdlog::info("owner world-live: '{}' withdrew the wish; its next session "
                         "pauses as before.",
                         id);
        }
        return true;
    }

    bool Bubble::OwnerWorldLiveWanted() {
        auto& self = GetSingleton();
        if (self.ownerWorldLive_.empty() || self.ownerContexts_.empty()) {
            return false;
        }
        for (const auto& id : self.ownerWorldLive_) {
            if (self.ownerContexts_.contains(id)) {
                return true;
            }
        }
        return false;
    }

    bool Bubble::CharacterFrozen() {
        return Settings::GetSingleton().freezeCharacter && !ReposeActive();
    }

    bool Bubble::ReposeActive() { return GetSingleton().reposeFrames_ > 0; }

    bool Bubble::UiStillHoldsACountedMenu() const {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            // ⚠ NO UI MEANS NO OPINION, AND NO OPINION MUST NOT READ AS "GONE".
            // The other witness (menusOpen_) still gets its say at the call
            // site; answering false here is that witness standing alone, which
            // is the pre-fix behaviour and the safe half of it.
            return false;
        }
        for (const auto& counted : countedMenus_) {
            if (ui->IsMenuOpen(counted)) {
                return true;
            }
        }
        // The counted set is cleared by the r19c self-heal and by the close
        // handler, either of which can run ahead of the teardown that asks this.
        // The name survives both, so it is the last witness standing.
        return !currentMenuName_.empty() && ui->IsMenuOpen(currentMenuName_);
    }

    void Bubble::Disarm() {
        // The re-settle is a WHILE-ARMED question, so both halves die with the
        // arm: out in the world the engine steps the graph itself, and the
        // next arm's first sighting must read as a first sighting rather than
        // as a swap.
        lastArmedPlayerRoot_ = nullptr;
        reposeFrames_        = 0;
        // Unconditional: a dip can be live before the first armed tick
        // (open event cuts to black; an unpause race or missing player 3D
        // disarms without ever arming) - the screen must never stay dark.
        CancelDipIfActive();
        // Before anything else touches the camera. StudioCamera reverts the
        // node it wrote, and OwnView's own restore further down assumes it is
        // putting back a camera nobody else is still holding.
        StudioCamera::Disarm();
        camOrbitDragging_ = false;
        camHoldDecided_ = false;
        // ⚠ EVERY DRAG LATCH DIES WITH THE MENU, and the pan one earned this
        // line the hard way: held the middle button, closed the menu, let go
        // outside it, and the release never reached this sink - so the next
        // menu opened already panning and every mouse move dragged the shot
        // with nothing held. A latch that outlives the thing it was taken in
        // is a stuck input, whichever button it belongs to.
        camPanDragging_ = false;
        camPanTravel_ = 0.0f;
        // ⚠ THE OTHER END OF THE SPIN PARK, WHICH NEVER EXISTED. The park
        // adopts the framed view and re-asserts it every frame while the menu
        // is up; nothing ever handed it back. OwnView saves and restores this
        // field too, which is why it looked covered - but OwnView only ARMS
        // when no view mod already frames the menu, and Show Player In
        // Inventory frames the inventory outright. So an inventory session
        // parked the rotation with nobody holding the value it started at, and
        // the editor session that followed captured the leftover as ITS
        // original and wrote it back faithfully.
        //
        // Measured 2026-08-05: tick 1 read -0.02, tick 62 read 2.64, and it
        // was still 2.64 across later menus and a gameplay stretch, reaching
        // 4.20. At those magnitudes the camera sits 150 to 240 degrees off the
        // character's facing, which is the field report that movement stops
        // following the camera.
        //
        // ⚠ AFTER StudioCamera::Disarm AND AFTER OwnView's restore, on
        // purpose. Both of those write the camera and the last writer is what
        // the engine rebuilds from. The policy's ownership test is what keeps
        // this from trampling a view mod that got there first.
        // ⚠ THE OWNERSHIP TEST IS GONE, AND THE FIELD IS WHY. It asked whether
        // the live value still sat on our park before writing, so as not to
        // trample a view mod that had restored first. The measurement killed
        // it: the player ORBITS during the editor, which moves freeRotation
        // off the park by design, so the test read "somebody else owns this"
        // for the one case the restore exists to cover and declined
        // ("freeRotation left at 0.512, parked 2.642"). It was protecting
        // against a conflict that cannot arise anyway - a view mod's copy and
        // ours are both the pre-menu value, so whoever writes last writes the
        // same number.
        //
        // ⚠ AND IT IS THE WHOLE STATE NOW, NOT TWO NUMBERS. The first cut
        // restored freeRotation.x and the pitch and left toggleAnimCam and
        // freeRotationEnabled behind, which is most of what the player feels:
        // animCam locks the camera to the animation, which is the reported
        // "can't look up and down". The run through the console, which the
        // field says behaves correctly, carries animCam=false throughout; the
        // run through our own button carries animCam=true at both ends.
        //
        // Nothing here can be the player's own doing: a menu holds their input
        // for the whole session, so every difference between the value we
        // found and the value at teardown was written by a framing.
        // Read BEFORE the park consumes them. These three latches are the
        // session's own "there was something to hand back" flag, and they are
        // cleared thirty lines below - so this reads true exactly once per
        // session, on the frame the teardown actually pays. Disarm() itself runs
        // every frame while no menu is open, which is why the signal needs an
        // edge and cannot just be "Disarm ran".
        const bool parkOwedSomething =
            preMenuFreeRotValid_ || preMenuPitchValid_ || preMenuFovValid_;
        // ⚠⚠ THE BEFORE AND AFTER FOR THE THREE FIELDS NOTHING HANDS BACK, and
        // it is the whole reason a diagnostic build exists. The 2026-08-31
        // report is combat-only, Tab-only, and describes a fast zoom, which is
        // three different mechanisms wearing one symptom. This prints the
        // reading's context beside the live values at the moment of payment, so
        // one line says which of them actually moved.
        //
        // ⚠ DIAGNOSTIC BUILD ONLY. Once per menu session is cheap, but this is
        // an answer to one open report and not a line the shipped log needs
        // carrying forever. The always-on capture line above already names the
        // context at the reading; this is the other end of it.
#ifdef MENUSTUDIO_DIAG
        if (parkOwedSomething) {
            auto* slot = ThirdPersonSlotState();
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const auto* camState = cam ? cam->currentState.get() : nullptr;
            const auto stanceNowDiag = WeaponStance::Read(pc);
            spdlog::info(
                "camera park DIAG: '{}' paying. zoom {:.2f}->{:.2f} at the "
                "reading vs {:.2f}->{:.2f} live (NOTHING hands this back). "
                "Camera {} at the reading vs {} live. Combat {} -> {}. Stance "
                "{} -> {}. Tween menu was {} when the reading was taken.",
                currentMenuName_.empty() ? "(unnamed)" : currentMenuName_,
                preMenuZoomCur_, preMenuZoomTgt_,
                slot ? slot->currentZoomOffset : 0.0f,
                slot ? slot->targetZoomOffset : 0.0f,
                CameraStateName(static_cast<RE::CameraState>(preMenuCameraState_)),
                camState ? CameraStateName(camState->id) : "(none)",
                preMenuInCombat_ ? "IN COMBAT" : "out",
                pc && pc->IsInCombat() ? "IN COMBAT" : "out",
                preMenuStanceKnown_ ? (preMenuWeaponDrawn_ ? "drawn" : "sheathed")
                                    : "unknown",
                stanceNowDiag.known ? (stanceNowDiag.drawn ? "drawn" : "sheathed")
                                    : "unknown",
                preMenuViaTween_ ? "UP (Tab route)" : "closed");
        }
#endif
        // ⚠ THE SLOT OBJECT, NOT THE CURRENT STATE. GetThirdPersonState()
        // returns null the moment the player switches POV, and this payment
        // used to be gated on it while the latches below were cleared
        // unconditionally - a POV toggle inside the ~150 ms deferred-exit
        // window destroyed the debt unpaid, permanently. The fields live on
        // the pooled state object either way; write them there.
        //
        // ⚠ AND EVERY FIELD PASSES THE STALE-PARK GUARD FIRST. A field that
        // moved between the close edge and this payment was restored by its
        // owner (the view mod that framed the menu; measured SPII at +110 ms
        // against this payment at +149 ms on the deferred exit) - re-writing
        // our copy on top of that was the 2026-08-11 pitch lock. The guard's
        // reasoning and its difference from the REMOVED ownership test live
        // with ParkMayRestore in RotationOwnershipPolicy.h.
        if (preMenuFreeRotValid_) {
            if (auto* tps = ThirdPersonSlotState()) {
                namespace P = RotationOwnershipPolicy;
                const bool payFreeRot = P::ParkMayRestore(
                    atCloseViewValid_, atCloseFreeRotX_, tps->freeRotation.x,
                    P::kPitchEpsilon);
                // ⚠⚠ THE TWO FLAGS ASK A DIFFERENT QUESTION FROM THE NUMBER
                // BESIDE THEM. freeRotation.x is a saved value and the
                // stale-park guard is the right test for it. animCam and
                // freeRotEnabled are derived by the engine from whether the
                // weapon is out, so the question is not "did somebody else
                // restore this" but "is the stance I read them in still the
                // stance the player is standing in". Full reasoning with
                // ChooseStanceFlags in RotationOwnershipPolicy.h.
                const auto stanceNow = WeaponStance::Read(
                    RE::PlayerCharacter::GetSingleton());
                const auto stancePlan = P::ChooseStanceFlags({
                    .haveCapture = true,
                    .stanceKnownAtCapture = preMenuStanceKnown_,
                    .drawnAtCapture = preMenuWeaponDrawn_,
                    .stanceKnownNow = stanceNow.known,
                    .drawnNow = stanceNow.drawn,
                });
                const bool derive = stancePlan == P::StanceFlags::kEngineDerived;
                // The stale-park guard still owns the SAME-STANCE path, which
                // is every path that was already healthy.
                const bool payFreeRotEnabled =
                    !derive && P::ParkMayRestore(atCloseViewValid_,
                                                 atCloseFreeRotEnabled_,
                                                 tps->freeRotationEnabled);
                if (tps->freeRotation.x != preMenuFreeRot_ ||
                    tps->toggleAnimCam != P::kAnimCamHandBack ||
                    tps->freeRotationEnabled != preMenuFreeRotEnabled_) {
                    spdlog::info(
                        "rotation park: third-person state handed back: freeRot "
                        "{:.3f}->{:.3f}{}, animCam {}->{} (captured {}, never "
                        "restored), freeRotEnabled {}->{}{} (park was {:.3f}).",
                        tps->freeRotation.x, preMenuFreeRot_,
                        payFreeRot ? "" : " WITHHELD (owner restored it)",
                        tps->toggleAnimCam, P::kAnimCamHandBack, preMenuAnimCam_,
                        tps->freeRotationEnabled, preMenuFreeRotEnabled_,
                        payFreeRotEnabled ? "" : " WITHHELD (owner restored it)",
                        freeRotArm_);
                }
                if (payFreeRot) {
                    tps->freeRotation.x = preMenuFreeRot_;
                }
                // ⚠⚠ HANDED OFF, NOT HANDED BACK, AND DELIBERATELY OUTSIDE
                // EVERY GUARD ABOVE. The capture is not consulted and the
                // stale-park guard is not asked, because neither can produce a
                // better answer than "let the player look up and down" and both
                // were producing a worse one. Show Player In Menus reaches the
                // same conclusion in its own ResetCamera. Full reasoning with
                // kAnimCamHandBack in RotationOwnershipPolicy.h.
                tps->toggleAnimCam = P::kAnimCamHandBack;
                if (payFreeRotEnabled) {
                    tps->freeRotationEnabled = preMenuFreeRotEnabled_;
                }
                // ⚠ THE DERIVED WRITE IS NOT SUBJECT TO THE STALE-PARK GUARD,
                // on purpose. That guard stands aside for a value somebody else
                // restored, because their copy and ours were both the pre-menu
                // reading. This one is not a copy of anything - it is read off
                // the stance the player is in at this instant, so it is the
                // better answer whoever wrote last, and it has to be the last
                // writer to be worth anything.
                if (derive) {
                    const bool freeRot =
                        P::FreeRotationForStance(stanceNow.drawn);
                    spdlog::info(
                        "rotation park: STANCE CHANGED across the menu ({} at the "
                        "capture, {} now), so free rotation is re-derived instead "
                        "of handed back: freeRotEnabled {}->{} (the capture held "
                        "{}). It describes a stance the player is no longer in.",
                        preMenuWeaponDrawn_ ? "drawn" : "sheathed",
                        stanceNow.drawn ? "drawn" : "sheathed",
                        tps->freeRotationEnabled, freeRot,
                        preMenuFreeRotEnabled_);
                    tps->freeRotationEnabled = freeRot;
                }
            }
        }
        // ⚠ THE SECOND FAULT, AND IT WAS DELIBERATELY LEFT REPORTING FOR ONE
        // ROUND SO THE FIRST FIX COULD BE JUDGED ON ITS OWN. The reading came
        // back "entered at 0.058 and leaves at 0.100", 0.1 being the literal in
        // the Show Player In Inventory recipe, alongside the field report that
        // the camera cannot be moved up and down afterwards. So now it is
        // restored too.
        //
        // No ownership test against the SESSION, unlike the free rotation
        // above - a menu holds the player's input, so any drift across the
        // session was a framing's. But that stops at the close edge: on the
        // deferred exit the ~150 ms to this payment is live gameplay, where
        // the framing view mod restores its own pitch write and the player's
        // mouse is back. The stale-park guard covers both.
        if (preMenuPitchValid_) {
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                const float now = pc->data.angle.x;
                const auto  plan = RotationOwnershipPolicy::ChooseScalarRestore(
                    preMenuPitchValid_, preMenuPitch_, now,
                    RotationOwnershipPolicy::kPitchEpsilon);
                const bool mayPay = RotationOwnershipPolicy::ParkMayRestore(
                    atCloseViewValid_, atClosePitch_, now,
                    RotationOwnershipPolicy::kPitchEpsilon);
                if (plan.restore && mayPay) {
                    spdlog::info("rotation park: the player's pitch handed back "
                                 "{:.3f} -> {:.3f}.",
                                 now, plan.value);
                    pc->data.angle.x = plan.value;
                } else if (plan.restore) {
                    spdlog::info("rotation park: pitch WITHHELD (moved since the "
                                 "close, its owner restored it): live {:.3f}, "
                                 "at close {:.3f}, park held {:.3f}.",
                                 now, atClosePitch_, plan.value);
                }
            }
        }
        // ⚠ THE LAST OF THE SET, AND IT ARRIVED ONE ROUND AFTER THE OTHERS
        // because it was the only one the probe kept calling correct. That
        // verdict compared the FOV against the value at the EDITOR's open, and
        // the editor was entered on 60 already - the inventory framing had
        // written it and the probe's baseline inherited it. A measurement is
        // only as good as where it starts counting.
        if (preMenuFovValid_) {
            if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                const float now = camera->worldFOV;
                const auto  plan = RotationOwnershipPolicy::ChooseScalarRestore(
                    preMenuFovValid_, preMenuFov_, now,
                    RotationOwnershipPolicy::kFovEpsilon);
                const bool mayPay = RotationOwnershipPolicy::ParkMayRestore(
                    atCloseViewValid_, atCloseFov_, now,
                    RotationOwnershipPolicy::kFovEpsilon);
                if (plan.restore && mayPay) {
                    spdlog::info("rotation park: the field of view handed back "
                                 "{:.1f} -> {:.1f}.",
                                 now, plan.value);
                    camera->worldFOV = plan.value;
                } else if (plan.restore) {
                    spdlog::info("rotation park: FOV WITHHELD (moved since the "
                                 "close, its owner restored it): live {:.1f}, "
                                 "at close {:.1f}, park held {:.1f}.",
                                 now, atCloseFov_, plan.value);
                }
            }
        }
        if (parkOwedSomething) {
            // The last writer in the teardown. StudioCamera::Disarm, OwnView's
            // restore and the park above have all run by here, so anything the
            // camera shows after this frame is the engine rebuilding from what
            // we left - which is the only thing the verdict is entitled to
            // grade. Before this signal existed the verdict fired on a fixed
            // one-second timer and could land mid-restore on any exit route
            // slower than the default one.
            CameraCloseProbe::NoteTeardownDone("rotation park");
        }
        // ⚠⚠ THE CAPTURE ONLY DIES WITH ITS MENU, AND THAT IS THE FROZEN-CAMERA
        // FIX. This block used to clear the capture unconditionally. That is
        // correct at a close and wrong everywhere else, because Disarm is also
        // reached WITH THE MENU STILL OPEN - a settings save inside the panel
        // tears the session down mid-menu, so does the dormancy latch under an
        // unpaused menu, so does a frame with no player 3D, so does a switch.
        // Disarm also clears armedLastFrame_, so the next tick re-entered the
        // arm path, found the capture flags false, and READ THE CAPTURE BACK OFF
        // THE FRAMED CAMERA. Everything after that handed the menu's own numbers
        // to gameplay. Measured on this rig 2026-08-31 (the MagicMenu line
        // quoted in CameraDebtPolicy.h): the capture held fov 60 / pitch 0.100 /
        // animCam true, which is the framing, against a live 80 / 0.063 / false,
        // which is the player's.
        //
        // ⚠ THE COUNT IS NOT A SUFFICIENT WITNESS AND THE UI IS NOT EITHER, so
        // both are asked and either one is enough. The count can already read
        // zero with the menu on screen - the r19c self-heal reconciles it
        // against the UI, and the 2026-08-30 log has the teardown 1.77 s ahead
        // of the close event. The UI map can lag an open by a frame (the r14
        // race). Keeping a capture one frame past its close costs nothing,
        // because Disarm runs every frame out in the world and the next one
        // retires it; dropping one a frame early costs the player their camera
        // until they force a rebuild. The decision table is in
        // CameraDebtPolicy.h.
        const bool debtLive =
            preMenuFreeRotValid_ || preMenuPitchValid_ || preMenuFovValid_;
        // ⚠ THE UI QUESTION IS ASKED ONLY WHEN THERE IS SOMETHING TO DECIDE.
        // Disarm() runs EVERY FRAME while no menu is open, and on all of those
        // frames the answer cannot change the outcome: the policy returns
        // kNothingOwed on !haveCapture whatever the witnesses say. Asking anyway
        // would put a UI map lookup on the idle path for no decision at all.
        const bool uiHoldsAMenu = debtLive && UiStillHoldsACountedMenu();
        const auto teardown = CameraDebtPolicy::ChooseTeardown({
            .haveCapture = debtLive,
            .menusCounted = menusOpen_.load() > 0,
            .uiHoldsAMenu = uiHoldsAMenu,
        });
        const bool keepCapture = teardown == CameraDebtPolicy::Teardown::kPayAndKeep;
        if (keepCapture && !captureKeptLogged_) {
            captureKeptLogged_ = true;
            spdlog::info(
                "camera park: mid-menu teardown for '{}' - our count says {}, "
                "the UI says the menu is {}. The reading of the player's own "
                "camera is KEPT for the close to hand back; before 1.1.5 this "
                "threw it away and the re-arm read the framing instead. Session: "
                "entered {}, armed {}, dormant {}.",
                currentMenuName_.empty() ? "(unnamed)" : currentMenuName_,
                menusOpen_.load(), uiHoldsAMenu ? "STILL OPEN" : "gone",
                studioEntered_.load(), armedLastFrame_, sessionDormant_);
        }
        // ⚠ THE WATCH STAYS, AND ITS MEANING HAS CHANGED. It used to describe
        // the fault on every mid-menu teardown. Now the teardown above keeps the
        // capture on exactly those routes, so this can only fire when the
        // capture is being retired with a menu the UI still holds - which the
        // policy no longer produces. A line here is a route into Disarm that
        // clears the count AND the UI entry while the framing is live, and it
        // wants finding rather than absorbing.
        if (debtLive && !keepCapture) {
            auto*       camera = RE::PlayerCamera::GetSingleton();
            auto*       pc = RE::PlayerCharacter::GetSingleton();
            auto*       tps = ThirdPersonSlotState();
            const float liveFov = camera ? camera->worldFOV : 0.0f;
            const float liveFreeRot = tps ? tps->freeRotation.x : 0.0f;
            const float livePitch = pc ? pc->data.angle.x : 0.0f;
            const float liveZoom = tps ? tps->targetZoomOffset : 0.0f;
            debtShadowFov_ = preMenuFov_;
            debtShadowFreeRot_ = preMenuFreeRot_;
            debtShadowPitch_ = preMenuPitch_;
            debtShadowAnimCam_ = preMenuAnimCam_;
            debtShadowFreeRotEnabled_ = preMenuFreeRotEnabled_;
            debtSpentEarly_ = true;
            debtSpentQpc_ = QpcNow();
            spdlog::debug(
                "camera debt: RETIRED at the close of '{}' - our count says {}, "
                "the UI says the menu is {}. Session: entered {}, armed {}, "
                "dormant {}, studio camera active {}. Drag latches: orbit {}, "
                "owed-left-up {}, pan {}. Held vs live: fov {:.1f} vs {:.1f}, "
                "freeRot {:.3f} vs {:.3f}, pitch {:.3f} vs {:.3f}, animCam {} vs "
                "{}, freeRotEnabled {} vs {}. Zoom is live only ({:.2f}), the "
                "park never captured it, which is its own gap.",
                currentMenuName_.empty() ? "(unnamed)" : currentMenuName_,
                menusOpen_.load(), uiHoldsAMenu ? "STILL OPEN" : "gone",
                studioEntered_.load(), armedLastFrame_, sessionDormant_,
                StudioCamera::Active(), camOrbitDragging_.load(),
                camOwedLeftUp_.load(), camPanDragging_.load(), preMenuFov_,
                liveFov, preMenuFreeRot_, liveFreeRot, preMenuPitch_, livePitch,
                preMenuAnimCam_, tps ? tps->toggleAnimCam : false,
                preMenuFreeRotEnabled_,
                tps ? tps->freeRotationEnabled : false, liveZoom);
        }
        if (!keepCapture) {
            preMenuFreeRotValid_ = false;
            preMenuPitchValid_ = false;
            preMenuFovValid_ = false;
            atCloseViewValid_ = false;  // the close-edge sample dies with the debt
            freeRotParked_ = false;
            // Both latches belong to the capture and are cleared with it, so the
            // next menu session reads the player's camera afresh.
            captureTakenThisSession_ = false;
            captureKeptLogged_ = false;
        }
        // The companion latches are per ARM, not per companion. Keyed only on
        // her form id they would survive to the next menu, and a follower who
        // walked over to you in between would never get her settle - she would
        // simply resume the walk clip in place.
        g_companionSettled = 0;
        // OS-108: per ARM, like the settle latch it qualifies. A follower who
        // was walking when one menu opened must not be treated as walking by the
        // next one.
        g_companionMovingArm = false;
        g_companionTickLogged = false;
        // OS-104: give her headtracking back. Resolved from the handle we held,
        // not from whoever is framed now - see g_headtrackHeld. A dead handle
        // means she streamed out, and the graph variable went with her 3D.
        // The player hold's release on the way out. The tick's own release
        // covers a switch INTO the editor; this covers every other exit,
        // including straight back to gameplay - where head tracking is
        // ordinary gameplay state and leaving it off is a bug that outlives
        // the menu.
        ReleasePlayerHeadtracking();
        if (g_headtrackHeld) {
            if (const auto ptr = g_headtrackHeld.get(); ptr && ptr->Get3D()) {
                // The CAPTURED value, never a literal - see g_headtrackPrior,
                // and the same field the hold wrote rather than the graph
                // variable it used to write.
                auto* const st = ptr->AsActorState();
                if (st) {
                    st->actorState2.headTracking = g_headtrackPrior ? 1 : 0;
                }
                spdlog::info("companion look: '{}' 0x{:08X}: headtracking restored "
                             "to {} ({}).",
                             ptr->GetName(), ptr->GetFormID(),
                             g_headtrackPrior ? "on" : "off",
                             st ? "ok" : "NO ACTOR STATE: she is left held off");
            } else {
                // Streamed out with the hold on. The graph went with her 3D, so
                // a rebuild takes the record's default; nothing to put back.
                spdlog::info("companion look: the held companion is gone at disarm. "
                             "Her graph went with her 3D, so there is nothing to "
                             "restore.");
            }
            g_headtrackHeld  = RE::ActorHandle{};
            g_headtrackPrior = true;
        }
        // OS-107: put her heading back. Same handle rule as the hold above - a
        // follower left spun to face a camera that is no longer there stays
        // wrong for the rest of the save, and the Update3DPosition is as
        // necessary on the way out as it was on the way in.
        if (g_headingHeld) {
            if (const auto ptr = g_headingHeld.get(); ptr && ptr->Get3D()) {
                ptr->data.angle.z = g_headingPrior;
                ptr->Update3DPosition(true);
                spdlog::info("companion look: '{}' 0x{:08X}: heading restored to "
                             "{:.2f} rad (OS-107).",
                             ptr->GetName(), ptr->GetFormID(), g_headingPrior);
            } else {
                // Streamed out while held. Her 3D went with her, and a rebuild
                // takes the heading off the ref record, so there is nothing here
                // that putting a number back would fix.
                spdlog::info("companion look: the turned companion is gone at disarm. "
                             "Her 3D went with her, so there is no heading to restore.");
            }
            g_headingHeld  = RE::ActorHandle{};
            g_headingPrior = 0.0f;
        }
        g_headtrackFailLogged = false;
        g_playerHeadtrackFailLogged = false;
        // Put the CONFIGURED space back, so nothing outside an arm (the panel,
        // a save load, the next open's own resolve) ever reads a menu-local 0.
        Settings::GetSingleton().declutterMode =
            Settings::GetSingleton().declutterModeIni;
        // What we hand back to the world. Paired with the open-edge print:
        // if the head is already broken HERE, on the way out of the
        // inventory, then the editor never had a chance and the damage is
        // done before it opens.
        LogHeadState("bubble disarm", spinYaw_, spinBasisValid_);
        exitPhase_ = 0;  // r47: the exit machine is a pure hold - no fader state
        // F-26 r2: the sheathe is DEFERRED past the switch gap, exactly like
        // the B-8 v2 move mirror. Paying it inline sheathed on every menu
        // SWITCH: at close time a switch and a real exit are indistinguishable,
        // and on the DEFAULT settings (bSleekExit=1, space on,
        // fExitHoldSeconds=0) the r50 zero-frame cut calls this Disarm from
        // inside the close event's own call stack - so the weapon went away and
        // was drawn again a frame later (inventory->magic, ~2 s of animation).
        //
        // Cancelling is safe because the debt outlives it: the gate checks its
        // weDrew branch FIRST and returns kNone while still armed, so the
        // re-opened menu holds the same drawn weapon and the same outstanding
        // sheathe (WeaponPreviewGate.h, and the "we drew, conditions still
        // hold" case in the gate suite).
        //
        // KEYED ON THE DEBT, NOT ON armedLastFrame_, and deliberately OUTSIDE
        // the armed block. Review caught the hole: a switch whose SECOND menu
        // fails to arm (the unpause race / missing 3D noted above) hits this
        // function with armedLastFrame_ already false, so an armed-gated
        // deferral would never re-take the debt the open event just cancelled -
        // and nothing else can pay it, because Update() only runs from an armed
        // Tick. The player would stay drawn for the rest of the session, which
        // is precisely the outcome WeaponPreview.cpp's own comment calls out.
        if (WeaponPreview::HasDebt()) {
            // r20b: STAMP ONCE, ON THE TAKING EDGE.
            //
            // Disarm() runs EVERY FRAME while no menu is open (see the call at
            // the bottom of the no-menus branch), so re-stamping here reset the
            // countdown every frame and the 0.085 s window could never elapse.
            // Field 2026-07-20: `waiting out the 0.085s switch window` held for
            // 2.2 s, then the next open cancelled it. The sheathe only ever
            // fired when the close fade happened to return early and suppress
            // Disarm for long enough - luck, not design.
            //
            // The cost was not cosmetic: an unpaid sheathe leaves a drawn state
            // WE manufactured standing in live gameplay, and the user saved on
            // top of it. On reload the graph rebuilds, the model returns to its
            // sheath node, and the save still says drawn - empty hands, weapon
            // on the hip.
            //
            // A switch still re-stamps correctly: the open handler clears
            // pendingWeaponRestore_, so the next close takes the debt afresh.
            //
            // Same shape as r19c: state that must be latched once was being
            // re-derived every frame.
            if (!pendingWeaponRestore_) {
                weaponRestoreQpc_ = QpcNow();
            }
            pendingWeaponRestore_ = true;
        }
        if (armedLastFrame_) {
            // B-7 v3: un-compose the preview spin from the root node - the
            // engine re-syncs from data.angle on unpause, but one spun
            // frame in the handoff would read as a flicker.
            if (spinBasisValid_) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (auto* root = player ? player->Get3D(false) : nullptr) {
                    root->local.rotate = rootBaseRotate_;
                    RE::NiUpdateData ctx;
                    ctx.time = 0.0f;
                    ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                    root->Update(ctx);
                }
                spinBasisValid_ = false;
            }
            if (auto* horse = spinHorseRoot_.get()) {  // r54: un-spin the mount
                horse->local.rotate = horseBaseRotate_;
                RE::NiUpdateData ctx;
                ctx.time = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                horse->Update(ctx);
            }
            spinHorseRoot_.reset();
            // Open 3: hand her back unspun. Same reason as the player's
            // un-compose above - the engine will re-sync her from her own angle
            // on unpause, but one spun frame in the handoff reads as a flicker.
            ReleaseSpinCompanion();
            spinTarget_ = 0.0f;
            spinYaw_ = 0.0f;
            // B-2 exit mirror fallback: the deferred close-time mirror
            // (B-8 v2) owns the normal exit; this only covers disarms that
            // never saw a close event (dormant unpause, player 3D gone).
            airFrozenArm_ = false;
            preserveDirectionBitsArm_ = false;
            movingArm_ = false;
            // The pumps' copy dies with the session too. A stale true would only
            // withhold a stop event the standing case does not need; a stale
            // false would send one the moving case cannot pay back.
            WeaponPreview::SetMovingArm(false);
            Transition::Snap(0.0f);  // F-12 backstop: early disarms don't ramp
        }
        // B-14: the scene restore is deliberately OUTSIDE the armed gate. The
        // warm-occluder open path builds the void from the open EVENT itself
        // ("gap occluder WARM (same-frame, gap-free) - cull now"), BEFORE the
        // first armed tick ever runs - so a session that dies between build and
        // arm (field 2026-07-20 01:00:36.595-36.600: Skyrim Souls stripped the
        // pause in the switch churn, dormancy fired, no "Bubble ARMED" line)
        // reached this Disarm with armedLastFrame_ still false, the whole block
        // below was skipped, and the built void (166 hidden refs, 42 culled
        // nodes, water, imods) was ORPHANED into gameplay. That is the "void
        // bubble stays after exit" report. Every callee no-ops when its piece
        // was never built, so running them un-armed costs a handful of flag
        // checks. Spin/camera restores stay armed-gated above and below - they
        // undo state only an armed tick creates.
        FaceNeutral::Restore();
        Declutter::RestoreAll();
        Backdrop::Remove();
        Backdrop::OccluderHide();  // keep the occluder resident + warm for the next open
        // r28: drop the live-studio claim BEFORE the rig comes down, so nothing
        // downstream can see RigAllowed() still true against a removed rig and
        // put it straight back up.
        if (Settings::GetSingleton().liveStudioActive) {
            Settings::GetSingleton().liveStudioActive = false;
            // r28c: hand the fade back down with it. The armed path's snap sits
            // inside the armedLastFrame_ block below and a live-studio session
            // never armed, so without this a session that only ever lit the rig
            // would leave the transition parked at 1.
            Transition::Snap(0.0f);
        }
        StudioRig::Remove();
        StudioLight::Restore();
        SceneTint::Restore();  // instant restore at the close edge - no tint leaks to gameplay
        FsmpDrive::SetRotationFreedom(false);
        CbpcDrive::SetSimulateWhilePaused(false);
        FootIkGate::SetSuppressed(false);
        if (armedLastFrame_ && Settings::GetSingleton().tickFace &&
            !CharacterFrozen()) {
            // THE SESSION IN NUMBERS. Blinks are too short and too rare to
            // judge by eye - this is the line that says whether the face
            // actually lived, and it is the line to quote before claiming the
            // blink is fixed. bakes>0 with blinks>0 and no visible blinking
            // means the bake is landing somewhere else; bakes==0 means the
            // call never ran and nothing below it is evidence of anything.
            spdlog::info("face session: {} mesh bakes, blinks {} started / {} "
                         "completed, peak composed lid {:.2f}, blink timer capped "
                         "{} times (bBlinkStressTest).",
                         faceMeshApplies_, blinkStarts_, blinkCompletes_,
                         blinkLidPeak_, blinkCaps_);
        }
        if (armedLastFrame_) {
            // The idle re-pick's pass/fail number, next to the face one. Before
            // AnimEventProbe::SetArmed(false) so nothing it counts is missed.
            ClipProbe::ArmedSessionReport();
            // F-15: the view was OURS - full SPIM ResetCamera-style restore
            // (first person handed back if we forced it, Settings originals,
            // interpolators snapped inside). Otherwise schedule only the zoom
            // reconcile for the next frame: an external view mod restores its
            // targets from another close-event sink, whose ordering is not
            // guaranteed relative to this one. Its yaw remains provider-owned;
            // SPII does not restore targetYaw and SmoothCam resumes it later.
            AnimEventProbe::SetArmed(false);  // diagnostic tag: back to live
            ActorTickProbe::MarkArmed(false);
            CompanionProbe::Report();  // one line, with its own interpretation
            CompanionLunge::ArmedSessionReport();
            EquipNotifyGate::SetArmed(false);
            const bool ownViewActive = OwnView::Active();
            const auto reconcileTiming = ExternalViewExitPolicy::ChooseDisarmTiming({
                .armed = true,
                .ownViewActive = ownViewActive,
            });
            if (ownViewActive) {
                OwnView::Disarm();
            } else if (reconcileTiming ==
                       ExternalViewExitPolicy::ReconcileTiming::kNextFrame) {
                pendingExternalViewReconcile_ = true;
            }
        }
        // ⚠ THE DEBT COLLECTOR, DELIBERATELY OUTSIDE THE armedLastFrame_ GATE
        // ABOVE. This is NOT a third restore of the third-person state: it is
        // the same single owner's same snapshot, called again because an earlier
        // attempt could not run (OwnView::Disarm returns early and loudly when a
        // singleton is null). Disarm() runs every frame while no counted menu is
        // open, so an owed framing is retried until the singletons exist.
        //
        // It cannot trample a view mod. g_state.active is true only when WE
        // applied the framing, and OwnView::Disarm early-returns on !active. The
        // retry is driven by our own ownership flag and never by reading the
        // camera back and guessing whether the value on it is ours.
        //
        // It also closes the second hole: OwnView::Disarm is otherwise reachable
        // only from inside the armedLastFrame_ branch plus three exit-path
        // sites, and nothing enforces that a future path cannot drop that flag
        // while a framing is still up.
        //
        // Silent on purpose: this fires every frame while no menu is open, and
        // OwnView::Disarm already says the useful thing exactly once per owed
        // debt. A line here would be the same wall of text one layer up.
        if (OwnView::Active()) {
            OwnView::Disarm();
        }
        armedLastFrame_ = false;
    }

    bool Bubble::IsBubbleActive() {
        // The gates get the LONG hold: a menu switch can outlast the visual
        // grace, and a camera gate that lapses mid-switch is a visible lurch.
        return ActiveWithHold(GetSingleton().gateHoldFrames_.load() > 0);
    }

    bool Bubble::IsBubbleVisible() {
        // Pictures get the SHORT one. See the header: this is what stopped the
        // action bar taking half a second to leave the screen.
        return ActiveWithHold(GetSingleton().graceFrames_.load() > 0);
    }

    bool Bubble::ActiveWithHold(bool a_holdLive) {
        auto& self = GetSingleton();
        if (!Settings::GetSingleton().enabled) {
            return false;
        }
        if (self.menusOpen_.load() <= 0) {
            // Hold through a menu switch: the pause counter dips to zero between
            // close and open, and tearing down in that gap is what both hold
            // counters exist to prevent. WHICH hold is the caller's choice.
            return a_holdLive;
        }
        // ⚠ THE MENU IS COUNTED AND THE STUDIO IS NOT IN IT. Under
        // bWaitForOwnerContext a covered menu opens, is counted, and stays
        // vanilla until an owner publishes a context. In a load order without
        // Skyrim Souls that menu PAUSES THE GAME ON ITS OWN, so every test
        // below would read true and hand the whole mod - the strip, the mouse
        // gates, the camera, the try-on prompt - a menu it was told to leave
        // alone. The studio session is the honest question here.
        //
        // Deliberately BELOW the switch-hold branch above, which is untouched:
        // the exit choreography and the gate hold run off the menu count and
        // armedLastFrame_, and threading the studio flag into them would give
        // the stand-down the 30-frame trailing window as well. This edge wants
        // no tail - the gate closing mid-menu should read false the same frame.
        if (!self.studioEntered_.load()) {
            return false;
        }
        auto* main = RE::Main::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        return (main && main->freezeTime) || (ui && ui->GameIsPaused());
    }

    bool Bubble::ShouldShowTryOnPrompt() const {
        const auto& cfg = Settings::GetSingleton();
        if (!cfg.showTryOnPrompt) {
            return false;
        }
        if (!IsBubbleActive() || cfg.declutterMode < 2) {
            return false;  // only the void / dressing room, only while armed
        }
        // Inventory-family only - you try on gear, not spells (MagicMenu excluded).
        const auto& m = currentMenuName_;
        return m == "InventoryMenu" || m == "GridInventoryMenu" || m == "BarterMenu" ||
               m == "ContainerMenu";
    }

    // One input event, for the pivot camera only. Called from ProcessEvent
    // BEFORE the pause-gated early return, because the pause lands a few frames
    // after a menu opens and a drag started in that window was being dropped.
    //
    // Orbiting the camera and spinning the character are separate features on
    // separate buttons, so this is deliberately not gated on bPreviewSpin
    // either: a setting should never disable something it does not name.
    void Bubble::HandleCameraInput(RE::InputEvent* a_event) {
        const auto cursorInZone = [] { return StudioCamera::CursorInZone(); };
        if (const auto* button = a_event->AsButtonEvent(); button) {
            // LAlt stays with the character editor (2026-08-06). The studio
            // camera stands down in that menu - see the RaceSex open edge -
            // so the menu's own zoom is the control doing the work again and
            // a remap would fight it. Everywhere else LAlt belongs to the
            // game (sprint by default), and the camera has no business on it.
            if (button->device.get() == RE::INPUT_DEVICE::kKeyboard) {
                return;
            }
            if (button->device.get() != RE::INPUT_DEVICE::kMouse) {
                return;
            }
            using Key = RE::BSWin32MouseDevice::Key;
            const auto id = button->GetIDCode();
            if (id == static_cast<std::uint32_t>(Key::kLeftButton)) {
                // ⚠ IsDown() AND IsUp(), NEVER Value() > 0. A ButtonEvent
                // repeats EVERY FRAME for as long as the button is held, so
                // asking "is it pressed" re-ran the region test on every frame
                // of a drag. The instant the cursor crossed onto the menu the
                // latch was dropped and the swing died, which is the whole of
                // "dragging does not work over the SkyUI menu", and it equally
                // killed a drag begun in the frames before the cursor source is
                // ready, which is "cannot orbit immediately". One fault, both
                // symptoms. The field log named it: 2187 refusals across 4685
                // frames is not clicking, it is a per-frame test.
                //
                // Same family as [[input-passthrough-and-hidden-ui-pitfalls]]:
                // a held-button gate that keeps re-asking eats its own gesture.
                if (button->Value() > 0.0f) {
                    camLeftEvidence_ = true;  // the tick's missing-release watch
                }
                if (button->IsDown()) {
                    camHoldDecided_ = true;
                    // A fresh press supersedes any un-consumed release debt.
                    // With the tap reading the queue upstream of FLICK, we can
                    // see a release whose Scaleform copy was swallowed and
                    // never arrives to be eaten; left standing, that debt
                    // would eat the NEXT legitimate click. A lone Scaleform
                    // up with no down under it is a no-op, so clearing here
                    // costs nothing in the ordinary path.
                    camOwedLeftUp_ = false;
                    camOrbitDragging_ = cursorInZone();
                } else if (button->IsUp()) {
                    // Owe Scaleform's copy of this release to the camera. It
                    // arrives later through the UI message queue, by which time
                    // the latch has already cleared, and without the debt the
                    // menu would take a click wherever the swing ended.
                    if (camOrbitDragging_.load()) {
                        camOwedLeftUp_ = true;
                    }
                    camOrbitDragging_ = false;
                    camHoldDecided_ = false;
                } else if (button->Value() > 0.0f && !camHoldDecided_.load()) {
                    // ⚠ A REPEAT WITH NO EDGE THIS HOLD IS A SWALLOWED PRESS.
                    // FLICK hooks the input dispatch and eats everything while
                    // a window without kPassInputToGame is up; Fitting Room's
                    // editor opens that gate only once a button is ALREADY
                    // held, so the down edge dies blocked and the held repeats
                    // are all that arrives here. The first one carries the
                    // press's zone decision - ONCE, so the crossed-onto-the-
                    // menu latch survival stands. See camHoldDecided_.
                    camHoldDecided_ = true;
                    camOrbitDragging_ = cursorInZone();
                }
            } else if (id == static_cast<std::uint32_t>(Key::kMiddleButton)) {
                if (button->Value() > 0.0f) {
                    camMiddleEvidence_ = true;  // the tick's missing-release watch
                }
                // Middle press-and-drag pans the shot; a plain CLICK keeps
                // its old job, the escape hatch back to the opening framing.
                // Which one it was is only known at the release - whether the
                // mouse travelled between the edges - so the recentre now
                // fires on the up rather than the down, and a drag's release
                // must not also recentre at wherever the pan ended.
                if (button->IsDown()) {
                    camPanDragging_ = cursorInZone();
                    camPanTravel_ = 0.0f;
                } else if (button->IsUp()) {
                    if (camPanDragging_.load() && camPanTravel_ < 4.0f &&
                        Settings::GetSingleton().middleClickRecentre) {
                        StudioCamera::ResetOffsets();
                    }
                    camPanDragging_ = false;
                }
            } else if (button->IsDown() &&
                       (id == static_cast<std::uint32_t>(Key::kWheelUp) ||
                        id == static_cast<std::uint32_t>(Key::kWheelDown))) {
                // The wheel has no press-and-hold, so there is no edge to latch
                // and the region is asked per notch.
                //
                // ⚠ THE EDITOR IS NOT SPECIAL-CASED HERE, AND ONE ROUND SPENT
                // MAKING IT SO IS WHY THE NOTE IS LONG. A field report of the
                // wheel scrolling RaceMenu's preset file dialog AND pulling
                // the camera in got answered by refusing the wheel outright
                // while that menu was open, which cost the editor its zoom
                // everywhere to fix it in one dialog.
                //
                // The region test already covers the ordinary case: RaceMenu's
                // slider column ends around a third of the way across and the
                // zone starts at 0.44, so a notch over the LIST was never
                // reaching the camera. Only a CENTRED MODAL reaches into the
                // zone, and that is a narrow, momentary overlap rather than a
                // reason to disarm the control.
                // Stamped whether or not the notch is ours, because the question
                // the inspect watch asks is "did a wheel notch happen just
                // before the item zoomed", not "did we act on one".
                wheelSeenQpc_ = QpcNow();
                if (cursorInZone()) {
                    StudioCamera::AddZoom(
                        id == static_cast<std::uint32_t>(Key::kWheelUp) ? 1.0f : -1.0f);
                }
            }
        } else if (a_event->eventType.get() == RE::INPUT_EVENT_TYPE::kMouseMove) {
            const auto* move = static_cast<const RE::MouseMoveEvent*>(a_event);
            if (camOrbitDragging_.load()) {
                StudioCamera::AddOrbit(static_cast<float>(move->mouseInputX),
                                       static_cast<float>(move->mouseInputY));
            } else if (camPanDragging_.load()) {
                const auto dx = static_cast<float>(move->mouseInputX);
                const auto dy = static_cast<float>(move->mouseInputY);
                // The click-or-drag decision: a click's tremor stays a
                // click, so the pan only starts once the hand has clearly
                // travelled.
                camPanTravel_ += (dx < 0.0f ? -dx : dx) + (dy < 0.0f ? -dy : dy);
                if (camPanTravel_ >= 4.0f) {
                    StudioCamera::AddPan(dx, dy);
                }
            }
        }
    }

    RE::BSEventNotifyControl Bubble::ProcessEvent(RE::InputEvent* const* a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>*) {
        // F-14: right-drag spins the PREVIEW BODY. The log proved the SPII
        // author's SmoothCam-API build rotates the CAMERA only (heading +
        // freeRot constant through every drag) - physics correctly sees
        // nothing move. This is the rotation that moves the SKELETON, so
        // hair/cloth swings with real momentum; it lives on its own input
        // and coexists with the author's camera orbit. MenuInputGate
        // already eats right-mouse in the UI (no quick-buy on drags).
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // Preview prompt: record the last-seen input device on every call,
        // even while idle, so the keyboard/gamepad label stays fresh across
        // the arm edge rather than only updating while armed.
        for (auto* e = *a_event; e; e = e->next) {
            lastInputDevice_.store(e->device.get());
        }
        // ⚠ THE CAMERA IS HANDLED ABOVE THIS EARLY RETURN, DELIBERATELY.
        // IsBubbleActive() requires the PAUSE to have landed, and the pause
        // lands a few frames after a menu opens. Everything below used to be
        // unreachable in that window, so a drag started the instant a menu
        // appeared was dropped before anything could act on it. A menu SWITCH
        // hides the bug completely: gateHoldFrames_ holds this condition true
        // across the gap, so events flow from the first frame and the camera
        // works, which is precisely the "only after switching to the magic menu
        // and back, and then it keeps working" report.
        //
        // The camera asks a different question: is one of our menus OPEN. It
        // does not need the world frozen to accept a drag, and StudioCamera
        // still writes nothing until it has armed.
        //
        // ⚠ FALLBACK ONLY. The input tap upstream of FLICK's swallow is the
        // camera's real source now; feeding events from here as well would
        // apply every gesture twice. This loop exists for a runtime where the
        // pump site could not be located.
        const auto& s = Settings::GetSingleton();
        if (!InputTapHook::installed && s.studioCamera && OpenMenuCount() > 0 &&
            StudioSessionEntered()) {
            for (auto* event = *a_event; event; event = event->next) {
                HandleCameraInput(event);
            }
        }
        if (!IsBubbleActive() || raceMenuOpen_.load()) {
            spinDragging_ = false;
            spinStickX_ = 0.0f;
            // ⚠ THE CAMERA DRAG IS DELIBERATELY NOT CLEARED HERE. This condition
            // wants the world frozen, and it is false for the opening frames of
            // every menu, so clearing here dropped swings that were legitimately
            // in flight. The release and Disarm both already own the latch, and
            // between them there is no way for it to survive something it
            // should not.
            return RE::BSEventNotifyControl::kContinue;
        }
        for (auto* event = *a_event; event; event = event->next) {
            // F-14 v3 input evidence (field r30: "controller rotation
            // doesn't work" with zero input facts in the log): the first
            // button/stick events of each arm get logged raw, so the next
            // session's log PROVES whether thumbstick events reach this
            // sink at all - and with which idCodes.
            if (s.verboseLog && inputEvidence_.load() > 0 &&
                event->eventType.get() != RE::INPUT_EVENT_TYPE::kMouseMove) {
                --inputEvidence_;
                if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    const auto* stick = static_cast<const RE::ThumbstickEvent*>(event);
                    spdlog::debug(
                        "input evidence: THUMBSTICK id={} right={} x={:.2f} y={:.2f}",
                        stick->idCode, stick->IsRight(), stick->xValue, stick->yValue);
                } else {
                    const auto* id = event->AsIDEvent();
                    spdlog::debug(
                        "input evidence: type={} device={} id={} userEvent='{}'",
                        static_cast<int>(event->eventType.get()),
                        static_cast<int>(event->GetDevice()),
                        id ? id->idCode : 0xFFFFFFFF,
                        id ? id->userEvent.c_str() : "");
                }
            }
            if (!s.previewSpin) {
                continue;  // evidence still collects above
            }
            if (const auto* button = event->AsButtonEvent(); button) {
                if (button->device.get() == RE::INPUT_DEVICE::kMouse &&
                    button->GetIDCode() == 1) {
                    spinDragging_ = button->Value() > 0.0f;
                }
            } else if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kMouseMove &&
                       spinDragging_.load()) {
                const auto* move = static_cast<const RE::MouseMoveEvent*>(event);
                // r60 (user): the sign was backwards - dragging LEFT spun the
                // character clockwise from their own perspective. Negated, so
                // the body follows the hand: drag left, they turn to their
                // left. The stick path shares this convention (see the tick).
                spinTarget_ -= static_cast<float>(move->mouseInputX) *
                               Settings::GetSingleton().spinSensitivity;
            } else if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                // F-14 v3: DIRECT right-stick rotation (the design SPIM
                // itself ships with iGamepadTurnMethod=0 - no hold button;
                // r30's hold-gate also compared idCode against 274 while
                // the engine delivers shoulder buttons as 9/10, so the
                // hold could never register). The tick integrates the
                // stored deflection; the inspect gate lives there too.
                const auto* stick = static_cast<const RE::ThumbstickEvent*>(event);
                if (stick->IsRight()) {
                    spinStickX_ = stick->xValue;
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    // ⚠⚠ THE PARK'S ONE READING OF THE PLAYER'S OWN CAMERA, AND THE MOMENT IS
    // THE MENU-OPEN EVENT AND NOT THE FIRST ARMED TICK. Field log 2026-08-31:
    // the open event reads fov 80.0, freeRot 3.196, pitch 0.063 - the player's
    // own - and the first armed tick reads 60.0 / 2.642 / 0.100 for the SAME
    // menu, three constants that never varied across sixteen consecutive opens
    // because they are the framing's. The comment that stood beside the tick
    // read said it ran "before ArmOwnViewIfOurs applies a framing", which is
    // true of OUR framing and false of a view mod's: Show Player In Inventory
    // frames the menu from its own open sink, and that sink runs first.
    //
    // ⚠⚠ FOUR REPAIRS MISSED THIS BECAUSE THEY ALL BELIEVED THAT COMMENT.
    // Three worked on the payment and one on the capture being re-taken; none
    // asked whether the first take was early enough.
    //
    // ⚠ SHARES CameraCloseProbe::SampleView's SHAPE, NEVER ITS STORAGE. The
    // probe's g_preOpen holds the right numbers and is gated on
    // bCameraCloseProbe, a diagnostic setting that is off for every player, so a
    // park that read it would be correct on the rig that debugged this and dead
    // in the field. That is the worst failure shape there is.
    //
    // ⚠ ALL FIVE FIELDS AND THE STANCE PAIR DESCRIBE ONE INSTANT.
    // RotationOwnershipPolicy::ChooseStanceFlags decides whether the two flags
    // are still ours to hand back by comparing the stance now against the stance
    // they were read in, so a stance sampled a frame away from its flags makes
    // that comparison a lie.
    void Bubble::TakePreMenuCameraCapture(const std::string& a_menuName,
                                          const char*        a_moment) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            // ⚠ NO LATCH ON THIS PATH, deliberately, and it is the one
            // difference from an empty read below. Nothing was read AND no
            // framing has run yet, so the arm-tick backstop is still the right
            // answer; burning the latch here would turn it into the loud refusal
            // instead.
            return;
        }
        // ⚠⚠ THE OTHER END OF THE FROZEN CAMERA. Each read below guards itself
        // on its own valid flag, which is right, but the flags alone cannot tell
        // a first take from a RE-take inside the same menu: a mid-menu Disarm
        // used to clear them, and the tick after it read the framing's numbers
        // as though they were the player's. The teardown rule in Disarm stops
        // the flags being cleared under a live menu; this asks the question a
        // second time, from the other side, so a route that gets past the first
        // one is refused rather than absorbed. Reasoning and the measurement are
        // in CameraDebtPolicy.h.
        const auto captureCall = CameraDebtPolicy::ChooseCapture({
            .haveCapture = preMenuFreeRotValid_ || preMenuPitchValid_ ||
                           preMenuFovValid_,
            .captureAlreadyTaken = captureTakenThisSession_,
        });
        if (captureCall == CameraDebtPolicy::Capture::kRefuseFramingLive) {
            // Loud, because it should be unreachable. The capture is gone and
            // this session already took one, so something cleared it while a
            // framing owned the camera. Reading now would hand that framing to
            // gameplay - free rotation on with the angle standing still, which
            // is a camera that stops following the character.
            spdlog::warn(
                "camera park: REFUSED to re-read the player's camera in "
                "'{}' at the {}. This menu session already took its reading and "
                "the reading is gone, so a teardown cleared it with the framing "
                "still on. Whatever this call can see belongs to the framing, "
                "not to the player. The close will hand nothing back, which "
                "is a worse camera than it should be and a better one than "
                "handing back the menu's own numbers.",
                a_menuName.empty() ? "(unnamed)" : a_menuName, a_moment);
        }
        if (captureCall != CameraDebtPolicy::Capture::kTake) {
            return;  // kAlreadyHeld is the healthy answer at the backstop
        }
        // ⚠⚠ WHOSE CAMERA IS LIVE RIGHT NOW. Through Tab the tween menu is up
        // and holding it, so a reading taken here is the tween's (field
        // 2026-09-02, 4 of 4). The tween's open edge read the player's camera
        // into preTween_ before that. Carry it, and read nothing.
        {
            bool tweenUp = false;
            if (const auto* ui = RE::UI::GetSingleton()) {
                tweenUp = const_cast<RE::UI*>(ui)->IsMenuOpen(RE::TweenMenu::MENU_NAME);
            }
            const auto source = CameraDebtPolicy::ChooseReadingSource({
                .tweenMenuUp = tweenUp,
                .preTweenReadingValid = preTween_.valid,
            });
            if (source == CameraDebtPolicy::ReadingSource::kPreTweenReading) {
                preMenuFreeRot_ = preTween_.freeRot;
                preMenuAnimCam_ = preTween_.animCam;
                preMenuFreeRotEnabled_ = preTween_.freeRotEnabled;
                preMenuFreeRotValid_ = true;
                preMenuZoomCur_ = preTween_.zoomCur;
                preMenuZoomTgt_ = preTween_.zoomTgt;
                preMenuStanceKnown_ = preTween_.stanceKnown;
                preMenuWeaponDrawn_ = preTween_.weaponDrawn;
                preMenuPitch_ = preTween_.pitch;
                preMenuPitchValid_ = true;
                preMenuFov_ = preTween_.fov;
                preMenuFovValid_ = true;
                preMenuCameraState_ = preTween_.cameraState;
                preMenuInCombat_ = player->IsInCombat();
                preMenuViaTween_ = true;
                captureTakenThisSession_ = true;
                const auto* camera = RE::PlayerCamera::GetSingleton();
                spdlog::info(
                    "camera park: reading CARRIED from the tween menu's open event into "
                    "'{}' at the {}: fov {:.1f}, freeRot {:.3f}, pitch {:.3f}, animCam {}, "
                    "freeRotEnabled {}, weapon {}. This is what the close hands back. The "
                    "camera live now is the tween's ({}) and was not read. Context NOT "
                    "restored: zoom {:.2f}->{:.2f}, camera {}, combat {}, tween menu UP "
                    "(Tab route).",
                    a_menuName.empty() ? "(unnamed)" : a_menuName, a_moment, preMenuFov_,
                    preMenuFreeRot_, preMenuPitch_, preMenuAnimCam_, preMenuFreeRotEnabled_,
                    preMenuStanceKnown_ ? (preMenuWeaponDrawn_ ? "drawn" : "sheathed")
                                        : "unknown",
                    camera && camera->currentState ? CameraStateName(camera->currentState->id)
                                                   : "(none)",
                    preMenuZoomCur_, preMenuZoomTgt_,
                    CameraStateName(static_cast<RE::CameraState>(preMenuCameraState_)),
                    preMenuInCombat_ ? "IN COMBAT" : "out of combat");
                return;
            }
        }
        // ⚠⚠ THE SLOT OBJECT, NOT THE CURRENT STATE. The payment and the
        // close-edge sample were moved off GetThirdPersonState() because it
        // returns null the moment the player is not looking through the
        // third-person camera; the capture was left behind, so a menu entered
        // from first person or from a mount captured nothing,
        // preMenuFreeRotValid_ stayed false, and the whole flag payment was
        // skipped for that entire session. A backstop gated on the same narrow
        // question it exists to back up is not a backstop.
        if (auto* slot = ThirdPersonSlotState()) {
            preMenuFreeRot_ = slot->freeRotation.x;
            preMenuAnimCam_ = slot->toggleAnimCam;
            preMenuFreeRotEnabled_ = slot->freeRotationEnabled;
            preMenuFreeRotValid_ = true;
            // ⛔ RECORDED, NEVER RESTORED. See the note on these members.
            preMenuZoomCur_ = slot->currentZoomOffset;
            preMenuZoomTgt_ = slot->targetZoomOffset;
            // The stance the two flags were read in. Same call, same reason:
            // this is the last moment on which the answer is still the player's
            // own and not a framing's.
            const auto stance = WeaponStance::Read(player);
            preMenuStanceKnown_ = stance.known;
            preMenuWeaponDrawn_ = stance.drawn;
        }
        preMenuPitch_ = player->data.angle.x;
        preMenuPitchValid_ = true;
        if (auto* camera = RE::PlayerCamera::GetSingleton()) {
            preMenuFov_ = camera->worldFOV;
            preMenuFovValid_ = true;
            // ⚠ WHOSE CAMERA WAS ACTUALLY CURRENT AT THIS INSTANT. A menu
            // reached through the tween menu (which is what Tab does, and a
            // hotkey does not) opens with kTween holding the camera, and the
            // tween menu carries its own field of view: this rig measured it at
            // 90.0 against a gameplay 80.0. A reading taken there is a reading
            // of the tween menu.
            preMenuCameraState_ = camera->currentState
                                      ? static_cast<int>(camera->currentState->id)
                                      : -1;
        }
        preMenuInCombat_ = player->IsInCombat();
        // ⚠ THE TWEEN MENU IS NOT IN sMenus, SO WE NEVER COUNT IT, which is
        // exactly why it can be standing over this reading without menusOpen_
        // knowing. That makes sessionFresh true on a route where the camera is
        // already somebody else's.
        preMenuViaTween_ = false;
        if (const auto* ui = RE::UI::GetSingleton()) {
            preMenuViaTween_ =
                const_cast<RE::UI*>(ui)->IsMenuOpen(RE::TweenMenu::MENU_NAME);
        }
        // ⚠ SET EVEN IF EVERY READ ABOVE FAILED ITS NULL CHECK. The latch means
        // "this session has had its one chance", and a session that had it and
        // came away empty must not get a second one with the framing up.
        captureTakenThisSession_ = true;
        // ⚠ ALWAYS ON, AND IT NAMES THE MOMENT. The whole fault was a reading
        // taken one step too late, so the next field log has to say which of the
        // two sites took it without anyone turning a diagnostic on first.
        spdlog::info("camera park: reading taken at the {} of '{}': fov {:.1f}, "
                     "freeRot {:.3f}, pitch {:.3f}, animCam {}, freeRotEnabled {}, "
                     "weapon {}. This is what the close hands back. Context NOT "
                     "restored: zoom {:.2f}->{:.2f}, camera {}, combat {}, tween "
                     "menu {}.",
                     a_moment, a_menuName.empty() ? "(unnamed)" : a_menuName,
                     preMenuFovValid_ ? preMenuFov_ : 0.0f, preMenuFreeRot_,
                     preMenuPitch_, preMenuAnimCam_, preMenuFreeRotEnabled_,
                     preMenuStanceKnown_
                         ? (preMenuWeaponDrawn_ ? "drawn" : "sheathed")
                         : "unknown",
                     preMenuZoomCur_, preMenuZoomTgt_,
                     CameraStateName(static_cast<RE::CameraState>(
                         preMenuCameraState_)),
                     preMenuInCombat_ ? "IN COMBAT" : "out of combat",
                     preMenuViaTween_ ? "UP (Tab route)" : "closed");
    }

    // ⚠ THE TWEEN MENU'S OPEN EDGE. See the slot's note in Bubble.h. The same
    // fields TakePreMenuCameraCapture reads, off the same slot object, under the
    // same one-instant rule for the stance pair.
    //
    // ⚠⚠ THE LINE THIS LOGS IS THE FIELD CHECK. If the camera it names is
    // already kTween, this edge fires after the tween has the camera and a
    // reading here is worth nothing; the return-state policy then falls back to
    // the player's own mode bit and says so. Two rigs will tell.
    // ⚠ EVERY FRAME, UNDER FOUR GATES, AND CHEAP: a dozen field copies. The
    // gates are the three session-fresh terms plus the tween state itself, so
    // what this holds is always a camera nobody but the player was driving.
    // ⚠ NOT kTween, EVER: that is the one state this exists to see past.
    void Bubble::SampleGameplayCamera() {
        if (menusOpen_.load() != 0 || gateHoldFrames_.load() != 0 || OwnView::Active()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!player || !camera || !camera->currentState) {
            return;
        }
        if (camera->currentState->id == RE::CameraState::kTween) {
            return;
        }
        PreTweenReading s{};
        if (auto* slot = ThirdPersonSlotState()) {
            s.freeRot = slot->freeRotation.x;
            s.animCam = slot->toggleAnimCam;
            s.freeRotEnabled = slot->freeRotationEnabled;
            s.zoomCur = slot->currentZoomOffset;
            s.zoomTgt = slot->targetZoomOffset;
            const auto stance = WeaponStance::Read(player);
            s.stanceKnown = stance.known;
            s.weaponDrawn = stance.drawn;
        }
        s.pitch = player->data.angle.x;
        s.fov = camera->worldFOV;
        s.cameraState = static_cast<int>(camera->currentState->id);
        s.valid = true;
        lastGameplay_ = s;
    }

    // ⚠ THE TWEEN MENU'S OPEN EDGE. See the slot's note in Bubble.h. Copies the
    // last gameplay frame, because the camera live at this edge is already the
    // tween's (field 2026-09-02, this rig: `fov 90.0, camera kTween` here).
    //
    // ⚠⚠ THE LINE THIS LOGS IS THE FIELD CHECK. The camera it names must be the
    // player's own state at the gameplay field of view. If it names kTween, the
    // per-frame sample never ran before this edge, the fix is on its blind
    // fallback, and the line says which of the two happened.
    void Bubble::SamplePreTween() {
        if (lastGameplay_.valid) {
            preTween_ = lastGameplay_;
            spdlog::info("camera park: tween menu opening, the player's camera copied from "
                         "the last gameplay frame: fov {:.1f}, camera {}, freeRot {:.3f}, "
                         "pitch {:.3f}, animCam {}, freeRotEnabled {}, weapon {}. A covered "
                         "menu opened over this tween carries THIS as its reading instead "
                         "of reading the tween's camera.",
                         preTween_.fov,
                         CameraStateName(static_cast<RE::CameraState>(preTween_.cameraState)),
                         preTween_.freeRot, preTween_.pitch, preTween_.animCam,
                         preTween_.freeRotEnabled,
                         preTween_.stanceKnown ? (preTween_.weaponDrawn ? "drawn" : "sheathed")
                                               : "unknown");
            return;
        }
        // No gameplay frame sampled since the load. Read live and say so: on
        // the Tab route this reads the tween, and the return-state policy
        // then falls back blind.
        preTween_ = {};
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!player || !camera) {
            return;
        }
        if (auto* slot = ThirdPersonSlotState()) {
            preTween_.freeRot = slot->freeRotation.x;
            preTween_.animCam = slot->toggleAnimCam;
            preTween_.freeRotEnabled = slot->freeRotationEnabled;
            preTween_.zoomCur = slot->currentZoomOffset;
            preTween_.zoomTgt = slot->targetZoomOffset;
            const auto stance = WeaponStance::Read(player);
            preTween_.stanceKnown = stance.known;
            preTween_.weaponDrawn = stance.drawn;
        }
        preTween_.pitch = player->data.angle.x;
        preTween_.fov = camera->worldFOV;
        preTween_.cameraState =
            camera->currentState ? static_cast<int>(camera->currentState->id) : -1;
        preTween_.valid = true;
        spdlog::warn("camera park: tween menu opening with NO gameplay frame sampled since "
                     "the load, so the camera was read live at the edge: fov {:.1f}, "
                     "camera {}. On the Tab route that is the tween's camera and the close "
                     "will fall back blind.",
                     preTween_.fov,
                     CameraStateName(static_cast<RE::CameraState>(preTween_.cameraState)));
    }

    RE::BSEventNotifyControl Bubble::ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const std::string name{ a_event->menuName.c_str() };
        // ⚠⚠ THE TWEEN MENU IS HANDLED HERE, AND IT IS NEVER COUNTED. Tab opens
        // it before the inventory or the magic menu, and it takes the camera
        // (kTween, its own field of view) the moment it is up. Every gate below
        // declines it, correctly: it is nobody's menu to frame. But the covered
        // menu that opens OVER it reads the camera at its own open event, and
        // on this route that camera is the tween's. Field 2026-09-02, the
        // reporter's diag3 log: 4 of 4 readings 'tween menu UP', the close
        // handed kTween back, STILL WRONG at +8.01s. So the tween's own open
        // edge takes a reading of the player's camera into a slot the covered
        // menu carries from, and the tween's close drops the slot. Not counted,
        // not bubbled, not paid: the debt machinery still sees one capture per
        // session, exactly as measured on 25 of 25. Reasoning and the decision
        // in CameraDebtPolicy.h.
        if (name == RE::TweenMenu::MENU_NAME) {
            if (a_event->opening) {
                // The same three terms sessionFresh uses below, asked one menu
                // earlier: a tween opening over a live session (a menu switch
                // through Tab) must not overwrite the reading that session holds.
                const bool fresh = menusOpen_.load() == 0 &&
                                   gateHoldFrames_.load() == 0 && !OwnView::Active();
                if (CameraDebtPolicy::ChooseTweenSample(fresh) ==
                    CameraDebtPolicy::TweenSample::kTake) {
                    SamplePreTween();
                }
            } else if (preTween_.valid) {
                // Consumed or not, the slot dies with the tween: a covered menu
                // opening later with no tween up must read the camera, not this.
                spdlog::debug("camera park: tween menu closed, its reading dropped "
                              "(fov {:.1f}, camera {}).",
                              preTween_.fov,
                              CameraStateName(static_cast<RE::CameraState>(
                                  preTween_.cameraState)));
                preTween_.valid = false;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        // ⚠ AT THE ENTRY POINT, ABOVE EVERY GATE BELOW. The camera probe needs
        // the framing the menu was entered FROM, and it needs it whether or
        // not we go on to bubble that menu - the unbubbled open is the control,
        // and every early-out under this line would swallow it. Reads the
        // camera and stores a handful of floats; no-op unless the probe's own
        // key is on.
        //
        // ⚠ THE OR IS NOT REDUNDANT. 'RaceSex Menu' is NOT in the default
        // sMenus, so IsBubbleMenu alone would make the editor unreachable in the
        // shipped configuration - which is exactly the bug the note at the close
        // edge below is the autopsy of.
        // ⚠ HOISTED OUT OF THE PROBE'S BRANCH BECAUSE THE PARK NOW SHARES IT,
        // and it has to be read BEFORE the ++menusOpen_ further down or its
        // first term answers a question about the menu that is opening rather
        // than about the one it is opening over. False for anything that is not
        // one of ours, which is the right answer for both readers.
        bool sessionFresh = false;
        if (a_event->opening &&
            (Settings::GetSingleton().IsBubbleMenu(name) ||
             name == RE::RaceSexMenu::MENU_NAME)) {
            // ⚠ SESSION-fresh, not OPEN-fresh, and all three terms carry weight.
            // menusOpen_ is 0 during a switch GAP too, which is why the gate
            // hold (the same window IsBubbleActive uses) and OwnView's own
            // ownership flag join it. A baseline captured on a switch's second
            // open is the framing measured against itself.
            sessionFresh = menusOpen_.load() == 0 &&
                           gateHoldFrames_.load() == 0 &&
                           !OwnView::Active();
            CameraCloseProbe::OnBubbleMenuOpened(name, sessionFresh);
        }
        if (a_event->opening && name == RE::RaceSexMenu::MENU_NAME) {
            // The head-state diff. Same line for a console open and one
            // reached through Fitting Room, so the two can be compared
            // directly - see LogHeadState.
            LogHeadState("racemenu open", spinYaw_, spinBasisValid_);
        }
        // §4b: ShouldBubbleMenu, not IsBubbleMenu. A menu the user has handed
        // back to Skyrim Souls drops out HERE, at the single entry point, so it
        // is left completely alone: no arm, no studio, and no force-pause,
        // since EnsurePaused is reached from below this line. Bailing here
        // rather than gating each consumer is the whole reason this stays one
        // line - a menu is either ours or it is Souls', and there is no
        // half-owned state to reason about later.
        // r19c - THE PREDICATE IS NOT STABLE ACROSS A MENU'S LIFETIME, SO THE
        // CLOSE MUST NOT CONSULT IT.
        //
        // ShouldBubbleMenu() reads live settings, and the settings panel can be
        // opened from inside a bubbled menu. Toggling the Souls split there
        // flips this predicate BETWEEN a menu's open and its close: the open
        // incremented menusOpen_, the close took the early-out above, and the
        // count was never given back. menusOpen_ stayed >= 1 with no menu open,
        // so OnFrame kept ticking an armed bubble straight into gameplay - the
        // field report was a void that survived into normal play, and the log
        // shows it exactly (11 opens, 10 closes, one InventoryMenu close eaten,
        // then thousands of armed ticks with the player walking around).
        //
        // The old comment here said "a menu is either ours or it is Souls', and
        // there is no half-owned state to reason about later". A menu that
        // OPENED as ours and CLOSES as Souls' is precisely that state, and it
        // exists the moment settings are mutable at runtime. So ownership is
        // decided once, at open, and remembered: close decrements if and only if
        // we counted this menu at ITS open, whatever the settings say now. A
        // handover therefore takes effect on the NEXT open, which is the only
        // edge where it can be applied consistently.
        if (a_event->opening) {
            if (!Settings::GetSingleton().ShouldBubbleMenu(name)) {
                // r28g: a Souls-live menu is no longer dropped outright when
                // the live lighting is on - it gets a LIGHTING-ONLY session.
                // Counted like any other (r19c: ownership decided at open,
                // remembered at close), but latched live-only so the arm
                // decision can never build the studio in it, no matter what
                // the pause counter happens to read on frame 1 - a borrowed
                // tween pause must not put the void into a menu the user
                // explicitly keeps live.
                //
                // !armedLastFrame_: a live menu opening on top of an ARMED
                // session (per-menu split mixes) keeps the old drop behaviour;
                // the mixed case has never existed and is not being invented
                // here as a side effect.
                const auto& s            = Settings::GetSingleton();
                const bool  liveLighting = s.studioInLiveMenus && s.IsBubbleMenu(name) &&
                                          s.IsSoulsLiveMenu(name) && !armedLastFrame_;
                if (!liveLighting) {
                    // ⚠ r28h: NEVER DECLINE SILENTLY. This early-out has now
                    // eaten two field rounds on its own - once as the original
                    // Souls drop (r28g) and once with bStudioInLiveMenus off,
                    // which produced a 25-line log identical to "the mod did
                    // nothing" and sent us hunting the lights instead of the
                    // gate. A decline that cannot be seen is indistinguishable
                    // from a crash, a bad build, or a broken feature.
                    //
                    // Logged per open, not deduped: menu opens are a handful
                    // per minute, and the previous "cheap" silence cost far
                    // more than these lines ever will.
                    if (s.IsBubbleMenu(name)) {
                        spdlog::info("Bubble menu DECLINED: {}; soulsLive={} "
                                     "studioInLiveMenus={} armedLastFrame={} -> no session "
                                     "(no pause, no scene, no lighting).",
                                     name, s.IsSoulsLiveMenu(name), s.studioInLiveMenus,
                                     armedLastFrame_);
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                sessionLiveOnly_ = true;
                spdlog::info("Bubble menu opened LIVE: {} (Souls keeps it unpaused). "
                             "Lighting-only session: no pause, no scene, rig + colour "
                             "filter only.",
                             name);
            } else {
                // A menu WE pause re-decides this at its own open: a live
                // session followed by a paused menu (per-menu split) must not
                // inherit the previous menu's live-only latch.
                sessionLiveOnly_ = false;
            }
            countedMenus_.insert(name);
            ++menusOpen_;
            currentMenuName_ = name;  // own-view coverage decides per menu at arm
            // ⚠⚠ THE PARK'S READING, TAKEN HERE AND NOT ON THE FIRST ARMED
            // TICK. This is the earliest point at which the menu is known to be
            // ours: every decline above has already returned, and nothing
            // between here and the tick that arms can touch the camera without
            // a framing being what did it. The measurement that moved it is in
            // TakePreMenuCameraCapture.
            //
            // ⚠ GATED ON SESSION-FRESH FOR THE REASON THE PROBE IS. menusOpen_
            // reads 0 across a menu SWITCH's gap too, so a second open would
            // otherwise re-read the camera the outgoing menu's framing left and
            // the park would hand that to gameplay - which is the exact fault
            // this move exists to end. The latch in the policy would catch it
            // anyway; this makes it not arise.
            if (sessionFresh) {
                TakePreMenuCameraCapture(name, "menu-open event");
            }
            // ⚠ THE STUDIO CAMERA'S ARM AND THE PER-MENU SPACE RESOLVE BOTH
            // MOVED DOWN into EnterStudioSession. They are the studio's, not
            // the menu's, and under bWaitForOwnerContext a counted menu can
            // exist for minutes with no studio in it. Neither depends on
            // anything set between here and there.
            // ⚠ THE EDITOR ONCE FORCED SCENE VIEW HERE, AND THE FIELD TOOK IT
            // BACK OUT (2026-08-06). The reasoning was good and the evidence
            // was not: the head drag appeared to work in Scene view and fail
            // in the void, so this clamped the editor to Scene. The next pass
            // logged the clamp firing on all four editor opens - the room was
            // there - with the head still dead.
            //
            // ⚠ AND THE OBSERVATION ITSELF IS SUSPECT, which is the part worth
            // keeping. The fault PERSISTS once triggered: it survives the menu,
            // survives a return to gameplay, and only a race change clears it.
            // So the second test in any session reads "broken" whatever it is
            // testing, and every A/B run that morning - scene vs void, limited
            // vs full, pause on vs off - was measuring the order the trials
            // ran in. A trial here is only worth its result if it starts from
            // a known-clean state.
            graceFrames_ = 0;
            gateHoldFrames_ = 0;
            loggedUnpausedOnce_ = false;
            sessionDormant_ = false;  // r18: new menu session, new arm decision
            // r28b: baseline the toggle watcher against what this session is
            // actually deciding with, so the first frame cannot read a stale
            // default as a user toggle and re-decide a session that just began.
            lastForcePause_ = Settings::GetSingleton().forcePause;
            // F-26 r2: same cancel, same reason - this open proves the close
            // was a SWITCH, so the weapon simply stays in hand and the debt
            // rides through to the next disarm. Deliberately NOT gated on
            // armedLastFrame_: the default zero-hold sleek exit disarms inside
            // the close event, so that flag is already false here - which is
            // exactly the case this fix exists for.
            if (pendingWeaponRestore_) {
                pendingWeaponRestore_ = false;
                spdlog::debug("weapon preview: deferred sheathe cancelled (menu switch), "
                              "weapon stays in hand.");
            }
            if (pendingExternalViewReconcile_) {
                pendingExternalViewReconcile_ = false;
                spdlog::debug("external view: post-close reconcile cancelled (menu switch).");
            }
            // ⚠ THE ENGINE'S NAME HAS A SPACE IN IT: "RaceSex Menu".
            // This read "RaceSexMenu" from the day F-9 was written, so BOTH
            // of these branches were dead code and raceMenuOpen_ was never
            // once set - so the camera and the preview spin would have
            // fought RaceMenu for the mouse the moment anyone bubbled it.
            // Taken from CommonLib's constant now so it cannot drift again.
            if (name == RE::RaceSexMenu::MENU_NAME) {
                raceMenuOpen_ = true;
                spdlog::info("RaceSexMenu bubbled (EXPERIMENTAL, F-9): anim/face/caster "
                             "ticks defer to the engine's racemenu mode; input gate off.");
                // ⚠ THE STUDIO CAMERA STANDS DOWN AT THIS DOOR (2026-08-06,
                // the player's call after a night of fixes that each held
                // and none of which ended it). The editor is the one bubbled
                // menu that brings a COMPLETE camera of its own - LAlt, its
                // prompts, a whole Camera tab - and a re-stamp that owns the
                // node makes that tab dead by construction: no fix on our
                // side can make another mod's camera UI drive our track. The
                // one open tonight where this layer never captured was also
                // the one the field called correct.
                //
                // A live arm carried in from the inventory is released HERE,
                // on the open edge, while the framing it captured over is
                // still the shot its revert describes - and the editor's own
                // camera takes the node unopposed from the first frame.
                // StudioCamera::Tick refuses this menu outright, so an
                // editor-first arm can never capture in there either.
                StudioCamera::Disarm();
            }
            spdlog::info("Bubble menu opened: {} (open count {}).", name, menusOpen_.load());
            // F-30 round 3 instrument. THE SCENE SIDE IS EXHAUSTED: with every
            // node under the cell root app-culled (10840 of them, flags held,
            // 2026-08-20 05:18) the inn still filled the frame, and the one
            // frame of the close showed the studio state that the whole arm's
            // measurements said was live. The only thing that can put a lit
            // room on screen past a fully culled scene is a FROZEN FRAME of
            // the pre-menu view, and menus can ask for exactly that
            // (kFreezeFrameBackground / kFreezeFramePause). So name the
            // opening menu's flags on every open: a flag that differs between
            // a broken arm and a healthy one is the whole diagnosis.
            if (const auto ui = RE::UI::GetSingleton()) {
                if (const auto menu = ui->GetMenu(name)) {
                    spdlog::info("menu flags [{}]: 0x{:08X} | pausesGame={} "
                                 "freezeFrameBackground={} freezeFramePause={} "
                                 "blurredBackground={} rendersUnderPauseMenu={} "
                                 "customRendering={} | ui numPausesGame={}",
                                 name, menu->menuFlags.underlying(),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kFreezeFrameBackground),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kFreezeFramePause),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kUsesBlurredBackground),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kRendersUnderPauseMenu),
                                 menu->menuFlags.all(RE::UI_MENU_FLAGS::kCustomRendering),
                                 RE::UI::GetSingleton()->numPausesGame);
                }
            }
            // A fresh menu session gets a fresh enter/leave tally, so the log
            // line below counts pairs WITHIN one menu - which is the lifecycle
            // nobody has exercised before this change.
            studioEnterCount_ = 0;
            // ⚠ THE MENU SESSION ENDS HERE AND THE STUDIO SESSION BEGINS. Above
            // this line is bookkeeping the close balances against; below it is
            // everything the bubble OWNS and has to hand back. With
            // bWaitForOwnerContext off the two are the same moment, exactly as
            // they always were.
            studioGateHeld_ = GateHoldsStudioDown();
            if (studioGateHeld_.load()) {
                // Vanilla, deliberately and visibly. A decline nobody can see
                // is indistinguishable from a crash or a bad build - the r28h
                // lesson, which this early-out is the same shape as.
                spdlog::info("Bubble menu {} opens WITHOUT the studio: "
                             "bWaitForOwnerContext is on and no mod is holding a "
                             "context. No pause, no camera, no scene. The strip "
                             "still draws whatever another mod put on it.",
                             name);
            } else {
                EnterStudioSession(name, "menu open");
            }
        } else {
            // ⚠ ABOVE THE COUNTED-MENUS EARLY RETURN, AND THAT IS THE WHOLE
            // POINT. Two reasons, and the first one is a bug this sat on for
            // an afternoon:
            //
            //   1. 'RaceSex Menu' ships OFF in sMenus, so countedMenus_ never
            //      holds it and the erase below returns 0 and leaves. Wired
            //      inside that branch, this probe was unreachable in the
            //      shipped configuration - a field run that could only ever
            //      come back empty.
            //   2. The unbubbled close is the CONTROL. If the editor's camera
            //      keeps writing after its menu closes whether or not we
            //      bubbled it, that is vanilla behaviour and not our bug; if
            //      it only does so when we did, it is. One flag on the line
            //      separates them, and it costs nothing to collect both.
            //
            // ⚠ HERE RATHER THAN IN Disarm for a third reason: the symptom
            // starts at THIS edge and has to be measured from the first frame
            // after it. Disarm runs later, past restores that are themselves
            // candidates - opening the watch first puts them inside the window
            // where they get attributed instead of assumed.
            //
            // ⚠ AND A FOURTH REASON, WHICH IS AN ORDERING CONSTRAINT RATHER
            // THAN A PREFERENCE: this must stay ABOVE the zero-frame sleek cut
            // further down. On the shipped defaults that branch runs
            // OwnView::Disarm and Bubble::Disarm IN THIS CALL STACK, and Disarm
            // is what signals the probe's teardown fuse. NoteTeardownDone
            // early-returns while the watch is closed, so arming below it would
            // drop the signal outright and every zero-frame exit would fall
            // through to the ceiling instead of grading a settled teardown.
            //
            // No-op unless bCameraCloseProbe is on.
            if (Settings::GetSingleton().IsBubbleMenu(name) ||
                name == RE::RaceSexMenu::MENU_NAME) {
                CameraCloseProbe::OnBubbleMenuClosed(
                    name, countedMenus_.contains(name), armedLastFrame_,
                    sessionDormant_, sessionLiveOnly_, preMenuFreeRotValid_);
            }
            // Decrement against what we counted, not against the live predicate.
            if (countedMenus_.erase(name) == 0) {
                return RE::BSEventNotifyControl::kContinue;  // never ours - nothing to give back
            }
            const int now = std::max(0, menusOpen_.load() - 1);
            menusOpen_ = now;
            if (name == RE::RaceSexMenu::MENU_NAME) {
                raceMenuOpen_ = false;
            }
            ForcePause::OnMenuClosed(name, OwnView::ExternalProviderCovers(name));
            // ⚠ FORGOTTEN, NOT RELEASED, AND THE DIFFERENCE IS A DOUBLE
            // DECREMENT. OnMenuClosed above hands this menu to the per-frame
            // settle, which reconciles the counter against the engine's own
            // invariant once the menu has actually left the stack. A studio
            // session that ALSO paid it back here would take the same increment
            // twice, and numPausesGame is unsigned: past zero it wraps to a
            // huge value that still reads "> 0" and the world never resumes.
            studioPauses_.Forget(name);
            if (now == 0) {
                // The menu session is over, so the studio session is too. The
                // teardown itself is NOT done here - the exit choreography
                // below and in OnFrame owns that, and it runs off
                // armedLastFrame_ and the gate-hold frames exactly as before.
                // This is bookkeeping only.
                //
                // ⚠⚠ EXCEPT THAT THE HUD IS NOT BOOKKEEPING. This line ends the
                // studio session without going through LeaveStudioSession, and
                // it is the route the field actually takes - the 2026-08-28 log
                // has seven ENTERs and no LEAVE at all. Everything else the
                // session took is handed back by the choreography or by the
                // per-frame settle; the HUD had no such second owner, so it
                // simply stayed hidden into gameplay and the player lost their
                // compass and crosshair for the rest of the run.
                studioEntered_ = false;
                RestoreHudIfHidden("the last bubble menu closed");
                (void)ItemPreviewBroker::SetClaim("MenuStudio.Settings", false);
                studioGateHeld_ = false;
                if (!studioPauses_.Empty()) {
                    // Every close forgets its own name one line above, so a
                    // ledger with anything left in it means a close event never
                    // arrived for a menu we took a pause for. Cleared WITHOUT
                    // paying: the per-frame settle reconciles those against the
                    // engine's own invariant, and paying here as well is the
                    // double decrement this whole ledger exists to prevent.
                    spdlog::warn("Bubble: the studio session's pause ledger still held "
                                 "{} entr(y/ies) when the last menu closed. A close "
                                 "event was missed. Left to the ForcePause settle.",
                                 studioPauses_.Size());
                    studioPauses_.Clear();
                }
                // ⚠ THE OWNER SET DIES WITH THE MENU. An owner that publishes a
                // context and then unloads, crashes, or simply forgets to
                // withdraw would otherwise pin the studio open for the rest of
                // the game session, and the player would have no way to tell
                // why their inventory kept freezing the world. The PROOF is
                // deliberately not cleared: it is what the gate is armed by,
                // and something has already demonstrated it can lift the
                // studio, which no later silence unproves.
                if (!ownerContexts_.empty()) {
                    spdlog::info("owner context: {} live context(s) dropped because the "
                                 "last covered menu closed. An owner that wants the "
                                 "studio again publishes again.",
                                 ownerContexts_.size());
                    ownerContexts_.clear();
                }
            }
            // B-2/B-8 v2 exit mirror: DEFERRED past the switch window. At
            // close time a real exit and a menu switch look identical, and
            // firing here restarted the walk on every switch (field r23;
            // gaps measured 52-71 ms). OnFrame fires it at ~85 ms if no
            // bubble menu re-opened; conditions re-checked live there.
            // r40 (field: "rain sfx stops when i exit the menu"): the sky
            // mode goes back RIGHT HERE, before a single unpaused frame -
            // Sky::Update ticking kNone in the exit's hold+dip window made
            // the engine stop the weather's rain loop, and steady rain has
            // no transition to re-trigger it. Visually free: the shell
            // keeps occluding the sky until the at-black teardown. A
            // switch re-open re-parks (open path above).
            if (now == 0 && armedLastFrame_) {
                // The close-edge sample for the stale-park guard. ABOVE the
                // zero-frame cut on purpose: when Disarm runs in this call
                // stack the payment sees live == atClose and pays exactly as
                // before; only a payment DEFERRED past other restorers can
                // find a field moved and stand aside. Slot object, not the
                // current state - a first-person arm still has the fields.
                if (auto* tps = ThirdPersonSlotState()) {
                    atCloseAnimCam_ = tps->toggleAnimCam;
                    atCloseFreeRotEnabled_ = tps->freeRotationEnabled;
                    atCloseFreeRotX_ = tps->freeRotation.x;
                    atCloseViewValid_ = true;
                }
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    atClosePitch_ = pc->data.angle.x;
                }
                if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                    atCloseFov_ = camera->worldFOV;
                }
                StudioLight::RestoreSkyModeEarly();
                // r50: with hold 0 the cut happens IN THIS CALL STACK -
                // the OnFrame machine ran one frame late (field: "quite
                // literally one frame where i see this after i close").
                // Same mechanism as the open-side r25 lesson: mutate
                // before the frame renders, and there is no frame to see.
                const auto& s = Settings::GetSingleton();
                if (s.sleekExit && s.declutterMode >= 2 &&
                    s.exitHoldSeconds <= 0.0f) {
                    if (pendingOwnViewRestore_) {
                        pendingOwnViewRestore_ = false;
                        OwnView::Disarm();
                    }
                    Transition::Snap(0.0f);
                    Disarm();
                    spdlog::debug("sleek exit: cut in the close event "
                                  "(zero-frame).");
                }
            }
            if (now == 0 && armedLastFrame_) {
                // Deferred teardown: menu SWITCHES (inventory -> magic) close
                // one bubble menu and open the next a frame or two apart.
                // Tearing down in that gap let the collision smoother press
                // the camera onto the player and churned declutter/lighting.
                // A short grace bridges the gap for the visual teardown; the
                // camera gate holds longer (the close->open gap of a switch
                // can span more frames than the grace - field-measured).
                graceFrames_ = 6;
                gateHoldFrames_ = 30;  // ~0.5 s
                // F-15 r35: the r33 close-event restore fixed the EXIT
                // flash but created a SWITCH flash - the unpaused gap
                // frames rendered the restored heading, then the re-frame
                // snapped back ("a frame where our character is not in the
                // right rotation"). Defer the restore past the measured
                // switch gap; a re-open cancels it and the SAME framing
                // simply stays up (seamless switch). A real exit keeps the
                // framed pose ≤85 ms under the menu-close fade - the
                // lesser residual of the two.
                if (OwnView::Active()) {
                    pendingOwnViewRestore_ = true;
                    ownViewRestoreQpc_ = QpcNow();
                }
                // F-12: start the exit dissolve - UNLESS the v4 black-fade
                // exit will run (it holds the studio at full and covers the
                // restore with the fader instead; a dissolve underneath it
                // would blank the pieces early). With transitions off (or
                // out=0) nothing is started either: the grace holds the
                // pieces exactly as pre-F-12.
                const auto& s = Settings::GetSingleton();
                const bool dipExit = s.sleekTransitions && s.dipToBlack &&
                                     s.declutterMode >= 2;
                if (!dipExit && s.sleekTransitions && s.transitionOutSeconds > 0.0f) {
                    Transition::SetTarget(0.0f);
                }
            }
            spdlog::info("Bubble menu closed: {} (open count {}).", name, now);
            // ⚠⚠ THE OTHER END OF THE CAMERA DEBT WATCH. Its twin fired when a
            // teardown threw the capture away with this menu still open; this
            // is the moment that would have paid it, and the numbers beside it
            // are what the player is now left holding. A line here with every
            // field equal means the framing stood down on its own and the early
            // spend cost nothing; anything else is the leak, sized.
            //
            // The count, not the name: a menu SWITCH closes one and opens the
            // next, and the debt belongs to the whole menu session rather than
            // to whichever menu happened to be last.
            // ⚠⚠ THE AGE OF THE SPEND IS WHAT MAKES IT PREMATURE, not where it
            // happened. A healthy close tears down in this same call stack, so
            // the spend is microseconds old and there is nothing to report. A
            // spend the framing outlived is seconds old (the field log has
            // 1.77 s), and that is the whole finding. Reading the clock rather
            // than the menu count also survives the case where the count went
            // to zero early, which is the case that fooled the first cut.
            const float spendAge =
                debtSpentEarly_ ? QpcSeconds(debtSpentQpc_, QpcNow()) : 0.0f;
            if (now == 0 && debtSpentEarly_ && spendAge > 0.10f) {
                debtSpentEarly_ = false;
                auto*       camera = RE::PlayerCamera::GetSingleton();
                auto*       pc = RE::PlayerCharacter::GetSingleton();
                auto*       tps = ThirdPersonSlotState();
                const float liveFov = camera ? camera->worldFOV : 0.0f;
                const float liveFreeRot = tps ? tps->freeRotation.x : 0.0f;
                const float livePitch = pc ? pc->data.angle.x : 0.0f;
                spdlog::warn(
                    "camera debt: THE CLOSE FOUND NOTHING OWED. The capture was "
                    "spent while a menu was still open, so these are the "
                    "gameplay values the player keeps: fov {:.1f} (was {:.1f}, "
                    "off by {:+.1f}), freeRot {:.3f} (was {:.3f}, off by "
                    "{:+.3f}), pitch {:.3f} (was {:.3f}, off by {:+.3f}), "
                    "animCam {} (was {}), freeRotEnabled {} (was {}), zoom "
                    "{:.2f}. The capture was {:.2f}s stale by the time this "
                    "close arrived. A camera that does not follow the character "
                    "is freeRotEnabled true with freeRot standing still.",
                    liveFov, debtShadowFov_, liveFov - debtShadowFov_,
                    liveFreeRot, debtShadowFreeRot_,
                    liveFreeRot - debtShadowFreeRot_, livePitch,
                    debtShadowPitch_, livePitch - debtShadowPitch_,
                    tps ? tps->toggleAnimCam : false, debtShadowAnimCam_,
                    tps ? tps->freeRotationEnabled : false,
                    debtShadowFreeRotEnabled_,
                    tps ? tps->targetZoomOffset : 0.0f, spendAge);
            } else if (now == 0) {
                debtSpentEarly_ = false;  // paid in the close's own call stack
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    void Bubble::OnFrame(RE::Main* a_main) {
        const auto& settings = Settings::GetSingleton();
        // THE FORCE-PAUSE SETTLE RUNS FIRST, ABOVE EVERY EARLY RETURN BELOW.
        // This used to sit further down, past `if (menusOpen_ <= 0) { ... return; }`
        // and the whole exit choreography, which meant it only ever ran WHILE A
        // COVERED MENU WAS STILL OPEN. But settling closed menus is its entire
        // job: ForcePause takes a numPausesGame++ on open, OnMenuClosed is a
        // deliberate no-op, and Reassert is the ONLY thing that ever gives that
        // increment back. So closing the LAST covered menu dropped menusOpen_ to
        // 0, OnFrame returned before the settle, and the +1 was never reclaimed -
        // the world stayed frozen after the menu closed. Closing one of TWO open
        // menus always worked (the counter stayed above 0, so the settle still
        // ran), which is why this survived the 0.6.0 rework: the logic was right
        // and simply unreachable in the one case it was written for. It also
        // defeated that code's own "ticks while frozen, so it self-heals a stuck
        // session" intent, since a stuck session has no menu open by definition.
        //
        // Cheap unconditionally: Reassert early-outs when it holds nothing. Above
        // the `enabled` check too, so switching the mod off while holding a pause
        // releases it instead of stranding the world frozen.
        ForcePause::Reassert();
        // Close-event sink ordering is not ours to control. SPII/SPIM may restore
        // their gameplay camera after Bubble::Disarm returned, so the old inline
        // current=target snap could run against their MENU targets and be undone
        // later in the same dispatch. This is the first safe point: every close
        // sink has completed, while the 85 ms switch/pause bridge still keeps the
        // first gameplay render from exposing an interpolated camera.
        ReconcileExternalViewAfterClose();
        // ⚠ THE LAST GAMEPLAY FRAME, every frame, gated inside. See the note on
        // lastGameplay_ in Bubble.h: no menu-open edge fires before the tween
        // menu has the camera, so the tween's open edge copies this instead.
        SampleGameplayCamera();
        // ⚠ ABOVE THE `enabled` GATE, for the same reason ForcePause::Reassert
        // is. This is the ONLY thing that can ever close the camera watch, while
        // the four PlayerCamera doors that write into it gate on Watching()
        // alone, from detours installed unconditionally at load. Below the gate,
        // unticking the mod mid-window strands the watch open until a save load
        // and the doors keep counting into a ledger whose frame denominator is
        // frozen, so the next verdict is an inflated rate against a stale count.
        //
        // It measures GAMEPLAY frames after a close, so it must not sit anywhere
        // the menu count can skip either. Self-gating: one relaxed atomic load
        // and an immediate return unless a close opened it.
        CameraCloseProbe::Tick();
        // The broker is a public coordination surface, so external claims stay
        // enforced even when Menu Studio itself is disabled or has no studio
        // session. Releasing this plugin's own claim above the enabled return
        // also prevents a live settings change from stranding it.
        const bool hideItemPreview = ItemPreviewPolicy::LocalClaimWanted(
            settings.enabled, menusOpen_.load() > 0, studioEntered_.load(),
            settings.disableItemPreview3D);
        (void)ItemPreviewBroker::SetClaim("MenuStudio.Settings", hideItemPreview);
        ItemPreviewBroker::Reconcile();
        if (!settings.enabled || !a_main) {
            graceFrames_ = 0;
            Disarm();
            // F-26 r2: this path returns every frame while the mod is off, so
            // the timed fire site below is unreachable - an outstanding sheathe
            // would hang here forever and leave the player permanently drawn.
            // No switch can follow a disable either, so pay it now.
            FireDeferredWeaponRestore(true);
            return;
        }
        // F-26 r2: OUTSIDE the menu-count block on purpose. The sheathe must
        // also be payable while a menu is still open but the bubble is dormant
        // (Souls unpause, missing 3D) - see the guard inside, which uses
        // armedLastFrame_ rather than the menu count.
        // DIAGNOSTIC (2026-07-20), OFF unless bDiagnosticProbes. Retried every
        // frame because both no-op once they have taken, and the graph does not
        // exist before the 3D loads and is rebuilt across a save load.
        // ClipProbe is NO LONGER a diagnostic - EquipClipInFlight() is the
        // signal the r36 freeze uses to tell "the draw clip is still running"
        // from "the weapon state says drawn", which r11 proved are not the same
        // thing. It installs and tracks unconditionally; only its LOGGING is
        // still gated on bDiagnosticProbes, so the cost when probes are off is
        // one pointer compare per clip activation.
        ClipProbe::Install();
        ClipProbe::TrackPlayerGraphs();
        // The editor button's owed open. Above every early return below: the
        // whole point is that it fires once the menus are DOWN and this
        // bubble is no longer armed, so gating it on the armed path would
        // mean it could never run.
        MTB::ActionBar::Tick();
        if (Settings::GetSingleton().diagnosticProbes) {
            // Both sides of the arm flag on purpose: the whole question is what
            // these markers do in a menu that they do not do live, and a
            // measurement with no control has already misled this channel once
            // today (a 3.33 s idle turned out to fire live too).
            ClipProbe::LogStanceMarkers(armedLastFrame_);
        }
        if (Settings::GetSingleton().diagnosticProbes) {
            // Idempotent and no-op once taken. These moved here from plugin
            // init, where the INI had not been read yet and they could never
            // fire - see the note at Install().
            ActorTickProbe::Install();
            AnimEventProbe::Install();
        }
        // ⚠ THE CAMERA TICKS BEFORE THE PAUSE DOES. Everything below the armed
        // branch waits for the world to freeze, and that takes most of a second
        // while the view mod slides its own framing into place. Input already
        // accumulates from the first frame, but with nothing applying it the
        // player drags and watches nothing happen, which is the whole of "no
        // orbit immediately". Once the pause lands the armed branch takes over
        // and this stands down, so the camera is ticked exactly once a frame
        // either way.
        //
        // The subject is the player here rather than a framed companion: the
        // companion is named by another mod after the menu is up, and this path
        // only exists for the handful of frames before that has happened.
        // ⚠ THE MISSING-RELEASE WATCH. FLICK swallows the whole input dispatch
        // while a window without kPassInputToGame is up, and Fitting Room's
        // editor drops that flag the frame the button opens - so the release
        // of a swing ended over the world can die exactly like its press edge
        // did. A latched drag that produced no left-pressed event for two
        // consecutive frames is over; held buttons repeat every frame, so a
        // real hold cannot look like this. The owed release is dropped WITH
        // the latch: the dispatch was blocked, the menu never saw the press,
        // and a stale debt would eat the next legitimate click.
        {
            const bool leftEvidence = camLeftEvidence_.exchange(false);
            if (camOrbitDragging_.load() && !leftEvidence) {
                if (++camSilentTicks_ >= 2) {
                    camOrbitDragging_ = false;
                    camHoldDecided_ = false;
                    camOwedLeftUp_ = false;
                    camSilentTicks_ = 0;
                }
            } else {
                camSilentTicks_ = 0;
            }
            // The pan latch needs the same watch for the same reason. Its
            // release can die in the swallowed dispatch exactly like the
            // swing's, and a stuck pan is worse than a stuck swing: it moves
            // the shot on every mouse move with nothing held down.
            const bool middleEvidence = camMiddleEvidence_.exchange(false);
            if (camPanDragging_.load() && !middleEvidence) {
                if (++camPanSilentTicks_ >= 2) {
                    camPanDragging_ = false;
                    camPanTravel_ = 0.0f;
                    camPanSilentTicks_ = 0;
                }
            } else {
                camPanSilentTicks_ = 0;
            }
        }
        // The studio session's own lifetime, decided once a frame. Above the
        // early camera tick and the item-preview switch below, because both of
        // those are the studio's and must not run in a menu it has not entered.
        //
        // ⚠ IT IS A NO-OP WITH NO MENU COUNTED, WHICH IS WHY IT IS SAFE THIS
        // HIGH. The exit choreography further down runs with menusOpen_ at 0,
        // and a reconcile that tore the studio down there would cut every menu
        // switch in half - so the menu close owns that edge instead. See
        // ChooseMove's note on why the no-menu case is deliberately "do
        // nothing" rather than "leave".
        ReconcileStudioSession();
        if (!armedLastFrame_ && menusOpen_.load() > 0 && studioEntered_.load() &&
            Settings::GetSingleton().studioCamera) {
            static std::uint64_t s_lastQpc = 0;
            const std::uint64_t  now = QpcNow();
            const float earlyDt = s_lastQpc ? QpcSeconds(s_lastQpc, now) : 0.016f;
            s_lastQpc = now;
            StudioCamera::Tick((std::min)(0.1f, earlyDt),
                               RE::PlayerCharacter::GetSingleton());
        }
        // The item-preview kill switch: while a bubble menu is up, anything
        // the engine's inventory 3D stage loads is cleared the frame it
        // appears. Clearing rather than hooking the load, because Fitting
        // Room's editor already proved Clear3D is the supported way to empty
        // this stage, and a per-frame clear turns "the preview never shows"
        // into ordinary state instead of a patched code path. Guarded on the
        // stage actually holding something, so the toggle costs nothing while
        // the player is not hovering a list.
        // ⚠ THE INSPECT WATCH. Two mechanisms have been aimed at the stray
        // inspect and missed, and both failed the same way: silently, in a build
        // that compiled and deployed cleanly. A blanked user event the wheel
        // does not carry looked exactly like a working gate, and so did a
        // refusal at Inventory3DManager that something else went around.
        //
        // This says when the engine's zoom actually left zero and whether a
        // wheel notch had just happened, which is the one fact that separates
        // "the gate works" from "the gate is not on the path". Bounded per
        // session: the first few openings answer the question and the rest are
        // noise. Costs one float compare a frame otherwise.
        if (menusOpen_.load() > 0) {
            if (auto* inv = RE::Inventory3DManager::GetSingleton()) {
                auto&       invData = inv->GetRuntimeData();
                const float zoom = invData.zoomProgress;
                if (lastZoomProgress_ == 0.0f && zoom > 0.0f) {
                    const auto  stamp = wheelSeenQpc_.load();
                    const float sinceWheel =
                        stamp ? QpcSeconds(stamp, QpcNow()) : -1.0f;
                    // ⚠ 0.012 s IS THE MEASUREMENT THIS WINDOW IS BUILT ON.
                    // Field 2026-08-13: the zoom left 0 twelve milliseconds
                    // after the notch. A quarter of a second is wide enough to
                    // survive a frame hitch and far too narrow to catch the
                    // inspect CONTROL, which the player presses in its own time
                    // with no wheel anywhere near it. That gap is the whole
                    // discriminator: we do not need to know which key inspect
                    // is bound to, only that a wheel notch did not just happen.
                    constexpr float kWheelInspectWindow = 0.25f;
                    const bool wheelOpenedIt =
                        sinceWheel >= 0.0f && sinceWheel <= kWheelInspectWindow;
                    const auto& s = Settings::GetSingleton();
                    // ⚠⚠ THIS ONCE CALLED Clear3D HERE AND IT TRAPPED THE PLAYER
                    // IN THE MENU. The reasoning was that clearing the stage
                    // undoes the entry whatever opened it. It does not: Clear3D
                    // removes the MODEL and leaves the inspect STATE running.
                    // Field 2026-08-13, in one second - stage cleared at .574
                    // with the zoom at 0.010, and the zoom then climbed 0.020,
                    // 0.058, 0.096, 1.000 with nothing on the stage to look at.
                    // Fully inspecting an empty stage, and no way out.
                    //
                    // ⚠ SO NOTHING IS UNDONE FROM HERE, EVER. An entry that has
                    // already happened has to be refused BEFORE it starts - see
                    // the Scaleform wheel gate in MenuInputGate.cpp, which is
                    // where the same field run proved the entry actually comes
                    // from. This block observes and says what it saw.
                    if (inspectOpensLogged_ < 6) {
                        ++inspectOpensLogged_;
                        spdlog::info(
                            "inspect watch #{}: the engine's item zoom left 0 (now "
                            "{:.3f}) in {}. Last wheel notch {} -> {}. "
                            "bInspectNeedsHotkey={}, studio session {}.",
                            inspectOpensLogged_, zoom, currentMenuName_,
                            sinceWheel < 0.0f
                                ? std::string{ "never seen this session" }
                                : fmt::format("{:.3f} s ago", sinceWheel),
                            wheelOpenedIt
                                ? "THE WHEEL opened it, the Scaleform gate let one "
                                  "through"
                                : "not the wheel (the inspect control, most likely)",
                            s.inspectNeedsHotkey, studioEntered_.load() ? "up" : "down");
                    }
                }
                // Re-read from the live stage so the transition detector never
                // carries a stale copy into the next frame.
                lastZoomProgress_ = invData.zoomProgress;
            }
        }
        FireDeferredWeaponRestore();
        // Runs disarmed as well as armed - see WatchWeaponStateAfterClose for
        // why the window has to outlive the menu.
        WatchWeaponStateAfterClose(armedLastFrame_, QpcNow(), &QpcSeconds);
        // DIAGNOSTIC (2026-07-20): the control sample. Disarmed only - an armed
        // frame is the case under investigation, and the whole point of this
        // call is to capture the world getting a weapon swap RIGHT so the menu
        // case has something real to be diffed against. Early-outs on the first
        // pointer compare when nothing changed.
        if (!armedLastFrame_) {
            WeaponPreview::ObserveUnarmed(RE::PlayerCharacter::GetSingleton());
        }
        // r19c SELF-HEAL. A menusOpen_ that never reaches 0 strands an armed
        // bubble in gameplay, and the player cannot recover from it without a
        // reload - the exact failure just field-reported. The counted-set fix
        // above closes the known cause (a predicate that changed mid-session),
        // but ANY missed close event produces the same unrecoverable state, so
        // reconcile against the UI itself rather than trusting the bookkeeping.
        //
        // Deliberately slow: a menu can lag the UI map right after its open
        // event (the r14 race), so a single absent frame proves nothing. Only a
        // menu absent for a full second is treated as gone.
        if (menusOpen_.load() > 0) {
            auto* ui = RE::UI::GetSingleton();
            bool anyReallyOpen = false;
            if (ui) {
                for (const auto& counted : countedMenus_) {
                    if (ui->IsMenuOpen(counted)) {
                        anyReallyOpen = true;
                        break;
                    }
                }
            }
            if (anyReallyOpen || !ui) {
                orphanFrames_ = 0;
            } else if (++orphanFrames_ >= 60) {
                spdlog::warn("Bubble: menusOpen_={} but none of the {} counted menu(s) has "
                             "been open for a second. A close event was missed. Reconciling "
                             "to 0 so the studio cannot outlive its menu.",
                             menusOpen_.load(), countedMenus_.size());
                orphanFrames_ = 0;
                countedMenus_.clear();
                menusOpen_ = 0;
                // The studio session cannot outlive its menu session either.
                // ReleaseAll below hands back every pause we hold, ledger
                // entries included, so the ledger is cleared rather than
                // drained - draining would pay a second time.
                //
                // ⚠ THE HUD IS PAID HERE AND THE PAUSE IS NOT, WHICH LOOKS
                // INCONSISTENT AND IS NOT. The pause is refcounted engine state
                // with a second owner that reconciles it; the alpha is a plain
                // value nobody else writes, so an unpaid one stays at zero
                // forever. A missed close is exactly the case where the ordinary
                // payer never runs.
                studioEntered_ = false;
                RestoreHudIfHidden("a close event was missed and the count was reconciled");
                (void)ItemPreviewBroker::SetClaim("MenuStudio.Settings", false);
                studioGateHeld_ = false;
                studioPauses_.Clear();
                // ⚠ AND TELL THE PAUSE OWNER, OR THE WORLD STAYS FROZEN.
                // This is the only place in the plugin that reconciles the
                // counted set against the live UI map, and ForcePause has no
                // such reconcile of its own on purpose (a map-keyed purge would
                // evict a menu that merely lags the map, the r14 race). Its own
                // covered set is still holding this menu's freeze and re-asserts
                // it every frame, and nothing is left that can ever release it:
                // the real close event, if it ever arrives, now finds an empty
                // counted set and returns early.
                //
                // Under Skyrim Souls that is a frozen world with no UI on
                // screen and no way back except a reload. The 60-frame rule
                // above is the bounded evidence that makes eviction safe HERE
                // and unsafe inside ForcePause.
                ForcePause::ReleaseAll();
            }
        } else {
            orphanFrames_ = 0;
        }
        if (menusOpen_.load() <= 0) {
            if (gateHoldFrames_.load() > 0) {
                --gateHoldFrames_;
            }
            const bool graceLive = graceFrames_.load() > 0;
            if (graceLive) {
                --graceFrames_;
            }
            if (armedLastFrame_) {
                const auto& s = Settings::GetSingleton();
                // r38 SLEEK EXIT: the r26 exit machine returns on its own
                // switch (bSleekExit), decoupled from the parked open-side
                // dip. r37 field made the need visible in exteriors: the
                // legacy path ran every restore in FULL VIEW - terrain/LOD
                // popped back as black holes for the frames between the UI
                // closing and Disarm ("for a few frames i can see this").
                // The machine is also the SKILLS MENU's own exit
                // choreography (StatsMenu::ProcessMessage case 3 calls the
                // same fader on close - mtb_statsmenu.c), which is the
                // presentation the user keeps pointing at.
                const bool exitMachine = s.sleekExit && s.declutterMode >= 2;
                // F-15 r35: the deferred view restore fires once the switch
                // window passed - this close was a real exit. With the exit
                // machine on it fires AT BLACK instead (the first-person
                // snap was visible in the first darkening frames).
                if (!exitMachine && pendingOwnViewRestore_ &&
                    QpcSeconds(ownViewRestoreQpc_, QpcNow()) >= 0.085f) {
                    pendingOwnViewRestore_ = false;
                    // ⚠ THE STUDIO CAMERA GOES DOWN FIRST, AND THE ORDER IS
                    // THE WHOLE FIX. OwnView::Disarm writes the third-person
                    // state's inputs back (zoom offset, free rotation, pos
                    // offset) and calls camera->Update once. Our re-stamp
                    // overwrites the camera node EVERY FRAME while it is
                    // armed, so a restore landing here used to be undone
                    // before the engine ever rebuilt from it.
                    //
                    // Measured, 2026-08-05 20:10:57: the editor closed at
                    // .784, own view restored at .876, and the studio camera
                    // did not release until .923 - 47 ms and six frames of
                    // re-stamping on top of the restore. The camera then
                    // settled ~70 units from where the editor was entered
                    // with the probe's verdict reading THE ENGINE IS ACTIVELY
                    // REBUILDING THE SHOT, which is what a correct engine
                    // rebuilding from parked inputs looks like.
                    //
                    // This is also exactly why the Void and the Dressing room
                    // never reproduced it. Their exit machine (above) runs
                    // OwnView::Disarm and the teardown in ONE call stack with
                    // no frame between, so nothing can re-stamp in the gap.
                    // The legacy path cannot do that, because the teardown has
                    // to wait for the dissolve - so the camera stands down on
                    // its own, here, and the visual teardown follows later.
                    //
                    // Idempotent: Bubble::Disarm calls this again at the end
                    // of the dissolve, and a second call finds the state reset
                    // and logs nothing.
                    StudioCamera::Disarm();
                    OwnView::Disarm();
                }
                // r31 field: menu SWITCHES stutter the spun character for a
                // split frame - the close→open gap runs UNPAUSED, so the
                // engine's own player update re-syncs the node from
                // data.angle (the pinned entry heading) and our armed tick
                // isn't running to re-compose. This hook site sits right
                // after that engine update: re-assert before the frame
                // renders. (The vfunc-0x3F hook covers explicit
                // Update3DPosition calls; the unpaused frame path isn't
                // guaranteed to route through it.)
                ReassertSpin();
                // r44 (field: "during the transition we see the skybox"):
                // these gap/exit frames run UNPAUSED - Sky::Update un-culls
                // its branch each frame; pin the world-feeder culls back
                // down until the at-black restore hands the sky over for
                // the fade-in.
                Declutter::ReassertWorldFeederCulls();
                // Closed mid-arm-dip (black since open, insta-closed): the
                // build never ran - bring the light back before anything.
                CancelDipIfActive();
                const auto now = QpcNow();
                const float dt = std::clamp(QpcSeconds(lastQpc_, now), 0.0f,
                                            Settings::GetSingleton().maxDeltaTime);
                lastQpc_ = now;
                if (exitMachine) {
                    // r47 EXIT v3 - THE INSTANT CUT (user spec: "the
                    // constellation and void should not even be visible
                    // the moment we switch out of the menu"). The fader
                    // choreography is DEAD: four rounds of field evidence
                    // say its black never reliably covers under ENB -
                    // every "restored under the black" was partially
                    // visible (r38 terrain holes, r43 skybox, r46 "entire
                    // menu view"). Hold the studio through the switch
                    // window (switches cancel for free, r36 seamlessness
                    // untouched), then restore EVERYTHING in one frame:
                    // world, studio, camera, all before the next render.
                    // Vanilla menus cut; so do we.
                    if (exitPhase_ == 0) {
                        exitPhase_ = 1;
                        exitQpc_ = now;
                    }
                    if (exitPhase_ == 1 &&
                        QpcSeconds(exitQpc_, now) >= s.exitHoldSeconds) {
                        exitPhase_ = 0;
                        if (pendingOwnViewRestore_) {
                            pendingOwnViewRestore_ = false;
                            OwnView::Disarm();
                        }
                        Transition::Snap(0.0f);
                        Disarm();
                        spdlog::debug("sleek exit: instant cut at {:.0f} ms "
                                      "(hold window passed).",
                                      s.exitHoldSeconds * 1000.0f);
                        return;
                    }
                    return;  // holding through the switch window
                }
                // Legacy exit (no dip): the dissolve runs THROUGH the grace
                // window - only fade values move; teardown holds until the
                // ramp lands (QPC-bounded).
                Transition::Tick(dt);
                StudioRig::PushFade();
                Backdrop::PushFade();
                if (graceLive || Transition::FadingOut()) {
                    return;  // bridging a switch / dissolving - hold state
                }
            }
            Disarm();
            return;
        }
        graceFrames_ = 0;

        // (The force-pause reconcile moved to the TOP of OnFrame - see there.
        // Here it could never settle a menu that had already left the stack.)

        auto* ui = RE::UI::GetSingleton();
        const bool paused = a_main->freezeTime || (ui && ui->GameIsPaused());

        // r18 - THE ARM DECISION IS LATCHED PER SESSION, NOT POLLED PER FRAME.
        //
        // This branch used to re-read the GLOBAL pause every frame and Disarm()
        // whenever it read false. That coupled the whole visual scene (void,
        // backdrop, declutter, studio light) to a global that four other actors
        // write: Skyrim Souls, the close/open gap of a menu switch, our own
        // force-pause, and ANY unrelated pausing menu the player opens on top.
        // Field 2026-07-20 09:55-09:58 measured the consequence: ONE
        // InventoryMenu stayed open for three minutes with zero menu events,
        // and the bubble armed and tore down TEN times - every console, journal,
        // tween and system menu built the entire void and then destroyed it.
        // Nine of fifteen arms that session came from a pause we did not own
        // (freezeTime=false numPauses=1). That is the "janky as fuck" report,
        // and it is also why the void showed up behind the console.
        //
        // The scene's lifetime belongs to the MENU SESSION, not to the pause.
        // So the decision is made once, on the first frame of a session, and
        // held: dormant sessions stay dormant no matter what pauses later, and
        // armed sessions keep their scene no matter what unpauses later. The
        // per-session wording the old log line already used was the intent all
        // along - this makes the code match it.
        // r28b: AN EXPLICIT TOGGLE RE-DECIDES THE SESSION.
        //
        // r18's latch is about INCIDENTAL pause changes - a console, a tween, a
        // menu switch - and it stays. This is the one deliberate change: the
        // user reaching into the panel and flipping force-pause. The panel is
        // drawn inside the menu whose session did the latching, so without this
        // the toggle appears to do nothing at all until you close and reopen,
        // which is exactly how it was reported from the field.
        //
        // Tear down and clear the latch; the normal decision below runs next
        // frame and picks the armed or the live-studio path on its own. No new
        // transition, just the existing one allowed to happen again.
        if (const bool fp = Settings::GetSingleton().forcePause; fp != lastForcePause_) {
            lastForcePause_ = fp;
            spdlog::info("Force-pause toggled to {} while a menu was open, re-deciding this "
                         "session instead of waiting for the next menu.", fp);
            Disarm();
            sessionDormant_     = false;
            armedLastFrame_     = false;
            loggedUnpausedOnce_ = false;
            // r28g: the live-only latch is part of what the toggle re-decides.
            // Recomputed, not cleared: flipping TO keep-unpaused mid-menu makes
            // this session live-only right now, and flipping AWAY makes it a
            // normal armable one.
            {
                const auto& s    = Settings::GetSingleton();
                sessionLiveOnly_ = s.studioInLiveMenus && !currentMenuName_.empty() &&
                                   s.IsBubbleMenu(currentMenuName_) &&
                                   s.IsSoulsLiveMenu(currentMenuName_);
            }
            return;  // next frame decides cleanly against the new pause state
        }

        // ⚠ THE GATE STANDS ABOVE THE ARM DECISION, NOT INSIDE IT. A menu the
        // owner-context gate is holding down has no studio session, so there is
        // no arm decision to make and above all no DORMANCY to latch: in a
        // Skyrim Souls load order the world is genuinely running in this state
        // (we did not take the pause), so the branch below would read unpaused,
        // latch the session dormant, and the studio could then never enter when
        // the owner finally published. Return with nothing held instead.
        if (!studioEntered_.load()) {
            return;
        }
        if (sessionDormant_) {
            // r28: a dormant session still gets the studio RIG and the COLOUR
            // FILTER if this is a Souls-live menu and the user kept them on.
            // Both are safe with the world running: the rig adds three lights
            // around the character, the filter is a screen grade. Neither hides
            // anything, which is the line this feature does not cross.
            //
            // StudioRig::Tick and SceneTint::Sync are both self-healing - they
            // apply when their setting turns on and restore when it turns off -
            // so these two calls are the whole per-frame cost and both no-op
            // when their feature is off.
            if (Settings::GetSingleton().liveStudioActive) {
                const auto& cfg = Settings::GetSingleton();
                // r28d: read back BEFORE Tick re-asserts anything, so the line
                // shows what survived the ENGINE's previous frame, not our own
                // writes. ~1 Hz; the counter is per-session state in spirit but
                // a static is fine - worst case the first line lands a beat
                // early on the next session.
                //
                // r29: GATED FOR RELEASE. This is three lines per second for as
                // long as a live menu is open, which is log spam a user would
                // report. It earned its keep - it is what proved the lights
                // were healthy - so it stays in the source behind the same
                // switch as the other probes rather than being deleted.
                if (cfg.diagnosticProbes) {
                    static std::uint32_t s_readbackCountdown = 0;
                    if (s_readbackCountdown == 0) {
                        StudioRig::LogLiveState("live-1Hz");
                        s_readbackCountdown = 120;
                    }
                    --s_readbackCountdown;
                }
                StudioRig::Tick();
                SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
            }
            // ⚠ THE LATCH IS PER STUDIO SESSION NOW, NOT PER MENU SESSION, AND
            // THE OLD COMMENT HERE SAID SOMETHING THAT IS NO LONGER TRUE. It
            // read "decided at session start; a later pause does not revive
            // it", full stop - one-way by construction, and r18's whole point.
            //
            // That claim survives verbatim for the thing it was written about:
            // a pause arriving later, from a console, a tween, an unrelated
            // pausing menu, still does NOT revive this. What has changed is
            // that "session" now means the STUDIO session. Under
            // bWaitForOwnerContext a menu can hold several of those in a row,
            // and each one gets its own decision - EnterStudioSession clears
            // this latch, which is the only thing that ever lifts it.
            //
            // Revival is therefore owner driven and edge driven, never a poll:
            // nothing here re-reads the pause and changes its mind, which is
            // exactly the shape r18 banned. The difference between a revive
            // bolted onto the latch and this is that the latch is not revived
            // at all - the session it belonged to has ended and a new one has
            // begun.
            return;
        }
        // ⚠⚠ "UNPAUSED" AND "NOT PAUSED YET" ARE THE SAME READING OF
        // GameIsPaused AND OPPOSITE CONCLUSIONS, and the decision below cannot
        // tell them apart on its own. ShadowPause POSTS its show to the UI
        // queue, so a pause taken this frame is published the next one; at a
        // menu open the take happens in the open EVENT and this is already a
        // later frame, which is why nothing ever hit it before the studio
        // session could enter mid-menu.
        //
        // Field 2026-08-13, three lines on one timestamp: "ShadowPause: shown",
        // "studio session ENTER #1", "game is UNPAUSED ... bubble dormant". The
        // latch below is one way, so the session was finished before its first
        // tick: pause released, ledger cleared, no camera, lighting only. That
        // is the whole of "we don't get camera controls when we enter Fitting
        // Room".
        //
        // Waiting is safe because it holds everything and decides nothing, and
        // it is bounded so a pause that never arrives still reaches the decision
        // it would have reached anyway. ForcePause::Holding is what separates
        // the two cases: a menu the player keeps live under Souls has no hold
        // outstanding, so it latches dormant on its first frame exactly as
        // before.
        if (StudioSessionPolicy::PauseStillLanding(paused, ForcePause::Holding(),
                                                   studioPauseSettleFrames_)) {
            if (--studioPauseSettleFrames_ == 0) {
                spdlog::warn("Bubble: a pause the studio session asked for never showed "
                             "up after {} frames, deciding the arm against a live world "
                             "instead. If this repeats, ShadowPause's message is not "
                             "reaching the queue.",
                             StudioSessionPolicy::kPauseSettleFrames);
            }
            return;  // hold everything, decide nothing, release nothing
        }
        studioPauseSettleFrames_ = 0;
        // r28g: sessionLiveOnly_ joins the condition. A lighting-only session
        // goes dormant even if frame 1 happens to READ paused - the tween
        // menu's borrowed pause rides under a fresh open often enough that
        // gating on the counter alone would build the void in a menu the user
        // explicitly keeps live, on a timing coin-flip.
        // An owner-live session (SetOwnerWorldLive) arms against an unpaused
        // world ON PURPOSE: the unpaused reading below is the wish being
        // honoured, not Skyrim Souls keeping a menu live, so it must not latch
        // the session dormant. Armed-unpaused is already a supported state -
        // "Armed sessions fall through even when `paused` is false" (RaceMenu,
        // further down) - this only lets the FIRST arm through the same door.
        // sessionLiveOnly_ still wins: a menu the player listed as live-only
        // stays lighting-only whatever an owner wishes.
        const bool ownerWorldLive = OwnerWorldLiveWanted();
        if (((!paused && !ownerWorldLive) || sessionLiveOnly_) && !armedLastFrame_) {
            // Never armed this session and the world is live (or the session
            // is live-only by decision): stay out of it for the whole session.
            if (!loggedUnpausedOnce_) {
                spdlog::info("Bubble {}: bubble dormant for this menu session (latched: "
                             "a pause arriving later will NOT arm it).",
                             sessionLiveOnly_
                                 ? "menu is LIVE-ONLY by decision (lighting-only session)"
                                 : "menu open but game is UNPAUSED (Skyrim Souls?)");
                loggedUnpausedOnce_ = true;
            }
            sessionDormant_ = true;
            // ⚠⚠ AND THE CAMERA READING IS RETIRED HERE, WHICH ONLY BECAME
            // NECESSARY WHEN THE CAPTURE MOVED TO THE MENU-OPEN EVENT. Before
            // the move a dormant session never reached the first armed tick, so
            // it captured nothing; now it captures at the open like any other
            // and then decides to sit the session out.
            //
            // ⚠ A DORMANT SESSION LEAVES THE WORLD LIVE, and that is what makes
            // holding the reading wrong rather than merely pointless. The player
            // can look around with the menu up, and the close-edge sample the
            // payment guards itself against is taken AFTER that, so live equals
            // atClose and ParkMayRestore waves it through. The park would hand
            // back a camera from before a menu the player spent moving in.
            //
            // ⚠ THE LATCH DELIBERATELY STAYS SET. The dormancy above is latched,
            // so no arm can follow this; if one somehow did, the framing another
            // mod put on this menu would still be live and the refusal alarm is
            // the answer that belongs there. This is the same shape as any other
            // teardown that keeps the latch and drops the reading.
            preMenuFreeRotValid_ = false;
            preMenuPitchValid_ = false;
            preMenuFovValid_ = false;
            // r18b: a dormant session must hold NOTHING. Field 2026-07-20: with
            // force-pause on and Souls keeping the menu live, r17's freeze was
            // still held for a session the bubble had already declined - the
            // world froze with no studio in it ("the player is completely
            // paused" with no bubble), which is worse than either outcome on its
            // own. Hand the freeze back the moment we decide to sit this one out.
            ForcePause::ReleaseAll();
            // ⚠ CLEARED, NOT DRAINED. ReleaseAll has just handed back every
            // pause we hold, this session's included, so the ledger now owes
            // nothing - and a Leave that drained it afterwards would decrement
            // the engine's counter a second time. The studio session stays
            // ENTERED through dormancy on purpose: it exists, it simply decided
            // to sit this one out, and dropping the flag here would have the
            // reconcile re-enter it next frame and loop.
            studioPauses_.Clear();
            // Clears anything the open event built before we got here (the warm
            // occluder cull); every callee no-ops when its piece was not built.
            Disarm();
            // r28 LIVE STUDIO, and it goes AFTER Disarm deliberately: Disarm
            // clears liveStudioActive and removes the rig, so setting the flag
            // first would be undone in the same breath. The next frame's
            // sessionDormant_ branch above is what actually raises the lights.
            //
            // The rig only. The void, backdrop and declutter stay out of a live
            // menu on purpose - see Settings::studioInLiveMenus - and the drives
            // are already suppressed by engineAnimates, which reads !paused.
            if (Settings::GetSingleton().studioInLiveMenus) {
                Settings::GetSingleton().liveStudioActive = true;
                // ⚠ r28c: SNAP THE TRANSITION UP, OR THE RIG EMITS NOTHING.
                //
                // StudioRig::PushConfig multiplies every light's fade by
                // Transition::Value(), and Disarm() above has just snapped it to
                // 0 (its F-12 backstop, so early disarms do not ramp). Without
                // this the lights are created, registered and correctly placed,
                // and put out 0.20 * 0.0 = zero photons.
                //
                // Field caught it exactly right: "changing their brightness
                // didn't do anything". It could not - brightness is one of the
                // factors being multiplied by zero. The log said "3 light(s) up"
                // the whole time, which is why this needed the CODE read and not
                // another round of looking at the log.
                Transition::Snap(1.0f);
                if (Settings::GetSingleton().studioRig) {
                    spdlog::info("Live studio: menu is unpaused, so the void and the drives "
                                 "stay out, but the studio rig stays on the character "
                                 "(bStudioInLiveMenus, fade snapped to 1).");
                }
            }
            return;
        }

        // Armed sessions fall through even when `paused` is false. Tick keeps
        // the scene alive and standing still; the drive is suppressed inside via
        // engineAnimates, exactly as it already is for RaceMenu.
        Tick(a_main, paused);
    }

    void Bubble::Tick(RE::Main* a_main, bool a_paused) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) {
            Disarm();
            return;
        }

        // r47: a re-open cancelled the exit machine - phase 1 is a pure
        // hold (nothing visible changed), so cancelling costs nothing.
        exitPhase_ = 0;

        const auto now = QpcNow();
        float dt = armedLastFrame_ ? QpcSeconds(lastQpc_, now) : (1.0f / 60.0f);
        lastQpc_ = now;
        dt = std::clamp(dt, 0.0f, Settings::GetSingleton().maxDeltaTime);

        // Smooth Moveset 0x804 is a permanent "combat on" ability and neither
        // ticking it nor casting its paired off-spell can retire it while the
        // actor update is paused. The body-graph hold at the animation tick
        // boundary below now owns this safely and framework-agnostically.

        if (!armedLastFrame_) {
            armedTicks_ = 0;
            telemetryCountdown_ = 0;
            // TRACKER B-7: the heading the player brought into the menu is
            // the one the whole session previews from - snapshot it here,
            // re-asserted after every graph tick below.
            armedHeading_ = player->data.angle.z;
            // B-7 v3 preview spin: park the camera where SPIM's open-time
            // setup put it and capture the root's engine-made rotation for
            // that heading - the spin composes on this immutable basis
            // every tick (never on the node's live value: whether the
            // engine re-syncs node←angle per tick or only on dirty flags,
            // an absolute basis cannot compound).
            spinTarget_ = 0.0f;
            spinYaw_ = 0.0f;
            spinDragging_ = false;
            spinStickX_ = 0.0f;
            spinBasisValid_ = false;
            inputEvidence_ = 24;  // F-14 v3: raw input events logged this arm
            // F-15 phase 2: the own-view decision + framing live in
            // ArmOwnViewIfOurs(), called after the B-4 camera refresh below
            // (and re-used by menu switches + the late-arm fallback).
            if (auto* tps = GetThirdPersonState()) {
                freeRotArm_ = tps->freeRotation.x;  // the spin park's own basis
            }
            // ⚠⚠ THE BACKSTOP, NOT THE MOMENT. The reading is taken at the
            // MENU-OPEN EVENT now, and on the shipped path it is already held by
            // the time this line runs, so the policy answers kAlreadyHeld and
            // this is a no-op. It stays because one route reaches an armed tick
            // with no open event behind it: a menu that was already on screen
            // when the plugin armed, which has no session-fresh open of its own
            // to have captured at.
            //
            // ⚠ THE OLD COMMENT HERE CLAIMED THIS WAS "the only moment the true
            // value is on the table" because it ran "before ArmOwnViewIfOurs
            // applies a framing". True of OUR framing, false of a view mod's:
            // Show Player In Inventory frames the menu from its own open sink,
            // which lands before this tick. Four repairs believed that comment.
            // The measurement that disproved it is in
            // TakePreMenuCameraCapture.
            TakePreMenuCameraCapture(currentMenuName_, "first armed tick");
            if (auto* root = player->Get3D(false)) {
                rootBaseRotate_ = root->local.rotate;
                spinBasisValid_ = true;
            }
            // r54: capture the mount's root so the spin rotates it too.
            spinHorseRoot_.reset();
            if (RE::ActorPtr mount; player->GetMount(mount) && mount) {
                auto* horseRoot = mount->Get3D(false);
                if (horseRoot) {
                    spinHorseRoot_ = RE::NiPointer<RE::NiAVObject>{ horseRoot };
                    horseBaseRotate_ = horseRoot->local.rotate;
                }
                spdlog::info("spin: mount '{}': 3D root {} (spin will {}rotate it).",
                             mount->GetName() ? mount->GetName() : "?",
                             horseRoot ? "captured" : "NULL",
                             horseRoot ? "" : "NOT ");
            }
            // Menu-drag rotation exceeds FSMP's 10 rad/s player clamp, which
            // clamps or hard-resets the sim ("momentum isn't kept"). Lift the
            // thresholds while armed; Disarm() restores the originals.
            FsmpDrive::SetRotationFreedom(true);
            if (Settings::GetSingleton().driveCbpc) {
                // CBPC keeps body physics simulating in paused menus via its
                // own RaceMenu special case; borrow it for the bubble.
                CbpcDrive::SetSimulateWhilePaused(true);
            }
            // Feet of Skyrim forces foot IK onto the (now hidden) real ground;
            // stand it down in the void / dressing room so the feet keep the
            // neutral animated pose on the stage. Restored on disarm / reset.
            // Void / dressing room hide the real ground, so foot IK must stand
            // down. Oblivion keeps the real ground visible, so foot IK stays ON
            // - gate on the void family, not >= 2.
            FootIkGate::SetSuppressed(Settings::GetSingleton().IsVoidFamily());
            // F-12 v4: if the open event cut to black, the world is already
            // culled behind it - build the studio FINISHED (T snapped to 1:
            // the r25 ramps under the reveal read as "harsh popping"), hold
            // the black a few frames so the light re-ingest and first-time
            // mesh demands land, then reveal a static, fully lit studio
            // with one clean fade. Otherwise the v1 bloom.
            {
                if (dipPhase_ == 1) {
                    dipPhase_ = 0;
                    Transition::Snap(1.0f);
                    StudioLight::Apply();
                    StudioRig::Apply();
                    Backdrop::Apply();
                    dipHoldFrames_ = 3;
                    spdlog::debug("dip: studio built at black, revealing in {} frames.",
                                  dipHoldFrames_);
                } else {
                    // Instant open: SNAP the studio + backdrop to full so the
                    // additive constellation dome shows the SAME frame as the
                    // opaque void shell. Fading in from 0 renders the shell (a
                    // solid dark sphere even at t=0) first and pops the emissive
                    // stars in a beat later, which read as an "empty void" flash
                    // on open. Close still dissolves (its edge sets the target 0).
                    Transition::Snap(1.0f);
                    StudioLight::Apply();
                    StudioRig::Apply();
                    Backdrop::Apply();
                }
            }
            // Colour filter (any view, off by default): grade the whole menu
            // scene through the imagespace base override. Sync applies it when on
            // and restores when off, so this is a guarded no-op when the filter is
            // off. Mode-independent - a view switch below leaves it untouched.
            {
                const auto& cfg = Settings::GetSingleton();
                SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
            }
            // TRACKER B-2: the walk→idle TRANSITION is event-driven (the
            // r17 float route field-failed). Moving arms now hold the caught
            // pose without manufacturing stop/start edges; this preserves the
            // live movement state and makes the exit seamless.
            airFrozenArm_ = false;
            preserveDirectionBitsArm_ = false;
            // ⚠ HOISTED ABOVE THE WEAPON PREVIEW, AND THAT IS THE FIX, NOT
            // TIDYING. This block used to sit below the arm-edge Update() call.
            // Update() can reach NormalizeToTerminal -> PumpToState ->
            // SettleLocomotion on a mid-draw arm, so with the old order the
            // first pump of the session ran against the PREVIOUS session's
            // movement decision. Reading the actor's state before any write of
            // ours is also the rule the companion latch already follows.
            movingArm_ = false;
            auto* state = player->AsActorState();
            const bool locomoting = state && (state->actorState1.walking ||
                                              state->actorState1.running ||
                                              state->actorState1.sprinting);
            unsigned directionMask = 0;
            if (state) {
                directionMask |= state->actorState1.movingForward ? 1u : 0u;
                directionMask |= state->actorState1.movingBack ? 2u : 0u;
                directionMask |= state->actorState1.movingLeft ? 4u : 0u;
                directionMask |= state->actorState1.movingRight ? 8u : 0u;
            }
            const auto movementPlan = MovementArmPolicy::Choose({
                .locomoting = locomoting,
                .grounded = !player->IsInMidair(),
                .swimming = state && state->actorState1.swimming,
                .directionMask = directionMask,
            });
            preserveDirectionBitsArm_ = movementPlan.preserveDirectionBits;
            // Hand the decision to the pumps. They run from a vfunc hook and
            // from the per-tick swap path, so this is the only way they can know
            // the arm promised to leave the movement graph alone.
            WeaponPreview::SetMovingArm(movementPlan.freezeCaughtPose);
            spdlog::debug("idle-in-menus: arm handed WeaponPreview movingArm={}.",
                          movementPlan.freezeCaughtPose);
            // F-26: evaluate the weapon preview at the arm edge. Cheap - it is
            // a no-op unless the decision changes. MUST run before the freeze
            // block below: it issues the draw, so that block would otherwise
            // see our own kDrawing and freeze the arm we just started. This
            // call runs ONCE per arm; the per-tick call further down is what
            // catches a weapon SWAP made inside the menu.
            //
            // 0.7.1: release the normalize latch FIRST. The normalize runs once
            // per arm now instead of once per tick, so this is what makes the
            // next session able to run it at all.
            WeaponPreview::ArmEdgeReset();
            WeaponPreview::Update(player, true, raceMenuOpen_.load());
            if (Settings::GetSingleton().idleInMenus && !raceMenuOpen_.load() &&
                !CharacterFrozen()) {
                // ⚠ FIELD 2026-07-21 17:20. This latch is what puts a WALK
                // ANIMATION in a paused menu. Repro: be unsheathing while
                // moving, open the inventory, switch weapon.
                //
                //   idle-in-menus: armed MID-DRAW WHILE MOVING - the draw/sheathe
                //                  hold stands aside, the locomotion settle owns this arm.
                //
                // It then does not settle. The hold stands aside, the graph
                // keeps ticking, and the walk clip that was already running just
                // plays on for the whole menu. The 0.7.1 handoff already had it
                // as SUSPECT, NOT ENDORSED - it fired once in 19 field arms and
                // works by letting a mid-transition graph tick, which is the
                // exact state the hold exists to prevent.
                //
                // Behind a key rather than deleted outright, default OFF, so the
                // moving arm falls back to the ordinary draw/sheathe hold. If
                // the field confirms the walking is gone, delete the latch and
                // this key together rather than leaving them as decoration.
                movingArm_ = Settings::GetSingleton().movingArmStandsAside &&
                             locomoting && state && !state->actorState1.swimming &&
                             !player->IsInMidair();
                // r36 (user design): the graph ticks ONLY for standing
                // idles and the locomotion-to-still settle below -
                // EVERYTHING else holds its caught frame (mid-air r18,
                // attacks r34, and now draw/sheathe transitions, furniture
                // enter/exit, swimming, and scripted idles - interactions,
                // crafting - detected via bAnimationDriven at the arm edge,
                // the same flag non-idle clips raise, B-7). Face, hair and
                // body physics keep living either way.
                const auto weaponState = state->actorState2.weaponState;
                // F-26: a draw/sheathe THIS feature asked for is allowed to
                // animate - it is the one the user requested by opening the
                // menu, not an animation leaking in from gameplay. Every other
                // transition still holds its caught frame, which is the whole
                // point of r36 ("genuinely only the standing still idles should
                // be playing"). Without this exception the preview deadlocks:
                // we ask for a draw, the freeze latches, and the state machine
                // never leaves kDrawing.
                //
                // 0.7.1: a clip the normalize could NOT finish is treated the
                // same way as one of our own. Freezing it is what produced the
                // field report - a replaced draw animation runs past the pump
                // budget, so the arm froze mid-clip for the whole menu and the
                // half-played transition leaked into live gameplay on close.
                // Letting it animate costs a beat of movement in a paused menu
                // and leaves the actor in a state the engine can reconcile.
                //
                // ⚠ 0.7.1 - AND THE CLIP, NOT ONLY THE STATE. Every state below
                // reads TERMINAL for the whole back half of a draw, because
                // kDrawn is set by the weaponDraw annotation about halfway
                // through the clip (r11). 21 field arm edges all reported a
                // terminal state while the user was deliberately opening the
                // menu mid-unsheathe, so this test alone could never catch the
                // case it exists for. ClipProbe answers the question the state
                // cannot: is the animation still running? See ClipProbe.h for
                // why letting that clip finish inside a paused menu is what
                // loops a stance mod's idle.
                const bool weaponTransition =
                    !WeaponPreview::TransitionInFlight() &&
                    !WeaponPreview::NormalizeCapped() &&
                    (ClipProbe::EquipClipInFlight() ||
                     weaponState == RE::WEAPON_STATE::kWantToDraw ||
                     weaponState == RE::WEAPON_STATE::kDrawing ||
                     weaponState == RE::WEAPON_STATE::kWantToSheathe ||
                     weaponState == RE::WEAPON_STATE::kSheathing);
                const auto sitSleep = state->GetSitSleepState();
                const bool furnitureTransition =
                    sitSleep != RE::SIT_SLEEP_STATE::kNormal &&
                    sitSleep != RE::SIT_SLEEP_STATE::kIsSitting;
                bool scriptedIdle = false;
                player->GetGraphVariableBool("bAnimationDriven", scriptedIdle);
                // r39 (field: "TK dodge and dodge mod animations aren't
                // paused"): a dodge reads as plain grounded locomotion -
                // none of the states below catch it, the graph keeps
                // ticking, and the dodge clip plays out in the menu. Dodge
                // mods PUBLISH their state as a graph bool though
                // (bIsDodging = the TK Dodge RE / TUDM convention; DMCO
                // spellings carried too - unknown names read false, free).
                // Any true flag freezes the arm exactly like an attack;
                // sFreezeGraphBools extends the list without a rebuild.
                std::string dodgeFlag;
                for (const auto& var : Settings::GetSingleton().freezeGraphBools) {
                    bool set = false;
                    if (player->GetGraphVariableBool(var.c_str(), set) && set) {
                        dodgeFlag = var;
                        break;
                    }
                }
                // ⚠ A MOVING ARM ALREADY HAS AN OWNER, AND IT IS NOT THIS.
                //
                // Field (user): "if we unsheath and are moving the period where
                // we freeze the character applies, can we make it so when we
                // start moving this pause period is negated". Drawing while
                // running set weaponTransition, which wins this chain outright,
                // so the locomotion settle below was never reached and the
                // character held a half-drawn running frame for the length of
                // the clip plus a second.
                //
                // Standing still is where the lunge loop lives: the graph is
                // between idles, the stance framework's conditions are stale,
                // and a latched selector loops the step-into-stance clip. A
                // MOVING graph is not in that state - it has a locomotion pose
                // to leave and an explicit moveStop to leave it by - so the
                // settle is both the better answer and the one already written.
                // Only the weapon reason stands aside; mid-air, mid-attack,
                // furniture, dodges and scripted idles still freeze while
                // moving, because none of those has a settle to fall back on.
                const bool movingDraw = weaponTransition && movingArm_;
                const char* freezeReason =
                    player->IsInMidair()                                        ? "mid-air"
                    : state->actorState1.meleeAttackState !=
                          RE::ATTACK_STATE_ENUM::kNone                          ? "mid-attack"
                    : (weaponTransition && !movingDraw)
                                       ? (ClipProbe::EquipClipInFlight()
                                              ? "draw/sheathe CLIP still playing"
                                              : "draw/sheathe")
                    // ⚠ REVERTED - `ClipProbe::GraphSettling()` WAS HERE AND IT
                    // WAS TOO BROAD. It froze on ANY recent clip activity, and
                    // walking activates clips continuously, so opening a menu
                    // while moving froze the whole character - and a window
                    // after stopping did too. Field: "it's a nuclear fix ...
                    // even if i walked and stopped, there's a window where we
                    // are frozen."
                    //
                    // It was aimed at a SECOND-HAND report (Movement Behavior
                    // Overhaul: stop, open inventory, stop-anim loops) that
                    // could not be reproduced here, and it broke behaviour that
                    // demonstrably worked. Do not reinstate it in this form.
                    // The locomotion case already has an owner: the `locomoting`
                    // branch below sends moveStop and lets the graph settle,
                    // which is the design and which this pre-empted.
                    //
                    // If MBO is chased again, it needs a signal specific to a
                    // stop TRANSITION - not "the graph did something recently".
                    // GraphSettling() is left in ClipProbe unused for that work.
                    : furnitureTransition                                       ? "furniture/sleep transition"
                    : state->actorState1.swimming                               ? "swimming"
                    : scriptedIdle                                              ? "scripted idle (animation-driven)"
                                                                                : nullptr;
                // ⚠ 0.7.1 - THE ONE WAY THE CAPPED-CLIP FIX GETS DEFEATED.
                //
                // Letting an un-normalizable clip animate only works if nothing
                // ELSE in the chain below freezes the arm. A replaced draw with
                // root motion - which is exactly what "lunging forward"
                // describes - can raise bAnimationDriven, and that freezes for
                // "scripted idle" instead: same held clip, different reason,
                // and indistinguishable in the field from the fix simply not
                // working. Report it instead of leaving the next round to
                // guess. Both outcomes testify, because a silent decline is
                // indistinguishable from a dead feature (r28h).
                //
                // Evaluated BEFORE the chain, deliberately: every input it
                // needs is already known here, so it cannot disturb the
                // if/else-if structure - restructuring that chain silently
                // moved the locomotion settle under a frozen arm once already.
                if (WeaponPreview::NormalizeCapped()) {
                    const char* const wouldFreeze =
                        freezeReason ? freezeReason
                                     : (!dodgeFlag.empty() ? "mid-dodge" : nullptr);
                    if (wouldFreeze) {
                        spdlog::warn("idle-in-menus: the arm is freezing for '{}' even though "
                                     "the draw/sheathe clip could not be normalized. The clip "
                                     "is STILL held half-played. The capped-clip exception did "
                                     "not take; that reason needs exempting too.",
                                     wouldFreeze);
                    } else {
                        spdlog::info("idle-in-menus: NOT freezing: the draw/sheathe clip could "
                                     "not be normalized, so it is allowed to finish on real "
                                     "frames instead of being held half-played.");
                    }
                }
                if (movingDraw) {
                    // Name the branch. A silent decline is indistinguishable
                    // from a dead feature (r28h), and this one can only be
                    // told apart from the old behaviour by which log line ran.
                    spdlog::info("idle-in-menus: armed MID-DRAW WHILE MOVING: the "
                                 "draw/sheathe hold stands aside, the locomotion "
                                 "settle owns this arm.");
                }
                if (freezeReason) {
                    airFrozenArm_ = true;
                    spdlog::debug("idle-in-menus: armed {}, anim tick frozen this arm.",
                                  freezeReason);
                } else if (!dodgeFlag.empty()) {
                    airFrozenArm_ = true;
                    spdlog::debug("idle-in-menus: armed mid-dodge ('{}' set), "
                                  "anim tick frozen this arm.", dodgeFlag);
                } else if (movementPlan.freezeCaughtPose) {
                    // Field 2026-07-23: the old stop/settle transaction was
                    // direction-sensitive when movement keys were mashed at
                    // the menu edge, and its deferred moveStart caused the
                    // visible exit slide. "Always freeze character" removed
                    // both because it never touched the movement graph.
                    //
                    // Apply that exact behavior only to moving arms: hold the
                    // caught pose, preserve every live movement bit and graph
                    // variable, and let the unpaused world resume naturally.
                    // Standing arms still tick their breathing/idle animation.
                    airFrozenArm_ = true;
                    spdlog::info(
                        "idle-in-menus: armed in grounded locomotion "
                        "(direction mask 0x{:X}): caught pose held; movement graph "
                        "left untouched for a seamless exit.",
                        directionMask);
                }
            }
            // F-13: papyrus expression mods are frozen by the pause (B-6) -
            // an arm can catch the face MID-SEQUENCE (half a blink) and
            // hold it all menu. Mode 2 (neutral): save the expression, ramp
            // to neutral via the engine's own facegen reset (our face tick
            // animates it), restore at disarm. Mode 1 (live): handled per
            // tick in the face block below (composed-buffer settle + blink
            // machine care); here only the arm-time CAUGHT snapshot is
            // logged - one line names the stuck channel when the field
            // reports a frozen face. RaceMenu owns its face; both modes
            // need the tick.
            if (Settings::GetSingleton().tickFace && !raceMenuOpen_.load()) {
                const int faceMode = Settings::GetSingleton().faceInMenus;
                if (faceMode == 2) {
                    FaceNeutral::Apply();
                } else if (faceMode == 1) {
                    if (auto* face = player->GetFaceGenAnimationData()) {
                        const auto val = [](const RE::BSFaceGenKeyframeMultiple& a_kf,
                                            std::uint32_t a_i) {
                            return (a_kf.values && a_kf.count > a_i) ? a_kf.values[a_i]
                                                                     : 0.0f;
                        };
                        float phonMax = 0.0f;
                        for (std::uint32_t i = 0;
                             face->unk140.values && i < face->unk140.count; ++i) {
                            phonMax = (std::max)(phonMax,
                                                 std::fabs(face->unk140.values[i]));
                        }
                        spdlog::info(
                            "face live: caught machine={} delay={:.2f} | composed "
                            "lids L/R={:.2f}/{:.2f} gaze8={:.2f} phonMax={:.2f} | "
                            "exprOverride={} eyesClosed21A={}",
                            face->unk200, face->blinkDelay, val(face->unk100, 0),
                            val(face->unk100, 1), val(face->unk100, 8), phonMax,
                            face->exprOverride, face->unk21A);

                        // F-16 r3 (user: "when we open the menu when they mid
                        // blink, we can see their eyes closed in the menu").
                        // ~9% of opens land inside a blink (field log
                        // 2026-07-18: 2 of 23, and the first telemetry sample
                        // is 0.35s AFTER the arm, so the real share is higher).
                        // The machine does finish the blink - but it finishes
                        // over the following ~0.2s WITH THE MENU ALREADY UP,
                        // and this is a posing/screenshot mod: a caught blink
                        // is exactly the frame you did not want.
                        //
                        // Release it AT THE ARM EDGE instead of waiting:
                        //   - park the state machine at 0. Per the decompile
                        //     (mtb_blinkgen.c 0x1403c2930) state 0 counts the
                        //     timer down and writes NOTHING, while states 1/2
                        //     rewrite the composed lids every Update - so
                        //     clearing lids without parking would just be
                        //     overwritten on the next tick.
                        //   - then clear the composed lid pair (the buffer the
                        //     bake reads) the way the machine writes it
                        //     (values + isUpdated=false), plus the input
                        //     keyframes so a later recompose cannot resurrect
                        //     the half-blink.
                        //   - hold the next blink off briefly so the menu does
                        //     not open straight into another close.
                        // ONE-SHOT, arm edge only: the per-tick path is left
                        // alone, because a per-tick pin is exactly what made
                        // r32-r34 "stop blinking" (r34.5 - our own pin held
                        // the machine mid-blink forever).
                        //
                        // unk21A gates it: state 4 drives the lids CLOSED while
                        // that flag is up (the engine's eyes-shut hold, e.g. a
                        // sleeping actor), so forcing those eyes open would
                        // fight a deliberate engine state.
                        const bool lidsShut = val(face->unk100, 0) > 0.05f ||
                                              val(face->unk100, 1) > 0.05f;
                        if (!face->unk21A && (lidsShut || face->unk200 != 0)) {
                            const int  caughtState = face->unk200;
                            face->unk200 = 0;  // waiting: the machine stops writing lids
                            if (auto& comp = face->unk100;
                                comp.values && comp.count > 1) {
                                comp.values[0] = 0.0f;
                                comp.values[1] = 0.0f;
                                comp.isUpdated = false;
                            }
                            if (auto& mod = face->modifierKeyFrame;
                                mod.values && mod.count > 1) {
                                mod.SetValue(0, 0.0f);
                                mod.SetValue(1, 0.0f);
                            }
                            if (face->blinkDelay < 0.6f) {
                                face->blinkDelay = 0.6f;
                            }
                            spdlog::info("face live: menu opened mid-blink "
                                         "(machine state {}): eyes released open; "
                                         "natural blinking resumes in {:.2f}s.",
                                         caughtState, face->blinkDelay);
                        }
                    }
                }
            }
            // TRACKER B-4: SPIM pushes exactly ONE camera update at menu
            // open and then only moves the camera on input (spim_camera.c:
            // RotateCamera tail vfunc call). If that update beat our
            // open-event bookkeeping, the collision smoother was still
            // ungated and clamped the camera onto the player at close
            // walls - where it then FROZE for the whole menu (r15 log:
            // one arm at horizontal 69 vs the healthy arms' constant 126).
            // Re-run the update now that the gate is provably live: the
            // builder recomputes the raw orbit position through the
            // (culled) wall.
            //
            // ⚠⚠ AND THE GATE IS HELD OPEN ACROSS IT, SINCE 2026-08-16. "Now
            // that the gate is provably live" was the one thing this comment
            // had wrong: IsBubbleActive also asks whether the world is frozen,
            // and the freeze is a UI queue post the engine publishes a frame
            // LATER than the take a few lines above. So the re-extend ran with
            // the gate reading false, the smoother clamped the rebuild, and a
            // third-person camera is not recomputed again while the world is
            // stopped - the player looks at that one clamped frame for the
            // whole menu. Reported as the camera being at your face after
            // opening against furniture or with your back to a wall, in scene
            // view and with declutter off alike, and going right the moment you
            // drag, which is StudioCamera taking the shot over.
            if (auto* camera = RE::PlayerCamera::GetSingleton();
                camera && camera->currentState &&
                camera->currentState->id == RE::CameraState::kThirdPerson) {
                const CameraGate::ForcedBypass hold;
                camera->Update();
                spdlog::debug("camera refreshed at arm (B-4 re-extend, "
                              "collision gate forced open).");
            }
            // F-15 phase 2: force third + apply the FULL SPIM framing when
            // the bubble owns this menu's view (originals saved inside; the
            // close event restores).
            ArmOwnViewIfOurs();
            // Clears state and writes nothing. The opening framing is read back
            // off the camera at the first real input instead of here, so a view
            // mod pushing its own update after this point cannot leave us
            // holding an original that was never on screen.
            StudioCamera::Arm();
            appliedRevision_ = Settings::GetSingleton().revision;
            appliedMode_ = Settings::GetSingleton().declutterMode;
            // Face counters are PER SESSION - the disarm edge reports them.
            faceMeshApplies_ = 0;
            blinkStarts_ = 0;
            blinkCompletes_ = 0;
            blinkCaps_ = 0;
            blinkLidPeak_ = 0.0f;
            blinkStatePrev_ = -1;
            faceNodeMissingLogged_ = false;
            faceHeldLogged_ = false;
            freezeDeclineLogged_ = false;
            ClipProbe::ArmedSessionBegin();  // clip counters are per session too
            CompanionLunge::ArmedSessionBegin();
            spdlog::info("Bubble ARMED (paused menu, player 3D loaded).");
        }
        AnimEventProbe::SetArmed(true);  // diagnostic tag: events below are the MENU case
        ActorTickProbe::MarkArmed(true);
        EquipNotifyGate::SetArmed(true);
        armedLastFrame_ = true;
        ++armedTicks_;

        // F-12 v4: the post-build black hold - reveal once it elapses. Dormant
        // now (nothing sets dipPhase_ since the fader open-cover was reverted);
        // the dip machinery is left intact for the definitive fix to reuse if it
        // needs a NON-fader build-then-show.
        if (dipHoldFrames_ > 0 && --dipHoldFrames_ == 0) {
            g_fadeOutGame(false, true, Settings::GetSingleton().dipInSeconds, false, 0.0f);
            spdlog::debug("dip: revealing ({:.2f}s).", Settings::GetSingleton().dipInSeconds);
        }

        // F-15 r33 fallback: rare field case - a few ticks in, nobody framed
        // the view (a covered view mod skipped its own conditions for this
        // menu), and the camera is still somewhere the framing cannot be seen
        // from. Late-arm our view.
        //
        // ⚠ THIS WAS kFirstPerson-GATED AND SO COULD NOT RESCUE THE 2026-08-11
        // REPORT, which is the failure it exists for: an animation camera at
        // the barter menu is precisely "the covering view mod skipped this
        // one", and the backstop declined because the stuck state was not
        // first person. It asks the same question the arm does now.
        //
        // The two settings keep their own contracts: ownViewFirstPerson is
        // "you always see your character" for a first-person arm, and every
        // other stuck state is an unmanaged one, which is ownViewUnmanaged's.
        if (armedTicks_ == 3 && !OwnView::Active()) {
            if (auto* camera = RE::PlayerCamera::GetSingleton();
                camera && camera->currentState) {
                const auto stateId = camera->currentState->id;
                const auto plan =
                    CameraArmStatePolicy::ChooseArm(static_cast<int>(stateId));
                const auto& s = Settings::GetSingleton();
                const bool allowed = stateId == RE::CameraState::kFirstPerson
                                         ? s.ownViewFirstPerson
                                         : s.ownViewUnmanaged;
                if (plan.forceThird && allowed) {
                    spdlog::info("own view: still {} at tick 3: the covering view "
                                 "mod skipped this menu; late-arming our view.",
                                 CameraStateName(stateId));
                    ArmOwnViewIfOurs(true);  // r45: bypass coverage - nobody framed
                }
            }
        }

        // SPIM's drag handler refuses ALL rotation while the actor-state
        // direction bits are set (spim_input.c: movingBack/Forward/Right/
        // Left gates every rotation path) - a menu opened mid-walk freezes
        // them ON for the whole pause (the release edge never reaches the
        // paused movement controller), which is why the preview wouldn't
        // rotate after walking in (field r25). They are per-frame INPUT state,
        // but clearing them is only safe when no live movement transaction
        // entered the menu. A key released during the pause emits no normal
        // input-release edge; direct clearing could therefore strand the graph
        // in locomotion after close, and the next weapon draw selected the
        // stance framework's looping start idle. Preserve them for moving arms.
        // Our own preview-spin sink does not need these bits cleared.
        if (Settings::GetSingleton().previewSpin && !preserveDirectionBitsArm_) {
            if (auto* state = player->AsActorState()) {
                state->actorState1.movingBack = 0;
                state->actorState1.movingForward = 0;
                state->actorState1.movingRight = 0;
                state->actorState1.movingLeft = 0;
            }
        }

        // r46: hold the exterior sun down for as long as the void is up. This
        // is the light-side twin of ReassertWorldFeederCulls below it and it
        // exists for the same reason: Sky::Update runs in the unpaused switch
        // and exit windows and repaints the sun and the directional ambient
        // from the weather, so a park that only fired on the arm edge would
        // lose them to a menu switch. State-based and idempotent, so this also
        // covers the mid-menu view-mode change, where the two StudioLight calls
        // sit behind CellLightAllowed() and a player with bStandardizeLighting
        // off would otherwise reach neither.
        StudioLight::SyncExteriorSun();

        // Declutter: the INITIAL cull for a fresh VOID open is deferred to HERE
        // (the open handler skips it when the occluder covers the swap), gated to
        // armedTicks_ 2 so the shell (attached by the arm block on tick 1) is up
        // before the sky is dropped. Then re-cull ~4x/s (the paused SPIM camera
        // can move/zoom, shifting the occluder corridor). Switches / non-void
        // opens already culled in the open event; culling again here is a harmless
        // idempotent top-up. (armedTicks_ is unsigned - guard the subtraction.)
        if (armedTicks_ >= 2 && (armedTicks_ - 2) % 15 == 0) {
            Declutter::Refresh();
        }

        // Live settings: the panel bumps the revision on every save (it can
        // be open on top of a bubble menu). Rig values flow per-tick inside
        // StudioRig::Tick; the cell look re-applies + re-ingests once per
        // change here.
        if (const auto rev = Settings::GetSingleton().revision; rev != appliedRevision_) {
            appliedRevision_ = rev;
            const auto& cfg = Settings::GetSingleton();
            if (const int mode = cfg.declutterMode; mode != appliedMode_) {
                appliedMode_ = mode;
                // B-5: a view-mode switch must UNDO the old mode's culls
                // right away - the periodic sweep only ever ADDS hides, so
                // without this the change waits for the next menu open.
                Declutter::RestoreAll();
                if (mode != 0) {
                    Declutter::Refresh();
                }
                // StudioLight owns the void's cell lighting: restore it when
                // leaving the void family, apply it when entering (self-gated to
                // 2/3). The colour filter is view-INDEPENDENT, so a mode switch
                // leaves SceneTint exactly as the filter toggle set it.
                if (!cfg.CellLightAllowed()) {
                    StudioLight::Restore();
                }
                if (cfg.CellLightAllowed()) {
                    StudioLight::Apply();
                }
                spdlog::info("view mode switched to {} mid-menu (live).", mode);
            }
            StudioLight::LiveRefresh();
            // Colour filter: apply / refresh / restore per the toggle, so turning
            // it on or off mid-menu (and slider drags) all land live.
            SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
        }
        // F-12: keep the ramp aimed up while armed (a menu switch flips it
        // down at the close edge; landing back here reverses it mid-flight)
        // and step it BEFORE the consumers read Value() this frame.
        Transition::SetTarget(1.0f);
        Transition::Tick(dt);
        StudioRig::Tick();
        Backdrop::Tick();

        // ⚠ THE EDITOR'S HEAD-AIM IS RACEMENU'S FEATURE, NOT A FIGHT
        // (2026-08-06). Two eras of an "editor look" hold lived here,
        // keeping the player's bHeadTracking off in that menu - first
        // because the face kept staring at an editor camera we had stood
        // down, then re-asserted per tick because the editor writes the
        // variable itself while aiming ("head jitters when we hold left
        // click drag"). Both eras belonged to the camera war. The studio
        // camera stands down in the editor now, so that menu's camera and
        // its head-aim are one native pair again, and a hold here would
        // only kill the feature the player asked to have back.
        //
        // The B-3 pin below is gated out of the editor for the same
        // reason: it exists for menus where nothing LEGITIMATE aims the
        // head (TDM re-aiming at a stale world direction), and in the
        // editor something legitimate does.
        // ⚠ AND THE HOLD IS RELEASED THE MOMENT IT STOPS APPLYING, not merely
        // left to lapse - switching INTO the editor is a live transition, and
        // a head still held there would kill the aim the editor legitimately
        // drives. Disarm covers every other exit.
        if (!(Settings::GetSingleton().freezeHeadTracking && !raceMenuOpen_.load())) {
            ReleasePlayerHeadtracking();
        } else {
            HoldPlayerHeadtracking(player);
        }

        // F-26: re-evaluate every tick, not just at the arm edge. This is what
        // catches a weapon SWAP made inside the menu - the engine attaches the
        // newly equipped weapon to its SHEATH node, and only a later Update()
        // sees the FormID change and re-draws it into the hand. It is also what
        // clears inFlight once our own transition settles. The arm-edge call
        // above cannot do either job: it runs once, before the draw it issues
        // has even started.
        WeaponPreview::Update(player, true, raceMenuOpen_.load());

        // RaceSexMenu (F-9, experimental): the engine's own racemenu mode
        // already animates the player (the same special path CBPC keys
        // off) - our graph/face/caster ticks would double-step it. The
        // space/lighting/rig/stage and the FSMP drive still run.
        //
        // r18 folds the unpaused case into the SAME seam. When the world is
        // live the engine is ticking the player itself, which is the identical
        // situation - so suppress the drive and keep the scene, instead of
        // tearing the scene down (what the old OnFrame Disarm did). This is the
        // half of the r18 decoupling that makes an armed session survive losing
        // the pause: the void belongs to the menu, the ticking belongs to the
        // pause, and they are now independent.
        const bool engineAnimates = raceMenuOpen_.load() || !a_paused;

        // THE RE-SETTLE WINDOW. A 3D swap between two armed frames is an apply
        // (or a race switch) rebuilding the player under an open menu, and the
        // fresh skeleton starts in its bind pose. Field r27: the looks apply
        // closes the editor as it begins, so the wish is already gone by the
        // time the rebuild lands and the freeze held the A-pose until the user
        // reopened the editor. The window steps the graph long enough for the
        // idle to take, then the freeze resumes on its own.
        if (const void* const rootNow = player->Get3D(false)) {
            if (StudioSessionPolicy::RootWasSwapped(lastArmedPlayerRoot_, rootNow)) {
                reposeFrames_ = StudioSessionPolicy::kReposeFrames;
                spdlog::info("re-settle: the player's 3D was REPLACED under this menu "
                             "(an apply or a race switch), stepping the graph for {} "
                             "frames so the new skeleton leaves its bind pose, then the "
                             "freeze resumes.",
                             reposeFrames_);
            }
            lastArmedPlayerRoot_ = rootNow;
        }
        if (reposeFrames_ > 0) {
            --reposeFrames_;
        }

        // ⚠ NEVER DECLINE SILENTLY (r28h). Freeze holds the pose by not ticking
        // the graph - which only means anything while the world is paused and
        // WE are the only thing moving the player. With Skyrim Souls keeping a
        // menu live the engine animates the player itself, so there is nothing
        // here to withhold and the checkbox does exactly nothing. That is not a
        // conflict to fix, it is a case to name: a Souls user who ticks
        // "Freeze the character" and sees their character keep moving needs the
        // log to tell them which switch actually governs it.
        if (Settings::GetSingleton().freezeCharacter && engineAnimates &&
            !freezeDeclineLogged_) {
            freezeDeclineLogged_ = true;
            spdlog::info("freeze character: NOT APPLIED this session: {}. The engine is "
                         "animating the player itself, so holding our own tick withholds "
                         "nothing. Untick \"Keep these menus unpaused\" to pose in a "
                         "frozen scene.",
                         raceMenuOpen_.load()
                             ? "RaceSexMenu drives the player on its own path"
                             : "this menu is not paused (Skyrim Souls keeps it live)");
        }

        // ⚠ EquipClipInFlight() is checked LIVE here, not only at the arm edge.
        //
        // airFrozenArm_ is an arm-edge LATCH, so it can only ever describe the
        // state the menu OPENED in. The field showed the hole: equipping a
        // weapon INSIDE an already-open menu starts a fresh draw/sheathe that
        // the latch knows nothing about, and ticking ran it - plus the idle
        // pick after it - under the frozen VM. 98 of 110 footstep events in the
        // field log came from menus where an equip started after the arm.
        //
        // Same rule as the arm edge, evaluated continuously: while a
        // draw/sheathe clip or its idle settle is in flight, do not step the
        // graph. It resumes by itself the moment the hold expires.
        //
        // movingArm_ exempts the LIVE half of the hold for the same reason the
        // arm-edge half is exempted above: this menu opened on a moving graph,
        // the settle owns it, and holding it half-drawn mid-stride is the thing
        // the user reported. A menu that opened standing still is untouched, so
        // the lunge-loop fix keeps the case it was built for.
        const bool equipClipInFlight = ClipProbe::EquipClipInFlight();
        const bool equipOccurredThisSession = ClipProbe::EquipOccurredThisSession();
        // ⚠ THE RE-SETTLE DROPS THE PERMANENT HALVES OF THE HOLD, AND ONLY IT
        // DOES. An apply EQUIPS, so equipOccurredThisSession is true from the
        // rebuild onward and the latch would pin the very bind pose the window
        // exists to leave. The LIVE halves stay even here: a draw/sheathe clip
        // actually in flight still settles first, and a mount is still a graph
        // nobody in this function steps. Outside the window an owner-live
        // session holds exactly as any other menu does - r27 briefly relaxed
        // this for the wish as well, which was behaviour nobody asked for.
        const bool reposing = ReposeActive();
        const bool animHeld = MenuAnimationHoldPolicy::ShouldHold({
            .armEdgeHeld = airFrozenArm_ && !reposing,
            .freezeDrawSheathe =
                Settings::GetSingleton().freezeDrawSheathe && !movingArm_,
            .equipClipInFlight = equipClipInFlight,
            .equipOccurredThisSession = equipOccurredThisSession && !reposing,
            // ⚠ ACTOR STATE, matching the spin gate and the weapon gate rather
            // than the camera's kMount. The question here is whether there is an
            // animal under her whose graph nobody is stepping, which is a fact
            // about the world and not about which camera is up.
            .mounted = player->IsOnMount(),
        });
        if (Settings::GetSingleton().tickAnimation && !engineAnimates && !animHeld &&
            !CharacterFrozen()) {
            // TESObjectREFR vfunc 0x7D - steps the behavior graph with our dt;
            // the PlayerCharacter override also refreshes 1st+3rd person
            // graphs. No pause gate inside (docs/SPIKE-A-RE.md).
            player->UpdateAnimation(dt);
            ClipProbe::AdvanceEquipTransitionGraphSeconds(dt);
        }

        // The second subject, if another mod put one in the shot. Same tick and
        // same dt, but NOT the same hold: animHeld latches on the player's first
        // equip of the session, and in an outfit editor that is immediate and
        // permanent. See TickFramedCompanion.
        // ⚠ OS-103: THE PER-TICK PLAYER RE-ASSERT USED TO BE HERE AND IS
        // DELIBERATELY GONE. It worked exactly as designed and still lost:
        // measured 15974 re-holds in a single arm, ~160/sec, with the root
        // pointer IDENTICAL every time (0x1fb9c150400 throughout). Same live
        // node, flag cleared every frame by another writer. Re-adding it buys
        // nothing and costs a scenegraph write per frame for a race we do not
        // win - see the OS-103 tracker row before trying again.
        //
        // OS-103(b): the READ-ONLY phase probe does sit here, and it is not that
        // re-assert. It samples where in the frame the flag dies instead of
        // fighting for it. Declutter::ProbePlayerCull carries the distinction.
        Declutter::ProbePlayerCull(Declutter::CullPhase::kPostRefresh);
        // ⚠⚠ THE LOOK COMES BEFORE THE MOTION, AND IT IS NOT GATED THE SAME WAY.
        // A framed companion must stop staring at the player whatever this menu
        // is doing with her animation: bTickCompanion off, bFreezeCharacter on,
        // or the engine animating a Skyrim Souls menu itself all skip the tick
        // below, and until 2026-08-16 they skipped the headtrack hold with it.
        // "No matter what the view is" is the user's wording and this line is
        // it: the only condition left is that somebody is framed.
        HoldFramedCompanionHeadtracking();
        TickFramedCompanion(dt, engineAnimates);

        // A HELD POSE HOLDS ITS FACE TOO. Field (user): "when the character is
        // on hold we can see them blinking still, it's slightly jarring" - a
        // statue that blinks reads as a bug, because the one thing still moving
        // draws the eye to everything that is not.
        //
        // But SETTLE BEFORE HOLDING. Stopping the face the instant the pose
        // freezes would bake whatever the pause caught - and a blink caught
        // halfway is exactly the shut-eyes defect this stint spent its whole
        // budget on. FaceAtRest gates the hold on the machine being parked with
        // the lids open, so an in-flight blink always finishes first and the
        // last frame baked is an open-eyed one.
        const bool faceHeld =
            animHeld && FaceAtRest(player->GetFaceGenAnimationData());
        if (faceHeld != faceHeldLogged_) {
            faceHeldLogged_ = faceHeld;
            spdlog::debug("face live: {} (the pose is {}).",
                          faceHeld ? "HELD: blinking stopped, eyes open"
                                   : "live again",
                          animHeld ? "held" : "animating");
        }
        if (Settings::GetSingleton().tickFace && !engineAnimates && !faceHeld &&
            !CharacterFrozen()) {
            // Facegen is a separate system from the behavior graph: blink
            // timers and MFG expression/phoneme/modifier ramps only advance
            // in BSFaceGenAnimationData::Update, whose engine caller is
            // render-model-side and pause-gated.
            if (auto* face = player->GetFaceGenAnimationData()) {
                // r33's post-update blink zero STILL blinked in the field -
                // order of operations: Update() BAKES the morph geometry
                // from the keyframe values as they stand, so a value some
                // mod wrote since our last tick was already baked before
                // the post-zero cleaned the keyframe. Pin BEFORE the bake
                // (and keep the post-zero so samplers between ticks read
                // clean). If eyes STILL blink with both pins, the writer
                // runs INSIDE Update (MFG Fix re-drive) - the telemetry
                // blink column is the discriminator.
                const int faceMode = Settings::GetSingleton().faceInMenus;
                const bool pinBlinks = faceMode == 2 && !engineAnimates;
                const auto zeroBlinks = [&face] {
                    if (auto& mod = face->modifierKeyFrame;
                        mod.values && mod.count > 1 &&
                        (mod.values[0] != 0.0f || mod.values[1] != 0.0f)) {
                        mod.SetValue(0, 0.0f);
                        mod.SetValue(1, 0.0f);
                    }
                };
                // F-16 mode 1 - LIVE face (the "Conditional Expressions
                // mid-blink" fix). The caught MOOD expression is kept:
                // exprOverride stays as the mod left it - when raised (CE
                // does), the engine's mood re-assert (the only thing +0x21E
                // gates, mtb_facegen.c:46/118) never wipes the authored
                // face. Everything below writes the COMPOSED buffers the
                // way the blink machine itself does (values + isUpdated
                // false) - the two-buffer lesson: input-buffer writes are
                // invisible while no channel composes.
                if (faceMode == 1) {
                    // Eyelids: the ambient machine (mtb_blinkgen.c) owns the
                    // composed pair and runs free - no pin. While it WAITS
                    // (state 0) it writes nothing, so visible residue there
                    // is a frozen writer's half-blink: pull the next natural
                    // blink close - one close-open cycle wipes it and ends
                    // eyes-open. One-shot by construction (post-blink lids
                    // are 0, the condition dies).
                    if (face->unk200 == 0) {
                        zeroBlinks();  // input hygiene: no recompose resurrection
                        if (const auto& comp = face->unk100;
                            comp.values && comp.count > 1 &&
                            (comp.values[0] > 0.05f || comp.values[1] > 0.05f) &&
                            face->blinkDelay > 0.3f) {
                            face->blinkDelay = 0.1f;
                            spdlog::debug("face live: composed lid residue, "
                                          "wipe blink pulled close.");
                        }
                        // ⚠ DIAGNOSTIC ONLY (bDiagnosticProbes) - THE BLINK IS
                        // UNVERIFIABLE BY EYE, AND THAT IS THE ACTUAL BLOCKER.
                        //
                        // Every measurable check on the blink fix passes: the
                        // machine is caught mid-blink with composed lids moving
                        // through real values, the engine's face caller runs
                        // while armed, and our change byte survives to be read.
                        // But a natural blink is ~0.2 s once every several
                        // seconds, and the field verdict was "hard to tell" -
                        // so the fix has sat unconfirmed, which is exactly how
                        // 0.6.0 shipped a changelog entry for a fix that did
                        // not work.
                        //
                        // This does not forge a blink or write a lid value. It
                        // only shortens the ENGINE's own countdown so its own
                        // machine blinks about twice a second. If the eyes
                        // visibly blink in a menu with probes on, the composed
                        // lids are reaching the mesh and natural blinking works
                        // too; if they stay open, the bake is still not landing
                        // and the fix is inert. Unmistakable either way.
                        //
                        // ⚠ It must testify to its own liveness: a probe that
                        // logs nothing is indistinguishable from a probe that
                        // never installed, and this one had never once been
                        // exercised with probes actually on (the run that was
                        // supposed to reached bDelay 7.5 s, proving the cap
                        // never applied). blinkCaps_ is that testimony.
                        if (Settings::GetSingleton().blinkStressTest &&
                            face->blinkDelay > 0.5f) {
                            face->blinkDelay = 0.5f;
                            ++blinkCaps_;
                        }
                    }
                    // Gaze (composed 8-11): frozen off-level gaze both looks
                    // stuck and can PARK the machine in its look-hold states
                    // (3/4 exit on a values[8] threshold test that nothing
                    // re-evaluates while gaze is pause-frozen -> no blinking
                    // all menu). Settle toward level; the machine un-parks
                    // itself on the next Update. A genuinely active gaze
                    // channel recomposes over this write and simply wins.
                    // Phonemes (composed unk140): a mouth caught mid-shape
                    // holds forever otherwise - settle it closed; the mood
                    // expression (unk0C0) carries the look and is untouched.
                    const float k = 1.0f - (std::min)(1.0f, dt * 8.0f);
                    const auto settle = [k](RE::BSFaceGenKeyframeMultiple& a_kf,
                                            std::uint32_t a_from,
                                            std::uint32_t a_to) {
                        if (!a_kf.values) {
                            return;
                        }
                        for (std::uint32_t i = a_from;
                             i <= a_to && i < a_kf.count; ++i) {
                            if (float& v = a_kf.values[i]; v != 0.0f) {
                                v *= k;
                                if (v > -0.01f && v < 0.01f) {
                                    v = 0.0f;
                                }
                                a_kf.isUpdated = false;
                            }
                        }
                    };
                    settle(face->unk100, 8, 11);
                    if (face->unk140.count > 0) {
                        settle(face->unk140, 0, face->unk140.count - 1);
                    }
                    // Input phonemes: cleared so a later recompose can't
                    // resurrect the caught mouth (the frozen writer re-drives
                    // itself after unpause anyway).
                    if (auto& ph = face->phenomeKeyFrame; ph.values) {
                        for (std::uint32_t i = 0; i < ph.count; ++i) {
                            if (ph.values[i] != 0.0f) {
                                ph.SetValue(i, 0.0f);
                            }
                        }
                    }
                }
                if (pinBlinks) {
                    // r34.5 - DECOMPILE VERDICT (mtb_blinkgen.c, the ambient
                    // blink generator 0x1403c2930): +0x200 is a STATE machine
                    // (0 waiting / 1 closing / 2 opening / 3-4 holds) and
                    // +0x204 (blinkDelay) is its TIMER. In state 0 the timer
                    // just counts down and NOTHING is written - but an arm
                    // that catches state 1/2 with our timer pin holds the
                    // machine MID-BLINK forever, rewriting the eyelids inside
                    // every Update (baked before any post-zero could clean
                    // it). The r32-r34 "still blinking" was OUR OWN pin.
                    // State 0 + fat timer = the machine idles silently.
                    face->unk200 = 0;         // blink state: WAITING
                    face->blinkDelay = 2.0f;  // its countdown, never reaches 0
                    zeroBlinks();
                }
                // ⚠ 0.7.1 - THE MIRROR STOPPED HALFWAY, AND THAT WAS THE WHOLE
                // BLINK BUG. The engine's caller (0x3D9440) does FOUR things:
                //
                //   1. entry gate - bail unless its own force arg is set OR
                //      +0x218 is non-zero;
                //   2. clear +0x218, call Update, KEEP its "changed" return;
                //   3. if changed, register the face MODEL into the re-bake
                //      queue (DAT_142f07d18) - this is what puts new lid
                //      values on the actual mesh;
                //   4. set +0x215 on the way out.
                //
                // We copied 2 and skipped 3. So every write here moved the
                // DATA and never reached the MESH: whatever was last baked
                // before the pause stayed on screen. Caught mid-blink that is
                // frozen shut eyes; caught open it is no blinking at all - one
                // defect, and both field reports are it.
                //
                // Pre-clearing +0x218 made it worse: the engine's own caller
                // then FAILED its entry gate (step 1), so we were also eating
                // refresh requests posted by other face mods.
                //
                // Step 3 is not ours to call - it wants the face model, not
                // this anim data, and takes the global face lock. So ask for it
                // instead: leave the pending flag SET whenever Update reports a
                // change, and the engine's caller does 1-4 itself.
                //
                // PROBE (0.7.1): +0x215 is set by that caller on every path
                // past its entry gate. Clearing it here means a set value next
                // tick PROVES the caller is alive inside a paused menu - which
                // is exactly what decides whether asking for a re-bake can work
                // at all. If this logs "is NOT running", the request cannot be
                // served and we have to drive the bake ourselves.
                // +0x215 sits inside unk214 (a uint16), so it is reached by
                // byte offset rather than by member.
                auto* const rawFace   = reinterpret_cast<std::uint8_t*>(face);
                const bool  callerRan = rawFace[0x215] != 0;
                rawFace[0x215]        = 0;
                // Did the change byte we set LAST tick survive? Update clears
                // +0x217 as its very first act and nothing else touches it, so
                // a 0 here proves the engine's caller ran Update ITSELF (its
                // path B) and clobbered our request. That single bit decides
                // the next move and cannot be read any other way.
                const bool ourFlagSurvived = face->unk217 != 0;
                if (callerRan != faceCallerSeen_) {
                    faceCallerSeen_ = callerRan;
                    spdlog::info("face live: engine face caller (0x3D9440) {} while armed "
                                 "(+0x215 {}); our change byte {}: {}.",
                                 callerRan ? "IS running" : "is NOT running",
                                 callerRan ? "came back set" : "stayed clear",
                                 ourFlagSurvived ? "SURVIVED" : "was CLEARED",
                                 ourFlagSurvived
                                     ? "the caller reads +0x217 (path A), so forcing it "
                                       "below is the fix"
                                     : "the caller re-runs Update itself (path B) and "
                                       "clobbers it; while paused its own dt is 0, which "
                                       "SKIPS the recompose entirely, so no write of ours "
                                       "can ever signal a change; the bake has to be "
                                       "driven off the face MODEL (+0x161) instead");
                }
                g_faceGenUpdate(face, dt, true);
                // ⚠ 0.7.1 - THE ACTUAL BAKE GATE. Read off the decompile, not
                // guessed: Update CLEARS +0x217 on entry, runs the blink
                // generator, and then ORs in a change bit ONLY when one of the
                // INPUT channels recomposed. The generator writes the COMPOSED
                // lid pair directly and never touches an input channel - so an
                // ordinary blink leaves +0x217 at 0, Update returns 0, and the
                // engine's caller skips the morph re-bake entirely.
                //
                // That is why the machine measurably blinks (bDelay counts down
                // and re-randomises on every completed blink, which only
                // happens on the state-2 exit) while the face on screen never
                // changes. The data was always fine. Nothing ever asked for the
                // mesh to be rebuilt.
                //
                // So say it changed, because it did. +0x217 is the caller's own
                // "something moved" signal and this is the one write that
                // reaches the mesh.
                face->unk217 = 1;
                face->unk218 = 1;  // and let it past the entry gate
                if (pinBlinks) {
                    zeroBlinks();  // other writers (MFG-style mods) stay covered
                }

                // Count the engine's own blink machine so the session can be
                // reported as numbers instead of an impression (Bubble.h).
                // Sampled AFTER Update, so these are the states the machine
                // actually advanced through on our dt.
                {
                    const int st = static_cast<int>(face->unk200);
                    if (blinkStatePrev_ != 1 && st == 1) {
                        ++blinkStarts_;
                    }
                    if (blinkStatePrev_ == 2 && st != 2) {
                        ++blinkCompletes_;
                    }
                    blinkStatePrev_ = st;
                    if (const auto& comp = face->unk100;
                        comp.values && comp.count > 1 && comp.values[0] > blinkLidPeak_) {
                        blinkLidPeak_ = comp.values[0];
                    }
                }

                // ⚠ 0.7.1 r2 - THE BAKE, AND THE ANSWER TO "CAN THE FACE MESH
                // REFRESH AT ALL WHILE THE WORLD IS PAUSED".
                //
                // It can. Nothing was ever asking it to.
                //
                // Everything above moves the DATA, and every measurement of the
                // data has passed for three rounds: the machine runs, blinks
                // complete, the change byte survives, the engine's face caller
                // is alive. The mesh still never moved, because setting the
                // change byte does not bake anything. Decompiling
                // BSFaceGenNiNode::UpdateDownwardPass end to end (Offsets.h,
                // Tools\re\research\mtb_facebake*.c) shows all it does with
                // that byte is REGISTER the face node in a global queue, and
                // the queue's only consumer runs from a parallel job batch on
                // Main::Update's NOT-PAUSED branch. A paused menu never drains
                // it, so the mesh keeps whatever was baked before the pause -
                // shut eyes if it caught a blink, no blinking at all if it did
                // not. One defect, and both field reports are it.
                //
                // Main::Update's PAUSED branch does bake one face, gated on a
                // single menu:
                //     if (UI::IsMenuOpen(InterfaceStrings::raceSexMenu))
                //         FaceGenApplyMorphs(player->GetFaceNodeSkinned(), true);
                // That is why RaceMenu has a live face in a paused game and
                // nothing else does. So do exactly what the engine does, for
                // our menus: same call, same force flag, same thread (this tick
                // runs from a write_call inside Main::Update).
                if (Settings::GetSingleton().faceMeshRefresh) {
                    auto* faceNode = player->GetFaceNodeSkinned();
                    if (faceNode) {
                        g_faceGenApplyMorphs(faceNode, true);
                        if (++faceMeshApplies_ == 1) {
                            spdlog::info("face live: MESH BAKE running: "
                                         "FaceGenApplyMorphs(force) per tick over {} "
                                         "face children. This is the half that "
                                         "reaches the geometry.",
                                         faceNode->GetChildren().capacity());
                        }
                    } else if (faceMeshApplies_ == 0 && !faceNodeMissingLogged_) {
                        // Never conclude from absent evidence: if the totals at
                        // disarm read zero, this line says whether it was the
                        // node or the call.
                        faceNodeMissingLogged_ = true;
                        spdlog::warn("face live: no skinned face node on the player, "
                                     "the mesh bake cannot run (eyes will hold "
                                     "whatever was baked before the pause).");
                    }
                }
            }
        }

        if (Settings::GetSingleton().tickMagicCasters && !engineAnimates) {
            // Equipped-spell hand art attaches in ActorMagicCaster::Update
            // (vfunc 0x1D), which runs on the paused world clock - so a
            // spell equipped in MagicMenu shows nothing until unpause. The
            // charge/drain paths inside are casting-state-gated (states 2/6
            // can't occur in a menu), leaving art maintenance + art-3D tick,
            // which is exactly what we want (decompile: mtb_magiccaster.c).
            for (const auto source : { RE::MagicSystem::CastingSource::kLeftHand,
                                       RE::MagicSystem::CastingSource::kRightHand }) {
                if (auto* caster = player->GetMagicCaster(source)) {
                    static_cast<RE::ActorMagicCaster*>(caster)->Update(dt);
                }
            }
        }

        // THE PREVIEW CONTRACT: any non-idle clip sets bAnimationDriven,
        // and the third-person camera locks free rotation while animation-
        // driven (the vanilla killmove/scripted-idle rule; SPIM fights the
        // same thing via m_shouldDisableAnimCam, spim_rotate.c). Cleared
        // AFTER the graph tick so the flag reads false for the rest of the
        // frame. The r22 forensics acquitted this flag as the LIVE rotation
        // blocker (animDriven=false in every sampled row) - the clear stays
        // as contract hygiene: animations play, they never drive.
        player->SetGraphVariableBool("bAnimationDriven", false);

        // THE PREVIEW SPIN (F-14, opt-in). Rotation at the one level
        // nothing recomputes while paused: the player's ROOT NODE, set
        // absolutely from the arm-time basis - above whatever eats body
        // rotation below (non-idle clips, r20-22), under any animation,
        // with real hair/cloth physics (the skeleton actually moves - a
        // camera orbit moves nothing). Input: our own right-drag sink
        // accumulates spinTarget_; the easing below makes it silk. The
        // SPIM freeRot harvest stays for setups where SPIM still drives
        // (its writes read as drag intent; SPII camera builds never touch
        // freeRot, so it no-ops there). data.angle stays pinned while
        // spinning: the basis holds and the exit heading is the entry
        // heading. Disarm/ForceReset un-compose the node.
        // r55: preview spin DISABLED while mounted (user: "disable rotation
        // when we are on a horse for now"). The horse is a separate actor
        // and rotating the player root alone spun the rider on a still
        // horse; the shared-yaw mount rotation (r54) didn't take. Revisit
        // with the mount 3D root diagnostic if mounted spin is wanted.
        if (Settings::GetSingleton().previewSpin && !engineAnimates &&
            !player->IsOnMount()) {
            player->data.angle.z = armedHeading_;
            if (auto* tps = GetThirdPersonState()) {
                const float delta = freeRotArm_ - tps->freeRotation.x;
                if (delta != 0.0f) {
                    if (std::fabs(delta) > 1.0f) {
                        // Not a drag: SPIM re-ran its open-time camera
                        // setup (menu switch re-park) - adopt the new park
                        // instead of spinning through the jump.
                        freeRotArm_ = tps->freeRotation.x;
                    } else {
                        // r61 (user: "when we detect Show Player In Menus we
                        // want to disable their rotation and just use our
                        // rotation"). SPIM rotates on RIGHT-MOUSE HELD - the
                        // very input our spin sink uses (its Event.cpp: mouse
                        // button 1 arms allowRotation, then each MouseMove does
                        // SetRotationZ on the PLAYER *and* freeRotation.x -=
                        // amt on the camera; it is a body turn with a camera
                        // counter-turn, not an orbit). So one drag fed BOTH
                        // systems: our sink plus this harvest. Since the r60
                        // sign flip they push the same way = double-speed spin
                        // (before r60 they fought and part-cancelled) - neither
                        // is right.
                        // With SPIM present we keep the PIN and drop the DELTA:
                        // their player-heading write is already overwritten by
                        // the armedHeading_ pin above, and the reset below
                        // undoes their camera counter-turn, so their rotation
                        // is fully neutralised and our sink is the only thing
                        // that spins the character. previewSpin gates this
                        // whole block, so switching our spin off hands their
                        // rotation straight back.
                        // Without SPIM the harvest is unchanged (it no-ops on
                        // SPII builds, which never touch freeRot).
                        if (RotationOwnershipPolicy::OwnsSpimRotation(
                                OwnView::SpimPresent(),
                                Settings::GetSingleton().overrideSpimRotation)) {
                            // One line per session: field-proof that the
                            // override actually engaged (it cannot be tested
                            // on a load order without SPIM).
                            static bool logged = false;
                            if (!logged) {
                                logged = true;
                                spdlog::info("preview spin: Show Player In Menus detected: "
                                             "its rotation is neutralised, our spin owns the "
                                             "character (bOverrideSpimRotation=1).");
                            }
                        } else {
                            spinTarget_ += delta;
                        }
                        tps->freeRotation.x = freeRotArm_;
                    }
                }
            }
            // Controller (F-14 v3): DIRECT right-stick rotation - no hold
            // button (the design SPIM itself ships; its Nolvus preset runs
            // iGamepadTurnMethod=0). The item-preview conflict is solved by
            // SPIM's own inspect gate: the stick spins the character only
            // while the item 3D is NOT zoomed; MenuInputGate blanks the
            // menu's item-rotate user event in exactly that window, so one
            // input means one thing per mode. Deadzone for stick drift.
            if (const float stickX = spinStickX_.load();
                std::fabs(stickX) > 0.15f && ItemPreviewZoomedOut()) {
                // Same convention as the right-drag (r60): pushing the stick
                // left turns the character to their left. Both direct inputs
                // flipped together so they can never disagree.
                spinTarget_ -= stickX *
                               Settings::GetSingleton().spinStickSensitivity * dt;
            }
            // Open 3: decide ownership BEFORE easing, because a change zeroes
            // the accumulator and easing a value that is about to be discarded
            // would put one spun frame on screen at the handover.
            const bool companionOwnsSpin = SyncSpinCompanion();
            spinYaw_ += (spinTarget_ - spinYaw_) * (std::min)(1.0f, dt * 14.0f);
            if (spinBasisValid_) {
                RE::NiMatrix3 spin;
                spin.EulerAnglesToAxesZXY(0.0f, 0.0f, spinYaw_);
                if (auto* root = player->Get3D(false)) {
                    root->local.rotate =
                        companionOwnsSpin ? rootBaseRotate_ : spin * rootBaseRotate_;
                }
                if (auto* horse = spinHorseRoot_.get()) {  // r54: rider + horse together
                    horse->local.rotate =
                        companionOwnsSpin ? horseBaseRotate_ : spin * horseBaseRotate_;
                }
                if (auto* her = spinCompanionRoot_.get()) {
                    her->local.rotate = spin * companionBaseRotate_;
                    // Her own publish. The pass below reaches the PLAYER's node
                    // only, and this write lands AFTER TickFramedCompanion has
                    // already published her for this frame - so without this the
                    // spin is invisible until the next tick, which is the same
                    // stale-transform defect Open 1 is about.
                    RE::NiUpdateData ctx;
                    ctx.time = 0.0f;
                    ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                    her->Update(ctx);
                }
            }
        }

        if (Settings::GetSingleton().tickAnimation || Settings::GetSingleton().tickFace ||
            Settings::GetSingleton().tickMagicCasters || spinYaw_ != 0.0f) {
            // The paused scene graph may never run a downward pass, leaving
            // new local transforms/morphs invisible (the preview spin's
            // root write needs the same propagation). Same kick AP uses
            // after its paused-menu equipment rebuild.
            if (auto* third = player->Get3D(false)) {
                RE::NiUpdateData ctx;
                ctx.time = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                third->Update(ctx);
            }
        }

        // r18: NOT gated on raceMenu (the F-9 note above keeps the FSMP drive
        // running there on purpose), but it MUST stand down when the world is
        // live - the engine steps SMP itself then, and stepping it again is a
        // double-step. This is the one drive site engineAnimates does not
        // already cover, because its two halves want different answers.
        if (Settings::GetSingleton().driveSmp && a_paused) {
            FsmpDrive::Step(dt);
        }

        // The pivot camera, after the spin and its propagation pass. The pivot
        // is a bone's WORLD transform, so reading it before those have run this
        // frame would aim the camera at where the subject was, and the whole
        // point of orbiting the character is that the character stays centred.
        //
        // Same subject the spin uses: a framed follower is the shot, so she is
        // what the camera circles. Resolved fresh rather than cached, because a
        // handle can die mid-menu and the player is the honest fallback.
        {
            auto* subject = Declutter::FramedCompanion().get().get();
            StudioCamera::Tick(dt, subject ? subject : player);
        }

        if (Settings::GetSingleton().verboseLog && telemetryCountdown_-- == 0) {
            telemetryCountdown_ = 60;  // roughly once a second
            LogTelemetry(a_main, a_paused, dt);
        }

        // OS-103(b), LAST THING IN THE ARMED FRAME. This closes the ring and is
        // also where the probe re-sets the cull, so the next frame starts with
        // him down and has something to lose again. Anything added below this
        // line lands in the wrap -> pre-dispatch gap and will be misattributed
        // to whatever runs between frames, so keep it last.
        Declutter::ProbePlayerCull(Declutter::CullPhase::kWrap);
    }

    void Bubble::LogTelemetry(RE::Main* a_main, bool a_paused, float a_dt) {
        auto* ui = RE::UI::GetSingleton();
        // Camera columns: state id + world position - evidence for the
        // parked camera-collision work (does the pull-in happen live?).
        auto* camera = RE::PlayerCamera::GetSingleton();
        const auto* camState = camera ? camera->currentState.get() : nullptr;
        const auto camId = camState ? static_cast<int>(camState->id) : -1;
        RE::NiPoint3 camPos;
        if (auto* root = camera ? camera->cameraRoot.get() : nullptr) {
            camPos = root->world.translate;
        }
        // B-7 rotation forensics (kept until the pin is field-confirmed):
        // heading == pin in every row means the body pin holds against
        // whatever rode the graph tick; freeRot moving freely on drag with
        // the sum no longer constant = the orbit is really orbiting. If
        // heading stays pinned in the LOG but the body still visibly
        // rotates, the writer is node-side (below data.angle) - that would
        // be the next lead, and this line is the discriminator.
        float freeRotX = 0.0f, freeRotY = 0.0f;
        bool freeRotEnabled = false;
        if (camState && camState->id == RE::CameraState::kThirdPerson) {
            const auto* tps = static_cast<const RE::ThirdPersonState*>(camState);
            freeRotX = tps->freeRotation.x;
            freeRotY = tps->freeRotation.y;
            freeRotEnabled = tps->freeRotationEnabled;
        }
        bool animDriven = false;
        float heading = 0.0f;
        float blinkL = -1.0f, blinkDelay = -1.0f;
        // F-16 discriminators. bState frozen at 0 with bDelay never
        // shrinking across rows = the ambient generator is globally gated
        // off (its enable byte) and we must drive blinks ourselves. bState
        // stuck at 3/4 = the look-hold parking (the gaze settle should
        // prevent it). bState cycling 0->1->2 with cLid moving while the
        // face visibly never blinks = a bake-side break. cPhon shrinking
        // to 0 = the mouth settle working.
        std::uint32_t blinkState = 0xFFFF;
        float cLidL = -1.0f, cGaze8 = -1.0f, cPhonMax = -1.0f;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            player->GetGraphVariableBool("bAnimationDriven", animDriven);
            heading = player->data.angle.z;
            // F-13 discriminator: a 0.00 here with visible blinking on
            // screen convicts a writer INSIDE the face update (bake-time).
            if (auto* face = player->GetFaceGenAnimationData()) {
                if (face->modifierKeyFrame.values && face->modifierKeyFrame.count > 0) {
                    blinkL = face->modifierKeyFrame.values[0];
                }
                blinkDelay = face->blinkDelay;
                blinkState = face->unk200;
                if (face->unk100.values && face->unk100.count > 8) {
                    cLidL = face->unk100.values[0];
                    cGaze8 = face->unk100.values[8];
                }
                cPhonMax = 0.0f;
                for (std::uint32_t i = 0;
                     face->unk140.values && i < face->unk140.count; ++i) {
                    cPhonMax = (std::max)(cPhonMax, std::fabs(face->unk140.values[i]));
                }
            }
        }
        spdlog::debug(
            "tick #{:>5}: dt={:.4f} | engine slowDt={:.4f} realDt={:.4f} dt3={:.4f} | "
            "freezeTime={} numPauses={} uiPaused={} | fsmp={} cbpc={} | cam st={} "
            "pos=({:.1f},{:.1f},{:.1f}) | freeRot=({:.2f},{:.2f}) en={} animDriven={} "
            "heading={:.2f} pin={:.2f} | spin={:.2f}->{:.2f} park={:.2f} | T={:.2f} | "
            "blinkL={:.2f} bDelay={:.1f} bState={} cLid={:.2f} cGaze8={:.2f} "
            "cPhon={:.2f}",
            armedTicks_, a_dt, *g_slowDt.get(), *g_realDt.get(), *g_dtVariant3.get(),
            a_main->freezeTime, ui ? ui->numPausesGame : std::uint32_t(0xFFFF),
            a_paused, FsmpDrive::IsAvailable(), CbpcDrive::IsAvailable(), camId,
            camPos.x, camPos.y, camPos.z, freeRotX, freeRotY, freeRotEnabled, animDriven,
            heading, armedHeading_, spinYaw_, spinTarget_, freeRotArm_, Transition::Value(),
            blinkL, blinkDelay, blinkState, cLidL, cGaze8, cPhonMax);
    }
}

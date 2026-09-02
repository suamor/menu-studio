#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

// The studio session: when it is allowed to exist, and what it owes back when
// it ends.
//
// Menu Studio has always had ONE lifetime, the menu session - a covered menu
// opens, the bubble counts it, builds the scene, holds the pause, and gives all
// of it back at the close. bWaitForOwnerContext splits that in two. The MENU
// session does not move: ShouldBubbleMenu still answers at open, countedMenus_
// still remembers, and the close still balances against the remembered set.
// The STUDIO session nests inside it and may enter and leave more than once
// while a single menu stays open.
//
// ⚠⚠ NO NEW PATH DECREMENTS menusOpen_. The r19c autopsy in Bubble.cpp is what
// happens when a close consults a predicate that changed mid-menu: the count is
// never given back and an armed bubble ticks into gameplay. Everything in this
// header is about the STUDIO session, which owns the pause, the rig, the
// lights, the backdrop and the declutter - and owns none of the counting.
//
// Engine-free on purpose, so the one decision that can cost a player their save
// session is testable without Skyrim. See tests/studio_session_policy_test.cpp.
namespace MTB::StudioSessionPolicy {

    // Everything the gate is allowed to look at.
    struct GateInput {
        // The INI setting, read live. The player can flip it from the panel
        // while the menu whose session it governs is already open.
        bool waitForOwnerContext = false;
        // Has ANY owner published a live context at least once in this game
        // session? Per-game-session, cleared on load and new game.
        bool ownerContextProven = false;
        // Is at least one owner holding a context right now?
        bool anyOwnerContextLive = false;
    };

    // ⚠⚠ ARMED BY PROOF, NOT BY CONFIGURATION. Fitting Room builds already in
    // the field call nothing at all. A player who updates Menu Studio, turns
    // this on, and is still on an older Fitting Room would otherwise get a
    // bubble that never comes up and nothing on screen telling them why.
    //
    // So the setting has no effect until something has demonstrated it can lift
    // the studio again, and from that first published context on it is real for
    // the rest of the session. The latch only ever moves in the direction that
    // has been proven safe, which is also why the two mods need no release
    // ordering between them: either half ships first and nothing breaks.
    [[nodiscard]] constexpr bool HoldsStudioDown(const GateInput& a_in) {
        return a_in.waitForOwnerContext && a_in.ownerContextProven &&
               !a_in.anyOwnerContextLive;
    }

    enum class Move {
        kNothing,
        kEnter,
        kLeave,
    };

    // One frame's reconcile. a_menuCounted is menusOpen_ > 0.
    //
    // The no-menu case is deliberately kNothing rather than kLeave: the menu
    // close already owns that edge, and the exit choreography (the switch
    // bridge, the sleek exit's hold window) runs off armedLastFrame_ and the
    // gate-hold frames. A reconcile that tore down the moment the count hit
    // zero would cut every menu switch in half.
    [[nodiscard]] constexpr Move ChooseMove(bool a_menuCounted, bool a_entered,
                                            bool a_gateHolds) {
        if (!a_menuCounted) {
            return Move::kNothing;
        }
        if (a_entered) {
            return a_gateHolds ? Move::kLeave : Move::kNothing;
        }
        return a_gateHolds ? Move::kNothing : Move::kEnter;
    }

    // ⚠⚠ A PAUSE ASKED FOR IS NOT A PAUSE OBSERVED, AND THE STUDIO SESSION MADE
    // THAT GAP REACHABLE. Under Skyrim Souls the studio's pause is ShadowPause,
    // which POSTS a show message to the UI queue: the engine moves
    // numPausesGame when it drains that queue, which is a later frame. At a menu
    // open nobody ever saw this, because the take runs in the open EVENT and the
    // first OnFrame is already past it. A studio session entering mid-menu runs
    // INSIDE OnFrame, so r18's arm decision read the pause in the same
    // millisecond it was asked for.
    //
    // Field 2026-08-13, three lines on one timestamp: "ShadowPause: shown",
    // "studio session ENTER #1", "game is UNPAUSED ... bubble dormant". The
    // latch is one way, so the session was over before it began: pause released,
    // ledger cleared, no camera, lighting only.
    //
    // So while a pause we hold has not shown up yet, the arm decision waits
    // instead of concluding the world is live. It cannot wait forever - a
    // session that is neither armed nor dormant is the studio in limbo - so the
    // window is counted in frames and running out decides exactly as before.
    //
    // ⚠ HOLDING is the whole difference between "not yet" and "no". A menu the
    // player listed as Souls-live takes no pause at all, so this is false for it
    // on the first frame and it latches dormant immediately, as it always did.
    inline constexpr int kPauseSettleFrames = 10;

    [[nodiscard]] constexpr bool PauseStillLanding(bool a_paused, bool a_holdingPause,
                                                   int a_settleFramesLeft) {
        return !a_paused && a_holdingPause && a_settleFramesLeft > 0;
    }

    // How long the graph is stepped after the player's 3D is REPLACED under an
    // armed menu. Long enough for the behaviour graph to leave its bind pose
    // and reach a standing idle, short enough that a freeze the player asked
    // for is back within a blink.
    //
    // ⚠ r28 CUT THIS FROM 90. The first field build spent a second and a half
    // visibly animating after every preset load, and the user reads that as
    // the freeze breaking - which is fair, because it is. The window only
    // needs to cover the bind-to-idle transition; what came after it was the
    // idle itself playing under a freeze that was supposed to be holding.
    inline constexpr int kReposeFrames = 24;

    // Did the player's 3D get REPLACED under us? Identity only - a rebuild
    // frees the node it replaces, so these values are compared and never
    // followed.
    //
    // ⚠ THE FIRST SIGHTING IS NOT A SWAP. An arm edge has no previous root to
    // compare against, and a rebuild that happened out in the world is the
    // pose the menu legitimately caught. Only a swap seen BETWEEN two armed
    // frames is the apply-inside-the-menu case this exists for (field r27: a
    // looks apply left the character in its A-pose for as long as the freeze
    // held).
    [[nodiscard]] constexpr bool RootWasSwapped(const void* a_lastArmedRoot,
                                                const void* a_rootNow) {
        return a_lastArmedRoot != nullptr && a_rootNow != nullptr &&
               a_lastArmedRoot != a_rootNow;
    }

    // What the studio session TOOK, so its Leave can give back exactly that.
    //
    // ⚠⚠ THE DANGEROUS HALF OF THE WHOLE CHANGE. ForcePause is refcounted engine
    // state - its own log lines show the counter going to 2 and coming back.
    // Today it is taken once per menu; under the context gate it is taken and
    // released repeatedly inside one menu, and a single unbalanced release
    // either leaves the world frozen with no menu open or leaves the menu
    // unpaused with the studio up. Neither is recoverable by the player.
    //
    // ⚠⚠ Drain() TAKES NO ARGUMENTS, AND THAT IS THE DESIGN. The release must
    // not be re-derived from the current settings or the current predicate,
    // because the settings panel is drawn INSIDE the menu whose session did the
    // taking and the player can change either mid-menu. That is r19c
    // generalised, and the safest way to obey it is to leave the caller nothing
    // to consult: a ledger with no inputs cannot read a stale one.
    //
    // A set rather than a count: EnsurePaused is idempotent per menu on its own
    // side, so a session that took the same menu twice owes exactly one release
    // back. Ordered, so a drain reads the same way twice in a log.
    class PauseLedger {
    public:
        // The studio session asked ForcePause to take this menu's pause.
        void RecordTaken(std::string_view a_menuName) {
            taken_.emplace(a_menuName);
        }

        // The menu CLOSED while the session was running. Its own close path and
        // the per-frame settle reconcile that one against the engine's
        // invariant, so the ledger drops it without owing a release - paying it
        // here as well is the double decrement that wraps an unsigned counter.
        void Forget(std::string_view a_menuName) {
            taken_.erase(std::string{ a_menuName });
        }

        // Leave. Exactly what was taken, once, and then the ledger is empty so
        // a second Leave in the same session pays nothing.
        [[nodiscard]] std::vector<std::string> Drain() {
            std::vector<std::string> out{ taken_.begin(), taken_.end() };
            taken_.clear();
            return out;
        }

        // Load / new game: the menus died with the save and the engine's own
        // counter came back with it, so drop the bookkeeping and hand nothing
        // back. Never a substitute for Drain on a live session.
        void Clear() { taken_.clear(); }

        [[nodiscard]] bool        Empty() const { return taken_.empty(); }
        [[nodiscard]] std::size_t Size() const { return taken_.size(); }

    private:
        std::set<std::string> taken_;
    };

    // Does the engine already stand the HUD down over this menu?
    //
    // ⚠ THE FOUR VANILLA NAMES, AND NOT "ANYTHING THE ENGINE KNOWS". These put
    // the game in menu mode and HUDMenu hides its own compass and crosshair, so
    // hiding it again would be a no-op there today and a behaviour change to a
    // shipped mod the day it stopped being one. Anything else in sMenus is
    // somebody's own window drawing over a live HUD, which is what the field
    // saw with Grid Inventory on 2026-08-27: the compass and the crosshair sat
    // over the void with the studio armed.
    //
    // ⚠ 'RaceSex Menu' IS IN HERE TOO even though it is off by default: it is
    // the engine's own character editor and hides the HUD like the rest, so a
    // player who adds it must not have their HUD hidden twice.
    [[nodiscard]] inline bool EngineHidesHud(std::string_view a_menuName) {
        return a_menuName == "InventoryMenu" || a_menuName == "ContainerMenu" ||
               a_menuName == "BarterMenu" || a_menuName == "MagicMenu" ||
               a_menuName == "RaceSex Menu";
    }

}  // namespace MTB::StudioSessionPolicy

#include "StudioSessionPolicy.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

namespace {
    namespace P = MTB::StudioSessionPolicy;

    bool Is(const std::vector<std::string>& a_got,
            const std::vector<std::string>& a_want) {
        return a_got == a_want;
    }
}

int main() {
    // ── The gate ───────────────────────────────────────────────────────────
    // Silence means "behave as today". A player on an older Fitting Room that
    // calls nothing must not get a bubble that never comes up and no way to
    // tell why, so the SETTING alone can never hold the studio down.
    {
        CHECK(!P::HoldsStudioDown({ .waitForOwnerContext = false,
                                    .ownerContextProven  = false,
                                    .anyOwnerContextLive = false }));
        CHECK(!P::HoldsStudioDown({ .waitForOwnerContext = true,
                                    .ownerContextProven  = false,
                                    .anyOwnerContextLive = false }));
        // Proven, and somebody is holding a context right now: the studio is
        // exactly what they asked for.
        CHECK(!P::HoldsStudioDown({ .waitForOwnerContext = true,
                                    .ownerContextProven  = true,
                                    .anyOwnerContextLive = true }));
        // Proven, nobody holding: this is the one combination that holds.
        CHECK(P::HoldsStudioDown({ .waitForOwnerContext = true,
                                   .ownerContextProven  = true,
                                   .anyOwnerContextLive = false }));
        // The setting still wins on its own way out: switch it off and the
        // studio comes back even mid-menu, proven or not.
        CHECK(!P::HoldsStudioDown({ .waitForOwnerContext = false,
                                    .ownerContextProven  = true,
                                    .anyOwnerContextLive = false }));
    }

    // The latch only ever moves in the direction something has already proven
    // safe: proof cannot be withdrawn, so once an owner has lifted the studio
    // once, the gate is entitled to hold it down for the rest of the session.
    {
        P::GateInput g{ .waitForOwnerContext = true,
                        .ownerContextProven  = false,
                        .anyOwnerContextLive = false };
        CHECK(!P::HoldsStudioDown(g));
        g.anyOwnerContextLive = true;   // an owner publishes
        g.ownerContextProven  = true;   // ...which is the proof
        CHECK(!P::HoldsStudioDown(g));
        g.anyOwnerContextLive = false;  // and withdraws
        CHECK(P::HoldsStudioDown(g));   // now the gate has teeth
    }

    // ── Enter and leave ────────────────────────────────────────────────────
    {
        using M = P::Move;
        // No counted menu: the menu session's own close owns that edge, and
        // the exit choreography runs off armedLastFrame_ rather than off this.
        CHECK(P::ChooseMove(false, false, false) == M::kNothing);
        CHECK(P::ChooseMove(false, true, true) == M::kNothing);
        // A counted menu with the gate open enters once and then sits still.
        CHECK(P::ChooseMove(true, false, false) == M::kEnter);
        CHECK(P::ChooseMove(true, true, false) == M::kNothing);
        // The gate closing under a live studio session takes it back down.
        CHECK(P::ChooseMove(true, true, true) == M::kLeave);
        // ...and holds it there rather than flapping.
        CHECK(P::ChooseMove(true, false, true) == M::kNothing);
    }

    // ⚠ NO ENTER/LEAVE LOOP. Repeated arm and disarm inside one menu is a
    // lifecycle nobody has exercised, so the one thing this decision must never
    // do is oscillate on a steady input. Drive it as a state machine and check
    // it settles.
    {
        using M = P::Move;
        bool entered = false;
        for (int i = 0; i < 8; ++i) {
            const auto move = P::ChooseMove(true, entered, false);
            if (i == 0) {
                CHECK(move == M::kEnter);
                entered = true;
            } else {
                CHECK(move == M::kNothing);
            }
        }
        for (int i = 0; i < 8; ++i) {
            const auto move = P::ChooseMove(true, entered, true);
            if (i == 0) {
                CHECK(move == M::kLeave);
                entered = false;
            } else {
                CHECK(move == M::kNothing);
            }
        }
    }

    // ── A pause asked for is not a pause observed ──────────────────────────
    // FIELD 2026-08-13, and it cost the whole first run of this feature. Under
    // Skyrim Souls the studio's pause is ShadowPause, which POSTS a show message
    // to the UI queue - the engine moves numPausesGame when it drains that
    // queue, a frame later. At a menu open the gap is invisible, because the
    // take happens in the open event and the first OnFrame is already a later
    // frame. A studio session entering MID-MENU runs inside OnFrame, so the arm
    // decision read the pause in the same millisecond it was asked for, saw an
    // unpaused world, and latched the session dormant. Three log lines, one
    // timestamp: shown, ENTER, "game is UNPAUSED ... dormant".
    {
        using P::PauseStillLanding;
        // The world is frozen: nothing is in flight, decide normally.
        CHECK(!PauseStillLanding(true, true, P::kPauseSettleFrames));
        CHECK(!PauseStillLanding(true, false, P::kPauseSettleFrames));
        // Unpaused and we hold nothing. This is a genuinely live menu - Souls
        // keeping it unpaused by the player's own choice - and it must latch
        // dormant at once, exactly as it did before this window existed.
        CHECK(!PauseStillLanding(false, false, P::kPauseSettleFrames));
        // Unpaused, and a pause we asked for has not landed. Wait.
        CHECK(PauseStillLanding(false, true, P::kPauseSettleFrames));
        CHECK(PauseStillLanding(false, true, 1));
        // ⚠ BOUNDED. If the pause never lands, waiting forever would leave the
        // session neither armed nor dormant and the studio in limbo. Out of
        // frames, decide as before.
        CHECK(!PauseStillLanding(false, true, 0));
        CHECK(!PauseStillLanding(false, true, -1));
    }

    // The window closes on its own within its budget, and it closes EARLY the
    // moment the pause shows up.
    {
        int  left = P::kPauseSettleFrames;
        int  waited = 0;
        bool paused = false;
        while (P::PauseStillLanding(paused, true, left)) {
            --left;
            ++waited;
            if (waited == 2) {
                paused = true;  // the queue drained
            }
        }
        CHECK(paused);
        CHECK(waited == 2);
        CHECK(left == P::kPauseSettleFrames - 2);
    }

    // ── The pause ledger ───────────────────────────────────────────────────
    // ⚠ THE ONE PART OF THIS CHANGE WHERE GETTING IT WRONG COSTS THE PLAYER
    // THEIR SESSION. ForcePause is refcounted engine state: an unbalanced
    // release leaves the world frozen with no menu open, or the menu unpaused
    // with the studio up. The rule is that the session records what it TOOK and
    // Leave gives back exactly that.
    {
        P::PauseLedger ledger;
        CHECK(ledger.Empty());
        CHECK(ledger.Size() == 0);
        ledger.RecordTaken("InventoryMenu");
        CHECK(!ledger.Empty());
        CHECK(Is(ledger.Drain(), { "InventoryMenu" }));
        // Drained once, and only once. A second Leave in the same session -
        // a close landing in the same call stack as a gate flip - must not
        // decrement the engine's counter a second time.
        CHECK(ledger.Empty());
        CHECK(Is(ledger.Drain(), {}));
    }

    // A take recorded twice is still ONE release. EnsurePaused is idempotent on
    // its own side (a menu already in g_forced returns settled), so a ledger
    // that counted instead of remembering would owe a decrement nobody took.
    {
        P::PauseLedger ledger;
        ledger.RecordTaken("InventoryMenu");
        ledger.RecordTaken("InventoryMenu");
        CHECK(ledger.Size() == 1);
        CHECK(Is(ledger.Drain(), { "InventoryMenu" }));
    }

    // A menu switch inside one studio session takes a pause per menu, and each
    // one is owed back. Drained in a stable order so a log line reads the same
    // way twice.
    {
        P::PauseLedger ledger;
        ledger.RecordTaken("MagicMenu");
        ledger.RecordTaken("InventoryMenu");
        CHECK(Is(ledger.Drain(), { "InventoryMenu", "MagicMenu" }));
    }

    // A menu that CLOSED while the studio session ran is forgotten rather than
    // released: its own close path plus the per-frame settle already reconcile
    // that one against the engine's invariant, and releasing it here as well is
    // the double decrement that wraps an unsigned counter.
    {
        P::PauseLedger ledger;
        ledger.RecordTaken("InventoryMenu");
        ledger.RecordTaken("MagicMenu");
        ledger.Forget("InventoryMenu");
        CHECK(ledger.Size() == 1);
        CHECK(Is(ledger.Drain(), { "MagicMenu" }));
    }

    // Forgetting something never taken is a no-op, not a phantom entry. Under
    // the gate a menu opens with no pause taken at all, so its close arrives
    // with nothing on the ledger every time.
    {
        P::PauseLedger ledger;
        ledger.Forget("InventoryMenu");
        CHECK(ledger.Empty());
        ledger.RecordTaken("MagicMenu");
        ledger.Forget("InventoryMenu");
        CHECK(Is(ledger.Drain(), { "MagicMenu" }));
    }

    // ⚠⚠ THE r19c LESSON, GENERALISED, AND THE REASON THE LEDGER IS SHAPED LIKE
    // THIS. r19c: a close that re-consulted a predicate the player had changed
    // mid-menu took the early-out and never gave menusOpen_ back. The player can
    // open the settings panel from inside the very menu whose studio session is
    // running and flip the gate, the space, force-pause, anything. So Drain
    // takes NO ARGUMENTS: there is no settings value and no predicate it could
    // consult even if a later edit tried to. This test drives every input that
    // could possibly have been consulted and shows the answer does not move.
    {
        P::PauseLedger ledger;
        ledger.RecordTaken("InventoryMenu");
        for (const bool wait : { false, true }) {
            for (const bool proven : { false, true }) {
                for (const bool live : { false, true }) {
                    const P::GateInput g{ .waitForOwnerContext = wait,
                                          .ownerContextProven  = proven,
                                          .anyOwnerContextLive = live };
                    (void)P::HoldsStudioDown(g);
                }
            }
        }
        CHECK(Is(ledger.Drain(), { "InventoryMenu" }));
    }

    // Clear is the load / new-game backstop: the menus died with the save and
    // the engine's own counter came back with it, so drop the bookkeeping
    // WITHOUT handing anything back.
    {
        P::PauseLedger ledger;
        ledger.RecordTaken("InventoryMenu");
        ledger.Clear();
        CHECK(ledger.Empty());
        CHECK(Is(ledger.Drain(), {}));
    }

    // The re-settle window's question: was the player's 3D replaced BETWEEN
    // two armed frames? A first sighting is not a swap (the arm edge has
    // nothing to compare against, and a rebuild out in the world is the pose
    // the menu legitimately caught), and neither is a root that went away.
    {
        int a = 0;
        int b = 0;
        const void* const rootA = &a;
        const void* const rootB = &b;
        CHECK(P::RootWasSwapped(rootA, rootB));         // the apply case
        CHECK(!P::RootWasSwapped(rootA, rootA));        // same skeleton, no swap
        CHECK(!P::RootWasSwapped(nullptr, rootA));      // first sighting
        CHECK(!P::RootWasSwapped(rootA, nullptr));      // 3D gone, not replaced
        CHECK(!P::RootWasSwapped(nullptr, nullptr));
        CHECK(P::kReposeFrames > 0);
    }

    if (g_failures == 0) {
        std::printf("studio session policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

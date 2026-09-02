#pragma once

namespace RE {
    class Actor;
}

// The lunge loop, on the framed companion.
//
// 0.7.1 fixed this for the PLAYER: a bubbled menu freezes the Papyrus VM, so a
// graph ticked in that state selects its next pose against stale state. The
// Nolvus OAR stance framework gates 420 of its 662 configs on magic-effect
// markers, the higher-priority "Idle Loop" fails its HasMagicEffect condition,
// the lower-priority "Idle Start" wins, a hkbManualSelectorGenerator LATCHES it,
// and a ~3.3 s step-into-stance clip repeats forever.
//
// ⚠ EVERY GUARD BUILT FOR THAT READS THE PLAYER SINGLETON. ClipProbe filters to
// the player's hkbCharacter pointers, AnimEventProbe attaches only to the
// player's graphs, and MenuAnimationHoldPolicy is fed entirely from player
// state. The companion has been ticked with none of it since she was added, and
// the field confirmed on 2026-07-31 that she does loop.
//
// She cannot simply be given the player's hold. That policy latches on
// equipOccurredThisSession, which trips on the first equip and never clears; in
// an outfit editor that is immediate and permanent, and it is exactly the bug
// `eaaf783` removed when she was a statue.
//
// So this watches instead of assuming. It is a separate module on purpose: the
// player's guards are field-proven and this must not be able to regress them.
namespace MTB::CompanionLunge {

    // Per armed tick, before her graph is stepped. Attaches the event sink to
    // her graph, and re-attaches when she changes or her 3D is rebuilt (which
    // Fitting Room does on most gear switches).
    void Sync(RE::Actor* a_companion);

    // TRUE once her event stream has been caught cycling. Latched for the rest
    // of the armed session: releasing it would re-enter the same loop, because
    // the thing that decides the pick cannot change while the menu is open.
    [[nodiscard]] bool ShouldHold();

    void ArmedSessionBegin();

    // The tag histogram and the verdict, at the disarm edge. This is the half
    // that matters most right now: nothing has ever recorded what her graph
    // does, so the repeat threshold below is a first estimate and this is what
    // turns it into a measured one.
    void ArmedSessionReport();

    // Detach and forget. Load / new game, and whenever the shot loses her.
    void Reset();

}  // namespace MTB::CompanionLunge

#pragma once

#include <cstddef>

namespace MTB {
    // "Dressing room" view: while the bubble is armed, hide what crowds or
    // occludes the player in the menu camera - nearby NPCs (radius around the
    // player) and Furniture/MovableStatic refs intersecting the camera→player
    // corridor. Purely visual (NiAVObject app-cull flag), fully restored on
    // disarm/menu close/ForceReset; nothing touches AI, cells or saves.
    namespace Declutter {
        // Re-scan and hide (idempotent, accumulates). Main thread, armed only.
        void Refresh();
        // Un-hide everything we hid. Idempotent; safe to call on any path.
        void RestoreAll();
        // r44: Sky::Update runs in the UNPAUSED close/switch/exit window and
        // un-culls its own branch every frame (field: "during the transition
        // we see the skybox") - re-assert the named world-feeder culls per
        // gap frame until the at-black restore. No-op unless this arm culled.
        // OS-103. Re-cull the player if something un-culled him after we stood
        // him down for a framed companion. Call every ARMED tick: the hide is
        // set once per Refresh and Show Player In Inventory renders him in this
        // very menu, so holding it is a per-tick job. No-op when we never hid
        // him, and no-op when he is still down.
        void ReassertPlayerHide();

        // OS-103(b). WHERE IN THE FRAME does the player's cull flag die?
        //
        // The per-tick re-assert (above) is backed out because it lost, and the
        // number it produced turned out to measure nothing: 15974 re-holds at
        // ~160/sec is OUR OWN frame rate, because ReassertPlayerHide ran once
        // per rendered frame from PlayerDispatchHook and found him un-culled on
        // essentially every call. All that proves is "cleared at least once per
        // frame". It says nothing about who.
        //
        // This probe answers a different question and is the one that
        // discriminates. It samples the flag at four fixed points around the
        // frame and tallies which GAP the clear happens in. The decisive pair
        // brackets the engine's own player dispatch, because
        // PlayerDispatchHook::thunk calls func(a_main) BEFORE our OnFrame, so a
        // clear in that gap implicates the engine path and a clear anywhere
        // else exonerates it.
        //
        // ⚠ IT WRITES THE CULL ONCE PER FRAME, at the wrap phase, and that is
        // deliberate and is NOT the backed-out fix returning. Without a re-set
        // there is nothing left to clear after the first frame and every later
        // sample reads the same un-culled state, so the probe would see one
        // transition per arm. The write exists to give each frame a fresh
        // subject to observe. It is gated behind bPlayerCullProbe, default OFF,
        // and costs literally nothing when off.
        enum class CullPhase : std::size_t {
            kPreDispatch = 0,   // in the thunk, BEFORE the engine's player dispatch
            kPostDispatch = 1,  // in the thunk, AFTER it. This pair is the money.
            kPostRefresh = 2,   // inside Tick, after the declutter sweeps
            kWrap = 3,          // end of Tick, and where the probe re-sets the cull
            kCount = 4
        };
        void ProbePlayerCull(CullPhase a_phase);

        // OS-103(b) round 2. A SELF-CONTAINED BRACKET around one suspect call,
        // kept apart from the ring above because this one does not fire every
        // frame and would corrupt the gap tally if it did.
        //
        // The suspect is TESObjectREFR::Update3DPosition (player vfunc 0x3F),
        // which is what Show Player In Inventory calls to put him on screen.
        // ⚠ THE OBVIOUS PRIOR ROUND DOES NOT COVER THIS. Round 6 asked whether
        // that call REBUILDS his 3D and answered no, 0 rebuilds in 17531
        // samples, and the whole SPII line of enquiry was dropped on it. But
        // un-culling an EXISTING node is a different act from replacing it, and
        // nothing has ever measured that one. Call ProbeUpdate3DBracket(true)
        // before the original and (false) after; the pair scores when the flag
        // was down going in and up coming out.
        void ProbeUpdate3DBracket(bool a_before);

        void ReassertWorldFeederCulls();

        // A SECOND actor the shot is deliberately about, who survives every
        // cull path exactly as the player and their mount do. Set by whoever
        // owns the shot: Fitting Room's outfit editor calls this when it is
        // dressing a follower instead of the player, so the character being
        // edited is the one on screen.
        //
        // Empty by default, so a solo studio shot is unchanged. Pass an empty
        // handle to clear. The CALLER owns clearing, deliberately: RestoreAll
        // also runs on a mid-menu declutter-mode change (immediately followed
        // by a fresh Refresh), so clearing there would silently drop the
        // companion out of the shot when the user touched an unrelated
        // setting. A handle left set after the editor closes is harmless: it
        // exempts one actor from a later sweep at worst, and a stale handle
        // simply fails to resolve.
        //
        // Changing this mid-arm does NOT retroactively un-hide anyone: the
        // culls already ran. Callers that need an immediate effect should let
        // the bubble cycle RestoreAll + Refresh, the same way a declutter-mode
        // change does. TookEffect() reports whether the current cast has been
        // swept yet, so a caller can tell "not framed" from "not yet framed".
        void SetFramedCompanion(RE::ActorHandle a_actor);
        [[nodiscard]] RE::ActorHandle FramedCompanion();
        [[nodiscard]] bool FramedCompanionTookEffect();

        // Whether the player is off screen right now so the companion can have
        // the shot to herself (bHidePlayerForCompanion, [Declutter]).
        //
        // Reports what is TRUE of the scene, not what was intended: it reads
        // the cull flag back after the attempt. StudioRig consults it so the
        // rig lights her alone instead of a midpoint between her and someone
        // the camera cannot see, and reading the flag back is what stops the
        // two modules disagreeing about who is on screen.
        [[nodiscard]] bool PlayerHiddenForCompanion();

        // OS-103(b). Do we WANT him off screen right now? This is the intent,
        // not the observation, and the two had to be split when the fix moved
        // from writing the cull flag to answering the question the engine asks
        // before it writes the flag itself.
        //
        // ⚠ PlayerHiddenForCompanion above reads the flag BACK and must keep
        // doing so: it is what stops the rig and this module disagreeing about
        // who is on screen. But the Is3rdPersonVisible hook cannot use it,
        // because under the fix that flag is set by the engine as a CONSEQUENCE
        // of what the hook returns, and a hook that keys on its own effect is a
        // latch, not a decision.
        [[nodiscard]] bool PlayerHideIntent();

        // Would this actor survive as a framed companion if she were named
        // right now? This is the PICKER's question, so it uses the entry
        // threshold with no hysteresis: "would she be accepted", not "is she
        // still accepted". A caller building a list of candidates uses it to
        // say so before the user commits, rather than letting them choose
        // someone the shot will silently refuse and fall back to the player on.
        [[nodiscard]] bool CouldFrameCompanion(RE::Actor* a_actor);
    }
}

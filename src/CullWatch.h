#pragma once

#include <cstdint>

namespace RE {
    class NiAVObject;
}

namespace MTB {
    // OS-103(b) round 3. NAME the code that clears the player's app-cull flag,
    // by watching the four bytes it lives in with a CPU hardware breakpoint and
    // recording the instruction address that trips it.
    //
    // ⚠ THIS EXISTS BECAUSE NARROWING RAN OUT OF ROAD. Three rounds of phase
    // probing eliminated a great deal and named nobody: the Main::Update
    // player-dispatch call site (0 of 24105), everything inside our own
    // OnFrame and Tick (0 of 24105 on all three inner gaps), and
    // Update3DPosition, which turned out never to be called at all during an
    // armed arm (0 brackets). 100% of the clears land in the one gap that
    // spans the rest of the frame, and there is no useful place left to put
    // another sample point. A write watch answers the question directly
    // instead of bisecting toward it.
    //
    // ⚠ AND A ZERO RESULT IS ALSO AN ANSWER. Debug registers are PER THREAD and
    // this arms them on the game thread only, so no hits means the write is
    // coming from another thread, which is a different investigation and worth
    // knowing before starting it.
    // ⚠ ROUND 4 RECORDS THE VALUE, NOT JUST THE ADDRESS. Round 3 answered "who
    // writes these four bytes" and that answer stands. It cannot answer the
    // question that is left, which is "who leaves bit 0 CLEAR and is never
    // followed by anyone setting it again", because a bare address tally cannot
    // tell a hide from a show. Two of the three writers found are scoped
    // scaffolding that hide and then restore, so counting their trips says
    // nothing about the state they leave behind.
    //
    // ⚠ AND IT RE-OPENS A MEASUREMENT THAT WAS READ AS A PASS. The phase probe
    // scored the fix at 0 clears of 5999, down from 24105 of 24105. That number
    // cannot carry the claim: the same commit removed the per-frame re-assert,
    // and the probe counts culled-then-un-culled TRANSITIONS. With nothing
    // putting the flag back each frame, one clear sticks and no second
    // transition is possible, so 0 is what it reads whether the vfunc hook
    // holds or not. An ordered log of writes and their results settles it
    // directly and needs no re-assert to do it.
    namespace CullWatch {
        // Watch a_node's flags word for WRITES. Idempotent: re-arming on the
        // same node does nothing, and on a different node moves the watch.
        // Safe to call every frame. No-op unless bPlayerCullProbe is on.
        void Arm(RE::NiAVObject* a_node);

        // Bump the frame counter that each recorded write is stamped with.
        // Call once per frame on the game thread. Cheap enough to be
        // unconditional; the stamp is what separates a write that sticks from
        // one that is undone on the very next instruction.
        void Tick();

        // Take the watch off. Always call this before the node can die; a
        // breakpoint left on freed memory fires on whatever lands there next.
        void Disarm();

        // Log what was captured, on the GAME thread. Nothing is logged from
        // inside the exception handler itself, deliberately: the handler runs
        // in an exception context where a logging call is the least safe thing
        // it could do, so it only records into a fixed array and returns.
        void Report(const char* a_why);
    }
}

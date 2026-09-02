#pragma once

namespace RE {
    class Actor;
}

// Orbit the camera around the character while a bubbled menu is open, and
// scroll in on them.
//
// This layers ON TOP of OwnView, which keeps owning the base framing and its
// whole save-and-restore ledger. Nothing here touches an INI setting or a
// ThirdPersonState member. It reads the shot the engine has already built,
// stores a revert ledger for the camera node alone, and writes a corrected
// transform derived from a pivot the caller names.
//
// ⚠ IT DOES NOT TOUCH THE CAMERA UNTIL THE PLAYER ASKS IT TO. The opening
// distance, yaw and pitch are read back off whatever framing is on screen at
// the moment of the first real input, not imposed at arm. That is deliberate
// and load-bearing twice over: the Show Player In Inventory look the player
// knows stays exactly as it was, and a view mod that pushes its own camera
// update after our arm cannot leave us holding a stale "original" to revert to.
//
// The arithmetic lives in StudioCameraPolicy.h with no engine types in it, so
// the parts that decide whether a camera is sane are tested off-engine.
namespace MTB::StudioCamera {

    // SKSEPlugin_Load, after AllocTrampoline and VersionCheck::Run. Re-stamps
    // our transform behind every door into the body that writes cameraRoot:
    // the vtable slot for virtual callers (all of SE), plus the two
    // devirtualized call sites AE compiled into the per-frame path.
    void InstallHook();

    // Menu open. Clears state; writes nothing.
    void Arm();

    // Once per armed tick, after the character spin has run. a_subject is
    // whoever the shot is about, so a framed follower is orbited instead of the
    // player. Null falls back to the player, which is also what a stale
    // companion handle resolves to.
    void Tick(float a_dt, RE::Actor* a_subject);

    // Menu close. Reverts the camera node if anything was ever written.
    void Disarm();

    // Game load. Drops state WITHOUT reverting: the camera stack belongs to the
    // incoming save and the node we recorded is gone with the old one.
    void DropOnLoad();

    // Back to the framing the layer started from, keeping the layer armed. The
    // escape hatch for orbiting into geometry and losing the character.
    void ResetOffsets();

    [[nodiscard]] bool Active();

    // Has the player built this framing by hand since the last reset? A
    // caller with pages of its own asks before deciding whether a page
    // change should keep the shot or hand back the whole character.
    [[nodiscard]] bool ShotWasPanned();

    // Point the shot at a named skeleton node, for a mod that knows what the
    // player is looking at - Fitting Room editing a head wants the head
    // framed, not the chest. a_closeness runs 0 (as far out as the boundary
    // allows) to 1 (as close as the near floor allows); NEGATIVE leaves the
    // distance alone and moves only the pivot.
    //
    // ⚠ THE PIVOT PERSISTS, THE DISTANCE DOES NOT. Holding the pivot is the
    // point: the player keeps orbiting the head for as long as they are
    // editing it. Holding the distance would fight them the moment they
    // touched the wheel, so the closeness is a one-shot request that ordinary
    // zooming then owns.
    //
    // ⚠ AND IT COUNTS AS THE PLAYER ASKING. This layer's contract is that it
    // writes nothing until asked, which is what keeps an untouched menu
    // identical to the shot the view mod built - so a focus request is treated
    // as that ask and will capture. It is refused outright when the player has
    // the camera switched off, which is the consent that matters.
    void FocusOnNode(const char* a_nodeName, float a_closeness);

    // Focus on a node and frame what HANGS OFF it: the pivot is the measured
    // centre of the geometry below the node and the distance comes from its
    // radius, so a dagger arrives tight and a greatsword arrives wide with no
    // table anywhere. a_fallbackCloseness is used only when there is nothing
    // to measure, where this behaves exactly like FocusOnNode.
    void FocusOnAttachment(const char* a_nodeName, float a_fallbackCloseness);

    // Back to the pivot the player's own settings name. The framing stays
    // where it is; only what the camera circles goes back.
    void ClearFocus();

    // Input, accumulated from Bubble's existing InputEvent sink. All three are
    // deltas; the gate is the sink's business, since only it sees the press
    // edge that the gate has to latch on.
    void AddOrbit(float a_dx, float a_dy);  // left drag
    void AddZoom(float a_steps);            // wheel, positive pulls in
    // Middle drag: grab the picture and slide it. Soft-bounded about the
    // subject and drawn back when overshot, so a framing can sit off-centre
    // without ever losing the character. Composes after the zoom track like
    // every other offset; recentre and disarm clear it.
    void AddPan(float a_dx, float a_dy);

    // ⚠ SetEditorZoomHold / ToggleEditorZoom / EnterEditorShot WERE DECLARED
    // HERE and were removed on 2026-08-07. They remapped the character editor's
    // LAlt zoom onto this camera, which was only ever needed for the window
    // where our re-stamp owned the node in that menu. The 2026-08-06 retreat
    // ended that window: the studio camera stands down in the editor, LAlt goes
    // back to the menu that advertises it, and all three lost their callers.
    // StudioCamera.cpp carries the full note where the definitions were.

    // Is the cursor somewhere a camera drag may start? Resolves the cursor
    // itself, since where a trustworthy reading comes from is its own small
    // problem and belongs in one place. False when no cursor can be read at
    // all, because refusing a drag costs a second attempt and taking a wrong
    // one eats a click meant for the menu.
    [[nodiscard]] bool CursorInZone();

}  // namespace MTB::StudioCamera

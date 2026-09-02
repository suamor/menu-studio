#pragma once

#include "CameraCloseProbePolicy.h"

#include <string>

// ── POST-MENU CAMERA WATCH ─────────────────────────────────────────────────
//
// Leaving a bubbled menu can strand the view in the framing that menu was
// using, until the player moves the camera and forces a rebuild. Three
// reasoned attempts at the exit have failed in the same way (docs/STATUS.md,
// 2026-08-04), so this forms no fourth hypothesis - it measures.
//
// ⚠ IT WATCHES EVERY BUBBLED MENU, NOT JUST THE CHARACTER EDITOR, and the
// widening is not cosmetic. The 2026-08-09 field report ("the third person
// camera gets locked up after closing the inventory, I can still look left and
// right but not up and down") is an ORDINARY INVENTORY close, which the
// editor-only version could not see at all. Every camera field the report
// could be about is written by OwnView's framing and handed back twice over,
// so the only question worth asking is which field was left changed, and that
// is exactly one grep for LEFT CHANGED in the verdict line.
//
// ⚠ EVERY OTHER CAMERA LINE IN THIS PLUGIN IS ARMED-ONLY. StudioCamera's
// summary prints at disarm and its telemetry runs on armed ticks at about 1 Hz,
// so the log goes quiet at exactly the frame the symptom starts. This opens on
// the close edge and runs past it ([[probe-fuse-must-outlive-the-symptom]]).
//
// WHAT IT DECIDES: whether anything still writes the camera root after the
// close, and through which door. StudioCamera already stands in all four
// doorways, so attribution is a compare either side of the engine's own update
// rather than a new hook. See CameraCloseProbePolicy.h for the four findings
// and why they rank the way they do.
namespace MTB::CameraCloseProbe {

    namespace P = CameraCloseProbePolicy;

    // The camera-root translation as it stood immediately before an engine
    // update ran. Invalid when the watch is closed, which is what makes the
    // hook-side cost a single relaxed load in normal play.
    struct Mark {
        bool  valid = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Is the watch open? The hooks call this first and do nothing otherwise.
    [[nodiscard]] bool Watching();

    // Called from inside StudioCamera's four hooks, either side of orig().
    // NoteAfter records that the door RAN as well as whether it changed the
    // node: a camera re-asserting one transform every frame is invisible to a
    // change test, and that is the shape most likely to be holding the strand.
    [[nodiscard]] Mark MarkBefore();
    void NoteAfter(P::Door a_door, const Mark& a_mark);

    // The open edge, before anything of ours touches the camera. Records the
    // framing the menu was entered FROM.
    //
    // ⚠ THIS IS WHAT THE 2026-08-04 CONTROL RUN MADE NECESSARY. That run
    // showed the camera root frozen for 177 frames after a vanilla close and
    // then moving only on player input, so "nothing is writing it" is what
    // NORMAL looks like and cannot by itself convict anything. What separates
    // the strand from the ordinary case is not who wrote but WHERE the node is
    // parked: back at the gameplay framing, or still at the menu's.
    //
    // Two jobs, and they are different jobs:
    //
    //  1. CANCEL any watch still running. A close followed by an open inside
    //     the switch window was never an exit, and its verdict would measure
    //     the next menu's framing being applied. Bubble solves the identical
    //     problem for its own view restore.
    //
    //  2. Capture the pre-menu baseline, but ONLY when a_sessionFresh.
    //     ⚠ OwnView::ApplyFraming returns early while a framing is already up,
    //     so a switch's second open sees the FRAMED camera: animCam true, FOV
    //     90, freeRotation.x 2.64. A baseline taken there is the framing
    //     measured against itself, and the verdict then prints a confident
    //     all-clear for the one bug this exists to find. That mistake has
    //     already been made once with the FOV alone, and its autopsy is in
    //     Bubble::Disarm beside the field-of-view park.
    void OnBubbleMenuOpened(const std::string& a_menuName, bool a_sessionFresh);

    // The close edge. Opens the watch immediately, because the symptom starts
    // HERE, but the verdict is held until the teardown has run and settled.
    //
    // a_wasBubbled: did we count this menu at its own open. False is the
    //     CONTROL run, and it is reachable in the shipped configuration.
    // a_wasArmed / a_wasDormant / a_wasLiveOnly: which SHAPE of session this
    //     was. Under Skyrim Souls a live menu latches the session dormant, and
    //     a dormant session retires the pre-menu camera reading as it latches
    //     (the world stays live under it), so its teardown hands nothing back. A
    //     verdict that cannot tell that apart from a FAILED restore is
    //     unreadable, and those two are the whole question.
    // a_parkHadCapture: did the rotation park have anything captured to give
    //     back. The park's capture is reached only from a live third-person
    //     camera, so a first-person or mounted arm captures nothing and the
    //     park is a no-op that leaves no trace. Without this flag that log is
    //     indistinguishable from a clean session.
    void OnBubbleMenuClosed(const std::string& a_menuName, bool a_wasBubbled,
                            bool a_wasArmed, bool a_wasDormant,
                            bool a_wasLiveOnly, bool a_parkHadCapture);

    // ⚠ THE FUSE, AND IT IS NOT A TIMER. Called from the tail of Bubble::Disarm
    // once the rotation park has written back, so the verdict grades what the
    // teardown LEFT rather than catching it mid-restore. A watch that never
    // receives this still speaks at the ceiling and says so: a teardown that
    // never ran is itself the finding.
    void NoteTeardownDone(const char* a_why);

    // Once per frame from Bubble::OnFrame, armed or not. Keeps its own clock:
    // the frames it cares about are gameplay frames after a close, where the
    // bubble's dt is not being computed at all.
    void Tick();

    // A save load invalidates everything the watch was holding.
    void DropOnLoad();

}  // namespace MTB::CameraCloseProbe

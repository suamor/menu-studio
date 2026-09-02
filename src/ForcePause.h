#pragma once

#include <string>
#include <string_view>

namespace MTB::ForcePause {
    // §3.1 force-pause ownership: a bubble menu that arrives WITHOUT kPausesGame
    // (Skyrim Souls strips it) gets one pause held on RE::UI::numPausesGame
    // directly at open and released at close - a self-balanced pair we own, so a
    // flag-stripping mod cannot swallow the release and leave the world frozen.
    // Menus outside sMenus are never touched, so Skyrim Souls keeps its behavior
    // everywhere else.
    void EnsurePaused(const std::string& a_menuName);  // menu-open event, in-stack
    void OnMenuClosed(const std::string& a_menuName,
                      bool a_externalCameraHandoff);  // release/bridge our held pause

    // Give back the pause taken for ONE menu that is STILL OPEN.
    //
    // ⚠ NOT OnMenuClosed, AND THE DIFFERENCE IS WHY THIS EXISTS. OnMenuClosed
    // deliberately releases nothing: it hands the menu to the per-frame settle,
    // which reconciles the counter once the menu has actually left the UI map.
    // A studio session leaving under bWaitForOwnerContext has no such moment -
    // the menu stays on the stack, so the settle would find it still open and
    // go on re-asserting the flag forever.
    //
    // ⚠ CALLED FROM THE STUDIO SESSION'S LEDGER AND NOWHERE ELSE. What is
    // released is what the session RECORDED taking, never what the current
    // settings imply it should be holding - see StudioSessionPolicy.h. Safe on
    // a name we never took: it says so and does nothing.
    void ReleaseFor(const std::string& a_menuName);

    // Do we hold a pause right now by ANY mechanism, whether or not the engine
    // has caught up with it yet?
    //
    // ⚠ THE "WHETHER OR NOT" IS THE WHOLE POINT. ShadowPause POSTS its show to
    // the UI queue, so numPausesGame moves a frame later; this answers true from
    // the moment the ask is made. That is what lets the arm decision tell "the
    // pause has not landed yet" from "the world is genuinely live", which are
    // the same reading of RE::UI::GameIsPaused and opposite conclusions. See
    // StudioSessionPolicy::PauseStillLanding.
    [[nodiscard]] bool Holding();
    void Reassert();     // per-frame, UI thread: release live if force-pause was turned off
    void ReleaseAll();   // drop every pause we hold right now (live toggle / disarm)
    void Reset();        // load/new-game backstop (menu instances died with the load)
}

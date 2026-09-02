#pragma once

#include <optional>
#include <string>

namespace MTB::OwnView {
    // F-15 phase 2: the bubble owns the third-person preview framing
    // whenever no view mod does - the full SPIM RotateCamera() recipe
    // (clean-room from the author's MIT source, Tools/ShowPlayerInMenus),
    // which is exactly the "Show Player In Inventory" look the user knows.

    // Does the bubble own the view for this arm? First-person arms are the
    // r30-proven rule (a first-person camera means no view mod switched);
    // third-person arms are owned when no loaded view mod covers a_menuName.
    [[nodiscard]] bool ShouldOwn(const std::string& a_menuName, bool a_firstPersonArm);

    // ⚠ WHAT THE CLOSE RETURNS TO, WHEN THAT IS NOT WHAT THE ARM FOUND. Field
    // 2026-09-02: an arm that finds kTween must not hand kTween back, because
    // the tween menu is gone by then and nothing leaves that state. The bubble
    // decides (CameraArmStatePolicy::ChooseReturnState, fed by the reading its
    // park took before the tween had the camera) and passes the answer in.
    // OwnView keeps no opinion of its own about the tween menu.
    struct PriorCamera {
        int   stateId = 0;  // RE::CameraState, as an int
        float worldFOV = 0.0f;
    };

    // Apply the framing (saves every original first). a_forcedThirdFromFirst
    // = this arm forced the camera out of first person (hand it back at exit).
    // a_prior, when given, is what the close hands back instead of the state
    // and field of view the arm found live.
    void ApplyFraming(bool a_forcedThirdFromFirst, bool a_mounted = false,
                      std::optional<PriorCamera> a_prior = std::nullopt);

    // Menu-close exit: full restore, SPIM ResetCamera order (first person
    // handed back first, camera update, mouse-wheel zoom speed LAST).
    void Disarm();

    // ForceReset (load/new game): restore only the process-global surfaces
    // (INI Settings, FOV, the persistent third-person state object) - actor
    // data and the camera stack belong to the incoming save.
    void DropOnLoad();

    [[nodiscard]] bool Active();

    // Does a loaded view provider cover this menu in the current player state?
    // Used at close to hand live camera updating back immediately instead of
    // holding SmoothCam behind the Skyrim Souls switch-pause bridge.
    [[nodiscard]] bool ExternalProviderCovers(const std::string& a_menuName);

    // Is Show Player In Menus loaded? (One module scan per session - DLLs
    // cannot hot-load.) SPIM rotates on RIGHT-MOUSE HELD, the same input as
    // our preview spin, so the spin tick needs to know it is there; see the
    // harvest branch in Bubble.cpp. Main-thread callers only, like the
    // coverage checks it shares its scan with.
    [[nodiscard]] bool SpimPresent();
}

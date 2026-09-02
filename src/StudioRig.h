#pragma once

namespace MTB::StudioRig {
    // THE SHOT'S FRONT, as a Skyrim yaw with 0 along +Y: the bearing from
    // a_centre out to the camera. a_fallback comes back when there is no camera
    // or when it is effectively overhead, where atan2 would return a bearing
    // built from float noise.
    //
    // Public because the rig is no longer the only thing that needs it. OS-107
    // turns the framed companion to face the lens with the same number the key
    // light is placed from, so that her body, the key and the fill all agree on
    // which way is front. Two derivations of "front" would drift apart the
    // moment either was touched.
    [[nodiscard]] float CameraBearing(const RE::NiPoint3& a_centre, float a_fallback);

    // Three-point studio lighting (key / fill / rim) around the player while
    // the bubble is armed in void mode. Pure render-side lights: engine
    // NiPointLights registered with the ShadowSceneNode - no LIGH forms, no
    // placed references, nothing a savegame could ever see.
    void Apply();     // arm edge, after StudioLight::Apply
    void Tick();      // per armed frame: re-assert transforms + bounds
    void PushFade();

    // r28d DIAGNOSTIC. Logs what each rig light looks like AT THE TOP of our
    // tick - i.e. what SURVIVED the engine's previous frame: bound radius,
    // culled flag, fade, parent, world position, and Transition::Value().
    //
    // Exists because the live-menu rig reports "3 light(s) up" while the field
    // sees nothing, brightness included ("even changing their brightness didn't
    // do anything"). Everything our own writes can prove is proven; what is
    // missing is what the LIVE engine does to the lights between our frames -
    // a pass the paused studio never runs, which would explain "works fine in
    // regular mode" exactly. Read before re-asserting, or the probe measures
    // our own writes instead of the engine's.
    void LogLiveState(const char* a_why);  // F-12: fade-only refresh for the teardown grace window
    void Remove();    // Disarm + ForceReset (all three exits per SPEC §4)
}

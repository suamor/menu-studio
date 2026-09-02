#pragma once

#include <string>

// The one door to Community Shaders' cubemap override (CSDC_* exports on
// CommunityShaders.dll, resolved at runtime - no headers shared, no link
// dependency). While an image backdrop is up, reflections on armor come from
// CS's screen-space capture, which cannot see the image sphere; handing CS
// the pack's pre-baked cubemap makes reflections agree with the backdrop.
//
// A plain CS build (or none) simply lacks the exports: Available() is false,
// every call degrades to a no-op, and the backdrop works as before. The
// cubemap must be a LOOSE file - CS reads it from disk, archives are not
// searched.
namespace MTB::CsCubemapBridge {

    // Whether the patched CS build is present (logs the answer once).
    [[nodiscard]] bool Available();

    // Points reflections at a cubemap DDS ("Data\Textures\..." spelling).
    // True when CS accepted the request.
    bool Push(const std::string& a_rootedCubeDds);

    // Returns reflections to CS's live capture. Safe to call when nothing
    // was pushed.
    void Clear();

    // Excludes the studio range from CS's exponential height fog while an
    // image backdrop is up (field 2026-08-20: the fog integral over the
    // sphere's ~800 units read as a grey veil - "washed out" - and no
    // brightness can cancel an inscatter veil). 0 in CS terms restores
    // normal fog; ClearFogFloor sends that. Degrades to a no-op on a stock
    // CS build, like everything else here.
    bool SetFogFloor(float a_units);
    void ClearFogFloor();

}  // namespace MTB::CsCubemapBridge

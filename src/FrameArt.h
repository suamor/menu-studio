#pragma once

// ⚠ imgui, NOT FUCK_API.h. That header needs CSimpleIniA and <Windows.h> to be
// in scope already, so including it from a header drags its whole prelude into
// every translation unit that wants a texture id, and the ones that have not
// set that up fail to compile inside FLICK's file rather than inside their own.
// ImTextureID is all this interface needs and imgui declares it on its own.
#include <imgui.h>  // ImTextureID

// The nine-slice frame art, ported from Fitting Room so this mod's tiles can be
// cut to the same shape its chrome is.
//
// ⚠⚠ WHY ART AND NOT GEOMETRY. ChamferPanel next door draws with quads, and a
// quad path can only ever produce an octagon: a straight bevel across the
// corner. The look being matched is a SCOOP, a disc bitten out of the corner,
// which is an arc and cannot be expressed as a polygon without spending
// triangles on it at every size. A texture costs one draw call and is exact.
// Fitting Room reached this the same way and the two mods now generate their
// PNGs from the same script - tools/make_frame_texture.py, `scoop` at radius 12,
// which produces byte-identical files in both trees. A change to the shape
// belongs in both.
//
// ⚠ BOTH ACCESSORS MAY RETURN 0 AND CALLERS MUST COPE. A missing or unreadable
// PNG has to leave a tile looking the way it did before this existed rather
// than leaving a hole where it was, so every call site keeps its quad path as
// the else branch. That is not defensive noise: these files ship in the FOMOD
// and a user who installs by hand can easily have the DLL without them.
namespace MTB::FrameArt {

    // The outline: white, with the frame's LINE in its alpha.
    [[nodiscard]] ImTextureID Frame();

    // The same shape with the FILL in its alpha rather than the line, for a
    // panel that has to meet a cut corner without bleeding out of it.
    [[nodiscard]] ImTextureID Fill();

}

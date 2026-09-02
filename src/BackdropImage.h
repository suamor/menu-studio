#pragma once

#include "PCH.h"

#include <string>

// Loading a texture by PATH through the engine's own loader, for the image
// sphere's Apply-time repoint. Ported from Fitting Room's TextureLoad (the
// field-proven original): an engine BSShaderTextureSet is created, the rooted
// path goes into slot 0, and the set's loader vfunc (slot 38, the exact call
// BSLightingShaderMaterialBase::OnLoadTextureSet makes) fills an NiPointer.
// The engine's texture cache dedups by path under it, and the resource system
// behind it sees archives - std::filesystem does not, so a BSA-shipped pack
// texture loads here while an exists() check on the same path says no.
//
// ⚠⚠ A MISSING FILE IS NOT A NULL. The engine substitutes its placeholder and
// hands back a live NiSourceTexture with a clean log, so a caller cannot tell
// the two apart by pointer. The Apply log line pairs the load with a
// loose-file existence note so a typo'd path is at least visible in the log.
namespace MTB::BackdropImage {

    // a_rooted is the full "Data\Textures\..." spelling
    // (BackdropImagePolicy::Rooted makes it). Null on a faulted load or when
    // the loader cannot run on this runtime.
    [[nodiscard]] RE::NiPointer<RE::NiSourceTexture> Load(const std::string& a_rooted);

}  // namespace MTB::BackdropImage

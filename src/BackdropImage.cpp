#include "BackdropImage.h"

#include "VersionCheck.h"

#include <Windows.h>  // the SEH vocabulary

#include <cstdint>

namespace MTB::BackdropImage {

    namespace {

        // BSShaderTextureSet creation, the one REL surface here: SE 99886 is
        // the ctor CommonLib mallocs behind, AE 107172 the engine's own
        // Create. The same pair FR's TextureLoad gates on; IdOk first so an
        // absent id degrades to "no image" instead of a fail-fast.
        inline constexpr REL::RelocationID kCreateId{ 99886, 107172 };

        // The set's loader: vfunc slot 38 fills an out NiPointer for one
        // texture slot. The vendored header declares SetTexture on this slot
        // with a storing signature the engine body does not have, so the call
        // goes through the vtable with the real one.
        using LoadTexture_t = void(RE::BSTextureSet*, std::uint32_t,
                                   RE::NiPointer<RE::NiSourceTexture>&);

        // A DDS is third-party bytes; the frame holds no C++ objects (MSVC
        // forbids __try beside unwinding), the callee owns them all.
        __declspec(noinline) bool LoadTextureSEH(RE::BSTextureSet*                    a_set,
                                                 RE::NiPointer<RE::NiSourceTexture>* a_out,
                                                 std::uint32_t*                      a_code) {
            *a_code = 0;
            __try {
                auto* vtbl = *reinterpret_cast<std::uintptr_t**>(a_set);
                reinterpret_cast<LoadTexture_t*>(vtbl[38])(a_set, 0, *a_out);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *a_code = static_cast<std::uint32_t>(GetExceptionCode());
                return false;
            }
        }

        bool LoaderOk() {
            static const bool ok = [] {
                const bool present = MTB::VersionCheck::IdOk(kCreateId);
                if (!present) {
                    spdlog::warn("BackdropImage: the BSShaderTextureSet create id is "
                                 "absent on this runtime. Image packs show the "
                                 "shipped sphere this session.");
                }
                return present;
            }();
            return ok;
        }

    }  // namespace

    RE::NiPointer<RE::NiSourceTexture> Load(const std::string& a_rooted) {
        RE::NiPointer<RE::NiSourceTexture> tex;
        if (a_rooted.empty() || !LoaderOk()) {
            return tex;
        }
        RE::NiPointer<RE::BSShaderTextureSet> set{ RE::BSShaderTextureSet::Create() };
        if (!set) {
            return tex;
        }
        // SetTexturePath copies.
        set->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, a_rooted.c_str());
        std::uint32_t code = 0;
        if (!LoadTextureSEH(set.get(), &tex, &code)) {
            spdlog::warn("BackdropImage: '{}' FAULTED with 0x{:08X}.", a_rooted, code);
            tex.reset();
        }
        return tex;
    }

}  // namespace MTB::BackdropImage

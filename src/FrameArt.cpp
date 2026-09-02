#include "PCH.h"

#include "FrameArt.h"

// ⚠ THIS ORDER IS LOAD-BEARING AND IT IS THE ONE EVERY OTHER FLICK-FACING FILE
// HERE USES. FUCK_API.h refers to CSimpleIniA in its INI callbacks and to the
// Win32 loader functions, and it declares neither, so SimpleIni has to be in
// scope before it and PCH.h has to have pulled <Windows.h> before that.
#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callbacks

#include "FUCK_API.h"

#include <string>

namespace MTB::FrameArt {

    namespace {

        // Data-relative, which is the path shape FUCK's own WidgetTemplate uses
        // and resolves wherever the mod manager has staged this mod. dist/ puts
        // these under the plugin's own folder rather than beside the INI, so a
        // future second texture has somewhere obvious to go.
        FUCK::Image Load(const char* a_stem) {
            const std::string path =
                std::string("Data/SKSE/Plugins/MenuStudio/icons/") + a_stem + ".png";
            FUCK::Image img(path.c_str());
            if (img.IsLoaded()) {
                spdlog::debug("FrameArt: loaded '{}' ({:.0f}x{:.0f}).", path, img.GetWidth(),
                              img.GetHeight());
            } else {
                spdlog::warn("FrameArt: '{}' did not load, tiles keep their straight bevel.",
                             path);
            }
            return img;
        }

        struct Store {
            FUCK::Image frame;
            FUCK::Image fill;
        };

        // ⚠⚠ DELIBERATELY NEVER DESTROYED, AND THIS IS COPIED FROM FITTING ROOM
        // ON PURPOSE RATHER THAN SIMPLIFIED. FUCK::Image's destructor calls back
        // into FUCK to release the texture. A function-local static would run
        // that at process exit, by which point the host DLL may already be
        // unloaded, and the call lands in freed code. The textures are wanted
        // for the whole session either way, so leaking the store is the cheap
        // way to guarantee we never call into an unloaded DLL.
        Store& Get() {
            static Store* const store = [] {
                auto* s = new Store{};
                s->frame = Load("frame");
                s->fill  = Load("frame_fill");
                return s;
            }();
            return *store;
        }

    }  // namespace

    ImTextureID Frame() {
        const auto& img = Get().frame;
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

    ImTextureID Fill() {
        const auto& img = Get().fill;
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

}

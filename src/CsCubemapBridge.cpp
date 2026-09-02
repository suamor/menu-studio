#include "CsCubemapBridge.h"

#include "PCH.h"

#include <Windows.h>

namespace MTB::CsCubemapBridge {

    namespace {

        using SetOverride_t = bool (*)(const char*);
        using ClearOverride_t = void (*)();
        using Version_t = std::uint32_t (*)();
        using SetFogFloor_t = bool (*)(float);
        using ClearFogFloor_t = void (*)();

        SetOverride_t   g_set = nullptr;
        ClearOverride_t g_clear = nullptr;
        SetFogFloor_t   g_setFog = nullptr;
        ClearFogFloor_t g_clearFog = nullptr;
        bool            g_pushed = false;
        bool            g_fogFloored = false;

        // Resolved once: CS loads before us or not at all this session, and a
        // handle lookup per Apply would re-log the same answer.
        bool Resolve() {
            static const bool ok = [] {
                const HMODULE cs = ::GetModuleHandleW(L"CommunityShaders.dll");
                if (!cs) {
                    spdlog::info("CsCubemapBridge: Community Shaders is not loaded, "
                                 "reflections keep CS's own behavior.");
                    return false;
                }
                const auto version = reinterpret_cast<Version_t>(
                    ::GetProcAddress(cs, "CSDC_OverrideVersion"));
                g_set = reinterpret_cast<SetOverride_t>(
                    ::GetProcAddress(cs, "CSDC_SetCubemapOverride"));
                g_clear = reinterpret_cast<ClearOverride_t>(
                    ::GetProcAddress(cs, "CSDC_ClearCubemapOverride"));
                g_setFog = reinterpret_cast<SetFogFloor_t>(
                    ::GetProcAddress(cs, "CSEHF_SetFogStartFloor"));
                g_clearFog = reinterpret_cast<ClearFogFloor_t>(
                    ::GetProcAddress(cs, "CSEHF_ClearFogStartFloor"));
                if (!version || !g_set || !g_clear) {
                    g_set = nullptr;
                    g_clear = nullptr;
                    g_setFog = nullptr;
                    g_clearFog = nullptr;
                    spdlog::info("CsCubemapBridge: this CS build has no cubemap override "
                                 "API, reflections keep CS's own behavior.");
                    return false;
                }
                spdlog::info("CsCubemapBridge: CS cubemap override v{} found ({} fog floor).",
                             version(), g_setFog && g_clearFog ? "with" : "without");
                return true;
            }();
            return ok;
        }

    }  // namespace

    bool Available() {
        return Resolve();
    }

    bool Push(const std::string& a_rootedCubeDds) {
        if (!Resolve() || a_rootedCubeDds.empty()) {
            return false;
        }
        if (g_set(a_rootedCubeDds.c_str())) {
            g_pushed = true;
            return true;
        }
        spdlog::warn("CsCubemapBridge: CS refused the override (feature off this session).");
        return false;
    }

    void Clear() {
        if (g_pushed && g_clear) {
            g_clear();
            g_pushed = false;
        }
    }

    bool SetFogFloor(float a_units) {
        if (!Resolve() || !g_setFog) {
            return false;
        }
        if (g_setFog(a_units)) {
            g_fogFloored = true;
            return true;
        }
        return false;
    }

    void ClearFogFloor() {
        if (g_fogFloored && g_clearFog) {
            g_clearFog();
            g_fogFloored = false;
        }
    }

}  // namespace MTB::CsCubemapBridge

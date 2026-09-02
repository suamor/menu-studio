#include "PCH.h"

#include "ItemPreviewBroker.h"

#include "ItemPreviewPolicy.h"
#include "Offsets.h"
#include "VersionCheck.h"

#include <safetyhook.hpp>

#include <atomic>

namespace {
    namespace Policy = MTB::ItemPreviewPolicy;

    // UpdateMagic3D is the worker used by UpdateItem3D. On a cache hit it calls
    // Clear3D and then explicitly clears kHidden on the selected cached model.
    // The third argument is pointer-width in the engine even though CommonLib
    // declares it as uint32_t; the AE decompile shows R8 carrying an address.
    constexpr const auto& kUpdateMagic3D = MTB::Offsets::Inventory3DUpdateMagic;

    // The common appender reached by both UpdateMagic3D and async load
    // completion. It attaches the new node visible before the scene pass.
    constexpr const auto& kAppendModel = MTB::Offsets::Inventory3DAppendModel;
    constexpr std::string_view kLocalOwner = "MenuStudio.Settings";

    Policy::ClaimSet g_claims;
    std::atomic<bool> g_suppressed{ false };

    SafetyHookInline g_updateHook{};
    SafetyHookInline g_appendHook{};
    std::atomic<bool> g_updateInstalled{ false };
    std::atomic<bool> g_appendInstalled{ false };

    std::atomic<std::uint32_t> g_updateCalls{ 0 };
    std::atomic<std::uint32_t> g_appendCalls{ 0 };
    std::atomic<std::uint32_t> g_hidden{ 0 };
    std::atomic<std::uint32_t> g_revealed{ 0 };
    std::atomic<std::uint32_t> g_deferred{ 0 };

    // Clear3D's decompile identifies loadedModels[count-1] as the current
    // record. Reveal only that record. Walking backward to an older non-null
    // node could resurrect a stale cached model while an async current model is
    // still pending.
    bool RevealCurrentForLocalInspect(RE::Inventory3DManager* a_manager) {
        if (!a_manager) {
            return false;
        }
        auto& data = a_manager->GetRuntimeData();
        if (data.zoomProgress <= 0.0f || !g_claims.OnlyOwner(kLocalOwner) ||
            data.loadedModels.empty()) {
            return false;
        }

        auto* node = data.loadedModels[data.loadedModels.size() - 1].spModel.get();
        if (!node || !node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
            return false;
        }
        node->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
        const auto total = g_revealed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 12 || total % 100 == 0) {
            spdlog::info("item preview broker: explicit inspect restored the current "
                         "model ({} total).",
                         total);
        }
        return true;
    }

    std::uint32_t HideVisible(RE::Inventory3DManager* a_manager, const char* a_source) {
        if (!a_manager || !g_suppressed.load(std::memory_order_acquire)) {
            return 0;
        }

        auto& data = a_manager->GetRuntimeData();
        if (Policy::ChooseHide(true, data.zoomProgress) != Policy::Hide::kNow) {
            const auto deferred = g_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
            if (deferred == 1) {
                spdlog::info("item preview broker: suppression deferred in {} because "
                             "inspect is already open (zoom {:.3f}). The live inspect is "
                             "left intact.",
                             a_source, data.zoomProgress);
            }
            return 0;
        }

        std::uint32_t hidden = 0;
        for (auto& model : data.loadedModels) {
            auto* node = model.spModel.get();
            if (node && !node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().set(RE::NiAVObject::Flag::kHidden);
                ++hidden;
            }
        }
        if (hidden == 0) {
            return 0;
        }

        const auto total = g_hidden.fetch_add(hidden, std::memory_order_relaxed) + hidden;
        if (total <= 12 || total % 100 == 0) {
            spdlog::info("item preview broker: {} hid {} visible model(s), {} total.",
                         a_source, hidden, total);
        }
        return hidden;
    }

    // Call through first. This preserves all of Skyrim's cache and extra-data
    // bookkeeping, then reverses only the measured visibility write before the
    // caller can render the stage.
    void UpdateMagic3D(RE::Inventory3DManager* a_this, RE::TESForm* a_form,
                       std::uintptr_t a_arg2) {
        g_updateHook.call<void, RE::Inventory3DManager*, RE::TESForm*, std::uintptr_t>(
            a_this, a_form, a_arg2);
        const auto calls = g_updateCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (calls == 1 || calls % 500 == 0) {
            spdlog::info("item preview broker: UpdateMagic3D detour ran {} time(s), "
                         "suppression {}.",
                         calls, g_suppressed.load(std::memory_order_relaxed) ? "on" : "off");
        }
        HideVisible(a_this, "UpdateMagic3D");
    }

    // The node does not exist until the engine appender returns. Hiding here is
    // still before Render's scene pass, including for an async task that began
    // before the first suppression claim arrived.
    void AppendModel(RE::Inventory3DManager* a_this, std::uintptr_t a_arg2,
                     std::uintptr_t a_arg3, void* a_arg4) {
        g_appendHook.call<void, RE::Inventory3DManager*, std::uintptr_t, std::uintptr_t, void*>(
            a_this, a_arg2, a_arg3, a_arg4);
        const auto calls = g_appendCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (calls == 1 || calls % 500 == 0) {
            spdlog::info("item preview broker: model appender detour ran {} time(s), "
                         "suppression {}.",
                         calls, g_suppressed.load(std::memory_order_relaxed) ? "on" : "off");
        }
        HideVisible(a_this, "model appender");
    }

    bool InstallOne(const REL::RelocationID& a_id, const char* a_name,
                    SafetyHookInline& a_hook, void* a_thunk,
                    std::atomic<bool>& a_installed) {
        if (!MTB::VersionCheck::IdOk(a_id)) {
            spdlog::error("item preview broker: {} id {} is absent from this runtime's "
                          "Address Library. Per-frame enforcement remains active.",
                          a_name, a_id.id());
            return false;
        }

        const auto address = a_id.address();
        a_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), a_thunk);
        if (!a_hook) {
            spdlog::error("item preview broker: could not detour {} at 0x{:X}. "
                          "Per-frame enforcement remains active.",
                          a_name, address);
            return false;
        }

        a_installed.store(true, std::memory_order_release);
        spdlog::info("item preview broker: {} detoured at 0x{:X} (id {}).", a_name,
                     address, a_id.id());
        return true;
    }
}

namespace MTB::ItemPreviewBroker {

    void Install() {
        const bool update = InstallOne(kUpdateMagic3D, "UpdateMagic3D", g_updateHook,
                                       reinterpret_cast<void*>(&UpdateMagic3D),
                                       g_updateInstalled);
        const bool append = InstallOne(kAppendModel, "model appender", g_appendHook,
                                       reinterpret_cast<void*>(&AppendModel),
                                       g_appendInstalled);
        spdlog::info("item preview broker: enforcement ready (cached update {}, late "
                     "creation {}, per-frame fallback on).",
                     update ? "hooked" : "reactive",
                     append ? "hooked" : "reactive");
    }

    bool SetClaim(const char* a_ownerId, bool a_active) {
        if (!a_ownerId || *a_ownerId == '\0') {
            return false;
        }

        const bool before = g_claims.Suppressed();
        const auto change = g_claims.Set(a_ownerId, a_active);
        const bool after = g_claims.Suppressed();
        g_suppressed.store(after, std::memory_order_release);

        if (change == Policy::Change::kAcquired) {
            spdlog::info("item preview claim: '{}' acquired ({} live).", a_ownerId,
                         g_claims.Size());
        } else if (change == Policy::Change::kReleased) {
            spdlog::info("item preview claim: '{}' released ({} live).", a_ownerId,
                         g_claims.Size());
        }

        if (!before && after) {
            Reconcile();
        }
        return true;
    }

    void Reconcile() {
        if (!g_suppressed.load(std::memory_order_acquire)) {
            return;
        }
        auto* manager = RE::Inventory3DManager::GetSingleton();
        // Menu Studio's own hide setting yields to the explicit inspect the
        // player has already opened. The engine decompile makes the current
        // record precise, so a cached model hidden at zoom zero is restored
        // without revealing older ring entries. Another live owner is never
        // overridden; it must release its own claim.
        if (RevealCurrentForLocalInspect(manager)) {
            return;
        }
        HideVisible(manager, "frame reconcile");
    }

    void Drain() {
        const auto count = g_claims.Drain();
        g_suppressed.store(false, std::memory_order_release);
        if (count != 0) {
            spdlog::info("item preview claim: drained {} live claim(s) at the load/new-game "
                         "boundary.",
                         count);
        }
    }

    bool Suppressed() {
        return g_suppressed.load(std::memory_order_acquire);
    }

}  // namespace MTB::ItemPreviewBroker

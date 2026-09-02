#pragma once

// Optional coordinator for Skyrim's floating Inventory3DManager item stage.
// Callers publish owner-scoped suppression claims through PublicApi.cpp. The
// suppression is enforced at rest until the last owner releases, while each
// caller keeps its own fallback for installations where Menu Studio is absent.
// Menu Studio's lone local claim yields the current record to explicit inspect.
// A release stops enforcement; Skyrim owns revealing the next normal selection.
namespace MTB::ItemPreviewBroker {

    // Installs entry detours for cached-model updates and late model creation.
    // Failure is degraded rather than fatal because Reconcile still enforces
    // the claim from Menu Studio's frame driver.
    void Install();

    // Main thread only. A valid owner request is accepted idempotently and
    // returns true. False means the owner id was null or empty.
    [[nodiscard]] bool SetClaim(const char* a_ownerId, bool a_active);

    // Main-thread safety net and first-claim enforcement. This never calls
    // Clear3D and never hides while Skyrim is already in inspect mode.
    void Reconcile();

    // Load / new-game backstop. Ordinary closes are released by each owner so
    // one menu's lifecycle cannot erase a claim belonging to another.
    void Drain();

    // Hook-safe aggregate state. No owner identity leaves the main thread.
    [[nodiscard]] bool Suppressed();

}  // namespace MTB::ItemPreviewBroker

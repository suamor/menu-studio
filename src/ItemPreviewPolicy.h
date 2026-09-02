#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

// Ownership policy for Skyrim's floating Inventory3DManager item stage.
//
// No plugin owns that shared engine resource. Callers publish owner-scoped
// suppression claims instead. The stage stays suppressed at rest until the
// last owner releases. Menu Studio's own lone claim yields the current record
// after an explicit inspect has opened; an external owner is never overridden.
// This file is engine-free so the balance rules remain testable without Skyrim.
namespace MTB::ItemPreviewPolicy {

    enum class Change {
        kInvalid,
        kUnchanged,
        kAcquired,
        kReleased,
    };

    class ClaimSet {
    public:
        [[nodiscard]] Change Set(std::string_view a_ownerId, bool a_active) {
            if (a_ownerId.empty()) {
                return Change::kInvalid;
            }

            const auto found = owners_.find(a_ownerId);
            if (a_active) {
                if (found != owners_.end()) {
                    return Change::kUnchanged;
                }
                owners_.emplace(a_ownerId);
                return Change::kAcquired;
            }
            if (found == owners_.end()) {
                return Change::kUnchanged;
            }
            owners_.erase(found);
            return Change::kReleased;
        }

        // Load / new-game backstop. Ordinary close edges remain owner-scoped:
        // Menu Studio cannot infer that its last covered menu is also an
        // external owner's last relevant menu.
        [[nodiscard]] std::size_t Drain() {
            const auto count = owners_.size();
            owners_.clear();
            return count;
        }

        [[nodiscard]] bool        Suppressed() const { return !owners_.empty(); }
        [[nodiscard]] std::size_t Size() const { return owners_.size(); }
        [[nodiscard]] bool OnlyOwner(std::string_view a_ownerId) const {
            return owners_.size() == 1 && owners_.find(a_ownerId) != owners_.end();
        }

    private:
        std::set<std::string, std::less<>> owners_;
    };

    enum class Hide {
        kInactive,
        kNow,
        kDefer,
    };

    // A claim arriving after inspect mode has already opened does not own that
    // interaction and must not blank it. Hooked engine writes are allowed to
    // finish and are post-hidden only while the zoom is at rest.
    [[nodiscard]] constexpr Hide ChooseHide(bool a_suppressed, float a_zoomProgress) {
        if (!a_suppressed) {
            return Hide::kInactive;
        }
        return a_zoomProgress > 0.0f ? Hide::kDefer : Hide::kNow;
    }

    // Menu Studio's reserved owner is just another claim. Keep the predicate
    // pure so disable, close, gate and live-setting transitions are covered by
    // the same tested decision wherever the lifecycle publishes it.
    [[nodiscard]] constexpr bool LocalClaimWanted(bool a_enabled, bool a_menuOpen,
                                                  bool a_studioEntered,
                                                  bool a_hideSetting) {
        return a_enabled && a_menuOpen && a_studioEntered && a_hideSetting;
    }

}  // namespace MTB::ItemPreviewPolicy

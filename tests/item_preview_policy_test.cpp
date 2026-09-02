#include "ItemPreviewPolicy.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

int main() {
    namespace P = MTB::ItemPreviewPolicy;

    {
        P::ClaimSet claims;
        CHECK(!claims.Suppressed());
        CHECK(claims.Size() == 0);

        CHECK(claims.Set("MenuStudio.Settings", true) == P::Change::kAcquired);
        CHECK(claims.Suppressed());
        CHECK(claims.Size() == 1);
        CHECK(claims.OnlyOwner("MenuStudio.Settings"));
        CHECK(!claims.OnlyOwner("FittingRoom.Editor"));

        // Per-frame publication is safe. A repeated assertion is not another
        // reference that would need another release.
        CHECK(claims.Set("MenuStudio.Settings", true) == P::Change::kUnchanged);
        CHECK(claims.Size() == 1);

        CHECK(claims.Set("FittingRoom.Editor", true) == P::Change::kAcquired);
        CHECK(claims.Size() == 2);
        CHECK(!claims.OnlyOwner("MenuStudio.Settings"));

        // One owner cannot release the other, accidentally or deliberately.
        CHECK(claims.Set("ApparelPreview.Hover", false) == P::Change::kUnchanged);
        CHECK(claims.Suppressed());
        CHECK(claims.Size() == 2);

        CHECK(claims.Set("MenuStudio.Settings", false) == P::Change::kReleased);
        CHECK(claims.Suppressed());
        CHECK(claims.Size() == 1);
        CHECK(claims.OnlyOwner("FittingRoom.Editor"));

        CHECK(claims.Set("FittingRoom.Editor", false) == P::Change::kReleased);
        CHECK(!claims.Suppressed());
        CHECK(claims.Size() == 0);
    }

    {
        P::ClaimSet claims;
        CHECK(claims.Set("", true) == P::Change::kInvalid);
        CHECK(!claims.Suppressed());
        (void)claims.Set("FittingRoom.Editor", true);
        (void)claims.Set("ApparelPreview.Hover", true);
        CHECK(claims.Drain() == 2);
        CHECK(!claims.Suppressed());
        CHECK(claims.Drain() == 0);
    }

    // Acquiring suppression while the player is already in Skyrim's inspect
    // view must not blank that view. Future updates finish normally, but their
    // ordinary at-rest visibility is hidden until the zoom returns to rest.
    CHECK(P::ChooseHide(false, 0.0f) == P::Hide::kInactive);
    CHECK(P::ChooseHide(true, 0.0f) == P::Hide::kNow);
    CHECK(P::ChooseHide(true, -0.01f) == P::Hide::kNow);
    CHECK(P::ChooseHide(true, 0.01f) == P::Hide::kDefer);
    CHECK(P::ChooseHide(true, 1.0f) == P::Hide::kDefer);

    CHECK(P::LocalClaimWanted(true, true, true, true));
    CHECK(!P::LocalClaimWanted(false, true, true, true));
    CHECK(!P::LocalClaimWanted(true, false, true, true));
    CHECK(!P::LocalClaimWanted(true, true, false, true));
    CHECK(!P::LocalClaimWanted(true, true, true, false));

    if (g_failures == 0) {
        std::printf("item preview policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

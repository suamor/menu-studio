#include "DeclutterRefPolicy.h"

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
    using MTB::DeclutterRefPolicy::ShouldInspect3D;

    CHECK(ShouldInspect3D({}));
    CHECK(!ShouldInspect3D({ .isPlayer = true }));
    CHECK(!ShouldInspect3D({ .isPlayerMount = true }));
    CHECK(!ShouldInspect3D({ .isOccupiedFurniture = true }));
    CHECK(!ShouldInspect3D({ .isDisabled = true }));
    CHECK(!ShouldInspect3D({ .isFramedCompanion = true }));

    // The cast members are exempt on their own account, not by luck of being
    // near something else that is exempt. Each one alone has to refuse.
    CHECK(!ShouldInspect3D({ .isPlayer = true, .isFramedCompanion = true }));
    CHECK(!ShouldInspect3D({ .isPlayerMount = true, .isFramedCompanion = true }));

    // A disabled ref is still skipped even if something upstream mistakenly
    // called it the companion. The lifetime guard is not negotiable: a
    // disabled temporary can be torn down while effect-heavy armor rebuilds
    // the player's 3D, which is the crash this predicate exists to prevent.
    CHECK(!ShouldInspect3D({ .isDisabled = true, .isFramedCompanion = true }));

    if (g_failures == 0) {
        std::printf("declutter ref policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

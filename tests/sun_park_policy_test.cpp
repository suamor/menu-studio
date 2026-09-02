#include "SunParkPolicy.h"

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
    using namespace MTB::SunParkPolicy;

    // The one shape that parks: the setting is on, the world is hidden behind
    // the void, and we are outdoors where the sun is what lights the character.
    CHECK(Decide({ .enabled = true, .worldHidden = true, .exterior = true }) ==
          Action::kPark);

    // ⚠ INTERIORS RELEASE. The cell's own lights are Declutter's job and the
    // cell's look is StudioLight's, and both were reported working. Parking the
    // sun indoors would be a third painter of the same appearance.
    CHECK(Decide({ .enabled = true, .worldHidden = true, .exterior = false }) ==
          Action::kRelease);

    // ⚠⚠ A VISIBLE WORLD RELEASES, and this is the term that matters most.
    // Off and Scene view render the world, so taking the sun out of them would
    // black out everything on screen rather than everything hidden. Note this
    // is true even with the setting on, which is what makes it a structural
    // guard and not a preference.
    CHECK(Decide({ .enabled = true, .worldHidden = false, .exterior = true }) ==
          Action::kRelease);
    CHECK(Decide({ .enabled = true, .worldHidden = false, .exterior = false }) ==
          Action::kRelease);

    // The opt-out wins everywhere. A player who turned it off asked to keep
    // their own daylight, and no combination of the other two overrides that.
    CHECK(Decide({ .enabled = false, .worldHidden = true, .exterior = true }) ==
          Action::kRelease);
    CHECK(Decide({ .enabled = false, .worldHidden = true, .exterior = false }) ==
          Action::kRelease);
    CHECK(Decide({ .enabled = false, .worldHidden = false, .exterior = true }) ==
          Action::kRelease);
    CHECK(Decide({ .enabled = false, .worldHidden = false, .exterior = false }) ==
          Action::kRelease);

    if (g_failures == 0) {
        std::printf("all SunParkPolicy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

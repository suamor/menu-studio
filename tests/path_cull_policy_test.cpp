#include "PathCullPolicy.h"

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
    using namespace MTB::PathCullPolicy;

    // ⚠⚠ THE ONE WAY TO GET THIS BADLY WRONG. A cast root is also in the
    // keep-set (the ancestor walk starts at it), and the naive version lets
    // the ancestor answer win, descends into the player, and culls his own
    // arms and armour. The root must be a TERMINAL keep.
    CHECK(Decide({ .isCastRoot = true, .isCastAncestor = true }) ==
          Action::kKeepWhole);
    CHECK(Decide({ .isCastRoot = true }) == Action::kKeepWhole);
    // A cast root someone already hid (our own player-hide culls his root
    // while a companion is framed) is still terminal: never descended into,
    // never recorded, and never un-culled by this walk.
    CHECK(Decide({ .isCastRoot = true, .alreadyCulled = true }) ==
          Action::kKeepWhole);

    // On the keep path: spare the node itself, keep deciding its children.
    // This is the portal-geometry case - the room node and the shared portal
    // levels are ancestors, and their OTHER children are what must go.
    CHECK(Decide({ .isCastAncestor = true }) == Action::kDescend);
    // An ancestor already culled still descends - un-hiding is not this
    // walk's job, and the decision for its children is unchanged.
    CHECK(Decide({ .isCastAncestor = true, .alreadyCulled = true }) ==
          Action::kDescend);

    // Our own studio rig: a light whose OWN node is culled is skipped by the
    // light gathering, so MTB_ nodes are spared wherever they hang.
    CHECK(Decide({ .isOurs = true }) == Action::kLeave);
    CHECK(Decide({ .isOurs = true, .alreadyCulled = true }) == Action::kLeave);

    // Already culled before we arrived: leave it and do NOT record it, so the
    // restore cannot un-hide what another mod hid.
    CHECK(Decide({ .alreadyCulled = true }) == Action::kLeave);

    // Everything else is off the keep path: cull it, record it, stop. This is
    // the whole point of the inversion - the cell's big object list is culled
    // from ABOVE even when the player's chain never passes through it.
    CHECK(Decide({}) == Action::kCull);

    if (g_failures == 0) {
        std::printf("all PathCullPolicy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

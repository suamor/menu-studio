#include "CameraArmStatePolicy.h"

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
    namespace P = MTB::CameraArmStatePolicy;
    using P::ChooseArm;

    // The two the old allowlist knew about, unchanged.
    {
        const auto first = ChooseArm(P::kFirstPerson);
        CHECK(first.frame);
        CHECK(first.forceThird);
        CHECK(!first.mountOffsets);

        const auto mount = ChooseArm(P::kMount);
        CHECK(mount.frame);
        CHECK(mount.forceThird);
        CHECK(mount.mountOffsets);  // the rider raise belongs to this one alone
    }

    // Already third person: frame in place, switch nothing, owe nothing back.
    {
        const auto third = ChooseArm(P::kThirdPerson);
        CHECK(third.frame);
        CHECK(!third.forceThird);
        CHECK(!third.mountOffsets);
    }

    // ── The regression ─────────────────────────────────────────────────────
    // Field 2026-08-11: "if an animation calls for a camera change it can get
    // stuck and fail to trigger the close-up", seen through the barter menu.
    // Every one of these used to fall through unswitched, apply the framing to
    // a camera nobody was looking through, and show the character from behind.
    {
        constexpr int kStuck[] = { P::kAnimated,    P::kFurniture, P::kPCTransition,
                                   P::kAutoVanity,  P::kTween,     P::kIronSights,
                                   P::kBleedout,    P::kDragon,    P::kVATS };
        for (const int stuck : kStuck) {
            const auto plan = ChooseArm(stuck);
            CHECK(plan.frame);
            CHECK(plan.forceThird);
            // Only kMount carries the rider offsets. A dragon is a mount in
            // English and not in this table: the raise was fitted to a horse.
            CHECK(!plan.mountOffsets);
        }
    }

    // The deliberate exception: somebody else is driving on purpose.
    {
        const auto free = ChooseArm(P::kFree);
        CHECK(!free.frame);
        CHECK(!free.forceThird);
        CHECK(!free.mountOffsets);
    }

    // ⚠ EVERY STATE THE ENGINE HAS IS DECIDED, which is the property the old
    // allowlist failed. A state that reaches the arm with no plan is how the
    // silent invisible framing happened in the first place, so an unlisted one
    // must never read as "do nothing" by accident.
    {
        int framed = 0;
        int stoodAside = 0;
        for (int id = 0; id < P::kTotal; ++id) {
            const auto plan = ChooseArm(id);
            if (plan.frame) {
                ++framed;
                // Framing is only ever visible from third person: either we are
                // already there or we are switching to it.
                CHECK((id == P::kThirdPerson) != plan.forceThird);
            } else {
                ++stoodAside;
                CHECK(!plan.forceThird);  // standing aside means touching nothing
            }
        }
        CHECK(framed == P::kTotal - 1);
        CHECK(stoodAside == 1);  // kFree, and only kFree
    }

    // Out-of-range ids (a future engine state, a garbage read) take the framing
    // branch rather than silently doing nothing - the same conservative default
    // the table gives every unknown driver.
    {
        const auto future = ChooseArm(P::kTotal);
        CHECK(future.frame);
        CHECK(future.forceThird);
    }

    // ── What the close hands back ─────────────────────
    // Field 2026-09-02, a 1.1.5 reporter's diag3 log: every menu she opens comes
    // through Tab, so the arm finds kTween, and the close did SetState(kTween)
    // back into a state whose menu was already gone. +8.01s STILL WRONG. The
    // state the arm FOUND is not always the state to RETURN to.
    {
        using P::ChooseReturnState;
        // The hotkey route, the one every field run before 2026-09-02 took:
        // whatever the arm found is what goes back. Unchanged.
        CHECK(ChooseReturnState({ .liveState = P::kFirstPerson }) == P::kFirstPerson);
        CHECK(ChooseReturnState({ .liveState = P::kMount }) == P::kMount);
        CHECK(ChooseReturnState({ .liveState = P::kThirdPerson,
                                  .readingValid = true,
                                  .readingState = P::kFirstPerson }) == P::kThirdPerson);

        // The Tab route with the pre-tween reading in hand: return to what the
        // reading saw, which is the player's own camera from before the tween.
        CHECK(ChooseReturnState({ .liveState = P::kTween,
                                  .readingValid = true,
                                  .readingState = P::kThirdPerson }) == P::kThirdPerson);
        CHECK(ChooseReturnState({ .liveState = P::kTween,
                                  .readingValid = true,
                                  .readingState = P::kFirstPerson }) == P::kFirstPerson);

        // The Tab route with no usable reading (the plugin armed mid-tween, or
        // the tween's open edge read too late and saw kTween itself): third
        // person, the state the restored values describe. Never a guess at
        // first person, and never kTween.
        CHECK(ChooseReturnState({ .liveState = P::kTween }) == P::kThirdPerson);
        CHECK(ChooseReturnState({ .liveState = P::kTween,
                                  .readingValid = true,
                                  .readingState = P::kTween }) == P::kThirdPerson);
        CHECK(ChooseReturnState({ .liveState = P::kTween,
                                  .readingValid = false,
                                  .readingState = P::kFirstPerson }) == P::kThirdPerson);

        // ⚠ THE PROPERTY, over every state and every reading: the close never
        // returns to kTween. That one value is the whole 2026-09-02 report.
        for (int live = 0; live < P::kTotal; ++live) {
            for (int seen = 0; seen < P::kTotal; ++seen) {
                for (int valid = 0; valid < 2; ++valid) {
                    CHECK(ChooseReturnState({ .liveState = live,
                                              .readingValid = valid != 0,
                                              .readingState = seen }) != P::kTween);
                }
            }
        }
        static_assert(P::ChooseReturnState({ .liveState = P::kTween,
                                             .readingValid = true,
                                             .readingState = P::kThirdPerson }) ==
                      P::kThirdPerson);
    }

    if (g_failures == 0) {
        std::printf("camera arm state policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

#include "CameraCollisionPolicy.h"

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
    using namespace MTB::CameraCollisionPolicy;

    // Only an enabled, active Menu Studio bubble may bypass the obstruction
    // result. Ordinary gameplay and disabled configurations remain vanilla.
    CHECK(Decide({ .enabled = true, .bubbleActive = true, .armWindow = false }) ==
          Action::kBypass);
    CHECK(Decide({ .enabled = true, .bubbleActive = false, .armWindow = false }) ==
          Action::kRunOriginal);
    CHECK(Decide({ .enabled = false, .bubbleActive = true, .armWindow = false }) ==
          Action::kRunOriginal);
    CHECK(Decide({ .enabled = false, .bubbleActive = false, .armWindow = false }) ==
          Action::kRunOriginal);

    // ⚠ THE ARM EDGE BYPASSES WITHOUT THE PAUSE, which is the whole point of
    // the term: the arm block rebuilds the camera before the freeze the engine
    // publishes a frame later, and a clamped rebuild is what the camera is
    // stuck with for the rest of a paused menu.
    CHECK(Decide({ .enabled = true, .bubbleActive = false, .armWindow = true }) ==
          Action::kBypass);
    CHECK(Decide({ .enabled = true, .bubbleActive = true, .armWindow = true }) ==
          Action::kBypass);

    // ⚠ AND IT IS NOT AN OVERRIDE OF THE SETTING. Turning the bypass off asks
    // for vanilla walls everywhere, arm edge included.
    CHECK(Decide({ .enabled = false, .bubbleActive = false, .armWindow = true }) ==
          Action::kRunOriginal);
    CHECK(Decide({ .enabled = false, .bubbleActive = true, .armWindow = true }) ==
          Action::kRunOriginal);

    if (g_failures == 0) {
        std::printf("all CameraCollisionPolicy tests passed\n");
    }
    return g_failures ? 1 : 0;
}

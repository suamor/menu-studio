#include "FramePolicy.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

namespace F = MTB::FramePolicy;

int main() {
    // ---- the setting is the whole answer now -----------------------------
    //
    // ⚠ AUTO WAS DELETED ON 2026-08-28 and this file is where that is pinned.
    // It followed the live FLICK preset, which meant a second input and a file
    // read; the field asked for carved and plain and nothing else.
    CHECK(F::Resolve(F::Style::kCarved) == F::Shape::kCarved);
    CHECK(F::Resolve(F::Style::kPlain) == F::Shape::kPlain);

    // ---- the wire format, and the value that no longer has a name ---------
    //
    // ⚠⚠ 0 IS THE ONE THAT MATTERS. Auto shipped as the default for a day and
    // sits in every INI written in that window, so it has to keep resolving to
    // something a player can live with rather than to a refusal. Carved is
    // where auto landed under Vel'dun and is the default everywhere now, so 0
    // reads as carved.
    //
    // ⚠ FITTING ROOM'S COPY OF THIS TEST CHECKS THE SAME THING. The two
    // plugins draw in one menu and a stored iFrameStyle that resolved
    // differently between them would show up as two corners on one screen.
    CHECK(F::StyleFromIni(0) == F::Style::kCarved);
    CHECK(F::StyleFromIni(1) == F::Style::kCarved);
    CHECK(F::StyleFromIni(2) == F::Style::kPlain);
    // ⚠ AN UNKNOWN VALUE IS CARVED, NOT A REFUSAL. A forward-dated INI written
    // by a later build has to keep working.
    CHECK(F::StyleFromIni(99) == F::Style::kCarved);
    CHECK(F::StyleFromIni(-1) == F::Style::kCarved);

    // The two named values round-trip; 0 deliberately does not, because it maps
    // onto 1 rather than staying a value of its own.
    for (int i = 1; i <= 2; ++i) {
        CHECK(F::IniFromStyle(F::StyleFromIni(i)) == i);
    }
    CHECK(F::IniFromStyle(F::StyleFromIni(0)) == 1);

    // ⚠ AND A PLAYER'S PLAIN SURVIVES THE WHOLE REMOVAL. 2 was never a default,
    // so a 2 in a file is somebody who went and found the setting.
    CHECK(F::Resolve(F::StyleFromIni(2)) == F::Shape::kPlain);
    CHECK(F::Resolve(F::StyleFromIni(0)) == F::Shape::kCarved);
    CHECK(F::Resolve(F::StyleFromIni(1)) == F::Shape::kCarved);

    // ---- the plain radius belongs to the SURFACE, and the theme may only ask
    //      for more ---------------------------------------------------------
    //
    // ⚠⚠ THE TILE'S NUMBER IS A FRACTION OF ITS OWN WIDTH, NOT A PIXEL COUNT,
    // and that is what it was before any chamfer work (5faf221,
    // `(x1 - x0) * kRoundingFraction`). A flat radius held the slot for one day
    // and it is the wrong KIND of answer: square on a large tile, swallowing a
    // small one. [General] fPlainRounding is gone for that reason.
    //
    // The live case: a 46px tile at resolution scale 1.333 snaps to about 61,
    // and 0.28 of that is the radius the strip draws. The theme's 1.000 must
    // not win it, which is what "we lost the rounded buttons" was about.
    CHECK(F::PlainRadius(1.0f, 61.0f * 0.28f) == 61.0f * 0.28f);
    CHECK(F::PlainRadius(0.0f, 61.0f * 0.28f) == 61.0f * 0.28f);
    // A theme that genuinely rounds more than the surface asked for is followed,
    // so the surface's number is a floor and never a cap.
    CHECK(F::PlainRadius(30.0f, 61.0f * 0.28f) == 30.0f);
    // Zero means "follow the theme exactly", for a surface with no number.
    CHECK(F::PlainRadius(0.0f, 0.0f) == 0.0f);
    CHECK(F::PlainRadius(6.0f, 0.0f) == 6.0f);
    // A garbage read must not be able to pick the radius.
    CHECK(F::PlainRadius(std::nanf(""), 8.0f) == 8.0f);
    CHECK(F::PlainRadius(-3.0f, 8.0f) == 8.0f);
    CHECK(F::PlainRadius(std::nanf(""), 0.0f) == 0.0f);

    // ---- the radius clamp predicts what a draw call will do --------------
    CHECK(F::ClampRadius(8.0f, 100.0f, 100.0f) == 8.0f);
    CHECK(F::ClampRadius(80.0f, 100.0f, 40.0f) == 20.0f);  // half the shorter side
    CHECK(F::ClampRadius(8.0f, 0.0f, 40.0f) == 0.0f);
    CHECK(F::ClampRadius(-1.0f, 100.0f, 100.0f) == 0.0f);
    CHECK(F::ClampRadius(0.0f, 100.0f, 100.0f) == 0.0f);

    if (g_failures == 0) {
        std::printf("frame_policy_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

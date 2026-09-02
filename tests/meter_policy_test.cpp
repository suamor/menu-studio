#include "MeterPolicy.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

namespace M = MTB::MeterPolicy;
namespace C = MTB::ChamferPolicy;

static bool Near(float a_lhs, float a_rhs, float a_eps = 1e-3f) {
    const float d = a_lhs - a_rhs;
    return (d < 0.0f ? -d : d) <= a_eps;
}

// The tile the strip actually draws: 46 unscaled at 1x, cut to the art corner.
constexpr float kMinX = 100.0f;
constexpr float kMinY = 200.0f;
constexpr float kMaxX = 146.0f;
constexpr float kMaxY = 246.0f;
constexpr float kCut  = 11.5f;  // a quarter of the shorter side, ArtCorner's clamp

int main() {
    // ---- nothing published, nothing drawn -------------------------------
    //
    // The caller gates on the negative itself, but a zero reaching here must
    // still not draw a fill: an empty stone is the trough alone.
    CHECK(!M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.0f, kCut).draw);
    CHECK(!M::Build(kMinX, kMinY, kMaxX, kMaxY, -1.0f, kCut).draw);

    // A degenerate tile draws nothing rather than something backwards.
    CHECK(!M::Build(kMinX, kMinY, kMinX, kMaxY, 1.0f, kCut).draw);
    CHECK(!M::Build(kMinX, kMinY, kMaxX, kMinY, 1.0f, kCut).draw);

    // ---- a full stone is the tile, corners and all ----------------------
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 1.0f, kCut);
        CHECK(f.draw);
        CHECK(Near(f.minX, kMinX));
        CHECK(Near(f.maxX, kMaxX));
        CHECK(Near(f.minY, kMinY));
        CHECK(Near(f.maxY, kMaxY));
        CHECK(Near(f.corner, kCut));
        // ⚠ THE ONE THE PLAYER SEES EVERY RECHARGE. Square shoulders inside a
        // cut silhouette is the failure this asserts against.
        CHECK(f.corners == C::kAll);
    }

    // ---- the top is cut from the moment the fill REACHES the corners -----
    //
    // ⚠ THIS ASSERTED THE BUG UNTIL 2026-08-29. It said "anything short of full
    // leaves the top square", which is only true below the corner band. The
    // tile is 46 tall with an 11.5 cut, so the top 11.5 pixels are already cut
    // away: a fill whose top edge lands anywhere in there and stays square
    // draws violet outside the silhouette. It was reported as the carved style
    // breaking "near the top", not at full, which is exactly this band.
    {
        // 0.99 puts the top edge 0.46 below the tile's top, well inside the cut.
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.99f, kCut);
        CHECK(f.draw);
        CHECK(f.corners == C::kAll);
        CHECK(f.minY > kMinY);
    }
    {
        // 0.75 lands the top edge exactly on the bottom of the band, where the
        // tile's edge has gone vertical again and square is correct.
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.75f, kCut);
        CHECK(f.draw);
        CHECK(Near(f.minY, kMinY + kCut));
        CHECK(f.corners == C::kBottom);
    }
    {
        // Just inside the band, so the pair comes back.
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.80f, kCut);
        CHECK(f.draw);
        CHECK(f.minY < kMinY + kCut);
        CHECK(f.corners == C::kAll);
    }

    // ---- tall enough to carry the tile's own corner ---------------------
    //
    // At half a 46 tall tile the fill is 23 high, comfortably past 2 * 11.5,
    // so it takes the tile's corner unchanged and needs no inset at all.
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.5f, kCut);
        CHECK(f.draw);
        CHECK(Near(f.corner, kCut));
        CHECK(Near(f.minX, kMinX));
        CHECK(Near(f.maxX, kMaxX));
        CHECK(Near(f.minY, kMaxY - 23.0f));
    }

    // ---- shorter than its own corner, which is the bug this file exists
    //      to prevent ---------------------------------------------------
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.2f, kCut);
        const float height = (kMaxY - kMinY) * 0.2f;  // 9.2, under 2 * 11.5
        CHECK(f.draw);
        CHECK(Near(f.corner, height * 0.5f));
        // Inset by exactly what the corner lost, both sides.
        CHECK(Near(f.minX, kMinX + (kCut - height * 0.5f)));
        CHECK(Near(f.maxX, kMaxX - (kCut - height * 0.5f)));
    }

    // ---- and the reason for that inset, stated as containment -----------
    //
    // ⚠ THE ASSERTION THAT MATTERS. Walk the fill's own left edge from its
    // bottom corner upward and check every sample is inside the TILE's cut. A
    // fill that fails this shows violet outside the button's silhouette, over
    // whatever the strip happens to be floating on, and no amount of colour
    // choice hides it.
    for (int step = 1; step <= 100; ++step) {
        const float frac = static_cast<float>(step) / 100.0f;
        const auto  f    = M::Build(kMinX, kMinY, kMaxX, kMaxY, frac, kCut);
        if (!f.draw) {
            continue;
        }
        const float height = f.maxY - f.minY;
        for (int s = 0; s <= 20; ++s) {
            const float d = (f.corner * static_cast<float>(s)) / 20.0f;
            if (d > height) {
                break;
            }
            // The fill's left edge at this height, by the same bevel rule the
            // tile is drawn with.
            const float x = (d < f.corner) ? (f.minX + f.corner - d) : f.minX;
            CHECK(M::InsideLeftCut(kMinX, x, d, kCut));
        }
    }

    // ---- the art path will not take a corner under a pixel --------------
    //
    // Asking for one anyway is how the fill and the tile end up cut to
    // different depths, so the floor is applied here where the inset can see
    // it rather than inside the draw call where it cannot.
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.03f, kCut, 1.0f);
        CHECK(f.draw);          // 1.38 tall, over the one pixel minimum
        CHECK(f.corner >= 1.0f);
        CHECK(Near(f.minX, kMinX + (kCut - f.corner)));
    }
    // The quad path has no floor, so the same fraction gives it the honest
    // half-height corner instead.
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.03f, kCut, 0.0f);
        CHECK(f.draw);
        CHECK(Near(f.corner, (kMaxY - kMinY) * 0.03f * 0.5f));
    }

    // ---- thinner than a pixel is not a level, it is a smear -------------
    {
        const auto f = M::Build(kMinX, kMinY, kMaxX, kMaxY, 0.01f, kCut);
        CHECK(!f.draw);  // 0.46 tall
    }

    // ---- a tile too narrow to gauge refuses rather than inverts ---------
    {
        const auto f = M::Build(kMinX, kMinY, kMinX + 6.0f, kMaxY, 0.1f, kCut);
        CHECK(!f.draw || f.maxX > f.minX);
    }

    if (g_failures == 0) {
        std::printf("meter_policy_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

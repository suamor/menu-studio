#pragma once

#include "ChamferPolicy.h"

#include <algorithm>

// Where the charge meter's filled part goes when the TILE ITSELF is the gauge.
//
// WHY THIS IS NOT THREE LINES IN THE CALLER. The tile has cut corners and the
// FLICK ABI has no clip rect, so a partial fill cannot be masked to that shape.
// The way round it is to draw the filled part as its own cut shape - but a
// short rect cannot carry the tile's corner, and both fill paths silently clamp
// the corner they are handed to half the shorter side of the rect they are
// handed. A SHALLOWER cut on the same bottom corners is a fill that stands
// OUTSIDE the tile's own silhouette, which shows as violet past the corner over
// whatever the strip is floating on.
//
// The correction is one subtraction and it is exact rather than close, on both
// of the shapes this plugin draws.
//
// On the 45 degree bevel, at a height d above the bottom, the tile's left edge
// sits at min.x + cut - d and a fill cut to c sits at min.x + c - d. Inset the
// fill's sides by cut - c and the two meet exactly at d = 0, and above that the
// tile's edge is further left than ours, so the fill stays inside.
//
// ⚠ THE ART CORNER IS A QUARTER DISC AND IT IS NOT INSIDE THAT BEVEL, IT IS
// OUTSIDE IT (measured off frame_fill.png: a quarter circle of radius 12 in the
// 20 texel slice, so a rounded corner rather than a cut one). It needs its own
// line and it gets the same answer. The tile's edge is min.x + k - sqrt(2kd -
// d^2) and the fill's, inset by k - c, is min.x + k - c + c - sqrt(2cd - d^2).
// The two differ only by sqrt(2kd - d^2) against sqrt(2cd - d^2), and c <= k by
// construction, so the tile's edge is never right of ours. Past d = c the fill
// has gone vertical at min.x + k - c while the tile is still curving, and
// 2kd - d^2 only grows up to d = k, so that half holds too.
//
// ⚠ FINAL PIXELS THROUGHOUT. ChamferPanel::Fill takes an UNSCALED cut and
// scales it itself, so a caller on that path unscales what comes back out of
// here. The art path is already in final pixels. Mixing the two conventions is
// what this header exists to keep out of the drawing code.
namespace MTB::MeterPolicy {

    namespace Chamfer = MTB::ChamferPolicy;

    struct Fill {
        // Nothing to draw: no meter, an empty stone, or a fraction so small the
        // fill would be thinner than a pixel. The trough is the caller's, and
        // it is drawn whether this says yes or no, because an empty stone still
        // has to read as empty.
        bool     draw    = false;
        float    minX    = 0.0f;
        float    minY    = 0.0f;
        float    maxX    = 0.0f;
        float    maxY    = 0.0f;
        float    corner  = 0.0f;
        unsigned corners = Chamfer::kBottom;
    };

    // a_tileCorner is the cut the TILE was drawn with, in final pixels.
    // a_cornerFloor is the smallest corner the caller's fill path will accept:
    // the art path floors at one pixel, the quad path does not floor at all.
    [[nodiscard]] inline Fill Build(float a_minX, float a_minY, float a_maxX, float a_maxY,
                                    float a_fraction, float a_tileCorner,
                                    float a_cornerFloor = 0.0f) {
        Fill out;
        const float width = a_maxX - a_minX;
        const float tall  = a_maxY - a_minY;
        if (width <= 0.0f || tall <= 0.0f) {
            return out;
        }
        const float frac = std::clamp(a_fraction, 0.0f, 1.0f);
        // ⚠ A PIXEL IS THE FLOOR, and it is not tidiness. Below that the fill
        // is a sub-pixel line whose own corner clamp has collapsed to nothing,
        // so it draws a smear at the bottom of the tile rather than a level.
        const float height = tall * frac;
        if (frac <= 0.0f || height < 1.0f) {
            return out;
        }
        // The tile's own corner cannot exceed what the tile could carry either.
        const float tileCorner =
            (std::max)(0.0f, (std::min)(a_tileCorner, (std::min)(width, tall) * 0.5f));
        // Both fill paths clamp to half the shorter side of the rect they are
        // given. Predicting that here rather than discovering it on screen is
        // the whole point of this file.
        float corner = (std::min)({ tileCorner, width * 0.5f, height * 0.5f });
        corner       = (std::max)(corner, (std::min)(a_cornerFloor, tileCorner));
        const float side = (std::max)(0.0f, tileCorner - corner);

        out.minX = a_minX + side;
        out.maxX = a_maxX - side;
        out.minY = a_maxY - height;
        out.maxY = a_maxY;
        if (out.maxX <= out.minX) {
            return out;  // inset past itself: a tile too narrow to gauge
        }
        out.corner = corner;
        // ⚠⚠ THE TOP PAIR IS CUT FROM THE MOMENT THE FILL REACHES THEM, NOT AT
        // FULL. This said `frac >= 1.0f` and that was wrong by the whole height
        // of the corner. The tile's top corners are cut over a band tileCorner
        // deep, so a fill whose top edge lands anywhere INSIDE that band draws
        // square shoulders where the tile has already cut away, and the violet
        // sits outside the silhouette. Full was simply the last and worst case
        // of a fault that starts as soon as the level enters the band, which is
        // why it read as "near the top" rather than "when full" in the field
        // (user screenshot, 2026-08-29, carved corners).
        //
        // ⚠ THE SAME `corner` ON BOTH PAIRS IS PROVABLY INSIDE, and that is
        // what makes one value enough. Let v = tileCorner - u0 be the cut still
        // standing at the fill's top edge, u0 being how far below the tile's
        // top that edge sits. The sides are already inset by side = tileCorner
        // - corner. At depth d below the fill's top the fill's left edge is
        // minX + side + max(0, corner - d) and the tile's is minX + max(0, v -
        // d). For d < corner the difference is tileCorner - d against v - d,
        // and v <= tileCorner. For d >= corner it is side against v - d, and
        // v - d <= v - corner <= tileCorner - corner. Neither can put the fill
        // outside, so the top may over-cut slightly and can never bleed.
        const bool topInCut = out.minY < a_minY + tileCorner;
        out.corners         = topInCut ? Chamfer::kAll : Chamfer::kBottom;
        out.draw    = true;
        return out;
    }

    // Is a_x at height a_d above the bottom inside a corner cut of a_cut?
    // Only the tests use this, and it lives here so "inside the tile" has one
    // definition rather than one per caller who asks.
    [[nodiscard]] inline bool InsideLeftCut(float a_minX, float a_x, float a_d, float a_cut) {
        const float edge = (a_d < a_cut) ? (a_minX + a_cut - a_d) : a_minX;
        return a_x >= edge - 1e-4f;
    }

}  // namespace MTB::MeterPolicy

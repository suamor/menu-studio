#include "PCH.h"

#include "ChamferPanel.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callbacks

#include "FUCK_API.h"

#include <algorithm>
#include <cmath>

namespace MTB::ChamferPanel {

    namespace {

        // ⚠ THE QUAD PRIMITIVES ARE API VERSION 3, AND BELOW THAT THEY NO-OP.
        // FUCK_API.h guards DrawQuad and DrawQuadFilled with `i->version >= 3`,
        // so against an older FLICK every call here would return silently and
        // the panel would render as nothing at all: no frame, no ground, just
        // contents floating over the game world. An invisible window is a much
        // worse failure than a square one, so ask first and fall back to the
        // rectangle primitives, which have been in the ABI since version 1.
        //
        // This is not hypothetical bookkeeping. Menu Studio is installable
        // beside whatever FLICK the player already has, and the version that
        // ships the quads is not the only one in the wild.
        [[nodiscard]] bool HasQuads() {
            const auto* i = FUCK::GetInterface();
            return i && i->version >= 3 && i->DrawQuadFilled && i->DrawQuad;
        }

        [[nodiscard]] ImVec2 Vec(const Policy::Point& a_p) {
            return ImVec2{ a_p.x, a_p.y };
        }

        // Whole-pixel bounds. A chamfer is mostly straight edges, and a straight
        // edge on a fractional coordinate spreads across two rows at partial
        // coverage and reads as a soft, unevenly weighted frame. Rounded inward
        // so the panel can only ever shrink into its rect, never grow out of it
        // and meet a clip edge - the lesson the ActionBar tile paid for twice.
        [[nodiscard]] Policy::Rect Snap(const ImVec2& a_min, const ImVec2& a_max,
                                        float a_cut, unsigned a_corners) {
            return Policy::Rect{
                Policy::Point{ std::ceil(a_min.x), std::ceil(a_min.y) },
                Policy::Point{ std::floor(a_max.x), std::floor(a_max.y) },
                a_cut,
                a_corners,
            };
        }

        [[nodiscard]] bool Degenerate(const Policy::Rect& a_r) {
            return Policy::Width(a_r) <= 0.0f || Policy::Height(a_r) <= 0.0f;
        }

    }  // namespace

    Style DefaultStyle() {
        Style s;
        s.fill = FUCK::GetStyleColorVec4(ImGuiCol_WindowBg);
        s.edge = FUCK::GetStyleColorVec4(ImGuiCol_Border);
        return s;
    }

    void Fill(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col,
              float a_cut, unsigned a_corners) {
        if (a_col.w <= 0.0f) {
            return;
        }
        const Policy::Rect rect = Snap(a_min, a_max, FUCK::Scale(a_cut), a_corners);
        if (Degenerate(rect)) {
            return;
        }
        // A square panel costs exactly what a plain rectangle costs, and the
        // fallback path is the same call - so an old FLICK degrades to the look
        // this plugin had before the chamfer existed rather than to nothing.
        if (!HasQuads() || Policy::ClampCut(rect) <= 0.0f) {
            FUCK::DrawRectFilled(Vec(rect.min), Vec(rect.max), a_col, 0.0f);
            return;
        }
        const Policy::Fill fill = Policy::BuildFill(rect);
        for (int i = 0; i < fill.count; ++i) {
            const Policy::Quad& q = fill.quads[i];
            FUCK::DrawQuadFilled(Vec(q.v[0]), Vec(q.v[1]), Vec(q.v[2]), Vec(q.v[3]), a_col);
        }
    }

    // ⚠ STACKED ONE-PIXEL STROKES, NOT ONE THICK ONE. A thick polyline is mitred
    // and anti-aliased along its joints, so its apparent width differs between
    // the straights and the 45 degree cuts by construction - and a chamfer is
    // nothing but joints. Each single-pixel pass, centred half a pixel inside an
    // integer edge, lands on exactly one row and needs no coverage blending, so
    // N of them stack into a band that is N pixels everywhere. Same reasoning
    // and the same fix as the ActionBar tile edge.
    //
    // Policy::Inset is what makes the stack correct rather than merely repeated:
    // it walks the cut in as well as the sides, so pass N stays parallel to pass
    // 0 instead of splaying at the corners. See kInsetCutScale for the geometry.
    void Stroke(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col,
                float a_cut, float a_thickness, unsigned a_corners) {
        if (a_col.w <= 0.0f || a_thickness <= 0.0f) {
            return;
        }
        const Policy::Rect base = Snap(a_min, a_max, FUCK::Scale(a_cut), a_corners);
        if (Degenerate(base)) {
            return;
        }
        const int passes =
            (std::max)(1, static_cast<int>(std::floor(FUCK::Scale(a_thickness))));

        // ⚠ THE FALLBACK HAS TO MATCH Fill's, AND IT DID NOT. Found while
        // wiring the tiles, 2026-08-12. Nothing in here needs the quads -
        // DrawLine has been in the ABI since version 1 - so on an older FLICK
        // this happily drew a CUT outline around a fill that had just fallen
        // back to a SQUARE rectangle, leaving the fill's corners sticking out
        // past their own border and the border missing at the corners. Worse
        // than either shape on its own. Ask the same question Fill asks and
        // degrade the same way.
        if (!HasQuads() || Policy::ClampCut(base) <= 0.0f) {
            for (int pass = 0; pass < passes; ++pass) {
                const float step = static_cast<float>(pass);
                FUCK::DrawRect(ImVec2{ base.min.x + step + 0.5f, base.min.y + step + 0.5f },
                               ImVec2{ base.max.x - step - 0.5f, base.max.y - step - 0.5f },
                               a_col, 0.0f, 1.0f);
            }
            return;
        }

        for (int pass = 0; pass < passes; ++pass) {
            const Policy::Rect ring = Policy::Inset(base, static_cast<float>(pass) + 0.5f);
            if (Degenerate(ring)) {
                break;
            }
            const Policy::Outline outline = Policy::BuildOutline(ring);
            for (int i = 0; i < outline.count; ++i) {
                const Policy::Point& p = outline.v[i];
                const Policy::Point& q = outline.v[(i + 1) % outline.count];
                FUCK::DrawLine(Vec(p), Vec(q), a_col, 1.0f);
            }
        }
    }

    void Draw(const ImVec2& a_min, const ImVec2& a_max, const Style& a_style) {
        Fill(a_min, a_max, a_style.fill, a_style.cut, a_style.corners);
        Stroke(a_min, a_max, a_style.edge, a_style.cut, a_style.border, a_style.corners);
    }

    void Draw(const ImVec2& a_min, const ImVec2& a_max) {
        Draw(a_min, a_max, DefaultStyle());
    }

    void DrawWindow(const Style& a_style) {
        const ImVec2 pos = FUCK::GetWindowPos();
        const ImVec2 size = FUCK::GetWindowSize();
        Draw(pos, ImVec2{ pos.x + size.x, pos.y + size.y }, a_style);
    }

    void DrawWindow() { DrawWindow(DefaultStyle()); }

    void DrawItem(const Style& a_style) {
        Draw(FUCK::GetItemRectMin(), FUCK::GetItemRectMax(), a_style);
    }

    // ---- the art path -----------------------------------------------------
    //
    // Ported from Fitting Room, whose copy carries the reasoning for each of the
    // decisions below. The slice fraction is the one number that MUST agree with
    // tools/make_frame_texture.py (SLICE over SIZE): a mismatch samples part of
    // the corner art into the stretched edge and reads as a frame that smears
    // near its corners.

    float ArtCorner(const ImVec2& a_min, const ImVec2& a_max) {
        const float shorter = (std::min)(a_max.x - a_min.x, a_max.y - a_min.y);
        // 20 texels of corner art, kept at its own weight rather than stretched,
        // so the corner looks the same on a tile as it does on a panel.
        const float art = 20.0f * (std::max)(1.0f, FUCK::GetResolutionScale());
        return (std::max)(1.0f, (std::min)(art, (std::max)(0.0f, shorter) * 0.25f));
    }

    void FillImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                   const ImVec4& a_tint, float a_corner, unsigned a_corners) {
        if (!a_tex || a_tint.w <= 0.0f) {
            return;
        }
        const float x0 = std::ceil(a_min.x), y0 = std::ceil(a_min.y);
        const float x1 = std::floor(a_max.x), y1 = std::floor(a_max.y);
        if (x1 - x0 <= 0.0f || y1 - y0 <= 0.0f) {
            return;
        }
        const float c = (std::max)(
            1.0f, (std::min)({ a_corner, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f }));
        constexpr float kS = 20.0f / 64.0f;
        const auto corner = [&](float a_dx, float a_dy, unsigned a_bit, float a_u, float a_v) {
            const ImVec2 p0{ a_dx, a_dy }, p1{ a_dx + c, a_dy + c };
            if (a_corners & a_bit) {
                FUCK::AddImage(a_tex, p0, p1, ImVec2{ a_u, a_v },
                               ImVec2{ a_u + kS, a_v + kS }, a_tint);
            } else {
                FUCK::DrawRectFilled(p0, p1, a_tint, 0.0f);
            }
        };
        corner(x0, y0, Policy::kTopLeft, 0.0f, 0.0f);
        corner(x1 - c, y0, Policy::kTopRight, 1.0f - kS, 0.0f);
        corner(x0, y1 - c, Policy::kBottomLeft, 0.0f, 1.0f - kS);
        corner(x1 - c, y1 - c, Policy::kBottomRight, 1.0f - kS, 1.0f - kS);
        // The cross between the corners is solid whatever the corners do, so it
        // is three rects rather than five stretched samples: same pixels, no
        // filtering, and it cannot pick up a seam from the texture's edge.
        FUCK::DrawRectFilled(ImVec2{ x0 + c, y0 }, ImVec2{ x1 - c, y0 + c }, a_tint, 0.0f);
        FUCK::DrawRectFilled(ImVec2{ x0, y0 + c }, ImVec2{ x1, y1 - c }, a_tint, 0.0f);
        FUCK::DrawRectFilled(ImVec2{ x0 + c, y1 - c }, ImVec2{ x1 - c, y1 }, a_tint, 0.0f);
    }

    void FrameImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                    const ImVec4& a_tint, float a_corner) {
        if (!a_tex || a_tint.w <= 0.0f) {
            return;
        }
        const float x0 = std::ceil(a_min.x), y0 = std::ceil(a_min.y);
        const float x1 = std::floor(a_max.x), y1 = std::floor(a_max.y);
        // A QUARTER, not a half: at a half the corners meet and the stretched
        // edge between them is zero wide, which draws a shape nobody asked for
        // rather than a frame.
        const float c = (std::max)(
            1.0f, (std::min)({ a_corner, (x1 - x0) * 0.25f, (y1 - y0) * 0.25f }));
        if (x1 - x0 <= 2.0f * c || y1 - y0 <= 2.0f * c) {
            return;
        }
        constexpr float kSliceUV = 20.0f / 64.0f;
        const float     u0 = 0.0f, u1 = kSliceUV, u2 = 1.0f - kSliceUV, u3 = 1.0f;
        // ⚠ AddImage, NOT DrawImage: no item is submitted, so a caller's
        // following IsItemHovered still asks about whatever it drew before this.
        const auto piece = [&](float a_dx0, float a_dy0, float a_dx1, float a_dy1,
                               float a_uu0, float a_vv0, float a_uu1, float a_vv1) {
            FUCK::AddImage(a_tex, ImVec2{ a_dx0, a_dy0 }, ImVec2{ a_dx1, a_dy1 },
                           ImVec2{ a_uu0, a_vv0 }, ImVec2{ a_uu1, a_vv1 }, a_tint);
        };
        piece(x0, y0, x0 + c, y0 + c, u0, u0, u1, u1);
        piece(x1 - c, y0, x1, y0 + c, u2, u0, u3, u1);
        piece(x0, y1 - c, x0 + c, y1, u0, u2, u1, u3);
        piece(x1 - c, y1 - c, x1, y1, u2, u2, u3, u3);
        piece(x0 + c, y0, x1 - c, y0 + c, u1, u0, u2, u1);
        piece(x0 + c, y1 - c, x1 - c, y1, u1, u2, u2, u3);
        piece(x0, y0 + c, x0 + c, y1 - c, u0, u1, u1, u2);
        piece(x1 - c, y0 + c, x1, y1 - c, u2, u1, u3, u2);
        // No centre piece: transparent in the texture, so drawing it would be a
        // full-panel blend that costs fill rate and changes nothing.
    }

}  // namespace MTB::ChamferPanel

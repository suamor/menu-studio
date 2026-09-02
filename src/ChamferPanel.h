#pragma once

#include "ChamferPolicy.h"

#include <imgui.h>

// Draws the chamfered panel whose geometry ChamferPolicy works out: a cut-corner
// frame in the Vel'dun idiom, painted by us because FLICK's theme format cannot
// describe one. ChamferPolicy.h carries the why, both for the shape and for the
// way it is tiled.
//
// HOW A CALLER USES THIS. The panel is chrome, so the window it decorates has to
// stop drawing its own:
//
//     FUCK::WindowFlags GetFlags() const override {
//         return FUCK::WindowFlags::kNoDecoration |
//                FUCK::WindowFlags::kNoBackground | ...;
//     }
//
//     void Draw() override {
//         MTB::ChamferPanel::DrawWindow();          // ground and frame first
//         ...contents...
//     }
//
// That is the same shape ActionBar already uses for its tile strip, so the
// pattern is proven in this plugin rather than proposed.
//
// ⚠ THE IMPLEMENTATION LIVES IN THE .CPP BECAUSE FUCK_API.h IS NOT SELF
// CONTAINED. It names CSimpleIniA, SKSE::log, std::string and the Win32 loader
// without including any of them, so it only compiles after PCH.h and
// <SimpleIni.h> - which is why every other file here reaches it from a .cpp and
// never from a header. Putting the drawing inline would push that ordering onto
// every future includer, and the failure mode is a hundred lines of syntax
// errors pointing into somebody else's header.
namespace MTB::ChamferPanel {

    namespace Policy = MTB::ChamferPolicy;

    using Policy::kAll;
    using Policy::kBottom;
    using Policy::kBottomLeft;
    using Policy::kBottomRight;
    using Policy::kLeft;
    using Policy::kNone;
    using Policy::kRight;
    using Policy::kTop;
    using Policy::kTopLeft;
    using Policy::kTopRight;

    struct Style {
        // Unscaled pixels, the same convention as ActionBar's tile constants:
        // FUCK::Scale is applied at draw time so one number holds at every
        // resolution and UI scale.
        float    cut     = 10.0f;
        float    border  = 1.0f;
        unsigned corners = kAll;
        ImVec4   fill{ 0.0f, 0.0f, 0.0f, 0.0f };
        ImVec4   edge{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // ⚠ THE COLOURS ARE READ FROM THE LIVE THEME, NOT BAKED. This pulls the
    // window background and border straight out of whatever style preset the
    // user has loaded, so a panel drawn through here follows a theme change with
    // no rebuild and matches the surrounding FLICK widgets by construction.
    // Baking a palette in would make our chrome the one thing in the menu that
    // ignores the user's preset, which is the complaint that starts every
    // theming thread.
    [[nodiscard]] Style DefaultStyle();

    // Solid ground, no edge.
    void Fill(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col,
              float a_cut, unsigned a_corners = kAll);

    // The frame, drawn as a_thickness stacked single-pixel rings.
    void Stroke(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col,
                float a_cut, float a_thickness, unsigned a_corners = kAll);

    // ---- the art path: a SCOOPED corner, which quads cannot draw -----------
    //
    // ⚠⚠ THESE THREE SPEAK FINAL DEVICE PIXELS AND EVERYTHING ELSE IN THIS FILE
    // DOES NOT. Fill, Stroke and Draw take an UNSCALED cut and apply FUCK::Scale
    // themselves, which is this mod's convention and stays that way. The corner
    // below is already scaled when it arrives. That is not an oversight and it
    // is not worth unifying: callers never spell the number themselves, they
    // pass ArtCorner's return value straight through, so the two conventions
    // never meet in a call site. Spell a number here by hand and it will be
    // wrong at every UI scale but yours.
    //
    // ⚠ Fitting Room's copy of this file is NOT interchangeable with it. Its
    // ChamferPanel takes final pixels throughout, because every dimension in
    // that editor comes off a font size that is already scaled three times over.
    // Check which tree you are in before moving a call site across.

    // The corner to hand BOTH FillImage and FrameImage for one rect, so the fill
    // and the outline come out the same shape.
    //
    // ⚠ HANDING THEM THE SAME NUMBER IS NOT ENOUGH. They clamp differently on
    // purpose: a fill may be cut up to HALF the shorter side, a frame only a
    // QUARTER, because a frame whose corners meet has no edge left to be an
    // edge. Pass a raw corner to both on a small rect and the fill is cut deeper
    // than the outline, so it retreats out of its own corners and whatever is
    // behind shows through beside the line. Never above a quarter here, so both
    // clamps accept the same value and the two cannot disagree.
    [[nodiscard]] float ArtCorner(const ImVec2& a_min, const ImVec2& a_max);

    // A filled panel whose corners are cut to the frame's curve, through a
    // texture whose alpha is the fill rather than a line. Corners outside the
    // mask are drawn as plain rects, so one call can cut the corners that meet
    // a frame and leave the ones that meet a straight edge square.
    void FillImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                   const ImVec4& a_tint, float a_corner, unsigned a_corners = kAll);

    // The outline, nine-sliced: corners at their own size, edges stretched along
    // their length only, and no centre piece because the texture is transparent
    // there.
    void FrameImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                    const ImVec4& a_tint, float a_corner);

    void Draw(const ImVec2& a_min, const ImVec2& a_max, const Style& a_style);
    void Draw(const ImVec2& a_min, const ImVec2& a_max);

    // The whole current window. Call this first thing inside Draw(), before any
    // content, so the chrome ends up underneath it in the draw list.
    void DrawWindow(const Style& a_style);
    void DrawWindow();

    // The last submitted item's rect, for decorating a single row or button
    // rather than a whole panel.
    void DrawItem(const Style& a_style);

}  // namespace MTB::ChamferPanel

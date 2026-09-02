#pragma once

#include <cstdint>

// Which SHAPE our own chrome draws: the carved corner this mod paints, or the
// plain one the surrounding theme uses.
//
// WHY THIS EXISTS. FLICK's theme format cannot describe a cut or carved corner
// (see ChamferPolicy.h), so this plugin paints its own out of a nine-slice
// texture.
//
// ⚠⚠ THERE IS NO AUTO ANY MORE, AND REMOVING IT IS THE FIELD'S CALL
// (2026-08-28: "now though the theme looks great on carved for vanilla and
// vel'dun ... don't even have the follow theme option, just make it carved and
// plain, carved by default"). Auto existed because the carve looked wrong under
// FLICK's own theme, and what actually looked wrong turned out to have nothing
// to do with the corner: Fitting Room's editor was stacking that theme's
// translucent ChildBg across nested child windows, and the bands it made were
// read as a nine-slice defect. The same art reads correctly under both presets
// once there is one ground under it, so the question auto was asked to answer
// no longer has two answers.
//
// ⚠⚠ A PRESET NAME NO LONGER DECIDES ANYTHING, so PresetIsCarved and the
// sCarvedPresets list it read are gone rather than left unused. Nothing opens
// FUCKs/FUCK/defaultstyle.ini for this any more. If a future shape ever needs
// the live preset again, the key is still there to read; what must not come
// back is a third Style that resolves differently in the two plugins.
//
// ⚠ THE WIRE NUMBERS DO NOT MOVE. 1 is carved and 2 is plain exactly as they
// were, so a player who went and found plain keeps it. 0 was auto and now reads
// as carved, which is both where auto landed under Vel'dun and where the default
// now sits for everyone else.
//
// ⚠ AND THE OVERRIDE STAYS A RUNTIME SETTING, never an install choice: an
// installer may write the starting value and must not be the only place it can
// be set.
//
// ⚠⚠ FITTING ROOM CARRIES A COPY OF THIS FILE AND THE TWO ARE THE SAME
// ARITHMETIC ON PURPOSE. They are separate plugins with separate INIs, so there
// is nothing to share at build time, and the one thing that must not drift is
// what a given preset resolves to: a player running both sees one editor and one
// button strip in the same menu, and a disagreement between them would show up
// as two different corners on one screen. Change one, change the other, and the
// tests are the check.
namespace MTB::FramePolicy {

    // What the INI holds. The numbers are the wire format, so append only.
    // ⚠ 0 IS ABSENT ON PURPOSE AND MUST NOT BE REUSED. It was auto until
    // 2026-08-28 and is still sitting in every INI written before then, so it
    // has to keep meaning something: StyleFromIni reads it, and everything else
    // it has never seen, as carved.
    enum class Style : std::uint8_t {
        kCarved = 1,
        kPlain  = 2,
    };

    // What the drawing code actually does. Deliberately a different type from
    // Style: Style is what the INI holds and may grow a value the draw sites
    // have never heard of, and Resolve is the one place that decides what such
    // a value looks like.
    enum class Shape : std::uint8_t {
        kCarved,
        kPlain,
    };

    [[nodiscard]] inline constexpr Style StyleFromIni(int a_value) {
        switch (a_value) {
            case 2:  return Style::kPlain;
            default: return Style::kCarved;
        }
    }

    [[nodiscard]] inline constexpr int IniFromStyle(Style a_style) {
        return static_cast<int>(a_style);
    }

    // ⚠ NO SECOND ARGUMENT ANY MORE. It carried "is the live preset one of
    // ours", which only auto ever read; a shape that answers from the setting
    // alone cannot disagree between the two plugins over a file neither of them
    // opens now.
    [[nodiscard]] inline constexpr Shape Resolve(Style a_style) {
        return a_style == Style::kPlain ? Shape::kPlain : Shape::kCarved;
    }

    // The radius a plain shape draws with, in final pixels: the theme's number
    // unless the SURFACE carries its own and that one is larger.
    //
    // ⚠⚠ a_default IS A PROPERTY OF THE SURFACE, NOT A SETTING, and it stopped
    // being one on 2026-08-14. It was [General] fPlainRounding for a day, one
    // flat radius for the plain path, and a flat number was the wrong KIND of
    // answer rather than the right kind badly tuned. Before any chamfer work
    // existed the action bar tiles drew `(x1 - x0) * kRoundingFraction`, a
    // fraction of their own width, which is what holds a shape together across
    // tile sizes and UI scales. ActionBar passes that fraction again.
    //
    // ⚠ THE THEME MAY ONLY ASK FOR MORE, so a surface's number is a FLOOR and
    // never a cap: a preset that rounds heavily still looks like itself, and no
    // surface can be forced squarer than the theme wants. Zero means follow the
    // theme exactly.
    //
    // ⚠ FITTING ROOM'S COPY OF THIS FILE SAYS THE SAME THING and reached it the
    // same day. Its plain path needed THREE different per-surface rules where
    // this one needs a single fraction, because Menu Studio paints exactly one
    // surface: the strip is kNoBackground and every other control is a stock
    // FLICK widget.
    //
    // ⚠ WRITTEN SO A NaN LANDS ON THE DEFAULT rather than propagating. A
    // GetStyleVar that answers garbage must not be able to pick the radius.
    [[nodiscard]] inline constexpr float PlainRadius(float a_themeRounding,
                                                     float a_default) {
        const float base = (a_default > 0.0f) ? a_default : 0.0f;
        return (a_themeRounding > base) ? a_themeRounding : base;
    }

    // ⚠ A RADIUS CANNOT EXCEED HALF THE SHORTER SIDE, the same clamp every
    // rounded-rect renderer applies internally. Spelled here so a caller can
    // predict what the draw call will do to it.
    [[nodiscard]] inline constexpr float ClampRadius(float a_radius, float a_width,
                                                     float a_height) {
        const float shorter = (a_width < a_height) ? a_width : a_height;
        const float limit   = (shorter > 0.0f) ? shorter * 0.5f : 0.0f;
        if (!(a_radius > 0.0f)) {
            return 0.0f;
        }
        return (a_radius > limit) ? limit : a_radius;
    }

}  // namespace MTB::FramePolicy

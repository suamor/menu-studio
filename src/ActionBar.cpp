#include "PCH.h"

#include "ActionBar.h"

#include "Bubble.h"
#include "ChamferPanel.h"  // the cut-corner tile, drawn by us: FLICK themes cannot
#include "FrameArt.h"      // the scooped corner, which quads cannot draw
#include "FramePolicy.h"   // carved or plain, and who decides
#include "MeterPolicy.h"   // where the charge fill sits inside the tile's shape
#include "Offsets.h"
#include "Settings.h"
#include "SettingsUI.h"
#include "VersionCheck.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callbacks

#include "FUCK_API.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cmath>
#include <string>
#include <vector>

namespace MTB::ActionBar {

    namespace {

        constexpr float kTileSize = 46.0f;   // unscaled; FUCK::Scale applies
        constexpr float kTileGap  = 6.0f;
        // ⚠ THE ITEM IS BIGGER THAN THE TILE, and this is the third and last
        // attempt at the shaved sides. The first two both tried to land the
        // stroke perfectly on the last pixel INSIDE the item rect - first by
        // snapping to the nearest pixel, then by rounding inward with a pixel
        // of margin. Both kept the tile edge and the window's clip edge in the
        // same place and bet on the arithmetic, and the field came back both
        // times.
        //
        // The bet is not worth making. This window auto-resizes to its content
        // and its clip rect is derived from a size computed a frame earlier, so
        // "exactly at the boundary" has a rounding story on both sides of it
        // that this code does not own. Reserving a few pixels MORE than the
        // tile and drawing the tile in the middle of that moves the stroke away
        // from the boundary entirely - there is nothing left to clip, whatever
        // the clip rect turns out to be. It costs the strip a few pixels of
        // width and nothing else.
        constexpr float kTileInset = 3.0f;   // unscaled, each side
        // Squircle, not a soft square: the corner radius is a FRACTION of the
        // tile, so the shape holds at every UI scale where a fixed pixel
        // radius would read square. 0.28 is the number Fitting Room's rail
        // tiles use, and these are meant to be recognisably the same button.
        constexpr float kRoundingFraction = 0.28f;
        // ⚠ THE GLYPH IS SIZED FROM THE TILE, NOT BY A FIXED MULTIPLIER, and
        // the first cut got this wrong in a way worth recording. Borrowing
        // Fitting Room's 1.45 looked like copying a proven number; it is
        // proven against ITS tile, which is measured in font heights, while
        // this one is a fixed 46 units. PushFontScaled multiplies the font's
        // own base size, so 1.45 came out at roughly 95% of the tile and the
        // symbol sat on the outline (field screenshot).
        //
        // Asking for a FRACTION OF THE TILE instead is the same request in
        // terms the tile can answer, and it survives any font base size or UI
        // scale the host is running. This is the em box, so the visible mark
        // lands a little smaller again.
        constexpr float kGlyphFraction = 0.52f;

        constexpr float kEdgeWidth = 2.0f;  // unscaled; the "thick" in the outline

        // How far a refusing tile's symbol fades. Enough to read as unavailable
        // at a glance, not so far that the button stops being identifiable:
        // the player still has to know WHICH thing they cannot have, and the
        // tooltip that says why is only reachable by recognising the tile.
        constexpr float kDisabledAlpha = 0.40f;

        // ⚠ FLICK'S THEME, AND THIS REVERSES THE BLOCK THAT USED TO STAND
        // HERE. The strip was dressed in SkyUI's own literals (near-black
        // translucent fills, parchment gold edges) on the reasoning that it
        // floats over SkyUI menus and should look like them. That reasoning
        // was not wrong about the look; it was wrong about who owns it. A
        // player who picks a FLICK theme is telling the host what its widgets
        // should look like, and a strip of buttons drawn by FLICK that ignores
        // the answer is the one thing on screen their choice cannot reach.
        // User's call, 2026-08-05: "make them use the same colors as default
        // FLICK so flick themes properly change the appearance."
        //
        // So the tile is an ordinary ImGui button, spelled the ordinary way:
        // the Button family carries the pointer state, Border draws the edge,
        // Text draws the symbol. A theme that restyles buttons restyles these.
        // ⚠ THE RESTING TILE IS ButtonActive, NOT Button, and that is a
        // deliberate shift of the whole ladder by one rung. User, on seeing
        // the first themed cut: "the appearance when we click on a button
        // actually looks very decent, let's use that for the regular button."
        //
        // It is also the right colour on the measurements. FLICK's theme reads
        // Button (0.60,0.60,0.60) against Text (0.73,0.74,0.75), which is a
        // separation of 0.14 - the "white on very white grey" report, in
        // numbers. A theme's plain Button is chosen to sit QUIETLY in a dense
        // panel full of other controls; a strip of three big pictograms
        // floating over a game is the opposite situation and wants the
        // emphatic end of the same family.
        //
        // Hover still brightens, because ButtonHovered is the brighter of the
        // two in every theme that distinguishes them, and a press then drops
        // back to Active. Bright on approach, deeper on contact, which is the
        // order a button is normally read in.
        // ⚠ THE FILL DOES NOT MOVE ON HOVER ANY MORE. It stepped
        // Active -> Hovered -> Active, which lit the tile up as the pointer
        // crossed it. User, 2026-08-05: "can we also keep dark background for
        // buttons when you hover over them, we can make it so the symbol goes
        // grey". A strip that sits over the character being looked at should
        // not flash a bright square every time the cursor passes, so the
        // hover moved off the background and onto the symbol (see GlyphColour).
        //
        // Held still steps, because a press is a deliberate act and wants an
        // answer. Hover is not.
        [[nodiscard]] ImVec4 TileFill(bool a_hovered, bool a_held) {
            (void)a_hovered;
            return FUCK::GetStyleColorVec4(a_held ? ImGuiCol_ButtonHovered
                                                  : ImGuiCol_ButtonActive);
        }

        // ⚠ THE SUBSTRATE EVERY OTHER IMGUI BUTTON GETS FOR FREE, and leaving
        // it out is what produced "barely any contrast between the button and
        // the symbol, it's like white on very white grey".
        //
        // A theme picks ImGuiCol_Button to be legible against its OWN
        // ImGuiCol_WindowBg, and it is usually translucent because it can
        // assume that background is there. This strip is kNoBackground: it
        // floats over the game with nothing behind it. So the theme's 40%
        // button was compositing onto whatever the scene happened to be, a
        // sunlit wall in the report, and the theme's white Text landed on the
        // pale result with nothing to separate them. The theme was not at
        // fault and neither was the Text colour; the surface they were both
        // designed against was missing.
        //
        // Painting WindowBg under the fill restores exactly the pair the theme
        // authored, so Text-on-Button reads the way that theme intends on any
        // backdrop, and a light theme keeps working too (dark text over a pale
        // substrate is the same relationship the other way up).
        //
        // Alpha is floored rather than taken as-is: a theme that runs a fairly
        // transparent WindowBg gets away with it inside a window stacked on
        // more window, and out here that is the whole problem again. A little
        // of the scene still shows through, which is what keeps the strip
        // feeling like it is over the menu rather than pasted onto it.
        [[nodiscard]] ImVec4 TileBackdrop() {
            ImVec4 bg = FUCK::GetStyleColorVec4(ImGuiCol_WindowBg);
            bg.w = (std::max)(bg.w, 0.90f);
            return bg;
        }

        // What the live theme rounds a FRAME by, in final pixels. Used for the
        // radius of a PLAIN tile, never to decide whether a tile is plain: see
        // FramePolicy.h for why rounding cannot answer that question.
        //
        // ⚠⚠ THE ABI WRAPPER DOES NOT CHECK THE FUNCTION POINTER, only that an
        // interface exists, so an older FLICK whose struct stops short of this
        // slot would be called through a pointer nobody wrote. Same question
        // HasQuads asks in ChamferPanel, and the same answer: ask first. Zero
        // reads as square, which is the safe direction.
        //
        // ⚠ FrameRounding AND NOT WindowRounding. A tile is a frame-sized thing
        // sitting among the theme's buttons; the window number is for a panel,
        // and several themes set the two differently on purpose.
        [[nodiscard]] float ThemeFrameRounding() {
            const auto* const i = FUCK::GetInterface();
            if (!i || !i->GetStyleVar) {
                return 0.0f;
            }
            return FUCK::GetStyleVar(ImGuiStyleVar_FrameRounding);
        }

        // ⚠⚠ NOTHING READS THE LIVE PRESET ANY MORE. LivePresetIsCarved sat
        // here until 2026-08-28: an INI read of FUCKs/FUCK/defaultstyle.ini
        // behind a one-second cache, because auto had to know which theme was
        // up. Auto is gone (FramePolicy.h has the field call), so the file is no
        // longer opened at all and the cache and its mutex went with it.

        // ⚠ ONE LINE, ONCE, FOR THE SAME REASON LogThemeOnce EXISTS. Two rounds
        // went on reasoning about what the theme held; "we lost the rounded
        // buttons" turned out to be a 1.0 rounding honoured faithfully, and
        // nothing in any log said so. This prints what the ABI answers and what
        // the strip decided, so the next report about a shape is a comparison.
        void LogFrameShapeOnce(FramePolicy::Shape a_shape, float a_radius) {
            static bool once = false;
            if (once) {
                return;
            }
            once                = true;
            const auto* const i = FUCK::GetInterface();
            spdlog::info("action bar shape: GetStyleVar={} FrameRounding={:.2f} -> {} "
                         "radius {:.2f} (tile x {:.2f}) scale {:.2f}; "
                         "iFrameStyle={}",
                         (i && i->GetStyleVar) ? "yes" : "MISSING", ThemeFrameRounding(),
                         a_shape == FramePolicy::Shape::kPlain ? "PLAIN" : "CARVED", a_radius,
                         kRoundingFraction, FUCK::GetResolutionScale(),
                         Settings::GetSingleton().frameStyle);
        }

        // Perceived brightness, the usual sRGB weights. Only ever compared
        // against another one of these, so the lack of gamma handling costs
        // nothing.
        [[nodiscard]] float Luminance(const ImVec4& a_c) {
            return 0.2126f * a_c.x + 0.7152f * a_c.y + 0.0722f * a_c.z;
        }

        // a over b, where b is treated as opaque. True of the substrate by
        // construction (TileBackdrop floors its alpha), which is the only
        // thing this is used on.
        [[nodiscard]] ImVec4 Over(const ImVec4& a_over, const ImVec4& a_under) {
            const float t = std::clamp(a_over.w, 0.0f, 1.0f);
            return ImVec4{ a_under.x + (a_over.x - a_under.x) * t,
                           a_under.y + (a_over.y - a_under.y) * t,
                           a_under.z + (a_over.z - a_under.z) * t, 1.0f };
        }

        // ⚠ THE THEME PICKS THE HUE. THIS GUARANTEES IT CAN BE SEEN.
        //
        // Handing the colours to FLICK was right and the field verdict on the
        // first cut of it was "barely any contrast between the button and the
        // symbol, it's like white on very white grey". Taking a theme's Text
        // and its Button and trusting the pair is what produced that: those
        // two are authored against each other INSIDE a window, over a
        // WindowBg, at a text size where a thin stem has a whole word of
        // context around it. A 46 unit pictogram alone on a floating tile has
        // none of that, so a pairing that is merely adequate in a settings
        // dialog is illegible here.
        //
        // So the theme's colour is kept and then moved along its own axis, as
        // far as it takes to clear a minimum separation from what it is being
        // drawn on and no further. A theme with good contrast is returned
        // untouched; a theme with poor contrast gets the smallest correction
        // that makes its own colour work. Luminance is linear in the channels,
        // so the distance to travel solves exactly rather than by iterating.
        [[nodiscard]] ImVec4 EnsureContrast(ImVec4 a_colour, float a_backdropLum,
                                            float a_minSeparation) {
            const float fg = Luminance(a_colour);
            if (std::fabs(fg - a_backdropLum) >= a_minSeparation) {
                return a_colour;
            }
            // Away from the backdrop, toward whichever end has the room.
            const float target = a_backdropLum < 0.5f ? 1.0f : 0.0f;
            const float want = a_backdropLum < 0.5f
                                   ? a_backdropLum + a_minSeparation
                                   : a_backdropLum - a_minSeparation;
            const float denom = target - fg;
            const float t = std::fabs(denom) > 1e-4f
                                ? std::clamp((want - fg) / denom, 0.0f, 1.0f)
                                : 1.0f;
            a_colour.x += (target - a_colour.x) * t;
            a_colour.y += (target - a_colour.y) * t;
            a_colour.z += (target - a_colour.z) * t;
            return a_colour;
        }

        // ⚠ ONE LINE, ONCE, BECAUSE TWO ROUNDS WENT ON GUESSING WHAT THE THEME
        // HELD. "White on very white grey" and "extremely shit icons" are both
        // reports about numbers nobody had read, and the rules above are only
        // as good as the values they are fed. This prints them, so the next
        // time the strip looks wrong the answer is a comparison rather than
        // another round of reasoning about what FLICK's default probably is.
        void LogThemeOnce(const ImVec4& a_backdrop, const ImVec4& a_fill,
                          float a_tileLum) {
            static bool once = false;
            if (once) {
                return;
            }
            once = true;
            const auto text = FUCK::GetStyleColorVec4(ImGuiCol_Text);
            const auto border = FUCK::GetStyleColorVec4(ImGuiCol_Border);
            const auto plain = FUCK::GetStyleColorVec4(ImGuiCol_Button);
            const auto hover = FUCK::GetStyleColorVec4(ImGuiCol_ButtonHovered);
            spdlog::info(
                "action bar theme: WindowBg->substrate ({:.2f},{:.2f},{:.2f},{:.2f}) "
                "resting fill ({:.2f},{:.2f},{:.2f},{:.2f}) -> tile luminance {:.2f}. "
                "Text ({:.2f},{:.2f},{:.2f},{:.2f}) lum {:.2f}, Border "
                "({:.2f},{:.2f},{:.2f},{:.2f}) lum {:.2f}. Unused Button "
                "({:.2f},{:.2f},{:.2f},{:.2f}) lum {:.2f}, hover "
                "({:.2f},{:.2f},{:.2f},{:.2f}) lum {:.2f}. Symbol asks for {:.2f} of "
                "separation at rest, outline {:.2f}.",
                a_backdrop.x, a_backdrop.y, a_backdrop.z, a_backdrop.w, a_fill.x,
                a_fill.y, a_fill.z, a_fill.w, a_tileLum, text.x, text.y, text.z,
                text.w, Luminance(text), border.x, border.y, border.z, border.w,
                Luminance(border), plain.x, plain.y, plain.z, plain.w,
                Luminance(plain), hover.x, hover.y, hover.z, hover.w,
                Luminance(hover), 0.55f, 0.40f);
        }

        // ⚠ WITH A FLOOR UNDER IT, because an outline was asked for by name
        // and ImGuiCol_Border is a colour a theme is allowed to leave fully
        // transparent (several do, to get borderless frames). Taking that
        // literally would answer "give the tiles proper outlines" with no
        // outline at all on those themes. When the theme declines to specify
        // one, the edge is derived from its own Text colour instead, which
        // still tracks the theme rather than reintroducing a literal.
        [[nodiscard]] ImVec4 TileEdge(bool a_hovered, bool a_held, float a_tileLum) {
            ImVec4 edge = FUCK::GetStyleColorVec4(ImGuiCol_Border);
            if (edge.w < 0.05f) {
                edge = FUCK::GetStyleColorVec4(ImGuiCol_Text);
            }
            // ⚠ OPAQUE, WHATEVER THE THEME SAID. Border colours are routinely
            // half-transparent because inside a window they sit between two
            // known surfaces and only need to hint at a division. This one is
            // the entire silhouette of a button floating over a game, and half
            // an alpha is how an outline ends up reading as a smudge.
            edge.w = 1.0f;
            // State steps the separation rather than the alpha, so the tile
            // under the pointer brightens on a dark theme and darkens on a
            // light one without either being spelled out here.
            return EnsureContrast(edge, a_tileLum,
                                  a_held ? 0.60f : a_hovered ? 0.52f : 0.40f);
        }

        // ⚠ THE REST DIM IS GONE. The palette this replaced faded the symbol
        // at rest so hover had somewhere to go, and that was affordable when
        // the colour underneath it was a known near-black literal. Against a
        // theme's Button colour it is a discount off a contrast budget that
        // had already run out. Hover now brightens by asking for MORE
        // separation instead, which costs the resting state nothing.
        // ⚠ HOVER DIMS THE SYMBOL RATHER THAN BRIGHTENING ANYTHING. It is the
        // inverse of the usual direction and it is deliberate: with the tile
        // holding still (see TileFill), the symbol is the only thing left to
        // answer the pointer, and greying it reads as "this one" without
        // putting a lit square over the character. Asking for LESS separation
        // is how that is spelled here - the symbol moves back toward the tile
        // it sits on, so it greys on a dark theme and darkens on a light one
        // with nothing named in either case.
        [[nodiscard]] ImVec4 GlyphColour(bool a_hot, float a_tileLum) {
            ImVec4 text = FUCK::GetStyleColorVec4(ImGuiCol_Text);
            text.w = 1.0f;
            // Legible first, on whatever the theme gave us.
            text = EnsureContrast(text, a_tileLum, 0.55f);
            if (!a_hot) {
                return text;
            }
            // ⚠ TOWARD A GREY OF THE TILE'S OWN BRIGHTNESS, not toward the
            // tile's colour. Fading into the tile hue would tint the symbol on
            // a coloured theme; fading toward its luminance desaturates it,
            // which is what "goes grey" means and reads the same on every
            // theme. EnsureContrast cannot do this - it only ever RAISES
            // separation, so asking it for less returns the colour untouched.
            constexpr float kHoverFade = 0.55f;
            text.x += (a_tileLum - text.x) * kHoverFade;
            text.y += (a_tileLum - text.y) * kHoverFade;
            text.z += (a_tileLum - text.z) * kHoverFade;
            return text;
        }

        struct Entry {
            std::string id;
            std::string label;
            std::string iconPath;
            std::string glyph;      // lettered fallback, computed once at register
            Callback    onClick = nullptr;
            FUCK::Image icon;       // loaded lazily, see EnsureIcon
            bool        iconTried = false;
            // Which bubbled menus this button belongs in. EMPTY MEANS EVERY
            // MENU, which is what every registration made before SetMenus
            // existed gets, so nobody has to adopt this to keep working.
            std::vector<std::string> menus;
            // Stood down by its owner through SetVisible. A context can be
            // finer than a menu name - Fitting Room's editor is a WINDOW
            // over the inventory, and only its owner sees that window's
            // edges. Not touched by Register's update-in-place, so a
            // re-registration keeps it the way it keeps the scope; distinct
            // from the settings panel's hide, which is the PLAYER's word.
            bool hidden = false;
            // Present but refusing, with the owner's reason for the tooltip.
            //
            // ⚠ NOT THE SAME AS hidden, AND THE DIFFERENCE IS THE POINT. Hiding
            // says "there is nothing here"; this says "there is something here
            // and you cannot have it yet". A button whose owner can state a
            // PRECONDITION - Fitting Room's character editor wants the price of
            // entry - was previously forced to choose between lying about its
            // existence and offering something that would not work.
            //
            // Owner's word, like hidden, so it survives re-registration and the
            // player's own panel switch is still separate and still wins.
            bool        disabled = false;
            std::string disabledReason;
            // How full the owner says this button's resource is, 0 to 1, or
            // NEGATIVE for "I have no meter", which is every button that has
            // never called SetMeter.
            //
            // ⚠ THE OWNER'S NUMBER, DRAWN BY US, and neither half can do it
            // alone. Fitting Room's Seamstone holds the charge that pays for
            // styling, and a player looking at the strip wants to know whether
            // pressing the button will get them anywhere before they press it.
            // Fitting Room cannot draw on our tile and we have no idea what a
            // Seamstone is, so the number crosses and the picture stays here.
            //
            // Owner's word like `hidden` and `disabled`, so it survives a
            // re-registration, and safe to re-assert every frame.
            float meter = -1.0f;
            // Came in through MenuStudio_RegisterAction rather than from this
            // file's own Install(). The only thing it decides is whether the
            // strip may offer this tile while the owner-context gate holds the
            // studio down - see Register's note, and the ⚠ in ActionBar.h on
            // why the rule is "external" rather than a list of ids.
            bool        external = false;
        };

        // A button with no declared menus draws everywhere. One that named its
        // menus draws only in those.
        [[nodiscard]] bool ShowsIn(const Entry& a_e, const std::string& a_menu) {
            return a_e.menus.empty() ||
                   std::find(a_e.menus.begin(), a_e.menus.end(), a_menu) !=
                       a_e.menus.end();
        }


        // "InventoryMenu, BarterMenu" -> two names. Blanks are dropped rather
        // than stored, so a trailing comma cannot leave an entry that matches
        // nothing and silently hides the button.
        [[nodiscard]] std::vector<std::string> SplitMenus(const char* a_csv) {
            std::vector<std::string> out;
            if (!a_csv) {
                return out;
            }
            std::string cur;
            const auto flush = [&] {
                const auto first = cur.find_first_not_of(" \t");
                if (first == std::string::npos) {
                    cur.clear();
                    return;
                }
                const auto last = cur.find_last_not_of(" \t");
                out.push_back(cur.substr(first, last - first + 1));
                cur.clear();
            };
            for (const char* p = a_csv; *p; ++p) {
                if (*p == ',') {
                    flush();
                } else {
                    cur.push_back(*p);
                }
            }
            flush();
            return out;
        }

        // A vector rather than a map: the strip's order IS registration order,
        // and a container that reshuffles between frames gives the player a
        // different button under the same pixel from one open to the next.
        std::vector<Entry> g_entries;

        // ⚠ THE CIRCULARITY THIS ANSWERS. The strip has always drawn only while
        // IsBubbleActive(), and the owning mod's own hotkey path needs the menu
        // already open - so a gate that holds the bubble down would also hide
        // the button that lifts it and leave the hotkey as the only way in.
        //
        // While the gate holds, the strip draws on the MENU alone and offers
        // only entries registered from outside. That rule needs no knowledge of
        // which mod is on the other side, which is what this file's opening note
        // forbids: Menu Studio's own RaceMenu tile is born hidden and is
        // revealed only while the owner's editor is up, so it stays correct
        // without ever being named here.
        [[nodiscard]] bool StudioIsDown() {
            return !Bubble::IsBubbleActive() && Bubble::StudioHeldForOwnerContext();
        }

        // Would this entry be drawn right now? The same three voices DrawBar
        // honours - the owner's hide, the player's panel hide, and the menu
        // scope - plus the externals-only rule while the studio is down.
        [[nodiscard]] bool WouldDraw(const Entry& a_e, const std::string& a_menu,
                                     bool a_studioDown) {
            if (a_studioDown && !a_e.external) {
                return false;
            }
            return !a_e.hidden && !Settings::GetSingleton().IsActionHidden(a_e.id) &&
                   ShowsIn(a_e, a_menu);
        }

        // Anything at all to draw? "If nothing external is visible in this menu,
        // draw nothing" - barter, container and magic reach that case at once,
        // since an outfit editor scopes itself to the inventory.
        [[nodiscard]] bool AnythingToDraw(bool a_studioDown) {
            const std::string& menuNow = Bubble::GetSingleton().CurrentMenuName();
            for (const auto& e : g_entries) {
                if (WouldDraw(e, menuNow, a_studioDown)) {
                    return true;
                }
            }
            return false;
        }

        struct WindowState {
            ImVec2 lastPos{};
            ImVec2 lastSize{};
            // The same rectangle as fractions of the display, published on each
            // draw. Anything outside the FLICK render pass (the camera layer
            // reads this from the input sink) cannot safely call FUCK's ImGui
            // queries, so the bar states its own bounds rather than being asked.
            float uMin = 0.0f, vMin = 0.0f, uMax = 0.0f, vMax = 0.0f;
            bool  onScreen = false;
            // The cursor, same space, same reason. See PublishedCursor.
            float cursorU = 0.0f, cursorV = 0.0f;
            bool  haveCursor = false;
            // The display's own extent, same reason again: the grid-window
            // exclusion has to turn another mod's pixel rectangle into this
            // space, and it cannot ask ImGui from the input sink.
            float dispW = 0.0f, dispH = 0.0f;
            // Is the pointer over ANY FLICK window, not just this one? Fitting
            // Room's editor is one, and a camera drag must not start on it.
            //
            // ⚠ STAMPED, because this is only true for the frame it was read.
            // The bar stops drawing whenever the bubble is not active, which
            // includes the first frames of a menu open before the pause lands.
            // An unstamped flag left true from the last time the pointer sat on
            // a window would then refuse every drag until the bar drew again.
            bool  overAnyWindow = false;
            float overAnyWindowAt = 0.0f;  // seconds, steady clock
        };
        WindowState g_win;

        // Where the tile that was just clicked sits, in screen pixels. A
        // button that opens a window wants that window beside IT rather than
        // wherever the window was last dragged, and the only place the tile's
        // rectangle is known is the frame that drew it.
        //
        // ⚠ WRITTEN DURING THE DRAW AND READ FROM THE CALLBACK, which is safe
        // only because DrawBar defers the callback to after its loop: the tile
        // that set this is the tile whose handler runs. Recording it inside the
        // click branch rather than for every tile keeps a hover from moving an
        // anchor a handler is about to read.
        ImVec2 g_clickMin{};
        ImVec2 g_clickMax{};
        bool   g_haveClick = false;

        [[nodiscard]] float NowSeconds() {
            using namespace std::chrono;
            return duration<float>(steady_clock::now().time_since_epoch()).count();
        }

        // Anything older than a few frames is not an answer about where the
        // pointer is now.
        constexpr float kPublishFreshness = 0.20f;

        // Font Awesome 5 Free Solid, rendered from FUCK's own baked fa-solid
        // atlas - the only glyph source a FLICK window has (its host font is
        // baked at startup and nothing can add a codepoint later). The
        // codepoint is field-proven under FLICK by Fitting Room's editor,
        // which draws it from the same atlas as its Columns button. A
        // codepoint NOT proven there may render as tofu, which is why it is
        // not picked off a cheatsheet.
        constexpr std::uint16_t kGlyphGear = 0xf013;  // cog - settings
        constexpr std::uint16_t kGlyphFace = 0xf007;  // user - the character editor

        // The codepoint as UTF-8, same three-liner Fitting Room's Icons::Utf8
        // uses. Every FA glyph in play is three bytes.
        [[nodiscard]] std::string Utf8(std::uint16_t a_cp) {
            std::string out;
            out.push_back(static_cast<char>(0xE0 | ((a_cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
            return out;
        }

        // Up to two letters, taken from the start of the first two words, so
        // "Fitting Room" reads FR and "Settings" reads Se. Only ever seen when
        // an icon is absent or failed to load.
        [[nodiscard]] std::string GlyphFor(const std::string& a_label) {
            std::string out;
            bool atWordStart = true;
            for (const char c : a_label) {
                if (c == ' ' || c == '\t') {
                    atWordStart = true;
                    continue;
                }
                if (atWordStart && out.size() < 2) {
                    out.push_back(c);
                    atWordStart = false;
                }
            }
            if (out.size() == 1 && a_label.size() > 1) {
                out.push_back(a_label[1]);
            }
            return out.empty() ? std::string{ "?" } : out;
        }

        // Deferred to the first draw. A plugin can register at kDataLoaded,
        // which may land before FUCK is connected, and Image's constructor
        // silently yields an unloaded handle when there is no interface yet.
        // Trying once and remembering keeps a bad path from being reopened
        // every frame.
        void EnsureIcon(Entry& a_e) {
            if (a_e.iconTried || a_e.iconPath.empty()) {
                return;
            }
            a_e.iconTried = true;
            a_e.icon = FUCK::Image{ a_e.iconPath.c_str() };
            if (!a_e.icon.IsLoaded()) {
                spdlog::warn("action bar: '{}' asked for icon '{}' and it did not "
                             "load; falling back to a lettered tile.",
                             a_e.id, a_e.iconPath);
            }
        }

        // Returns the callback to run, rather than running it. A button whose
        // handler unregisters itself would otherwise mutate g_entries while the
        // draw loop is walking it, and the caller cannot be stopped from writing
        // that. Invoked once the loop is finished.
        //
        // The shape is Fitting Room's glyph-button idiom (its slot sidebar):
        // one invisible hit area, a rounded fill painted under it that answers
        // the pointer, and the symbol centered on top. The colours come from
        // FLICK's theme - see the palette block above, which reverses an
        // earlier decision to use SkyUI literals. kNoNav like Fitting Room's:
        // the strip is mouse-only.
        [[nodiscard]] Callback DrawTile(Entry& a_e) {
            EnsureIcon(a_e);
            const float tile = FUCK::Scale(kTileSize);
            const float inset = (std::max)(2.0f, std::floor(FUCK::Scale(kTileInset)));
            // The HIT AREA is the tile plus its margin, so the few pixels of
            // daylight around the outline still belong to the button. A dead
            // ring around a 46 unit target would be its own bug report.
            const float  size = tile + inset * 2.0f;
            const ImVec2 origin = FUCK::GetCursorScreenPos();

            FUCK::PushItemFlag(FUCK::ItemFlags::kNoNav, true);
            const bool pressed = FUCK::InvisibleButton(a_e.id.c_str(),
                                                       ImVec2{ size, size });
            FUCK::PopItemFlag();
            const ImVec2 after = FUCK::GetCursorScreenPos();
            // ⚠ THE ITEM IS STILL SUBMITTED WHEN DISABLED, and it has to be.
            // The tooltip is what a disabled tile is FOR, and a tooltip needs
            // something hovered to hang off; skipping the item would leave a
            // dead square that explains nothing. So the click is swallowed here
            // instead, at the one place that decides whether a press counts.
            const bool clicked = pressed && !a_e.disabled;
            // Hover is still read, because the tooltip wants it. The two
            // APPEARANCE terms are forced off instead, so a refusing tile never
            // lights up under the cursor or sinks under a press: looking
            // interactive is the whole of what makes an inert control feel
            // broken rather than closed.
            const bool   pointerOver = FUCK::IsItemHovered();
            const bool   hovered = pointerOver && !a_e.disabled;
            const bool   held = FUCK::IsItemActive() && !a_e.disabled;

            if (clicked) {
                // The VISIBLE tile, not the padded item: this is what the
                // settings panel pops out beside, and it should read from the
                // button the player can see.
                g_clickMin = ImVec2{ origin.x + inset, origin.y + inset };
                g_clickMax = ImVec2{ origin.x + inset + tile, origin.y + inset + tile };
                g_haveClick = true;
            }

            // ⚠ SNAPPED TO WHOLE PIXELS, WHICH IS WHAT MAKES THE STROKE ONE
            // WIDTH ALL THE WAY ROUND. Both inputs here are fractional - the
            // tile's origin comes from the layout and its size from a
            // resolution scale - and a 2px line centred on a fractional
            // coordinate is spread across three rows of pixels at partial
            // coverage. The eye reads that as a border that is thicker on
            // some edges than others, which is exactly the field report, and
            // it lands differently on the horizontal and vertical edges
            // because their fractions differ. Rounded INWARD so the tile can
            // only ever shrink toward the middle of its margin, never eat into
            // it and meet the auto-resizing window's clip edge, which is what
            // shaved the sides off the tiles twice.
            //
            // The glyph centring further down reads these four back, so they
            // stay here rather than moving into the panel call.
            const float x0 = std::ceil(origin.x + inset);
            const float y0 = std::ceil(origin.y + inset);
            const float x1 = std::floor(origin.x + inset + tile);
            const float y1 = std::floor(origin.y + inset + tile);
            const ImVec2 cMin{ x0, y0 };
            const ImVec2 cMax{ x1, y1 };

            // ⚠ THE CUT IS UNSCALED, AND `tile` ABOVE IS NOT. ChamferPanel
            // applies FUCK::Scale to the cut itself, so handing it
            // tile * fraction would scale it twice: correct at 100% UI scale
            // and growing wronger from there, which is the worst kind of thing
            // to find in a screenshot.
            //
            // ⚠ A FRACTION OF THE TILE, NOT A PIXEL COUNT, for the reason
            // kRoundingFraction is one: the shape has to hold at every UI
            // scale, and a fixed cut reads square on a large tile and swallows
            // a small one. Same 0.28 the squircle used, so the silhouette is
            // recognisably the same button with its corners cut instead of
            // curved.
            const float cut = kTileSize * kRoundingFraction;

            // Substrate first, then the theme's button colour over it. Two
            // passes rather than one blended constant, so a translucent button
            // colour stays translucent AGAINST THE SUBSTRATE and its hover and
            // active steps still read as the theme drew them.
            //
            // ⚠ TileBackdrop(), NOT ChamferPanel::DefaultStyle(). The helper
            // reads WindowBg raw; this strip is kNoBackground and floats over
            // the game world, so its substrate floors that alpha at 0.90.
            // Handing the raw theme colour to the tiles is precisely the
            // "barely any contrast between the button and the symbol" report.
            const ImVec4 backdrop = TileBackdrop();
            const ImVec4 fill = TileFill(hovered, held);
            // ⚠⚠ THE CORNER COMES FROM ART WHEN WE HAVE IT, AND THE TWO SHAPES
            // ARE NOT THE SAME SHAPE. ChamferPanel's quad path can only draw an
            // octagon: a straight bevel across the corner. The look this is
            // matching is a SCOOP, a disc bitten out, which is an arc. Fitting
            // Room hit this first and the answer there was a texture, because
            // nothing in the ABI draws an arc and spending triangles on one at
            // every size is worse than one sample (user 2026-08-12, "make the
            // icon buttons for menu studio have the circular corner cut like the
            // outline interface").
            //
            // ⚠ artCut IS IN FINAL PIXELS AND `cut` ABOVE IS NOT. Fill and
            // Stroke scale what they are given; the image calls do not. That is
            // why the fallback below still passes `cut` and nothing here mixes
            // the two.
            // ⚠ THE SHAPE IS THE SETTING AND NOTHING ELSE. It consulted the live
            // FLICK preset for one day; carved is the default again on both
            // plugins and the theme gets no vote. Resolved per draw rather than
            // cached, so a change in the settings shows without a restart.
            const auto shape = FramePolicy::Resolve(
                FramePolicy::StyleFromIni(Settings::GetSingleton().frameStyle));
            const bool  plain   = shape == FramePolicy::Shape::kPlain;
            const auto  fillTex = plain ? ImTextureID{} : FrameArt::Fill();
            const float artCut  = ChamferPanel::ArtCorner(cMin, cMax);
            // The radius the plain path draws with, clamped exactly as the
            // renderer will clamp it, so the meter below can predict it.
            //
            // ⚠⚠ THE SAME FRACTION THE CARVED CUT USES, AND IT IS WHAT THESE
            // TILES DREW BEFORE ANY OF THIS EXISTED. At 5faf221, the last commit
            // before the chamfer work, the tile was a squircle painted with
            // `(x1 - x0) * kRoundingFraction` and no theme read anywhere near it.
            // A flat pixel count replaced that for one day and it was wrong for
            // the same reason a flat CUT would be, which the comment on `cut`
            // above already spells out: a fixed radius reads square on a large
            // tile and swallows a small one. Fitting Room reached the identical
            // answer for its rail tiles on 2026-08-14, which is fitting, because
            // kRoundingFraction was copied FROM those tiles to make the two mods
            // look like one set of buttons.
            //
            // ⚠ FINAL PIXELS, unlike `cut`. This goes to FUCK::DrawRectFilled
            // directly rather than through ChamferPanel, so nothing scales it on
            // the way in and the fraction is taken of the already-scaled snapped
            // width. Handing it `kTileSize * kRoundingFraction` would be the
            // unscaled number in a scaled slot: right at 100% and shrinking from
            // there.
            //
            // ⚠ THE THEME MAY STILL ASK FOR MORE, so a preset that rounds harder
            // than this keeps its own look. It cannot ask for less.
            const float plainR = FramePolicy::ClampRadius(
                FramePolicy::PlainRadius(ThemeFrameRounding(),
                                         (cMax.x - cMin.x) * kRoundingFraction),
                cMax.x - cMin.x, cMax.y - cMin.y);
            LogFrameShapeOnce(shape, plainR);
            if (plain) {
                // The theme's own shape, drawn by us because the strip is
                // kNoBackground and there is nothing else out here to draw it.
                FUCK::DrawRectFilled(cMin, cMax, backdrop, plainR);
                FUCK::DrawRectFilled(cMin, cMax, fill, plainR);
            } else if (fillTex) {
                ChamferPanel::FillImage(fillTex, cMin, cMax, backdrop, artCut);
                ChamferPanel::FillImage(fillTex, cMin, cMax, fill, artCut);
            } else {
                // No PNG staged means the straight bevel, which is exactly what
                // these tiles looked like before the art existed. A hand install
                // that misses the icons folder degrades, it does not break.
                ChamferPanel::Fill(cMin, cMax, backdrop, cut);
                ChamferPanel::Fill(cMin, cMax, fill, cut);
            }

            // ---- the owner's meter, if it published one --------------------
            //
            // ⚠⚠ THE TILE IS THE GAUGE, AND THE BAR THAT USED TO SIT INSIDE IT
            // IS GONE (user 2026-08-14, "make the button charge fill in the
            // button background"). The bar was inset because the FLICK ABI
            // exposes no clip rect and no window draw list, so a partial fill
            // could not be masked to the cut corners and a full-width bar poked
            // out through both bottom cuts. The way round that is not a mask:
            // draw the FILLED PART as its own cut shape and its corners cut
            // themselves, with nothing to clip.
            //
            // ⚠ TWO EDGES, AND BOTH ARE ARITHMETIC RATHER THAN JUDGEMENT.
            //   - A full stone reaches the top of the tile, so it has to cut
            //     the TOP corners as well or it draws square shoulders inside
            //     a cut silhouette.
            //   - A nearly empty one is SHORTER THAN THE CORNER IT IS ASKING
            //     FOR, and both fill paths clamp the corner they are given to
            //     the rect they are given: FillImage to half the shorter side,
            //     ChamferPolicy::ClampCut to the same. A smaller corner is a
            //     SHALLOWER cut than the tile's, so the fill would stand
            //     outside the tile's own corners and show violet past the
            //     silhouette. Insetting the sides by exactly the difference
            //     puts it back inside, and MeterPolicy.h carries the two lines
            //     of arithmetic that make it exact rather than close - one for
            //     the bevel and a separate one for the art corner, which is a
            //     quarter disc and does NOT follow from the bevel's.
            //
            // ⚠ BETWEEN THE BACKDROP AND THE EDGE, so the theme's own hover and
            // press steps still read over it. Painting it last would make a
            // full stone look identical hovered and not.
            //
            // ⚠ NEGATIVE LEAVES THE TILE EXACTLY AS IT WAS. Zero is "the stone
            // is empty" and draws the trough; negative is "I publish no meter"
            // and must not put a dark wash over every other button in the
            // strip. SetMeter carries the same contract from the other end.
            //
            // A soul-gem violet rather than the theme's accent: the meter is a
            // THING IN THE WORLD the player recognises, and an accent-coloured
            // tile would read as a selection state on themes whose accent is
            // warm.
            const ImVec4 meterTrough{ 0.0f, 0.0f, 0.0f, 0.28f };
            const ImVec4 meterFill{ 0.52f, 0.38f, 0.78f, 0.62f };
            // ⚠ THE LEVEL NEEDS AN EDGE, AND THAT IS WHAT WAS MISSING. A
            // translucent wash rising up a tile has no line anywhere on it, so
            // it reads as the tile being tinted rather than as a level, and the
            // player sees violet bleeding into the button instead of a gauge
            // (user 2026-08-29). Every liquid meter worth reading has a
            // surface; this is that surface.
            //
            // ⚠ A SEPARATE DRAW RATHER THAN A BRIGHTER FILL, because the fill
            // is what the glyph sits on and lifting all of it costs contrast on
            // exactly the themes TileEdge already fights for. One line moves no
            // composite worth speaking of.
            const ImVec4 meterSurface{ 0.78f, 0.66f, 0.98f, 0.92f };
            const bool   hasMeter = a_e.meter >= 0.0f;
            const float  frac     = hasMeter ? std::clamp(a_e.meter, 0.0f, 1.0f) : 0.0f;
            if (hasMeter) {
                // The empty part, over the whole tile and in the tile's own
                // shape, so an empty stone is legible as EMPTY rather than as a
                // button that forgot to draw anything.
                if (plain) {
                    FUCK::DrawRectFilled(cMin, cMax, meterTrough, plainR);
                } else if (fillTex) {
                    ChamferPanel::FillImage(fillTex, cMin, cMax, meterTrough, artCut);
                } else {
                    ChamferPanel::Fill(cMin, cMax, meterTrough, cut);
                }
                // ⚠ THE TWO PATHS SPEAK DIFFERENT UNITS and MeterPolicy speaks
                // final pixels, so the tile's corner is resolved to final
                // pixels on the way in: artCut already is, `cut` is not. The
                // floor differs too - FillImage will not go below a pixel and
                // ChamferPolicy has no floor at all - so each path says which
                // it is rather than the policy assuming one.
                //
                // ⚠ THE PLAIN PATH GOES THROUGH THE SAME POLICY AND FOR THE SAME
                // REASON. A rounded plate cuts its bottom corners too, so a
                // full-width fill under a short one would show violet outside
                // the tile exactly as it did on the carved shape. Only the
                // DRAWING differs; the arithmetic is the arithmetic.
                const float plateCorner =
                    plain ? plainR : (fillTex ? artCut : FUCK::Scale(cut));
                const auto fill3 = MeterPolicy::Build(cMin.x, cMin.y, cMax.x, cMax.y, frac,
                                                      plateCorner,
                                                      (!plain && fillTex) ? 1.0f : 0.0f);
                if (fill3.draw) {
                    const ImVec2 fMin{ fill3.minX, fill3.minY };
                    const ImVec2 fMax{ fill3.maxX, fill3.maxY };
                    if (plain) {
                        // ⚠ ALL FOUR CORNERS, because the ABI's rounded rect
                        // takes no corner mask. A partial fill is a lozenge
                        // rather than a flat-topped band, which is the rounded
                        // idiom's own answer and is why the carved path keeps
                        // its square top instead of copying this.
                        FUCK::DrawRectFilled(fMin, fMax, meterFill, fill3.corner);
                    } else if (fillTex) {
                        ChamferPanel::FillImage(fillTex, fMin, fMax, meterFill,
                                                fill3.corner, fill3.corners);
                    } else {
                        // Fill scales what it is given, so the corner goes back
                        // to unscaled units on the way in. FUCK::Scale is a
                        // multiply by GetResolutionScale and nothing else, so
                        // dividing by it is the exact inverse.
                        const float res = std::max(0.01f, FUCK::GetResolutionScale());
                        ChamferPanel::Fill(fMin, fMax, meterFill, fill3.corner / res,
                                           fill3.corners);
                    }

                    // The surface, on the fill's own top edge.
                    //
                    // ⚠ THE TOP OF THE FILL IS SQUARE ON EVERY PATH, which is
                    // what lets one flat rect sit on it. MeterPolicy cuts the
                    // BOTTOM corners to follow the tile and leaves the top
                    // alone, so fMin.x to fMax.x is exactly the shape's width
                    // there. Nothing here has to know which of the three draw
                    // paths ran.
                    //
                    // ⚠ SKIPPED WHEN THE FILL IS BARELY TALLER THAN THE LINE.
                    // At a sliver of charge the surface IS the fill, and
                    // drawing both puts a bright bar where the player should be
                    // reading "almost empty". Hidden rather than clamped: a
                    // line thinner than a pixel is a colour, not a line.
                    //
                    // ⚠⚠ IT MUST BE INSET WHERE THE FILL'S TOP IS CUT, AND THE
                    // FIRST VERSION WAS NOT. Drawn full width with a square
                    // corner, this line is the one thing on the tile that does
                    // not follow the silhouette, so as soon as the level
                    // reached the carved band the LINE hung outside the button
                    // while the fill under it sat correctly inside. Reported as
                    // the top leaking on the carved style, and it survived the
                    // corners fix in MeterPolicy because it was never the fill
                    // that was leaking (user, 2026-08-29).
                    //
                    // ⚠ THE INSET AT THE TOP EDGE IS THE FILL'S OWN CORNER.
                    // MeterPolicy hands back kAll exactly when the top pair is
                    // cut, and at depth 0 a cut of c has moved the edge in by
                    // c. Using that one number for the line's whole height
                    // leaves it a hair narrow further down rather than wide,
                    // and narrow cannot leak.
                    const float surfaceH = std::max(1.0f, FUCK::Scale(1.5f));
                    const float lineInset =
                        (fill3.corners == MTB::ChamferPolicy::kAll) ? fill3.corner : 0.0f;
                    const float lineMinX = fill3.minX + lineInset;
                    const float lineMaxX = fill3.maxX - lineInset;
                    if (fill3.maxY - fill3.minY > surfaceH * 2.0f &&
                        lineMaxX > lineMinX) {
                        FUCK::DrawRectFilled({ lineMinX, fill3.minY },
                                             { lineMaxX, fill3.minY + surfaceH },
                                             meterSurface, 0.0f);
                    }
                }
            }

            // What the edge and the symbol are actually going to sit on, which
            // is the composite and not either colour on its own. Computing it
            // rather than assuming is the difference between a contrast rule
            // that holds on every theme and one that holds on the theme it was
            // written against.
            //
            // ⚠ THE METER IS PART OF THAT SURFACE NOW, and it was not when the
            // meter was a small bar in the middle of the tile. A full stone
            // covers the whole tile in violet, which moves the composite far
            // enough to matter to a theme that was already close to the
            // separation floor. Blended by the fraction rather than by alpha:
            // the tile really is two surfaces at once, and what the edge and
            // the glyph meet is how much of each there is.
            //
            // ⚠ THE LOG LINE STILL GETS THE THEME'S OWN NUMBER. It is a theme
            // diagnostic and prints once, so feeding it a value that depends on
            // how full the player's soul gem happened to be would make two runs
            // of the same theme disagree.
            const ImVec4 tileSurface = Over(fill, backdrop);
            const float  baseLum     = Luminance(tileSurface);
            float        tileLum     = baseLum;
            if (hasMeter) {
                const ImVec4 emptySurface = Over(meterTrough, tileSurface);
                const float  emptyLum     = Luminance(emptySurface);
                const float  fullLum      = Luminance(Over(meterFill, emptySurface));
                tileLum                   = emptyLum + (fullLum - emptyLum) * frac;
            }
            LogThemeOnce(backdrop, fill, baseLum);

            // The frame, on the same rect the fill used. Stroke walks each pass
            // half a pixel inside the one before it, the cut included, which is
            // the stacked one-pixel arrangement this block used to build by
            // hand and for the same reason: a thick mitred outline is
            // anti-aliased along its joints, so its apparent width differs
            // between the straights and the 45 degree cuts by construction, and
            // a chamfer is nothing but joints.
            const ImVec4 edgeColour = TileEdge(hovered, held, tileLum);
            if (plain) {
                // ⚠ THE SAME RADIUS THE FILL USED. An outline drawn at a
                // different radius than its fill retreats out of its own corners
                // and the game shows through beside the line, which is the exact
                // fault the art path's ArtCorner note exists to prevent.
                FUCK::DrawRect(cMin, cMax, edgeColour, plainR, kEdgeWidth);
            } else if (const auto frameTex = FrameArt::Frame()) {
                // ⚠ THE SAME artCut THE FILL USED, AND THAT IS NOT A TIDINESS
                // POINT. The two clamp differently - a fill may be cut to half
                // the shorter side, a frame only to a quarter - so ArtCorner
                // returns a value both accept. Handing them different numbers
                // cuts the fill deeper than the outline and the game shows
                // through beside the line at each corner.
                ChamferPanel::FrameImage(frameTex, cMin, cMax, edgeColour, artCut);
            } else {
                ChamferPanel::Stroke(cMin, cMax, edgeColour, cut, kEdgeWidth);
            }

            if (a_e.icon.IsLoaded()) {
                // Inset rather than full-bleed, so a square image reads as a
                // symbol on a tile like its glyph siblings, not a sticker.
                // Clear of the bevel, so the picture never sits on the edge.
                const float pad = FUCK::Scale(8.0f);
                FUCK::SetCursorScreenPos(ImVec2{ origin.x + pad, origin.y + pad });
                // Dimmed when refusing, the picture's own counterpart to the
                // glyph's muted colour below. A tint rather than a separate
                // greyed asset, so every registered icon gets it for free and
                // no caller has to ship two files.
                FUCK::DrawImage(a_e.icon.GetID(),
                                ImVec2{ size - 2.0f * pad, size - 2.0f * pad },
                                ImVec2{ 0.0f, 0.0f }, ImVec2{ 1.0f, 1.0f },
                                a_e.disabled ? ImVec4{ 1.0f, 1.0f, 1.0f, kDisabledAlpha }
                                             : ImVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
            } else {
                // ⚠ CALCTEXTSIZE MUST BE INSIDE THE PUSH. It measures the
                // CURRENT font, so measuring at body size and drawing at 1.45x
                // would centre the glyph against the wrong box and knock it
                // off centre by exactly what the scale adds. Fitting Room
                // carries this same warning over its rail tiles.
                auto* font = FUCK::GetFont(FUCK::Font::kRegular);
                // PushFontScaled asks for a multiple of the font's own base
                // size times the host's global scale, so the fraction of the
                // TILE we actually want has to be converted into those terms.
                // Guarded because both terms come from outside: a null font or
                // a zero scale would divide, and a strip with no buttons drawn
                // is a worse failure than a mis-sized glyph.
                const float base = font ? font->LegacySize : 30.0f;
                const float host = FUCK::GetGlobalScale();
                const float denom = base * (host > 0.0f ? host : 1.0f);
                const float glyphScale =
                    denom > 0.0f ? (size * kGlyphFraction) / denom : 1.0f;
                FUCK::PushFontScaled(font, glyphScale);
                const ImVec2 text = FUCK::CalcTextSize(a_e.glyph.c_str());
                // Whole pixels, so the symbol's own stems land on the grid
                // rather than straddling it. x0/y0 are already integers, so
                // only the centring half-difference needs rounding.
                FUCK::SetCursorScreenPos(
                    ImVec2{ std::floor(x0 + ((x1 - x0) - text.x) * 0.5f + 0.5f),
                            std::floor(y0 + ((y1 - y0) - text.y) * 0.5f + 0.5f) });
                ImVec4 glyph = GlyphColour(hovered || held, tileLum);
                if (a_e.disabled) {
                    // ⚠ ALPHA, NOT A FIXED GREY. GlyphColour has already fought
                    // for contrast against whatever this theme painted the tile,
                    // and replacing its answer with a constant would undo that
                    // on exactly the themes that needed it. Fading what it chose
                    // keeps the relationship and only softens it.
                    glyph.w *= kDisabledAlpha;
                }
                FUCK::TextColored(glyph, "%s", a_e.glyph.c_str());
                FUCK::PopFont();
            }
            FUCK::SetCursorScreenPos(after);  // hand the layout back untouched

            // ⚠ OFF pointerOver RATHER THAN hovered, because hovered is forced
            // false while refusing and this is the one thing a refusing tile
            // still owes the player. The reason replaces the label when the
            // owner gave one: "Change your appearance" is not the answer to why
            // nothing happened when it was clicked.
            if (pointerOver) {
                FUCK::SetTooltip(a_e.disabled && !a_e.disabledReason.empty()
                                     ? a_e.disabledReason.c_str()
                                     : a_e.label.c_str());
            }
            return clicked ? a_e.onClick : nullptr;
        }

        void DrawBar() {
            Callback fire = nullptr;
            const char* firedId = nullptr;
            // ⚠ NO TILES IN THE CHARACTER EDITOR, BUT THE WINDOW STILL DRAWS.
            // Every button here either opens something over the editor or is
            // the editor, so the strip has nothing to offer in there and the
            // field asked for it gone. Closing the WINDOW would have been the
            // obvious way and is the wrong one: this window is also the
            // camera's cursor source, the only reading in the codebase
            // measured to track exactly, and killing it would drop the camera
            // onto the unproven fallback in the one menu we just started
            // supporting. Empty, with no decoration and no background, it
            // renders nothing and keeps publishing.
            const bool inEditor = Bubble::IsRaceMenuOpen();
            // The menu the strip is currently drawing over. CurrentMenuName is
            // meaningful for as long as a menu is COUNTED, which covers both the
            // bubbled case and the gated one below - it is written on every
            // counted open and nothing else touches it.
            const std::string& menuNow = Bubble::GetSingleton().CurrentMenuName();
            // ⚠ THE STRIP HAS NEVER DRAWN OUTSIDE A BUBBLE BEFORE THIS. Every
            // frame it has ever drawn was inside a paused, armed menu. A strip
            // over a live unpaused inventory is a genuinely new state: FLICK
            // windows do draw there, but this one's input handling reads bubble
            // state in places and none of that has been exercised. One line, the
            // first time it happens per session, so a field log can say whether
            // it did.
            const bool studioDown = StudioIsDown();
            if (studioDown) {
                static bool s_loggedUngated = false;
                if (!s_loggedUngated) {
                    s_loggedUngated = true;
                    spdlog::info("action bar: FIRST UNGATED FRAME, drawing over {} with "
                                 "the studio held down, offering only buttons other mods "
                                 "registered. This state is new; if input misbehaves on "
                                 "the strip, start here.",
                                 menuNow);
                }
            }
            for (auto& e : g_entries) {
                // Switched off in the panel. Skipped at DRAW time rather than
                // dropped at registration, so ticking one back on brings it
                // straight back without the owning mod re-registering - and a
                // mod that never notices the setting exists still behaves.
                //
                // The menu scope is checked the same way and for the same
                // reason: an outfit editor has nothing to offer over a
                // merchant's stock, but walking into the barter menu must not
                // cost the button its registration. e.hidden is the third
                // voice - the OWNER's, through SetVisible - and skipping at
                // draw time keeps all three reversible for free.
                //
                // The fourth voice is the owner-context gate, and it is not a
                // per-button setting at all: while the studio is down every tile
                // Menu Studio registered for itself controls something that is
                // not running, so only what another mod put here is offered.
                if (inEditor || !WouldDraw(e, menuNow, studioDown)) {
                    continue;
                }
                if (const Callback hit = DrawTile(e); hit && !fire) {
                    fire = hit;
                    firedId = e.id.c_str();
                }
                FUCK::Dummy(ImVec2{ 0.0f, FUCK::Scale(kTileGap) });
            }
            if (fire) {
                // Logged before the call, not after. If a caller's handler
                // throws or hangs, the line that says which button did it has
                // to already be on disk.
                spdlog::info("action bar: '{}' clicked.", firedId);
                fire();
            }

            // ⚠ AFTER THE CALLBACK, so the gear's OpenPopup and this BeginPopup
            // land in the same frame and the panel appears on the click rather
            // than one frame later.
            //
            // The scroll box is sized in FONT UNITS rather than pixels, the way
            // Fitting Room's gear popup is, so the panel keeps its proportions
            // at every UI scale instead of clipping its own contents at one.
            // ⚠ THE PADDING HAS TO GO ON THE POPUP, NOT THE CHILD, and the
            // field caught the difference ("the side can cut out very
            // slightly, there's no padding"). ImGui ZEROES WindowPadding.x on
            // a child with no border - Fitting Room's own rail sizing carries
            // the same note - so the scroll box below cannot hold a left
            // margin of its own however it is styled. Padding the popup insets
            // the child instead, which puts the gap where it was wanted.
            //
            // Popped straight after BeginPopup: Begin has already copied the
            // value onto the window by then, so the child underneath keeps
            // FLICK's own metrics and the inset is applied exactly once.
            const float pad = FUCK::Scale(12.0f);
            FUCK::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ pad, pad });
            const bool settingsOpen = FUCK::BeginPopup(SettingsUI::kPopupId);
            FUCK::PopStyleVar();
            if (settingsOpen) {
                const float em = FUCK::GetTextLineHeight() > 0.0f
                                     ? FUCK::GetTextLineHeight()
                                     : FUCK::Scale(18.0f);
                FUCK::BeginChild("##ms_settings_scroll",
                                 ImVec2{ em * 30.0f, em * 26.0f });
                SettingsUI::DrawPanelBody();
                FUCK::EndChild();
                FUCK::EndPopup();
            }
        }

        class BarWindow : public FUCK::IWindow {
        public:
            const char* Id() const override { return "MS_ActionBar"; }
            const char* Title() const override { return "Menu Studio"; }

            bool IsOpen() const override {
                if (!Settings::GetSingleton().actionBar || g_entries.empty()) {
                    return false;
                }
                // ⚠ IsBubbleVisible, NOT IsBubbleActive, AND THE DIFFERENCE IS
                // HALF A SECOND ON SCREEN. Both hold their condition true for a
                // few frames after a menu closes so a menu SWITCH does not tear
                // anything down in the gap, but the gate hold is 30 frames
                // (~0.5 s) for the camera and the visual grace is 6 (~0.1 s).
                // The strip is a picture and was reading the camera's number:
                // "MS icons take a second to disappear when we exit out the
                // menu, it needs to be instant" (user 2026-08-14).
                if (Bubble::IsBubbleVisible()) {
                    return true;
                }
                // The gate is holding the studio down over a menu that IS open.
                // Draw on the menu alone - but only if there is actually
                // something another mod put here to offer. If nothing external
                // is visible in this menu, draw nothing: a bare frame over a
                // vanilla inventory is worse than no frame at all.
                return StudioIsDown() && AnythingToDraw(true);
            }
            void SetOpen(bool) override {}

            void Draw() override {
                // Placed every frame, not once on appear. kCustomPosition opts
                // out of the host's own position saving, so GetDefaultPos is
                // consulted only when the window first shows itself and the
                // panel sliders would otherwise do nothing until a restart.
                // See [[fuck-window-geometry-persistence]].
                FUCK::SetWindowPos(GetDefaultPos(), ImGuiCond_Always);
                DrawBar();
                g_win.lastPos = FUCK::GetWindowPos();
                g_win.lastSize = FUCK::GetWindowSize();
                if (const ImVec2 display = FUCK::GetDisplaySize();
                    display.x > 0.0f && display.y > 0.0f) {
                    // A small margin, so a drag started right against the edge
                    // of a button is not stolen by a one-pixel miss.
                    const float pad = FUCK::Scale(4.0f);
                    g_win.uMin = (g_win.lastPos.x - pad) / display.x;
                    g_win.vMin = (g_win.lastPos.y - pad) / display.y;
                    g_win.uMax = (g_win.lastPos.x + g_win.lastSize.x + pad) / display.x;
                    g_win.vMax = (g_win.lastPos.y + g_win.lastSize.y + pad) / display.y;
                    g_win.onScreen = true;

                    const ImVec2 mouse = FUCK::GetMousePos();
                    g_win.cursorU = mouse.x / display.x;
                    g_win.cursorV = mouse.y / display.y;
                    g_win.haveCursor = true;
                    g_win.dispW = display.x;
                    g_win.dispH = display.y;

                    // ⚠ ASKED HERE BECAUSE HERE IS THE ONLY PLACE IT CAN BE.
                    // This reads ImGui state and is valid only inside the FLICK
                    // render pass; the camera's gate runs in the input sink,
                    // which is not that pass.
                    //
                    // ⚠ 4 IS AnyWindow. 2 IS RootWindow, AND ASKING FOR IT
                    // ALONGSIDE IsAnyItemActive COST A FIELD ROUND. Those item
                    // queries are about widget FOCUS rather than where the
                    // pointer is, and Fitting Room's editor holds an active item
                    // for as long as it is open, so every camera drag and every
                    // wheel notch was refused while it was on screen. The log
                    // read "notches seen 0, drags refused 146". The only
                    // question worth asking here is whether the pointer is over
                    // a window, and AnyWindow is the flag that asks it.
                    g_win.overAnyWindow = FUCK::IsWindowHovered(4);
                    g_win.overAnyWindowAt = NowSeconds();
                }
            }

            // kCloseOnGameMenu is absent deliberately: it HIDES a window when
            // native menus open, and those menus are the only time this exists.
            FUCK::WindowFlags GetFlags() const override {
                auto flags = FUCK::WindowFlags::kNoDecoration |
                             FUCK::WindowFlags::kNoBackground |
                             FUCK::WindowFlags::kAutoResize |
                             FUCK::WindowFlags::kNoResize |
                             FUCK::WindowFlags::kCustomPosition |
                             FUCK::WindowFlags::kNoMove;
                const ImVec2 mouse = FUCK::GetMousePos();
                const bool over = mouse.x >= g_win.lastPos.x &&
                                  mouse.x <= g_win.lastPos.x + g_win.lastSize.x &&
                                  mouse.y >= g_win.lastPos.y &&
                                  mouse.y <= g_win.lastPos.y + g_win.lastSize.y;
                // Hand the mouse back whenever it is not on the strip, so the
                // menu underneath keeps working and the camera layer can read
                // the drag. Racemenu Enhancer gates its windows the same way.
                if (!over && !FUCK::IsAnyItemActive() &&
                    !FUCK::IsPopupOpen(nullptr, FUCK::PopupFlags::kAnyPopup)) {
                    flags = flags | FUCK::WindowFlags::kPassInputToGame;
                }
                return flags;
            }

            ImVec2 GetDefaultPos() const override {
                const auto&  cfg = Settings::GetSingleton();
                const ImVec2 display = FUCK::GetDisplaySize();
                return { (std::max)(0.0f, display.x * cfg.actionBarX),
                         (std::max)(0.0f, display.y * cfg.actionBarY) };
            }
        };

        BarWindow g_window;  // process lifetime; the registered pointer stays valid

        [[nodiscard]] std::vector<Entry>::iterator Find(const char* a_id) {
            return std::find_if(g_entries.begin(), g_entries.end(),
                                [a_id](const Entry& e) { return e.id == a_id; });
        }

        // Menu Studio's own buttons. The gear opens the settings in a window of
        // their own, beside the menu being styled - routing it through FLICK's
        // overlay buried that menu under the sidebar chrome. (A recentre tile
        // lived here once; the middle click does that job now, without asking
        // the hand to leave the shot it is holding.)
        // ⚠ A POPUP, WHICH IS WHY THE ANCHOR ARITHMETIC IS GONE. This used to
        // open a registered window and hand it the tile's rectangle so it could
        // place itself beside the button. A popup opens where it is asked for
        // and sizes itself to its contents, so the anchor, the placement, the
        // saved-geometry fight and the clipped-empty-panel that came of moving
        // a window mid-append all stop being problems rather than being solved.
        //
        // Drawn by the bar, a few lines below the tile loop, because OpenPopup
        // and BeginPopup have to meet in the same ImGui context and the gear
        // lives here.
        void OpenSettings() { FUCK::OpenPopup(SettingsUI::kPopupId); }

        // The character editor.
        //
        // ⚠ THIS WAS A CONSOLE COMMAND AND IT CRASHED THE GAME. The first cut
        // ran `showracemenu` through RE::Script::CompileAndRun, on the
        // reasoning that the console command is the engine's own entry point
        // and must carry setup a bare UI message would miss. Both halves of
        // that were wrong, and the second one is the dangerous one:
        //
        //   CommonLib's AE id for Script::CompileAndRun is 21890, and 21890 is
        //   ABSENT from the 1.6.1170 Address Library. id2offset does a
        //   lower_bound and only fails when an id runs past the END of the
        //   database, so an absent id silently returns its NEIGHBOUR's
        //   address - here 21891 - and the call lands in an unrelated
        //   function. The field crash log names it exactly:
        //   SkyrimSE.exe+0x33D8D9 is 21891+0x59, reached from MenuStudio.dll.
        //   This is the trap VersionCheck.cpp's own header warns about, and
        //   the guard below is that warning finally being obeyed here.
        //
        // And the first half was wrong too, which the binary settles. The
        // vanilla ShowRaceMenu console handler (AE RVA 0x355960) with no
        // argument calls exactly one parameterless function, id 52366, whose
        // entire body is:
        //
        //     UIMessageQueue::AddMessage(singleton,
        //                                InterfaceStrings + 0x160, kShow, 0)
        //
        // InterfaceStrings + 0x160 IS raceSexMenu. So the console command is
        // this message and nothing else, there is no extra setup to miss, and
        // what follows is the same call with its own singletons.
        //
        // ⚠ TWO FRAMES, AND BOTH HOPS ARE LOAD-BEARING. The click lands on
        // FLICK's draw pass rather than the main thread, so the first task
        // moves the work. The editor also cannot open on top of the inventory
        // that is still up, so that task closes the menu and the SECOND one
        // opens the editor a frame later, once the close has been pumped.
        // Defined below with the wait it arms. Forward-declared because the
        // button that owes the open is written above the machinery that pays
        // it, and moving either would separate this comment from its subject.
        void ArmPendingEditorOpen();

        void OpenRaceMenu() {
            // ⚠ THE HOUSE RULE, APPLIED. Every engine address this mod calls
            // is proven present in the running database first, because the
            // failure mode when it is not is a silent wrong call rather than
            // an error. AddMessage carries both steps below.
            if (!VersionCheck::IdOk(Offsets::UIMessageQueueAddMessage)) {
                spdlog::warn("action bar: UIMessageQueue::AddMessage is not in this "
                             "runtime's Address Library, so the character editor "
                             "button stands down rather than calling a neighbouring "
                             "function.");
                return;
            }
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }
            tasks->AddTask([] {
                if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                    const auto& menu = Bubble::GetSingleton().CurrentMenuName();
                    if (!menu.empty()) {
                        queue->AddMessage(menu, RE::UI_MESSAGE_TYPE::kForceHide,
                                          nullptr);
                    }
                    // ⚠ ARM THE WAIT HERE, NOT A SECOND TASK. See
                    // PendingEditorOpen: the open is owed until the game is
                    // actually back in gameplay.
                    ArmPendingEditorOpen();
                    // ⚠ THE TWEEN WRAPPER GOES TOO. Pressing I opens the
                    // inventory INSIDE the TweenMenu, and hiding only the
                    // inventory leaves that wrapper open underneath the whole
                    // editor session. The 2026-08-04 23:32 field log convicted
                    // it by name: 'menus at close - 15 open: TweenMenu, ...',
                    // TWEEN=true on every frame of the strand - an invisible
                    // tween is what the editor's close handed back. HUD faded
                    // by it, its cursor up, and the camera never handed back,
                    // until the player found their own way out with Tab. The
                    // engine's Tab-out closes both together; so do we.
                    // Conditional, because not every covered menu arrives
                    // through the tween (containers and barter do not).
                    if (auto* ui = RE::UI::GetSingleton();
                        ui && ui->IsMenuOpen(RE::TweenMenu::MENU_NAME)) {
                        queue->AddMessage(RE::TweenMenu::MENU_NAME,
                                          RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                    }
                }
            });
        }

        // ⚠ THE EDITOR IS OPENED FROM GAMEPLAY, THE WAY THE CONSOLE DOES IT
        // (2026-08-06, the player's call after everything else failed).
        //
        // The old path posted kForceHide and then kShow on the very next
        // task: the menus never finished closing, every close handler in the
        // load order was skipped or ran underneath a menu that was already
        // being replaced, and gameplay never resumed for a single frame. The
        // console door does none of that, and the console door is the one
        // where RaceMenu's head drag works - through a morning where every
        // difference we could name inside the menu was eliminated by log
        // line. So stop being a different door: close down, let the world
        // come back and every mod run its own restore, THEN open.
        //
        // Nothing is lost by it. Leaving the editor puts the player in
        // gameplay either way - it never returned to the inventory - so the
        // only change the player can see is the moment of world in between.
        // The owed open, and how long it has been owed. Both live here rather
        // than in a chain of tasks because the wait is measured in FRAMES OF
        // THE WORLD RUNNING, which a task chain cannot express: two queued
        // tasks run back to back on consecutive frames whether or not a single
        // menu has actually closed.
        bool          g_editorOpenPending = false;
        std::uint32_t g_editorOpenFrames = 0;
        std::uint32_t g_editorSettleFrames = 0;

        // How long to let the world run before opening. Three frames is not a
        // magic number, it is "more than one": the close handlers that matter
        // here are other mods' menu-close sinks, and a sink that restores on
        // the frame AFTER the close would still be mid-restore at one.
        constexpr std::uint32_t kEditorSettleFrames = 3;
        // And a ceiling, because a wait with no end is a button that silently
        // does nothing. If the menus will not close, open anyway and say so -
        // the old behaviour, which at least gets the player into the editor.
        constexpr std::uint32_t kEditorWaitCap = 90;

        void ArmPendingEditorOpen() {
            g_editorOpenPending = true;
            g_editorOpenFrames = 0;
            g_editorSettleFrames = 0;
        }

        // Is the game back to being the game? Not "is our bubble down" - the
        // question is whether every menu that was covering the world has gone
        // and handed its own state back, which is exactly the condition the
        // console door enjoys and ours never had.
        [[nodiscard]] bool BackInGameplay() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return true;  // cannot tell; do not hang the button on it
            }
            return !ui->GameIsPaused() &&
                   !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) &&
                   !ui->IsMenuOpen(RE::TweenMenu::MENU_NAME) &&
                   !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) &&
                   !ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) &&
                   !ui->IsMenuOpen(RE::MagicMenu::MENU_NAME);
        }

        void FireEditorOpen() {
            auto* queue = RE::UIMessageQueue::GetSingleton();
            auto* strings = RE::InterfaceStrings::GetSingleton();
            if (!queue || !strings) {
                return;
            }
            // LIMITED, LIKE THE FACE SCULPTOR. A strip button in the middle
            // of an inventory is an appearance tool, not a race change -
            // that is a rare, deliberate act with quest and save
            // consequences, and the field asked for exactly this split ("so
            // we can't alter the race", 2026-08-05). The engine has a switch
            // for it: Game.ShowLimitedRaceMenu()'s entire body is this byte
            // plus the message below, and the menu's own code resets the
            // byte after use.
            //
            // AE only, and through the same proof the AddMessage call takes.
            // The SE id is pattern-matched rather than decompile-proven, and
            // a WRITE through a wrong id corrupts a stranger's global
            // silently - see the note in Offsets.h. SE gets the full editor
            // and a line saying why.
            if (!Settings::GetSingleton().limitedEditor) {
                spdlog::info("action bar: character editor opening FULL "
                             "(bLimitedEditor is off). Race and sex tabs "
                             "included, the same mode showracemenu opens.");
            } else if (REL::Module::IsAE() &&
                       VersionCheck::IdOk(Offsets::RaceMenuLimitedFlag)) {
                REL::Relocation<std::uint8_t*> flag{ Offsets::RaceMenuLimitedFlag };
                *flag.get() = 1;
                spdlog::info("action bar: character editor opening LIMITED "
                             "(appearance only, no race or sex). The engine's "
                             "own sculptor mode.");
            } else {
                spdlog::info("action bar: limited-mode flag not proven on this "
                             "runtime. The editor opens FULL, race tab included.");
            }
            // The engine's own string rather than a literal, which is what id
            // 52366 reads and what keeps this correct if the menu is ever
            // renamed under us.
            queue->AddMessage(strings->raceSexMenu, RE::UI_MESSAGE_TYPE::kShow,
                              nullptr);
        }

    }  // namespace

    void Tick() {
        if (!g_editorOpenPending) {
            return;
        }
        ++g_editorOpenFrames;
        if (!BackInGameplay()) {
            if (g_editorOpenFrames < kEditorWaitCap) {
                return;  // still coming down
            }
            // ⚠ OPENS ANYWAY, LOUDLY. Something in this load order will not
            // let go of the menu stack, and a button that quietly does
            // nothing is worse than one that opens the old way - but the
            // line has to be here, because from the player's side this
            // failure and the fixed path look identical until the head does
            // not move.
            spdlog::warn("action bar: waited {} frames and the menus never fully "
                         "closed, so the editor opens from inside them, the old "
                         "path. If the head drag is dead in there, this line is why.",
                         g_editorOpenFrames);
            g_editorOpenPending = false;
            FireEditorOpen();
            return;
        }
        if (g_editorSettleFrames < kEditorSettleFrames) {
            ++g_editorSettleFrames;
            return;  // let the world run a moment; other mods restore here
        }
        g_editorOpenPending = false;
        spdlog::info("action bar: the menus are down and the world ran {} frames, "
                     "so the character editor opens from GAMEPLAY, the same door "
                     "showracemenu uses.",
                     g_editorSettleFrames);
        FireEditorOpen();
    }

    bool Register(const char* a_id, const char* a_label, const char* a_iconPath,
                  Callback a_onClick, bool a_external) {
        if (!a_id || !*a_id || !a_onClick) {
            spdlog::warn("action bar: a registration was refused (id or callback "
                         "missing). Nothing was added.");
            return false;
        }
        const std::string label = (a_label && *a_label) ? a_label : a_id;

        // What the icon argument is. A dot or a slash makes it a PATH to an
        // image file; empty falls back to a lettered tile; anything else is
        // drawn VERBATIM as the tile's symbol - the route for a Font Awesome
        // glyph out of FLICK's baked atlas, which is how the built-in buttons
        // get their gear and circle-arrow and how Fitting Room sends a shirt.
        const std::string icon = a_iconPath ? a_iconPath : "";
        const bool isPath = icon.find('.') != std::string::npos ||
                            icon.find('/') != std::string::npos ||
                            icon.find('\\') != std::string::npos;
        const std::string imagePath = isPath ? icon : "";
        const std::string glyph =
            (!icon.empty() && !isPath) ? icon : GlyphFor(label);

        if (const auto it = Find(a_id); it != g_entries.end()) {
            // Replaced in place. A caller re-registering after a settings change
            // wants its button updated, not a second one beside the first, and
            // moving it to the end would shuffle the strip under the player.
            it->label = label;
            it->glyph = glyph;
            if (imagePath != it->iconPath) {
                it->iconPath = imagePath;
                it->icon.Reset();
                it->iconTried = false;
            }
            it->onClick = a_onClick;
            // Sticky, like the scope and the owner's own visibility: a caller
            // re-registering after its own settings change is updating the
            // button, not re-declaring where it came from.
            it->external = it->external || a_external;
            spdlog::info("action bar: '{}' re-registered as '{}'.", a_id, label);
            return true;
        }
        Entry e;
        e.id = a_id;
        e.label = label;
        e.iconPath = imagePath;
        e.glyph = glyph;
        e.onClick = a_onClick;
        e.external = a_external;
        g_entries.push_back(std::move(e));
        spdlog::info("action bar: '{}' registered as '{}' ({}, {} buttons).", a_id, label,
                     a_external ? "from another mod" : "Menu Studio's own",
                     g_entries.size());
        return true;
    }

    bool SetMenus(const char* a_id, const char* a_menus) {
        if (!a_id || !*a_id) {
            return false;
        }
        const auto it = Find(a_id);
        if (it == g_entries.end()) {
            spdlog::warn("action bar: menu scope for '{}' ignored: no such button. "
                         "Register it first.", a_id);
            return false;
        }
        it->menus = SplitMenus(a_menus);
        if (it->menus.empty()) {
            spdlog::info("action bar: '{}' shows in every menu.", a_id);
            return true;
        }
        std::string joined;
        for (const auto& m : it->menus) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += m;
        }
        spdlog::info("action bar: '{}' shows in {} only.", a_id, joined);
        return true;
    }

    bool SetVisible(const char* a_id, bool a_visible) {
        if (!a_id || !*a_id) {
            return false;
        }
        const auto it = Find(a_id);
        if (it == g_entries.end()) {
            spdlog::warn("action bar: visibility for '{}' ignored: no such "
                         "button. Register it first.",
                         a_id);
            return false;
        }
        if (it->hidden != !a_visible) {
            it->hidden = !a_visible;
            // Logged only on a real change: a window asserting its state on
            // every draw would otherwise write this line at the frame rate.
            spdlog::info("action bar: '{}' {} by its owner.", a_id,
                         a_visible ? "shown" : "hidden");
        }
        return true;
    }

    bool SetMeter(const char* a_id, float a_fraction) {
        if (!a_id || !*a_id) {
            return false;
        }
        const auto it = Find(a_id);
        if (it == g_entries.end()) {
            return false;
        }
        // ⚠ NEGATIVE CLEARS, and it is not the same as zero. Zero is "the stone
        // is empty", which is a fact worth drawing; negative is "I do not have
        // a meter", which must leave the tile exactly as it was before anybody
        // asked. Collapsing the two would put an empty bar on every button that
        // ever calls this and then stops.
        const float want = a_fraction < 0.0f ? -1.0f : std::clamp(a_fraction, 0.0f, 1.0f);
        // ⚠ NO CHANGE-ONLY LOG LINE HERE, unlike its two neighbours. A charge
        // meter moves continuously, so logging each change would write a line
        // per soul gem and then a line per styling charge spent, which buries
        // everything else this file says. The value is on screen; that is the
        // report.
        it->meter = want;
        return true;
    }

    bool SetEnabled(const char* a_id, bool a_enabled, const char* a_reason) {
        if (!a_id || !*a_id) {
            return false;
        }
        const auto it = Find(a_id);
        if (it == g_entries.end()) {
            spdlog::warn("action bar: enablement for '{}' ignored: no such "
                         "button. Register it first.",
                         a_id);
            return false;
        }
        const std::string reason = a_reason ? a_reason : "";
        // Same change-only logging as SetVisible above, and for the same
        // reason: a caller re-asserting this every frame is the expected
        // shape, since the condition it reflects can move at any time.
        if (it->disabled != !a_enabled || it->disabledReason != reason) {
            it->disabled       = !a_enabled;
            it->disabledReason = reason;
            spdlog::info("action bar: '{}' {} by its owner{}{}.", a_id,
                         a_enabled ? "enabled" : "disabled",
                         (!a_enabled && !reason.empty()) ? ": " : "",
                         (!a_enabled && !reason.empty()) ? reason : "");
        }
        return true;
    }

    bool Unregister(const char* a_id) {
        if (!a_id || !*a_id) {
            return false;
        }
        const auto it = Find(a_id);
        if (it == g_entries.end()) {
            return false;
        }
        g_entries.erase(it);
        spdlog::info("action bar: '{}' unregistered ({} buttons left).", a_id,
                     g_entries.size());
        return true;
    }

    std::size_t Count() { return g_entries.size(); }

    std::vector<Info> Snapshot() {
        std::vector<Info> out;
        out.reserve(g_entries.size());
        for (const auto& e : g_entries) {
            out.push_back(Info{ e.id, e.label });
        }
        return out;
    }

    bool CursorOverBar(float a_u, float a_v) {
        // The window stops drawing when the bubble disarms, and the last
        // published rectangle would otherwise keep refusing drags in a region
        // with nothing in it.
        // The window stops drawing when the bubble disarms, and the last
        // published rectangle would otherwise keep refusing drags in a region
        // with nothing in it. StudioIsDown() joins the test because the strip
        // does draw there now, and a drag beginning on one of its buttons still
        // belongs to the button.
        if (!g_win.onScreen || !Settings::GetSingleton().actionBar ||
            g_entries.empty() ||
            (!Bubble::IsBubbleActive() && !StudioIsDown())) {
            return false;
        }
        // Any FLICK window counts, not only the strip. Fitting Room's editor
        // sits inside the camera's drag region and a drag must not start on it.
        // Only honoured while the reading is fresh: a stale true would refuse
        // every drag in a menu the bar has not started drawing in yet.
        if (g_win.overAnyWindow &&
            NowSeconds() - g_win.overAnyWindowAt <= kPublishFreshness) {
            return true;
        }
        return a_u >= g_win.uMin && a_u <= g_win.uMax && a_v >= g_win.vMin &&
               a_v <= g_win.vMax;
    }

    bool PublishedDisplay(float& a_outW, float& a_outH) {
        if (g_win.dispW <= 0.0f || g_win.dispH <= 0.0f) {
            return false;
        }
        a_outW = g_win.dispW;
        a_outH = g_win.dispH;
        return true;
    }

    bool PublishedCursor(float& a_outU, float& a_outV) {
        if (!g_win.haveCursor ||
            (!Bubble::IsBubbleActive() && !StudioIsDown())) {
            return false;
        }
        a_outU = g_win.cursorU;
        a_outV = g_win.cursorV;
        return true;
    }

    void Install() {
        FUCK::RegisterWindow(&g_window);
        Register("MenuStudio.RaceMenu", "Change your appearance",
                 Utf8(kGlyphFace).c_str(), &OpenRaceMenu);
        // The appearance door belongs to the styling context, not to a
        // merchant's stock, a chest, or a bare inventory. The menu scope is
        // the outer fence; the visibility below is the real gate.
        SetMenus("MenuStudio.RaceMenu", "InventoryMenu");
        // Born hidden (2026-08-06, the author's call: "it has to be in
        // Fitting Room only"). Fitting Room reveals it on its editor
        // window's open edge through MenuStudio_SetActionVisible and hides
        // it again on close, so the button exists exactly while the styling
        // context does. Without Fitting Room installed nothing reveals it -
        // that is the intended shape, not an accident, and this line is
        // where to flip it if that call ever changes.
        SetVisible("MenuStudio.RaceMenu", false);
        Register("MenuStudio.Settings", "Menu Studio settings",
                 Utf8(kGlyphGear).c_str(), &OpenSettings);
        spdlog::info("action bar: window registered. Other plugins add buttons "
                     "through MenuStudio_RegisterAction.");
    }

}  // namespace MTB::ActionBar

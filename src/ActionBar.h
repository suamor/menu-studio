#pragma once

#include <cstddef>
#include <string>
#include <vector>

// A strip of buttons drawn over a bubbled menu, for the things a player wants
// to reach while looking at their character.
//
// Menu Studio owns the strip and almost none of its contents. Any SKSE plugin
// registers a button through the C ABI in PublicApi.cpp and supplies its own
// callback, so Fitting Room puts an editor button here without Menu Studio
// learning what Fitting Room is. That direction matters: Fitting Room already
// consumes this mod's exports, and a dependency pointing back the other way
// would make neither installable alone.
//
// The bar is a FUCK::IWindow rather than an ITool because the FLICK sidebar
// lists tools only, and this has to draw on top of a Skyrim menu. Whether that
// works at all was measured before any of this was written, with a probe
// that has since been deleted; the findings are in the 2026-08-04 design doc.
namespace MTB::ActionBar {

    // No arguments and no return. A button that needs context should capture it
    // in the registering plugin, since anything passed through here would have
    // to be ABI-stable forever.
    using Callback = void (*)();

    // Adds a button, or replaces the one already under a_id while keeping its
    // place in the strip. False means a_id or a_onClick was null or empty.
    //
    // a_iconPath is optional and relative to the game's Data folder. When it is
    // empty or fails to load, the button falls back to a lettered tile, so a
    // missing texture costs the look and never the function.
    //
    // a_external marks an entry that came in through MenuStudio_RegisterAction
    // rather than from Install(). It decides what the strip may offer while the
    // owner-context gate holds the studio down: the strip has to keep drawing in
    // that state or it would hide the very button that lifts the gate, but every
    // tile Menu Studio registers for ITSELF controls a studio that is not
    // running - the settings gear opens a panel full of switches for a bubble
    // that is down. So the honest rule is "only what somebody else put here",
    // and it needs no knowledge of which mod that somebody is.
    //
    // ⚠ DEFAULTS TO FALSE SO THE INTERNAL CALLS BELOW STAY CORRECT BY DOING
    // NOTHING. Once an entry is external it stays external across a
    // re-registration, the same way its scope and its owner-visibility do.
    bool Register(const char* a_id, const char* a_label, const char* a_iconPath,
                  Callback a_onClick, bool a_external = false);

    // Limits a button to the named bubbled menus. a_menus is comma separated
    // ("InventoryMenu, MagicMenu"); empty or null puts it back in all of them.
    //
    // EMPTY IS THE DEFAULT AND MEANS EVERYWHERE, so a button that never calls
    // this behaves exactly as buttons did before the call existed. That is what
    // keeps the shipped Fitting Room builds correct.
    //
    // Separate from Register rather than an argument to it, because the two are
    // answering different mods: Register is the button, this is where it
    // belongs. Re-registering (a caller reacting to its own settings change)
    // therefore keeps the scope, the same way it keeps its place in the strip.
    //
    // False when no button is registered under a_id. Register first.
    bool SetMenus(const char* a_id, const char* a_menus);

    // Show or hide a button without touching its registration or its scope.
    // The owner's word for a context finer than a menu name: Fitting Room's
    // editor is a WINDOW over the inventory, and only its owner sees that
    // window open and close - so it says so here, on both edges, and the
    // appearance button exists exactly while the styling context does.
    //
    // Distinct from the settings panel's per-button hide, which is the
    // PLAYER's word and persists in the INI. This one belongs to the owning
    // mod, lives for the session, and survives re-registration the same way
    // the menu scope does.
    //
    // False when no button is registered under a_id yet - load order can put
    // a caller ahead of this strip's own Install, so repeat the call on a
    // later edge rather than assuming it took.
    bool SetVisible(const char* a_id, bool a_visible);

    // Leave a button on the strip but refuse it, with the owner's reason shown
    // on hover in place of the label.
    //
    // ⚠ THE POINT IS THAT HIDING IS THE WRONG ANSWER TO A PRECONDITION. Hiding
    // says "there is nothing here"; a button whose owner knows WHY it cannot be
    // used right now has something better to say, and was previously forced to
    // choose between vanishing and offering something that would not work.
    // Fitting Room's character editor is the case that asked for it: entry has
    // a price, and a player who cannot pay should see the door and be told,
    // rather than watch a button disappear from under them.
    //
    // A disabled tile draws in its resting look, never lighting under the
    // cursor or sinking under a press, with its symbol faded. It still hovers,
    // because the tooltip is the whole point of leaving it there.
    //
    // a_reason may be null or empty, which falls back to the label - correct
    // when the refusal is self-evident and there is nothing to add.
    //
    // The owner's word, like SetVisible: it survives re-registration, and the
    // player's own panel switch is separate and still wins. Safe to call every
    // frame; only a real change is logged.
    //
    // False when no button is registered under a_id yet.
    bool SetEnabled(const char* a_id, bool a_enabled, const char* a_reason);

    // Fill the button's background to show how full the owner's resource is.
    // a_fraction is 0 to 1; anything NEGATIVE clears the meter, which is not
    // the same as passing 0 (that draws an empty bar, which is a fact worth
    // showing). Values outside 0..1 are clamped.
    //
    // ⚠ THE OWNER'S NUMBER AND OUR PICTURE, because neither side can do it
    // alone. Fitting Room's Seamstone holds the charge that pays for styling
    // and a player wants to know whether pressing this will get them anywhere
    // BEFORE they press it; Fitting Room cannot draw on our tile and we do not
    // know what a Seamstone is.
    //
    // Safe to call every frame. Unlike its neighbours this one logs nothing,
    // because a charge meter moves continuously and a line per change would
    // bury the rest of the log.
    //
    // False when no button is registered under a_id yet.
    bool SetMeter(const char* a_id, float a_fraction);

    // False when nothing was registered under a_id, which is the honest answer
    // rather than a silent success.
    bool Unregister(const char* a_id);

    // After FUCK::Connect has succeeded. Registers the window and Menu Studio's
    // own buttons.
    void Install();

    // Once per frame, from Bubble::OnFrame, armed or not.
    //
    // The editor button owes an open: it closes the menus and then waits for
    // the WORLD TO BE BACK before showing the character editor, because the
    // console door - the one where RaceMenu's head drag works - opens from
    // gameplay and ours never did. That wait is counted in frames of the game
    // actually running, which no chain of queued tasks can express.
    //
    // Cheap: one bool test when nothing is owed, which is every frame but a
    // handful per session.
    void Tick();

    [[nodiscard]] std::size_t Count();

    // One registered button, for the settings panel's list.
    struct Info {
        std::string id;
        std::string label;
    };

    // Every registered button in strip order, hidden ones included - the panel
    // has to list a switched-off button to offer switching it back on.
    //
    // A copy rather than a view of the live vector, because a caller walking it
    // draws checkboxes that write settings, and a plugin re-registering during
    // that walk would move the storage under them.
    [[nodiscard]] std::vector<Info> Snapshot();

    // Is a point inside the strip? Coordinates are fractions of the display, so
    // a caller counting in screen pixels or Scaleform stage units can normalise
    // its own and ask without knowing what space the bar draws in.
    //
    // The camera layer needs this: the strip sits inside the region where a
    // camera drag is allowed to start, and a drag beginning on a button belongs
    // to the button. False whenever the bar is not on screen.
    [[nodiscard]] bool CursorOverBar(float a_u, float a_v);

    // The cursor as fractions of the display, republished on every draw.
    //
    // FUCK's own mouse position is the only cursor reading in this codebase
    // measured to track exactly (r = 1.0000 against the screen), but it reads
    // ImGui state and is only safe inside the FLICK render pass. The camera's
    // gate runs in the input sink, which is not that pass, so the bar states
    // what it saw instead of the sink asking.
    //
    // False when nothing has published yet, which is the case when the bar is
    // switched off or has no buttons. Callers need a fallback for that.
    [[nodiscard]] bool PublishedCursor(float& a_outU, float& a_outV);

    // The display extent the bar last drew against, in pixels. Published for the
    // same reason as the cursor: a reader outside the FLICK render pass cannot
    // ask ImGui, and turning another mod's pixel rectangle into cursor space
    // needs this. False until the bar has drawn once.
    [[nodiscard]] bool PublishedDisplay(float& a_outW, float& a_outH);

}  // namespace MTB::ActionBar

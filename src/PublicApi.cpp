#include "PCH.h"

#include "ActionBar.h"
#include "Bubble.h"
#include "Declutter.h"
#include "ItemPreviewBroker.h"
#include "StudioCamera.h"

// Menu Studio's public C ABI, for other SKSE plugins that need a say in what
// the studio shot contains.
//
// A raw DLL export rather than an SKSE message or a Papyrus mod event, for one
// decisive reason: Menu Studio's entire job is freezing the game, and a frozen
// Papyrus VM does not deliver mod events. An SKSE message would work but is
// asynchronous and load-order sensitive, and the caller here wants to set a
// value and know it took. This mirrors FUCK.dll's RequestFUCK export, which is
// the pattern Fitting Room already consumes at runtime with GetModuleHandleW +
// GetProcAddress, so nothing links against anything.
//
// ⚠ MAIN THREAD ONLY. Both functions touch form lookup and declutter state.

namespace {
    // 2 adds the shot-focus pair below. Callers that resolve each symbol by
    // name do not need this - a missing export is already distinguishable -
    // but a caller deciding whether a FEATURE is available reads better
    // against one number than against three GetProcAddress results.
    constexpr std::uint32_t kApiVersion = 2;
}

extern "C" {

// Bumped only for a BREAKING change to an existing entry point. Adding a new
// export leaves this alone: a caller resolves each symbol by name and a missing
// one is already distinguishable from an old one.
__declspec(dllexport) std::uint32_t MenuStudio_GetApiVersion() { return kApiVersion; }

// Keep one actor visible and lit through every cull path the bubble runs, for
// as long as the caller leaves it set. Intended for a mod that is showing the
// player a specific NPC (Fitting Room's outfit editor dressing a follower).
//
// a_refFormID is the ACTOR REFERENCE's form id, not the base NPC's. Zero
// clears. Returns true if the value was accepted, false if the id resolved to
// nothing that is a loaded Actor - which is also what a caller gets for an
// unloaded or "(away)" follower, and is the honest answer rather than a
// silently retained handle.
//
// The caller owns clearing. Menu Studio will not drop this on menu close,
// because RestoreAll also runs when the user changes an unrelated declutter
// setting mid-menu and clearing there would pull the character out of the shot.
__declspec(dllexport) bool MenuStudio_SetFramedCompanion(std::uint32_t a_refFormID) {
    if (a_refFormID == 0) {
        MTB::Declutter::SetFramedCompanion(RE::ActorHandle{});
        return true;
    }
    auto* form = RE::TESForm::LookupByID(a_refFormID);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor) {
        return false;
    }
    MTB::Declutter::SetFramedCompanion(actor->GetHandle());
    return true;
}

// Whether a declutter sweep has actually run since the companion was last set.
// False right after a successful SetFramedCompanion and true once the shot has
// caught up, so a caller can tell "not framed" from "not framed yet" instead of
// guessing across one refresh.
__declspec(dllexport) bool MenuStudio_FramedCompanionTookEffect() {
    return MTB::Declutter::FramedCompanionTookEffect();
}

// Would this actor be accepted as the framed companion right now? For a caller
// offering the user a CHOICE of who to frame, so it can mark the ones that will
// not work before the user commits to one.
//
// a_refFormID is the ACTOR REFERENCE's form id, as above. False means she is
// unloaded, unbuilt, in another cell, or too far from the player to be part of
// the same shot. That last bound is the one a caller cannot guess: it is Menu
// Studio's own composition limit, it is what decides whether the studio rig
// lights a pair or a soloist, and duplicating a copy of it in the caller is how
// the two ends start disagreeing about what is on screen.
//
// This does NOT consider whether she is currently hidden. Menu Studio's own
// sweep hides every actor who is not the companion while a menu is open, and
// naming her is precisely what un-hides her, so a cull check would answer
// "nobody, ever".
__declspec(dllexport) bool MenuStudio_CompanionCanBeFramed(std::uint32_t a_refFormID) {
    if (a_refFormID == 0) {
        return false;
    }
    auto* form = RE::TESForm::LookupByID(a_refFormID);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    return MTB::Declutter::CouldFrameCompanion(actor);
}

// Put a button on the strip Menu Studio draws over a bubbled menu, so a mod can
// be reached from inside the menu the player is already looking at.
//
// a_id names the button and is how it is replaced or removed later; prefix it
// with your plugin name. a_label is what the tooltip says and what the lettered
// fallback tile is derived from. a_iconPath carries the tile's face and reads
// three ways: a string with a dot or a slash in it is a PATH to an image,
// relative to Data, and a texture that fails to load costs the look rather
// than the button; empty falls back to a lettered tile; any other short
// string is drawn VERBATIM as the symbol - pass a Font Awesome solid glyph as
// UTF-8 and it renders from FLICK's own baked atlas, which is what the
// built-in buttons do. Only codepoints already proven under FLICK render;
// an absent one draws as tofu.
//
// Registering an id twice UPDATES that button and keeps its place in the strip,
// so a caller reacting to its own settings change does not end up with two.
//
// The callback takes nothing and returns nothing. Capture what you need on your
// own side: an argument here would be ABI-frozen forever. It is invoked from the
// FLICK draw pass on the main thread, after the strip has finished drawing, so
// it is safe to unregister yourself from inside it.
//
// False means a_id or a_onClick was missing. The button is NOT dropped when the
// menu closes; the caller owns removing it.
__declspec(dllexport) bool MenuStudio_RegisterAction(const char* a_id,
                                                     const char* a_label,
                                                     const char* a_iconPath,
                                                     void (*a_onClick)()) {
    // EXTERNAL, and the flag matters beyond bookkeeping. While the owner-context
    // gate holds the studio down the strip draws over a plain unbubbled menu and
    // offers ONLY entries registered from outside, because every tile Menu
    // Studio registers for itself controls a studio that is not running. This is
    // the door that marks them.
    return MTB::ActionBar::Register(a_id, a_label, a_iconPath, a_onClick, true);
}

// Say that your own window is open over the menu, or that it has closed.
//
// This is the signal behind the player's "Only wake when a mod asks"
// (bWaitForOwnerContext). With that on, Menu Studio counts a covered menu at its
// open and leaves it looking completely vanilla - no pause, no camera, no scene
// - until some owner publishes a live context here. The studio comes up when the
// first one does and goes back down when the last one withdraws, and both can
// happen more than once while a single menu stays open.
//
// a_ownerId names the caller and is the key the set is held by; prefix it with
// your plugin name, and pass the SAME string to both edges. a_active true opens
// the context, false closes it. Idempotent on both edges, so re-asserting is
// safe and only a real change is logged. False means a_ownerId was null or
// empty.
//
// ⚠ MAIN THREAD ONLY, like the rest of this file.
//
// ⚠ SILENCE MEANS "BEHAVE AS TODAY", AND THE GATE IS ARMED BY PROOF RATHER THAN
// BY CONFIGURATION. Until some owner has published a live context at least once
// in the current game session, the player's setting has no effect and menus
// bubble exactly as they always did. A player who updates Menu Studio, turns the
// setting on, and is on an older build of the mod they installed it for would
// otherwise get a bubble that never comes up and nothing telling them why. The
// consequence for callers is that there is no release ordering between the two
// mods and no version to coordinate: ship either half first.
//
// ⚠ THE SET IS CLEARED WHEN THE LAST COVERED MENU CLOSES. An owner that opens a
// context and then unloads, or simply misses its own close edge, cannot pin the
// studio open for the rest of the session. Publish again on your next open
// rather than assuming a context survives the menu it was opened in.
__declspec(dllexport) bool MenuStudio_SetOwnerContext(const char* a_ownerId,
                                                      bool        a_active) {
    return MTB::Bubble::SetOwnerContext(a_ownerId, a_active);
}

// Ask that YOUR sessions keep the world running. The studio still arms in
// full - camera, lights, backdrop - but takes no force-pause and refuses the
// dormant latch that normally answers an unpaused world, so physics and
// animation stay visible while your window is up. Fitting Room's styling
// editor is the caller this exists for: hair and body physics cannot be
// judged on a frozen world.
//
// The wish is keyed by the same a_ownerId you pass SetOwnerContext and only
// counts while that owner also holds a live context; it is NOT cleared with
// the context, so set it once at startup or re-assert it per open, whichever
// is simpler. a_live false withdraws it. False means a_ownerId was null or
// empty.
//
// ⚠ MAIN THREAD ONLY, like the rest of this file.
//
// ⚠ SILENCE MEANS "BEHAVE AS TODAY": a caller that never asks keeps the
// paused studio it has always had, and an older Menu Studio without this
// export simply pauses as before - no release ordering between the mods.
__declspec(dllexport) bool MenuStudio_SetOwnerWorldLive(const char* a_ownerId,
                                                        bool        a_live) {
    return MTB::Bubble::SetOwnerWorldLive(a_ownerId, a_live);
}

// Keep Skyrim's floating Inventory3DManager item stage hidden while this owner
// needs the character or another surface to remain unobstructed. Claims are
// owner-scoped and idempotent: one caller cannot release another caller's
// claim, and suppression remains enforced until the last owner releases. A
// release does not blindly reveal a stale ring entry; Skyrim republishes its
// current selection on the next normal update.
//
// This coordinates only the engine's floating stage. It does not suppress a
// mod's positive on-character preview feature, such as Apparel Preview's worn
// biped preview.
//
// MAIN THREAD ONLY. False means the owner id was null or empty. A repeated
// valid edge returns true because it was accepted, even when it changed no
// state. Claims are reset on load/new-game, and callers publish again on their
// next relevant open edge.
__declspec(dllexport) bool MenuStudio_SetItemPreviewSuppressed(const char* a_ownerId,
                                                               bool a_active) {
    return MTB::ItemPreviewBroker::SetClaim(a_ownerId, a_active);
}

// Say which menus a button belongs in. a_menus is a comma separated list of
// menu names as the engine spells them ("InventoryMenu, MagicMenu"); null or
// empty puts the button back in every menu the strip appears over.
//
// ⚠ EMPTY IS THE DEFAULT, AND THE DEFAULT IS EVERYWHERE. A button that never
// calls this draws in every bubbled menu, which is exactly what buttons did
// before this export existed, so an older caller keeps working untouched and
// nothing has to be adopted to stay correct.
//
// Call it after MenuStudio_RegisterAction, on the same id. False means no
// button is registered under that id yet.
//
// Deliberately not an argument to MenuStudio_RegisterAction: that entry point
// is ABI-frozen and already shipped, and the two say different things anyway.
// Re-registering an id keeps the scope, the same way it keeps the button's
// place in the strip - a caller re-registering after its own settings change is
// updating the button, not moving it.
__declspec(dllexport) bool MenuStudio_SetActionMenus(const char* a_id,
                                                     const char* a_menus) {
    return MTB::ActionBar::SetMenus(a_id, a_menus);
}

// Show or hide a registered button without touching its registration or its
// menu scope. The owner's word for a context finer than a menu name: Fitting
// Room's editor is a WINDOW over the inventory, and only Fitting Room sees
// that window open and close - so it says so here, on both edges, and Menu
// Studio's appearance button exists exactly while the styling context does.
//
// Distinct from the settings panel's per-button hide, which is the PLAYER's
// word and persists in the INI. This one belongs to the calling mod, lives
// for the session, and survives re-registration the same way the menu scope
// does. Idempotent and cheap on repeat calls; only a real change is logged.
//
// False means no button under that id YET. Load order can run a caller's
// startup before this strip installs its own buttons, so a caller keying off
// startup should repeat the call on a later edge - its window's own open is
// the natural one - rather than assume it took.
__declspec(dllexport) bool MenuStudio_SetActionVisible(const char* a_id,
                                                       bool a_visible) {
    return MTB::ActionBar::SetVisible(a_id, a_visible);
}

// Leave a registered button on the strip but refuse it, showing a_reason on
// hover in place of its label.
//
// ⚠ FOR THE CASE WHERE HIDING IS THE WRONG ANSWER. SetActionVisible above says
// "there is nothing here", which is right for a context that does not apply at
// all. A button whose owner knows WHY it cannot be used right now has something
// better to say, and without this was forced to choose between vanishing and
// offering something that would not work. Fitting Room's character editor asked
// for it: entry has a price, and a player who cannot pay should see the door and
// be told, rather than watch a button disappear from under them.
//
// A disabled tile draws in its resting look, never lighting under the cursor or
// sinking under a press, with its symbol faded. It still hovers, because the
// tooltip is the whole point of leaving it there. Clicks are swallowed here
// rather than by the caller, so a refusing button cannot fire even if its owner
// stops checking.
//
// a_reason may be null or empty, which falls back to the label. Correct when
// the refusal is self-evident and there is nothing to add.
//
// The caller's word, like the visibility above: it survives re-registration and
// the settings panel's own hide stays separate and still wins. Safe to call
// every frame - the condition behind it can move at any time, so re-asserting is
// the expected shape, and only a real change is logged.
//
// False means no button under that id YET, with the same load-order caveat as
// SetActionVisible.
__declspec(dllexport) bool MenuStudio_SetActionEnabled(const char* a_id,
                                                       bool        a_enabled,
                                                       const char* a_reason) {
    return MTB::ActionBar::SetEnabled(a_id, a_enabled, a_reason);
}

// Fill a button's background to show how full the owner's resource is, so the
// tile answers "will pressing this get me anywhere" before it is pressed.
//
// a_fraction is 0 to 1. NEGATIVE clears the meter, which is deliberately not
// the same as 0: zero draws an empty bar, which is a fact the player wants,
// while negative restores a plain tile. Out-of-range values are clamped.
//
// ⚠ THE CALLER'S NUMBER AND OUR PICTURE. Fitting Room's Seamstone holds the
// charge that pays for styling and it cannot draw on our tile; we have no idea
// what a Seamstone is. So the fraction crosses the boundary and nothing else
// does, which is the same split every other call here takes.
//
// Safe to call every frame, and expected to be: the value moves whenever the
// player spends or refills. Nothing is logged, unlike the visibility and
// enablement calls, because a continuously moving number would bury the log.
//
// False means no button under that id YET, with the same load-order caveat as
// SetActionVisible.
__declspec(dllexport) bool MenuStudio_SetActionMeter(const char* a_id,
                                                     float       a_fraction) {
    return MTB::ActionBar::SetMeter(a_id, a_fraction);
}

// Takes the button off the strip. False means nothing was registered under that
// id, which is the honest answer and not a failure worth acting on.
__declspec(dllexport) bool MenuStudio_UnregisterAction(const char* a_id) {
    return MTB::ActionBar::Unregister(a_id);
}

// ---------------------------------------------------------------- //
// The shot - API 2                                                  //
// ---------------------------------------------------------------- //
// Point the camera at part of the character, for a mod that knows what the
// player is working on. An outfit editor on a helmet wants the head framed;
// the same editor on boots wants the feet. Menu Studio cannot know which,
// and a static pivot setting cannot either.
//
// a_nodeName is a skeleton node ("NPC Head [Head]"); null or empty is the same
// as clearing. a_closeness runs 0 (as far out as the boundary allows) to 1 (as
// close as the near floor allows), and NEGATIVE moves only the pivot, leaving
// the distance to the player.
//
// ⚠ THE PIVOT STICKS, THE DISTANCE DOES NOT. Keeping the pivot is the point:
// the player goes on orbiting the head for as long as they are editing it.
// Keeping the distance would fight them the moment they touched the wheel, so
// closeness is a one-shot request that ordinary zooming then owns.
//
// ⚠ MAIN THREAD ONLY, like the rest of this file. Everything moves through the
// same easing and the same soft boundary as a player's own drag, so a caller
// cannot put the camera anywhere the player could not have.
//
// Silently does nothing when the player has the camera switched off. That is
// deliberate: this layer's contract is that it writes nothing until asked, and
// the setting is where the player says whether asking is allowed at all.
__declspec(dllexport) void MenuStudio_FocusShotOnNode(const char* a_nodeName,
                                                      float a_closeness) {
    MTB::StudioCamera::FocusOnNode(a_nodeName, a_closeness);
}

// The same request, for a caller pointing at something WORN rather than at a
// body part. The pivot is the measured centre of whatever geometry hangs off
// the node and the distance comes from its radius, so the caller does not have
// to know how long a two-handed sword is, or how long a modded one is.
//
// a_fallbackCloseness is used only when there is nothing to measure, where this
// behaves exactly like MenuStudio_FocusShotOnNode.
//
// ⚠ ADDITIVE, SO THE API VERSION DOES NOT MOVE. The version bumps for a
// BREAKING change to an existing entry point; a new name is resolved or not
// resolved by GetProcAddress on the caller's side, which is the gate.
__declspec(dllexport) void MenuStudio_FocusShotOnAttachment(const char* a_nodeName,
                                                            float a_fallbackCloseness) {
    MTB::StudioCamera::FocusOnAttachment(a_nodeName, a_fallbackCloseness);
}

// End the focus round: the pivot goes back to the one the player's own
// settings name, the shot eases back to the framing the menu opened with,
// and a hand-built pan is dropped with it. Safe to call when nothing is
// focused.
__declspec(dllexport) void MenuStudio_ClearShotFocus() {
    MTB::StudioCamera::ClearFocus();
}

// Has the player moved this framing by hand (a middle-drag pan) since the
// last focus change or recentre?
//
// For a caller with pages of its own. Switching a page is not by itself a
// reason to throw a shot away: a player who has nudged the framing onto a
// detail wants it kept while they go and pick a colour, and one who has
// touched nothing wants the whole character back. Ask this, then either
// leave the camera alone or call MenuStudio_ClearShotFocus.
//
// False when the camera is not armed or the player has it switched off,
// which is the honest answer for "did the player frame this" in both cases.
__declspec(dllexport) bool MenuStudio_ShotWasPanned() {
    return MTB::StudioCamera::ShotWasPanned();
}

}  // extern "C"

#pragma once

#include "BackdropPolicy.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace MTB {
    // Named [Lighting] preset (shared by INI load and the SMF panel).
    // A preset is a complete vibe: background (ambient/directional/fog/
    // DALC fill) AND the three-point rig (per-light color + intensity).
    struct LightPreset {
        struct RigLook {
            RE::Color color;
            float     intensity;
        };
        const char* name;
        RE::Color   ambient, directional, fog, fill;
        RigLook     key, fillLight, rim;
    };

    // One extra set piece of a stage beyond floor + dome (braziers, props,
    // wall segments…). Offsets are world-axis units from the player's feet;
    // fitRadius > 0 rescales the mesh's bound to that size, otherwise the
    // explicit scale applies. tint = follow the look's emissive tint
    // (props like fire usually want their native look).
    struct StagePiece {
        const char* mesh;
        float       fitRadius;
        float       scale;
        float       x, y, z;
        float       yawDeg;
        bool        tint;
    };

    // The BACKGROUND is its own axis (user design, r18): the dome that
    // surrounds you in the Void AND the Dressing room - mix and match
    // with any stage. 'blank' = no dome, just the colored fog void.
    struct BackgroundPreset {
        const char* name;
        const char* mesh;   // the dome/sphere mesh (empty = no dome piece)
        float       radius, z;
        const char* image;  // non-empty: repoint the image sphere to this DDS
        bool        faceCamera;  // lock the background to the camera (images)
        float       yaw;         // extra rotation (degrees) when locked
        const char* group = "";  // accordion label ("" = the plain packs group)
        const char* thumb = "";  // optional card picture (PNG, textures-relative)
    };

    // Named backdrop stage: the floor (panel-dialable via the fields
    // below) plus fixed extra set pieces - dressing room only. Picking
    // one copies the floor into the backdrop fields (same pattern as
    // LightPreset); hand-edits become per-key overrides on top.
    struct StagePreset {
        const char* name;
        const char* floorMesh;
        float       floorRadius, floorZ;
        std::span<const StagePiece> extras;
    };

    // INI-backed settings. Data/SKSE/Plugins/MenuStudio.ini
    // (MO2: overwrite/ wins). Loaded at kDataLoaded and re-read on every
    // save load; Save() writes the panel-edited keys back (load-modify-save
    // so the shipped comments survive).
    class Settings {
    public:
        static Settings& GetSingleton();

        static std::span<const LightPreset>      LightPresets();
        static std::span<const StagePreset>      StagePresets();
        static std::span<const BackgroundPreset> BackgroundPresets();

        // Copies the named stage's floor into the backdrop fields.
        bool ApplyStagePreset(std::string_view a_name);
        // Copies the named background's dome into the backdrop fields.
        bool ApplyBackgroundPreset(std::string_view a_name);

        // The active stage's extra set pieces (empty for unknown stages).
        [[nodiscard]] std::span<const StagePiece> ActiveStageExtras() const;

        void Load();
        void Save();

        // Sets the four colors + lightPreset from a named preset.
        bool ApplyLightPreset(std::string_view a_name);

        bool IsBubbleMenu(const std::string& a_menuName) const {
            return menus.contains(a_menuName);
        }

        // §4b: is this menu handed back to Skyrim Souls - live, unpaused, no
        // studio? Only ever true when Souls is actually loaded, because
        // force-pause exists solely to undo Souls' unpausing: without Souls
        // every covered menu already carries kPausesGame and there is nothing
        // to hand back.
        //
        // The conflict this settles is real and irreducible per menu - the
        // studio needs a frozen scene, Souls exists to keep menus live, and no
        // single menu can be both. What it does NOT have to be is all-or-
        // nothing, which is what it was before.
        //
        // Two layers on purpose, so no existing INI changes meaning on update:
        // bForcePause=0 is the OLD all-or-nothing opt-out and still hands over
        // every menu, while sSoulsLiveMenus refines things per menu when
        // force-pause is on. An empty list is exactly the pre-0.6.1 behaviour.
        [[nodiscard]] bool IsSoulsLiveMenu(const std::string& a_menuName) const {
            if (!soulsLoaded) {
                return false;
            }
            return !forcePause || soulsLiveMenus.contains(a_menuName);
        }

        // The single question the bubble asks when a menu opens. The two halves
        // stay separate above because the panel needs the CONFIGURED set to
        // draw its per-menu list, while this is the EFFECTIVE answer.
        [[nodiscard]] bool ShouldBubbleMenu(const std::string& a_menuName) const {
            return IsBubbleMenu(a_menuName) && !IsSoulsLiveMenu(a_menuName);
        }

        // View-mode family helper - kill the scattered magic numbers. The VOID
        // FAMILY (cull the world + studio rig + backdrop + gap occluder + studio
        // imagespace) is mode 2 (Void) and mode 3 (Dressing room). The colour
        // FILTER (bColorFilter) is independent of the view mode - it grades any
        // view - so it is not part of this family test.
        [[nodiscard]] bool IsVoidFamily() const {
            return declutterMode == 2 || declutterMode == 3;
        }

        // F-24 (field request: "custom lighting without a scene - Natural
        // world behind me + this mod's lighting"). The rig and the cell-light
        // override were both welded to the void family, so the only way to get
        // studio light was to lose the world. They are NOT one feature: the rig
        // ADDS three lights around the character and leaves the cell alone,
        // while the override REWRITES the room's own ambient/fog and flattens
        // its imagespace. Reading the ask as "the lighting, minus the void"
        // could mean either, so each piece gets its own opt-in instead of one
        // shared flag guessing which. Both default off - an existing install
        // sees no change on update.
        //
        // Safe outside the void BY CONSTRUCTION: bCutCellLights lives inside
        // Declutter::Refresh's IsVoidFamily branch (which returns before the
        // mode 0/1 path is reached), so freeing these can never drag the
        // light-source cull along and darken the world that is still visible.
        // r28: `liveStudioActive` is the LIVE-MENU case (Skyrim Souls keeping a
        // menu unpaused). It is a runtime flag Bubble sets for the duration of
        // such a session, never an INI key - see studioInLiveMenus below.
        //
        // The rig is the ONE piece that is safe here, and safe for the same
        // reason it is safe outside the void: it adds three lights around the
        // character and touches nothing else. No pause, no culling, no
        // rewriting the room. Nothing to restore but the lights.
        [[nodiscard]] bool RigAllowed() const {
            return studioRig && (IsVoidFamily() || rigWithoutSpace || liveStudioActive);
        }

        [[nodiscard]] bool CellLightAllowed() const {
            return standardizeLighting && (IsVoidFamily() || studioLightWithoutSpace);
        }

        // Does this menu get the SPACE (void / dressing room)? Field request
        // (NymerethRole, 2026-07-18): "would love it even more if it would be
        // possible to just have it when I open my character menu - with npcs
        // and followers I dont necessarily need it." So the backdrop becomes
        // per-menu while the rest of the bubble (pause, physics, the live
        // character) still applies everywhere in sMenus.
        [[nodiscard]] bool MenuWantsSpace(const std::string& a_menuName) const {
            return spaceMenus.contains(a_menuName);
        }

        // The CONFIGURED space mode (INI + panel). `declutterMode` below holds
        // the EFFECTIVE one for the menu currently open - the bubble sets it at
        // arm from this value and MenuWantsSpace(), and restores it at disarm.
        // Consumers keep reading `declutterMode` and get the per-menu answer
        // for free; only Load/Save/panel touch this one.
        int declutterModeIni = 2;
        // Menus that get the space; default = the same four the bubble covers.
        // The character editor is in here too, so the studio it opens with
        // is the one the strip's button promises. Its space is the reason
        // the editor looked wrong without it: the bubble armed, but the
        // world stayed visible behind the character.
        std::unordered_set<std::string> spaceMenus{ "ContainerMenu", "BarterMenu",
                                                    "InventoryMenu", "MagicMenu",
                                                    "GridInventoryMenu" };

        bool  enabled = true;
        // Hold the studio down until a mod that uses Menu Studio says its own
        // window is open over the menu, so a plain inventory looks vanilla and
        // the studio only comes up for the thing the player installed it for.
        //
        // ⚠ THE MENU IS STILL COUNTED, ONLY THE STUDIO STANDS DOWN. The bubble's
        // entry test (ShouldBubbleMenu) is untouched and still answers once, at
        // open - see the r19c autopsy in Bubble.cpp for why that predicate can
        // never become "is the owner's window open right now". The studio
        // session is a second, nested lifetime that CAN move mid-menu.
        //
        // ⚠⚠ ARMED BY PROOF, NOT BY THIS FLAG. Until some owner has published a
        // context at least once in the current game session this does nothing
        // at all, so a player on an older Fitting Room that calls nothing gets
        // their bubble rather than one that never comes up with no way to tell
        // why. StudioSessionPolicy.h holds the rule.
        bool  waitForOwnerContext = false;  // INI bWaitForOwnerContext
        bool  tickAnimation = true;   // Spike A
        bool  driveSmp = true;        // Spike B/C
        bool  tickFace = true;        // facegen morphs: blinks, MFG expressions
        // 0.7.5. Keep the FRAMED COMPANION as alive as the player: step her
        // behaviour graph and her face on the same ticks. Without it she is a
        // statue standing next to a breathing character, which reads worse than
        // either alone, and it defeats the point of putting her in the shot.
        //
        // ⚠ ITS OWN SWITCH, on purpose, and not folded into bTickAnimation. The
        // 0.7.1 lunge loop is the precedent: stepping a behaviour graph while
        // the Papyrus VM is frozen lets the graph SELECT its next pose against
        // stale state, and every guard built for that (ClipProbe's in-flight
        // equip detection, the arm-edge latch, the moving-arm exemption) reads
        // the PLAYER. None of it covers her. If she lunges or loops, turn this
        // off and the player's own liveness is untouched.
        bool  tickCompanion = true;   // INI bTickCompanion
        // The 0.7.1 lunge loop, on HER. Confirmed in the field 2026-07-31.
        //
        // She cannot be given the player's hold: that latches on the first equip
        // of the session and never clears, which in an outfit editor made her a
        // permanent statue. So this watches her event stream and holds only once
        // it is caught cycling.
        //
        // ⚠ THE REPEAT COUNT IS AN ESTIMATE AND THAT IS WHY IT IS A SETTING.
        // The player's loop was five tags cycling once per 3.3 s, so one tag
        // repeating four times inside a single menu should not happen in a
        // settled idle. Nobody has ever recorded HERS. The disarm report prints
        // the full histogram so this can be set from a measurement instead:
        // raise it if a normal menu reads as a loop, lower it if she lunges
        // several times before the hold bites.
        bool  companionLungeGuard = true;    // INI bCompanionLungeGuard
        int   companionLungeRepeats = 4;     // INI iCompanionLungeRepeats
        // TEMPORARY, and it should not outlive the question it was asked for:
        // why she is a statue, and whether the player is really on the
        // camera-to-her axis. Both are one field run from settled. Delete
        // CompanionProbe.{h,cpp}, these two members, their loader lines and the
        // Sample/Report/Reset call sites together once they are.
        //
        // ⚠ LOADED BUT NEVER SAVED, on purpose. A diagnostic that writes itself
        // into the user's live INI outlives the question it was asked for, and
        // this project's own rule is that a live INI value beats any later code
        // default - so a saved probe key would quietly survive its own deletion.
        bool        companionProbe = true;   // INI bCompanionProbe
        std::string companionProbeBone{ "NPC Spine2 [Spn2]" };  // INI sCompanionProbeBone
        bool  tickMagicCasters = true;// equipped-spell hand art under pause (0.2)
        bool  idleInMenus = true;     // B-2: feed Speed=0 so the walk cycle settles to idle
        // F-26: draw the equipped weapon while a bubbled menu is open so it is
        // visible in the character's hand, and sheathe it again on close. ON by
        // default (direct field request); off restores byte-identical behaviour.
        bool  weaponPreviewInMenus = true;
        // r20b: DRAW A SHEATHED WEAPON? OFF by default as of 2026-07-20.
        //
        // Auto-drawing is the only way this feature can ever OWE a sheathe, and
        // an unpaid sheathe wrote a broken weapon state into the user's SAVE:
        // reload showed empty hands with the weapon on the hip, because the
        // save recorded a drawn state we manufactured and the rebuilt animation
        // graph put the model back on its sheath node. Off, the preview only
        // MIRRORS - if the weapon was already out you see it, and if it was
        // sheathed it stays on the hip - so no debt exists to leak.
        //
        // Set 1 for the old always-draw behaviour. Weapon SWAP handling is
        // unaffected either way; it has never depended on us having drawn.
        bool  autoDrawInMenus = false;
        // B-3, rebuilt 2026-08-24: hold the player's head tracking OFF while a
        // bubble menu is armed (actor state bit, targets cleared, restored on
        // exit). It used to pin a target ahead of the spun body, which the
        // node spin then double-counted into a twisted neck.
        bool  freezeHeadTracking = true;
        // Does the strip's editor button open the engine's LIMITED sculptor
        // mode (appearance only, no race or sex) or the full editor?
        //
        // Limited is the default and the field's own call ("so we can't alter
        // the race"), but it is a different engine path from the one
        // `showracemenu` takes, and 2026-08-06 put a question on it that
        // nothing else could answer: the head follows the mouse in a console
        // racemenu and does not in ours, with every Menu Studio suspect
        // eliminated by log line. This is the switch that tells the two paths
        // apart in one field pass instead of by argument.
        bool  limitedEditor = true;
        bool  pinBodyHeading = true;  // B-7: retired key, force-disabled at load (rotation upstream)
        // F-13 / F-16: what happens to the FACE while a bubble menu is up
        // (expression mods run on Papyrus, which the pause freezes):
        //   0 = hold as caught (can stick a half-finished blink all menu)
        //   1 = LIVE (default): keep the caught expression (Conditional
        //       Expressions' exprOverride stays up), natural blinking runs,
        //       a blink the pause caught halfway is released
        //   2 = neutral: save + dissolve to neutral at arm, restore at close
        int   faceInMenus = 1;

        // F-14: the bubble's own body spin (right-mouse drag / right
        // stick) - the ONLY rotation that moves the skeleton, so
        // hair/cloth physics swings with it (a camera orbit moves
        // nothing, so physics correctly stays still). Default ON since
        // r32 (user call after the controller rotation field-confirmed);
        // coexists with the SPII author's camera rotation.
        bool  previewSpin = true;
        // r61: Show Player In Menus rotates on RIGHT-MOUSE HELD too (a player
        // turn plus a camera counter-turn), so on a SPIM setup one drag drove
        // both its rotation and our spin. When SPIM is loaded, neutralise its
        // rotation and let our spin own the character. Escape hatch for a
        // SPIM user who wants the old combined behavior back.
        bool  overrideSpimRotation = true;
        float spinSensitivity = 0.005f;       // radians per mouse count
        int   spinGamepadButton = 274;        // hold to rotate (274 = left shoulder)
        float spinStickSensitivity = 3.0f;    // radians/second at full right-stick deflection

        // Preview-item prompt (2026-07-13): a persistent, device-aware on-screen
        // hint that the highlighted item can be tried on in the dressing room.
        // The try-on itself is Apparel Preview's (it owns the key / R3 press); this
        // is only the visible affordance. Labels are DISPLAY-ONLY text - set them to
        // match your Apparel Preview binding (keyboard default = its inspect key,
        // fallback C; gamepad default = R3 / right-stick click).
        bool        showTryOnPrompt = true;
        std::string tryOnLabelKbd = "C";
        std::string tryOnLabelPad = "R3";

        // F-15: a first-person arm has no character to preview (the SPII
        // build misses this on barter). The bubble provides the third-
        // person view itself and puts first person back at the exit.
        bool  ownViewFirstPerson = true;
        // F-15 phase 2: own the framing on third-person arms too whenever
        // no loaded view mod covers the menu (this profile: SPII Preview
        // skips barter + container). The framing itself is SPIM's recipe;
        // offsets below match the Nolvus SPIM preset and are superseded by
        // SPIM's own MCM files when those exist in the VFS.
        bool  ownViewUnmanaged = true;
        float ownViewXOffset = -20.0f;   // over-shoulder X = -this - 75
        float ownViewYOffset = 50.0f;    // boom length = 155 - this
        float ownViewZOffset = 0.0f;     // over-shoulder Z = this - 50
        float ownViewPitch = 0.0f;       // player pitch = 0.2 + this
        float ownViewRotation = 0.0f;    // face-the-player yaw = pi + this - 0.5
        // Mounted-only framing deltas, applied ON TOP of the standing framing
        // (a rider sits ~120u up on the horse, so the standing offset frames
        // the legs). Raise the look-at to the rider's torso, add boom to fit,
        // optionally tilt. Boom default trimmed from the r53 +140 (which read
        // as a tiny rider lost above a big horse) toward the standing look.
        //
        // ⚠⚠ 120 WAS FITTED WHEN THE SHOT'S ANCHOR WAS THE SADDLE, AND THE
        // ANCHOR HAS MOVED. The subject is now the whole rider-and-horse
        // column, whose middle sits about 40 BELOW the rider's ref rather than
        // at it, so a raise that put the lens level with her torso now puts it
        // 117 above what the shot is built around. Two things followed and the
        // field reported both: the opening frame aims over her head, because
        // before the studio camera captures the view direction is level and a
        // level lens sees whatever height it sits at; and the captured orbit
        // comes out pitched 0.46 down, which reads as looking at the horse
        // from above.
        //
        // 45 puts a level lens at her chest (her crown measures 48.5 above the
        // ref) and brings the captured pitch to about 0.33.
        //
        // ⚠ The do-not-retry list bans RAISING this, on the evidence that
        // raising drops the subject further out of frame. Lowering is the same
        // finding read forwards, and it is what that entry was pointing at.
        float ownViewMountRaise = 45.0f;  // over-shoulder Z += this when mounted
        float ownViewMountBoom  = 50.0f;   // boom (vanity dist) += this when mounted
        float ownViewMountPitch = 0.0f;    // player pitch += this when mounted

        // F-12: fade the studio (stage pieces, rig lights) in at arm and
        // out through the teardown grace instead of popping. Durations in
        // seconds; 0 = instant (identical to the pre-F-12 behavior).
        bool  sleekTransitions = true;
        float transitionInSeconds = 0.35f;
        float transitionOutSeconds = 0.15f;

        // F-12 v3: cut the screen to black IN the menu-open call stack (the
        // skills-menu / RaceMenu look) - the UI never appears over the
        // dressed world; the studio builds behind the black and blooms in
        // under the fade-in. Engine FaderMenu (UI-clocked, pause-immune).
        // Void + dressing room only. Out-duration 0 = instant cut (field
        // r24: any visible world under the menu UI reads as jarring).
        bool  dipToBlack = true;
        float dipOutSeconds = 0.0f;
        float dipInSeconds = 0.35f;

        // r38 SLEEK EXIT (F-12's close half, unparked on user ask "the exit
        // transition is shit"): hold the studio through the switch window,
        // engine fader to black, ALL restores under the black, fade back
        // in - the skills menu's own exit choreography (its ProcessMessage
        // close path drives the same fader). Close-side only; the open dip
        // stays parked (r27). Switches cancel in the hold phase for free.
        bool  sleekExit = true;
        // r48 (user spec: "not even visible the moment we switch out"):
        // hold 0 = the cut happens AT the close event - nothing of the
        // studio survives into a single post-menu frame. The cost is a
        // few frames of world between menu SWITCHES; raise the hold to
        // ~0.085 to trade back (gaps measure 52-71 ms).
        float exitHoldSeconds = 0.0f;
        float exitDipSeconds = 0.12f;    // retired r47 (fader deleted); parsed, unused
        float exitInSeconds = 0.20f;     // retired r47 (fader deleted); parsed, unused
        bool  forcePause = true;      // §3.1: re-pause bubble menus Skyrim Souls unpaused
        // SkyrimSoulsRE.dll present this session (read-only; set during Load).
        // Drives the force-pause default when the INI does not say, and makes
        // "is Souls even loaded" answerable from a log or the settings panel.
        bool  soulsLoaded = false;
        // r19: under Skyrim Souls, hold the pause with our OWN pausing menu
        // (Souls only knows vanilla menu names) instead of Main::freezeTime.
        // freezeTime freezes the clock but leaves the game in a non-pausing-menu
        // state, which leaks gameplay camera input. Set bShadowPause=0 to fall
        // back to the r17 freeze if this misbehaves.
        bool  shadowPause = true;
        bool  blockRightMouse = true; // eat right-mouse in bubble menus (UI quick-buy vs rotation)
        // The floating button strip. Position is a FRACTION of the display
        // rather than a pixel pair, so a resolution change moves it with the
        // screen instead of stranding it off the edge.
        bool  actionBar = true;       // INI bActionBar
        // Which strip buttons the player has switched OFF, by id.
        //
        // ⚠ STORED AS THE HIDDEN SET, NOT THE SHOWN ONE, and that is the whole
        // design. Any plugin may register a button at any time, so a list of
        // what to SHOW would hide every button this file has not heard of -
        // install a mod, get nothing, with no way to know why. An id absent
        // from the hidden set shows, so a new button appears the first time it
        // registers and only an explicit tick takes it away.
        std::unordered_set<std::string> hiddenActions;  // INI sHiddenActions

        [[nodiscard]] bool IsActionHidden(const std::string& a_id) const {
            return hiddenActions.contains(a_id);
        }
        // Top-right corner. The first default (0.86, 0.18) floated the strip
        // over the character's weapon in the field screenshot; the corner is
        // clear in every menu this bar appears in.
        float actionBarX = 0.965f;    // INI fActionBarX, 0..1 across the display
        float actionBarY = 0.03f;     // INI fActionBarY, 0..1 down the display
        // Which shape our own chrome draws: 0 auto, 1 carved, 2 plain.
        //
        // ⚠⚠ CARVED IS THE DEFAULT AND AUTO IS GONE, 2026-08-28, the field's
        // call: "now though the theme looks great on carved for vanilla and
        // vel'dun ... don't even have the follow theme option, just make it
        // carved and plain, carved by default". Auto had been the default for
        // one day, and it was there because the carve looked wrong under FLICK's
        // own theme. What looked wrong was Fitting Room's editor stacking that
        // theme's translucent ChildBg across nested children; the corner itself
        // reads correctly under both presets.
        //
        // ⚠⚠ THE ART MUST SHIP FOR CARVED TO BE HONEST, and it is the default
        // again, so this matters more than it did. FrameArt loads frame.png and
        // frame_fill.png; without them every carved surface falls back to the
        // straight bevel, which is a different shape. tools/package.sh copies
        // the icons folder for exactly this reason.
        //
        // ⚠ NO MIGRATION, AND NONE IS NEEDED. 1 stays carved, 0 was auto and
        // StyleFromIni reads it as carved, and a 2 is a player who went and
        // found plain. ⚠ Fitting Room made the same change on the same day: the
        // two draw in one menu and must not disagree.
        static constexpr int kFrameStyleDefault = 1;
        int frameStyle = kFrameStyleDefault;  // INI iFrameStyle
        // The stamp Save writes into iSettingsVersion.
        //
        // ⚠ IT NEVER GOES DOWN, WHATEVER LEAVES. This was
        // MTB::FrameStyle::kSettingsVersion until the frame-style migration was
        // deleted on 2026-08-28. Nothing gates on it today; it stays written so
        // the next migration can tell an old file from a new one.
        static constexpr int kSettingsVersion = 1;
        // ⚠⚠ THERE IS NO fPlainRounding KEY AND ONE SHOULD NOT COME BACK. It
        // was the plain path's single radius for a day. The tiles drew a
        // fraction of their own width before any of this existed (5faf221,
        // `(x1 - x0) * kRoundingFraction`), and a flat pixel count is wrong for
        // the same reason a flat CUT is: square on a large tile, swallowing a
        // small one. ActionBar takes the fraction again. Fitting Room removed
        // its copy of this key the same day for the same reason. Save() deletes
        // it so a dead line cannot sit in a player's INI looking live.
        // The pivot camera. Left-drag orbits, wheel pulls in, middle-drag moves
        // the pivot. The character spin on right-drag is a separate feature and
        // is untouched by all of this.
        bool  studioCamera = true;                     // INI bStudioCamera
        // The head, not the chest. Zooming in on a face is what people reach for
        // first, and a head pivot keeps the face centred all the way in; a chest
        // pivot pushes it out of frame as the camera closes. At a full-length
        // distance the difference is barely visible, so the close-up wins.
        std::string cameraPivotNode{ "NPC Head [Head]" };  // INI sCameraPivotNode
        float cameraOrbitSensitivity = 0.006f;         // radians per mouse count
        // The zoom track: the wheel drives one 0-to-1 position, distance is
        // a log-lerp between the near floor and the far boundary, and the
        // pivot height is keyframed over the same value. One notch travels
        // this share of the track, so the default crosses the whole range in
        // about fifteen notches and every notch is the same distance RATIO.
        float cameraTrackStep = 1.0f / 15.0f;          // INI fCameraTrackStep
        // The authored compositions, tunable as data: where the torso
        // keyframe sits on the track, how far its anchor sits from the face
        // anchor toward the body middle, and where the body middle sits
        // between feet and crown. Face and body are pinned to the track's
        // stops by construction.
        // The composition: the shot aims at whatever was focused and gives
        // way to the body middle only as fast as the widening frame forces
        // it to. Slack is how far the frame may hang past the body while it
        // still has room to, which is what keeps a boot clear of the bottom
        // edge rather than sitting on it. The lens slope is the tan of half
        // the vertical FOV; the menus run FOV 60, measured, and nothing
        // reads the live frustum.
        float cameraTrackSlack = 0.5f;         // INI fCameraTrackSlack
        // How much wider a focus on the end of a limb arrives than the
        // caller asked for, as a multiplier on the wheel notches each part
        // states. A bone says where a joint is and never how much boot
        // hangs off it, so a hand or a boot arrives a notch back. 0 takes
        // callers at their word.
        float cameraFocusMargin = 1.0f;        // INI fCameraFocusMargin
        float cameraTrackLensSlope = 0.325f;   // INI fCameraTrackLensSlope
        // How much of the half-frame a measured attachment spans when the shot
        // arrives on it. Below 1 leaves margin, so a sword does not touch the
        // edges of the frame; 1 fits it exactly and 0 turns the measured
        // distance off, leaving the caller's fallback closeness to decide.
        float cameraAttachmentFill = 0.8f;     // INI fCameraAttachmentFill
        float cameraTrackBodyMid = 0.5f;       // INI fCameraTrackBodyMid
        // How much of a focused part's sideways offset the close shot
        // carries. The pivot's XY sits on the subject's axis, which is what
        // keeps the shot steady while the preview spin turns them, but a
        // hand or a foot hangs well off that axis and a height-only pivot
        // framed the hip instead of the hand. 0 goes back to axis-only.
        float cameraTrackLateral = 1.0f;       // INI fCameraTrackLateral
        // The face keyframe as a fraction of the held body height - the eye
        // line. Authored against the ORIGIN rather than any head-cluster
        // joint: on the field skeleton the head, neck and magic-node joints
        // all sit within a few units of the crown, so no joint can place a
        // face, and the live crown bows when the head tracks the camera.
        float cameraTrackFaceHeight = 0.93f;   // INI fCameraTrackFaceHeight
        // The middle-drag pan: world units per mouse count per unit of
        // distance (the default tracks the cursor about 1:1 at the menus'
        // optics), and how far the pan may carry the frame off the subject,
        // as a fraction of the held body height.
        float cameraPanSensitivity = 0.00045f;  // INI fCameraPanSensitivity
        float cameraPanRange = 0.5f;            // INI fCameraPanRange
        // ⚠ DOWN REACHES FURTHER, AND THAT ASYMMETRY IS THE POINT. The pan
        // composes onto the track's pivot, which sits at the eye line when
        // the shot is close, so the shoes are nearly a whole body below it
        // while the crown is a head above. At the shared 0.5 the field could
        // not reach the shoes from a face zoom; 1.0 carries the eye line
        // past the feet with a little margin for a low angle.
        float cameraPanRangeDown = 1.0f;        // INI fCameraPanRangeDown
        // How much the camera trails the hand. 0 follows almost exactly, 1
        // drifts. The default is loose enough that a drag reads as a move
        // rather than a jump, which is what "stiff" meant in the field.
        float cameraSmoothing = 0.7f;                  // INI fCameraSmoothing
        // ⚠ TWO SEPARATE NUMBERS, AND CONFLATING THEM BROKE THE ZOOM ONCE.
        // "How close may the lens get before it is inside them" and "what is a
        // sensible distance to open at when the shot we found was framed on
        // somebody else" are different questions with answers an order of
        // magnitude apart. A first attempt derived one value from the subject's
        // bounding sphere and used it for both; that sphere encloses the whole
        // skeleton, so it landed just under the opening distance and pinned the
        // wheel after a single notch.
        float cameraMinDistance = 60.0f;   // hard floor, chest pivot, clears a torso
        float cameraOpenDistance = 180.0f;  // used only when the capture is unusable
        // Far boundary. The default is a plain 250, the value the field
        // settled on after living with 300: it frames a whole character with
        // room around them and stops well before they become a speck. Set 0 to
        // work it out from the space instead, in which case the Void and the
        // Dressing room use the dome the player is standing inside and it
        // follows that dome when they resize it. The wheel travels a little
        // past whatever this resolves to and is then eased back, so the limit
        // is felt rather than hit.
        float cameraMaxDistance = 250.0f;   // INI fCameraMaxDistance
        // The escape hatch on a button of its own. A strip icon did this job
        // first and earned its keep, but a click on a distant tile mid-orbit
        // costs the framing you were holding; the middle mouse is free (the
        // pan it once drove was cut) and is already resting under the finger
        // that zooms.
        bool middleClickRecentre = true;  // INI bMiddleClickRecentre
        // ── The framing the player built, carried to the next menu ──────────
        //
        // Field 2026-08-30: "How do I save a camera position? I have to
        // readjust every time as I want to have a full character view instead
        // of the top half." There was no answer: the shot was rebuilt from the
        // opening framing on every single menu, so a player who prefers a
        // full-body view re-made it every time they opened a bag.
        //
        // ⚠⚠ OFFSETS, NEVER A CAMERA POSITION, AND THAT IS WHAT MAKES IT SAFE.
        // StudioCamera's whole contract is that it reads the shot the menu (or
        // a view mod) already built and composes onto it - see the note at the
        // top of StudioCamera.h. An absolute position saved from an inventory
        // would be meaningless in a barter menu, on a horse, or under a
        // different view mod. These five are exactly what a drag, a wheel notch
        // and a middle-drag accumulate, in the same units, so restoring them is
        // indistinguishable from the player having made the same moves again -
        // the same currency the pre-capture park already speaks.
        //
        // The yaw and pitch are radians away from the framing the menu opened
        // with; the zoom is a step along the normalised track from where the
        // menu opened; the two pan values are shares of the subject's height.
        // ⚠⚠ SHIPS OFF, AND THAT IS THE AUTHOR'S OWN CALL AFTER LIVING WITH IT
        // (2026-08-31, "I thought I would like it but I don't"). The ask above
        // was real and the switch stays for anyone who shares it, but a menu
        // that opens on a shot you built an hour ago is a surprise by default,
        // and the person who asked for it is the person who turned it off.
        // ⚠ FREE TO CHANGE ONLY BECAUSE NOTHING SHIPPED WITH IT ON. The key is
        // written by Save(), so once an install has one this default is read
        // exactly once and every later change to it is invisible. Nexus is on
        // 1.1.3; 1.1.4 and 1.1.5 never left this machine. See
        // [[a-saved-default-outlives-the-default]].
        bool  rememberFraming = false;  // INI bRememberFraming
        bool  framingSaved = false;     // INI bFramingSaved: is there one yet?
        float framingYaw = 0.0f;        // INI fFramingYaw
        float framingPitch = 0.0f;      // INI fFramingPitch
        float framingZoom = 0.0f;       // INI fFramingZoom
        float framingHeight = 0.0f;     // INI fFramingHeight
        float framingLateral = 0.0f;    // INI fFramingLateral
        // Kill the engine's floating item preview in every bubbled menu. It
        // shares the character's screen space and, mid-swing, its hover-driven
        // model loads read as flicker; with the studio camera in play some
        // players want the character to be the only 3D on screen.
        bool disableItemPreview3D = false;  // INI bDisableItemPreview3D
        // Entry into the engine's 3D item inspect stops being something the
        // wheel can do, so a notch over the character is the camera's and
        // nothing else. Once inspect is OPEN the wheel reaches the item again -
        // the gate only covers the frames where zoomProgress is still 0, which
        // is what makes it an entry gate rather than a wheel ban. See
        // MenuInputGate.cpp for the mechanism and Bubble.cpp's note on the
        // blanket refusal that was tried instead.
        //
        // ⚠ SHIPS ON, WHICH IS A BEHAVIOUR CHANGE FOR EXISTING PLAYERS. The
        // stray-scroll inspect is a defect, not a preference: the first notch
        // moved the camera AND dropped the item behind it into inspect, and the
        // report was that it reads as very jarring. This switch is the way back.
        bool inspectNeedsHotkey = true;  // INI bInspectNeedsHotkey
        // Where on screen a camera drag may START, as fractions of the display.
        // ⚠ A STATED REGION, NOT A DERIVED ONE, and per UI skin. Asking the menu
        // itself was tried and refuted: GFxMovieView::HitTest answers "over the
        // UI" about as often over the character as over the item list, because
        // SkyUI carries button-enabled objects across the character view. The
        // default covers the right side, where the vanilla-family skins stand
        // the character; a skin that puts its list on the right needs this moved.
        // ⚠ 0.44 IS MEASURED, NOT CHOSEN. The 2026-08-04 logs recorded where
        // drags were being turned away: they cluster from 0.46 up to 0.52 and
        // stop dead at the old boundary, which is the shape of a boundary
        // cutting across the character rather than of clicks landing in the
        // list. SkyUI's item columns end around 0.445 on a 16:9 screen, so this
        // sits just past them and gives the whole character back.
        float cameraZoneLeft = 0.44f;
        float cameraZoneTop = 0.0f;
        float cameraZoneRight = 1.0f;
        float cameraZoneBottom = 1.0f;
        // The same region again, for Grid Inventory, because its window is not
        // where SkyUI's list is. A drag that starts on the grid was swinging the
        // camera (field, 2026-08-27), and 0.44 is the wrong boundary there: that
        // number was measured against SkyUI's item columns and means nothing to
        // somebody else's layout.
        //
        // ⚠ STATED, NOT DERIVED, FOR THE SAME REASON AS THE ONE ABOVE, and
        // reading Grid Inventory's own GridInventory_ui.ini was considered and
        // dropped: it does state its windows as x,y,w,h and rewrites them when
        // you move one, but it says nothing about which of them are OPEN, so a
        // closed settings panel would take a third of the screen out of the
        // drag region and nothing on screen would explain why.
        //
        // ⚠ 0.66 IS A STARTING POINT, NOT A MEASUREMENT, which is the honest
        // difference between this and the 0.44 above. Grid Inventory's window
        // moves and scales with the player's own layout, so there is no one
        // right answer to measure; this clears the default window and the panel
        // has sliders for the rest. StudioCamera logs the first drag it takes
        // here with its coordinates, so a wrong boundary is one grep.
        float gridZoneLeft = 0.66f;
        float gridZoneTop = 0.0f;
        float gridZoneRight = 1.0f;
        float gridZoneBottom = 1.0f;
        // Hide the compass and crosshair while the studio is armed over a menu
        // that leaves the HUD drawing. The four vanilla menus put the game in
        // menu mode and the HUD stands itself down, so this only ever reaches a
        // third-party menu drawing its own UI. INI bHideHudOverCustomMenus.
        bool  hideHudOverCustomMenus = true;
        bool  bypassCameraCollision = true;  // skip the third-person camera pull-in while armed
        bool  standardizeLighting = true;    // neutral studio light + unified void color (interiors)
        bool  studioLightWithoutSpace = false;  // F-24: also standardize in Off / Scene view
        bool  verboseLog = true;      // per-second instrumentation while armed
        float maxDeltaTime = 0.05f;   // dt clamp (seconds)

        // The space around the player while armed.
        // 0 = off, 1 = SCENE VIEW (room stays with its own lighting; NPCs
        // + furniture hidden), 2 = VOID (everything hides; studio lighting
        // + rig, NO stage), 3 = DRESSING ROOM (the stage builds in the
        // void). Migration: pre-r17 mode 2 with bBackdrop=1 becomes 3.
        int   declutterMode = 2;   // r45 (user): the VOID is the default view
        float soloHideRadius = 4096.0f;    // modes 1+2: units around the player
        bool  hideLightRefs = true;        // mode 2: hide flame/smoke art on light refs too
        bool  cutCellLights = true;        // r33: self-cull cell NiLights (illumination off)
        // r46: the same idea outdoors, where the r33 cut cannot reach. The sun
        // is not in the cell's scene graph and an exterior has no INTERIOR_DATA
        // to standardize, so until now a void outdoors was a hidden world still
        // lit by full daylight. Void family only - see SunParkPolicy for why
        // this one cannot borrow CellLightAllowed().
        bool  cutSunLight = true;          // INI bCutSunLight
        // Stand the player down while another mod's framed companion is the
        // subject. Measured rather than assumed: the 13:38 field run put him
        // between the lens and her in four readings out of six, once at 2
        // degrees off the axis, and NEARER the camera than her in all six. A
        // bearing change cannot fix that, because how far off-axis he stands
        // depends on where she walked to and swung from 71 degrees to 2 across
        // one session.
        //
        // ⚠ ALL OR NOTHING, never "hide him when he happens to occlude". A
        // threshold makes him blink in and out as she moves, which reads far
        // worse than either state. He is hidden whenever she is the subject.
        bool  hidePlayerForCompanion = true;  // INI bHidePlayerForCompanion

        // OS-107. TURN THE FRAMED COMPANION TO FACE THE LENS.
        //
        // ⚠ THIS IS THE THING SPII ALREADY DOES FOR THE PLAYER, and its absence
        // is why the follower shot is the inconsistent one. The player is turned
        // to face the camera on every menu open, so his bearing to the lens is a
        // constant and the shot always reads the same. Nothing turns a follower,
        // so she is presented at whatever heading her AI last left her at, and
        // the same follower frames front-on, in profile or from behind on
        // successive opens. Held for the arm and restored on exit, like every
        // other borrowed piece of her state.
        bool  faceCompanionToCamera = true;  // INI bFaceCompanionToCamera

        // Degrees off dead-on, applied to the bearing above. 0 is face the lens,
        // which is what the player gets. Positive turns her clockwise seen from
        // above, so ~30 gives a three-quarter portrait rather than a mugshot.
        float companionFacingOffset = 0.0f;  // INI fCompanionFacingOffset
        bool  voidEngine = true;           // r37 F-20: cull grass/land/LOD/precip at their
                                           // engine roots + zero frozen imods (modes >=2)

        // [Filter] - an optional colour grade over the whole menu scene, in ANY
        // view (a uniform imagespace wash: colour tint + saturation + brightness,
        // delivered through the ImageSpaceManager base override in SceneTint.cpp).
        // OFF by default. It grades everything in frame, the character INCLUDED -
        // a true world-not-character filter needs a stencil / two-layer
        // compositor pass the plugin can't do (deferred). Defaults to a warm,
        // gently-faded look for whoever wants it.
        bool      colorFilter = false;             // master toggle (off by default)
        RE::Color tintColor{ 200, 170, 120, 0 };   // warm sepia wash
        float     tintStrength = 0.5f;             // 0..1 tint amount (TNAM)
        float     tintSaturation = 0.6f;           // 0..1 (CNAM, washed look)
        float     tintBrightness = 0.9f;           // 0..1.5 (CNAM); 1.0 = neutral

        // r39: graph BOOLs that freeze the arm like an attack when true -
        // dodge mods read as plain locomotion otherwise (TK Dodge RE/TUDM
        // publish bIsDodging; DMCO spellings carried; unknown names are
        // free no-ops). INI sFreezeGraphBools, comma-separated.
        std::vector<std::string> freezeGraphBools{ "bIsDodging",
                                                   "DMCO_IsDodging",
                                                   "bDMCO_IsDodging" };

        // DIAGNOSTIC (2026-07-20). Extra graph variables dumped at every pump
        // boundary and on a live weapon change. The variable that selects the
        // VISIBLE idle is demonstrably NOT iRightHandEquipped - field 12:20:22
        // read `iRightHandEquipped=2 weaponType=2` while the character stood in
        // the two-handed idle holding a dagger - so this is a net cast for one
        // that does track it. Names below are CANDIDATES, not confirmed: an
        // unknown name fails all three typed reads and costs nothing, which is
        // exactly what makes widening the list safe. INI sDiagGraphVars,
        // comma-separated, so a new name can be tried without a rebuild.
        std::vector<std::string> diagGraphVars{ "iLeftHandEquipped",
                                                "iState",
                                                "iSyncIdleLocomotion",
                                                "iWantBlockBash",
                                                "bEquipOk",
                                                "bIsEquipping",
                                                "bIsUnequipping",
                                                "bMotionDriven",
                                                "bIsSynced",
                                                "bWantCastLeft",
                                                "bWantCastRight",
                                                "bIsStaggering",
                                                "bIsBashing",
                                                "bAllowRotation",
                                                "bHeadTracking",
                                                "IsBlocking",
                                                "IsBashing",
                                                "bBowDrawn",
                                                "bIsAttacking" };
        // r20 EXPERIMENT - REFUTED 2026-07-20, kept OFF and kept around.
        //
        // Runs a cross-class swap across REAL frames instead of pumping it
        // inside one, to test whether the in-frame pump was what stopped the
        // graph re-picking the idle. It was not: the field ran a full 146-frame
        // sheathe and 105-frame redraw, neither capped, and the pose was still
        // wrong. Frame boundaries are eliminated.
        //
        // DEFAULT FALSE because with the theory dead this is strictly worse
        // than r13 - same wrong pose, plus ~2s of visible animation per swap.
        // Kept rather than deleted because the code is the cheap half of
        // re-running it if the OAR line of enquiry gives a reason to.
        bool  slowSwapExperiment = false;  // INI bSlowSwapExperiment

        // r22 A/B. TRUE is r13's behaviour: a cross-class swap sheathes fully
        // and redraws, both pumped. FALSE skips the sheathe and falls through
        // to PumpSwap, the path a same-class swap already takes.
        //
        // This exists because r21's event probe found the only difference
        // between a working live swap and a broken menu swap, and it is this
        // detour. The engine NEVER sheathes on a weapon swap: it goes
        // tailCombatIdle -> BeginWeaponDraw -> WeapEquip_Out and stays drawn.
        // Ours drops through MTState / tailMTIdle, the sheathed movement idle,
        // on the way past. The draw prologues differ too - ours raises
        // AnimObjectUnequip + IdleOffsetStop (a draw FROM SHEATHED), the
        // engine's raises DisableBumper (a REPLACE WHILE DRAWN). Two different
        // transitions, not one with extra steps in front.
        //
        // ⚠ DEFAULT FLIPPED TO FALSE IN r26, ON FIELD EVIDENCE. With r25 on,
        // the log shows every cross-class swap running TWICE: our detour goes
        // first and STILL picks the wrong clip (2HW_Equip for an Iron Dagger),
        // then ~14 ms later the engine's own notification plays the right one
        // (Dag_Equip). r13 is not merely redundant now, it is a wrong-clip
        // transition that runs ahead of the correct one.
        //
        // Kept rather than deleted because it is the fallback if r25 ever has
        // to be turned off, and because deleting it would erase the record of
        // why it existed. r13's original premise - "the engine queues its own
        // replace and it never gets to finish" - is REFUTED: it is not queued
        // and it never starts. See EquipNotifyGate.h for the real chain.
        bool  crossClassSheatheRedraw = false;  // INI bCrossClassSheatheRedraw

        // r25. Let the engine's own equip notification run inside a menu, by
        // answering Unk_B3 "do not bail" while armed. See EquipNotifyGate.h for
        // the whole chain - the short version is that our own kPausesGame menu
        // increments UI::numPausesGame, which is the counter the engine reads
        // to decide to SKIP the notification, so the graph is never told the
        // weapon changed and a forced draw plays the old weapon's clip.
        //
        // ⚠ DEFAULT FLIPPED TO TRUE IN r26. Field-confirmed, and confirmed by
        // the LOG rather than by the symptom: `equip notify gate: r25 ACTIVE`
        // followed by `clip [MENU] Dag_Equip.hkx` where every previous run had
        // `2HW_Equip.hkx`. The path provably ran and provably changed the clip.
        //
        // r26 pairs it with a pump of the engine's own equip clip (see
        // EquipNotifyGate's OnItemEquipped hook) - without that, r25 starts the
        // right animation and lets it play out visibly, which is what the field
        // saw as a bow unsheathing "sometimes".
        bool  liveEquipNotifyInMenus = true;  // INI bLiveEquipNotifyInMenus

        // r27. The animation-event / clip / actor-tick probes that found the
        // wrong-idle bug. OFF for release and not merely quiet: when false the
        // hooks are never INSTALLED at all, so they cost nothing.
        //
        // That matters for the clip probe in particular - it sits on
        // hkbClipGenerator::Activate, which every actor in the cell goes
        // through. Cheap per call, but it is not a thing to leave in a shipped
        // build for no reason. The event probe and the tick probe also write a
        // line per graph event, which is log spam a user would report.
        //
        // Kept in the source because they are the only reason r21-r26 got
        // anywhere: the bug was invisible to every value we could read, and
        // these read the graph's actual behaviour instead.
        bool  diagnosticProbes = false;  // INI bDiagnosticProbes

        // ⚠ THE CAMERA WATCH GETS ITS OWN KEY, AND IT DEFAULTS ON. It used to
        // ride on bDiagnosticProbes, which is not a logging-verbosity dial: it
        // is an installer for three unrelated probes and one of them is
        // UNBOUNDED (the anim-event probe writes a line per graph event, and its
        // sink does not re-read this setting once installed). So the only way to
        // ask a user for camera evidence was to ask them to turn on something
        // that floods their log.
        //
        // This one is bounded by construction and costs nothing when idle. It
        // logs only in the window after a bubbled menu closes: one close line,
        // one roster line, at most twelve dense samples, change samples capped
        // at the emit budget, one verdict and one expiry summary. Call it 15 to
        // 25 lines per close, 65 in the worst case. In gameplay it is four
        // relaxed atomic loads a frame and nothing else.
        //
        // It is on because the alternative has a cost too, and we have been
        // paying it: the 2026-08-09 field report of a camera that cannot pitch
        // after an inventory close arrived with a log that could not answer it,
        // and the probe that would have named the field in one line was both
        // switched off and pointed at the wrong menu.
        bool  cameraCloseProbe = true;  // INI bCameraCloseProbe

        // OS-103(b). The player-cull PHASE probe: which part of the frame
        // clears the cull we set on him for a framed companion. OFF for
        // release, and it gets its own key rather than riding on
        // bDiagnosticProbes for the reason bBlinkStressTest does: those are the
        // animation instrumentation, this is a scenegraph race, and turning one
        // on to chase the other is how a probe ends up measuring the wrong
        // thing. ⚠ It WRITES the cull once per armed frame while on, which is
        // the only way each frame has something left to lose; that is
        // instrumentation, not the backed-out re-assert returning. See
        // Declutter::ProbePlayerCull.
        bool  playerCullProbe = false;  // INI bPlayerCullProbe

        // Bake the face MESH each armed tick (Offsets::FaceGenApplyMorphs).
        // Without this the face DATA moves and the mesh never does - the
        // paused-menu blink bug. Kill-switch only: it calls an engine
        // function on every armed frame, so a way to turn it off without a
        // rebuild is worth six lines.
        bool  faceMeshRefresh = true;   // INI bFaceMeshRefresh

        // Blink stress test: caps the ENGINE's own blink countdown to 0.5 s so
        // the character blinks ~twice a second. Forges nothing - it only
        // shortens a timer - and it is what made the paused-menu blink fix
        // confirmable by eye instead of by impression.
        //
        // It used to ride on bDiagnosticProbes, which was wrong: the probes are
        // the animation instrumentation and this is a face stress test, so
        // turning the first on to chase a graph bug also made the face strobe.
        // Undocumented, its own key.
        bool  blinkStressTest = false;  // INI bBlinkStressTest

        // Stop each weapon-preview pump the moment the draw/sheathe it is
        // driving reaches an idle, instead of running a fixed step budget past
        // it. See the pump helper in WeaponPreview.cpp for the field evidence.
        //
        // ⚠ DEFAULT OFF ON PURPOSE, for now. The measurement that selects this
        // fix (idle picks inside pumps vs on ticked frames) has not been taken
        // on the repro yet, and this channel has shipped a broad fix for an
        // unreproduced symptom twice. Run the repro once with it off, read the
        // "idle session:" line, THEN turn it on and run the same repro again -
        // the same build answers both, so it costs one launch and no rebuild.
        bool  pumpStopsAtIdle = true;  // INI bPumpStopsAtIdle

        // Let a menu armed mid-draw WHILE MOVING skip the draw/sheathe hold.
        // Default OFF: it hands the arm to a "locomotion settle" that does not
        // settle, and the caught walk clip plays on for the whole menu. See the
        // latch's own note in Bubble.cpp.
        bool  movingArmStandsAside = false;  // INI bMovingArmStandsAside

        // ── The two freezes a user might reasonably want to switch off. ──────
        //
        // Menu Studio freezes a caught pose whenever letting it animate would
        // look worse than holding it. Mid-air, mid-attack, furniture and
        // scripted idles are not negotiable: there is nothing to settle into and
        // ticking them produces nonsense. These two ARE arguable, because both
        // trade a live pose for a still one to dodge an engine behaviour, and
        // some people would rather have the live pose and take the jank.
        // freezeUnsettledPose / bFreezeUnsettledPose was removed in 0.7.5. Its
        // only consumer was the settle loop 0.7.4 deleted, so it had become a
        // control that saved and did nothing.
        bool  freezeDrawSheathe   = true;  // INI bFreezeDrawSheathe

        // FREEZE THE CHARACTER (opt-in): hold the exact frame the menu caught -
        // no behaviour graph, no settle-to-idle, no face. Hair and cloth, body
        // physics, spell hand art and the weapon preview all stay live, so the
        // preview still reacts to a spin and still shows what you equip; only
        // the character's own ANIMATION stops. Off by default: the live preview
        // is the mod's whole point, and this is for people who want a still.
        bool  freezeCharacter = false;  // INI bFreezeCharacter
        bool  driveCbpc = true;            // CBPC body physics while paused (fingerprinted)

        // Studio-light look, resolved at load from [Lighting] sPreset +
        // per-channel overrides (StudioLight consumes these; only applies
        // in void mode - scene view keeps the cell's own lighting).
        RE::Color lightAmbient{ 96, 96, 100, 0 };
        RE::Color lightDirectional{ 160, 155, 150, 0 };
        RE::Color lightFog{ 13, 13, 15, 0 };      // = the void color
        RE::Color lightFill{ 80, 80, 84, 0 };     // DALC all-axis fill

        // r45 (user ask: "color picker to change the voidsphere color"):
        // when the override is on, the shell tints to EXACTLY this color -
        // no vibe hue, no luminance cap (the pick is the pick). Off = the
        // capped fog+ambient-hue vibe flow, unchanged.
        bool      voidColorOverride = false;
        RE::Color voidColor{ 13, 13, 15, 0 };
        float     lightFogNear = 3000.0f;
        float     lightFogFar = 6000.0f;
        std::string lightPreset = "studio";
        bool      studioRig = true;        // key/fill/rim point lights in the void
        bool      rigWithoutSpace = false;  // F-24: also light the character in Off / Scene view

        // r28. Keep the studio rig in menus Skyrim Souls holds LIVE.
        //
        // Before this, a live menu made the bubble dormant and the whole panel
        // collapsed to "the studio is off" - a Souls user lost the three-point
        // lighting along with everything else, which is what they actually
        // wanted from the mod. The lighting never needed the pause.
        //
        // Only the RIG. Deliberately NOT the void, the backdrop or the
        // declutter: those hide the world, and a live menu is live precisely so
        // the player can still see what is happening to them. Blanking the
        // scene while a dragon is landing on you is not a feature. The drives
        // (animation, physics, face) are not needed either - they exist only to
        // undo a pause, and Bubble's engineAnimates already stands them down.
        //
        // DEFAULT TRUE: it only ever adds back something that was previously
        // lost, and it costs nothing when the rig itself is off.
        bool      studioInLiveMenus = true;  // INI bStudioInLiveMenus

        // RUNTIME ONLY, never read from or written to the INI. TRUE while a
        // Souls-live menu session is being lit. Same pattern as declutterMode's
        // per-menu override: Bubble owns it for the length of a session.
        bool      liveStudioActive = false;
        float     rigBrightness = 0.2f;    // multiplies each rig light's fade (r59: dimmer default)

        // Per-light three-point controls (colors 0-255; intensity multiplies
        // that light's base fade). Applied LIVE every armed tick.
        struct RigLight {
            bool      enabled;
            RE::Color color;
            float     intensity;
        };
        RigLight rigKey{ true, { 255, 242, 222, 0 }, 1.0f };
        RigLight rigFill{ true, { 184, 199, 230, 0 }, 1.0f };
        RigLight rigRim{ true, { 255, 255, 255, 0 }, 1.0f };

        // Backdrop pieces, demand-loaded from the game's own meshes and
        // cloned render-side - no plugin, no placed refs, no save surface.
        // BACKGROUND (dome fields) surrounds you in Void + Dressing room;
        // STAGE (floor fields + extras) builds in the Dressing room. The
        // fields hold the ACTIVE presets' values (+ INI/panel overrides);
        // empty mesh = piece dropped. Radii are the world size each mesh
        // is SCALED to (bound radius).
        std::string backdropStage = "starlight";
        // ⚠ EMPTY IS "no dome, just the void colour", and it is the default
        // (user, 2026-08-27). The installer used to ask which dome hung behind
        // the character; the picker in the panel is a better place to answer
        // that, and a plain void is the one that competes least with what the
        // character is wearing.
        std::string backdropBackground = "";
        std::string backdropFloorMesh = "clutter\\nightingale\\nightingaleplatform.nif";
        std::string backdropDomeMesh = "interface\\intperkskydome.nif";
        // F-7 v3: opaque SHELL behind the (additive, see-through) star dome
        // - the void's skin, colored per the vibe. Default: OUR OWN shipped
        // mesh (dist/meshes/mtb/voidshell.nif - inverted sphere, unlit
        // effect shader, flat white texture): texturally FLAT, so the void
        // reads one solid color in every cell. The r29/r30 vanilla
        // candidates are convicted - loadscreen sphere (2nd geometry = wall
        // at fit scale), vampire dome (seams + per-cell color shift + its
        // starry texture masquerading as the constellation). Empty = off.
        std::string backdropShellMesh = "mtb\\voidshell.nif";
        // r49 shipped the vanilla stars mesh here; r50 pulled it back OFF
        // by default - fitted to our sphere it renders as a white VEIL
        // over the whole view (field screenshot), not star points. The key
        // stays for experiments (sStarsMesh in [Backdrop]).
        std::string backdropStarsMesh = "";
        float       backdropFloorRadius = 600.0f;
        float       backdropDomeRadius = BackdropPolicy::kBackgroundRadiusDefault;
        float       backdropFloorZ = -10.0f;
        float       backdropDomeZ = 0.0f;
        float       backdropBrightness = 1.0f;  // emissive tint strength
        // Image packs render emissive over a studio tuned for DARK content
        // (the nebula sits at 0.02-0.16 luminance; ENB effect gain on top),
        // so an as-authored daylight picture blows out white. This scales the
        // image sphere's baseColorScale, live from the panel.
        float       backdropImageBrightness = 0.35f;
        // r33: hard luminance ceiling on the dome/shell tint - the void is
        // dark in EVERY look and cell; the vibe varies only its hue.
        float       voidBrightnessCap = 0.10f;

        // r57 custom-image composition: lock the background sphere to FACE
        // THE CAMERA so a hand-made texture on the void shell presents its
        // front consistently, no matter which way the player faced when the
        // menu opened (and it tracks if the camera orbits). Off (default) =
        // the world-locked dynamic look the user likes for the star domes.
        // The yaw offset (degrees) rotates the image to taste.
        bool        backgroundFaceCamera = false;
        float       backgroundYawOffset = 0.0f;
        // The active background's image (DDS) wrapped on the image sphere; empty
        // means use the mesh's own baked texture. ApplyBackgroundPreset copies it
        // from the selected background preset.
        std::string backdropBackgroundImage = "";

        // Match time & season ("the mood"): pick the whole look (background +
        // rig) from the game clock - dawn/day/sunset/evening/night base
        // preset, tinted by the calendar season. Manual preset + overrides
        // apply when OFF. Default ON for fresh installs; an explicit saved
        // value still wins when the INI is loaded. The panel disables manual
        // mood and three-point rig controls while this is on.
        bool matchTimeAndSeason = true;

        // What consumers actually render: manual = the fields above;
        // auto = computed from Calendar. Enable flags always follow the
        // user's rig layout.
        struct LookValues {
            RE::Color ambient, directional, fog, fill;
            RigLight  key, fillLight, rim;
        };
        [[nodiscard]] LookValues    CurrentLook() const;
        [[nodiscard]] std::string DescribeTimeAndSeason() const;  // "21:12 Frostfall: warm + autumn"

        // The colour-filter values the SceneTint module renders. A plain snapshot
        // of the fields above (no time/season computation - the filter is manual),
        // mirroring LookValues / CurrentLook so consumers stay decoupled.
        struct TintValues {
            RE::Color color;
            float     strength;
            float     saturation;
            float     brightness;
        };
        [[nodiscard]] TintValues CurrentTint() const;

        // Bumped on every Load()/Save(); the bubble re-applies the studio
        // look mid-arm when it changes (live panel edits).
        std::uint32_t revision = 0;
        // ⚠ "RaceSex Menu" HAS A SPACE. It is the engine's own name for the
        // character editor and the only one a menu event ever carries; the
        // spaceless spelling is what made F-9's handling dead code for months.
        // Included so the studio is up in the editor the strip's own button
        // opens, which is what a player who clicked it is expecting to see.
        // Still EXPERIMENTAL: RaceMenu drives the character and the camera
        // itself, so the studio stands its own camera and spin down there
        // (raceMenuOpen_) and leaves the lighting and the space.
        // ⚠ GRID INVENTORY IS ON BY DEFAULT AND COSTS NOTHING WHEN IT IS NOT
        // INSTALLED. Its plugin registers a real IMenu called
        // 'GridInventoryMenu' and opens it INSTEAD of the vanilla inventory, so
        // without the name here the studio never armed in it and the action bar
        // never drew: a player with Grid Inventory got a plain menu and no
        // buttons (field, 2026-08-27). A menu nobody has installed never opens,
        // so the entry is inert rather than a risk. Unlike 'RaceSex Menu' it is
        // not experimental, it is simply somebody else's inventory.
        std::unordered_set<std::string> menus{ "ContainerMenu", "BarterMenu", "InventoryMenu",
                                               "MagicMenu", "GridInventoryMenu" };
        // §4b: the menus left LIVE for Skyrim Souls, a subset of `menus`.
        // EMPTY by default, which is precisely the pre-0.6.1 behaviour, so an
        // existing setup is untouched until the user asks for a split.
        std::unordered_set<std::string> soulsLiveMenus;

    private:
        Settings() = default;
    };
}

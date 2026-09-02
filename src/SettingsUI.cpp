#include "PCH.h"

#include "SettingsUI.h"

#include "Settings.h"

#include "ActionBar.h"
#include "Bubble.h"
#include "BackdropPacks.h"
#include "OwnView.h"
#include "StudioCamera.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callback typedefs

#include "FUCK_API.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "BackdropImagePolicy.h"  // Rooted, the one image-path spelling

namespace {
    // iDeclutterMode order: combo index == INI value (pre-r17 mode 2 with the
    // stage on migrates to 3 at load). Short labels; the explanation is a
    // hover tooltip (FLICK style, like Feet of Skyrim).
    constexpr std::array<const char*, 4> kViewModes{
        "Off", "Scene view", "Void", "Dressing room",
    };
    // ⚠ NO TRAILING FULL STOP ON ANY OF THESE. They are tooltips, and the house
    // rule is absolute: a multi-sentence tooltip keeps the stops between its
    // sentences and loses only the last one.
    //
    // ⚠ AND THE DOME IS NOT DRESSING-ROOM ONLY. The old dressing-room line
    // ("a stage builds itself around you, floor and dome and set pieces
    // included") sold the dome as this mode's own; Backdrop's IsVoidFamily
    // covers modes 2 and 3 alike, so what mode 3 actually adds is the floor
    // and whatever set pieces the chosen stage carries.
    constexpr std::array<const char*, 4> kViewModeTips{
        "Menus open over the world exactly as you left it",
        "The room stays as it is and only the NPCs and the furniture clear out",
        "Everything around you is hidden, leaving a lit void with no stage",
        "The void, plus a floor and any set pieces from the chosen stage",
    };

    // Capitalize the first letter for DISPLAY only. Preset .name fields double
    // as the INI key (ApplyXPreset / the saved-value comparisons use them), so
    // the key stays lowercase; only what the combo shows is title-cased.
    std::string TitleFirst(const char* a_name) {
        std::string s = a_name ? a_name : "";
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
            s[0] = static_cast<char>(s[0] - 'a' + 'A');
        }
        return s;
    }

    // Tooltip for the item just drawn (shown only on hover).
    void Tip(const char* a_desc) {
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip(a_desc);
        }
    }

    void ColorRow(const char* a_label, RE::Color& a_color, bool& a_dirty) {
        float rgb[3]{ a_color.red / 255.0f, a_color.green / 255.0f, a_color.blue / 255.0f };
        if (FUCK::ColorEdit3(a_label, rgb)) {
            a_color.red   = static_cast<std::uint8_t>(rgb[0] * 255.0f + 0.5f);
            a_color.green = static_cast<std::uint8_t>(rgb[1] * 255.0f + 0.5f);
            a_color.blue  = static_cast<std::uint8_t>(rgb[2] * 255.0f + 0.5f);
            a_dirty = true;
        }
    }

    // Skyrim Souls RE loaded? Its unpaused menus are the only reason the
    // force-pause control matters, so the panel only surfaces that control when
    // Souls is actually present. Checked once - a plugin cannot load mid-session.
    bool SkyrimSoulsPresent() {
        static const bool present = GetModuleHandleA("SkyrimSoulsRE.dll") != nullptr;
        return present;
    }

    // §4b: display order and labels for the per-menu Souls split. The order is
    // fixed here because Settings::menus is an unordered_set and a column of
    // checkboxes that reshuffles between frames is unusable. The ### suffix
    // keeps each checkbox's ImGui ID unique and stable while the visible text
    // stays short; FUCK sizes a label on the part in FRONT of the ###, so the
    // long id costs no width (see [[fuck-tab-item-sizing]]).
    constexpr std::array<std::pair<const char*, const char*>, 4> kSoulsMenuLabels{ {
        { "InventoryMenu", "Inventory###soulsLiveInventoryMenu" },
        { "BarterMenu", "Barter###soulsLiveBarterMenu" },
        { "ContainerMenu", "Container###soulsLiveContainerMenu" },
        { "MagicMenu", "Magic###soulsLiveMagicMenu" },
    } };

    // The bubble's own scope, on the Menus tab. Same fixed order and same ###
    // trick as the Souls list above, for the same two reasons: Settings::menus
    // is an unordered_set and a column of checkboxes that reshuffles between
    // frames is unusable, and the long id keeps each checkbox's ImGui identity
    // stable while the visible label stays short.
    //
    // ⚠ 'RaceSex Menu' IS DELIBERATELY ABSENT. It genuinely works - the studio,
    // the space and the lighting all arm in the character editor - but the way
    // OUT is unsolved: that menu owns a camera of its own and leaving it can
    // strand the view in the menu's framing. Three attempts did not hold. A
    // checkbox would make a known-bad exit one click away, so it stays an INI
    // edit, and Settings::Save writes it back for anyone who made that edit.
    constexpr std::array<std::pair<const char*, const char*>, 4> kBubbleMenuLabels{ {
        { "InventoryMenu", "Inventory###bubbleInventoryMenu" },
        { "MagicMenu", "Magic###bubbleMagicMenu" },
        { "BarterMenu", "Barter###bubbleBarterMenu" },
        { "ContainerMenu", "Container###bubbleContainerMenu" },
    } };

    // The panel body. FLICK's Combo is the array form only (no SMF-style
    // callback), and the preset .name fields are const char*, so each preset
    // combo gathers its names into a temp vector.
    void DrawPanel() {
        auto& cfg   = MTB::Settings::GetSingleton();
        bool  dirty = false;
        const ImVec4 kWarn{ 0.95f, 0.75f, 0.25f, 1.0f };

        // The 3-point studio rig for the Void / Dressing room; drawn in the
        // Lighting section below.
        // ⚠⚠ THE CLOCK DECIDES LESS THAN THE PANEL USED TO PRETEND, AND THAT
        // WAS THE WHOLE "disabled wall". Every control below sat inside one
        // BeginDisabled(matchTimeAndSeason), so with the clock driving - which
        // is the SHIPPED DEFAULT - the tab read as a page of dead sliders.
        // Settings::CurrentLook is the authority on what the clock actually
        // overrides, and it is exactly two things per light: the COLOUR and the
        // INTENSITY. It copies the RigLight wholesale first and then writes
        // `.color` and `.intensity` alone, so `enabled` survives, and
        // studioRig / rigWithoutSpace / rigBrightness are not in LookValues at
        // all. Six working controls were greyed out, and one of them was
        // "Studio rig" itself - the master switch for the entire rig, which a
        // fresh install therefore could not reach without first turning the
        // clock off. Only the two the clock really owns are locked now.
        //
        // ⚠ AND A LOCKED CONTROL SHOWS THE CLOCK'S VALUE, NOT THE STALE MANUAL
        // ONE. A greyed swatch displaying a colour that is not the colour on
        // screen is worse than no swatch: it answers "what is lighting my
        // character" wrongly. a_look carries what is actually being rendered.
        const auto drawRig = [&](const MTB::Settings::LookValues& a_look,
                                 bool a_clockOwnsColours) {
            dirty |= FUCK::Checkbox("Studio rig (key / fill / rim lights)",
                                    &cfg.studioRig);
            Tip("Key, fill and rim lights on your character, like a photo studio");
            if (cfg.studioRig) {
                dirty |= FUCK::Checkbox("Also light your character in Off and Scene view",
                                        &cfg.rigWithoutSpace);
                Tip("Studio lights on your character with the world still behind you, "
                    "though anything standing close catches them too");
                if (FUCK::SliderFloat("Rig brightness", &cfg.rigBrightness, 0.0f, 3.0f, "%.2f")) {
                    dirty = true;
                }
                Tip("Overall strength of the studio rig. 0 turns it off. The clock "
                    "never touches this one, so it works either way");
                const auto rigRow = [&](const char* a_label,
                                        MTB::Settings::RigLight& a_light,
                                        const MTB::Settings::RigLight& a_shown) {
                    FUCK::Text("%s", a_label);
                    FUCK::SameLine(90.0f, 0.0f);
                    std::string id = std::string("##en") + a_label;
                    // The ONE checkbox that keeps the manual args. This row
                    // hand-places a hidden-label tick between its own Text and
                    // the colour swatch, so alignFar would fling it to the far
                    // right and break the row. Every LABELLED checkbox in this
                    // panel now takes FLICK's defaults (alignFar + labelLeft)
                    // instead, which is what lines them up with the sliders.
                    //
                    // ⚠ AND IT STAYS LIVE UNDER THE CLOCK. Turning a light off
                    // is a layout decision, not a mood one, and CurrentLook
                    // carries the player's answer through untouched.
                    dirty |= FUCK::Checkbox(id.c_str(), &a_light.enabled, false, false);
                    FUCK::SameLine(0.0f, 8.0f);
                    // Scratch copies while the clock owns them: the widgets are
                    // disabled so nothing can be written back, and displaying
                    // the rendered value is the point.
                    RE::Color shownColor = a_shown.color;
                    float     shownIntensity = a_shown.intensity;
                    FUCK::BeginDisabled(a_clockOwnsColours);
                    id = std::string("Colour##") + a_label;
                    if (a_clockOwnsColours) {
                        bool ignored = false;
                        ColorRow(id.c_str(), shownColor, ignored);
                    } else {
                        ColorRow(id.c_str(), a_light.color, dirty);
                    }
                    id = std::string("Intensity##") + a_label;
                    if (a_clockOwnsColours) {
                        FUCK::SliderFloat(id.c_str(), &shownIntensity, 0.0f, 3.0f, "%.2f");
                    } else if (FUCK::SliderFloat(id.c_str(), &a_light.intensity,
                                                 0.0f, 3.0f, "%.2f")) {
                        dirty = true;
                    }
                    FUCK::EndDisabled();
                };
                rigRow("Key", cfg.rigKey, a_look.key);
                rigRow("Fill", cfg.rigFill, a_look.fillLight);
                rigRow("Rim", cfg.rigRim, a_look.rim);
            }
        };

        // Pulled out of the Colour filter section (r28b) so the Souls-live
        // panel can show the same controls without duplicating them. The
        // filter is a screen grade and hides nothing, so it is safe with the
        // world running for the same reason the rig is. Caller draws the
        // SeparatorText, since the two placements want different headings.
        const auto drawColorFilter = [&] {
            dirty |= FUCK::Checkbox("Colour filter", &cfg.colorFilter);
            Tip("Washes the whole shot in a colour, your character included");
            if (!cfg.colorFilter) {
                return;
            }
            ColorRow("Filter colour", cfg.tintColor, dirty);
            Tip("The colour washed over the scene");
            if (FUCK::SliderFloat("Strength", &cfg.tintStrength, 0.0f, 1.0f, "%.2f")) {
                dirty = true;
            }
            Tip("How strongly the colour washes the scene (lower is greyer)");
            if (FUCK::SliderFloat("Saturation", &cfg.tintSaturation, 0.0f, 1.0f, "%.2f")) {
                dirty = true;
            }
            Tip("Lower is more faded and desaturated");
            if (FUCK::SliderFloat("Brightness", &cfg.tintBrightness, 0.0f, 1.5f, "%.2f")) {
                dirty = true;
            }
            Tip("Overall brightness of the filtered scene (1.0 leaves it as-is)");
        };

        dirty |= FUCK::Checkbox("Enable Menu Studio", &cfg.enabled);
        Tip("Master switch for the whole mod. When off, menus behave normally");

        // Skyrim Souls RE runs these menus unpaused; Menu Studio re-pauses them so
        // the studio has a frozen scene to work with. Offer the opt-out only when
        // Souls is present, framed as "keep them unpaused" - the inverse of the
        // stored forcePause. Checked hands the menus back to Souls, and with no
        // pause the whole studio is inert, so the options below hide.
        const bool souls = SkyrimSoulsPresent();

        // TABS. The page had grown into one long scroll - view modes, then the
        // per-menu space list, then the character rows, then the filter, the
        // background, the stage and finally the lighting rig - and finding any
        // one control meant reading past all the others. Feet of Skyrim's panel
        // solves the same problem the same way.
        //
        // Each section is a lambda here and a tab at the bottom. Defined in
        // source order, drawn in tab order: the two are deliberately not the
        // same, so nothing has to move to be re-grouped.
        const auto drawSouls = [&] {
            bool keepUnpaused = !cfg.forcePause;
            if (FUCK::Checkbox("Keep ALL these menus unpaused (Skyrim Souls)",
                               &keepUnpaused)) {
                cfg.forcePause = !keepUnpaused;
                // r28f: NEVER SILENT. A field session loaded with
                // forcePause=false and opened its menu with it true, and the
                // log could not say who flipped it - this checkbox, a stray
                // click, or a bug - because the click did not log. One line
                // makes the next such log self-explaining.
                spdlog::info("Panel: 'Keep ALL these menus unpaused' clicked -> "
                             "forcePause={} (checkbox now {}).",
                             cfg.forcePause, keepUnpaused ? "TICKED" : "UNTICKED");
                dirty = true;
            }
            Tip("Hands these menus back to Skyrim Souls and keeps them live, with no "
                "studio and nothing frozen to pose against");

            // §4b per-menu split. The studio needs a frozen scene and Souls
            // exists to keep menus live, so no single menu can be both - but
            // that is a per-menu conflict, not a global one, and this is where
            // it stops being all-or-nothing. Only worth showing while the
            // studio is on at all; when the master is checked every menu is
            // already live and there is nothing to divide.
            if (cfg.forcePause) {
                FUCK::TextDisabled("Or hand individual menus back to Souls:");
                // Fixed order, not set order: the set is unordered, and a list
                // of checkboxes that reshuffles between frames is unusable.
                for (const auto& [key, label] : kSoulsMenuLabels) {
                    if (!cfg.IsBubbleMenu(key)) {
                        continue;
                    }
                    bool live = cfg.soulsLiveMenus.contains(key);
                    if (FUCK::Checkbox(label, &live)) {
                        if (live) {
                            cfg.soulsLiveMenus.insert(key);
                        } else {
                            cfg.soulsLiveMenus.erase(key);
                        }
                        dirty = true;
                    }
                }
                // Every menu handed over is the same end state as the master
                // checkbox, reached a different way. Say so rather than leaving
                // a panel full of studio options that cannot apply anywhere,
                // but do NOT return: unchecking one of these is how they come
                // back, so the list has to stay reachable.
                std::size_t live = 0;
                for (const auto& m : cfg.menus) {
                    if (cfg.soulsLiveMenus.contains(m)) {
                        ++live;
                    }
                }
                if (!cfg.menus.empty() && live == cfg.menus.size()) {
                    FUCK::TextColored(kWarn,
                                      "Every menu is live, so the studio has nowhere to run.");
                }
            }
        };

        // Without a pause the studio cannot arm, so under Souls + keep-unpaused
        // most of the panel has nothing to configure. It used to say so and
        // RETURN, which cost a Souls user the whole page; now it says so once
        // above the tabs and the tabs that cannot apply simply do not appear.
        const bool studioInert = souls && !cfg.forcePause;
        if (studioInert) {
            // r28: this used to be the end of the panel. A Souls user who kept
            // their menus live lost the whole page, including the three-point
            // rig, which never needed the pause in the first place. The staging
            // half genuinely does need a still scene, so say which half is gone
            // rather than "the studio is off", and keep the half that works.
            FUCK::TextColored(kWarn,
                              "Skyrim Souls is keeping these menus live, so the void, "
                              "the backdrop and the live-physics posing are off.");
            FUCK::TextDisabled(
                "Untick \"Keep ALL these menus unpaused\" on the Skyrim Souls tab "
                "to pose in a frozen scene.");
            // ⚠ r28h: NO CHECKBOX for the live lighting. It had one, the field
            // turned it off while hunting for the missing lights, and the mod
            // then declined every menu in silence - a whole round lost to a
            // control that only existed to switch off the thing being tested.
            //
            // The user's standing instruction on this feature was "don't even
            // make them toggles, it works so just let it work with no settings
            // for it", and this is the second time ignoring that has cost a
            // round. bStudioInLiveMenus survives as an undocumented INI escape
            // hatch for a load order we have not seen; it is not a preference.
        }

        // MENUS - where the studio applies at all, and what has to happen before
        // it comes up. The bubble's own scope lives here, which is why the
        // owner-context switch does too: both answer "does the studio engage",
        // while every other tab answers "what does it do once it has".
        //
        // ⚠ NOT GATED ON studioInert, like Lighting and Buttons. A player whose
        // studio cannot arm is exactly the one who needs to reach this tab.
        const auto drawMenus = [&] {
            dirty |= FUCK::Checkbox("Only wake when a mod asks",
                                    &cfg.waitForOwnerContext);
            Tip("Menus stay vanilla until a mod that uses Menu Studio opens its "
                "own window over them");
            if (cfg.waitForOwnerContext && !MTB::Bubble::OwnerContextProven()) {
                // A switch that is on and provably doing nothing is worse than
                // one that is off, so say which of the two states this is in.
                // Nothing has published a context yet, so menus still bubble
                // exactly as they did, which is deliberate and would otherwise
                // be invisible.
                FUCK::TextDisabled(
                    "Nothing has asked yet this session, so menus still open as usual.");
            }

            FUCK::SeparatorText("Which menus");
            // ⚠ GREYED, NOT HIDDEN, WHILE THE STUDIO WAITS TO BE ASKED. With
            // that switch on, no menu wakes on its own, so this list has nothing
            // left to decide and offering it live would promise something it
            // cannot deliver. Hiding it would be worse: the player would be left
            // wondering where their menu list went, and the answer is one
            // checkbox above. Disabled says both things at once - it is still
            // there, and this is what turned it off.
            const bool menuListInert = cfg.waitForOwnerContext;
            FUCK::BeginDisabled(menuListInert);
            // ⚠ THE TIMING IS NOT A DETAIL, IT IS THE FIRST THING A PLAYER WILL
            // THINK IS BROKEN. Ownership of a menu is decided once, at its open,
            // and remembered until its close - the r19c rule that keeps a
            // mid-menu settings change from stranding an armed bubble in
            // gameplay. So unticking the menu you are standing in does nothing
            // until you close it, and a panel that did not say so would read as
            // a dead checkbox.
            FUCK::TextWrapped(
                "Untick one to leave that menu completely alone. A change here "
                "applies the next time that menu opens, not to the one you are "
                "in now.");
            for (const auto& [key, label] : kBubbleMenuLabels) {
                bool on = cfg.IsBubbleMenu(key);
                if (FUCK::Checkbox(label, &on)) {
                    if (on) {
                        cfg.menus.insert(key);
                    } else {
                        cfg.menus.erase(key);
                    }
                    dirty = true;
                }
            }
            if (cfg.menus.empty()) {
                FUCK::TextColored(kWarn,
                                  "No menus left, so Menu Studio has nowhere to run.");
            }
            FUCK::TextDisabled(
                "The character editor is left out on purpose. It works, but "
                "leaving it can strand the camera, so adding it is an INI edit.");
            FUCK::EndDisabled();
            if (menuListInert) {
                FUCK::TextDisabled(
                    "These are waiting on \"Only wake when a mod asks\" above.");
            }
        };

        // CHARACTER - your character, the camera that looks at them, and the
        // other 3D the menu puts on screen.
        //
        // ⚠ FOUR SECTIONS RATHER THAN ONE RUN OF FOURTEEN CONTROLS (author,
        // 2026-08-13: "it's quite messy"). The camera half is most of the tab
        // and it was a flat column with a single heading in the middle of it, so
        // finding "how far out can I go" meant reading past the pan range and
        // the drag region. The four camera groups answer four different
        // questions: how it moves, how near and far it goes, how the framing
        // slides, and where a drag is allowed to start.
        const auto drawCharacter = [&] {
            FUCK::SeparatorText("Your character");
            dirty |= FUCK::Checkbox("Always freeze the character", &cfg.freezeCharacter);
            // "Always" is load-bearing, not decoration: Menu Studio already
            // freezes on its own whenever a caught pose cannot settle (mid-air,
            // mid-attack, furniture, and a locomotion graph that will not leave
            // its walk clip). A plain "Freeze the character" read as "this is
            // the only time freezing happens", which is not true.
            Tip("Holds the caught frame in every menu, instead of only when a "
                "pose cannot settle on its own");
            dirty |= FUCK::Checkbox("Spin the character", &cfg.previewSpin);
            Tip("Turn your character with the right mouse or the right stick, "
                "hair and cloth swinging as they go");
            // Only meaningful with SPIM loaded - it is the one mod known to
            // rotate on the same right-drag. Hidden otherwise, like the Souls
            // toggle. Beside the spin it modifies now, rather than stranded at
            // the bottom of the tab underneath the camera settings.
            if (cfg.previewSpin && MTB::OwnView::SpimPresent()) {
                dirty |= FUCK::Checkbox("Override Show Player In Menus rotation",
                                        &cfg.overrideSpimRotation);
                Tip("Stops Show Player In Menus turning your character too, so "
                    "one drag no longer spins you twice as far");
            }

            FUCK::SeparatorText("Camera");
            dirty |= FUCK::Checkbox("Move the camera", &cfg.studioCamera);
            Tip("Drag with the left mouse to swing the camera around your "
                "character and roll the wheel to move in and out");
            if (cfg.studioCamera) {
                // ⚠ EVERY SLIDER OWNS ITS OWN TOOLTIP NOW, AND TWO DID NOT. Tip
                // describes the item JUST DRAWN, and these three sliders were
                // drawn in a row followed by two Tip calls - so both landed on
                // the third one, "Smoothing" showed the zoom step's text, and
                // "Swing speed" and "Zoom step" had no tooltip at all.
                FUCK::TextDisabled("How it moves");
                if (FUCK::SliderFloat("Swing speed", &cfg.cameraOrbitSensitivity,
                                      0.001f, 0.02f, "%.4f")) {
                    dirty = true;
                }
                Tip("How far the camera swings for a given drag of the mouse");
                if (FUCK::SliderFloat("Zoom step", &cfg.cameraTrackStep, 0.02f,
                                      0.20f, "%.3f")) {
                    dirty = true;
                }
                Tip("How far one notch of the wheel takes you along the whole "
                    "zoom range. The default crosses it in about fifteen notches");
                if (FUCK::SliderFloat("Smoothing", &cfg.cameraSmoothing, 0.0f, 1.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                Tip("How much the camera trails your hand. Left follows almost "
                    "exactly, right drifts after it");

                FUCK::TextDisabled("How near and far it goes");
                if (FUCK::SliderFloat("Closest approach", &cfg.cameraMinDistance,
                                      25.0f, 300.0f, "%.0f")) {
                    dirty = true;
                }
                Tip("How near the camera may come. Raise it if the view ends up "
                    "inside your character, lower it for a tighter close-up");
                if (FUCK::SliderFloat("Furthest out", &cfg.cameraMaxDistance, 0.0f,
                                      900.0f, "%.0f")) {
                    dirty = true;
                }
                Tip("How far out you can go. Set 0 to work it out from the space "
                    "around you instead, which in the Void and the Dressing room "
                    "is the dome you are standing in. You can always push a "
                    "little past the limit; the camera eases back");
                if (FUCK::SliderFloat("Opening distance", &cfg.cameraOpenDistance,
                                      60.0f, 600.0f, "%.0f")) {
                    dirty = true;
                }
                Tip("Where the camera starts when the menu opened on somebody "
                    "else, which happens in a follower editor. Menus framed on "
                    "the character you are circling keep their own distance");

                FUCK::TextDisabled("Keeping your framing");
                dirty |= FUCK::Checkbox("Open on the framing I left",
                                        &cfg.rememberFraming);
                Tip("The swing, zoom and slide you finished with come back the "
                    "next time a menu opens, so a full-body view stays a "
                    "full-body view. Recentre to go back to the opening shot "
                    "and it stops coming back");
                if (cfg.rememberFraming) {
                    // ⚠ SAYS WHETHER THERE IS ONE, because "it isn't working"
                    // and "I haven't made one yet" look identical from here.
                    if (cfg.framingSaved) {
                        FUCK::TextDisabled(
                            "Remembering a shot swung %.0f degrees, %s the "
                            "opening distance.",
                            cfg.framingYaw * 57.2957795f,
                            cfg.framingZoom > 0.0f ? "nearer than"
                            : cfg.framingZoom < 0.0f ? "further out than"
                                                     : "at");
                    } else {
                        FUCK::TextDisabled(
                            "Nothing remembered yet. Frame a shot you like and "
                            "close the menu.");
                    }
                }

                FUCK::TextDisabled("Sliding the framing");
                dirty |= FUCK::Checkbox("Middle click recentres",
                                        &cfg.middleClickRecentre);
                Tip("A middle click over your character puts the camera back "
                    "where the menu opened it. Holding and dragging the middle "
                    "button pans either way");
                if (FUCK::SliderFloat("Pan speed", &cfg.cameraPanSensitivity,
                                      0.0001f, 0.0015f, "%.5f")) {
                    dirty = true;
                }
                Tip("How fast the picture slides while you hold the middle mouse "
                    "and drag");
                if (FUCK::SliderFloat("Pan range", &cfg.cameraPanRange, 0.0f,
                                      1.5f, "%.2f")) {
                    dirty = true;
                }
                Tip("How far the framing may sit off your character sideways or "
                    "upward, as a share of their height. Push past the edge and "
                    "the shot eases back");
                if (FUCK::SliderFloat("Pan range downward",
                                      &cfg.cameraPanRangeDown, 0.0f, 2.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                Tip("How far the framing may go down. Larger than the other ranges "
                    "on purpose: a close shot sits at eye level, so the feet are "
                    "almost a whole body below it. Raise it if you cannot reach "
                    "them");

                // ⚠ THE ONE CONTROL THAT IS NOT A PREFERENCE. Whether a drag
                // reaches the camera or the menu is decided by this region, and
                // every UI skin puts its item list somewhere different, so a
                // player on a skin we have never seen needs to be able to move
                // it. Saying that plainly is the difference between a tuning
                // step and a bug report. Last in the section because it is the
                // one nobody should have to touch.
                FUCK::TextDisabled("Where dragging moves the camera");
                FUCK::TextWrapped(
                    "Dragging only moves the camera inside this part of the "
                    "screen, so clicking still works everywhere your menu puts "
                    "its list. Widen it if your UI keeps the list somewhere "
                    "else.");
                if (FUCK::SliderFloat("Left edge", &cfg.cameraZoneLeft, 0.0f, 1.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                if (FUCK::SliderFloat("Right edge", &cfg.cameraZoneRight, 0.0f, 1.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                if (FUCK::SliderFloat("Top edge", &cfg.cameraZoneTop, 0.0f, 1.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                if (FUCK::SliderFloat("Bottom edge", &cfg.cameraZoneBottom, 0.0f, 1.0f,
                                      "%.2f")) {
                    dirty = true;
                }
                if (FUCK::Button("Recentre the camera")) {
                    MTB::StudioCamera::ResetOffsets();
                }
                Tip("Puts the camera back where the menu opened it");
            }

            // ⚠ ITS OWN SECTION, OUTSIDE THE CAMERA BLOCK, AND "HIDE THE
            // FLOATING ITEM PREVIEW" MOVED OUT TO JOIN IT. That one was nested
            // camera switch, so a player who turned the camera off could not
            // reach it at all - the live example the context-gate design note
            // cites for why nesting a control inside an unrelated one is a bug
            // rather than a layout. Neither has anything to do with the camera
            // being on: the menu puts an item on the 3D stage either way.
            FUCK::SeparatorText("Floating item preview");
            dirty |= FUCK::Checkbox("Hide the floating item preview",
                                    &cfg.disableItemPreview3D);
            Tip("Hides Skyrim's floating item preview while you browse, leaving "
                "your character alone on the 3D stage");
            dirty |= FUCK::Checkbox("Enter inspect mode only with its control",
                                    &cfg.inspectNeedsHotkey);
            Tip("Keeps the wheel on your character until you use the inspect "
                "control; item controls work normally once inspect mode opens");
            // The two per-case freeze toggles live on the Experimental tab. They
            // were here, under a one-line caveat that could not be worded both
            // unambiguously and short enough to survive the panel width. A tab
            // whose NAME is the caveat solves it without any caption at all.
            // "Show my weapon in hand" was here and is GONE (user, 2026-07-21:
            // "it doesn't do anything and i don't think we should have it
            // anyways"). The bWeaponPreviewInMenus key still exists and still
            // works for anyone who has set it - only the control is withdrawn,
            // so nobody's saved INI changes meaning under them.
        };

        // SCENE - the space around you, and what fills it.
        // EXPERIMENTAL - settings that trade a known-good behaviour for a
        // livelier one, and can look wrong. The tab name is the warning, which
        // is why nothing in here needs a caption explaining itself.
        const auto drawExperimental = [&] {
            FUCK::TextDisabled("These trade a reliable pose for a livelier one.");
            FUCK::SeparatorText("Freezing");
            // "Freeze poses that cannot settle" lived here. Its only consumer
            // was the settle loop 0.7.4 deleted, so the checkbox moved, saved
            // and did nothing at all. A control that lies is worse than a
            // missing one; if the behaviour ever comes back, so does this.
            dirty |= FUCK::Checkbox("Freeze while drawing or sheathing",
                                    &cfg.freezeDrawSheathe);
            Tip("On, a menu opened part way through drawing or putting away a "
                "weapon holds that frame. Off, the animation finishes and can "
                "leave the wrong stance");
            if (cfg.freezeCharacter) {
                FUCK::TextColored(kWarn, "\"Always freeze the character\" is on, so "
                                         "these do nothing.");
            }
        };

        // The strip and everything on it. Its own tab rather than a corner of
        // Scene, because the list of buttons grows with the load order and a
        // section that changes size under the player belongs somewhere it can.
        const auto drawButtons = [&] {
            dirty |= FUCK::Checkbox("Buttons on screen", &cfg.actionBar);
            Tip("A small strip of buttons over the menu, for the mods that "
                "offer one");
            if (cfg.actionBar) {
                if (FUCK::SliderFloat("Buttons across", &cfg.actionBarX, 0.0f, 0.95f,
                                      "%.2f")) {
                    dirty = true;
                }
                Tip("Where the strip sits from the left edge of the screen");
                if (FUCK::SliderFloat("Buttons down", &cfg.actionBarY, 0.0f, 0.95f,
                                      "%.2f")) {
                    dirty = true;
                }
                Tip("Where the strip sits from the top of the screen");

                // ⚠ HERE RATHER THAN IN A THEME TAB OF ITS OWN. It is one
                // control, and a tab is navigation you have to find before you
                // can change anything. It sits with the strip because the strip
                // is what it visibly changes; if a second theme control ever
                // arrives, that is when a tab earns its place.
                FUCK::SeparatorText("Corners");
                {
                    int style = cfg.frameStyle;
                    // Order matches the INI values, which are the wire format.
                    const char* const kItems[] = { "Follow the theme (default)",
                                                   "Always carved",
                                                   "Always plain" };
                    if (FUCK::Combo("Corner style", &style, kItems, 3)) {
                        cfg.frameStyle = style;
                        dirty          = true;
                    }
                    Tip("How the button tiles are cornered. Auto is the default: "
                        "carved under the themes listed in sCarvedPresets, plain "
                        "everywhere else, and a theme switch needs no restart. The "
                        "carve was drawn to sit inside Vel'dun");
                }

                FUCK::SeparatorText("Which buttons");
                const auto buttons = MTB::ActionBar::Snapshot();
                if (buttons.empty()) {
                    FUCK::TextWrapped(
                        "Nothing has registered a button yet. Menu Studio adds its "
                        "own when a menu opens, and other mods add theirs as they "
                        "load.");
                } else {
                    FUCK::TextWrapped(
                        "Untick one to take it off the strip. Buttons other mods "
                        "add appear here on their own, and a new one always starts "
                        "switched on.");
                    for (const auto& b : buttons) {
                        // ⚠ THE CHECKBOX SHOWS THE OPPOSITE OF WHAT IS STORED.
                        // The setting is the HIDDEN set (see Settings.h for why
                        // it has to be), and a list of tickboxes reading "hide
                        // this" beside a heading reading "which buttons" is the
                        // kind of double negative nobody parses correctly at a
                        // glance. Shown is the honest label; the flip happens
                        // here, once.
                        bool shown = !cfg.IsActionHidden(b.id);
                        FUCK::PushID(b.id.c_str());
                        if (FUCK::Checkbox(b.label.c_str(), &shown)) {
                            if (shown) {
                                cfg.hiddenActions.erase(b.id);
                            } else {
                                cfg.hiddenActions.insert(b.id);
                            }
                            dirty = true;
                        }
                        FUCK::PopID();
                    }
                }
            }
        };

        const auto drawScene = [&] {
        FUCK::SeparatorText("Space");
        // The CONFIGURED mode: while a menu that opted out of the space is
        // open, cfg.declutterMode is a temporary 0, so the panel must edit and
        // display declutterModeIni or it would fight the per-menu resolve.
        if (int mode = cfg.declutterModeIni;
            FUCK::Combo("Space around you", &mode, kViewModes.data(),
                        static_cast<int>(kViewModes.size()))) {
            cfg.declutterModeIni = mode;
            cfg.declutterMode    = mode;  // live for the menu already open
            dirty = true;
        }
        Tip(kViewModeTips[cfg.declutterModeIni]);
        // Per-menu space (NymerethRole): keep the backdrop for your own
        // character menus and drop it where you are looking at someone else's
        // things. Only the space is affected - the pause, the physics and the
        // live character still apply in every menu the mod covers.
        if (cfg.declutterModeIni != 0) {
            const auto spaceMenuRow = [&](const char* a_label, const char* a_menu,
                                          const char* a_tip) {
                bool on = cfg.spaceMenus.contains(a_menu);
                if (FUCK::Checkbox(a_label, &on)) {
                    if (on) {
                        cfg.spaceMenus.insert(a_menu);
                    } else {
                        cfg.spaceMenus.erase(a_menu);
                    }
                    dirty = true;
                }
                Tip(a_tip);
            };
            FUCK::TextDisabled("%s", "Show that space in:");
            spaceMenuRow("Inventory", "InventoryMenu",
                         "Your own inventory, the usual place to look at your character");
            spaceMenuRow("Magic", "MagicMenu", "Your own magic menu");
            spaceMenuRow("Container", "ContainerMenu",
                         "Chests, and looting followers or bodies. Turn this off if you "
                         "only want the backdrop for your own menus");
            spaceMenuRow("Barter", "BarterMenu",
                         "Trading with merchants. Turn this off if you only want the "
                         "backdrop for your own menus");
        }
        // NO PANEL ROW for the weapon-swap stance fix. It is correct behaviour,
        // not a preference: without it the character holds a new weapon in the
        // old weapon's stance, which nobody would choose. bLiveEquipNotifyInMenus
        // stays in the INI as an escape hatch for a load order we have not seen,
        // deliberately undocumented so it does not read as a supported choice.
        dirty |= FUCK::Checkbox("Camera ignores walls", &cfg.bypassCameraCollision);
        Tip("The camera slips through walls instead of shoving in close, which "
            "reads better in the void than in a real room");

        // The colour filter moved to the Lighting tab - it is a look, not a
        // piece of the scene, and it belongs beside the mood and the rig.

        FUCK::SeparatorText("Background");
        // The background picker is CARDS (user 2026-08-20, FR-style): a
        // picture is picked by its look, not by a name in a combo. Two
        // accordions - the shipped backgrounds and the installed packs;
        // an image pack's card shows the image itself, anything without one
        // (star domes, the blank sphere) draws a named tile. The click does
        // exactly what the old combo did.
        const auto backgrounds = MTB::Settings::BackgroundPresets();
        int bgIdx = -1;
        for (int i = 0; i < static_cast<int>(backgrounds.size()); ++i) {
            if (cfg.backdropBackground == backgrounds[i].name) {
                bgIdx = i;
            }
        }
        // Thumbnails load once per image path and live for the session; a
        // format FUCK cannot read falls back to the named tile, so the picker
        // never depends on the loader.
        static std::unordered_map<std::string, FUCK::Image> s_thumbs;
        const auto thumbFor = [&](const MTB::BackgroundPreset& a_bg) -> FUCK::Image* {
            const bool hasThumb = a_bg.thumb && *a_bg.thumb;
            const bool hasImage = a_bg.image && *a_bg.image;
            if (!hasThumb && !hasImage) {
                return nullptr;
            }
            auto it = s_thumbs.find(a_bg.name);
            if (it == s_thumbs.end()) {
                // An explicit thumb= first (the sky harvest bakes one from
                // the dome's own texture), then the image's baked PNG
                // sibling (FUCK reads PNG, not DDS), then the DDS itself as
                // a last attempt.
                FUCK::Image img;
                if (hasThumb) {
                    img = FUCK::Image{
                        MTB::BackdropImagePolicy::Rooted(a_bg.thumb).c_str()
                    };
                }
                if (!img.IsLoaded() && hasImage) {
                    const std::string rooted = MTB::BackdropImagePolicy::Rooted(a_bg.image);
                    img = FUCK::Image{
                        MTB::BackdropImagePolicy::ThumbSibling(rooted).c_str()
                    };
                    if (!img.IsLoaded()) {
                        img = FUCK::Image{ rooted.c_str() };
                    }
                }
                if (!img.IsLoaded()) {
                    spdlog::info("panel: no readable thumbnail for '{}', the card "
                                 "shows its name instead.", a_bg.name);
                }
                it = s_thumbs.emplace(a_bg.name, std::move(img)).first;
            }
            return it->second.IsLoaded() ? &it->second : nullptr;
        };
        // A label longer than its card bleeds into the neighbour (field
        // screenshot: names overlapping columns); trim to the card's width.
        const auto fitLabel = [](const std::string& a_name, float a_width) {
            if (FUCK::CalcTextSize(a_name.c_str()).x <= a_width) {
                return a_name;
            }
            std::string s = a_name;
            while (s.size() > 1 &&
                   FUCK::CalcTextSize((s + "..").c_str()).x > a_width) {
                s.pop_back();
            }
            return s + "..";
        };
        // The image button pads the picture with FramePadding on every side,
        // so a card on screen is wider than kCardW. The field grid ran off
        // the right edge when the wrap counted columns from the bare width;
        // the wrap measures the drawn card instead and asks whether one more
        // of them still fits. Computed HERE, above the lambda, because the
        // box height below is built from the same numbers: two copies of
        // this arithmetic is how the box and its rows drift apart.
        constexpr float kCardW = 120.0f, kCardH = 60.0f, kGap = 6.0f;
        // Card names draw below panel size: at full size most pack names
        // ellipsized after two words (field ask). The measurement and the
        // draw sit under the same scale or fitLabel trims for the wrong font.
        constexpr float kLabelScale = 0.85f;
        const ImVec2 cardPad   = FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding);
        const ImVec2 rowGap    = FUCK::GetStyleVarVec(ImGuiStyleVar_ItemSpacing);
        const float  cardOuterW = kCardW + 2.0f * cardPad.x;
        const float  cardOuterH = kCardH + 2.0f * cardPad.y;
        FUCK::SetWindowFontScale(kLabelScale);
        const float  labelH     = FUCK::CalcTextSize("A").y;
        FUCK::SetWindowFontScale(1.0f);
        // One card row as stacked: picture, spacing, label, spacing to the
        // next row. The thumbless tile keeps the same pitch through its
        // spacer Dummy, which is what lets one number describe both kinds.
        const float  rowPitch   = cardOuterH + rowGap.y + labelH + rowGap.y;
        const auto drawCards = [&](const std::vector<int>& a_indices) {
            const ImVec4 kGold{ 0.87f, 0.72f, 0.35f, 1.0f };
            const float rowRight =
                FUCK::GetCursorScreenPos().x + FUCK::GetContentRegionAvail().x;
            bool first = true;
            for (const int i : a_indices) {
                const auto& bg = backgrounds[i];
                if (!first &&
                    FUCK::GetItemRectMax().x + kGap + cardOuterW <= rowRight) {
                    FUCK::SameLine(0.0f, kGap);
                }
                first = false;
                FUCK::PushID(i);
                FUCK::BeginGroup();
                const bool selected = i == bgIdx;
                bool clicked = false;
                bool hovered = false;
                auto* thumb = thumbFor(bg);
                if (thumb) {
                    if (selected) {
                        FUCK::PushStyleColor(ImGuiCol_Button, kGold);
                    }
                    clicked = FUCK::ImageButton("##card", thumb->GetID(),
                                                ImVec2(kCardW, kCardH));
                    if (selected) {
                        FUCK::PopStyleColor(1);
                    }
                    hovered = FUCK::IsItemHovered();
                    // The picture cannot say its own name; the label under it
                    // does, gold when this is the one on the void.
                    FUCK::SetWindowFontScale(kLabelScale);
                    const std::string label = fitLabel(TitleFirst(bg.name), kCardW);
                    if (selected) {
                        FUCK::TextColored(kGold, "%s", label.c_str());
                    } else {
                        FUCK::TextDisabled("%s", label.c_str());
                    }
                    FUCK::SetWindowFontScale(1.0f);
                } else {
                    // A thumbless pack still draws as a card: the same outer
                    // size as an image card with the name centred where the
                    // picture would be, so it keeps the row's rhythm instead
                    // of breaking it. The tile IS its own label - a second
                    // one under it read as a doubled name in the field - so
                    // a spacer stands in for the label line.
                    FUCK::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,
                                       ImVec2(0.5f, 0.5f));
                    FUCK::SetWindowFontScale(kLabelScale);
                    clicked = FUCK::Selectable(fitLabel(TitleFirst(bg.name), kCardW).c_str(),
                                               selected, 0,
                                               ImVec2(cardOuterW, cardOuterH));
                    FUCK::SetWindowFontScale(1.0f);
                    FUCK::PopStyleVar(1);
                    hovered = FUCK::IsItemHovered();
                    FUCK::Dummy(ImVec2(cardOuterW, labelH));
                }
                if (hovered) {
                    const auto author = MTB::BackdropPacks::AuthorOf(bg.name);
                    std::string tip = TitleFirst(bg.name);
                    if (!author.empty()) {
                        tip += "\nBackdrop pack by " + std::string{ author };
                    }
                    FUCK::SetTooltip(tip.c_str());
                }
                FUCK::EndGroup();
                FUCK::PopID();
                if (clicked) {
                    cfg.ApplyBackgroundPreset(bg.name);
                    dirty = true;
                }
            }
        };
        const int builtinCount =
            (std::min)(static_cast<int>(MTB::BackdropPacks::BuiltinBackgroundCount()),
                       static_cast<int>(backgrounds.size()));
        constexpr int kDefaultOpen = 1 << 5;  // ImGuiTreeNodeFlags_DefaultOpen
        // The card area scrolls inside a fixed-height child: with the sky
        // harvest the list is dozens of cards, and from the gear button the
        // panel otherwise runs off the bottom of the screen (field).
        //
        // The height is one accordion header plus three whole card rows,
        // measured from the same metrics the rows draw with. It was a raw
        // 340.0f, which fits no whole number of rows at any UI scale, so the
        // box always cut its bottom row through the picture (field, via the
        // gear popup) and cut more of it the larger the scale.
        const ImVec2 boxPad = FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding);
        const float  boxH =
            2.0f * boxPad.y + FUCK::GetFrameHeight() + 3.0f * rowPitch;
        FUCK::BeginChild("##bgcards", ImVec2(0.0f, boxH), true, 0);
        std::vector<int> shipped(builtinCount);
        for (int i = 0; i < builtinCount; ++i) {
            shipped[i] = i;
        }
        if (FUCK::CollapsingHeader("Shipped backgrounds", kDefaultOpen)) {
            drawCards(shipped);
        }
        // One accordion per pack group, in encounter order. A pack that names
        // no group lands in the plain packs accordion; the load-order harvest
        // names its own, so dozens of skies do not bury the HDRIs.
        std::vector<std::pair<std::string, std::vector<int>>> groups;
        for (int i = builtinCount; i < static_cast<int>(backgrounds.size()); ++i) {
            const char* raw = backgrounds[i].group;
            const std::string label = (raw && *raw) ? raw : "Backdrop packs";
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [&](const auto& g) { return g.first == label; });
            if (it == groups.end()) {
                groups.push_back({ label, {} });
                it = std::prev(groups.end());
            }
            it->second.push_back(i);
        }
        for (const auto& [label, indices] : groups) {
            FUCK::PushID(label.c_str());
            if (FUCK::CollapsingHeader(label.c_str(),
                                       label == "Backdrop packs" ? kDefaultOpen : 0)) {
                drawCards(indices);
            }
            FUCK::PopID();
        }
        FUCK::EndChild();
        if (cfg.declutterMode < 2) {
            FUCK::TextColored(kWarn, "Only shows in the Void and the Dressing room.");
        }
        if (FUCK::SliderFloat("Size", &cfg.backdropDomeRadius,
                              MTB::BackdropPolicy::kBackgroundRadiusMin,
                              MTB::BackdropPolicy::kBackgroundRadiusMax, "%.0f")) {
            dirty = true;
        }
        Tip("How far away the backdrop sits");
        if (FUCK::SliderFloat("Height", &cfg.backdropDomeZ, -2048.0f, 2048.0f, "%.0f")) {
            dirty = true;
        }
        Tip("Raises or lowers the backdrop, which is how you frame the nebula on "
            "a star dome");
        if (!cfg.backdropBackgroundImage.empty()) {
            if (FUCK::SliderFloat("Image brightness", &cfg.backdropImageBrightness,
                                  0.05f, 2.0f, "%.2f")) {
                dirty = true;
            }
            Tip("Tames a bright picture over the dark studio. Daylight images "
                "usually want a low value");
        }
        dirty |= FUCK::Checkbox("Lock background angle", &cfg.backgroundFaceCamera);
        Tip("Frames a custom image the same way no matter which way you were "
            "facing when the menu opened");
        if (cfg.backgroundFaceCamera) {
            if (FUCK::SliderFloat("Background rotation", &cfg.backgroundYawOffset,
                                  -180.0f, 180.0f, "%.0f")) {
                dirty = true;
            }
            Tip("Turns the locked image left or right");
        }
        dirty |= FUCK::Checkbox("Custom void colour", &cfg.voidColorOverride);
        Tip("Overrides the mood and paints the void a colour you pick");
        if (cfg.voidColorOverride) {
            ColorRow("Void colour", cfg.voidColor, dirty);
        }

        if (cfg.declutterMode == 3) {
            FUCK::SeparatorText("Stage");
            const auto stages = MTB::Settings::StagePresets();
            std::vector<std::string> stageDisplay;
            std::vector<const char*> stageNames;
            stageDisplay.reserve(stages.size());
            stageNames.reserve(stages.size());
            int stageIdx = -1;
            for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
                stageDisplay.push_back(TitleFirst(stages[i].name));
                if (cfg.backdropStage == stages[i].name) {
                    stageIdx = i;
                }
            }
            for (const auto& d : stageDisplay) {
                stageNames.push_back(d.c_str());
            }
            if (FUCK::Combo("Stage", &stageIdx, stageNames.data(),
                            static_cast<int>(stageNames.size())) &&
                stageIdx >= 0) {
                cfg.ApplyStagePreset(stages[stageIdx].name);
                dirty = true;
            }
            if (stageIdx >= 0) {
                const auto author = MTB::BackdropPacks::AuthorOf(stages[stageIdx].name);
                if (!author.empty()) {
                    Tip(("Stage from a backdrop pack by " + std::string{ author }).c_str());
                } else {
                    Tip("The floor and set pieces you stand on in the dressing room");
                }
            } else {
                Tip("The floor and set pieces you stand on in the dressing room");
            }
            if (FUCK::SliderFloat("Floor size", &cfg.backdropFloorRadius, 64.0f,
                                  2048.0f, "%.0f")) {
                dirty = true;
            }
            Tip("How wide the stage floor is");
            if (FUCK::SliderFloat("Floor height", &cfg.backdropFloorZ, -256.0f,
                                  256.0f, "%.0f")) {
                dirty = true;
            }
            Tip("Raises or lowers the floor to meet your feet");
        }
        };

        // LIGHTING - the mood, the rig and the colour grade over the whole shot.
        const auto drawLighting = [&] {
        // ⚠ A STATED CHOICE RATHER THAN A SWITCH THAT KILLS THE PAGE. This was
        // a checkbox, ticked on a fresh install, and everything under it was
        // wrapped in one BeginDisabled - so the tab a new player opened was a
        // page of grey. Two named options say the same thing and read as a
        // decision they have already made rather than as a broken screen, and
        // the sections below are now locked one at a time by whether the clock
        // genuinely decides them. Field feedback 2026-08-30: "lighting settings
        // overhaul for better UX".
        FUCK::SeparatorText("Mood");
        static const char* const kMoodSource[] = { "The time of day and season",
                                                   "Me, from the list below" };
        int moodSource = cfg.matchTimeAndSeason ? 0 : 1;
        if (FUCK::Combo("Mood chosen by", &moodSource, kMoodSource, 2)) {
            cfg.matchTimeAndSeason = moodSource == 0;
            dirty = true;
        }
        Tip("Whether the game clock picks the lighting for you or you pick it "
            "yourself. Either way the rig below stays yours");

        // What is ACTUALLY being rendered right now, clock or not. The rig rows
        // display this when the clock owns the colours, so a greyed swatch is
        // still the truth rather than a stale manual value.
        const auto look = cfg.CurrentLook();

        if (cfg.matchTimeAndSeason) {
            FUCK::TextDisabled("Right now: %s.", cfg.DescribeTimeAndSeason().c_str());
        } else {
            const auto presets = MTB::Settings::LightPresets();
            std::vector<std::string> moodDisplay;
            std::vector<const char*> moodNames;
            moodDisplay.reserve(presets.size());
            moodNames.reserve(presets.size());
            int presetIdx = -1;
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                moodDisplay.push_back(TitleFirst(presets[i].name));
                if (cfg.lightPreset == presets[i].name) {
                    presetIdx = i;
                }
            }
            for (const auto& d : moodDisplay) {
                moodNames.push_back(d.c_str());
            }
            if (FUCK::Combo("Mood", &presetIdx, moodNames.data(),
                            static_cast<int>(moodNames.size())) &&
                presetIdx >= 0) {
                cfg.ApplyLightPreset(presets[presetIdx].name);
                dirty = true;
            }
            Tip("The lighting look. Sets the colour of the void and the studio rig");
        }

        FUCK::SeparatorText("Studio rig");
        drawRig(look, cfg.matchTimeAndSeason);
        // A screen grade, so it is safe with the world running for the same
        // reason the rig is: it hides nothing. That is also why both survive on
        // the Souls-live page while everything needing a frozen scene does not.
        FUCK::SeparatorText("Colour filter");
        drawColorFilter();
        };

        // THE TABS. What a user reaches for most first: their character, then
        // where the studio applies, then the space around them, then how it is
        // lit. Character leads on the author's call, 2026-08-13. The Souls
        // tab only exists when Souls does, and the two tabs that need a frozen
        // scene disappear when there is not going to be one - rather than
        // showing controls that silently cannot apply (r28h).
        if (FUCK::BeginTabBar("##MenuStudio")) {
            if (!studioInert && FUCK::BeginTabItem("Character")) {
                drawCharacter();
                FUCK::EndTabItem();
            }
            // Second: scope is set once and then left alone, while the tab above
            // is the one a player opens to adjust what they are looking at.
            // Never hidden by studioInert - see drawMenus.
            if (FUCK::BeginTabItem("Menus")) {
                drawMenus();
                FUCK::EndTabItem();
            }
            if (!studioInert && FUCK::BeginTabItem("Scene")) {
                drawScene();
                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("Lighting")) {
                drawLighting();
                FUCK::EndTabItem();
            }
            // NOT gated on studioInert, unlike Character and Scene: the strip
            // draws over any bubbled menu whether or not the studio has a
            // scene to build, so its controls always have something to act on.
            if (FUCK::BeginTabItem("Buttons")) {
                drawButtons();
                FUCK::EndTabItem();
            }
            if (souls && FUCK::BeginTabItem("Skyrim Souls")) {
                drawSouls();
                FUCK::EndTabItem();
            }
            // Last on purpose: nobody should land on it by accident.
            if (!studioInert && FUCK::BeginTabItem("Experimental")) {
                drawExperimental();
                FUCK::EndTabItem();
            }
            FUCK::EndTabBar();
        }

        if (dirty) {
            cfg.Save();
        }
    }

    // FLICK sidebar entry: the user opens FUCK (hotkey / controller menu) and
    // picks "Menu Studio".
    class SettingsTool : public FUCK::ITool {
    public:
        const char* Name() const override { return "Menu Studio"; }
        void        Draw() override { DrawPanel(); }
    };

    SettingsTool g_settingsTool;  // process-lifetime; registered pointer stays valid

}

namespace MTB::SettingsUI {
    void Register() {
        // Soft dependency: without FUCK.dll the mod stays INI-only with one log
        // line. The name passed here is what FLICK shows in its sidebar / the
        // registered-plugin panel, so it must read "Menu Studio".
        if (!FUCK::Connect("Menu Studio")) {
            // ⚠ LOUD ON PURPOSE. FUCK/FLICK has no build for the mid 1.6
            // runtimes, so users there get a mod with NO VISIBLE SETTINGS and
            // no way to tell whether that is the intended fallback or a broken
            // install. Everything still works and every setting is in the INI -
            // this line is the only place that says so, so it says it in full
            // and at warn level rather than hiding in the info stream.
            spdlog::warn("SettingsUI: FLICK (FUCK) is not present, so there is no in-game "
                         "settings panel on this setup. Nothing is broken. Every setting lives "
                         "in Data/SKSE/Plugins/MenuStudio.ini and is re-read each time you load "
                         "a save. FLICK has no build for the mid 1.6 runtimes "
                         "(1.6.317-1.6.1129).");
            return;
        }
        FUCK::RegisterTool(&g_settingsTool);
        spdlog::info("SettingsUI: settings registered as a FLICK (FUCK) sidebar tool. "
                     "The strip's gear opens the same panel in a popup of its own.");
        ActionBar::Install();
    }

    // ⚠ A POPUP, NOT A WINDOW OF ITS OWN, AND THAT IS WHY THIS IS A BARE
    // FUNCTION NOW. The gear used to open a registered FUCK::IWindow, which
    // brings a frame, a title bar and a close button with it, and a titled box
    // is the wrong shape for a control panel hung off a small round button.
    // Fitting Room's own gear has always used a popup; this matches it (user,
    // 2026-08-05: "why can't you do the MS setting like how we do with FR. it
    // pops up in the same place as the cog, it doesn't have that ugly box").
    //
    // The popup is opened by the gear's handler and drawn by the bar, both
    // inside the bar's own ImGui window, because that is what an ImGui popup
    // requires: OpenPopup and BeginPopup have to meet in the same context. It
    // also solves for free every geometry problem the window had - placement,
    // saved-position fights, and the empty panel that came of moving a window
    // mid-append - since a popup opens where it was asked for and sizes itself.
    void DrawPanelBody() { DrawPanel(); }
}

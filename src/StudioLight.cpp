#include "PCH.h"

#include "StudioLight.h"

#include "Offsets.h"
#include "Settings.h"
#include "SunParkPolicy.h"

namespace {
    RE::FormID g_cellID = 0;
    RE::INTERIOR_DATA g_saved{};
    RE::FormID g_imageSpaceID = 0;
    RE::ImageSpaceBaseData g_savedImage{};
    RE::Sky::Mode g_savedSkyMode = RE::Sky::Mode::kNone;
    bool g_skyParked = false;

    using SkyRefresh_t = void(__fastcall*)(RE::Sky*, float);
    REL::Relocation<SkyRefresh_t> g_skyForceInteriorRefresh{
        MTB::Offsets::SkyForceInteriorRefresh
    };

    // The renderer never reads INTERIOR_DATA directly: the Sky ingests it
    // (blended with the lighting template) on cell ATTACH and caches the
    // result - form edits alone change nothing on screen (field evidence:
    // barebones sessions 0355/0407/0422). This drives the engine's own
    // forced re-ingest so edits (and restores) take effect immediately.
    void ReingestCellLighting() {
        auto* sky = RE::Sky::GetSingleton();
        if (!sky) {
            return;
        }
        const auto skyAddr = reinterpret_cast<std::uintptr_t>(sky);
        // Refresh throttle timer at Sky+0x1D4: the wrapper only fires once
        // the accumulated interval passes - max it for a deterministic run.
        *reinterpret_cast<float*>(skyAddr + 0x1D4) = 1.0e9f;
        // Second gate: the wrapper silently does NOTHING while
        // TESWaterSystem+0xB8 is nonzero (round-5 field sessions showed all
        // our calls "succeeding" with zero visual effect). Clear it around
        // the call and restore.
        REL::Relocation<RE::TESWaterSystem**> waterSingleton{
            MTB::Offsets::TESWaterSystemSingleton
        };
        auto* water = reinterpret_cast<std::uint8_t*>(*waterSingleton.get());
        std::uint8_t gate = water ? water[0xB8] : 0;
        if (water && gate) {
            water[0xB8] = 0;
        }
        // Ingest proof: one of the Sky's cell-fed color slots, before/after.
        const auto* slot = reinterpret_cast<const float*>(skyAddr + 0xB4);
        const float b0 = slot[0], b1 = slot[1], b2 = slot[2];
        g_skyForceInteriorRefresh(sky, 0.0f);
        spdlog::debug(
            "studio light: sky re-ingest forced (gate byte {}, slotB4 "
            "({:.3f},{:.3f},{:.3f}) -> ({:.3f},{:.3f},{:.3f})).",
            gate, b0, b1, b2, slot[0], slot[1], slot[2]);
        if (water && gate) {
            water[0xB8] = gate;
        }
    }
}

namespace {
    // Values from the effective look: manual preset + overrides, or the
    // auto time-of-day/season pick (Settings::CurrentLook decides).
    void WriteLook(RE::INTERIOR_DATA* a_lighting) {
        const auto& cfg = MTB::Settings::GetSingleton();
        const auto look = cfg.CurrentLook();
        a_lighting->ambient = look.ambient;
        a_lighting->directional = look.directional;  // rotation fields kept as-is
        a_lighting->fogColorNear = look.fog;
        a_lighting->fogColorFar = look.fog;
        a_lighting->fogNear = cfg.lightFogNear;
        a_lighting->fogFar = cfg.lightFogFar;
        a_lighting->fogPower = 1.0f;
        a_lighting->fogClamp = 1.0f;
        auto& dalc = a_lighting->directionalAmbientLightingColors;
        dalc.directional.x.max = look.fill;
        dalc.directional.x.min = look.fill;
        dalc.directional.y.max = look.fill;
        dalc.directional.y.min = look.fill;
        dalc.directional.z.max = look.fill;
        dalc.directional.z.min = look.fill;
    }
}

namespace {
    // ------------------------------------------------------------------
    // r46 EXTERIOR SUN PARK (user, field: "the sun outside lights our
    // character"; interiors reported fine).
    //
    // ⚠⚠ NOTHING IN THIS FILE OR IN DECLUTTER HAS EVER REACHED OUTDOOR
    // LIGHTING, and both of them say so out loud. Apply below needs
    // INTERIOR_DATA, which an exterior does not have, so it has always
    // returned before the override; Declutter's r33 cut walks the CELL's
    // scene graph and self-culls every NiLight in it, and the sun is not in
    // that graph - it hangs off the Sky. F-18 shipped with the gap written
    // into its own entry, "exteriors, sun untouched". So an outdoor void has
    // always been a hidden world lit by a sun the player cannot see, which is
    // exactly what the field reported.
    //
    // Two live writes, both saved whole and both restored:
    //
    //  - THE SKY'S TWO DIRECTIONAL LIGHTS, zeroed and self-culled. The colour
    //    write alone would be the obvious fix and it is not enough on its own:
    //    r8's physics, proven both directions by the studio rig, is that light
    //    gathering skips a light whose OWN node is culled, so the cull is what
    //    guarantees nothing downstream re-reads it. The colour write is what
    //    reaches anything that reads the light without asking about the cull.
    //    Neither needs a re-ingest - the gather reads the NiLight per frame.
    //
    //  - BSSHADERMANAGER'S DIRECTIONAL AMBIENT, which is the sky's fill on
    //    every surface in frame and the reason a character reads as "outdoors"
    //    even with the key light gone. ⚠ ZEROED WHOLE rather than written to
    //    the studio fill the interior branch uses: the live value is a 3x3 plus
    //    a translate, and which slot carries which axis is NOT measured yet, so
    //    writing a colour into it would be a guess. Zero needs no layout at all
    //    and the whole-struct save restores exactly. The park edge logs the
    //    daylight values it found, so the next round can write a real fill from
    //    a measurement instead of guessing too. Until then the three-point rig
    //    owns the fill out here, which is what fRigBrightness is for.
    //
    // The whole thing is state-based rather than an edge: ParkSun saves on its
    // first call and re-writes on every later one, so the armed tick can call
    // this as a re-assert against Sky::Update repainting both of these in the
    // unpaused switch and exit windows.
    // ------------------------------------------------------------------
    struct SavedLight {
        // ⚠ THE LIGHT WE TOOK, HELD, and released back through. Re-reading the
        // scene node at release time would look equivalent and is not: a
        // quickload or a cell change inside an open menu can put a different
        // BSLight in that slot, and the restore would then paint one cell's
        // saved daylight onto another cell's sun. A held NiPointer also keeps
        // the object alive long enough to be given its values back.
        RE::NiPointer<RE::NiLight> node{};
        RE::NiColor                diffuse{};
        RE::NiColor                ambient{};
        float                      fade{ 0.0f };
        bool                       culled{ false };
    };

    struct SunPark {
        bool            parked{ false };
        SavedLight      sun{};
        SavedLight      cloud{};
        RE::NiTransform ambient{};
        bool            haveAmbient{ false };
    };
    SunPark g_sunPark{};

    void SaveLight(RE::NiLight* a_light, SavedLight& a_saved) {
        a_saved.node.reset();
        if (!a_light) {
            return;
        }
        const auto& rt = a_light->GetLightRuntimeData();
        a_saved.diffuse = rt.diffuse;
        a_saved.ambient = rt.ambient;
        a_saved.fade = rt.fade;
        a_saved.culled = a_light->GetAppCulled();
        a_saved.node.reset(a_light);
    }

    void ZeroLight(RE::NiLight* a_light) {
        if (!a_light) {
            return;
        }
        auto& rt = a_light->GetLightRuntimeData();
        rt.diffuse = RE::NiColor{ 0.0f, 0.0f, 0.0f };
        rt.ambient = RE::NiColor{ 0.0f, 0.0f, 0.0f };
        rt.fade = 0.0f;
        a_light->SetAppCulled(true);
    }

    void ReleaseLight(SavedLight& a_saved) {
        if (auto* light = a_saved.node.get()) {
            auto& rt = light->GetLightRuntimeData();
            rt.diffuse = a_saved.diffuse;
            rt.ambient = a_saved.ambient;
            rt.fade = a_saved.fade;
            light->SetAppCulled(a_saved.culled);
        }
        a_saved.node.reset();
    }

    // BSShaderManager's own global. The CommonLib this project pins has no
    // header for it at all, so the two things needed out of it are read by
    // offset - the same way this file already reaches Sky+0x1D4 and
    // TESWaterSystem+0xB8 above. Offsets.h carries the layout and why both
    // offsets are SE/AE-identical; VersionCheck carries the id as kData, so a
    // build where it is wrong declines to load rather than writing over a
    // stranger.
    REL::Relocation<std::uintptr_t> g_shaderState{ MTB::Offsets::ShaderManagerState };

    RE::ShadowSceneNode* WorldShadowScene() {
        auto* const* scenes =
            reinterpret_cast<RE::ShadowSceneNode* const*>(g_shaderState.address());
        return scenes[0];
    }

    RE::NiTransform* DirectionalAmbient() {
        return reinterpret_cast<RE::NiTransform*>(g_shaderState.address() + 0xC8);
    }

    RE::NiLight* SceneLight(RE::BSLight* a_light) {
        return a_light ? a_light->light.get() : nullptr;
    }

    // ⚠ THE SUN COMES FROM THE SHADOW SCENE NODE, NOT FROM Sky::sun. In
    // vanilla the two are the same object, and the scene node is still the
    // right side to write for two reasons. It is by construction the light the
    // gather reads, so there is no question of writing a staging copy nothing
    // consumes. And BSLight::light is a NiPointer<NiLight> the pinned
    // CommonLib actually defines, where Sky::sun->light is a
    // NiPointer<NiDirectionalLight> it only forward-declares. The park edge
    // logs whether Sky agrees, so a lighting overhaul that registers a sun of
    // its own surfaces as a disagreement instead of as a fix that silently
    // does nothing.
    void ParkSun() {
        if (!g_sunPark.parked) {
            auto* ssn = WorldShadowScene();
            if (!ssn) {
                return;
            }
            auto& scene = ssn->GetRuntimeData();
            SaveLight(SceneLight(scene.sunLight), g_sunPark.sun);
            SaveLight(SceneLight(scene.cloudLight), g_sunPark.cloud);
            g_sunPark.ambient = *DirectionalAmbient();
            g_sunPark.haveAmbient = true;
            g_sunPark.parked = true;

            const auto* sunLight = g_sunPark.sun.node.get();
            const auto* cloudLight = g_sunPark.cloud.node.get();
            auto* sky = RE::Sky::GetSingleton();
            auto* sun = sky ? sky->sun : nullptr;
            const void* skySun =
                sun ? static_cast<const void*>(sun->light.get()) : nullptr;
            const auto& amb = g_sunPark.ambient.rotate;
            spdlog::debug(
                "studio light: exterior sun parked. scene sun {} (diffuse "
                "{:.3f},{:.3f},{:.3f} fade {:.2f}), scene cloud light {} (diffuse "
                "{:.3f},{:.3f},{:.3f}), sky's own sun {} ({}). Directional ambient "
                "was rows ({:.3f},{:.3f},{:.3f}) ({:.3f},{:.3f},{:.3f}) "
                "({:.3f},{:.3f},{:.3f}) translate ({:.3f},{:.3f},{:.3f}).",
                static_cast<const void*>(sunLight), g_sunPark.sun.diffuse.red,
                g_sunPark.sun.diffuse.green, g_sunPark.sun.diffuse.blue,
                g_sunPark.sun.fade, static_cast<const void*>(cloudLight),
                g_sunPark.cloud.diffuse.red, g_sunPark.cloud.diffuse.green,
                g_sunPark.cloud.diffuse.blue, skySun,
                skySun == static_cast<const void*>(sunLight) ? "the same light"
                                                             : "A DIFFERENT LIGHT",
                amb.entry[0][0], amb.entry[0][1], amb.entry[0][2], amb.entry[1][0],
                amb.entry[1][1], amb.entry[1][2], amb.entry[2][0], amb.entry[2][1],
                amb.entry[2][2], g_sunPark.ambient.translate.x,
                g_sunPark.ambient.translate.y, g_sunPark.ambient.translate.z);
        }

        // Every call, not just the first: this is the re-assert.
        //
        // ⚠ IT RE-ZEROES THE LIGHTS WE SAVED, NOT WHATEVER THE SCENE NODE HOLDS
        // NOW, so park and release stay symmetric: nothing is ever taken down
        // that we do not also hold the values for. If a load or a cell change
        // swaps the scene node's sun mid-menu, the new one simply keeps its
        // daylight, which is the direction this whole module has to fail in.
        ZeroLight(g_sunPark.sun.node.get());
        ZeroLight(g_sunPark.cloud.node.get());
        auto* ambient = DirectionalAmbient();
        for (auto& row : ambient->rotate.entry) {
            row[0] = 0.0f;
            row[1] = 0.0f;
            row[2] = 0.0f;
        }
        ambient->translate = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
    }

    void ReleaseSun() {
        if (!g_sunPark.parked) {
            return;
        }
        ReleaseLight(g_sunPark.sun);
        ReleaseLight(g_sunPark.cloud);
        if (g_sunPark.haveAmbient) {
            *DirectionalAmbient() = g_sunPark.ambient;
            g_sunPark.haveAmbient = false;
        }
        g_sunPark.parked = false;
        spdlog::debug("studio light: exterior sun released (daylight restored).");
    }
}

namespace MTB::StudioLight {
    void SyncExteriorSun() {
        const auto& cfg = Settings::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        const auto action = SunParkPolicy::Decide({
            .enabled = cfg.cutSunLight,
            .worldHidden = cfg.IsVoidFamily(),
            // No cell resolves to "not an exterior", which releases. Every
            // unknown here has to fail towards giving the daylight back.
            .exterior = cell != nullptr && !cell->IsInteriorCell(),
        });
        if (action == SunParkPolicy::Action::kRelease) {
            ReleaseSun();
            return;
        }
        ParkSun();
    }

    void Apply() {
        const auto& cfg = Settings::GetSingleton();
        // ⚠ BEFORE EVERY EARLY-OUT BELOW, AND THAT IS THE POINT. The sun park
        // is gated like bCutCellLights (its own toggle, void family only) and
        // not like the cell override, so bStandardizeLighting must not decide
        // it either way. Apply is called unconditionally on the arm edge, which
        // makes the top of it the one place the sun is guaranteed to be asked
        // about on every open.
        SyncExteriorSun();
        if (!cfg.standardizeLighting) {
            return;
        }
        // Void (2) and dressing room (3) only: Off / Scene view keep the cell's
        // own look - standardized studio light there reads as a bug. (The colour
        // filter is a separate, view-independent post-process, not this.)
        // F-24: bStudioLightWithoutSpace opts a visible room in. Kept a
        // SEPARATE toggle from the rig deliberately - the rig only adds lights
        // to the character, but everything below rewrites the CELL (ambient,
        // fog, imagespace), so with the world still on screen it restyles the
        // whole room rather than just the person standing in it.
        if (!cfg.CellLightAllowed()) {
            spdlog::debug("studio light: skipped, space mode {} keeps the cell's own look.",
                          cfg.declutterMode);
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        // r44 REWRITE of the F-7 park. The r42 discovery: Sky::mode is
        // consumed by Sky::Update, which never runs in a paused menu - the
        // park was RENDER-INERT in every armed menu ever; the sky's
        // disappearance is owned by the void engine's 'Sky'/'Weather'
        // branch culls now. What the mode VALUE still drives is the
        // interior FOG-INGEST chain (r26, slotB4-proven: kInterior keeps
        // INTERIOR_DATA fog alive, kNone kills it) - so INTERIORS still
        // park to kInterior for the fog, and EXTERIORS never touch the
        // mode again: nothing to park means the r40 weather-audio stop
        // latch is structurally impossible outdoors.
        if (!g_skyParked && cell && cell->IsInteriorCell()) {
            if (auto* sky = RE::Sky::GetSingleton()) {
                g_savedSkyMode = sky->mode.get();
                sky->mode = RE::Sky::Mode::kInterior;
                g_skyParked = true;
                spdlog::debug("studio light: sky mode parked to kInterior for "
                              "fog ingest (was mode {}).",
                              static_cast<int>(g_savedSkyMode));
            }
        }
        if (g_cellID != 0) {
            return;  // interior override already applied this arm
        }
        auto* lighting = cell && cell->IsInteriorCell() ? cell->GetLighting() : nullptr;
        if (!lighting) {
            // Exteriors have no INTERIOR_DATA to standardize - the parked
            // sky + the dome carry the look out there.
            return;
        }

        g_saved = *lighting;
        g_cellID = cell->GetFormID();

        // Cells can INHERIT channels from their lighting template (LTMP) -
        // the renderer then ignores the per-cell values entirely for those
        // channels. Field case: barebones 'Editor Smoke Test Cell' inherits
        // fog/ambient, so the spike's override never showed there (green
        // template murk instead of the void). Clear the inherit bits for
        // exactly the channels we set; the whole-struct save restores them.
        using Inherit = RE::INTERIOR_DATA::Inherit;
        lighting->lightingTemplateInheritanceFlags.reset(
            Inherit::kAmbientColor, Inherit::kDirectionalColor, Inherit::kFogColor,
            Inherit::kFogNear, Inherit::kFogFar, Inherit::kFogPower, Inherit::kFogMax);

        WriteLook(lighting);

        if (cfg.matchTimeAndSeason) {
            spdlog::debug("studio light: applied time-and-season look ({}) to cell {:08X} ('{}').",
                          cfg.DescribeTimeAndSeason(), g_cellID, cell->GetName());
        } else {
            spdlog::debug("studio light: applied '{}' to cell {:08X} ('{}').",
                          cfg.lightPreset, g_cellID, cell->GetName());
        }

        // The cell's IMAGESPACE post-processes the whole frame AFTER the
        // lighting above: cinematic tint, saturation/contrast, HDR eye
        // adaptation. On vanilla lighting stacks it dominates the void -
        // a green-tinted, auto-exposing imagespace crushes any fog color
        // into the same murk (field case: barebones 'Editor Smoke Test
        // Cell'). Neutralize it in place while armed; same save/restore
        // lifecycle as the cell lighting.
        auto* xImg = cell->extraList.GetByType<RE::ExtraCellImageSpace>();
        if (auto* img = xImg ? xImg->imageSpace : nullptr) {
            g_savedImage = img->data;
            g_imageSpaceID = img->GetFormID();
            auto& d = img->data;
            spdlog::debug(
                "studio light: neutralizing imagespace {:08X} (was: tint {:.2f} "
                "({:.2f},{:.2f},{:.2f}) sat {:.2f} bright {:.2f} contrast {:.2f} "
                "adaptStrength {:.2f} adaptSpeed {:.2f} white {:.2f}), adaptation "
                "SPEED frozen at 0 while armed (exposure pinned pre-arm).",
                g_imageSpaceID, d.tint.amount, d.tint.color.red, d.tint.color.green,
                d.tint.color.blue, d.cinematic.saturation, d.cinematic.brightness,
                d.cinematic.contrast, d.hdr.eyeAdaptStrength, d.hdr.eyeAdaptSpeed,
                d.hdr.white);
            d.tint.amount = 0.0f;
            d.cinematic.saturation = 1.0f;
            d.cinematic.brightness = 1.0f;
            d.cinematic.contrast = 1.0f;
            // HDR adaptation STRENGTH stays untouched (r27: zeroing it
            // blew ENB exposure to white). r32 pins the adaptation SPEED
            // instead: the r31 Dragonsreach log proved everything else
            // neutral (tint 0, sat/bright/contrast 1, fog ingesting
            // charcoal, shell up) yet the void whited - that's the
            // auto-exposure DRIFTING up over the culled-dark studio
            // (adaptStrength 1.0 there, ENB amplifies). Speed 0 freezes
            // the running exposure at its pre-arm, world-lit value for
            // the whole menu; the whole-struct restore lets it resume.
            d.hdr.eyeAdaptSpeed = 0.0f;
        } else {
            // No XCIM: the engine's default imagespace applies and stays
            // untouched. If the void still reads tinted in such a cell,
            // that's the evidence for a manager-level override next round.
            spdlog::debug("studio light: cell {:08X} has no imagespace of its own, "
                          "tone left to the engine default.", g_cellID);
        }

        // r38 Dragonsreach-day-white evidence line (field: "still really
        // white during the day"): one correlatable row per arm - if the
        // white tracks the HOUR with identical imagespace/fog/adapt values,
        // the writer is ENB-side (its own day-interior adaptation/mist,
        // outside game data) and the next lever is the field A/B with
        // fVoidBrightnessCap, not another form write.
        {
            const auto* cal = RE::Calendar::GetSingleton();
            const float hour = cal ? cal->GetHour() : -1.0f;
            spdlog::info("void diag: cell='{}' interior={} hour={:.2f} xcim={} "
                         "adaptSpeed(was)={:.2f} adaptStrength={:.2f}",
                         cell->GetName() ? cell->GetName() : "?",
                         cell->IsInteriorCell() ? 1 : 0, hour,
                         g_imageSpaceID != 0 ? 1 : 0,
                         g_imageSpaceID != 0 ? g_savedImage.hdr.eyeAdaptSpeed : -1.0f,
                         g_imageSpaceID != 0 ? g_savedImage.hdr.eyeAdaptStrength : -1.0f);
        }

        ReingestCellLighting();
    }

    void LiveRefresh() {
        // Mid-arm settings change (panel edit / INI reload): rewrite the
        // preset values into the already-overridden cell and re-ingest.
        // The saved backup from Apply stays authoritative for Restore.
        //
        // ⚠ THE SUN ANSWER COMES FIRST, because the early-out below is an
        // interior test: g_cellID is only ever set by the cell override, so it
        // is zero for the whole of every exterior menu and a toggle of
        // bCutSunLight would never be seen out here.
        SyncExteriorSun();
        if (g_cellID == 0) {
            return;
        }
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_cellID);
        auto* lighting = cell ? cell->GetLighting() : nullptr;
        if (!lighting) {
            return;
        }
        WriteLook(lighting);
        ReingestCellLighting();
        spdlog::debug("studio light: live refresh ('{}').",
                      Settings::GetSingleton().lightPreset);
    }

    // r40 (field: "rain sfx stops when i exit the menu"): the sleek exit
    // runs UNPAUSED frames between the close and the at-black restore -
    // Sky::Update ticks there with the parked mode (kNone outdoors),
    // treats the weather as gone and STOPS the rain loop, which only
    // re-triggers on a precipitation TRANSITION. Restoring the mode later
    // brings the visuals back but never the sound. So the sky mode alone
    // goes back AT THE CLOSE EDGE: visually free (the opaque shell keeps
    // occluding the sky through hold + dip), and the weather audio never
    // sees a kNone frame. The full Restore below skips the mode when this
    // already ran (g_skyParked guard); a menu-switch re-open re-parks.
    void RestoreSkyModeEarly() {
        if (!g_skyParked) {
            return;
        }
        if (auto* sky = RE::Sky::GetSingleton()) {
            sky->mode = g_savedSkyMode;
        }
        g_skyParked = false;
        spdlog::debug("studio light: sky mode restored at close edge "
                      "(weather/audio keeps running; shell occludes).");
    }

    void ReparkSkyMode() {
        if (g_skyParked) {
            return;
        }
        // r44: interiors only - exteriors never park the mode (the branch
        // culls own the visuals; the mode value only matters for interior
        // fog ingest).
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell || !cell->IsInteriorCell()) {
            return;
        }
        if (auto* sky = RE::Sky::GetSingleton()) {
            g_savedSkyMode = sky->mode.get();
            sky->mode = RE::Sky::Mode::kInterior;
            g_skyParked = true;
            spdlog::debug("studio light: sky mode re-parked (kInterior).");
        }
    }

    void Restore() {
        // The sun first and unconditionally, because this is the one path that
        // runs on every exit including ForceReset. A parked sun that outlived
        // its menu is a black sky, so it must not sit behind any gate that
        // could have changed while the menu was open.
        ReleaseSun();
        // Sky next: it exists in every cell kind (the interior block below
        // early-outs in exteriors).
        if (g_skyParked) {
            if (auto* sky = RE::Sky::GetSingleton()) {
                sky->mode = g_savedSkyMode;
            }
            g_skyParked = false;
            spdlog::debug("studio light: sky renderer restored (mode {}).",
                          static_cast<int>(g_savedSkyMode));
        }
        if (g_imageSpaceID != 0) {
            if (auto* img = RE::TESForm::LookupByID<RE::TESImageSpace>(g_imageSpaceID)) {
                img->data = g_savedImage;
                spdlog::debug("studio light: restored imagespace {:08X}.", g_imageSpaceID);
            }
            g_imageSpaceID = 0;
        }
        if (g_cellID == 0) {
            return;
        }
        if (auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_cellID)) {
            if (auto* lighting = cell->GetLighting()) {
                *lighting = g_saved;
                spdlog::debug("studio light: restored cell {:08X}.", g_cellID);
            }
        }
        g_cellID = 0;
        // Push the restored values through the same ingest path - otherwise
        // the studio values would linger on screen until the next cell load.
        ReingestCellLighting();
    }
}

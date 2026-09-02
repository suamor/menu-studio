#include "PCH.h"

#include "Settings.h"

#include "BackdropPacks.h"

#include <SimpleIni.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {
    constexpr auto kIniPath = L"Data/SKSE/Plugins/MenuStudio.ini";

#ifdef MENUSTUDIO_DIAG
    // The camera probe's value as the reporter's own file had it, kept so a
    // diagnostic build writes their setting back rather than its own. See the
    // note at the end of Settings::Load.
    bool g_diagProbeAsRead = true;
#endif

    // Comma-separated INI list -> trimmed entries. Shared by sFreezeGraphBools
    // and the diagnostic's sDiagGraphVars; extracted rather than copied so the
    // two cannot drift in how they trim.
    void ParseCsvList(const char* a_raw, std::vector<std::string>& a_out) {
        a_out.clear();
        std::string list{ a_raw };
        std::size_t pos = 0;
        while (pos <= list.size()) {
            auto comma = list.find(',', pos);
            if (comma == std::string::npos) {
                comma = list.size();
            }
            auto       item  = list.substr(pos, comma - pos);
            const auto first = item.find_first_not_of(" \t");
            const auto last  = item.find_last_not_of(" \t");
            if (first != std::string::npos) {
                a_out.push_back(item.substr(first, last - first + 1));
            }
            pos = comma + 1;
        }
    }

    // Named [Lighting] presets - background + three-point rig as one vibe.
    // "studio" = the field-proven neutrals; "bright" carries the player
    // alone in light-poor cells; warm/cool/dusk trade fill for mood (dim
    // fill = harder shadows, hot rim = stronger silhouette).
    constexpr std::array<MTB::LightPreset, 5> kLightPresets{ {
        { "studio", { 96, 96, 100, 0 }, { 160, 155, 150, 0 }, { 13, 13, 15, 0 }, { 80, 80, 84, 0 },
          { { 255, 242, 222, 0 }, 1.0f }, { { 184, 199, 230, 0 }, 1.0f }, { { 255, 255, 255, 0 }, 1.0f } },
        { "bright", { 148, 148, 152, 0 }, { 255, 250, 242, 0 }, { 16, 16, 19, 0 }, { 132, 132, 138, 0 },
          { { 255, 250, 240, 0 }, 1.3f }, { { 235, 240, 250, 0 }, 1.1f }, { { 255, 255, 255, 0 }, 1.2f } },
        { "warm",   { 105, 88, 70, 0 }, { 200, 160, 110, 0 }, { 18, 12, 8, 0 },  { 96, 78, 60, 0 },
          { { 255, 190, 120, 0 }, 1.15f }, { { 200, 150, 100, 0 }, 0.7f }, { { 255, 220, 180, 0 }, 1.0f } },
        { "cool",   { 78, 88, 105, 0 }, { 140, 160, 200, 0 }, { 10, 12, 18, 0 }, { 70, 80, 96, 0 },
          { { 200, 220, 255, 0 }, 1.0f }, { { 120, 150, 220, 0 }, 0.65f }, { { 220, 235, 255, 0 }, 1.4f } },
        { "dusk",   { 70, 60, 80, 0 },  { 180, 120, 90, 0 },  { 14, 10, 16, 0 }, { 76, 64, 84, 0 },
          { { 255, 160, 90, 0 }, 1.2f }, { { 150, 110, 200, 0 }, 0.7f }, { { 255, 200, 150, 0 }, 1.3f } },
    } };

    const MTB::LightPreset* FindPreset(std::string_view a_name) {
        for (const auto& preset : kLightPresets) {
            if (a_name == preset.name) {
                return &preset;
            }
        }
        return nullptr;
    }

    // Case-insensitive equality of a saved name against a preset key. Built-in
    // keys are lowercase, but pack names keep the author's casing (e.g. "Example
    // Nebula"), so preset lookups must ignore case for a saved selection to
    // resolve back to its pack.
    bool NameMatches(std::string_view a_name, const char* a_key) {
        std::size_t i = 0;
        for (; i < a_name.size(); ++i) {
            const unsigned char k = static_cast<unsigned char>(a_key[i]);
            if (k == '\0') {
                return false;  // key shorter than a_name
            }
            if (std::tolower(static_cast<unsigned char>(a_name[i])) != std::tolower(k)) {
                return false;
            }
        }
        return a_key[i] == '\0';  // both ended together
    }

    const MTB::StagePreset* FindStage(std::string_view a_name) {
        for (const auto& stage : MTB::BackdropPacks::Stages()) {
            if (NameMatches(a_name, stage.name)) {
                return &stage;
            }
        }
        return nullptr;
    }

    const MTB::BackgroundPreset* FindBackground(std::string_view a_name) {
        for (const auto& bg : MTB::BackdropPacks::Backgrounds()) {
            if (NameMatches(a_name, bg.name)) {
                return &bg;
            }
        }
        return nullptr;
    }

    // Time-of-day clock mapping: base preset by hour…
    const char* TimePresetName(float a_hour) {
        if (a_hour < 5.0f) return "cool";    // deep night
        if (a_hour < 7.0f) return "dusk";    // dawn glow
        if (a_hour < 17.0f) return "studio"; // day
        if (a_hour < 19.0f) return "dusk";   // sunset
        if (a_hour < 22.0f) return "warm";   // candlelit evening
        return "cool";                       // night
    }
    // …tinted toward a season preset. Skyrim months: Morning Star=0 …
    // Evening Star=11.
    struct SeasonBlend {
        const char* name;
        const char* preset;  // nullptr = no tint
        float       t;
    };
    SeasonBlend SeasonFor(std::uint32_t a_month) {
        switch (a_month) {
        case 2: case 3: case 4:
            return { "spring", nullptr, 0.0f };
        case 5: case 6: case 7:
            return { "summer", "bright", 0.20f };
        case 8: case 9: case 10:
            return { "autumn", "warm", 0.20f };
        default:  // Sun's Dusk, Evening Star, Morning Star, Sun's Dawn
            return { "winter", "cool", 0.25f };
        }
    }

    float LerpF(float a_from, float a_to, float a_t) {
        return a_from + (a_to - a_from) * a_t;
    }
    RE::Color LerpColor(const RE::Color& a_from, const RE::Color& a_to, float a_t) {
        RE::Color out;
        out.red   = static_cast<std::uint8_t>(LerpF(a_from.red, a_to.red, a_t) + 0.5f);
        out.green = static_cast<std::uint8_t>(LerpF(a_from.green, a_to.green, a_t) + 0.5f);
        out.blue  = static_cast<std::uint8_t>(LerpF(a_from.blue, a_to.blue, a_t) + 0.5f);
        return out;
    }

    // "R,G,B" (0-255) -> Color; leaves a_out untouched on parse failure.
    void ParseColor(const CSimpleIniA& a_ini, const char* a_key, RE::Color& a_out,
                    const char* a_section = "Lighting") {
        const char* raw = a_ini.GetValue(a_section, a_key, nullptr);
        if (!raw || !*raw) {
            return;
        }
        int r = 0, g = 0, b = 0;
        if (std::sscanf(raw, " %d , %d , %d", &r, &g, &b) == 3) {
            a_out.red   = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            a_out.green = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
            a_out.blue  = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
        } else {
            spdlog::warn("Settings: [{}] {} = '{}' is not R,G,B, ignored.", a_section, a_key, raw);
        }
    }
}

namespace MTB {
    Settings& Settings::GetSingleton() {
        static Settings instance;
        return instance;
    }

    std::span<const LightPreset> Settings::LightPresets() {
        return kLightPresets;
    }

    std::span<const StagePreset> Settings::StagePresets() {
        return BackdropPacks::Stages();
    }

    std::span<const BackgroundPreset> Settings::BackgroundPresets() {
        return BackdropPacks::Backgrounds();
    }

    bool Settings::ApplyStagePreset(std::string_view a_name) {
        const auto* stage = FindStage(a_name);
        if (!stage) {
            return false;
        }
        backdropFloorMesh   = stage->floorMesh;
        backdropFloorRadius = stage->floorRadius;
        backdropFloorZ      = stage->floorZ;
        backdropStage       = stage->name;
        return true;
    }

    bool Settings::ApplyBackgroundPreset(std::string_view a_name) {
        const auto* bg = FindBackground(a_name);
        if (!bg) {
            return false;
        }
        backdropDomeMesh        = bg->mesh;
        backdropDomeRadius      = BackdropPolicy::ClampBackgroundRadius(bg->radius);
        backdropDomeZ           = bg->z;
        backdropBackground      = bg->name;
        backdropBackgroundImage = bg->image ? bg->image : "";
        backgroundFaceCamera    = bg->faceCamera;
        backgroundYawOffset     = bg->yaw;
        return true;
    }

    std::span<const StagePiece> Settings::ActiveStageExtras() const {
        const auto* stage = FindStage(backdropStage);
        return stage ? stage->extras : std::span<const StagePiece>{};
    }

    bool Settings::ApplyLightPreset(std::string_view a_name) {
        for (const auto& preset : kLightPresets) {
            if (a_name == preset.name) {
                lightAmbient     = preset.ambient;
                lightDirectional = preset.directional;
                lightFog         = preset.fog;
                lightFill        = preset.fill;
                // The rig is part of the vibe: colors + intensities follow
                // the preset; the enable flags stay the user's layout call.
                rigKey.color      = preset.key.color;
                rigKey.intensity  = preset.key.intensity;
                rigFill.color     = preset.fillLight.color;
                rigFill.intensity = preset.fillLight.intensity;
                rigRim.color      = preset.rim.color;
                rigRim.intensity  = preset.rim.intensity;
                lightPreset       = preset.name;
                return true;
            }
        }
        return false;
    }

    Settings::LookValues Settings::CurrentLook() const {
        LookValues look{ lightAmbient, lightDirectional, lightFog, lightFill,
                       rigKey, rigFill, rigRim };
        if (!matchTimeAndSeason) {
            return look;
        }
        auto* calendar = RE::Calendar::GetSingleton();
        const auto* base = calendar ? FindPreset(TimePresetName(calendar->GetHour())) : nullptr;
        if (!base) {
            return look;
        }
        look.ambient     = base->ambient;
        look.directional = base->directional;
        look.fog         = base->fog;
        look.fill        = base->fill;
        look.key.color       = base->key.color;
        look.key.intensity   = base->key.intensity;
        look.fillLight.color     = base->fillLight.color;
        look.fillLight.intensity = base->fillLight.intensity;
        look.rim.color       = base->rim.color;
        look.rim.intensity   = base->rim.intensity;

        const auto season = SeasonFor(calendar->GetMonth());
        if (const auto* tint = season.preset ? FindPreset(season.preset) : nullptr) {
            const float t = season.t;
            look.ambient     = LerpColor(look.ambient, tint->ambient, t);
            look.directional = LerpColor(look.directional, tint->directional, t);
            look.fog         = LerpColor(look.fog, tint->fog, t);
            look.fill        = LerpColor(look.fill, tint->fill, t);
            look.key.color       = LerpColor(look.key.color, tint->key.color, t);
            look.key.intensity   = LerpF(look.key.intensity, tint->key.intensity, t);
            look.fillLight.color     = LerpColor(look.fillLight.color, tint->fillLight.color, t);
            look.fillLight.intensity = LerpF(look.fillLight.intensity, tint->fillLight.intensity, t);
            look.rim.color       = LerpColor(look.rim.color, tint->rim.color, t);
            look.rim.intensity   = LerpF(look.rim.intensity, tint->rim.intensity, t);
        }
        return look;
    }

    Settings::TintValues Settings::CurrentTint() const {
        return { tintColor, tintStrength, tintSaturation, tintBrightness };
    }

    std::string Settings::DescribeTimeAndSeason() const {
        auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return "no calendar";
        }
        const float hour = calendar->GetHour();
        const auto season = SeasonFor(calendar->GetMonth());
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%02d:%02d %s: %s%s%s",
                      static_cast<int>(hour),
                      static_cast<int>((hour - static_cast<int>(hour)) * 60.0f),
                      calendar->GetMonthName().c_str(), TimePresetName(hour),
                      season.preset ? " + " : "", season.preset ? season.name : "");
        return buf;
    }

    void Settings::Load() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(kIniPath) < 0) {
            spdlog::info("No MenuStudio.ini found; using defaults.");
        }

        enabled       = ini.GetBoolValue("General", "bEnabled", enabled);
        waitForOwnerContext =
            ini.GetBoolValue("General", "bWaitForOwnerContext", waitForOwnerContext);
        tickAnimation = ini.GetBoolValue("General", "bTickAnimation", tickAnimation);
        driveSmp      = ini.GetBoolValue("General", "bDriveSmp", driveSmp);
        tickFace      = ini.GetBoolValue("General", "bTickFace", tickFace);
        tickCompanion = ini.GetBoolValue("General", "bTickCompanion", tickCompanion);
        companionLungeGuard = ini.GetBoolValue("General", "bCompanionLungeGuard",
                                               companionLungeGuard);
        companionLungeRepeats = static_cast<int>(ini.GetLongValue(
            "General", "iCompanionLungeRepeats", companionLungeRepeats));
        companionProbe = ini.GetBoolValue("General", "bCompanionProbe", companionProbe);
        companionProbeBone = ini.GetValue("General", "sCompanionProbeBone",
                                          companionProbeBone.c_str());
        tickMagicCasters = ini.GetBoolValue("General", "bTickMagicCasters", tickMagicCasters);
        forcePause    = ini.GetBoolValue("General", "bForcePause", forcePause);
        // Auto-tick force-pause when Skyrim Souls is actually loaded (Fuzzles'
        // suggestion). Force-pause exists ONLY to undo Souls' unpausing - with
        // Souls absent every covered menu already carries kPausesGame and
        // ForcePause::EnsurePaused early-outs, so the setting is inert either
        // way. What this buys is that a user who installs Souls later does not
        // have to know the checkbox exists, and that the log states the module's
        // presence outright: "force-pause is on but Souls is not loaded" and
        // "Souls is loaded but force-pause is off" are different bug reports and
        // used to look identical from a log.
        //
        // An EXPLICIT bForcePause in the INI always wins - detection only fills
        // in the default, so nobody's deliberate 0 is overridden by a DLL being
        // present. SimpleIni returns the fallback for a missing key, so ask for
        // the raw value to tell "absent" from "set to 0".
        soulsLoaded = ::GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr;
        if (ini.GetValue("General", "bForcePause", nullptr) == nullptr) {
            forcePause = soulsLoaded;
        }
        spdlog::info("Skyrim Souls: {}, force-pause {}{}.",
                     soulsLoaded ? "SkyrimSoulsRE.dll loaded" : "not loaded",
                     forcePause ? "ON" : "OFF",
                     ini.GetValue("General", "bForcePause", nullptr) ? " (set in INI)"
                                                                    : " (auto)");
        shadowPause = ini.GetBoolValue("General", "bShadowPause", shadowPause);
        blockRightMouse = ini.GetBoolValue("General", "bBlockRightMouse", blockRightMouse);
        actionBar  = ini.GetBoolValue("General", "bActionBar", actionBar);
        actionBarX = static_cast<float>(
            ini.GetDoubleValue("General", "fActionBarX", actionBarX));
        actionBarY = static_cast<float>(
            ini.GetDoubleValue("General", "fActionBarY", actionBarY));
        // ⚠⚠ NO MIGRATION ON THIS KEY ANY MORE, AND IT NEEDS NONE. There was
        // one for a day, to move installs off the carved default while auto was
        // the answer. Auto is gone as of 2026-08-28 and carved is the default
        // again, so every value already in the wild lands where it should: 1
        // stays carved, 0 was auto and StyleFromIni reads it as carved, and a 2
        // is a player who went and found plain.
        frameStyle =
            static_cast<int>(ini.GetLongValue("General", "iFrameStyle", frameStyle));
        // ⚠ fPlainRounding IS NOT READ. The plain tile takes a fraction of its
        // own width again, the way it did before the chamfer work; see
        // Settings.h. Save() deletes the key, not Load, because Load never
        // writes the file back and a Delete here would go with the local copy.
        studioCamera = ini.GetBoolValue("General", "bStudioCamera", studioCamera);
        cameraPivotNode = ini.GetValue("General", "sCameraPivotNode",
                                       cameraPivotNode.c_str());
        cameraOrbitSensitivity = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraOrbitSensitivity", cameraOrbitSensitivity));
        cameraTrackStep = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraTrackStep", cameraTrackStep));
        cameraTrackSlack = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraTrackSlack", cameraTrackSlack));
        cameraFocusMargin = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraFocusMargin", cameraFocusMargin));
        cameraTrackLensSlope = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraTrackLensSlope", cameraTrackLensSlope));
        cameraAttachmentFill = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraAttachmentFill", cameraAttachmentFill));
        cameraTrackLateral = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraTrackLateral", cameraTrackLateral));
        cameraTrackBodyMid = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraTrackBodyMid", cameraTrackBodyMid));
        cameraTrackFaceHeight = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraTrackFaceHeight", cameraTrackFaceHeight));
        cameraPanSensitivity = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraPanSensitivity", cameraPanSensitivity));
        cameraPanRange = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraPanRange", cameraPanRange));
        cameraPanRangeDown = static_cast<float>(ini.GetDoubleValue(
            "General", "fCameraPanRangeDown", cameraPanRangeDown));
        cameraSmoothing = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraSmoothing", cameraSmoothing));
        cameraMinDistance = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraMinDistance", cameraMinDistance));
        cameraOpenDistance = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraOpenDistance", cameraOpenDistance));
        cameraMaxDistance = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraMaxDistance", cameraMaxDistance));
        cameraZoneLeft = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraZoneLeft", cameraZoneLeft));
        cameraZoneTop = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraZoneTop", cameraZoneTop));
        cameraZoneRight = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraZoneRight", cameraZoneRight));
        cameraZoneBottom = static_cast<float>(
            ini.GetDoubleValue("General", "fCameraZoneBottom", cameraZoneBottom));
        rememberFraming = ini.GetBoolValue("General", "bRememberFraming",
                                           rememberFraming);
        framingSaved = ini.GetBoolValue("General", "bFramingSaved", framingSaved);
        framingYaw = static_cast<float>(
            ini.GetDoubleValue("General", "fFramingYaw", framingYaw));
        framingPitch = static_cast<float>(
            ini.GetDoubleValue("General", "fFramingPitch", framingPitch));
        framingZoom = static_cast<float>(
            ini.GetDoubleValue("General", "fFramingZoom", framingZoom));
        framingHeight = static_cast<float>(
            ini.GetDoubleValue("General", "fFramingHeight", framingHeight));
        framingLateral = static_cast<float>(
            ini.GetDoubleValue("General", "fFramingLateral", framingLateral));
        gridZoneLeft = static_cast<float>(
            ini.GetDoubleValue("General", "fGridZoneLeft", gridZoneLeft));
        gridZoneTop = static_cast<float>(
            ini.GetDoubleValue("General", "fGridZoneTop", gridZoneTop));
        gridZoneRight = static_cast<float>(
            ini.GetDoubleValue("General", "fGridZoneRight", gridZoneRight));
        gridZoneBottom = static_cast<float>(
            ini.GetDoubleValue("General", "fGridZoneBottom", gridZoneBottom));
        middleClickRecentre =
            ini.GetBoolValue("General", "bMiddleClickRecentre", middleClickRecentre);
        disableItemPreview3D =
            ini.GetBoolValue("General", "bDisableItemPreview3D", disableItemPreview3D);
        inspectNeedsHotkey =
            ini.GetBoolValue("General", "bInspectNeedsHotkey", inspectNeedsHotkey);
        bypassCameraCollision =
            ini.GetBoolValue("General", "bBypassCameraCollision", bypassCameraCollision);
        hideHudOverCustomMenus = ini.GetBoolValue(
            "General", "bHideHudOverCustomMenus", hideHudOverCustomMenus);
        standardizeLighting =
            ini.GetBoolValue("Declutter", "bStandardizeLighting", standardizeLighting);
        studioLightWithoutSpace = ini.GetBoolValue(
            "Declutter", "bStudioLightWithoutSpace", studioLightWithoutSpace);
        verboseLog    = ini.GetBoolValue("General", "bVerboseLog", verboseLog);
        maxDeltaTime  = static_cast<float>(
            ini.GetDoubleValue("General", "fMaxDeltaTime", maxDeltaTime));
        if (maxDeltaTime < 0.001f || maxDeltaTime > 0.5f) {
            maxDeltaTime = 0.05f;
        }

        declutterMode = static_cast<int>(
            ini.GetLongValue("Declutter", "iDeclutterMode", declutterMode));
        // r17 split the old dressing room (2) into void (2) and stage (3):
        // a pre-r17 INI that had the stage enabled means mode 3 now.
        if (declutterMode == 2 && ini.GetBoolValue("Backdrop", "bBackdrop", false)) {
            declutterMode = 3;
        }
        declutterMode = std::clamp(declutterMode, 0, 3);  // 0 Off .. 3 Dressing room
        // The INI value is the CONFIGURED mode; declutterMode is the effective
        // one the bubble re-derives per menu (see MenuWantsSpace). Seed both so
        // anything reading before the first arm sees the configured space.
        declutterModeIni = declutterMode;
        soloHideRadius = static_cast<float>(
            ini.GetDoubleValue("Declutter", "fSoloHideRadius", soloHideRadius));
        hideLightRefs = ini.GetBoolValue("Declutter", "bHideLightRefs", hideLightRefs);
        hidePlayerForCompanion = ini.GetBoolValue("Declutter", "bHidePlayerForCompanion",
                                                  hidePlayerForCompanion);
        faceCompanionToCamera = ini.GetBoolValue("Declutter", "bFaceCompanionToCamera",
                                                 faceCompanionToCamera);
        companionFacingOffset = static_cast<float>(ini.GetDoubleValue(
            "Declutter", "fCompanionFacingOffset", companionFacingOffset));
        cutCellLights = ini.GetBoolValue("Declutter", "bCutCellLights", cutCellLights);
        cutSunLight   = ini.GetBoolValue("Declutter", "bCutSunLight", cutSunLight);
        voidEngine    = ini.GetBoolValue("Declutter", "bVoidEngine", voidEngine);
        if (const char* raw = ini.GetValue("General", "sFreezeGraphBools", nullptr);
            raw && *raw) {
            ParseCsvList(raw, freezeGraphBools);
        }
        if (const char* raw = ini.GetValue("General", "sDiagGraphVars", nullptr);
            raw && *raw) {
            ParseCsvList(raw, diagGraphVars);
        }
        slowSwapExperiment =
            ini.GetBoolValue("General", "bSlowSwapExperiment", slowSwapExperiment);
        crossClassSheatheRedraw =
            ini.GetBoolValue("General", "bCrossClassSheatheRedraw", crossClassSheatheRedraw);
        liveEquipNotifyInMenus =
            ini.GetBoolValue("General", "bLiveEquipNotifyInMenus", liveEquipNotifyInMenus);
        diagnosticProbes =
            ini.GetBoolValue("General", "bDiagnosticProbes", diagnosticProbes);
        cameraCloseProbe =
            ini.GetBoolValue("General", "bCameraCloseProbe", cameraCloseProbe);
        playerCullProbe =
            ini.GetBoolValue("General", "bPlayerCullProbe", playerCullProbe);
        faceMeshRefresh =
            ini.GetBoolValue("General", "bFaceMeshRefresh", faceMeshRefresh);
        freezeCharacter =
            ini.GetBoolValue("General", "bFreezeCharacter", freezeCharacter);
        blinkStressTest =
            ini.GetBoolValue("General", "bBlinkStressTest", blinkStressTest);
        pumpStopsAtIdle =
            ini.GetBoolValue("General", "bPumpStopsAtIdle", pumpStopsAtIdle);
        movingArmStandsAside =
            ini.GetBoolValue("General", "bMovingArmStandsAside", movingArmStandsAside);
        freezeDrawSheathe =
            ini.GetBoolValue("General", "bFreezeDrawSheathe", freezeDrawSheathe);
        driveCbpc     = ini.GetBoolValue("General", "bDriveCbpc", driveCbpc);
        idleInMenus   = ini.GetBoolValue("General", "bIdleInMenus", idleInMenus);
        weaponPreviewInMenus =
            ini.GetBoolValue("General", "bWeaponPreviewInMenus", weaponPreviewInMenus);
        autoDrawInMenus =
            ini.GetBoolValue("General", "bAutoDrawInMenus", autoDrawInMenus);
        freezeHeadTracking =
            ini.GetBoolValue("General", "bFreezeHeadTracking", freezeHeadTracking);
        limitedEditor = ini.GetBoolValue("General", "bLimitedEditor", limitedEditor);
        pinBodyHeading = ini.GetBoolValue("General", "bPinBodyHeading", pinBodyHeading);
        // F-16: iFaceInMenus (0 hold / 1 live / 2 neutral). Legacy
        // bNeutralExpression (<= 0.5.x) migrates when the new key is absent:
        // 0 was "hold as caught" -> 0; 1 was the old shipped default -> the
        // new default (live) owns that slot now.
        if (ini.GetValue("General", "iFaceInMenus")) {
            faceInMenus = static_cast<int>(
                ini.GetLongValue("General", "iFaceInMenus", faceInMenus));
        } else if (ini.GetValue("General", "bNeutralExpression")) {
            faceInMenus =
                ini.GetBoolValue("General", "bNeutralExpression", true) ? 1 : 0;
        }
        faceInMenus = std::clamp(faceInMenus, 0, 2);
        previewSpin = ini.GetBoolValue("General", "bPreviewSpin", previewSpin);
        overrideSpimRotation =
            ini.GetBoolValue("General", "bOverrideSpimRotation", overrideSpimRotation);
        spinSensitivity = static_cast<float>(
            ini.GetDoubleValue("General", "fSpinSensitivity", spinSensitivity));
        spinSensitivity = std::clamp(spinSensitivity, 0.0005f, 0.05f);
        spinGamepadButton = static_cast<int>(
            ini.GetLongValue("General", "iSpinGamepadButton", spinGamepadButton));
        spinStickSensitivity = static_cast<float>(
            ini.GetDoubleValue("General", "fSpinStickSensitivity", spinStickSensitivity));
        spinStickSensitivity = std::clamp(spinStickSensitivity, 0.5f, 10.0f);
        showTryOnPrompt = ini.GetBoolValue("Preview", "bShowTryOnPrompt", showTryOnPrompt);
        tryOnLabelKbd   = ini.GetValue("Preview", "sTryOnLabelKbd", tryOnLabelKbd.c_str());
        tryOnLabelPad   = ini.GetValue("Preview", "sTryOnLabelPad", tryOnLabelPad.c_str());
        ownViewFirstPerson = ini.GetBoolValue("General", "bOwnViewFirstPerson",
                                              ownViewFirstPerson);
        ownViewUnmanaged = ini.GetBoolValue("General", "bOwnViewUnmanaged",
                                            ownViewUnmanaged);
        ownViewXOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewXOffset", ownViewXOffset));
        ownViewYOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewYOffset", ownViewYOffset));
        ownViewZOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewZOffset", ownViewZOffset));
        ownViewPitch = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewPitch", ownViewPitch));
        ownViewRotation = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewRotation", ownViewRotation));
        ownViewMountRaise = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountRaise", ownViewMountRaise));
        ownViewMountBoom = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountBoom", ownViewMountBoom));
        ownViewMountPitch = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountPitch", ownViewMountPitch));
        ownViewYOffset = std::clamp(ownViewYOffset, -200.0f, 140.0f);  // boom stays positive
        sleekTransitions = ini.GetBoolValue("General", "bSleekTransitions", sleekTransitions);
        transitionInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fTransitionIn", transitionInSeconds));
        transitionOutSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fTransitionOut", transitionOutSeconds));
        transitionInSeconds  = std::clamp(transitionInSeconds, 0.0f, 1.0f);
        transitionOutSeconds = std::clamp(transitionOutSeconds, 0.0f, 1.0f);
        dipToBlack = ini.GetBoolValue("General", "bDipToBlack", dipToBlack);
        dipOutSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fDipOutSeconds", dipOutSeconds));
        dipInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fDipInSeconds", dipInSeconds));
        sleekExit = ini.GetBoolValue("General", "bSleekExit", sleekExit);
        exitHoldSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitHoldSeconds", exitHoldSeconds));
        exitDipSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitDipSeconds", exitDipSeconds));
        exitInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitInSeconds", exitInSeconds));
        // r48: 0 = cut at close (the default); anything above holds the
        // studio through menu switches. The fader durations are retired
        // (r47) - parsed for INI compat, consumed by nothing.
        exitHoldSeconds = std::clamp(exitHoldSeconds, 0.0f, 0.30f);
        exitDipSeconds  = std::clamp(exitDipSeconds, 0.05f, 0.50f);
        exitInSeconds   = std::clamp(exitInSeconds, 0.05f, 1.0f);
        dipOutSeconds = std::clamp(dipOutSeconds, 0.0f, 0.50f);
        dipInSeconds  = std::clamp(dipInSeconds, 0.05f, 1.00f);
        soloHideRadius = std::clamp(soloHideRadius, 512.0f, 16384.0f);

        // [Backdrop]: stage preset first, then per-key overrides on top
        // (the same layering as [Lighting]); an explicitly empty mesh
        // value drops that piece.
        // Discover backdrop packs so the saved sStage/sBackground can resolve
        // against them (and the menu can list them). No bubble menu is open here.
        BackdropPacks::Scan();
        if (const char* raw = ini.GetValue("Backdrop", "sStage", nullptr); raw && *raw) {
            backdropStage = raw;
            std::transform(backdropStage.begin(), backdropStage.end(), backdropStage.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!ApplyStagePreset(backdropStage)) {
            spdlog::warn("Settings: [Backdrop] sStage='{}' unknown, using starlight.",
                         backdropStage);
            ApplyStagePreset("starlight");
        }
        if (const char* raw = ini.GetValue("Backdrop", "sBackground", nullptr); raw && *raw) {
            backdropBackground = raw;
            std::transform(backdropBackground.begin(), backdropBackground.end(),
                           backdropBackground.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        // Legacy name migration. The third vanilla perk dome used to be keyed
        // "teat", lifted straight from Bethesda's own mesh filename, which
        // really is spelled that way in Skyrim - Meshes1.bsa. The MESH path is
        // unchanged; only the user-facing key became "aurora". Without this, a
        // 0.5.0 INI that had picked it would fail the lookup below and fall
        // back to constellation, silently losing the user's choice.
        if (backdropBackground == "teat") {
            backdropBackground = "aurora";
            spdlog::info("Settings: migrated sBackground 'teat' -> 'aurora' "
                         "(same dome, the old key was Bethesda's filename).");
        }
        // ⚠⚠ AN EMPTY sBackground MEANS blank, AND IT MEANT THE STAR DOME UNTIL
        // 2026-08-28. The shipped template has carried `sBackground=` since the
        // key existed, with a comment directly above it calling blank "THE
        // DEFAULT, written here as an empty value", and tools/make_fomod.sh
        // guards that empty value in every generated flavour as "no dome by
        // default". None of the three was true. FindBackground("") matches no
        // preset, so the lookup below failed and the fallback put every fresh
        // install under constellation - a documented default that three
        // separate places asserted and the code had never honoured.
        //
        // ⚠ RESOLVED HERE RATHER THAN IN THE TEMPLATE, because the template is
        // only read on a FOMOD install. Every INI already on disk holds the
        // empty value too, and a fix that only edits the shipped file leaves
        // every existing player under the dome they never picked.
        if (backdropBackground.empty()) {
            backdropBackground = "blank";
        }
        if (!ApplyBackgroundPreset(backdropBackground)) {
            // A NON-EMPTY name that resolves to nothing is a different case and
            // keeps the old fallback: it is usually a backdrop pack the player
            // has uninstalled, and dropping them onto a visible dome says so
            // more loudly than dropping them onto an empty void would.
            spdlog::warn("Settings: [Backdrop] sBackground='{}' matches no "
                         "builtin or installed pack, using constellation.",
                         backdropBackground);
            ApplyBackgroundPreset("constellation");
        }
        if (const char* raw = ini.GetValue("Backdrop", "sFloorMesh", nullptr); raw) {
            backdropFloorMesh = raw;
        }
        if (const char* raw = ini.GetValue("Backdrop", "sDomeMesh", nullptr); raw) {
            backdropDomeMesh = raw;
        }
        if (const char* raw = ini.GetValue("Backdrop", "sStarsMesh", nullptr); raw) {
            backdropStarsMesh = raw;  // empty string = stars off
        }
        if (const char* raw = ini.GetValue("Backdrop", "sShellMesh", nullptr); raw) {
            backdropShellMesh = raw;
        }
        // F-7 v3 migration: every pre-r31 shell candidate is field-convicted
        // (loadscreen sphere = inner wall; vampire dome = seams + per-cell
        // color + fake constellation) - a stale value in a panel-saved
        // overwrite INI must not pin the old look past the fix. Genuine
        // custom paths (anything not on this list) are honored.
        {
            std::string lowered = backdropShellMesh;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* stale :
                 { "dlc01\\interface\\intvampireperkskydome.nif",
                   "loadscreenart\\loadscreensphere.nif",
                   "architecture\\solitude\\interiors\\slgcdome01.nif",
                   "architecture\\markarth\\mrktempledome01.nif" }) {
                if (lowered == stale) {
                    spdlog::info("Settings: sShellMesh '{}' is a retired shell, "
                                 "migrated to the shipped voidshell.", backdropShellMesh);
                    backdropShellMesh = "mtb\\voidshell.nif";
                    break;
                }
            }
        }
        backdropFloorRadius = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fFloorRadius", backdropFloorRadius));
        backdropDomeRadius = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fDomeRadius", backdropDomeRadius));
        backdropFloorZ = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fFloorZ", backdropFloorZ));
        backdropDomeZ = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fDomeZ", backdropDomeZ));
        backdropBrightness = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fBrightness", backdropBrightness));
        backdropImageBrightness = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fImageBrightness", backdropImageBrightness));
        backdropImageBrightness = std::clamp(backdropImageBrightness, 0.05f, 2.0f);
        voidBrightnessCap = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fVoidBrightnessCap", voidBrightnessCap));
        voidBrightnessCap = std::clamp(voidBrightnessCap, 0.02f, 0.35f);
        backgroundFaceCamera =
            ini.GetBoolValue("Backdrop", "bLockBackgroundAngle", backgroundFaceCamera);
        backgroundYawOffset = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fBackgroundYaw", backgroundYawOffset));
        backgroundYawOffset = std::clamp(backgroundYawOffset, -180.0f, 180.0f);
        backdropFloorRadius = std::clamp(backdropFloorRadius, 64.0f, 4096.0f);
        backdropDomeRadius  = BackdropPolicy::ClampBackgroundRadius(backdropDomeRadius);
        backdropFloorZ      = std::clamp(backdropFloorZ, -512.0f, 512.0f);
        backdropDomeZ       = std::clamp(backdropDomeZ, -4096.0f, 4096.0f);
        backdropBrightness  = std::clamp(backdropBrightness, 0.0f, 4.0f);

        // [Lighting]: preset first, then per-channel overrides on top.
        if (const char* raw = ini.GetValue("Lighting", "sPreset", nullptr); raw && *raw) {
            lightPreset = raw;
            std::transform(lightPreset.begin(), lightPreset.end(), lightPreset.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!ApplyLightPreset(lightPreset)) {
            spdlog::warn("Settings: [Lighting] sPreset='{}' unknown (studio/bright/warm/cool/"
                         "dusk), using studio values.", lightPreset);
            ApplyLightPreset("studio");
        }
        ParseColor(ini, "rAmbient", lightAmbient);
        ParseColor(ini, "rDirectional", lightDirectional);
        ParseColor(ini, "rFog", lightFog);
        ParseColor(ini, "rFill", lightFill);
        voidColorOverride =
            ini.GetBoolValue("Lighting", "bVoidColorOverride", voidColorOverride);
        ParseColor(ini, "rVoidColor", voidColor);

        // [Filter] - optional colour grade over any view (off by default).
        colorFilter = ini.GetBoolValue("Filter", "bColorFilter", colorFilter);
        ParseColor(ini, "rTintColor", tintColor, "Filter");
        tintStrength = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fTintStrength", tintStrength)), 0.0f, 1.0f);
        tintSaturation = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fSaturation", tintSaturation)), 0.0f, 1.0f);
        tintBrightness = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fBrightness", tintBrightness)), 0.0f, 1.5f);
        lightFogNear = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fFogNear", lightFogNear));
        lightFogFar = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fFogFar", lightFogFar));
        lightFogNear = std::clamp(lightFogNear, 256.0f, 20000.0f);
        lightFogFar  = std::clamp(lightFogFar, lightFogNear, 40000.0f);
        studioRig = ini.GetBoolValue("Lighting", "bStudioRig", studioRig);
        rigWithoutSpace = ini.GetBoolValue("Lighting", "bRigWithoutSpace", rigWithoutSpace);
        studioInLiveMenus =
            ini.GetBoolValue("Lighting", "bStudioInLiveMenus", studioInLiveMenus);
        rigBrightness = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fRigBrightness", rigBrightness));
        rigBrightness = std::clamp(rigBrightness, 0.0f, 4.0f);
        const auto loadRigLight = [&](const char* a_prefix, RigLight& a_light) {
            std::string key = std::string("b") + a_prefix;
            a_light.enabled = ini.GetBoolValue("Lighting", key.c_str(), a_light.enabled);
            key = std::string("r") + a_prefix + "Color";
            ParseColor(ini, key.c_str(), a_light.color);
            key = std::string("f") + a_prefix + "Intensity";
            a_light.intensity = static_cast<float>(
                ini.GetDoubleValue("Lighting", key.c_str(), a_light.intensity));
            a_light.intensity = std::clamp(a_light.intensity, 0.0f, 4.0f);
        };
        loadRigLight("RigKey", rigKey);
        loadRigLight("RigFill", rigFill);
        loadRigLight("RigRim", rigRim);
        matchTimeAndSeason = ini.GetBoolValue("Lighting", "bMatchTimeAndSeason", matchTimeAndSeason);

        // Which menus get the SPACE (void / dressing room). Same comma-list
        // shape as sMenus; absent key keeps the default (all four = the
        // pre-0.6.0 behavior, so nobody's setup changes on update).
        if (const char* raw = ini.GetValue("General", "sSpaceMenus", nullptr);
            raw && *raw) {
            spaceMenus.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    spaceMenus.insert(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }

        // §4b: which menus are handed back to Skyrim Souls, per menu. Same
        // comma-list shape as sMenus. An ABSENT key leaves the set empty, which
        // is the pre-0.6.1 behaviour, so updating changes nothing on its own -
        // the old all-or-nothing bForcePause=0 still works and still wins.
        if (const char* raw = ini.GetValue("General", "sSoulsLiveMenus", nullptr);
            raw && *raw) {
            soulsLiveMenus.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    soulsLiveMenus.insert(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }

        // Strip buttons switched off, by id. Same comma-list shape as the sets
        // above. An absent key leaves the set empty, which is every button
        // showing - see the note on the field for why the OFF ids are the ones
        // written down.
        if (const char* raw = ini.GetValue("General", "sHiddenActions", nullptr);
            raw && *raw) {
            hiddenActions.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    hiddenActions.insert(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }

        if (const char* raw = ini.GetValue("General", "sMenus", nullptr); raw && *raw) {
            menus.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    menus.insert(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }

        // r27 stabilization: the retired SPIM-harvest rotation key and the
        // black fade-in stay DISABLED IN CODE (the fader dip is parked on
        // the r26 input-capture suspicion). Forced after the INI read so a
        // stale panel-saved INI in MO2's overwrite can't re-enable them.
        // Rotation returned in r29 as bPreviewSpin - our OWN input sink,
        // opt-in, not this key.
        if (pinBodyHeading || dipToBlack || transitionInSeconds > 0.0f) {
            spdlog::info("Settings: bPinBodyHeading/bDipToBlack/fTransitionIn are "
                         "retired or parked in this build (values ignored).");
        }
        pinBodyHeading = false;
        dipToBlack = false;
        transitionInSeconds = 0.0f;

        ++revision;

        std::string names;
        for (const auto& m : menus) {
            names += names.empty() ? m : ", " + m;
        }
        spdlog::info("Settings: enabled={} forcePause={} tickAnimation={} driveSmp={} tickFace={} "
                     "tickMagic={} maxDt={:.3f} menus=[{}] space: mode={} solo(r={:.0f}) "
                     "bg={} stage={} (floor '{}' r={:.0f} z={:.0f}, dome '{}' r={:.0f} z={:.0f}) "
                     "light: preset={} amb=({},{},{}) fog=({},{},{})",
                     enabled, forcePause, tickAnimation, driveSmp, tickFace, tickMagicCasters,
                     maxDeltaTime, names, declutterMode, soloHideRadius, backdropBackground,
                     backdropStage,
                     backdropFloorMesh, backdropFloorRadius, backdropFloorZ, backdropDomeMesh,
                     backdropDomeRadius, backdropDomeZ, lightPreset,
                     lightAmbient.red, lightAmbient.green, lightAmbient.blue,
                     lightFog.red, lightFog.green, lightFog.blue);
        // r22 A/B, on its own line so one grep answers "which arm produced
        // this log". §0's lesson: a build that reports its own identity costs
        // one line and settles in one grep what otherwise costs a field round.
        // One line, and only when something is NOT at its shipped default -
        // then a support log says so without a user having to be asked. All
        // defaults stays silent, which is the common case.
        if (!liveEquipNotifyInMenus || crossClassSheatheRedraw || autoDrawInMenus ||
            slowSwapExperiment || diagnosticProbes) {
            spdlog::info("Settings: weapon preview overrides: bLiveEquipNotifyInMenus={} "
                         "bCrossClassSheatheRedraw={} bAutoDrawInMenus={} "
                         "bSlowSwapExperiment={} bDiagnosticProbes={}",
                         liveEquipNotifyInMenus, crossClassSheatheRedraw, autoDrawInMenus,
                         slowSwapExperiment, diagnosticProbes);
        }
        // OS-103(b): its own line, because a probe you have to switch on by
        // hand fails silently in exactly one way, and it is always the same
        // one: the key never reached the loader and the absence of output reads
        // as "no clears observed". This line is the difference between a
        // measurement and a shrug.
        if (playerCullProbe) {
            // ⚠ THE PER-FRAME RE-CULL THIS USED TO ANNOUNCE IS GONE, since
            // abacad8. Saying otherwise at the head of the log is worse than
            // saying nothing: the phase tally below only counts culled-then-
            // un-culled TRANSITIONS, and with nothing putting the flag back each
            // frame it reads 0 whether the vfunc fix holds or not. Read the
            // CullWatch trace instead, which records what each write left
            // behind and needs no re-cull to mean anything.
            spdlog::info("Settings: bPlayerCullProbe=true. The player-cull watch is ON while "
                         "a framed companion holds the shot. Read the ordered "
                         "'CullWatch: trip N ... left the flags word' lines and the 'last "
                         "write that left him ON SCREEN' verdict under them. ⚠ The "
                         "'declutter/probe: gap X -> Y' tallies count transitions only and "
                         "read 0 once the flag stops being put back, so they cannot score "
                         "the fix. Turn this back off after the round.");
        }
        // Same rule for the face mesh bake: silent at its default, loud when
        // someone has turned the blink fix off.
        if (!faceMeshRefresh) {
            spdlog::info("Settings: bFaceMeshRefresh=0: the face MESH is NOT baked "
                         "while a menu is up; expect frozen eyes.");
        }
        if (freezeCharacter) {
            spdlog::info("Settings: bFreezeCharacter=1: the character holds the frame the "
                         "menu caught (no graph, no settle, no face). Hair/cloth, body "
                         "physics and the weapon preview stay live.");
        }
        // ⚠⚠ A DIAGNOSTIC BUILD FORCES ITS OWN INSTRUMENTS ON, LAST, AFTER THE
        // WHOLE FILE HAS BEEN READ. A reporter's own INI wins over every code
        // default ([[live-ini-overrides-code-defaults]]), so a diagnostic build
        // that only changed defaults would come back with the log switched off
        // and a wasted round trip. Asking them to edit two keys by hand is the
        // same wasted round trip with extra steps.
        //
        // ⚠ bVerboseLog IS READ AND NEVER SAVED, so forcing it cannot reach
        // their file. bCameraCloseProbe IS saved, so its original is kept and
        // Save() writes that instead: a build handed out to answer one report
        // must not leave a setting behind after they go back to the release.
#ifdef MENUSTUDIO_DIAG
        g_diagProbeAsRead = cameraCloseProbe;
        verboseLog = true;
        cameraCloseProbe = true;
        spdlog::warn("Settings: DIAGNOSTIC BUILD. bVerboseLog and bCameraCloseProbe "
                     "are forced ON in memory whatever MenuStudio.ini says (the file "
                     "read bCameraCloseProbe={}), so this log is complete without "
                     "anyone editing anything. Your saved setting is untouched. This "
                     "build is for diagnosing one report and is not a release.",
                     g_diagProbeAsRead);
#endif
    }

    void Settings::Save() {
        // Load-modify-save: only the panel-edited keys change; every other
        // key and the shipped comment blocks survive the round-trip.
        CSimpleIniA ini;
        ini.SetUnicode();
        ini.LoadFile(kIniPath);

        ini.SetBoolValue("General", "bEnabled", enabled);
        ini.SetBoolValue("General", "bWaitForOwnerContext", waitForOwnerContext);
        // ⚠ DELETED WHEN IT MATCHES THE DETECTION, never written back blind.
        // The shipped INI leaves bForcePause ABSENT on purpose so Load() can
        // derive it from SkyrimSoulsRE.dll (see 326-329). Save() runs after ANY
        // control on ANY tab changes (SettingsUI.cpp:735-736), so writing the
        // derived value back turned one unrelated slider drag into a permanent
        // override. The bad direction is silent and sticky: a `false` pinned
        // while Souls was absent goes on declining every bubble menu after Souls
        // is installed, which reads in the field as "the action bar is just not
        // there" - the studio never arms, so the window that carries it never
        // opens (ActionBar.cpp:766).
        //
        // Same discipline the preset overrides already use below: a key that
        // agrees with what detection would say does not need to exist.
        if (forcePause == soulsLoaded) {
            ini.Delete("General", "bForcePause", true);
        } else {
            ini.SetBoolValue("General", "bForcePause", forcePause);
        }
        ini.SetBoolValue("General", "bTickAnimation", tickAnimation);
        ini.SetBoolValue("General", "bTickFace", tickFace);
        ini.SetBoolValue("General", "bTickCompanion", tickCompanion);
        ini.SetBoolValue("General", "bTickMagicCasters", tickMagicCasters);
        ini.SetBoolValue("General", "bIdleInMenus", idleInMenus);
        ini.SetBoolValue("General", "bWeaponPreviewInMenus", weaponPreviewInMenus);
        ini.SetBoolValue("General", "bAutoDrawInMenus", autoDrawInMenus);
        ini.SetBoolValue("General", "bFreezeHeadTracking", freezeHeadTracking);
        ini.SetBoolValue("General", "bLimitedEditor", limitedEditor);
        ini.SetLongValue("General", "iFaceInMenus", faceInMenus);
        ini.SetBoolValue("General", "bPreviewSpin", previewSpin);
        ini.SetBoolValue("General", "bOverrideSpimRotation", overrideSpimRotation);
        ini.SetBoolValue("Preview", "bShowTryOnPrompt", showTryOnPrompt);
        ini.SetValue("Preview", "sTryOnLabelKbd", tryOnLabelKbd.c_str());
        ini.SetValue("Preview", "sTryOnLabelPad", tryOnLabelPad.c_str());
        ini.SetBoolValue("General", "bOwnViewFirstPerson", ownViewFirstPerson);
        ini.SetBoolValue("General", "bOwnViewUnmanaged", ownViewUnmanaged);
        ini.SetBoolValue("General", "bSleekTransitions", sleekTransitions);
        ini.SetDoubleValue("General", "fTransitionIn", transitionInSeconds);
        ini.SetDoubleValue("General", "fTransitionOut", transitionOutSeconds);
        ini.SetBoolValue("General", "bDipToBlack", dipToBlack);
        ini.SetBoolValue("General", "bSleekExit", sleekExit);
        ini.SetDoubleValue("General", "fExitHoldSeconds", exitHoldSeconds);
        ini.SetDoubleValue("General", "fExitDipSeconds", exitDipSeconds);
        ini.SetDoubleValue("General", "fExitInSeconds", exitInSeconds);
        ini.SetBoolValue("General", "bDriveSmp", driveSmp);
        ini.SetBoolValue("General", "bSlowSwapExperiment", slowSwapExperiment);
        ini.SetBoolValue("General", "bCrossClassSheatheRedraw", crossClassSheatheRedraw);
        ini.SetBoolValue("General", "bLiveEquipNotifyInMenus", liveEquipNotifyInMenus);
        ini.SetBoolValue("General", "bDiagnosticProbes", diagnosticProbes);
        // ⚠ THE VALUE AS READ, NOT THE FORCED ONE. See the note at the end of
        // Load: a diagnostic build must not persist its own instrument.
#ifdef MENUSTUDIO_DIAG
        ini.SetBoolValue("General", "bCameraCloseProbe", g_diagProbeAsRead);
#else
        ini.SetBoolValue("General", "bCameraCloseProbe", cameraCloseProbe);
#endif
        ini.SetBoolValue("Declutter", "bFaceCompanionToCamera", faceCompanionToCamera);
        ini.SetDoubleValue("Declutter", "fCompanionFacingOffset", companionFacingOffset);
        ini.SetBoolValue("General", "bPlayerCullProbe", playerCullProbe);
        ini.SetBoolValue("General", "bFaceMeshRefresh", faceMeshRefresh);
        ini.SetBoolValue("General", "bFreezeCharacter", freezeCharacter);
        ini.SetBoolValue("General", "bBlinkStressTest", blinkStressTest);
        ini.SetBoolValue("General", "bPumpStopsAtIdle", pumpStopsAtIdle);
        ini.SetBoolValue("General", "bMovingArmStandsAside", movingArmStandsAside);
        ini.SetBoolValue("General", "bFreezeDrawSheathe", freezeDrawSheathe);
        ini.SetBoolValue("General", "bDriveCbpc", driveCbpc);
        ini.SetBoolValue("General", "bShadowPause", shadowPause);
        ini.SetBoolValue("General", "bBlockRightMouse", blockRightMouse);
        ini.SetBoolValue("General", "bActionBar", actionBar);
        ini.SetDoubleValue("General", "fActionBarX", actionBarX);
        ini.SetDoubleValue("General", "fActionBarY", actionBarY);
        // ⚠ THE STAMP GOES IN ON EVERY SAVE even though nothing gates on it
        // today. The frame migration it was added for is gone; the number stays
        // so the next migration can tell a file written before it from one
        // written after.
        ini.SetLongValue("General", "iSettingsVersion",
                         static_cast<long>(kSettingsVersion),
                         "; which migrations this file has already been "
                         "through. Do not edit.");
        ini.SetLongValue("General", "iFrameStyle", frameStyle,
                         "; corner shape for our own chrome: 1 carved (the "
                         "default), 2 plain");
        // ⚠ REMOVED RATHER THAN LEFT UNWRITTEN, the same rule fPlainRounding
        // below is under. sCarvedPresets told auto which FLICK presets the carve
        // was for, and auto is gone.
        ini.Delete("General", "sCarvedPresets", true);
        // ⚠ REMOVED RATHER THAN LEFT UNWRITTEN. Save() edits the file in place,
        // so not setting a key leaves whatever was already on that line. Nothing
        // reads fPlainRounding now and an editable line with nothing behind it
        // is worse than no line. See Settings.h.
        ini.Delete("General", "fPlainRounding", true);
        ini.SetBoolValue("General", "bStudioCamera", studioCamera);
        ini.SetValue("General", "sCameraPivotNode", cameraPivotNode.c_str());
        ini.SetDoubleValue("General", "fCameraOrbitSensitivity", cameraOrbitSensitivity);
        ini.SetDoubleValue("General", "fCameraTrackStep", cameraTrackStep);
        ini.SetDoubleValue("General", "fCameraTrackSlack", cameraTrackSlack);
        ini.SetDoubleValue("General", "fCameraFocusMargin", cameraFocusMargin);
        ini.SetDoubleValue("General", "fCameraTrackLensSlope", cameraTrackLensSlope);
        ini.SetDoubleValue("General", "fCameraAttachmentFill", cameraAttachmentFill);
        ini.SetDoubleValue("General", "fCameraTrackLateral", cameraTrackLateral);
        ini.SetDoubleValue("General", "fCameraTrackBodyMid", cameraTrackBodyMid);
        ini.SetDoubleValue("General", "fCameraTrackFaceHeight", cameraTrackFaceHeight);
        ini.SetDoubleValue("General", "fCameraPanSensitivity", cameraPanSensitivity);
        ini.SetDoubleValue("General", "fCameraPanRange", cameraPanRange);
        ini.SetDoubleValue("General", "fCameraPanRangeDown", cameraPanRangeDown);
        ini.SetDoubleValue("General", "fCameraSmoothing", cameraSmoothing);
        ini.SetDoubleValue("General", "fCameraMinDistance", cameraMinDistance);
        ini.SetDoubleValue("General", "fCameraOpenDistance", cameraOpenDistance);
        ini.SetDoubleValue("General", "fCameraMaxDistance", cameraMaxDistance);
        ini.SetDoubleValue("General", "fCameraZoneLeft", cameraZoneLeft);
        ini.SetDoubleValue("General", "fCameraZoneTop", cameraZoneTop);
        ini.SetDoubleValue("General", "fCameraZoneRight", cameraZoneRight);
        ini.SetDoubleValue("General", "fCameraZoneBottom", cameraZoneBottom);
        // ⚠ THE SHOT ITSELF IS WRITTEN BY StudioCamera AT THE CLOSE, not by a
        // panel control, so these five reach the file through this same
        // load-modify-save like everything else. Only the toggle is a
        // preference; the numbers are a recording of what the player did.
        ini.SetBoolValue("General", "bRememberFraming", rememberFraming);
        ini.SetBoolValue("General", "bFramingSaved", framingSaved);
        ini.SetDoubleValue("General", "fFramingYaw", framingYaw);
        ini.SetDoubleValue("General", "fFramingPitch", framingPitch);
        ini.SetDoubleValue("General", "fFramingZoom", framingZoom);
        ini.SetDoubleValue("General", "fFramingHeight", framingHeight);
        ini.SetDoubleValue("General", "fFramingLateral", framingLateral);
        ini.SetDoubleValue("General", "fGridZoneLeft", gridZoneLeft);
        ini.SetDoubleValue("General", "fGridZoneTop", gridZoneTop);
        ini.SetDoubleValue("General", "fGridZoneRight", gridZoneRight);
        ini.SetDoubleValue("General", "fGridZoneBottom", gridZoneBottom);
        ini.SetBoolValue("General", "bMiddleClickRecentre", middleClickRecentre);
        ini.SetBoolValue("General", "bDisableItemPreview3D", disableItemPreview3D);
        ini.SetBoolValue("General", "bInspectNeedsHotkey", inspectNeedsHotkey);
        ini.SetBoolValue("General", "bBypassCameraCollision", bypassCameraCollision);
        ini.SetBoolValue("General", "bHideHudOverCustomMenus", hideHudOverCustomMenus,
                         "; hide the compass and crosshair while the studio is armed "
                         "over a menu that leaves the HUD drawing. The vanilla menus "
                         "put the game in menu mode and the HUD stands itself down, "
                         "so this only ever reaches another mod's own window");
        // CONFIGURED, never the effective value - saving while a no-space menu
        // is open must not persist that menu's temporary 0.
        ini.SetLongValue("Declutter", "iDeclutterMode", declutterModeIni);
        // The panel's per-menu space checkboxes edit spaceMenus, so it has to
        // round-trip. Written in the same fixed order every time so the file
        // does not churn between saves.
        {
            std::string list;
            // "RaceSex Menu" carries its space - the engine's own name. A
            // member missing from THIS list is dropped on the next save even
            // though it loads fine, which is a silent revert.
            for (const char* m : { "ContainerMenu", "BarterMenu", "InventoryMenu",
                                   "MagicMenu", "GridInventoryMenu", "RaceSex Menu" }) {
                if (spaceMenus.contains(m)) {
                    if (!list.empty()) {
                        list += ",";
                    }
                    list += m;
                }
            }
            ini.SetValue("General", "sSpaceMenus", list.c_str(),
                         "; which menus get the void / dressing room (the rest keep "
                         "the normal world; pause + physics are unaffected)");
        }
        // The Menus tab edits this now, so it has to round-trip or a tick would
        // last exactly until the next Load().
        //
        // ⚠ THE SAME FIXED-ORDER RULE AS sSpaceMenus ABOVE, AND THE SAME TRAP:
        // a member missing from THIS list loads fine and is dropped on the next
        // save, which is a silent revert. 'RaceSex Menu' is therefore written
        // even though the panel deliberately does not offer it - somebody who
        // added it by hand must not lose it by opening the settings.
        //
        // ⚠ THE COMMENT ARGUMENT DOES NOT OVERWRITE AN EXISTING ONE. Measured
        // against the deployed INI, which has been panel-saved many times and
        // still carries the shipped block above sSpaceMenus in full. It is only
        // used when the key is absent, which is the case on an INI that predates
        // this key.
        {
            std::string list;
            for (const char* m : { "ContainerMenu", "BarterMenu", "InventoryMenu",
                                   "MagicMenu", "GridInventoryMenu", "RaceSex Menu" }) {
                if (menus.contains(m)) {
                    if (!list.empty()) {
                        list += ",";
                    }
                    list += m;
                }
            }
            ini.SetValue("General", "sMenus", list.c_str(),
                         "; which menus Menu Studio takes at all (editable on the "
                         "Menus tab; a change applies at the next open of that menu)");
        }
        // §4b. Fixed emit order for the same reason as sSpaceMenus: the set is
        // unordered, so writing it in iteration order would churn the file on
        // every save.
        {
            std::string list;
            for (const char* m : { "ContainerMenu", "BarterMenu", "InventoryMenu",
                                   "MagicMenu", "GridInventoryMenu" }) {
                if (soulsLiveMenus.contains(m)) {
                    if (!list.empty()) {
                        list += ",";
                    }
                    list += m;
                }
            }
            ini.SetValue("General", "sSoulsLiveMenus", list.c_str(),
                         "; with Skyrim Souls installed, which menus stay LIVE and "
                         "unpaused (no studio); the rest are paused so the studio "
                         "works. Empty = studio in all of them");
        }
        // The buttons switched off. SORTED rather than emitted in a fixed list
        // like the two above, because those know every member at compile time
        // and this one cannot: any plugin may register an id. Sorting is what
        // keeps an unordered set from rewriting the line in a new order on
        // every save, which is the same churn the fixed orders exist to avoid.
        {
            std::vector<std::string> ids(hiddenActions.begin(), hiddenActions.end());
            std::sort(ids.begin(), ids.end());
            std::string list;
            for (const auto& id : ids) {
                if (!list.empty()) {
                    list += ",";
                }
                list += id;
            }
            ini.SetValue("General", "sHiddenActions", list.c_str(),
                         "; buttons hidden from the on-screen strip, by id. Empty = "
                         "every button shows, including ones added by other mods");
        }
        ini.SetBoolValue("Declutter", "bStandardizeLighting", standardizeLighting);
        ini.SetBoolValue("Declutter", "bStudioLightWithoutSpace", studioLightWithoutSpace);
        ini.SetBoolValue("Declutter", "bCutCellLights", cutCellLights);
        ini.SetBoolValue("Declutter", "bCutSunLight", cutSunLight);
        ini.SetBoolValue("Declutter", "bVoidEngine", voidEngine);
        {
            std::string joined;
            for (const auto& var : freezeGraphBools) {
                joined += joined.empty() ? var : ", " + var;
            }
            ini.SetValue("General", "sFreezeGraphBools", joined.c_str());
        }
        {
            std::string joined;
            for (const auto& var : diagGraphVars) {
                joined += joined.empty() ? var : ", " + var;
            }
            ini.SetValue("General", "sDiagGraphVars", joined.c_str());
        }
        // bBackdrop retired in r17 (the stage is view mode 3 now); drop the
        // stale key so the 2→3 migration can't re-trigger.
        ini.Delete("Backdrop", "bBackdrop");
        ini.SetValue("Backdrop", "sStage", backdropStage.c_str());
        ini.SetValue("Backdrop", "sBackground", backdropBackground.c_str());
        // Backdrop keys carry only values differing from the active
        // presets (floor keys vs the stage, dome keys vs the background) -
        // a clean pick leaves the INI clean (the [Lighting] policy).
        const auto* activeStage = FindStage(backdropStage);
        const auto* activeBg = FindBackground(backdropBackground);
        const auto diffString = [&](const char* a_key, const std::string& a_value,
                                    const char* a_presetValue) {
            if (a_presetValue && a_value == a_presetValue) {
                ini.Delete("Backdrop", a_key);
            } else {
                ini.SetValue("Backdrop", a_key, a_value.c_str());
            }
        };
        const auto diffFloat = [&](const char* a_key, float a_value, bool a_hasPreset,
                                   float a_presetValue) {
            if (a_hasPreset && std::abs(a_value - a_presetValue) < 0.05f) {
                ini.Delete("Backdrop", a_key);
            } else {
                ini.SetDoubleValue("Backdrop", a_key, a_value);
            }
        };
        diffString("sFloorMesh", backdropFloorMesh,
                   activeStage ? activeStage->floorMesh : nullptr);
        diffString("sDomeMesh", backdropDomeMesh, activeBg ? activeBg->mesh : nullptr);
        diffFloat("fFloorRadius", backdropFloorRadius, activeStage != nullptr,
                  activeStage ? activeStage->floorRadius : 0.0f);
        diffFloat("fDomeRadius", backdropDomeRadius, activeBg != nullptr,
                  activeBg ? activeBg->radius : 0.0f);
        diffFloat("fFloorZ", backdropFloorZ, activeStage != nullptr,
                  activeStage ? activeStage->floorZ : 0.0f);
        diffFloat("fDomeZ", backdropDomeZ, activeBg != nullptr,
                  activeBg ? activeBg->z : 0.0f);
        ini.SetDoubleValue("Backdrop", "fBrightness", backdropBrightness);
        ini.SetDoubleValue("Backdrop", "fImageBrightness", backdropImageBrightness);
        ini.SetBoolValue("Backdrop", "bLockBackgroundAngle", backgroundFaceCamera);
        ini.SetDoubleValue("Backdrop", "fBackgroundYaw", backgroundYawOffset);
        ini.SetValue("Lighting", "sPreset", lightPreset.c_str());
        ini.SetDoubleValue("Lighting", "fFogNear", lightFogNear);
        ini.SetDoubleValue("Lighting", "fFogFar", lightFogFar);
        ini.SetBoolValue("Lighting", "bStudioRig", studioRig);
        ini.SetBoolValue("Lighting", "bRigWithoutSpace", rigWithoutSpace);
        ini.SetBoolValue("Lighting", "bStudioInLiveMenus", studioInLiveMenus);
        ini.SetDoubleValue("Lighting", "fRigBrightness", rigBrightness);
        ini.SetBoolValue("Lighting", "bMatchTimeAndSeason", matchTimeAndSeason);

        // Override keys carry only channels that differ from the active
        // preset - a clean preset pick leaves the INI clean.
        const LightPreset* active = nullptr;
        for (const auto& preset : kLightPresets) {
            if (lightPreset == preset.name) {
                active = &preset;
                break;
            }
        }

        const auto saveRigLight = [&](const char* a_prefix, const RigLight& a_light,
                                      const LightPreset::RigLook* a_presetLook) {
            std::string key = std::string("b") + a_prefix;
            ini.SetBoolValue("Lighting", key.c_str(), a_light.enabled);
            key = std::string("r") + a_prefix + "Color";
            if (a_presetLook && a_light.color == a_presetLook->color) {
                ini.Delete("Lighting", key.c_str());
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u,%u,%u", a_light.color.red,
                              a_light.color.green, a_light.color.blue);
                ini.SetValue("Lighting", key.c_str(), buf);
            }
            key = std::string("f") + a_prefix + "Intensity";
            if (a_presetLook && std::abs(a_light.intensity - a_presetLook->intensity) < 0.005f) {
                ini.Delete("Lighting", key.c_str());
            } else {
                ini.SetDoubleValue("Lighting", key.c_str(), a_light.intensity);
            }
        };
        saveRigLight("RigKey", rigKey, active ? &active->key : nullptr);
        saveRigLight("RigFill", rigFill, active ? &active->fillLight : nullptr);
        saveRigLight("RigRim", rigRim, active ? &active->rim : nullptr);
        const auto writeOverride = [&](const char* a_key, const RE::Color& a_value,
                                       const RE::Color& a_presetValue) {
            if (active && a_value == a_presetValue) {
                ini.Delete("Lighting", a_key);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u,%u,%u", a_value.red, a_value.green,
                              a_value.blue);
                ini.SetValue("Lighting", a_key, buf);
            }
        };
        writeOverride("rAmbient", lightAmbient, active ? active->ambient : lightAmbient);
        writeOverride("rDirectional", lightDirectional,
                      active ? active->directional : lightDirectional);
        writeOverride("rFog", lightFog, active ? active->fog : lightFog);
        writeOverride("rFill", lightFill, active ? active->fill : lightFill);
        ini.SetBoolValue("Lighting", "bVoidColorOverride", voidColorOverride);
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u,%u,%u", voidColor.red,
                          voidColor.green, voidColor.blue);
            ini.SetValue("Lighting", "rVoidColor", buf);
        }

        ini.SetBoolValue("Filter", "bColorFilter", colorFilter);
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u,%u,%u", tintColor.red,
                          tintColor.green, tintColor.blue);
            ini.SetValue("Filter", "rTintColor", buf);
        }
        ini.SetDoubleValue("Filter", "fTintStrength", tintStrength);
        ini.SetDoubleValue("Filter", "fSaturation", tintSaturation);
        ini.SetDoubleValue("Filter", "fBrightness", tintBrightness);

        if (ini.SaveFile(kIniPath) < 0) {
            spdlog::warn("Settings: could not write MenuStudio.ini.");
        } else {
            // r28f: the pause master is the one value whose silent change has
            // now cost a field round, so its transitions are logged from the
            // WRITER's side too. If a future log shows this line with no
            // matching panel-click line above it, the flip came from code, not
            // the user - that distinction is exactly what the last log could
            // not make.
            static int s_lastSavedForcePause = -1;
            const int  fp                    = forcePause ? 1 : 0;
            if (s_lastSavedForcePause != -1 && s_lastSavedForcePause != fp) {
                spdlog::info("Settings: forcePause CHANGED across saves {} -> {}.",
                             s_lastSavedForcePause != 0, forcePause);
            }
            s_lastSavedForcePause = fp;
            spdlog::info("Settings saved (panel).");
        }
        ++revision;
    }
}

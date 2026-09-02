#include "PCH.h"

#include "Backdrop.h"

#include "BackdropAnchorPolicy.h"
#include "BackdropImage.h"
#include "BackdropImagePolicy.h"
#include "CsCubemapBridge.h"
#include "Offsets.h"
#include "Settings.h"
#include "Transition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

namespace {
    // BSModelDB::DBTraits::ArgsType (NG layout, 0xC). Demand prefixes
    // "meshes\\" itself and resolves through the model DB - the returned
    // root is the CACHED TEMPLATE (entry+0x28, refcount-bumped), shared
    // with every placed reference using that model. It must be cloned,
    // never attached (decompile: Tools\re\research\mtb_modeldb.c).
    struct ModelDBArgs {
        std::uint32_t lodMult{ 0 };
        std::uint32_t texLoadLevel{ 3 };
        bool          unk8{ true };
        bool          unk9{ false };
        bool          unkA{ true };
        bool          postProcess{ true };
    };
    static_assert(sizeof(ModelDBArgs) == 0xC);

    using DemandModel_t = std::int32_t (*)(const char*, RE::NiPointer<RE::NiNode>&,
                                           const ModelDBArgs&);
    // Engine deep clone: stack NiCloningProcess with the engine's own
    // copyType/appendChar globals + CreateClone/ProcessClone - the same
    // instantiate-a-template path placed refs use (mtb_niclone.c).
    using NiClone_t = RE::NiAVObject* (*)(RE::NiAVObject*);

    REL::Relocation<DemandModel_t> g_demandModel{ MTB::Offsets::BSModelDBDemand };
    REL::Relocation<NiClone_t>     g_niClone{ MTB::Offsets::NiAVObjectClone };

    // Shader values we overwrite for the tint. CreateClone's base
    // implementation SHARES properties/materials between clone and cached
    // template, so an un-restored write could tint every placed instance
    // of the mesh in the world and outlive the menu - save at Apply,
    // restore at Remove (all three exits, SPEC §4 invariant 1).
    struct TintBackup {
        RE::NiPointer<RE::BSGeometry> geom;
        bool                          isLighting{ false };
        bool                          hadOwnEmit{ false };
        bool                          hadEmissivePtr{ false };
        RE::NiColor                   emissive{};
        float                         emissiveMult{ 1.0f };
        RE::NiColorA                  baseColor{};
        float                         baseColorScale{ 1.0f };
    };

    // A piece to build: floor/dome come from the Settings fields (panel-
    // dialable), extras from the active stage preset. Names carry the
    // MTB_ prefix = exempt from our own culls (r8).
    struct PieceDef {
        std::string name;
        std::string mesh;
        std::string image;              // non-empty: repoint the image sphere to this DDS
        float       fitRadius{ 0.0f };  // >0: scale bound radius to this
        float       scale{ 1.0f };      // used when fitRadius == 0
        float       x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
        float       yawDeg{ 0.0f };
        bool        tint{ true };
        bool        dialable{ false };  // floor/dome: panel sliders refit live
        bool        isDome{ false };    // picks the tint color
        bool        exactTint{ false }; // F-7 v3: write the tint color as-is
    };

    // The image repoint's save/restore record, one per repointed geometry.
    // Same SPEC §4 invariant as TintBackup and for the same reason: the clone
    // SHARES its shader property and material with the cached template, so an
    // un-restored write would ride every later demand of the mesh. The backup
    // holds a strong ref on the original texture - overwriting the material's
    // NiPointer drops its ref, and the original must still be alive when the
    // restore puts it back.
    struct ImageBackup {
        RE::NiPointer<RE::BSGeometry>      geom;
        RE::NiPointer<RE::NiSourceTexture> tex;
        RE::BSFixedString                  path;
        float                              baseColorScale{ 1.0f };
    };

    // The last shader values this piece was logged writing. Bubble hunt
    // 2026-08-27: the arm line already says the dome came up with its image,
    // and the field round proved it did on the arm that then went strange, so
    // the fault is in what gets pushed at the pieces AFTER they are up. None
    // of the three writers logged a value, which left "the backdrop went grey"
    // with no reading behind it. Sentinel below any real value so the first
    // push after an arm always prints one.
    struct ShaderValues {
        float r{ -1.0f }, g{ -1.0f }, b{ -1.0f };
        float brightness{ -1.0f };
        float imageBrightness{ -1.0f };
        float fade{ -1.0f };
    };

    struct Piece {
        PieceDef                      def;
        RE::NiPointer<RE::NiAVObject> node;
        std::vector<TintBackup>       tints;
        std::vector<ImageBackup>      images;
        float                         unitRadius{ 0.0f };  // bound radius at scale 1
        ShaderValues                  logged;
    };
    std::vector<Piece> g_active;
    std::string   g_appliedStage;
    std::string   g_appliedBackground;
    int           g_appliedMode = 0;
    bool          g_up = false;
    bool          g_attemptedThisArm = false;  // no per-tick retry storm on bad paths
    bool          g_settleLoggedThisArm = false;
    // Budget for the value trace above, spent per arm. Refresh runs every tick,
    // so an ungated line would be a flood and a per-arm-once line would miss
    // the change it exists to catch. On movement, capped, is the shape that
    // answers "what did the values do while it was up" in a readable number of
    // lines.
    unsigned      g_shaderValueLines = 0;
    unsigned      g_settleRefitTicksRemaining = 0;
    std::uint32_t g_fitRevision = 0;

    // Persistent gap occluder (RE Option P). Held resident across menus so its
    // un-cull is same-frame; node null = not built. See Backdrop.h.
    Piece g_occluder;

    float QpcMs(std::uint64_t a_from, std::uint64_t a_to) {
        static const double freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        return static_cast<float>(static_cast<double>(a_to - a_from) * 1000.0 / freq);
    }

    RE::NiColor ToNi(const RE::Color& a_c) {
        return { a_c.red / 255.0f, a_c.green / 255.0f, a_c.blue / 255.0f };
    }
    RE::NiColor Lerp(const RE::NiColor& a_from, const RE::NiColor& a_to, float a_t) {
        return { a_from.red + (a_to.red - a_from.red) * a_t,
                 a_from.green + (a_to.green - a_from.green) * a_t,
                 a_from.blue + (a_to.blue - a_from.blue) * a_t };
    }

    // The look's tint for a piece: the floor reads as a lit stage (DALC
    // fill), the dome/shell as the void itself.
    RE::NiColor PieceTint(const MTB::Settings::LookValues& a_look, bool a_isDome) {
        if (a_isDome) {
            // r33 standardization (user call: "always dark, vary it only
            // slightly"): the old 30% ambient lift let daytime looks pull
            // the void toward white (field: "the background was dependent
            // on time of day"), washing out the constellation. The vibe now
            // varies the HUE (small ambient mix) under a hard luminance
            // cap - the void can never be bright, in any look, in any cell.
            RE::NiColor c = Lerp(ToNi(a_look.fog), ToNi(a_look.ambient), 0.12f);
            const float cap = MTB::Settings::GetSingleton().voidBrightnessCap;
            if (const float maxc = (std::max)({ c.red, c.green, c.blue });
                maxc > cap && maxc > 0.0001f) {
                const float s = cap / maxc;
                c = { c.red * s, c.green * s, c.blue * s };
            }
            return c;
        }
        return ToNi(a_look.fill);
    }

    // Returns the colour it wrote, so the value trace can report what actually
    // landed rather than recomputing PieceTint and the picker override beside
    // it and drifting from them.
    RE::NiColor PushTint(Piece& a_piece, const MTB::Settings::LookValues& a_look,
                         float a_brightness) {
        // r47: the color picker drives the WHOLE void - the dome takes the
        // picked color through the hue-transfer (its texture keeps its
        // authored luminance), the shell takes it verbatim below. Off =
        // the vibe flow, unchanged.
        RE::NiColor tint = PieceTint(a_look, a_piece.def.isDome);
        {
            const auto& cfg = MTB::Settings::GetSingleton();
            if (cfg.voidColorOverride) {
                tint = RE::NiColor{ cfg.voidColor.red / 255.0f,
                                    cfg.voidColor.green / 255.0f,
                                    cfg.voidColor.blue / 255.0f };
            }
        }
        for (auto& backup : a_piece.tints) {
            auto* geom = backup.geom.get();
            if (!geom) {
                continue;
            }
            auto* prop = geom->GetGeometryRuntimeData()
                             .properties[RE::BSGeometry::States::kEffect]
                             .get();
            if (backup.isLighting) {
                auto* lighting = static_cast<RE::BSLightingShaderProperty*>(prop);
                if (lighting->emissiveColor) {
                    *lighting->emissiveColor = tint;
                }
                lighting->emissiveMult = a_brightness;
            } else if (auto* material = static_cast<RE::BSEffectShaderMaterial*>(
                           static_cast<RE::BSEffectShaderProperty*>(prop)->material)) {
                if (a_piece.def.exactTint) {
                    // F-7 v3: the SHELL is the void's skin - its color must
                    // BE the look's color, identical in every cell. The
                    // hue-transfer below exists to keep additive star
                    // textures visible; on a flat opaque shell it would
                    // re-luminance the void to whatever the mesh author
                    // picked (the vampire dome's per-cell color shift, in
                    // part). Exact write, exact standardization. (The r45
                    // picker override is applied to `tint` above - for the
                    // whole void, not just the shell.)
                    material->baseColor = RE::NiColorA{ tint.red, tint.green, tint.blue,
                                                        backup.baseColor.alpha };
                    material->baseColorScale = a_brightness;
                } else {
                    // Effect shaders (the star dome) are usually additive: a
                    // dark fog-based tint would black the stars out. Transfer
                    // the look's HUE but keep the material's authored
                    // luminance (max-channel match).
                    const float origMax = (std::max)({ backup.baseColor.red,
                                                       backup.baseColor.green,
                                                       backup.baseColor.blue });
                    const float tintMax = (std::max)({ tint.red, tint.green, tint.blue });
                    const float gain = tintMax > 0.001f ? origMax / tintMax : 0.0f;
                    material->baseColor = RE::NiColorA{ tint.red * gain, tint.green * gain,
                                                        tint.blue * gain,
                                                        backup.baseColor.alpha };
                    material->baseColorScale = backup.baseColorScale * a_brightness;
                }
            }
        }
        return tint;
    }

    // The value trace itself. Prints only when something MOVED, so a healthy
    // arm costs one line per piece and a backdrop that goes grey while it is up
    // prints the moment it does, with the reading that says which of the three
    // writers moved it.
    //
    // ⚠ THE TRANSITION RAMP IS NOT A FINDING. t runs 0 to 1 at every arm and
    // multiplies both brightnesses, so logging during the ramp would print the
    // same expected climb every time and bury the one line that matters. The
    // trace waits for t to settle.
    // ⚠⚠ a_tinted IS NOT OPTIONAL AND ITS ABSENCE WAS THE TRACE'S FIRST BUG.
    // The ARM site calls PushTint on every piece; Refresh and PushFade call it
    // only when def.tint. The first cut of this reported the skipped case as a
    // real colour, so the dome printed a healthy tint at the arm and then
    // "(MOVED) tint (0.000,0.000,0.000) brightness 0.000" in the SAME
    // millisecond, on every arm, which reads exactly like the backdrop going
    // black and is nothing of the kind (field 2026-08-27, 04:45:51 and twice
    // more). An untinted piece now says so and its colour fields do not take
    // part in the movement test at all.
    //
    // ⚠ THE DOME IS THE UNTINTED ONE HERE, and its arm line says why:
    // '0 tintable geom(s)'. So PushTint has nothing to write on it either way
    // and the two call sites disagreeing is harmless today. It is still a real
    // inconsistency and worth closing separately.
    void LogShaderValues(Piece& a_piece, const RE::NiColor& a_tint, bool a_tinted,
                         float a_brightness, float a_imageBrightness, float a_fade) {
        constexpr float kEps      = 0.01f;
        constexpr unsigned kBudget = 40;
        if (a_fade < 0.999f) {
            return;  // still ramping in or dissolving out; expected motion
        }
        const auto moved = [](float a_was, float a_now) {
            return a_was < 0.0f || std::fabs(a_was - a_now) > kEps;
        };
        auto& was = a_piece.logged;
        const bool colourMoved = a_tinted && (moved(was.r, a_tint.red) ||
                                              moved(was.g, a_tint.green) ||
                                              moved(was.b, a_tint.blue) ||
                                              moved(was.brightness, a_brightness));
        if (!colourMoved && !moved(was.imageBrightness, a_imageBrightness) &&
            !moved(was.fade, a_fade)) {
            return;
        }
        const bool first = was.imageBrightness < 0.0f;
        if (a_tinted) {  // an untinted pass leaves the colour record alone
            was.r          = a_tint.red;
            was.g          = a_tint.green;
            was.b          = a_tint.blue;
            was.brightness = a_brightness;
        }
        was.imageBrightness = a_imageBrightness;
        was.fade            = a_fade;
        if (g_shaderValueLines >= kBudget) {
            return;  // the budget is spent; the lines already printed carry the shape
        }
        ++g_shaderValueLines;
        if (a_tinted) {
            spdlog::info("backdrop values: '{}' tint ({:.3f},{:.3f},{:.3f}) brightness {:.3f} "
                         "image brightness {:.3f} fade {:.3f}{}{}",
                         a_piece.def.name, a_tint.red, a_tint.green, a_tint.blue, a_brightness,
                         a_imageBrightness, a_fade, first ? " (settled)" : " (MOVED)",
                         g_shaderValueLines == kBudget ? " [budget spent]" : "");
        } else {
            spdlog::info("backdrop values: '{}' untinted (def.tint off, nothing to write) "
                         "image brightness {:.3f} fade {:.3f}{}{}",
                         a_piece.def.name, a_imageBrightness, a_fade,
                         first ? " (settled)" : " (MOVED)",
                         g_shaderValueLines == kBudget ? " [budget spent]" : "");
        }
    }

    // F-12: the whole-piece dissolve rides the clone's own fade node -
    // the engine's native object-fade path (dithered, works on opaque
    // geometry). currentFade is NODE-side state on OUR deep-cloned root:
    // no shared-template surface, nothing to restore, dies with the
    // piece. Meshes whose root is not a BSFadeNode fall back to the tint
    // ramp alone (additive domes dissolve fully through baseColorScale;
    // a lit floor would pop its albedo but bloom its glow).
    void PushPieceFade(Piece& a_piece, float a_t) {
        if (auto* node = a_piece.node.get()) {
            if (auto* fade = node->AsFadeNode()) {
                fade->GetRuntimeData().currentFade = a_t;
            }
        }
    }

    void RestoreTints(Piece& a_piece) {
        for (auto& backup : a_piece.tints) {
            auto* geom = backup.geom.get();
            if (!geom) {
                continue;
            }
            auto* prop = geom->GetGeometryRuntimeData()
                             .properties[RE::BSGeometry::States::kEffect]
                             .get();
            if (!prop) {
                continue;
            }
            if (backup.isLighting) {
                auto* lighting = static_cast<RE::BSLightingShaderProperty*>(prop);
                if (!backup.hadOwnEmit) {
                    lighting->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kOwnEmit,
                                       false);
                }
                if (backup.hadEmissivePtr && lighting->emissiveColor) {
                    *lighting->emissiveColor = backup.emissive;
                }
                lighting->emissiveMult = backup.emissiveMult;
            } else if (auto* material = static_cast<RE::BSEffectShaderMaterial*>(
                           static_cast<RE::BSEffectShaderProperty*>(prop)->material)) {
                material->baseColor = backup.baseColor;
                material->baseColorScale = backup.baseColorScale;
            }
        }
        a_piece.tints.clear();
    }

    // Collect tintable geometry + save originals; enable own-emit on
    // lighting shaders so the emissive channel actually renders.
    void CollectTints(Piece& a_piece) {
        int lightingCount = 0, effectCount = 0, otherCount = 0;
        RE::BSVisit::TraverseScenegraphGeometries(
            a_piece.node.get(), [&](RE::BSGeometry* a_geom) {
                auto* prop = a_geom->GetGeometryRuntimeData()
                                 .properties[RE::BSGeometry::States::kEffect]
                                 .get();
                if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(prop)) {
                    TintBackup backup;
                    backup.geom = RE::NiPointer<RE::BSGeometry>{ a_geom };
                    backup.isLighting = true;
                    backup.hadOwnEmit = lighting->flags.any(
                        RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
                    backup.hadEmissivePtr = lighting->emissiveColor != nullptr;
                    if (lighting->emissiveColor) {
                        backup.emissive = *lighting->emissiveColor;
                    }
                    backup.emissiveMult = lighting->emissiveMult;
                    if (!backup.hadOwnEmit) {
                        lighting->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kOwnEmit,
                                           true);
                    }
                    a_piece.tints.push_back(std::move(backup));
                    ++lightingCount;
                } else if (auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(prop)) {
                    if (auto* material =
                            static_cast<RE::BSEffectShaderMaterial*>(effect->material)) {
                        TintBackup backup;
                        backup.geom = RE::NiPointer<RE::BSGeometry>{ a_geom };
                        backup.isLighting = false;
                        backup.baseColor = material->baseColor;
                        backup.baseColorScale = material->baseColorScale;
                        // r55: r51 cleared the dome's Z-WRITE to stop sky
                        // punching through its dark gaps - but that also
                        // stopped the dome occluding the SHELL, so when the
                        // shell sorted after the dome it painted over it
                        // (field r54: "constellation not visible at all";
                        // earlier "size nudge makes it appear" = the
                        // unstable sort). Reverted: the dome keeps its
                        // native Z-write (the r50 checkpoint look - visible
                        // constellation). The proper both-ways fix is an
                        // OPAQUE shell (own mesh) so the dome never fights
                        // it in the transparent pass - queued.
                        a_piece.tints.push_back(std::move(backup));
                        ++effectCount;
                    }
                } else {
                    ++otherCount;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        spdlog::debug("backdrop: '{}' shaders: {} lighting, {} effect, {} other.",
                      a_piece.def.name, lightingCount, effectCount, otherCount);
    }

    // The image-pack repoint: point the image sphere's effect material at the
    // pack's DDS. Runs on the CLONE at Apply, but the material is SHARED with
    // the cached template (the TintBackup lesson), so this is a save/restore
    // write, not a material swap - FR's SetMaterial idiom would run the
    // shared material through the cache's release and contaminate the
    // template for every later arm. The piece is untinted (r58), so nothing
    // else writes these fields between Apply and Remove.
    void ApplyImage(Piece& a_piece) {
        const std::string rooted = MTB::BackdropImagePolicy::Rooted(a_piece.def.image);
        LARGE_INTEGER t0{}, t1{};
        ::QueryPerformanceCounter(&t0);
        auto tex = MTB::BackdropImage::Load(rooted);
        ::QueryPerformanceCounter(&t1);
        if (!tex) {
            spdlog::warn("backdrop: image '{}' did not load. '{}' keeps the shipped "
                         "texture.", rooted, a_piece.def.name);
            return;
        }
        int repointed = 0;
        RE::BSVisit::TraverseScenegraphGeometries(
            a_piece.node.get(), [&](RE::BSGeometry* a_geom) {
                auto* prop = a_geom->GetGeometryRuntimeData()
                                 .properties[RE::BSGeometry::States::kEffect]
                                 .get();
                auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(prop);
                auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(
                                              effect->material)
                                        : nullptr;
                if (!material) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                ImageBackup backup;
                backup.geom = RE::NiPointer<RE::BSGeometry>{ a_geom };
                backup.tex = material->sourceTexture;  // strong ref, outlives the write
                backup.path = material->sourceTexturePath;
                backup.baseColorScale = material->baseColorScale;
                a_piece.images.push_back(std::move(backup));
                material->sourceTexture = tex;
                material->sourceTexturePath = rooted.c_str();
                // The pass list caches on the (shared) property; stale passes
                // from an earlier arm would keep sampling the old texture.
                effect->DoClearRenderPasses();
                ++repointed;
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        // ⚠ A missing file is NOT a null: the engine substitutes a live
        // placeholder texture. The loose-file check below is the only tell,
        // and it is blind to archives - a BSA-shipped pack logs "not loose"
        // and still renders fine, so the note stays a hint, not a verdict.
        std::error_code ec;
        const bool onDisk = std::filesystem::exists(rooted, ec);
        spdlog::info("backdrop: '{}' image -> '{}' (load {:.1f} ms, {} geom(s){}).",
                     a_piece.def.name, rooted, QpcMs(t0.QuadPart, t1.QuadPart), repointed,
                     onDisk ? "" : "; not a loose file: if no archive carries it, "
                                   "the engine shows a flat placeholder");
        if (repointed == 0) {
            spdlog::warn("backdrop: image '{}' loaded but '{}' has no effect-shader "
                         "geometry to repoint. Is the mesh voidimage.nif?",
                         rooted, a_piece.def.name);
        }

        // Reflections: hand CS the pack's pre-baked cubemap sibling so armor
        // reflects the image instead of the screen-space capture's stale
        // history. Loose-file check is honest here - CS reads the file from
        // disk itself, so an archived cubemap genuinely does not work.
        const std::string cube = MTB::BackdropImagePolicy::CubeSibling(rooted);
        std::error_code cubeEc;
        if (!cube.empty() && std::filesystem::exists(cube, cubeEc)) {
            if (MTB::CsCubemapBridge::Push(cube)) {
                spdlog::info("backdrop: reflections follow the image ('{}').", cube);
            }
        } else if (MTB::CsCubemapBridge::Available()) {
            spdlog::info("backdrop: no cubemap sibling ('{}'). Reflections keep CS's "
                         "capture. Re-run the pack pipeline to bake one.", cube);
        }

        // Atmosphere: CS's exponential height fog integrates over the
        // sphere's ~800 units and reads as a grey veil on the picture
        // (field: "washed out", brightness could not cancel it). The image
        // replaces the distant world, so the studio range is excluded from
        // the fog while it is up.
        if (MTB::CsCubemapBridge::SetFogFloor(100000.0f)) {
            spdlog::info("backdrop: height fog excluded over the studio while the "
                         "image is up.");
        }
    }

    void RestoreImage(Piece& a_piece) {
        for (auto& backup : a_piece.images) {
            auto* geom = backup.geom.get();
            if (!geom) {
                continue;
            }
            auto* prop = geom->GetGeometryRuntimeData()
                             .properties[RE::BSGeometry::States::kEffect]
                             .get();
            auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(prop);
            auto* material =
                effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->material) : nullptr;
            if (!material) {
                continue;
            }
            material->sourceTexture = backup.tex;
            material->sourceTexturePath = backup.path;
            material->baseColorScale = backup.baseColorScale;
            effect->DoClearRenderPasses();
        }
        if (!a_piece.images.empty()) {
            MTB::CsCubemapBridge::Clear();         // reflections return to CS's capture
            MTB::CsCubemapBridge::ClearFogFloor(); // atmosphere returns with the world
        }
        a_piece.images.clear();
    }

    // The image sphere's own brightness, dialed live from the panel (field
    // 2026-08-20: an as-authored daylight image blows out white over the dark
    // studio - the nebula the void was tuned for sits at 0.02-0.16 luminance
    // and ENB effect gain rides on top). Value writes on the shared material
    // are live per pass, the same way PushTint's are; RestoreImage puts the
    // authored scale back.
    void PushImageBrightness(Piece& a_piece, float a_brightness) {
        for (auto& backup : a_piece.images) {
            auto* geom = backup.geom.get();
            if (!geom) {
                continue;
            }
            auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(
                geom->GetGeometryRuntimeData()
                    .properties[RE::BSGeometry::States::kEffect]
                    .get());
            auto* material =
                effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->material) : nullptr;
            if (material) {
                material->baseColorScale = a_brightness;
            }
        }
    }

    // r57 custom-image composition: the world yaw (degrees) that points a
    // background piece's front at the camera, so a hand-made texture on the
    // shell frames the same way no matter which direction the player faced at
    // open - and it tracks a camera orbit. The camera's own yaw (radians)
    // shares the EulerAnglesToAxesZXY sense the fit/spin use, so a sphere set
    // to +camYaw keeps a fixed texture column centred on the view (1:1
    // tracking); the user offset picks which column. 0 (world-locked, the
    // default look) when the option is off or the piece is not a background.
    float BackgroundYawDeg(const MTB::Settings& a_cfg, const Piece& a_piece) {
        if (!a_cfg.backgroundFaceCamera || !a_piece.def.isDome) {
            return 0.0f;
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        const float camYaw = camera ? camera->yaw : 0.0f;
        return camYaw * 57.2957795f + a_cfg.backgroundYawOffset;
    }

    // Parent-local placement at the player (rig math: never assume the
    // cell root carries an identity transform). Stage pieces sit on world
    // axes around the player's feet, optionally yawed.
    void FitTransform(Piece& a_piece, RE::NiNode* a_parent,
                      const RE::NiPoint3& a_playerPos) {
        const auto& def = a_piece.def;
        const float fit = def.fitRadius > 0.0f
                              ? def.fitRadius / (std::max)(a_piece.unitRadius, 1.0f)
                              : def.scale;

        const auto& pw = a_parent->world;
        const auto pwRotInv = pw.rotate.Transpose();
        const float invScale = pw.scale != 0.0f ? 1.0f / pw.scale : 1.0f;
        const RE::NiPoint3 world{ a_playerPos.x + def.x, a_playerPos.y + def.y,
                                  a_playerPos.z + def.z };

        RE::NiMatrix3 yaw;  // identity unless the piece is rotated
        if (def.yawDeg != 0.0f) {
            yaw.EulerAnglesToAxesZXY(0.0f, 0.0f,
                                     def.yawDeg * 0.017453293f);
        }
        auto* node = a_piece.node.get();
        node->local.rotate = pwRotInv * yaw;  // world-space yaw
        node->local.translate = (pwRotInv * (world - pw.translate)) * invScale;
        node->local.scale = fit * invScale;

        RE::NiUpdateData ctx;
        ctx.time = 0.0f;
        node->Update(ctx);
    }

    // The pieces the current settings + view mode ask for: the BACKGROUND
    // dome surrounds the void (mode 2) and the dressing room (mode 3);
    // the STAGE floor + extras build in the dressing room only (r18
    // split - mix and match).
    std::vector<PieceDef> BuildDefs(const MTB::Settings& a_cfg) {
        std::vector<PieceDef> defs;
        // F-7 shell: the star dome is ADDITIVE - its dark areas are fully
        // transparent, so ENB interior mist / leftover FX behind it read
        // as the background (r28 field: white void in Dragonsreach with
        // everything else proven healthy). An OPAQUE sphere just outside
        // the dome makes the void's skin ours: the engine's own
        // loading-screen sphere, fog-tinted per the vibe. Present in both
        // studio modes, with or without a star dome.
        if (!a_cfg.backdropShellMesh.empty() && a_cfg.IsVoidFamily()) {
            PieceDef shell;
            shell.name = "MTB_BackdropShell";
            shell.mesh = a_cfg.backdropShellMesh;
            shell.fitRadius = a_cfg.backdropDomeRadius * 1.08f;
            shell.z = a_cfg.backdropDomeZ;
            shell.tint = true;
            shell.isDome = true;    // fog-leaning tint - it IS the void color
            shell.exactTint = true; // F-7 v3: one solid color, every cell
            shell.dialable = true;  // r54: track the dome on size/height (stays 1.08x)
            defs.push_back(std::move(shell));
        }
        if (!a_cfg.backdropDomeMesh.empty() && a_cfg.IsVoidFamily()) {
            PieceDef dome;
            dome.name = "MTB_BackdropDome";
            dome.mesh = a_cfg.backdropDomeMesh;
            dome.fitRadius = a_cfg.backdropDomeRadius;
            dome.z = a_cfg.backdropDomeZ;
            // r58: the custom-image sphere (voidimage.nif) shows AS AUTHORED -
            // the dark void tint would black it out; vanilla domes tint as before.
            const bool imageSphere =
                a_cfg.backdropDomeMesh.find("voidimage") != std::string::npos;
            dome.tint = !imageSphere;
            // Image packs: the pack's DDS replaces the sphere's shipped texture
            // at Apply. Keyed on the same mesh test as the tint carve-out so a
            // stale image path can never touch a vanilla dome.
            if (imageSphere) {
                dome.image = a_cfg.backdropBackgroundImage;
            }
            // r59: the flat void-colour sphere (blank) takes the void colour
            // EXACTLY (like the shell), not the capped fog/ambient hue-transfer.
            dome.exactTint = a_cfg.backdropDomeMesh.find("voidcolor") != std::string::npos;
            dome.dialable = true;
            dome.isDome = true;
            defs.push_back(std::move(dome));
            // r46's mirrored lower dome REVERTED in r47: the perk dome's
            // authored horizon skirt hung a mountain-profile seam across
            // the view, and a third same-centered transparent sphere made
            // the alpha sort order flip between arms (field: void color
            // lost on re-entry until a size-change refit). The aperture is
            // fixed by SINKING the dome (preset z, r48) instead.
            //
            // r49 (field: "i don't see the stars"): the vanilla skills
            // menu draws its stars with a SEPARATE mesh - the StatsMenu
            // ctor loads INTPerkStars01 right next to the skydome
            // (mtb_statsmenu2.c). Additive stars are draw-order-immune
            // against the additive dome, and the piece sits at a
            // DIFFERENT center (z −300 vs the shell's 0) so the depth
            // sort against the opaque-blend shell stays deterministic -
            // the r46 lesson.
            if (!a_cfg.backdropStarsMesh.empty()) {
                PieceDef stars;
                stars.name = "MTB_BackdropStars";
                stars.mesh = a_cfg.backdropStarsMesh;
                stars.fitRadius = a_cfg.backdropDomeRadius * 0.9f;
                stars.z = -300.0f;
                stars.tint = true;
                stars.dialable = true;
                stars.isDome = true;  // hue-transfer keeps the star points lit
                defs.push_back(std::move(stars));
            }
        }
        if (a_cfg.declutterMode == 3) {
            if (!a_cfg.backdropFloorMesh.empty()) {
                PieceDef floor;
                floor.name = "MTB_BackdropFloor";
                floor.mesh = a_cfg.backdropFloorMesh;
                floor.fitRadius = a_cfg.backdropFloorRadius;
                floor.z = a_cfg.backdropFloorZ;
                floor.tint = true;
                floor.dialable = true;
                floor.isDome = false;
                defs.push_back(std::move(floor));
            }
            int index = 0;
            for (const auto& extra : a_cfg.ActiveStageExtras()) {
                PieceDef def;
                def.name = "MTB_StagePiece" + std::to_string(index++);
                def.mesh = extra.mesh;
                def.fitRadius = extra.fitRadius;
                def.scale = extra.scale;
                def.x = extra.x;
                def.y = extra.y;
                def.z = extra.z;
                def.yawDeg = extra.yawDeg;
                def.tint = extra.tint;
                defs.push_back(std::move(def));
            }
        }
        return defs;
    }
}

namespace MTB::Backdrop {
    void Apply() {
        const auto& cfg = Settings::GetSingleton();
        if (g_up || !cfg.IsVoidFamily()) {
            return;
        }
        g_attemptedThisArm = true;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell) {
            return;
        }
        // F-7 phase 1 (r25): exteriors get the dome + stage too. The
        // weather sky is parked by StudioLight while armed, so the
        // constellation reads the same out here as in a closed interior;
        // terrain stays by design (it grounds the character - the far
        // world is the field experiment this round).
        auto* playerRoot = player->Get3D(false);
        auto* parent = playerRoot ? playerRoot->parent : nullptr;
        if (!parent) {
            spdlog::warn("backdrop: no scene parent, skipped.");
            return;
        }

        const auto playerPos = player->GetPosition();
        const auto look = cfg.CurrentLook();
        auto defs = BuildDefs(cfg);
        g_active.clear();
        g_active.reserve(defs.size());
        g_settleLoggedThisArm = false;
        g_settleRefitTicksRemaining =
            MTB::BackdropAnchorPolicy::kSettledRefitFrames;
        int up = 0;

        for (auto& def : defs) {
            RE::NiPointer<RE::NiNode> model;
            const ModelDBArgs args{};
            LARGE_INTEGER t0{}, t1{};
            ::QueryPerformanceCounter(&t0);
            const auto code = g_demandModel(def.mesh.c_str(), model, args);
            ::QueryPerformanceCounter(&t1);
            if (code != 0 || !model) {
                spdlog::warn("backdrop: demand failed for '{}' (error {}). Piece skipped. "
                             "Check the stage's mesh path.", def.mesh, code);
                continue;
            }

            auto* clone = g_niClone(model.get());
            if (!clone) {
                spdlog::warn("backdrop: clone failed for '{}'. Piece skipped.", def.mesh);
                continue;
            }
            auto& piece = g_active.emplace_back();
            piece.def = std::move(def);
            piece.node = RE::NiPointer<RE::NiAVObject>{ clone };
            clone->name = piece.def.name.c_str();

            // Fit basis: the template ships its bound; fall back to one
            // update pass, then to a safe guess (loudly).
            float radius = clone->worldBound.radius;
            if (radius < 1.0f) {
                RE::NiUpdateData ctx;
                ctx.time = 0.0f;
                clone->Update(ctx);
                radius = clone->worldBound.radius;
            }
            if (radius < 1.0f) {
                spdlog::warn("backdrop: '{}' has no usable bound, assuming radius 512.",
                             piece.def.mesh);
                radius = 512.0f;
            }
            piece.unitRadius = radius;

            parent->AttachChild(clone, true);
            piece.def.yawDeg = BackgroundYawDeg(cfg, piece);  // r57: face-camera at arm
            FitTransform(piece, parent, playerPos);
            if (piece.def.tint) {
                CollectTints(piece);
            }
            if (!piece.def.image.empty()) {
                ApplyImage(piece);
            }
            if (!clone->AsFadeNode()) {
                spdlog::debug("backdrop: '{}' root is not a fade node. The piece "
                              "pops in (its tint still ramps).", piece.def.name);
            }
            ++up;

            // The bound-center offset tells the next round how the mesh is
            // authored around its origin - the datum for dialing offsets
            // from evidence.
            const auto centerOff = clone->worldBound.center - clone->world.translate;
            spdlog::info("backdrop: '{}' up: mesh '{}' (demand {:.1f} ms), unit r={:.0f}, "
                         "scale {:.3f}, world ({:.0f},{:.0f},{:.0f}), bound r={:.0f} "
                         "center off ({:.0f},{:.0f},{:.0f}), {} tintable geom(s).",
                         piece.def.name, piece.def.mesh, QpcMs(t0.QuadPart, t1.QuadPart),
                         piece.unitRadius, clone->local.scale, clone->world.translate.x,
                         clone->world.translate.y, clone->world.translate.z,
                         clone->worldBound.radius, centerOff.x, centerOff.y, centerOff.z,
                         piece.tints.size());

            // r42 shell-invisibility forensics (field: the voidshell has
            // never provably produced a pixel - NifSkope renders it, the
            // engine doesn't). Log what the LOADED clone actually is: the
            // legacy stream-83 NiTriShape either survived conversion into
            // a renderable BSTriShape with its effect shader and flags, or
            // this line names what the loader dropped.
            {
                int geoms = 0;
                RE::BSVisit::TraverseScenegraphGeometries(
                    clone, [&](RE::BSGeometry* a_geom) {
                        ++geoms;
                        const char* rtti = a_geom->GetRTTI() && a_geom->GetRTTI()->name
                                               ? a_geom->GetRTTI()->name : "?";
                        auto& rt = a_geom->GetGeometryRuntimeData();
                        auto* effectProp = rt.properties[RE::BSGeometry::States::kEffect].get();
                        auto* shaderProp = rt.properties[RE::BSGeometry::States::kProperty].get();
                        const char* effectRtti =
                            effectProp && effectProp->GetRTTI() && effectProp->GetRTTI()->name
                                ? effectProp->GetRTTI()->name : "NONE";
                        const char* propRtti =
                            shaderProp && shaderProp->GetRTTI() && shaderProp->GetRTTI()->name
                                ? shaderProp->GetRTTI()->name : "NONE";
                        spdlog::info(
                            "backdrop diag: '{}' geom {} type={} effect={} prop={} "
                            "culled={} boundR={:.0f} skinned={}",
                            piece.def.name, geoms, rtti, effectRtti, propRtti,
                            a_geom->GetAppCulled(), a_geom->worldBound.radius,
                            rt.skinInstance ? 1 : 0);
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
                if (geoms == 0) {
                    spdlog::warn("backdrop diag: '{}' has ZERO BSGeometry after load. "
                                 "The legacy NiTriShape did NOT convert; the piece "
                                 "cannot render.", piece.def.name);
                }
            }
        }

        g_up = up > 0;
        g_appliedStage = cfg.backdropStage;
        g_appliedBackground = cfg.backdropBackground;
        g_appliedMode = cfg.declutterMode;
        g_fitRevision = cfg.revision;
        if (g_up) {
            // Pieces are born at the transition's current value - a fresh
            // arm blooms them in from 0; a mid-menu rebuild lands at 1.
            const float t = Transition::Value();
            const float brightness = cfg.backdropBrightness * t;
            g_shaderValueLines = 0;  // a fresh arm gets a fresh budget
            for (auto& piece : g_active) {
                PushPieceFade(piece, t);
                const RE::NiColor pushed = PushTint(piece, look, brightness);
                PushImageBrightness(piece, cfg.backdropImageBrightness * t);
                LogShaderValues(piece, pushed, true, brightness,
                                cfg.backdropImageBrightness * t, t);
            }
            spdlog::info("backdrop: background '{}' + stage '{}' up: {} piece(s) (mode {}).",
                         g_appliedBackground, cfg.declutterMode == 3 ? g_appliedStage.c_str()
                                                                     : "(none)",
                         up, g_appliedMode);
        }
    }

    // Preheat: demand the configured meshes into BSModelDB's cache so the
    // first Apply of this arm finds them warm. The cold first-open demand was
    // ~13 ms in the field (the log's first-arm 'demand 12.8 ms' vs 0.3 ms on
    // the second) - long enough that the shell reached the screen a frame or
    // two AFTER the world was culled, which is the "blue void" flash. A cached
    // demand costs a fraction of a ms; a config-signature guard skips the work
    // entirely once the current background/stage/mode is already warm.
    void Warm() {
        const auto& cfg = Settings::GetSingleton();
        if (!cfg.IsVoidFamily()) {
            return;
        }
        static std::string s_warmed;
        const std::string sig = cfg.backdropShellMesh + "|" + cfg.backdropDomeMesh + "|" +
                                std::to_string(cfg.declutterMode);
        if (sig == s_warmed) {
            return;
        }
        int n = 0;
        for (const auto& def : BuildDefs(cfg)) {
            RE::NiPointer<RE::NiNode> model;
            const ModelDBArgs       args{};
            if (g_demandModel(def.mesh.c_str(), model, args) == 0 && model) {
                ++n;
            }
        }
        s_warmed = sig;
        spdlog::debug("backdrop: warmed {} mesh(es) into the model cache.", n);
    }

    bool OccluderShow() {
        const auto& cfg = Settings::GetSingleton();
        if (!cfg.IsVoidFamily() || cfg.backdropShellMesh.empty()) {
            return false;
        }
        // The occluder is a plain, untinted copy of the shell, fitted just
        // OUTSIDE it (dome*1.10 vs the shell's *1.08) so the real tinted shell
        // always renders in front once it draws; this only exists to hold the
        // black backing on the one frame before that.
        const auto occluderDef = [&cfg] {
            PieceDef d;
            d.name      = "MTB_GapOccluder";
            d.mesh      = cfg.backdropShellMesh;
            d.fitRadius = cfg.backdropDomeRadius * 1.10f;
            d.z         = cfg.backdropDomeZ;
            d.tint      = false;
            return d;
        };

        auto* player     = RE::PlayerCharacter::GetSingleton();
        auto* playerRoot = player ? player->Get3D(false) : nullptr;
        auto* parent     = playerRoot ? playerRoot->parent : nullptr;
        if (!parent) {
            return false;
        }
        // Resident iff the node is alive AND still under the CURRENT scene parent
        // (a cell change / 3D rebuild nulls or changes its parent). Comparing
        // pointers never dereferences the old parent, so this is safe even if that
        // parent was destroyed.
        const bool resident = g_occluder.node && g_occluder.node->parent == parent;
        if (!resident) {
            // Release any orphan WITHOUT touching its (possibly dead) old parent,
            // then build cold. Cold = a one-frame publish latency THIS open, so the
            // caller keeps the no-fader interim (defer the cull) - never blue. It is
            // warm from the next open on.
            g_occluder.node.reset();
            RE::NiPointer<RE::NiNode> model;
            const ModelDBArgs       args{};
            if (g_demandModel(cfg.backdropShellMesh.c_str(), model, args) != 0 || !model) {
                return false;
            }
            auto* clone = g_niClone(model.get());
            if (!clone) {
                return false;
            }
            g_occluder      = Piece{};
            g_occluder.def  = occluderDef();
            g_occluder.node = RE::NiPointer<RE::NiAVObject>{ clone };
            clone->name     = g_occluder.def.name.c_str();
            float radius = clone->worldBound.radius;
            if (radius < 1.0f) {
                RE::NiUpdateData u;
                u.time = 0.0f;
                clone->Update(u);
                radius = clone->worldBound.radius;
            }
            g_occluder.unitRadius = radius > 1.0f ? radius : 512.0f;
            parent->AttachChild(clone, true);
            spdlog::debug("occluder: (re)built cold, warms for the next open.");
        }
        // Reposition to the player, then un-cull. FitTransform + Update makes the
        // transform current the SAME frame; SetAppCulled(false) is consulted live
        // by the render's cull walk that same frame (resident node = no publish lag).
        g_occluder.def = occluderDef();  // radius/z may have moved via the panel
        FitTransform(g_occluder, parent, player->GetPosition());
        g_occluder.node->SetAppCulled(false);
        return resident;
    }

    void OccluderHide() {
        if (g_occluder.node) {
            g_occluder.node->SetAppCulled(true);
        }
    }

    void OccluderDrop() {
        if (g_occluder.node) {
            if (auto* p = g_occluder.node->parent) {
                p->DetachChild2(g_occluder.node.get());
            }
            g_occluder.node.reset();
        }
    }

    void Tick() {
        const auto& cfg = Settings::GetSingleton();
        // Live view-mode change from the panel mid-arm.
        if (g_up && !cfg.IsVoidFamily()) {
            Remove();
            OccluderHide();  // no void in this mode - hide the gap occluder too
            return;
        }
        if (!g_up) {
            if (cfg.IsVoidFamily() && !g_attemptedThisArm) {
                Apply();
            }
            if (!g_up) {
                return;
            }
        }

        // Live refit when the panel saves (size/height sliders) - and a
        // full rebuild when the background, stage or mode changed (they
        // decide WHICH pieces exist; presets switch live).
        const bool refit = cfg.revision != g_fitRevision;
        const bool modeChanged = g_appliedMode != cfg.declutterMode;
        if (refit || modeChanged) {
            g_fitRevision = cfg.revision;
            const bool setChanged =
                modeChanged || g_appliedStage != cfg.backdropStage ||
                g_appliedBackground != cfg.backdropBackground;
            if (setChanged) {
                spdlog::info("backdrop: set switched mid-menu, rebuilding "
                             "(bg '{}', stage '{}', mode {}).",
                             cfg.backdropBackground, cfg.backdropStage, cfg.declutterMode);
                Remove();
                Apply();
                return;
            }
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerRoot = player ? player->Get3D(false) : nullptr;
        auto* parent = playerRoot ? playerRoot->parent : nullptr;

        // ⚠⚠ A 3D REBUILD MOVES THE SCENE PARENT OUT FROM UNDER THE PIECES, AND
        // THAT WAS A SILENT SKIP. The refit below already compares
        // `node->parent == parent` and simply does nothing when it does not
        // match, so a backdrop left hanging under the old parent went on being
        // tinted and faded from here while it was no longer anywhere near the
        // character. Nothing said so, in any log, ever.
        //
        // OccluderShow asks this same question two hundred lines up and treats
        // the answer as a repair. The pieces had the read and not the repair.
        //
        // Field 2026-08-27: "menu studio bug still happens ... usually when I
        // change from male to female or vice versa from importing looks
        // presets", which is exactly when Get3D(false) is replaced.
        //
        // ⚠ THE REBUILD IS THE PATH A PRESET SWITCH ALREADY TAKES, not a new
        // one: Remove then Apply, the same pair `setChanged` uses above.
        if (parent) {
            std::size_t orphaned = 0;
            for (const auto& piece : g_active) {
                if (piece.node && piece.node->parent != parent) {
                    ++orphaned;
                }
            }
            if (orphaned != 0) {
                spdlog::info("backdrop: {} of {} piece(s) hang under a scene parent that is no "
                             "longer the player's, which is what a 3D rebuild leaves behind, so "
                             "the set goes up again.",
                             orphaned, g_active.size());
                Remove();
                Apply();
                return;
            }
        }

        const auto look = cfg.CurrentLook();
        const float t = Transition::Value();
        for (auto& piece : g_active) {
            auto* node = piece.node.get();
            if (!node) {
                continue;
            }
            // r57: the face-camera background re-yaws each frame to track the
            // view; only refits when the yaw actually moved, so a static
            // camera costs nothing. Non-background pieces get 0 (unchanged).
            const float wantYaw = BackgroundYawDeg(cfg, piece);
            const bool yawChanged = std::abs(wantYaw - piece.def.yawDeg) > 0.05f;
            const bool settleRefit =
                MTB::BackdropAnchorPolicy::ShouldRefitAfterArm(
                    piece.def.isDome, g_settleRefitTicksRemaining);
            if (piece.def.dialable && parent && node->parent == parent &&
                (refit || yawChanged || settleRefit)) {
                // Sliders edit the Settings fields - refresh our copy. The
                // shell (exactTint) tracks the dome at 1.08x; the dome and
                // floor take their own radius.
                if (refit) {
                    if (piece.def.exactTint && piece.def.isDome) {
                        piece.def.fitRadius = cfg.backdropDomeRadius * 1.08f;  // shell
                    } else {
                        piece.def.fitRadius = piece.def.isDome ? cfg.backdropDomeRadius
                                                               : cfg.backdropFloorRadius;
                    }
                    piece.def.z = piece.def.isDome ? cfg.backdropDomeZ : cfg.backdropFloorZ;
                }
                piece.def.yawDeg = wantYaw;
                FitTransform(piece, parent, player->GetPosition());
            }
            // Same defense as the rig: a stray cull flag (pre-exemption
            // sweep entry restored on close, or another mod) would blank
            // the stage silently.
            if (node->GetAppCulled()) {
                node->SetAppCulled(false);
            }
            PushPieceFade(piece, t);
            RE::NiColor pushed{};
            if (piece.def.tint) {
                pushed = PushTint(piece, look, cfg.backdropBrightness * t);
            }
            PushImageBrightness(piece, cfg.backdropImageBrightness * t);
            LogShaderValues(piece, pushed, piece.def.tint, cfg.backdropBrightness * t,
                            cfg.backdropImageBrightness * t, t);
        }
        if (!g_settleLoggedThisArm && player) {
            if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                if (auto* root = camera->cameraRoot.get()) {
                    const auto playerPos = player->GetPosition();
                    const auto rootPos = playerRoot ? playerRoot->world.translate : playerPos;
                    const auto delta = root->world.translate - playerPos;
                    spdlog::info(
                        "backdrop: spherical pieces settled around player ref "
                        "({:.0f},{:.0f},{:.0f}); 3D root ({:.0f},{:.0f},{:.0f}), "
                        "camera ({:.0f},{:.0f},{:.0f}), camera-player distance {:.0f}.",
                        playerPos.x, playerPos.y, playerPos.z,
                        rootPos.x, rootPos.y, rootPos.z,
                        root->world.translate.x, root->world.translate.y,
                        root->world.translate.z,
                        std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                  delta.z * delta.z));
                    g_settleLoggedThisArm = true;
                }
            }
        }
        if (g_settleRefitTicksRemaining > 0) {
            --g_settleRefitTicksRemaining;
        }
    }

    void PushFade() {
        // Grace-window refresh (F-12): fade values only - no refits, no
        // rebuild decisions; the pieces dissolve in place while the world
        // is already live again, then Remove() lands at ramp end.
        if (!g_up) {
            return;
        }
        const auto& cfg = Settings::GetSingleton();
        const auto look = cfg.CurrentLook();
        const float t = Transition::Value();
        for (auto& piece : g_active) {
            PushPieceFade(piece, t);
            RE::NiColor pushed{};
            if (piece.def.tint) {
                pushed = PushTint(piece, look, cfg.backdropBrightness * t);
            }
            PushImageBrightness(piece, cfg.backdropImageBrightness * t);
            LogShaderValues(piece, pushed, piece.def.tint, cfg.backdropBrightness * t,
                            cfg.backdropImageBrightness * t, t);
        }
    }

    void Remove() {
        g_attemptedThisArm = false;
        g_settleRefitTicksRemaining = 0;
        if (!g_up) {
            return;
        }
        int down = 0;
        for (auto& piece : g_active) {
            RestoreTints(piece);
            RestoreImage(piece);
            if (piece.node) {
                if (auto* parent = piece.node->parent) {
                    parent->DetachChild2(piece.node.get());
                }
                piece.node.reset();
                ++down;
            }
        }
        g_active.clear();
        g_appliedStage.clear();
        g_appliedBackground.clear();
        g_appliedMode = 0;
        g_up = false;
        spdlog::debug("backdrop: {} piece(s) removed, shader values restored.", down);
    }
}

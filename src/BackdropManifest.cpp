#include "BackdropManifest.h"

#include <SimpleIni.h>

#include <string>

namespace {
    float GetF(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        if (!raw || !*raw) {
            return a_def;
        }
        try {
            return std::stof(raw);
        } catch (...) {
            return a_def;
        }
    }

    bool GetB(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, bool a_def) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        if (!raw || !*raw) {
            return a_def;
        }
        switch (raw[0]) {
        case '1': case 't': case 'T': case 'y': case 'Y':
            return true;
        default:
            return false;
        }
    }

    std::string GetS(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        return raw ? std::string{ raw } : std::string{};
    }

    bool HasSection(const CSimpleIniA& a_ini, const char* a_sec) {
        return a_ini.GetSectionSize(a_sec) >= 0;  // -1 when the section is absent
    }

    bool EndsWithCi(std::string_view a_s, std::string_view a_suffix) {
        if (a_s.size() < a_suffix.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_suffix.size(); ++i) {
            const char a = a_s[a_s.size() - a_suffix.size() + i], b = a_suffix[i];
            const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
            const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
            if (la != lb) {
                return false;
            }
        }
        return true;
    }
}

namespace MTB {
    ParsedPack ParseBackdropManifest(std::string_view a_iniText, std::string_view a_packId) {
        ParsedPack pack;
        pack.id = std::string{ a_packId };

        CSimpleIniA ini;
        ini.SetUnicode();
        ini.SetMultiKey(false);
        if (ini.LoadData(a_iniText.data(), a_iniText.size()) < 0) {
            pack.warnings.emplace_back("manifest is not valid INI");
            return pack;
        }

        pack.name = GetS(ini, "Pack", "name");
        pack.author = GetS(ini, "Pack", "author");
        if (pack.name.empty()) {
            pack.warnings.emplace_back("missing [Pack] name");
            return pack;
        }

        pack.group = GetS(ini, "Pack", "group");

        // [Background], then [Background1], [Background2], ... - the same
        // numbered-section walk the pieces use, stopping at the first absent
        // section. A single-background pack is the plain [Background] and
        // nothing changed for it.
        const auto parseBackground = [&](const char* a_sec, int a_index) {
            const std::string image = GetS(ini, a_sec, "image");
            const std::string dome = GetS(ini, a_sec, "dome");
            if (image.empty() && dome.empty()) {
                pack.warnings.push_back(std::string{ "[" } + a_sec +
                                        "] has neither image nor dome; ignored");
                return;
            }
            ParsedBackground bg;
            bg.name = GetS(ini, a_sec, "name");
            if (bg.name.empty()) {
                bg.name = a_index == 0 ? pack.name
                                       : pack.name + " " + std::to_string(a_index);
            }
            if (!image.empty() && !dome.empty()) {
                pack.warnings.push_back(std::string{ "[" } + a_sec +
                                        "] has both image and dome; using image");
            }
            if (!image.empty()) {
                bg.image = image;
                bg.faceCamera = GetB(ini, a_sec, "faceCamera", true);
                // A wrong-format path fails INVISIBLY at load - the engine
                // substitutes a live placeholder texture, not a null - so
                // the parse is the one place the mistake can be named.
                if (!EndsWithCi(image, ".dds")) {
                    pack.warnings.push_back(std::string{ "[" } + a_sec +
                                            "] image does not end in .dds; the "
                                            "engine will show a flat placeholder");
                }
            } else {
                bg.dome = dome;
                bg.faceCamera = GetB(ini, a_sec, "faceCamera", false);
            }
            bg.thumb = GetS(ini, a_sec, "thumb");
            bg.radius = BackdropPolicy::ClampBackgroundRadius(
                GetF(ini, a_sec, "radius", BackdropPolicy::kBackgroundRadiusDefault));
            bg.z = GetF(ini, a_sec, "z", 0.0f);
            bg.yaw = GetF(ini, a_sec, "yaw", 0.0f);
            pack.backgrounds.push_back(std::move(bg));
        };
        if (HasSection(ini, "Background")) {
            parseBackground("Background", 0);
        }
        for (int i = 1;; ++i) {
            const std::string sec = "Background" + std::to_string(i);
            if (!HasSection(ini, sec.c_str())) {
                break;
            }
            parseBackground(sec.c_str(), i);
        }
        if (!pack.backgrounds.empty()) {
            pack.hasBackground = true;
            const auto& first = pack.backgrounds.front();
            pack.bgImage = first.image;
            pack.bgDome = first.dome;
            pack.bgRadius = first.radius;
            pack.bgZ = first.z;
            pack.bgFaceCamera = first.faceCamera;
            pack.bgYaw = first.yaw;
        }

        if (HasSection(ini, "Stage")) {
            const std::string floor = GetS(ini, "Stage", "floor");
            if (floor.empty()) {
                pack.warnings.emplace_back("[Stage] has no floor; ignored");
            } else {
                pack.hasStage = true;
                pack.floorMesh = floor;
                pack.floorRadius = GetF(ini, "Stage", "radius", 600.0f);
                pack.floorZ = GetF(ini, "Stage", "z", -10.0f);
                for (int i = 1;; ++i) {
                    const std::string sec = "Piece" + std::to_string(i);
                    if (!HasSection(ini, sec.c_str())) {
                        break;
                    }
                    const std::string mesh = GetS(ini, sec.c_str(), "mesh");
                    if (mesh.empty()) {
                        pack.warnings.push_back(sec + " has no mesh; skipped");
                        continue;
                    }
                    ParsedPiece pc;
                    pc.mesh = mesh;
                    pc.x = GetF(ini, sec.c_str(), "x", 0.0f);
                    pc.y = GetF(ini, sec.c_str(), "y", 0.0f);
                    pc.z = GetF(ini, sec.c_str(), "z", 0.0f);
                    pc.fitRadius = GetF(ini, sec.c_str(), "fit", 0.0f);
                    pc.scale = GetF(ini, sec.c_str(), "scale", 1.0f);
                    pc.yawDeg = GetF(ini, sec.c_str(), "yaw", 0.0f);
                    pc.tint = GetB(ini, sec.c_str(), "tint", false);
                    pack.extras.push_back(std::move(pc));
                }
            }
        }

        if (!pack.hasBackground && !pack.hasStage) {
            pack.warnings.emplace_back("pack defines neither a usable background nor stage");
            return pack;
        }
        pack.valid = true;
        return pack;
    }
}

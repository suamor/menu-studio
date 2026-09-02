#pragma once

#include "BackdropPolicy.h"

#include <string>
#include <string_view>
#include <vector>

namespace MTB {
    // One optional set piece parsed from a [PieceN] section.
    struct ParsedPiece {
        std::string mesh;
        float       x = 0.0f, y = 0.0f, z = 0.0f;
        float       fitRadius = 0.0f;  // >0: scale bound to this; else use scale
        float       scale = 1.0f;
        float       yawDeg = 0.0f;
        bool        tint = false;
    };

    // One background parsed from [Background] / [BackgroundN]. A pack used to
    // carry at most one; the load-order harvest writes dozens into a single
    // file, so the manifest grew the same numbered-section pattern the pieces
    // already use.
    struct ParsedBackground {
        std::string name;      // preset name (defaults derive from the pack name)
        std::string image;     // non-empty => image background (DDS path)
        std::string dome;      // else a dome mesh path
        std::string thumb;     // optional card picture (PNG path, textures-relative)
        float       radius = BackdropPolicy::kBackgroundRadiusDefault;
        float       z = 0.0f;
        bool        faceCamera = false;
        float       yaw = 0.0f;
    };

    // A parsed backdrop manifest. Plain types only (no engine headers) so it is
    // unit-testable from string fixtures. valid=false means "skip this pack";
    // warnings explains why / what was dropped.
    struct ParsedPack {
        std::string id;      // filename stem (caller supplies)
        std::string name;    // [Pack] name (required)
        std::string author;  // [Pack] author (optional)
        std::string group;   // [Pack] group (optional accordion label)

        bool                          hasBackground = false;  // backgrounds non-empty
        std::vector<ParsedBackground> backgrounds;
        // The FIRST background, mirrored - the fields every pre-multi caller
        // and test already reads. Kept in lockstep by the parser.
        std::string bgImage;
        std::string bgDome;
        float       bgRadius = BackdropPolicy::kBackgroundRadiusDefault;
        float       bgZ = 0.0f;
        bool        bgFaceCamera = false;
        float       bgYaw = 0.0f;

        bool        hasStage = false;
        std::string floorMesh;
        float       floorRadius = 600.0f;
        float       floorZ = -10.0f;
        std::vector<ParsedPiece> extras;

        bool                     valid = false;
        std::vector<std::string> warnings;
    };

    // Parse a manifest's TEXT (the whole .ini as a string). Pure, no file IO.
    ParsedPack ParseBackdropManifest(std::string_view a_iniText, std::string_view a_packId);
}

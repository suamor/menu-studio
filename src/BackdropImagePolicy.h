#pragma once

#include <string>
#include <string_view>

// The one spelling rule for a pack's image= path. The engine's texture loader
// is handed a fully rooted "Data\Textures\..." string (the same prefix the
// engine itself puts on the stack buffer it gives BSShaderTextureSet); a pack
// author writes whatever felt natural - either slash, any case, with or
// without a leading "textures\" segment. Normalising in exactly one place
// keeps a second caller from drifting on the prefix, and the double-prefix
// mistake is a known defect in the wild (FR rooted an already-rooted override
// path once and every eye went purple).
namespace MTB::BackdropImagePolicy {

    namespace detail {
        [[nodiscard]] inline bool StartsWithCi(std::string_view a_s, std::string_view a_prefix) {
            if (a_s.size() < a_prefix.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a_prefix.size(); ++i) {
                const char a = a_s[i], b = a_prefix[i];
                const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
                const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
                if (la != lb) {
                    return false;
                }
            }
            return true;
        }
    }  // namespace detail

    // "" stays "" (no image); anything else comes back rooted with single
    // backslashes. The author's own casing is kept after the root - the
    // engine's path handling is case-blind, so there is nothing to fix.
    [[nodiscard]] inline std::string Rooted(std::string_view a_manifestPath) {
        if (a_manifestPath.empty()) {
            return {};
        }
        std::string path{ a_manifestPath };
        for (char& c : path) {
            if (c == '/') {
                c = '\\';
            }
        }
        while (!path.empty() && path.front() == '\\') {
            path.erase(path.begin());
        }
        if (detail::StartsWithCi(path, "data\\textures\\")) {
            return path;
        }
        if (detail::StartsWithCi(path, "textures\\")) {
            return "Data\\" + path;
        }
        return "Data\\Textures\\" + path;
    }

    // The cubemap sibling of a rooted image path: "...\x.dds" -> "...\x_cube.dds".
    // The pack pipeline bakes it next to the image; reflections use it through
    // the CS override. A path without a .dds suffix has no sibling (empty).
    [[nodiscard]] inline std::string CubeSibling(std::string_view a_rootedImage) {
        constexpr std::string_view ext = ".dds";
        if (a_rootedImage.size() <= ext.size() ||
            !detail::StartsWithCi(a_rootedImage.substr(a_rootedImage.size() - ext.size()), ext)) {
            return {};
        }
        std::string cube{ a_rootedImage.substr(0, a_rootedImage.size() - ext.size()) };
        cube += "_cube.dds";
        return cube;
    }

    // The card thumbnail sibling: "...\x.dds" -> "...\x_thumb.png". FUCK's
    // image loader reads PNG, not DDS, so the pack pipeline bakes one.
    [[nodiscard]] inline std::string ThumbSibling(std::string_view a_rootedImage) {
        constexpr std::string_view ext = ".dds";
        if (a_rootedImage.size() <= ext.size() ||
            !detail::StartsWithCi(a_rootedImage.substr(a_rootedImage.size() - ext.size()), ext)) {
            return {};
        }
        std::string thumb{ a_rootedImage.substr(0, a_rootedImage.size() - ext.size()) };
        thumb += "_thumb.png";
        return thumb;
    }

}  // namespace MTB::BackdropImagePolicy

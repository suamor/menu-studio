#include "BackdropImagePolicy.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    using MTB::BackdropImagePolicy::Rooted;

    // A bare path is relative to textures\.
    CHECK(Rooted("a.dds") == "Data\\Textures\\a.dds");
    CHECK(Rooted("mtb\\backdrops\\a.dds") == "Data\\Textures\\mtb\\backdrops\\a.dds");

    // A path that already names textures\ must NOT get the segment twice
    // (the double-prefix defect). The author's casing after the root stays.
    CHECK(Rooted("textures\\mtb\\a.dds") == "Data\\textures\\mtb\\a.dds");
    CHECK(Rooted("Textures\\MTB\\A.dds") == "Data\\Textures\\MTB\\A.dds");

    // A fully rooted spelling passes through untouched.
    CHECK(Rooted("Data\\Textures\\mtb\\a.dds") == "Data\\Textures\\mtb\\a.dds");
    CHECK(Rooted("data\\textures\\x.dds") == "data\\textures\\x.dds");

    // Forward slashes and a leading slash are the author's habit, not an error.
    CHECK(Rooted("mtb/backdrops/a.dds") == "Data\\Textures\\mtb\\backdrops\\a.dds");
    CHECK(Rooted("/mtb/a.dds") == "Data\\Textures\\mtb\\a.dds");
    CHECK(Rooted("textures/mtb/a.dds") == "Data\\textures\\mtb\\a.dds");

    // No image stays no image.
    CHECK(Rooted("").empty());

    // "textures" as a plain filename prefix is not the directory segment.
    CHECK(Rooted("texturesA.dds") == "Data\\Textures\\texturesA.dds");

    // The cubemap sibling: swap ".dds" for "_cube.dds"; nothing else has one.
    using MTB::BackdropImagePolicy::CubeSibling;
    CHECK(CubeSibling("Data\\Textures\\mtb\\backdrops\\a.dds") ==
          "Data\\Textures\\mtb\\backdrops\\a_cube.dds");
    CHECK(CubeSibling("Data\\Textures\\x.DDS") == "Data\\Textures\\x_cube.dds");
    CHECK(CubeSibling("Data\\Textures\\noext").empty());
    CHECK(CubeSibling(".dds").empty());
    CHECK(CubeSibling("").empty());

    // The thumbnail sibling mirrors the cube rule with a PNG suffix.
    using MTB::BackdropImagePolicy::ThumbSibling;
    CHECK(ThumbSibling("Data\\Textures\\mtb\\backdrops\\a.dds") ==
          "Data\\Textures\\mtb\\backdrops\\a_thumb.png");
    CHECK(ThumbSibling("noext").empty());

    if (g_fail == 0) {
        std::printf("backdrop_image_policy_test: all checks passed\n");
    }
    return g_fail == 0 ? 0 : 1;
}

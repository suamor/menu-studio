#include "BackdropManifest.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    using namespace MTB;

    // 1. Full valid manifest: image background + stage.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=Aurora\nauthor=Me\n"
            "[Background]\nimage=textures\\mtb\\backdrops\\a.dds\nradius=2000\n"
            "[Stage]\nfloor=meshes\\m\\f.nif\n",
            "aurora");
        CHECK(p.valid);
        CHECK(p.name == "Aurora");
        CHECK(p.author == "Me");
        CHECK(p.hasBackground);
        CHECK(p.bgImage == "textures\\mtb\\backdrops\\a.dds");
        CHECK(p.bgDome.empty());
        CHECK(p.bgFaceCamera == true);  // image default is true
        CHECK(p.bgRadius == 1200.0f);  // explicit values clamp to the supported maximum
        CHECK(p.hasStage);
        CHECK(p.floorMesh == "meshes\\m\\f.nif");
    }

    // Backgrounds without an authored radius use Menu Studio's shipped size.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=Default size\n[Background]\ndome=meshes\\m\\dome.nif\n",
            "default-size");
        CHECK(p.valid);
        CHECK(p.hasBackground);
        CHECK(p.bgRadius == 800.0f);
    }

    // 2. Missing [Pack] name => invalid.
    {
        auto p = ParseBackdropManifest("[Background]\nimage=a.dds\n", "x");
        CHECK(!p.valid);
    }

    // 3. Both image and dome => image wins, with a warning.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=B\n[Background]\nimage=a.dds\ndome=d.nif\n", "b");
        CHECK(p.valid);
        CHECK(p.bgImage == "a.dds");
        CHECK(p.bgDome.empty());
        CHECK(!p.warnings.empty());
    }

    // 3b. Non-.dds image parses but warns - the engine would substitute a
    // placeholder at load with no error, so the parse is the tripwire.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=P\n[Background]\nimage=mtb\\sky.png\n", "p");
        CHECK(p.valid);
        CHECK(p.bgImage == "mtb\\sky.png");
        CHECK(!p.warnings.empty());
    }
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=P2\n[Background]\nimage=mtb\\sky.DDS\n", "p2");
        CHECK(p.valid);
        CHECK(p.warnings.empty());  // case-blind: .DDS is fine
    }

    // 3c. Numbered backgrounds: [Background] + [Background1..N], own names,
    // stop at the first absent section, pack group carried through.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=Skies\ngroup=Load order skies\n"
            "[Background]\nname=Sovngarde\ndome=sky\\sovngarde.nif\n"
            "[Background1]\ndome=sky\\dome2.nif\n"
            "[Background2]\nname=Third\nimage=mtb\\third.dds\n"
            "[Background4]\ndome=sky\\never.nif\n",  // gap: 3 absent, 4 unread
            "skies");
        CHECK(p.valid);
        CHECK(p.group == "Load order skies");
        CHECK(p.backgrounds.size() == 3);
        CHECK(p.backgrounds[0].name == "Sovngarde");
        CHECK(p.backgrounds[1].name == "Skies 1");   // default numbered name
        CHECK(p.backgrounds[1].dome == "sky\\dome2.nif");
        CHECK(p.backgrounds[2].name == "Third");
        CHECK(p.backgrounds[2].image == "mtb\\third.dds");
        CHECK(p.backgrounds[2].faceCamera == true);  // image default
        CHECK(p.bgDome == "sky\\sovngarde.nif");     // first-background mirror
    }

    // 3d. thumb= rides a background section through to the parse.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=T\n[Background]\ndome=d.nif\nthumb=mtb\\skies\\t.png\n", "t");
        CHECK(p.valid);
        CHECK(p.backgrounds[0].thumb == "mtb\\skies\\t.png");
    }

    // 4. Dome background: faceCamera defaults false.
    {
        auto p = ParseBackdropManifest("[Pack]\nname=D\n[Background]\ndome=d.nif\n", "d");
        CHECK(p.valid);
        CHECK(p.bgDome == "d.nif");
        CHECK(p.bgFaceCamera == false);
    }

    // 5. Stage with no floor, no background => invalid.
    {
        auto p = ParseBackdropManifest("[Pack]\nname=C\n[Stage]\nradius=600\n", "c");
        CHECK(!p.valid);
    }

    // 6. Stage extras.
    {
        auto p = ParseBackdropManifest(
            "[Pack]\nname=E\n[Stage]\nfloor=f.nif\n"
            "[Piece1]\nmesh=b.nif\ny=380\nfit=60\ntint=1\n",
            "e");
        CHECK(p.valid);
        CHECK(p.extras.size() == 1);
        CHECK(p.extras[0].mesh == "b.nif");
        CHECK(p.extras[0].y == 380.0f);
        CHECK(p.extras[0].fitRadius == 60.0f);
        CHECK(p.extras[0].tint == true);
    }

    if (g_fail == 0) {
        std::printf("ALL PASS\n");
    }
    return g_fail ? 1 : 0;
}

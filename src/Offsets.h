#pragma once

namespace MTB::Offsets {
    // §3.4 AE-readiness (docs/SPEC.md): every engine address the plugin uses,
    // in one table, as RELOCATION-style (SE, AE) pairs. The AE (1.6.1170) half
    // was reverse-engineered against the real binary and each function
    // decompile-matched to its SE twin (Ghidra AE project SkyrimAE_1170), then
    // checked so every id resolves in the 1.6.1170 Address Library and the
    // DispatchCallOffset byte is E8 on both builds. Universal DLL: gate in
    // plugin.cpp accepts SE 1.5.97 || AE 1.6.317+, with VersionCheck::Run()
    // proving the table on any build that is not one of the two verified ones.

    // Main::Update - hosts the frame-driver call site.
    inline constexpr REL::RelocationID MainUpdate{ 35565, 36564 };

    // The per-frame player-update dispatch Main::Update calls - i.e. the
    // TARGET of the frame-driver call site, not a function we ever call
    // ourselves. It is in the table so VersionCheck can find that call site by
    // matching its target instead of trusting a hand-measured byte offset,
    // which is the one kind of address that cannot survive a version change.
    inline constexpr REL::RelocationID MainUpdatePlayerDispatch{ 35578, 36581 };

    // Engine frame-dt globals (Main::UpdateTimers) - telemetry only; the
    // bubble runs on its own QPC clock.
    inline constexpr REL::RelocationID SlowDt{ 523660, 410199 };
    inline constexpr REL::RelocationID RealDt{ 523661, 410200 };
    inline constexpr REL::RelocationID DtVariant3{ 523662, 410201 };

    // BSFaceGenAnimationData::Update - the face tick (SE RVA 0x3C4030).
    // AE 26598 (FUN_14041d750): exact decompile-match - same (this,float,char)
    // sig, +0x220 lock, +0x217 return flag, +0x200==4 reset gate, +0x224 dance,
    // and the sub-controller cascade on +0x20..+0x180. Sibling of Reset
    // (25977/26586) + SetExpressionOverride (25980/26594) in CommonLib.
    inline constexpr REL::RelocationID FaceGenUpdate{ 25983, 26598 };

    // THE FACEGEN MORPH APPLY - what actually puts new lid/expression values
    // on the face MESH. `void(BSFaceGenNiNode* a_faceNode, bool a_force)`;
    // SE RVA 0x3D8980, AE RVA 0x432550.
    //
    // Found by decompiling BSFaceGenNiNode::UpdateDownwardPass end to end
    // (SE 26417 / AE 26998, Tools\re\research\mtb_facegen2.c + mtb_facebake*.c).
    // Setting the anim data's change byte BAKES NOTHING: all UpdateDownwardPass
    // does with it is register the face node in a global queue (SE
    // DAT_142f07d18), and the queue's only consumer (SE 0x3D9320) is reached
    // from a parallel job batch that Main::Update runs on its NOT-PAUSED
    // branch. So while a menu holds the pause, nothing drains the queue.
    //
    // Main::Update's PAUSED branch carries exactly one face bake, and it is
    // this function, hard-gated on one menu:
    //     if (UI::IsMenuOpen(InterfaceStrings::raceSexMenu))
    //         FaceGenApplyMorphs(player->GetFaceNodeSkinned(), true);
    // which is why faces animate in RaceMenu and in no other paused menu.
    //
    // Both builds verified the same way: exactly two callers (the job
    // callback and the Main::Update paused branch), identical field offsets
    // (+0x122 child count, +0x118 children, +0x150 anim data, +0x160 flags),
    // and the cross-check that this resolver reproduces FaceGenUpdate's
    // known 25983/26598 pair from the same two binaries.
    inline constexpr REL::RelocationID FaceGenApplyMorphs{ 26407, 26988 };

    // Third-person camera position builder (SE RVA 0x850260) - hosts the
    // collision-smoother call CameraGate gates. AE 50911 (FUN_1408e8640):
    // decompile-matched via its caller (49960->50896). AE inlined the smoother,
    // but its obstruction query remains one ordinary call (below).
    inline constexpr REL::RelocationID CameraPositionBuilder{ 49975, 50911 };

    // The collision smoother the builder above calls on SE - the TARGET of
    // CameraGate's hook site, same role as MainUpdatePlayerDispatch. AE id is
    // 0 on purpose: AE inlined it, so there is no function and no call, and
    // RelocationID::address() returns 0 for a zero id, which VersionCheck
    // reports as "n/a on this runtime" rather than as a failure.
    inline constexpr REL::RelocationID CameraCollisionSmoother{ 49980, 0 };

    // AE's obstruction query inside the inlined smoother. 1.6.1170 builder
    // 50911 calls ID 50832 exactly once at +0x1CE. The query mutates the
    // proposed ThirdPersonState::translation when it hits geometry; gating
    // this call plus settling collisionPos reproduces the SE bypass without
    // replacing the whole position builder. SE keeps using the smoother hook.
    inline constexpr REL::RelocationID CameraCollisionTest{ 0, 50832 };

    // TESCamera::Update - the base body that actually copies the camera state
    // into cameraRoot. It is PlayerCamera vtable slot 2 on BOTH builds
    // (RTTI-verified: SE vtable 0x16A9EE0 -> 0x4F5B60, AE vtable 0x18EF278 ->
    // 0x5510A0), which is what the studio camera's write_vfunc re-assert
    // covers. The pair matches CommonLib's published TESCamera::Update ids -
    // the independent cross-check that the RTTI walk found the right body.
    inline constexpr REL::RelocationID TESCameraUpdateBase{ 32289, 33025 };

    // ⚠ AE DEVIRTUALIZED THE ENGINE'S OWN CALLS TO THAT BODY, WHICH IS WHY A
    // VTABLE HOOK ALONE WAS A DECOY THERE. Exhaustive E8 scan over .text:
    // SE 1.5.97 has ZERO direct callers (every SE path dispatches through the
    // vtable), AE 1.6.1170 has exactly two, and they are the per-frame ones -
    // the player-update chunk (40450, calls at +0x69) and PlayerCamera's
    // master per-frame update (50784, the id camera mods detour, calls at
    // +0x1A6). A mod driving the camera during a menu reaches the root write
    // through one of these whether it calls camera->Update() virtually or the
    // master update by id. Both containers are 0 on SE on purpose, same
    // convention as CameraCollisionSmoother: no site, hook simply absent.
    inline constexpr REL::RelocationID PlayerUpdateCameraCaller{ 0, 40450 };
    inline constexpr REL::RelocationID PlayerCameraMasterUpdate{ 0, 50784 };

    // The input pump's dispatch call, the site every ImGui-overlay mod hooks
    // (FLICK's PDB names its thunk on it Hooks::ProcessInputQueue). FLICK
    // swallows the whole queue while one of its windows holds the mouse, which
    // is upstream of every BSTEventSource sink - so a camera gesture that
    // starts before its passthrough opens simply never existed as far as a
    // sink can tell. Menu Studio chains at the same site and, loading after
    // FUCK.dll, wraps it: the tap reads the REAL queue before the swallow.
    // Verified on both clean binaries: the E8 sits at +0x7B in the container
    // and its target is the dispatch below (cross-check: the AE container is
    // the call at Main::Update+0x567).
    inline constexpr REL::RelocationID InputPumpCaller{ 67315, 68617 };
    inline constexpr REL::RelocationID InputPumpDispatch{ 67355, 68655 };

    // Inventory3DManager's item-stage visibility paths. UpdateMagic3D clears
    // kHidden on a cached selection; the appender attaches both synchronous
    // and late async models visible. ItemPreviewBroker post-hides each write.
    inline constexpr REL::RelocationID Inventory3DUpdateMagic{ 50885, 51758 };
    inline constexpr REL::RelocationID Inventory3DAppendModel{ 50896, 51772 };

    // UIMessageQueue::AddMessage - how a menu is asked to open or close. The
    // ids are CommonLib's own (Offsets.h, RELOCATION_ID(13530, 13631)); they
    // are repeated here so the self-check can PROVE the id is in the running
    // database before anything calls it.
    //
    // ⚠ THAT PROOF IS NOT CEREMONY, IT IS A CRASH THIS MOD ALREADY SHIPPED.
    // CommonLib's AE id for Script::CompileAndRun is 21890 and 21890 is ABSENT
    // from the 1.6.1170 database. id2offset's lower_bound only fails past the
    // END of the table, so an absent id resolves to its NEIGHBOUR silently:
    // the call went to 21891 and took the game down (field log 2026-08-04,
    // SkyrimSE.exe+0x33D8D9 = 21891+0x59, called from MenuStudio.dll). An id
    // being in CommonLib is not evidence that it is in the player's database.
    inline constexpr REL::RelocationID UIMessageQueueAddMessage{ 13530, 13631 };

    // The engine's limited-racemenu switch: a static bool the RaceSexMenu
    // reads at open to hide race and sex - the mode Galathil the face
    // sculptor uses, and the whole body of Game.ShowLimitedRaceMenu() is
    // "set this to 1, post the same kShow message we already post"
    // (AE 1.6.1170 native worker 0x140953450, decompiled 2026-08-05:
    // DAT_1431af130 = 1; AddMessage(queue, InterfaceStrings+0x160, kShow, 0)).
    // The menu's own code carries the writes back to 0, so setting it is the
    // entire contract. AE id 406380 = RVA 0x31af130, round-tripped through
    // the 1.6.1170 Address Library.
    //
    // ⚠ THE SE ID IS PATTERN-MATCHED, NOT PROVEN. 519785 is the byte a
    // worker in the SE racemenu region sets to 1 beside the same AddMessage
    // call, but which NATIVE owns that worker was never pinned down by
    // decompile, and a static WRITE through a wrong id corrupts a stranger's
    // global with no crash to notice it by. So the action bar only writes
    // this on AE and says so on SE - prove the SE side before widening,
    // exactly per the CompileAndRun note above.
    inline constexpr REL::RelocationID RaceMenuLimitedFlag{ 519785, 406380 };

    // Studio rig lights - engine factories for formless render-side lights
    // (no LIGH forms, no placed refs, zero save surface; the approach
    // po3's Light Placer proved). Signatures verified by decompile
    // (Tools\re\research\mtb_studiorig.c): AddLight RTTI-checks the light
    // and branches on params.shadowLight/params.fov exactly per the
    // LIGHT_CREATE_PARAMS layout mirrored in StudioRig.cpp.
    // AE: NiPointLightCreate 70966 (FUN_140d399d0, the plain factory that
    // writes the NiPointLight vtable + allocs 0x150; disambiguated from an
    // identical sibling by reinit-adjacency). AddLight 106326 (FUN_1414a0940,
    // exact match: two-descriptor RTTI check + shadowLight/fov 0x140/0x560/0x578
    // branch). RemoveLight 106332 (FUN_1414a1250, same *param_2 && +0x300 guard
    // + the +6-id adjacency to AddLight).
    inline constexpr REL::RelocationID NiPointLightCreate{ 69582, 70966 };     // () -> NiPointLight*
    inline constexpr REL::RelocationID ShadowSceneAddLight{ 99692, 106326 };   // (SSN*, NiLight*, params*) -> BSLight*
    inline constexpr REL::RelocationID ShadowSceneRemoveLight{ 99698, 106332 };// (SSN*, NiPointer<BSLight>&)

    // r46 exterior sun park: BSShaderManager::State, the renderer's own
    // global. Two things are read out of it, both at fixed offsets that are
    // the SAME on SE and AE (CommonLib declares the struct with plain member
    // offsets and only relocates the singleton itself, which is what makes
    // that claim checkable rather than assumed):
    //   +0x00  ShadowSceneNode* shadowSceneNode[4] - slot 0 is the world's,
    //          and its sunLight/cloudLight are the BSLights the gather
    //          actually reads. Taken from here rather than from Sky::sun so
    //          the light we write is the one the renderer uses.
    //   +0xC8  NiTransform directionalAmbientTransform - the sky's ambient
    //          fill on every surface in frame.
    // ⚠ THE IDS ARE COMMONLIB'S OWN (RELOCATION_ID(513211, 390951)), not a
    // reading of ours - the version of CommonLib this project builds against
    // predates the header that declares them. VersionCheck carries it as
    // kData for exactly that reason: if the id is wrong on a build, the
    // plugin declines to load rather than writing over a stranger.
    inline constexpr REL::RelocationID ShaderManagerState{ 513211, 390951 };

    // Sky's forced interior-lighting refresh (SE RVA 0x3B4030): when an
    // interior cell is current it re-pulls ambient/directional/fog/DALC
    // from the cell through the inherit-aware blend getters (family at
    // 0x262C00..0x263400), pushes them to the renderer + sun light, then
    // restores the sky mode and re-runs Sky::Update (25682; CalculateColors
    // = 25685). The engine otherwise only does this on cell ATTACH - the
    // reason live INTERIOR_DATA edits never rendered. Gated by a refresh
    // timer at Sky+0x1D4 (write big to force) and a TESWaterSystem flag
    // (irrelevant in menus). Decompile: Tools\re\research\mtb_skyrefresh.c.
    // AE 26237 (FUN_14040d780): exact decompile-match - TLS tag 0x5d, timer
    // +0x1d4, mode +0x1bc set-to-3/restore, +0xB8 gate, interior flags +0xf4,
    // two-phase calculate -> Sky::Update, +0x70/+0x88 light-list vfunc+0x18.
    inline constexpr REL::RelocationID SkyForceInteriorRefresh{ 25690, 26237 };

    // TESWaterSystem singleton (AE id from CommonLib) - the refresh wrapper
    // above silently no-ops while its +0xB8 byte is set; StudioLight clears
    // it around the call and restores.
    inline constexpr REL::RelocationID TESWaterSystemSingleton{ 514290, 400450 };

    // Backdrop meshes - BSModelDB::Demand (SE RVA 0xD2F220): resolves
    // "meshes\\<path>" through the model DB and hands back the CACHED
    // template root (entry+0x28, refcount-bumped), shared with every placed
    // ref of that model - clone it, never attach it. NiAVObject deep clone
    // (SE RVA 0xC52750): stack NiCloningProcess wired to the engine's own
    // copyType/appendChar globals, CreateClone + ProcessClone - the same
    // instantiate-a-template path placed refs use. Decompiles:
    // Tools\re\research\mtb_modeldb.c + mtb_niclone.c. AE ids are from
    // CommonLibSSE-NG's table; both resolve in the 1.6.1170 addrlib.
    inline constexpr REL::RelocationID BSModelDBDemand{ 74040, 75782 };
    inline constexpr REL::RelocationID NiAVObjectClone{ 68835, 70187 };

    // The engine's screen fade - the function behind the Papyrus native
    // Game.FadeOutGame (SE RVA 0x8D5530): looks up the FADER MENU via
    // InterfaceStrings+0x08, writes direction (+0x24, stored inverted),
    // black flag (+0x25), duration (+0x20), then starts the fade. It is a
    // MENU - its Scaleform movie advances on UI time, so the fade animates
    // while the game is paused (why RaceMenu's and the skills menu's dips
    // work in paused contexts - the exact look F-12's arm dip borrows).
    // ABI byte-verified from the native wrapper (0x9724F0): RCX=fadingOut,
    // RDX=blackFade, XMM2=duration, R9=unk(0), stack=secsBeforeFade.
    // Decompiles: Tools\re\research\mtb_fadeout*.c.
    // AE 52847 (FUN_1409768e0): exact decompile-match - TLS tag 0x46,
    // InterfaceStrings menu lookup, the five field writes +0x24(inverted
    // dir)/+0x25/+0x26/+0x20(duration)/+0x1c, single tail call to start the fade.
    inline constexpr REL::RelocationID FadeOutGame{ 51909, 52847 };

    // UI's by-name menu pause setter (SE RVA 0xEBFC10): finds the menu in
    // the registered-menu map, no-ops when kPausesGame already matches,
    // otherwise sets/clears the flag, adjusts UI::numPausesGame and fires
    // the pause-edge event - the exact bookkeeping the UI message pump's
    // open (ID 79945) and close paths run. Raw counter helpers 79942/79943
    // exist but skip the flag, which the close path re-reads; the setter is
    // the symmetric choice. Decompiles: Tools\re\research\mtb_pausecounter.c.
    // AE 82089 (FUN_140fa5870): exact decompile-match - +0x1c flag bit&1 =
    // kPausesGame, +0x160 numPausesGame ++/--, the (bit != arg) no-op guard, the
    // pause-edge event fired on this+0x60. (The sibling 82090 uses bit 0x8000 /
    // counter +0x174 - a different setter.) Matches the menu-pump (79945/82082)
    // +7 adjacency.
    inline constexpr REL::RelocationID SetMenuPausesGame{ 79952, 82089 };

    // Call-site offsets INSIDE the functions above.
    //
    // ⚠ THESE ARE HINTS, NOT ADDRESSES. Everything else in this file is an
    // Address Library id, which the database re-points for whatever build is
    // running. A byte offset into a function is the opposite: it was measured
    // on ONE binary and nothing makes it survive a recompile. These two values
    // are what actually blocked mid-version support - not the ids.
    //
    // So they are only ever a fast path now. VersionCheck::LocateCall tries
    // the hint, and if the byte there is not an E8 whose target is the known
    // callee it scans the containing function for the call that is. Callers
    // must take the located offset from VersionCheck, never these directly.

    // Main::Update -> player dispatch (MainUpdatePlayerDispatch).
    // SE +0x28E, AE +0x2A7 on 1.6.1170 (it MOVED between the two).
    inline std::ptrdiff_t DispatchCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0x28E }, std::ptrdiff_t{ 0x2A7 });
    }

    // Position builder -> collision smoother (CameraCollisionSmoother).
    // SE +0x1C5; AE inlined the smoother, so there is no site and the hint is
    // 0. The scan then finds nothing either, and the bypass is simply OFF on
    // AE - a documented limitation, not an error.
    inline std::ptrdiff_t SmootherCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0x1C5 }, std::ptrdiff_t{ 0 });
    }

    // AE position builder -> inlined smoother's obstruction query.
    inline std::ptrdiff_t CollisionTestCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0 }, std::ptrdiff_t{ 0x1CE });
    }

    // The two devirtualized TESCamera::Update calls (AE only; SE has none).
    // Measured on the 1.6.1170 binary by target-verified E8 scan.
    inline std::ptrdiff_t CameraPlayerCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0 }, std::ptrdiff_t{ 0x69 });
    }
    inline std::ptrdiff_t CameraMasterCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0 }, std::ptrdiff_t{ 0x1A6 });
    }

    // Input pump -> dispatch (InputPumpDispatch). Same +0x7B on both builds.
    inline std::ptrdiff_t InputPumpCallOffsetHint() {
        return REL::Relocate(std::ptrdiff_t{ 0x7B }, std::ptrdiff_t{ 0x7B });
    }
}

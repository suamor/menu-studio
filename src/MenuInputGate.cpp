#include "PCH.h"

#include "MenuInputGate.h"

#include "Bubble.h"
#include "Settings.h"
#include "StudioCamera.h"

namespace {
    constexpr std::uint32_t kLeftMouseButton = 0;   // GFx: 0 left, 1 right, 2 middle
    constexpr std::uint32_t kRightMouseButton = 1;

    // Two separate reasons to keep a mouse press away from Flash, sharing one
    // hook because they are the same operation on the same message.
    bool ShouldEatMouse(RE::UIMessage& a_message) {
        // ⚠ NOT GATED ON IsBubbleActive HERE. That asks whether the world is
        // frozen, and the pause lands a few frames after a menu opens, so a
        // swing begun in that window would have its press handed to the menu
        // after all. A drag the camera has already taken is its own proof that
        // this press is not the menu's, so the left branch below stands on that
        // instead. The right branch keeps the old condition, since the preview
        // spin genuinely only runs while the bubble is up.
        if (a_message.type != RE::UI_MESSAGE_TYPE::kScaleformEvent || !a_message.data) {
            return false;
        }
        const auto* data = static_cast<RE::BSUIScaleformData*>(a_message.data);
        const auto* ev = data->scaleformEvent;
        using ET = RE::GFxEvent::EventType;
        if (!ev) {
            return false;
        }
        // Mouse MOVES too, while a swing is held. Flash tracks the cursor on
        // its own clock, so a drag crossing the list still hover-highlighted
        // rows, played SkyUI's move sound and, in Fitting Room, flashed the
        // item preview under the editor - all from a gesture that was never
        // the menu's. Only while the drag is ACTIVE: the owed-release tail
        // leaves moves alone, since by then the cursor belongs to the menu.
        if (ev->type == ET::kMouseMove) {
            return MTB::Bubble::CameraDragActive();
        }
        // ⚠⚠ FLASH IS THE PATH, AND IT TOOK FOUR ROUNDS TO GET HERE. Measured
        // 2026-08-13, one second of log:
        //
        //   .562  Scaleform MOUSE WHEEL #1, item zoom 0.000
        //   .562  inspect gate refused a notch at CanProcess (2, ProcessButton 0)
        //   .574  item zoom left 0 (0.010)
        //   .581  Scaleform MOUSE WHEEL #2, item zoom 0.020
        //   .607  #3, 0.058     .632  #4, 0.096     1.320  #5, 1.000
        //
        // The MenuControls handler was refusing correctly and the zoom climbed
        // anyway, in step with the Scaleform wheel events. So the item inspect
        // is driven from Flash, and the only place to stop it is here, on the
        // message that carries it.
        //
        // ⚠⚠ AND IT IS SPLIT BY THE CAMERA'S OWN REGION, WHICH IS WHY THIS IS
        // NOT THE BLANKET REFUSAL Bubble.cpp:2130 is the autopsy of. Menu list
        // scrolling rides this same pump; eating every notch would take the
        // list's wheel away to fix the character's. The zone answers exactly the
        // question being asked - is the cursor over the character or over the
        // menu - and it is the same measured boundary that already decides
        // whether a drag is the camera's. Over the list, nothing changes at all.
        //
        // ⚠⚠ AND ONLY WHILE INSPECT IS CLOSED. Once it is open the wheel MUST
        // reach the item, or the player is inside a mode they cannot zoom or
        // leave. That is not hypothetical: the previous build cleared the stage
        // from under a live inspect and stranded them in an empty one.
        if (ev->type == ET::kMouseWheel) {
            if (!MTB::Settings::GetSingleton().inspectNeedsHotkey ||
                !MTB::Bubble::IsBubbleActive() || MTB::Bubble::IsRaceMenuOpen()) {
                return false;
            }
            auto* inv = RE::Inventory3DManager::GetSingleton();
            if (inv && inv->GetRuntimeData().zoomProgress != 0.0f) {
                return false;  // already inspecting - the wheel is the item's
            }
            if (!MTB::StudioCamera::CursorInZone()) {
                return false;  // over the menu's own list; leave scrolling alone
            }
            static std::uint32_t s_eaten = 0;
            if (++s_eaten <= 6) {
                spdlog::info("inspect gate: ate a Scaleform wheel event over the "
                             "character (#{}), so the notch is the camera's alone. "
                             "Over the menu's list it is untouched, and once inspect "
                             "is open it is handed straight back.",
                             s_eaten);
            }
            return true;
        }
        if (ev->type != ET::kMouseDown && ev->type != ET::kMouseUp) {
            return false;
        }
        const auto button = static_cast<const RE::GFxMouseEvent*>(ev)->button;

        // Right: the preview spin's own drag, so the UI's quick buy or equip
        // never fires under a rotation. Eat both edges so Flash never sees half
        // a press.
        if (button == kRightMouseButton) {
            return MTB::Settings::GetSingleton().blockRightMouse &&
                   MTB::Bubble::IsBubbleActive();
        }

        // Left: a camera swing that began away from the panel. It is allowed to
        // cross a panel and keep turning, and while it does the menu must not
        // react to it at all, or ending a swing over the item list would equip
        // whatever the cursor landed on.
        //
        // ⚠ Narrow ON PURPOSE. This is true only between a press the camera
        // took and its own release, so ordinary clicking is untouched: a press
        // that started ON the panel was never the camera's and is never eaten.
        if (button == kLeftMouseButton && MTB::Bubble::CameraDragOwnsMouse()) {
            if (ev->type == ET::kMouseUp) {
                MTB::Bubble::ClearOwedLeftUp();  // one swing owes exactly one
            }
            return true;
        }
        return false;
    }

    // One vfunc-0x4 (ProcessMessage) gate per bubble menu. Coexists with
    // other same-slot vfunc hooks (Apparel Preview's inspect gates) - each
    // chains through to the previous target.
    template <class MenuT>
    struct ProcessMessageGate {
        static RE::UI_MESSAGE_RESULTS thunk(MenuT* a_this, RE::UIMessage& a_message) {
            if (ShouldEatMouse(a_message)) {
                return RE::UI_MESSAGE_RESULTS::kHandled;
            }
            return orig(a_this, a_message);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;

        static void Install(const REL::VariantID& a_vtable0) {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtable0 };
            orig = vtbl.write_vfunc(0x4, thunk);
        }
    };

    // F-14 v3: while the bubble spins the character on the right stick, the
    // menu's own item-preview rotation must let go of that stick - the
    // engine maps it to the 'Rotate' user event, which MenuControls feeds
    // to Inventory3DManager. Blank the event ONLY while not inspecting
    // (zoomProgress == 0); inspect mode keeps vanilla item rotation. SPIM
    // ships this exact hook for its gamepad mode (Item3DControls,
    // Tools/ShowPlayerInMenus/src/Event.cpp, MIT - its 2.0.3 fix commit);
    // it is absent from this load order (SPIM disabled), so the bubble
    // carries its own. write_vfunc chains cleanly if both ever load.
    //
    // The MOUSE branch beside it is only a compatibility fallback for control
    // maps that name their wheel events. On this rig every notch has an empty
    // userEvent, so the measured inspect-entry gate is the Scaleform message
    // handler above. Once inspect opens, both paths stand aside.
    //
    // ⚠ NOT A WHEEL BAN, AND THE DIFFERENCE IS THE WHOLE DESIGN. A blanket
    // wheel refusal was tried once and cost the editor its zoom everywhere to
    // fix one dialog - the autopsy is in Bubble.cpp beside the RaceMenu preset
    // report. This blanks one user event on one device in the frames before
    // inspect is open, and leaves the inspect control itself untouched.
    //
    // ⚠ THE TWO SETTINGS ARE READ SEPARATELY. They used to share one outer
    // test because there was only one branch; a player who turned the character
    // spin off would otherwise silently lose the inspect gate as well, which is
    // a setting disabling something it does not name.
    // Is this the mouse wheel? By ID CODE, because that is the only thing the
    // wheel reliably carries - see the ⚠⚠ on ItemPreviewWheelGate below.
    [[nodiscard]] bool IsWheelEvent(const RE::InputEvent* a_event) {
        if (!a_event || a_event->GetDevice() != RE::INPUT_DEVICE::kMouse ||
            !a_event->HasIDCode()) {
            return false;
        }
        using Key = RE::BSWin32MouseDevice::Key;
        const auto id = static_cast<const RE::IDEvent*>(a_event)->idCode;
        return id == static_cast<std::uint32_t>(Key::kWheelUp) ||
               id == static_cast<std::uint32_t>(Key::kWheelDown);
    }

    // THE INSPECT ENTRY GATE, at the one place that can actually refuse it.
    //
    // ⚠⚠ THE FIRST ATTEMPT BLANKED A USER EVENT AND COULD NEVER HAVE WORKED ON
    // THIS RIG. The design was measured against one `controlmap.txt` out of the
    // four in the load order, on the reading that the wheel carries `Zoom In`
    // and `Zoom Out` in the Item Menus context. It does not carry them here. The
    // field log is unambiguous - every notch arrives as
    //
    //     input evidence: type=0 device=1 id=8 userEvent=''
    //
    // device 1 is the mouse, id 8 is kWheelUp, and the user event is the EMPTY
    // STRING. Nothing downstream is matching a name, so blanking the name
    // changes nothing, and the same log shows the item entering inspect anyway
    // (`18 item preview` refusals on the camera side - the first time that
    // counter has ever been above zero in any captured log).
    //
    // The next attempt moved to the Inventory3DManager handler. Field evidence
    // rejected it too: CanProcess refused while Flash still opened inspect,
    // and ProcessButton was never called. The struct below is retained only as
    // the measured autopsy and is deliberately not installed.
    //
    // ⚠ THIS IS NOT THE BLANKET WHEEL REFUSAL Bubble.cpp:2130 is the autopsy of.
    // That one refused the wheel for a whole MENU and cost the editor its zoom
    // everywhere to fix one dialog. This refuses ONE HANDLER - the floating item
    // preview - and only in the frames where it is not already open. Every other
    // consumer of the wheel, the menu's own list scrolling included, is
    // untouched because their handlers are never asked about.
    //
    // ⚠ AND IT LEAVES THE DOOR. Only the wheel is refused, so the inspect
    // control itself still reaches ProcessButton, still opens inspect, and from
    // that moment zoomProgress is above zero and the wheel is handed back for
    // vanilla item zooming.
    // Historical diagnostic only. Field evidence proved Flash opens inspect
    // even when this handler refuses CanProcess, so Install() is deliberately
    // not called. Keeping the counters beside the autopsy makes that negative
    // result auditable without occupying Apparel Preview's same vtable slot.
    struct ItemPreviewWheelGate {
        // Shared by both doors below. a_this is the handler, which is the
        // Inventory3DManager singleton on both.
        [[nodiscard]] static bool ShouldRefuse(RE::Inventory3DManager* a_this,
                                               const RE::InputEvent*   a_event) {
            return a_this && MTB::Settings::GetSingleton().inspectNeedsHotkey &&
                   MTB::Bubble::IsBubbleActive() && !MTB::Bubble::IsRaceMenuOpen() &&
                   IsWheelEvent(a_event) &&
                   a_this->GetRuntimeData().zoomProgress == 0.0f;
        }

        // ⚠ THE COUNTERS ARE THE POINT, NOT DECORATION. A one-shot line proved
        // this hook RUNS and proved nothing about whether it is on the path
        // that opens inspect - the item kept zooming with the refusal logged.
        // Two counters, reported together, separate the three live theories:
        // CanProcess refused and ProcessButton never asked (the engine gates on
        // consent, so something else opens inspect); ProcessButton asked anyway
        // (the engine does NOT gate, and refusing here is the fix); or neither
        // fires (this handler is not the path at all).
        static inline std::uint32_t canProcessRefusals = 0;
        static inline std::uint32_t processButtonRefusals = 0;

        static void Report(const char* a_door) {
            const std::uint32_t total = canProcessRefusals + processButtonRefusals;
            if (total > 12 && total % 25 != 0) {
                return;  // the first dozen answer it; then one line per 25
            }
            spdlog::info("inspect gate: refused a wheel notch at {} "
                         "(CanProcess {}, ProcessButton {}). Inspect is closed, so the "
                         "notch belongs to the camera; the inspect control still opens "
                         "inspect and the wheel comes back once it is.",
                         a_door, canProcessRefusals, processButtonRefusals);
        }

        static bool canProcessThunk(RE::Inventory3DManager* a_this,
                                    RE::InputEvent*         a_event) {
            if (ShouldRefuse(a_this, a_event)) {
                ++canProcessRefusals;
                Report("CanProcess");
                return false;
            }
            return canProcessOrig(a_this, a_event);
        }

        // ⚠ THE SECOND DOOR, AND IT IS NOT REDUNDANT UNTIL MEASURED. A handler
        // whose CanProcess said no should never be asked to process the button,
        // but the field says the item still enters inspect with CanProcess
        // refusing - so either the engine does not gate on consent here, or
        // this handler is not the one doing it. Refusing at both closes the
        // first case outright and leaves the second visible in the counters.
        static bool processButtonThunk(RE::Inventory3DManager* a_this,
                                       RE::ButtonEvent*        a_event) {
            if (ShouldRefuse(a_this, a_event)) {
                ++processButtonRefusals;
                Report("ProcessButton");
                return false;
            }
            return processButtonOrig(a_this, a_event);
        }

        static inline REL::Relocation<decltype(&canProcessThunk)>    canProcessOrig;
        static inline REL::Relocation<decltype(&processButtonThunk)> processButtonOrig;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Inventory3DManager[0] };
            canProcessOrig    = vtbl.write_vfunc(0x1, canProcessThunk);
            processButtonOrig = vtbl.write_vfunc(0x5, processButtonThunk);
        }
    };

    struct ItemRotateGate {
        static RE::BSEventNotifyControl thunk(RE::MenuControls* a_this,
                                              RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>* a_source) {
            const auto& cfg = MTB::Settings::GetSingleton();
            const bool  gateRotate  = cfg.previewSpin;
            const bool  gateInspect = cfg.inspectNeedsHotkey;
            if (a_event && (gateRotate || gateInspect) &&
                MTB::Bubble::IsBubbleActive() && !MTB::Bubble::IsRaceMenuOpen()) {
                auto* inv = RE::Inventory3DManager::GetSingleton();
                if (!inv || inv->GetRuntimeData().zoomProgress == 0.0f) {
                    const auto* userEvents = RE::UserEvents::GetSingleton();
                    for (auto* event = *a_event; userEvents && event; event = event->next) {
                        if (!event->HasIDCode()) {
                            continue;
                        }
                        auto*      idEvent = static_cast<RE::IDEvent*>(event);
                        const auto device  = event->GetDevice();
                        if (gateRotate && device == RE::INPUT_DEVICE::kGamepad &&
                            idEvent->userEvent == userEvents->rotate) {
                            idEvent->userEvent = "";
                        } else if (gateInspect && device == RE::INPUT_DEVICE::kMouse &&
                                   (idEvent->userEvent == userEvents->zoomIn ||
                                    idEvent->userEvent == userEvents->zoomOut)) {
                            // ⚠ BELT, NOT BRACES, AND IT DOES NOTHING ON THIS
                            // RIG. The dev rig's winning controlmap leaves the
                            // wheel unnamed in the item menus - the field log
                            // reads `userEvent=''` for every notch - so this
                            // branch never fires and ItemPreviewWheelGate above
                            // is what actually does the work. It stays for a
                            // load order whose controlmap DOES name the wheel,
                            // where a named event could reach a consumer the
                            // handler gate cannot see.
                            //
                            // ⚠ 'Zoom In' / 'Zoom Out' on the mouse is also the
                            // gameplay camera's zoom, neutralised inside a
                            // bubbled menu only while OwnView owns it
                            // (OwnView.cpp). Blanking is the safer direction of
                            // the two, but a bubbled menu OwnView does NOT own
                            // has still not been checked - and on this rig
                            // nothing has exercised it either way.
                            idEvent->userEvent = "";
                        }
                    }
                }
            }
            return orig(a_this, a_event, a_source);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MenuControls[0] };
            orig = vtbl.write_vfunc(0x1, thunk);
        }
    };
}

namespace MTB::MenuInputGate {
    void Install() {
        ProcessMessageGate<RE::ContainerMenu>::Install(RE::VTABLE_ContainerMenu[0]);
        ProcessMessageGate<RE::BarterMenu>::Install(RE::VTABLE_BarterMenu[0]);
        ProcessMessageGate<RE::InventoryMenu>::Install(RE::VTABLE_InventoryMenu[0]);
        ProcessMessageGate<RE::MagicMenu>::Install(RE::VTABLE_MagicMenu[0]);
        ItemRotateGate::Install();
        spdlog::info("MenuInputGate: mouse gates on Container/Barter/Inventory/Magic "
                     "(right-mouse for the preview spin, left-mouse for the duration "
                     "of a camera swing, wheel for inspect entry) + the gamepad "
                     "item-rotate gate. The gates "
                     "read their settings per event, so nothing is decided here; this "
                     "runs before the INI is loaded.");
    }
}

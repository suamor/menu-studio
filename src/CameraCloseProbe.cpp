#include "PCH.h"

#include "CameraCloseProbe.h"

#include "OwnView.h"
#include "Settings.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace MTB::CameraCloseProbe {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Defined below, beside the other naming helpers; forward-declared here
        // because the view readout now names the camera state and the readout
        // has to sit next to the struct it prints.
        [[nodiscard]] const char* CameraStateName(int a_id);

        // ⚠ CHECKED FIRST, BY EVERY DOOR. The four hooks that call in here are
        // on the camera update path, which runs every frame of normal play
        // forever. Everything else in this file sits behind this one load, so
        // the cost of carrying the probe with the watch closed is four relaxed
        // reads a frame.
        std::atomic<bool> g_watching{ false };

        // ⚠ THE FIELD SYMPTOMS ARE NOT ABOUT THE NODE, AND THAT IS WHY THREE
        // ROUNDS OF WATCHING THE NODE HAVE NOT EXPLAINED THEM. "The camera
        // can't look up and down" and "input locomotion doesn't follow the
        // camera" are both about the THIRD-PERSON STATE: whether the camera is
        // in its animation-locked mode, whether free rotation is on, and where
        // the player's own pitch and heading sit. A camera root at a perfectly
        // correct position, which is what the verdict has now reported twice,
        // is entirely compatible with every one of them.
        //
        // Every field here is one OwnView saves at arm and writes back at
        // disarm, so a value still wrong after the close is either one it
        // captured wrongly or one something re-parked afterwards, and the
        // samples say which by showing WHEN it changed.
        struct ViewState {
            bool  animCam = false;       // ThirdPersonState::toggleAnimCam
            // ⚠ RENAMED FROM freeRotation, WHICH WAS THE BOOL SITTING ONE
            // TOKEN AWAY FROM THE VECTOR OF THE SAME NAME in the line a user
            // pastes into a bug report. These two are the whole field report:
            // animCam is "cannot look up and down", freeRotEnabled is "can
            // still look left and right". They must never be confusable.
            bool  freeRotEnabled = false;  // ThirdPersonState::freeRotationEnabled
            float freeRotX = 0.0f;         // ThirdPersonState::freeRotation.x
            float freeRotY = 0.0f;         // ThirdPersonState::freeRotation.y
            float targetZoom = 0.0f;
            float currentZoom = 0.0f;
            float pitch = 0.0f;          // player data.angle.x
            float heading = 0.0f;        // player data.angle.z
            // ⚠ THE CAMERA STATE ID JOINS THE STRUCT, so a kFirstPerson left on
            // the stack after a forced-third arm is visible. It was invisible
            // before: the samples printed it, the VERDICT never did.
            int stateId = -1;            // PlayerCamera::currentState->id

            // ⚠ THE FOV IS DELIBERATELY NOT IN HERE. This struct's defaulted
            // operator== is a SAMPLE TRIGGER, and an exact float compare on a
            // field another mod may dither would wake a sample every frame.
            // The FOV keeps its own epsilon trigger and rides the readout
            // below as an explicit argument.
            [[nodiscard]] bool operator==(const ViewState&) const = default;
        };

        [[nodiscard]] ViewState SampleView() {
            ViewState v{};
            if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                if (auto* third = static_cast<RE::ThirdPersonState*>(
                        camera->cameraStates[RE::CameraState::kThirdPerson].get())) {
                    v.animCam = third->toggleAnimCam;
                    v.freeRotEnabled = third->freeRotationEnabled;
                    v.freeRotX = third->freeRotation.x;
                    v.freeRotY = third->freeRotation.y;
                    v.targetZoom = third->targetZoomOffset;
                    v.currentZoom = third->currentZoomOffset;
                }
                // ⚠ THE STATE OBJECT ABOVE IS READ FROM THE SLOT, NOT FROM THE
                // LIVE STATE, exactly as OwnView writes it - so the sample is
                // meaningful even while the player is in first person. This is
                // the id of whatever is actually CURRENT, which is a different
                // question and the reason both are here.
                if (const auto* state = camera->currentState.get()) {
                    v.stateId = static_cast<int>(state->id);
                }
            }
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                v.pitch = player->data.angle.x;
                v.heading = player->data.angle.z;
            }
            return v;
        }

        [[nodiscard]] std::string ViewText(const ViewState& a_v) {
            return fmt::format("animCam={} freeRotEnabled={} freeRot=({:.3f},{:.3f}) "
                               "zoom {:.2f}->{:.2f} pitch={:.3f} heading={:.3f} "
                               "state={}",
                               a_v.animCam, a_v.freeRotEnabled, a_v.freeRotX,
                               a_v.freeRotY, a_v.currentZoom, a_v.targetZoom,
                               a_v.pitch, a_v.heading, CameraStateName(a_v.stateId));
        }

        // ⚠ EVERY FIELD, EVERY TIME, IN->OUT, ON ONE LINE. Not a diff list: a
        // field that came back is evidence too, and its absence from the line
        // reads as "not measured". The reader is a user pasting one line into a
        // bug report, and the question they must not have to ask again is
        // "which one was left changed". One grep for LEFT CHANGED answers it.
        [[nodiscard]] std::string FieldsText(const ViewState& a_in,
                                             const ViewState& a_out, float a_fovIn,
                                             float a_fovOut) {
            const auto chg = [](bool a_changed) {
                return a_changed ? " <<LEFT CHANGED" : "";
            };
            return fmt::format(
                "FIELDS in->out || toggleAnimCam {}->{}{} || freeRotationEnabled "
                "{}->{}{} || freeRotation.x {:.3f}->{:.3f}{} || freeRotation.y "
                "{:.3f}->{:.3f}{} || pitch(data.angle.x) {:.3f}->{:.3f}{} || "
                "heading(data.angle.z) {:.3f}->{:.3f}{} || zoom cur {:.2f}->{:.2f}{} "
                "tgt {:.2f}->{:.2f}{} || worldFOV {:.1f}->{:.1f}{} || cameraState "
                "{}->{}{}",
                a_in.animCam, a_out.animCam, chg(a_in.animCam != a_out.animCam),
                a_in.freeRotEnabled, a_out.freeRotEnabled,
                chg(a_in.freeRotEnabled != a_out.freeRotEnabled),
                a_in.freeRotX, a_out.freeRotX,
                chg(P::ScalarChanged(a_out.freeRotX, a_in.freeRotX, P::kAngleEpsilon)),
                a_in.freeRotY, a_out.freeRotY,
                chg(P::ScalarChanged(a_out.freeRotY, a_in.freeRotY, P::kAngleEpsilon)),
                a_in.pitch, a_out.pitch,
                chg(P::ScalarChanged(a_out.pitch, a_in.pitch, P::kAngleEpsilon)),
                a_in.heading, a_out.heading,
                chg(P::ScalarChanged(a_out.heading, a_in.heading, P::kAngleEpsilon)),
                a_in.currentZoom, a_out.currentZoom,
                chg(P::ScalarChanged(a_out.currentZoom, a_in.currentZoom,
                                     P::kZoomEpsilon)),
                a_in.targetZoom, a_out.targetZoom,
                chg(P::ScalarChanged(a_out.targetZoom, a_in.targetZoom,
                                     P::kZoomEpsilon)),
                a_fovIn, a_fovOut, chg(P::FovChanged(a_fovOut, a_fovIn)),
                CameraStateName(a_in.stateId), CameraStateName(a_out.stateId),
                chg(a_in.stateId != a_out.stateId));
        }

        // ⚠ ONE PREDICATE FOR THE VERDICT WORD AND THE FIELD MARKERS, because
        // they must never be able to disagree. The verdict used to ask
        // ViewState's defaulted operator==, which is a BIT-EXACT float compare,
        // while the markers beside it on the same line are epsilon-gated. So
        // the headline could shout THE CAMERA STATE DID NOT COME BACK WHOLE
        // with not one field flagged, which teaches the reader to distrust the
        // single line we ask a tester to paste.
        //
        // Reachable on any own-view run rather than theoretical: the framing
        // restore snaps currentZoomOffset to targetZoomOffset to kill the exit
        // glide, so a menu opened mid-glide has in.currentZoom != in.targetZoom
        // and comes back "changed" by a hair nobody can see.
        //
        // operator== stays exactly what its comment says it is - a SAMPLE
        // TRIGGER, where an over-eager wake costs one log line and nothing else.
        [[nodiscard]] bool AnyFieldChanged(const ViewState& a_in,
                                           const ViewState& a_out, float a_fovIn,
                                           float a_fovOut) {
            return a_in.animCam != a_out.animCam ||
                   a_in.freeRotEnabled != a_out.freeRotEnabled ||
                   a_in.stateId != a_out.stateId ||
                   P::ScalarChanged(a_out.freeRotX, a_in.freeRotX, P::kAngleEpsilon) ||
                   P::ScalarChanged(a_out.freeRotY, a_in.freeRotY, P::kAngleEpsilon) ||
                   P::ScalarChanged(a_out.pitch, a_in.pitch, P::kAngleEpsilon) ||
                   P::ScalarChanged(a_out.heading, a_in.heading, P::kAngleEpsilon) ||
                   P::ScalarChanged(a_out.targetZoom, a_in.targetZoom, P::kZoomEpsilon) ||
                   P::ScalarChanged(a_out.currentZoom, a_in.currentZoom,
                                    P::kZoomEpsilon) ||
                   P::FovChanged(a_fovOut, a_fovIn);
        }

        // The framing the editor was entered FROM. Kept OUTSIDE the watch
        // state on purpose: it is recorded at the open and has to survive the
        // reset that the close performs.
        struct PreOpen {
            bool         valid = false;
            RE::NiPoint3 translate{};
            // The FOV the menu was entered on, so a close that hands back the
            // wrong one is a number rather than an impression. Captured beside
            // the translation because the field report is BOTH ("our camera and
            // FOV get bugged") and a run that measures one of them cannot say
            // whether the other came back.
            float worldFOV = 0.0f;
            // The third-person state on the way in, which is what the close is
            // supposed to hand back. Declared after ViewState below is not an
            // option, so the type is defined above this struct.
            ViewState view{};
            // ⚠ WHOSE BASELINE, AND HOW OLD. Editor-only, this could not go
            // stale: one open, one close. Widened, a close whose open edge was
            // missed - probes switched on mid-session, a menu already up when
            // the sink registered, a sessionFresh that read false - would grade
            // itself against a baseline from an arbitrarily older menu and
            // print a confident, entirely fictional diff. Stamped so the close
            // can refuse it and say so out loud instead.
            std::string       menuName;
            Clock::time_point stampedAt{};
            // Which configuration this rig is in, captured where it is true.
            // This is the ENTIRE difference between the developer's rig (Show
            // Player In Inventory frames the inventory, so OwnView declines and
            // never writes the flags) and a reporter's, and it turns "which
            // setup is this user on" from a follow-up question into a grep.
            bool ownViewActiveAtOpen = false;
            bool spimPresent = false;
        };
        PreOpen g_preOpen;

        // A baseline older than this is not this menu's. A menu session is
        // minutes; anything beyond an hour is a different play session in the
        // same process.
        inline constexpr float kBaselineCeilingSeconds = 3600.0f;

        // ⚠ THE UI LAYER, SAMPLED BESIDE THE CAMERA. The 2026-08-04 bubbled
        // run answered the camera question and exposed the one this probe
        // could not see: the player re-opened the inventory 2.7s after the
        // close, the bubble ARMED (the log proves it), and the screen showed
        // none of it until the first drag - HUD hidden, a live menu invisible,
        // and a cursor over HUD-less gameplay in the screenshot. The camera
        // columns cannot explain that, so each sample now carries what the UI
        // system thinks is true at the same instant. Declared above State,
        // which keeps the previous frame's snapshot to edge-detect against.
        struct UiSnapshot {
            std::uint32_t stackSize = 0;
            std::uint32_t pauses = 0;
            bool          frozen = false;   // Main::freezeTime
            bool          paused = false;   // UI::GameIsPaused
            bool          hud = false;      // "HUD Menu" open
            bool          cursor = false;   // "Cursor Menu" open
            bool          racemenu = false; // the editor itself still open?
            // ⚠ THE PRIME SUSPECT, promoted to a column of its own. The
            // 2026-08-04 23:17 run showed stack=14 with the HUD OPEN yet
            // invisible, and the player's own diagnosis was "I was actually in
            // the tween menu". Pressing I opens the inventory INSIDE the tween
            // wrapper; the action bar's editor button force-hides only the
            // inventory, so the tween would ride through the whole editor
            // session and be what the close hands back.
            bool tween = false;

            [[nodiscard]] bool operator==(const UiSnapshot&) const = default;
        };

        struct State {
            Clock::time_point closedAt{};
            std::uint32_t     frames = 0;
            std::uint32_t     emitted = 0;
            // Frames the watch was open for but could not measure (no camera or
            // no root). Kept OFF the ledger's own frame count so every rate in
            // the verdict is against frames that could actually have shown
            // something.
            std::uint32_t skippedFrames = 0;
            bool              verdictDone = false;
            std::string       menuName;
            // False is the control run: the menu closed with Menu Studio not
            // covering it. Carried onto every line so the two can never be
            // confused in a log that contains both.
            bool wasBubbled = false;
            // ⚠ THE SHAPE OF THE SESSION, WITHOUT WHICH THE VERDICT LIES BY
            // OMISSION. A dormant session (Skyrim Souls kept the menu live)
            // retires its camera reading the moment it latches, so it hands
            // nothing back - and a verdict that cannot distinguish that
            // from a restore that RAN AND FAILED is pointing at the wrong half
            // of the plugin.
            bool wasArmed = false;
            bool wasDormant = false;
            bool wasLiveOnly = false;
            // Did the rotation park have anything to give back. Its capture is
            // reached only from a LIVE third-person camera while the writer it
            // backstops is not, so a first-person or mounted arm captures
            // nothing and the park is a silent no-op.
            bool parkHadCapture = false;
            // Did we owe the restore at all, sampled at the close edge.
            bool ownViewAtClose = false;

            // The teardown fuse. Set by NoteTeardownDone; the verdict waits on
            // it plus a settle, and prints its absence as a finding.
            bool        teardownSeen = false;
            float       teardownSeconds = 0.0f;
            const char* teardownWhy = "";

            P::Ledger ledger{};

            // Doors that wrote since the previous Tick. Attribution is by
            // INTERVAL, not by position in the frame: whether the engine's
            // camera update happens to run before or after our OnFrame does
            // not matter, only that it landed between two consecutive samples.
            std::uint32_t doorMask = 0;

            // Which view mode the run was made in. The field discriminator is
            // that Scene view reproduces and the Void and the Dressing room do
            // not, and a log holding several runs has to say which is which on
            // its own.
            int viewMode = 0;

            // The transform as the previous Tick left it.
            bool         haveObserved = false;
            RE::NiPoint3 observed{};

            // The FOV as the previous Tick left it. Its own change trigger, so
            // an FOV that moves while the node holds still still emits a line.
            bool  haveFov = false;
            float observedFov = 0.0f;

            // The third-person state as the previous Tick left it, and its own
            // trigger for the same reason: the reported symptoms live here and
            // every one of them can change with the node perfectly still.
            bool      haveView = false;
            ViewState view{};

            // The UI as the previous Tick saw it. ⚠ A UI edge is a sample
            // trigger of its own: the 2026-08-04 run had the inventory OPEN
            // (invisibly) at +2.7s, squarely inside the probe's quiet window,
            // and a camera-only change test slept straight through it.
            bool       haveUi = false;
            UiSnapshot ui{};
        };

        // Plain, not atomic, and the same call about threads StudioCamera's own
        // state makes: the doors and OnFrame are both the main game loop. The
        // close edge is the one caller that could arrive elsewhere, so it
        // resets this BEFORE opening the watch and nothing here is touched
        // while the flag is false.
        State g_s;

        [[nodiscard]] const char* CameraStateName(int a_id) {
            switch (a_id) {
            case RE::CameraState::kFirstPerson:  return "kFirstPerson";
            case RE::CameraState::kAutoVanity:   return "kAutoVanity";
            case RE::CameraState::kVATS:         return "kVATS";
            case RE::CameraState::kFree:         return "kFree";
            case RE::CameraState::kIronSights:   return "kIronSights";
            case RE::CameraState::kFurniture:    return "kFurniture";
            case RE::CameraState::kPCTransition: return "kPCTransition";
            case RE::CameraState::kTween:        return "kTween";
            case RE::CameraState::kAnimated:     return "kAnimated";
            case RE::CameraState::kThirdPerson:  return "kThirdPerson";
            case RE::CameraState::kMount:        return "kMount";
            case RE::CameraState::kBleedout:     return "kBleedout";
            case RE::CameraState::kDragon:       return "kDragon";
            default:                             return "?";
            }
        }

        // The CONFIGURED view mode, not the effective one. A menu that opted
        // out of the space drives declutterMode to a temporary 0, so the
        // effective value would label a Scene view run "Off" and lose the one
        // discriminator the field handed us.
        [[nodiscard]] const char* ViewModeName(int a_mode) {
            switch (a_mode) {
            case 0:  return "Off";
            case 1:  return "Scene view";
            case 2:  return "Void";
            case 3:  return "Dressing room";
            default: return "?";
            }
        }

        [[nodiscard]] RE::NiAVObject* CameraRoot() {
            auto* camera = RE::PlayerCamera::GetSingleton();
            return camera ? camera->cameraRoot.get() : nullptr;
        }

        // How far the node sits from where the editor was entered. Negative
        // means we never captured an entry framing to measure against.
        [[nodiscard]] float DriftFromEntry(const RE::NiPoint3& a_now) {
            if (!g_preOpen.valid) {
                return -1.0f;
            }
            const float dx = a_now.x - g_preOpen.translate.x;
            const float dy = a_now.y - g_preOpen.translate.y;
            const float dz = a_now.z - g_preOpen.translate.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        [[nodiscard]] UiSnapshot SampleUi() {
            UiSnapshot s{};
            if (auto* ui = RE::UI::GetSingleton()) {
                s.stackSize = static_cast<std::uint32_t>(ui->menuStack.size());
                s.pauses = ui->numPausesGame;
                s.paused = ui->GameIsPaused();
                s.hud = ui->IsMenuOpen(RE::HUDMenu::MENU_NAME);
                s.cursor = ui->IsMenuOpen(RE::CursorMenu::MENU_NAME);
                s.racemenu = ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
                s.tween = ui->IsMenuOpen(RE::TweenMenu::MENU_NAME);
            }
            if (auto* main = RE::Main::GetSingleton()) {
                s.frozen = main->freezeTime;
            }
            return s;
        }

        [[nodiscard]] std::string UiText(const UiSnapshot& a_ui) {
            return fmt::format("stack={} pauses={} frozen={} paused={} hud={} "
                               "cursor={} rsm={} TWEEN={}",
                               a_ui.stackSize, a_ui.pauses, a_ui.frozen, a_ui.paused,
                               a_ui.hud, a_ui.cursor, a_ui.racemenu, a_ui.tween);
        }

        // Every open menu, by the engine's own name for it. A count of 14 says
        // something is wrong; only the names say WHAT. Walked from the UI's
        // menu map so nothing register-time is missed, and emitted only at the
        // close and on UI edges, so the cost is a handful of map walks per
        // watch rather than per frame.
        [[nodiscard]] std::string OpenMenuNames() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return "(no UI singleton)";
            }
            std::string out;
            std::uint32_t n = 0;
            for (const auto& [name, entry] : ui->menuMap) {
                if (!ui->IsMenuOpen(name)) {
                    continue;
                }
                if (!out.empty()) {
                    out += ", ";
                }
                out += name.c_str();
                ++n;
            }
            return out.empty() ? "(none)" : fmt::format("{} open: {}", n, out);
        }

        // The doors that wrote in the interval just closed, as text. "nothing"
        // is a real and important answer here, not a missing value.
        [[nodiscard]] std::string DoorsIn(std::uint32_t a_mask) {
            if (a_mask == 0) {
                return "nothing";
            }
            std::string out;
            for (std::size_t i = 0; i < P::kDoorCount; ++i) {
                if ((a_mask & (1u << i)) == 0) {
                    continue;
                }
                if (!out.empty()) {
                    out += " + ";
                }
                out += P::DoorName(static_cast<P::Door>(i));
            }
            return out;
        }

        void EmitVerdict(float a_seconds) {
            const auto& l = g_s.ledger;
            // The run type and the ledger ride in one string so they reach all
            // four verdicts from one place. A verdict that does not say which
            // run produced it is unreadable in a log holding both.
            //
            // ⚠ THE FORMAT STRING STAYS A LITERAL AT THE CALL. fmt v10 parses
            // it at compile time, so lifting it to a `const char*` is a hard
            // error rather than a style choice.
            // ⚠ THE SHAPE OF THE SESSION, FIRST, BECAUSE IT DECIDES WHETHER THE
            // REST MEANS ANYTHING. A dormant session hands nothing back because
            // it never took anything; a session that never owed the restore has
            // no restore to have failed. Reading a verdict without these is how
            // a control run gets diagnosed as a bug.
            const char* shapeText =
                g_s.wasDormant
                    ? "DORMANT (Skyrim Souls kept the menu live, so it never armed "
                      "and nothing of ours framed or restored)"
                : g_s.wasLiveOnly ? "LIVE-ONLY (lighting only, no framing)"
                : g_s.wasArmed    ? "ARMED"
                                  : "never armed";
            const char* oweText =
                g_s.ownViewAtClose
                    ? "ACTIVE at the close, so WE owed the restore"
                    : "off at the close (a view mod covered this menu, or nothing "
                      "framed it, or the restore had already run)";
            const char* parkText =
                g_s.parkHadCapture
                    ? "the rotation park HAD a capture to give back"
                    : "⚠ the rotation park had NOTHING captured (the arm was not "
                      "from a live third-person camera), so its silence is not "
                      "evidence of a clean session";
            const auto teardownText =
                g_s.teardownSeen
                    ? fmt::format("ran at +{:.3f}s ({})", g_s.teardownSeconds,
                                  g_s.teardownWhy)
                    : std::string("⚠ NEVER SIGNALLED: no teardown paid a rotation "
                                  "park this session");
            const auto ledgerText = fmt::format(
                "Session was {}; own view {}; {}; teardown {}. "
                "Run was {} in view mode '{}'. Ledger over {} frames ({} of them with "
                "the node moved): ran/wrote: vtable {}/{}, player site {}/{}, master "
                "site {}/{}, editor camera {}/{}, FOREIGN -/{}. UI at verdict: {}.",
                shapeText, oweText, parkText, teardownText,
                g_s.wasBubbled ? "BUBBLED" : "the CONTROL (not bubbled)",
                ViewModeName(g_s.viewMode), l.frames,
                l.movedFrames, l.Ran(P::Door::kVtable), l.Count(P::Door::kVtable),
                l.Ran(P::Door::kPlayerSite), l.Count(P::Door::kPlayerSite),
                l.Ran(P::Door::kMasterSite), l.Count(P::Door::kMasterSite),
                l.Ran(P::Door::kEditor), l.Count(P::Door::kEditor),
                l.Count(P::Door::kForeign), UiText(SampleUi()));

            // ⚠ THE NUMBER THAT NOW DECIDES IT. The control run proved a frozen
            // node is what a NORMAL close looks like, so where it is frozen is
            // the discriminator, not whether anything wrote.
            auto*       root = CameraRoot();
            const float drift = root ? DriftFromEntry(root->local.translate) : -1.0f;
            auto        driftText =
                drift < 0.0f
                           ? std::string(" No entry framing was captured, so how far the "
                                   "camera sits from where this menu SESSION was "
                                   "entered is unknown for this run.")
                           : fmt::format(" The camera sits {:.1f} units from where this menu "
                                  "SESSION was entered.",
                                  drift);
            // ⚠ THE OTHER HALF OF THE REPORTED SYMPTOM, and it gets its own
            // sentence rather than a column: a run where the node came back and
            // the FOV did not is a completely different search from one where
            // neither did, and the verdict is the line the next session greps
            // for.
            // ⚠ THE LINE THE NEXT SESSION SHOULD READ FIRST. The node and the
            // FOV have both come back clean twice while the field kept
            // reporting a broken camera, so the interesting question is no
            // longer "did the shot return" but "did the state the shot is
            // DERIVED FROM return". Field by field against the way in, with
            // the ones that explain the reported symptoms named.
            auto* camera = RE::PlayerCamera::GetSingleton();
            const float fovNow = camera ? camera->worldFOV : 0.0f;
            if (g_preOpen.valid) {
                const ViewState now = SampleView();
                const bool      whole =
                    !AnyFieldChanged(g_preOpen.view, now, g_preOpen.worldFOV, fovNow);
                // ⚠ THE FIELDS LINE IS UNCONDITIONAL, in both branches. The old
                // shape printed a prose list of names with no numbers on the
                // changed path and a single blob on the clean one, so reading a
                // delta meant cross-referencing two readouts. It also printed
                // no camera state id anywhere and compared the FOV in a
                // separate sentence with its own opinion.
                //
                // ⚠ The verdict word is a VALUE, not a format string. fmt v10
                // parses the format at compile time, so a ternary between two
                // literals in the call is a hard error - the same rule the
                // ledger text above is annotated with.
                const char* wholeText =
                    whole ? " The camera state came back whole."
                          : " ⚠ THE CAMERA STATE DID NOT COME BACK WHOLE.";
                driftText += fmt::format(
                    "{} {}", wholeText,
                    FieldsText(g_preOpen.view, now, g_preOpen.worldFOV, fovNow));
            } else {
                driftText +=
                    " ⚠ NO PRE-MENU BASELINE was captured for this session, so "
                    "nothing above is a comparison. Either the open edge was "
                    "missed, or the baseline belonged to another menu and was "
                    "refused as stale.";
            }
            const char* budget =
                g_s.emitted >= P::Schedule{}.emitBudget
                    ? " Sample budget was exhausted, so the lines above are the "
                      "first second and not the whole window."
                    : "";

            switch (P::Classify(l)) {
            case P::Finding::kEditorStillActive:
                spdlog::warn(
                    "menu camera watch VERDICT +{:.2f}s after '{}' closed: THE "
                    "EDITOR'S OWN CAMERA IS STILL BEING TICKED. RaceSexCamera::Update "
                    "ran {} times AFTER its menu closed and changed the camera root on "
                    "{} of them, so the framing on screen is not merely stale: a "
                    "camera whose menu is gone is still driving it. Next search is why "
                    "that camera keeps updating past the close, NOT the restores.{} {}{}",
                    a_seconds, g_s.menuName, l.Ran(P::Door::kEditor),
                    l.Count(P::Door::kEditor), driftText, ledgerText, budget);
                break;
            case P::Finding::kForeignWriter:
                spdlog::warn(
                    "menu camera watch VERDICT +{:.2f}s after '{}' closed: A WRITER "
                    "WE CANNOT SEE. The camera root moved on {} frames with none of the "
                    "four hooked doors claiming it, so something reaches this node by a "
                    "path this plugin does not stand in. Next search is finding that "
                    "path: the three exit attempts all assumed the writer was one of "
                    "ours.{} {}{}",
                    a_seconds, g_s.menuName, l.Count(P::Door::kForeign), driftText,
                    ledgerText, budget);
                break;
            case P::Finding::kParkedInputs:
                spdlog::warn(
                    "menu camera watch VERDICT +{:.2f}s after '{}' closed: THE "
                    "UPDATE RUNS, THE INPUTS ARE PARKED. The PlayerCamera update body "
                    "ran {} times across {} frames and moved the node {} time(s): "
                    "alive and recomputing every frame, but the camera STATE it reads "
                    "still holds the menu framing, so the recompute keeps producing "
                    "the parked value until player input changes the inputs. The node "
                    "is DERIVED: writing it back was always going to lose. Next search "
                    "is what should restore the state's inputs at close (third-person "
                    "offsets, target, zoom), not the node.{} {}{}",
                    a_seconds, g_s.menuName, P::PlayerCameraRan(l), l.frames,
                    l.PlayerCameraWrites(), driftText, ledgerText, budget);
                break;
            case P::Finding::kEngineRecomputing:
                spdlog::info(
                    "menu camera watch VERDICT +{:.2f}s after '{}' closed: THE "
                    "ENGINE IS ACTIVELY REBUILDING THE SHOT. The ordinary PlayerCamera "
                    "doors wrote on {} of {} frames, a real rate rather than a stray "
                    "write, and nothing foreign or editor-side touched the node. "
                    "Neither the node nor the inputs are stale; if the shot still "
                    "looks wrong, the fault is in WHAT the inputs hold.{} {}{}",
                    a_seconds, g_s.menuName, l.PlayerCameraWrites(), l.frames, driftText,
                    ledgerText, budget);
                break;
            case P::Finding::kNothingWrote:
                spdlog::warn(
                    "menu camera watch VERDICT +{:.2f}s after '{}' closed: THE "
                    "UPDATE BODY IS NOT EVEN RUNNING. No PlayerCamera door executed at "
                    "a real rate (ran {} across {} frames) and the node moved on {} "
                    "frame(s), so the camera update itself is not being called. Next "
                    "search is who stopped calling it, a shape no run has confirmed "
                    "yet, so treat this verdict as news.{} {}{}",
                    a_seconds, g_s.menuName, P::PlayerCameraRan(l), l.frames,
                    l.movedFrames, driftText, ledgerText, budget);
                break;
            }
        }

        // ⚠ THE LINE THAT SAYS WHETHER IT RECOVERED, which no other line
        // answers. The verdict lands about a second after the close and the
        // watch then runs on for the rest of the window collecting the player's
        // own corrective input - the very thing the long fuse exists to catch -
        // and until now all of it was gathered and thrown away. A strand that
        // heals itself and a strand that persists produce identical verdicts and
        // completely different expiry lines.
        void EmitExpirySummary(float a_seconds) {
            if (!g_s.verdictDone) {
                return;  // the verdict above just spoke for the first time
            }
            const auto& l = g_s.ledger;
            auto*       camera = RE::PlayerCamera::GetSingleton();
            const float fovNow = camera ? camera->worldFOV : 0.0f;
            if (!g_preOpen.valid) {
                spdlog::info("menu camera watch: window closed at +{:.2f}s after '{}': "
                             "{} frames observed ({} skipped), node moved on {}, "
                             "PlayerCamera doors ran {}. No baseline, so no comparison.",
                             a_seconds, g_s.menuName, l.frames, g_s.skippedFrames,
                             l.movedFrames, P::PlayerCameraRan(l));
                return;
            }
            const ViewState now = SampleView();
            const bool      whole =
                !AnyFieldChanged(g_preOpen.view, now, g_preOpen.worldFOV, fovNow);
            const char* healed =
                whole ? "RECOVERED by the end of the window: whatever was left "
                        "changed at the verdict is back now"
                      : "⚠ STILL WRONG at the end of the window, so this did not "
                        "heal on its own";
            spdlog::info(
                "menu camera watch: window closed at +{:.2f}s after '{}': {} frames "
                "observed ({} skipped), node moved on {}, PlayerCamera doors ran {}. "
                "State {}. {}",
                a_seconds, g_s.menuName, l.frames, g_s.skippedFrames, l.movedFrames,
                P::PlayerCameraRan(l), healed,
                FieldsText(g_preOpen.view, now, g_preOpen.worldFOV, fovNow));
        }

    }  // namespace

    bool Watching() { return g_watching.load(std::memory_order_acquire); }

    Mark MarkBefore() {
        Mark mark{};
        if (!Watching()) {
            return mark;
        }
        auto* root = CameraRoot();
        if (!root) {
            return mark;
        }
        const auto& t = root->local.translate;
        mark.valid = true;
        mark.x = t.x;
        mark.y = t.y;
        mark.z = t.z;
        return mark;
    }

    void NoteAfter(P::Door a_door, const Mark& a_mark) {
        if (!Watching()) {
            return;
        }
        // ⚠ THE THUNK RAN, WHICH IS A FINDING BY ITSELF. Counted before and
        // independently of the change test: a camera re-asserting one
        // transform every frame writes nothing a compare can see, and that is
        // the most likely shape for something holding a framing in place.
        g_s.ledger.NoteRan(a_door);
        if (!a_mark.valid) {
            return;
        }
        auto* root = CameraRoot();
        if (!root) {
            return;
        }
        // ⚠ THE SECOND TEST IS "DID ORIG CHANGE IT", NOT "DID ORIG RUN". A
        // door that fires every frame and writes back the value already there
        // has not moved anything, and counting that as a write would convict
        // the wrong camera.
        const auto& t = root->local.translate;
        if (!P::Moved(t.x, t.y, t.z, a_mark.x, a_mark.y, a_mark.z)) {
            return;
        }
        g_s.ledger.Note(a_door);
        g_s.doorMask |= 1u << static_cast<std::uint32_t>(a_door);
    }

    void OnBubbleMenuOpened(const std::string& a_menuName, bool a_sessionFresh) {
        if (!Settings::GetSingleton().cameraCloseProbe) {
            return;
        }
        // ⚠ CANCEL FIRST. A close followed by an open inside the switch window
        // was never an exit. Left running, that watch samples straight through
        // the NEXT menu's framing being applied, and worse: Classify ranks the
        // editor door first, so opening the character editor from the action bar
        // while an inventory watch is live would headline the INVENTORY's
        // verdict as "the editor's own camera is still being ticked".
        if (g_watching.load(std::memory_order_acquire)) {
            spdlog::info("menu camera watch: '{}' opened while the watch on '{}' was "
                         "still running: cancelled, that close was a menu SWITCH and "
                         "not an exit.",
                         a_menuName, g_s.menuName);
            g_watching.store(false, std::memory_order_release);
            g_s = State{};
        }
        // ⚠ SESSION-FRESH, NOT OPEN-FRESH, and this is the trap that would make
        // the whole probe lie. OwnView::ApplyFraming returns early while a
        // framing is already up, so on a switch the framing simply stays and the
        // second open sees animCam true, FOV 90, freeRotation.x 2.64. A baseline
        // captured there grades the framing against itself and reports a
        // confident all-clear for the exact bug this exists to find.
        if (!a_sessionFresh) {
            spdlog::debug("menu camera watch: '{}' opened mid-session, baseline kept "
                          "from '{}' (a switch's second open already carries the "
                          "framing, so re-capturing here would measure it against "
                          "itself).",
                          a_menuName, g_preOpen.valid ? g_preOpen.menuName : "nothing");
            return;
        }
        g_preOpen = PreOpen{};
        if (auto* root = CameraRoot()) {
            auto* camera = RE::PlayerCamera::GetSingleton();
            g_preOpen.valid = true;
            g_preOpen.translate = root->local.translate;
            g_preOpen.worldFOV = camera ? camera->worldFOV : 0.0f;
            g_preOpen.view = SampleView();
            g_preOpen.menuName = a_menuName;
            g_preOpen.stampedAt = Clock::now();
            g_preOpen.ownViewActiveAtOpen = OwnView::Active();
            g_preOpen.spimPresent = OwnView::SpimPresent();
            spdlog::info(
                "menu camera watch: '{}' opening from local=({:.1f},{:.1f},{:.1f}) "
                "fov={:.1f} | own view {} | SPIM {} | view state {}: the framing the "
                "close has to get back to.",
                a_menuName, g_preOpen.translate.x, g_preOpen.translate.y,
                g_preOpen.translate.z, g_preOpen.worldFOV,
                g_preOpen.ownViewActiveAtOpen ? "ACTIVE" : "off",
                g_preOpen.spimPresent ? "present" : "absent",
                ViewText(g_preOpen.view));
        }
    }

    void OnBubbleMenuClosed(const std::string& a_menuName, bool a_wasBubbled,
                            bool a_wasArmed, bool a_wasDormant, bool a_wasLiveOnly,
                            bool a_parkHadCapture) {
        if (!Settings::GetSingleton().cameraCloseProbe) {
            return;
        }
        // ⚠ A BASELINE FROM ANOTHER MENU IS WORSE THAN NO BASELINE, because the
        // verdict would print a precise diff of two unrelated moments. Refuse it
        // and let the "NO PRE-MENU BASELINE" branch say so.
        if (g_preOpen.valid) {
            const float age =
                std::chrono::duration<float>(Clock::now() - g_preOpen.stampedAt)
                    .count();
            if (age > kBaselineCeilingSeconds) {
                spdlog::info("menu camera watch: the baseline from '{}' is {:.0f}s old, "
                             "so it is not this session's, refused.",
                             g_preOpen.menuName, age);
                g_preOpen = PreOpen{};
            }
        }
        // Reset BEFORE the flag goes up: no door touches this state while the
        // watch reads closed, so this is the one ordering that needs no lock.
        g_s = State{};
        g_s.closedAt = Clock::now();
        g_s.menuName = a_menuName;
        g_s.wasBubbled = a_wasBubbled;
        g_s.wasArmed = a_wasArmed;
        // ⚠ A SESSION THAT NEVER ARMED HAS NO TEARDOWN TO WAIT FOR, and without
        // this it waits anyway. The fuse is signalled only when the rotation
        // park owed something, and the park's latches are written on the first
        // ARMED tick - so the unbubbled CONTROL close, the Souls-dormant session
        // and the live-only session never signal, fall through to the ceiling,
        // and get graded at +3.0s instead of the +1.0s floor.
        //
        // That is not a cosmetic delay. The control run measured the player's
        // own corrective camera input arriving at +1.74s, and the verdict
        // samples the state LIVE - so a healthy control graded at +3.0s reports
        // the player's mouse as a fault. It also breaks the A/B outright: the
        // bubbled arm and the control arm stop being sampled at the same offset.
        // Souls dormancy is this reporter's own second symptom, so it is exactly
        // the shape that must not be measured late.
        if (!a_wasArmed) {
            g_s.teardownSeen = true;
            g_s.teardownSeconds = 0.0f;
            g_s.teardownWhy = "nothing owed, the session never armed";
        }
        g_s.wasDormant = a_wasDormant;
        g_s.wasLiveOnly = a_wasLiveOnly;
        g_s.parkHadCapture = a_parkHadCapture;
        g_s.ownViewAtClose = OwnView::Active();
        g_s.viewMode = Settings::GetSingleton().declutterModeIni;
        g_watching.store(true, std::memory_order_release);
        spdlog::info(
            "menu camera watch: '{}' closed ({}, view mode '{}'), watching the "
            "camera root, the FOV and the third-person state for {:.0f}s to see what "
            "still drives them. Per-frame for the first {} frames, then only on a "
            "change.",
            g_s.menuName,
            a_wasBubbled ? "BUBBLED, the case under investigation"
                         : "not bubbled, CONTROL run",
            ViewModeName(g_s.viewMode), P::Schedule{}.expireAt,
            P::Schedule{}.denseFrames);
        // The roster at the moment of the close. stack=14 was the finding that
        // forced this line into existence; the count accuses, the names convict.
        spdlog::info("menu camera watch: menus at close: {}", OpenMenuNames());
    }

    void NoteTeardownDone(const char* a_why) {
        // Early-out while the watch is closed, which is also the reason the
        // close edge has to be armed ABOVE the zero-frame sleek cut: on the
        // shipped defaults that cut runs the whole teardown inside the close's
        // own call stack, and a signal arriving before the watch opened is a
        // signal lost.
        if (!g_watching.load(std::memory_order_acquire) || g_s.teardownSeen) {
            return;
        }
        g_s.teardownSeen = true;
        g_s.teardownSeconds =
            std::chrono::duration<float>(Clock::now() - g_s.closedAt).count();
        g_s.teardownWhy = a_why;
    }

    void Tick() {
        if (!Watching()) {
            return;
        }
        // ⚠ THE CLOCK IS READ AND THE FUSE TESTED BEFORE THE NULL CHECK, and
        // the order is not cosmetic. The null-camera early return used to sit
        // above every clock read, so a camera root that stayed null for the
        // length of the window left g_watching true INDEFINITELY - and the four
        // doors kept counting into a stale ledger until some later close reset
        // it. Rare with one menu; routine with four.
        const float seconds =
            std::chrono::duration<float>(Clock::now() - g_s.closedAt).count();
        if (seconds >= P::Schedule{}.expireAt) {
            if (!g_s.verdictDone) {
                EmitVerdict(seconds);
            }
            EmitExpirySummary(seconds);
            g_watching.store(false, std::memory_order_release);
            g_s = State{};
            return;
        }

        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* root = CameraRoot();
        if (!camera || !root) {
            // No camera yet is not a sample; wait rather than record a frame
            // that measured nothing. Counted separately so the ledger's frame
            // denominator is not quietly inflated by frames that measured
            // nothing - a rate is only honest against the frames it could have
            // been observed on.
            ++g_s.skippedFrames;
            return;
        }

        const auto& local = root->local.translate;
        const bool  moved =
            g_s.haveObserved && P::Moved(local.x, local.y, local.z, g_s.observed.x,
                                         g_s.observed.y, g_s.observed.z);
        const std::uint32_t mask = g_s.doorMask;
        g_s.doorMask = 0;

        ++g_s.ledger.frames;
        if (moved) {
            ++g_s.ledger.movedFrames;
            // Moved with nobody owning up to it. The whole point of the mask.
            if (mask == 0) {
                g_s.ledger.Note(P::Door::kForeign);
            }
        }
        g_s.observed = local;
        g_s.haveObserved = true;

        // ⚠ ITS OWN TRIGGER, NOT A COLUMN ON THE NODE'S. The node and the FOV
        // are restored by different writes and can come back at different
        // times, so an FOV that moves while the node holds still has to be able
        // to wake a sample on its own. Never fed to the door ledger: that
        // ledger attributes writes to the camera ROOT, and no door here claims
        // the FOV.
        const float fovNow = camera->worldFOV;
        const bool  fovMoved =
            g_s.haveFov && P::FovChanged(fovNow, g_s.observedFov);
        g_s.observedFov = fovNow;
        g_s.haveFov = true;

        // The third-person state, on its own trigger for the third time and
        // the same reason each time: it can change with the node dead still,
        // and it is where the reported symptoms actually live.
        const ViewState viewNow = SampleView();
        const bool viewMoved = g_s.haveView && !(viewNow == g_s.view);
        g_s.view = viewNow;
        g_s.haveView = true;

        // A UI edge speaks even while the camera holds still - see the note on
        // State::ui. Feeds only the sample trigger, never the camera ledger.
        const auto ui = SampleUi();
        const bool uiChanged = g_s.haveUi && !(ui == g_s.ui);
        g_s.ui = ui;
        g_s.haveUi = true;

        const auto plan = P::PlanTick({ .framesSinceClose = g_s.frames,
                                        .secondsSinceClose = seconds,
                                        .changed = moved || fovMoved || viewMoved ||
                                                   uiChanged,
                                        .verdictDone = g_s.verdictDone,
                                        .emitted = g_s.emitted,
                                        .teardownSeen = g_s.teardownSeen,
                                        .teardownSeconds = g_s.teardownSeconds });
        ++g_s.frames;

        if (plan.sample) {
            ++g_s.emitted;
            const auto* state = camera->currentState.get();
            const int   id = state ? static_cast<int>(state->id) : -1;
            const auto& world = root->world.translate;
            const float drift = DriftFromEntry(local);
            // The FOV against the one the editor was entered on. An absolute
            // number alone cannot say whether the close handed the right one
            // back, and the entry value is the only thing it has to match.
            const auto fovText =
                g_preOpen.valid
                    ? fmt::format("fov={:.1f} (entry {:.1f}, off by {:+.1f}){}",
                                  fovNow, g_preOpen.worldFOV,
                                  fovNow - g_preOpen.worldFOV,
                                  fovMoved ? " CHANGED" : "")
                    : fmt::format("fov={:.1f} (no entry value captured){}", fovNow,
                                  fovMoved ? " CHANGED" : "");
            spdlog::info(
                "menu camera watch: +{:.3f}s f{} | state {} ({}) | "
                "local=({:.1f},{:.1f},{:.1f}) world=({:.1f},{:.1f},{:.1f}) | "
                "drift from entry {:.1f} | {} | moved={} | wrote since last frame: {} "
                "| own view {} | view {}{} | ui: {}",
                seconds, g_s.frames - 1, id, CameraStateName(id), local.x, local.y,
                local.z, world.x, world.y, world.z, drift, fovText,
                moved ? "YES" : "no", DoorsIn(mask),
                OwnView::Active() ? "ACTIVE" : "off", ViewText(viewNow),
                viewMoved ? " CHANGED" : "", UiText(ui));
            // A UI edge is exactly when the roster changes, so name the roster
            // then and there - the 23:17 run had the I-press and the tween exit
            // inside the quiet window, and counts alone could not say what
            // opened or closed.
            if (uiChanged) {
                spdlog::info("menu camera watch: menus now: {}", OpenMenuNames());
            }
        }
        if (plan.verdict) {
            g_s.verdictDone = true;
            EmitVerdict(seconds);
        }
        // plan.expire is no longer consulted here: the expiry test was hoisted
        // to the top of this function so a null camera cannot strand the watch
        // open forever. PlanTick still reports it for the policy's own tests.
    }

    void DropOnLoad() {
        g_watching.store(false, std::memory_order_release);
        g_s = State{};
        g_preOpen = PreOpen{};
    }

}  // namespace MTB::CameraCloseProbe

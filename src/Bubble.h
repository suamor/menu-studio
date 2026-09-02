#pragma once

#include "StudioSessionPolicy.h"

#include <string>
#include <unordered_set>

namespace MTB {
    // The bubble: while a configured menu is open AND the game is actually
    // paused, tick the player's animation graph (and optionally FSMP) with a
    // real dt from our own QPC clock. Frame driver is a write_call<5> on the
    // player-update dispatch call inside Main::Update, so the tick lands right
    // after the engine's own (paused = skipped) player update.
    class Bubble : public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                   public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static Bubble& GetSingleton();

        static void InstallHook();  // SKSEPlugin_Load (after AllocTrampoline)
        static void Register();     // kDataLoaded: menu event sink
        void        ForceReset();   // kPreLoadGame / kNewGame backstop

        // Live bubble condition (menu open + game paused + the studio session
        // entered + enabled) - safe from any main-thread context, no per-frame
        // state required.
        [[nodiscard]] static bool IsBubbleActive();

        // The same condition, keyed on the SHORT visual grace instead of the
        // long gate hold. For anything the player can SEE.
        //
        // ⚠⚠ THE TWO HOLDS ARE NOT INTERCHANGEABLE AND THE STRIP PROVED IT. On
        // the frame a menu closes, menusOpen_ dips to zero, and both counters
        // start so a menu SWITCH (inventory to magic, one or two frames apart)
        // does not tear anything down in the gap. The gate hold is 30 frames
        // (~0.5 s) because the camera gate must survive a switch slower than the
        // visual teardown can; graceFrames_ is 6 (~0.1 s) because that is all a
        // PICTURE needs. The action bar asked IsBubbleActive and therefore
        // inherited the camera's half second: "MS icons take a second to
        // disappear when we exit out the menu, it needs to be instant" (user
        // 2026-08-14). It is a visual, so it takes the visual number.
        //
        // ⚠ NOT ZERO, AND DELIBERATELY. Dropping the hold entirely would blink
        // the strip off and on through every menu switch, which is the bug both
        // counters exist to prevent. Six frames covers the field-measured 52-71
        // ms switch gap and is under a tenth of a second on the way out.
        [[nodiscard]] static bool IsBubbleVisible();

        // The last bubble menu to open. Meaningful while a counted menu is open
        // (OpenMenuCount() > 0), and stale afterwards, since it is never
        // cleared on close. Main thread.
        //
        // ⚠ THE OLD WORDING SAID "while IsBubbleActive()", WHICH IS NOW TOO
        // NARROW. Under bWaitForOwnerContext the strip draws over a menu the
        // studio has NOT entered, and it needs this name to decide which
        // buttons belong in that menu. The count is the honest bound: the name
        // is written on every counted open and nothing else touches it.
        [[nodiscard]] const std::string& CurrentMenuName() const {
            return currentMenuName_;
        }

        // Has the studio session been entered for the menu that is open? The
        // menu session and the studio session are two lifetimes under
        // bWaitForOwnerContext: a menu can be counted, and vanilla on screen,
        // with no studio in it at all.
        //
        // Narrower than IsBubbleActive(), which also asks whether the world is
        // actually frozen. Use this when the question is "did we build the
        // studio", and IsBubbleActive when it is "is the scene up and still".
        [[nodiscard]] static bool StudioSessionEntered() {
            return GetSingleton().studioEntered_.load();
        }

        // Is a counted menu open with the gate holding the studio down? The
        // action bar's weaker predicate: the strip has to keep drawing in this
        // state or it would hide the button that lifts the gate, which would
        // leave the owning mod's hotkey as the only way in.
        [[nodiscard]] static bool StudioHeldForOwnerContext() {
            auto& self = GetSingleton();
            return self.menusOpen_.load() > 0 && !self.studioEntered_.load() &&
                   self.studioGateHeld_.load();
        }

        // An owning mod says its own window is open over the menu, or is not.
        //
        // ⚠ MAIN THREAD ONLY, like every other export. The set is read by the
        // per-frame reconcile and by nothing else.
        //
        // The FIRST live context of a game session is also the proof that arms
        // the gate - see StudioSessionPolicy::HoldsStudioDown. False means the
        // id was null or empty.
        static bool SetOwnerContext(const char* a_ownerId, bool a_active);

        // An owning mod asks that ITS sessions keep the world running: the
        // studio arms in full - camera, lights, backdrop - but takes no
        // force-pause and refuses the dormant latch that normally answers an
        // unpaused world. Fitting Room's ask (2026-08-23): physics must stay
        // visible while styling, which a paused world cannot show. The wish is
        // per owner id and only counts while that owner also holds a live
        // context, so a stale wish from a closed session gates nothing.
        //
        // ⚠ MAIN THREAD ONLY, same as SetOwnerContext. False means the id was
        // null or empty.
        static bool SetOwnerWorldLive(const char* a_ownerId, bool a_live);

        // Does any owner with a LIVE context want the world running? The
        // per-frame arm decision and EnterStudioSession's pause takes read
        // this; nothing else should.
        [[nodiscard]] static bool OwnerWorldLiveWanted();

        // The ONE answer to "is the character's own liveness held this
        // frame". The graph, face and companion ticks read THIS instead of
        // the raw setting.
        //
        // ⚠⚠ bFreezeCharacter IS LITERAL AND STAYS LITERAL (user, r27:
        // "physics is driven fine when the character is frozen too - do the
        // literal freeze for anims but still drive physics"). An r27 build
        // let an owner-live session yield the freeze so hair would swing in
        // the editor; that was solving a problem that did not exist. The
        // FSMP and CBPC drives arm with the bubble and were NEVER gated on
        // this setting, so a frozen character has live hair, cloth and body
        // physics already - it just is not walking about. The override is
        // gone; the only thing that lifts this is the bounded re-settle
        // below, and that exists because a freeze must not hold a pose the
        // menu never caught.
        [[nodiscard]] static bool CharacterFrozen();

        // Is the short post-rebuild re-settle window open? A 3D swap seen
        // while armed (an outfit apply, a race switch) leaves the new
        // skeleton in its bind pose, so the graph is stepped for this many
        // frames even under the freeze - and the PERMANENT halves of the
        // animation hold stand aside for it, exactly as they do for an
        // owner-live session. See lastArmedPlayerRoot_.
        [[nodiscard]] static bool ReposeActive();

        // Has anything published a context yet this game session? The settings
        // panel says so out loud, because a switch that is on and provably
        // doing nothing is worse than one that is off. Main thread.
        [[nodiscard]] static bool OwnerContextProven() {
            return GetSingleton().ownerContextProven_;
        }

        // Preview prompt: the last input device the sink saw, and whether the
        // try-on hint should render right now (armed + inventory-family menu +
        // dressing-room view mode + enabled).
        [[nodiscard]] bool UsingGamepad() const {
            return lastInputDevice_.load() == RE::INPUT_DEVICE::kGamepad;
        }
        [[nodiscard]] bool ShouldShowTryOnPrompt() const;

        // Raw bubble-menu open count (no pause/enable condition) - for
        // tripwires that must distinguish "gate off because no menu" from
        // "gate off DURING a menu" (TRACKER B-4).
        [[nodiscard]] static int OpenMenuCount() {
            return GetSingleton().menusOpen_.load();
        }

        // RaceSexMenu is bubbled EXPERIMENTALLY (F-9): the engine's own
        // racemenu mode already animates the player, so our anim ticks
        // skip, and the right-mouse gate stands down (RaceMenu owns its
        // input).
        // Is a camera drag holding the left mouse? While this is true the menu
        // must not see left-mouse at all: the drag started away from the panel
        // and crossing over one mid-swing should move the camera and nothing
        // else. Read by MenuInputGate's Scaleform gate.
        [[nodiscard]] static bool CameraDragOwnsMouse() {
            auto& self = GetSingleton();
            return self.camOrbitDragging_.load() || self.camOwedLeftUp_.load();
        }

        // Consumes the owed release. Called by the gate once it has eaten one,
        // so a single swing eats exactly one up and ordinary clicking is never
        // touched.
        static void ClearOwedLeftUp() { GetSingleton().camOwedLeftUp_ = false; }

        // Narrower than CameraDragOwnsMouse: true only WHILE the swing is
        // held, without the owed-release tail. The gate eats mouse MOVES on
        // this one - the field found the hidden menu under a swing still
        // hover-tracking the cursor, playing SkyUI's list sounds and flashing
        // item previews as the drag crossed its rows. Moves during the owed
        // tail stay untouched, since by then the cursor is the menu's again.
        [[nodiscard]] static bool CameraDragActive() {
            auto& self = GetSingleton();
            return self.camOrbitDragging_.load() || self.camPanDragging_.load();
        }

        [[nodiscard]] static bool IsRaceMenuOpen() {
            return GetSingleton().raceMenuOpen_.load();
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

        // F-14: the bubble's own drag input - right-mouse deltas accumulate
        // into the preview-spin target while armed (bPreviewSpin).
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override;

        void OnFrame(RE::Main* a_main);

        // The pivot camera's share of one input event. Runs BEFORE the
        // pause-gated early return in ProcessEvent, since the pause lands a few
        // frames after a menu opens and a drag started in that window used to
        // be dropped entirely.
        void HandleCameraInput(RE::InputEvent* a_event);

        // SPIM changes heading/camera and then calls Update3DPosition for each
        // drag event. Neutralize those fields before the original vfunc can
        // publish them, then re-compose and propagate our root rotation.
        void PrepareSpinReassert();
        void ReassertSpin();

    private:
        Bubble() = default;

        // The body both public predicates above share, so the only thing that
        // can differ between them is WHICH hold counter they were handed. Two
        // copies of this condition would drift the first time the studio-session
        // term or the pause test changed under one of them.
        [[nodiscard]] static bool ActiveWithHold(bool a_holdLive);

        void Tick(RE::Main* a_main, bool a_paused);
        void RestoreOwnedRotationState(RE::PlayerCharacter* a_player,
                                       bool a_pinHeading);
        void Disarm();  // armed→idle edge: restore decluttered refs

        // ⚠⚠ THE PARK'S ONE READING OF THE PLAYER'S OWN CAMERA, AND ITS MOMENT
        // IS THE MENU-OPEN EVENT. Called from the open handler while the session
        // is fresh, and again on the first armed tick as a BACKSTOP for a menu
        // that was already up when the plugin armed. The policy in
        // CameraDebtPolicy.h makes the second call a no-op when the first one
        // landed, so both sites can call it unconditionally.
        void TakePreMenuCameraCapture(const std::string& a_menuName,
                                      const char*        a_moment);

        // The studio session, nested inside the menu session. Enter builds
        // everything the bubble owns (pause, camera, space, scene); Leave hands
        // back exactly what Enter took. Reconcile is one frame's decision and
        // is the only caller of either.
        void EnterStudioSession(const std::string& a_menuName, const char* a_why);
        void LeaveStudioSession(const char* a_why);
        // ⚠ NOT PART OF THE Enter/Leave PAIR, ON PURPOSE. Leave is reached from
        // Reconcile alone and a shipped session can end without it; every route
        // that ends one calls this instead. Idempotent, so calling it is never
        // a claim that anything was owed.
        void RestoreHudIfHidden(const char* a_why);
        void ReconcileStudioSession();
        [[nodiscard]] bool GateHoldsStudioDown() const;
        // F-15: force third + full framing when we own the view; a_force
        // (r45) bypasses the coverage check - tick-3 fallback only.
        void ArmOwnViewIfOurs(bool a_force = false);
        // ⚠ THE SECOND WITNESS THAT A MENU OF OURS IS STILL ON SCREEN.
        // menusOpen_ alone cannot answer it: the r19c self-heal reconciles
        // that count to zero against the UI, and a mid-menu teardown can run
        // well after it has. Asks the UI map about every menu we counted, plus
        // the last one we named in case the count was already cleared under us.
        [[nodiscard]] bool UiStillHoldsACountedMenu() const;
        void LogTelemetry(RE::Main* a_main, bool a_paused, float a_dt);
        void CancelDipIfActive();          // F-12 v2: never leave the screen dark
        // F-26: the preview sheathe, past the switch gap. a_force pays it now
        // (paths that return before the timed fire site can ever run again).
        void FireDeferredWeaponRestore(bool a_force = false);
        // External view mods restore from their own close-event sinks. Run
        // after that dispatch, before the first gameplay render.
        void ReconcileExternalViewAfterClose();
        // Open 3: the framed companion as a second rotatable subject. Sync
        // returns true while she owns the drag; the other two are its halves.
        bool SyncSpinCompanion();
        void CaptureSpinCompanion(RE::Actor* a_actor);
        void ReleaseSpinCompanion();

        std::atomic<int>  menusOpen_{ 0 };
        std::atomic<bool> raceMenuOpen_{ false };
        // ── The studio session ─────────────────────────────────────────────
        // Owners currently holding a live context, by id. Main thread only:
        // written by the export, read by the per-frame reconcile.
        std::unordered_set<std::string> ownerContexts_;
        // Owners who asked their sessions to keep the world running
        // (SetOwnerWorldLive). Meaningful only intersected with the live set
        // above; a wish outlives its context harmlessly. Main thread only.
        std::unordered_set<std::string> ownerWorldLive_;
        // ── The re-settle window ───────────────────────────────────────────
        // ⚠⚠ IDENTITY ONLY, NEVER DEREFERENCED, and the type says so. The
        // rebuild this catches FREES the node it replaces, so the stale value
        // is only ever compared, never followed.
        //
        // Field r27: an outfit apply rebuilds the player's 3D from inside an
        // armed menu, and a fresh skeleton starts in its BIND pose. With the
        // freeze on, nothing steps the graph afterwards, so the character
        // holds that A-pose until something else unfreezes it (the user's
        // report: the A-pose survived the apply and only broke when they
        // reopened the editor). "Hold the frame the menu caught" was never
        // meant to hold a pose the menu did not catch, so a swap seen WHILE
        // ARMED opens a short window in which the graph is stepped regardless.
        const void*                     lastArmedPlayerRoot_ = nullptr;
        int                             reposeFrames_        = 0;
        // ⚠ PER GAME SESSION, AND IT NEVER GOES BACK DOWN INSIDE ONE. The gate
        // is armed by proof rather than by configuration, so this is the proof:
        // until something has demonstrated it can lift the studio, the setting
        // has no effect and menus bubble as they always did. Cleared only by
        // ForceReset (load / new game).
        bool              ownerContextProven_ = false;
        // ATOMIC because the input sink and the FLICK draw pass both ask.
        // IsBubbleActive() is read from a MenuControls hook and from the mouse
        // gates, neither of which is the frame thread.
        std::atomic<bool> studioEntered_{ false };
        // The RAW gate answer, cached for the same reason: the owner set above
        // is a plain container and must not be walked off-thread. Refreshed
        // every frame by the reconcile. The menu count and the entered flag are
        // applied by StudioHeldForOwnerContext, so this field means one thing.
        std::atomic<bool> studioGateHeld_{ false };
        // What this studio session TOOK from ForcePause, so its Leave can give
        // back exactly that and never re-derive it from live settings. See the
        // ⚠⚠ on PauseLedger.
        StudioSessionPolicy::PauseLedger studioPauses_;
        // Frames the arm decision may still wait for a pause this session asked
        // for but the engine has not published yet. Set at every fresh Enter,
        // spent only while a hold is outstanding and the world still reads
        // live. See StudioSessionPolicy::PauseStillLanding for what it cost.
        int               studioPauseSettleFrames_ = 0;
        // Enter/leave pairs inside the CURRENT menu session, for the log.
        // Repeated arm and disarm inside one menu is a lifecycle nobody has
        // exercised, so every pair says so out loud.
        std::uint32_t     studioEnterCount_ = 0;
        std::atomic<int>  graceFrames_{ 0 };    // deferred VISUAL teardown (short)
        std::atomic<int>  gateHoldFrames_{ 0 }; // gate continuity through menu switches (long)
        bool              armedLastFrame_ = false;
        bool              airFrozenArm_ = false;  // B-2: armed mid-air → anim tick frozen (vanilla look)
        // A live movement transaction was present at arm. Its direction bits
        // must remain engine-owned; clearing them without an input edge can
        // strand the graph in locomotion after the menu closes.
        bool              preserveDirectionBitsArm_ = false;
        // Was the player MOVING on solid ground when this menu opened? An
        // arm-edge latch, because the pause makes it unknowable afterwards.
        // The optional moving-draw exception only chooses which freeze reason
        // owns a simultaneous weapon transition; movement itself stays intact.
        bool              movingArm_ = false;
        bool              faceCallerSeen_ = false; // 0.7.1 probe: engine face caller (0x3D9440) alive?
        // THE BLINK, MADE COUNTABLE. A natural blink is ~0.2 s once every few
        // seconds, and "hard to tell" was the field verdict three rounds
        // running - which is how a changelog claimed a fix that did not work.
        // These count the engine's OWN blink machine (unk200: 0 wait /
        // 1 closing / 2 opening / 3-4 look-holds) and our mesh bakes, and the
        // disarm edge prints the totals. A number closes what an impression
        // cannot.
        std::uint32_t     faceMeshApplies_ = 0;   // mesh bakes this session
        std::uint32_t     blinkStarts_ = 0;       // machine entered state 1
        std::uint32_t     blinkCompletes_ = 0;    // machine left state 2
        std::uint32_t     blinkCaps_ = 0;         // diagnostic 0.5 s cap applied
        float             blinkLidPeak_ = 0.0f;   // highest composed lid value seen
        int               blinkStatePrev_ = -1;   // last unk200, for edge counting
        bool              faceNodeMissingLogged_ = false;  // one warning per session
        bool              faceHeldLogged_ = false;   // face-hold edge, logged on change
        bool              freezeDeclineLogged_ = false;  // freeze asked for, pause absent
        float             armedHeading_ = 0.0f;   // B-7: body heading snapshot at arm (pinned per tick)
        // B-7 v3 preview spin: SPIM's freeRotation writes are harvested as
        // input, the camera stays parked, and the smoothed yaw is composed
        // onto the player's root node above whatever the graph wrote.
        float             freeRotArm_ = 0.0f;     // camera park value (captured at arm)
        // ⚠ THE VALUE BEFORE ANY FRAMING TOUCHED IT, which freeRotArm_ stops
        // being the moment the framing runs: ArmOwnViewIfOurs deliberately
        // re-adopts it so the park sits on the framed view. That adoption is
        // right for the park and wrong for the teardown, and one field cannot
        // be both. Captured once per session and never re-adopted.
        float             preMenuFreeRot_ = 0.0f;
        bool              preMenuFreeRotValid_ = false;
        bool              freeRotParked_ = false;   // did the framing ever park it?
        float             preMenuPitch_ = 0.0f;
        bool              preMenuPitchValid_ = false;
        // ⚠ THE TWO FLAGS, WHICH THE FIRST CUT MISSED BY FIXING ONLY NUMBERS.
        // toggleAnimCam locks the third-person camera to the animation, which
        // is "the camera can't look up and down"; freeRotationEnabled decides
        // whether movement follows the camera. Both are written by a framing
        // and both were being left behind.
        bool              preMenuAnimCam_ = false;
        bool              preMenuFreeRotEnabled_ = false;
        // ⚠⚠ AND THE STANCE THOSE TWO FLAGS BELONG TO, which is what makes the
        // capture above answerable at all. The engine derives both flags from
        // whether the weapon is out, so they are only ours to hand back while
        // the player is still in the stance we read them in. A menu session is
        // where that changes: the weapon preview draws on open and sheathes
        // after the camera restore, and the gate sheathes outright the moment
        // combat starts. Reasoning and decision in RotationOwnershipPolicy.h.
        bool              preMenuWeaponDrawn_ = false;
        bool              preMenuStanceKnown_ = false;
        // ⚠ AND THE FIELD OF VIEW, the last of the set. OwnView saves it and
        // restores it, and OwnView only arms when no view mod already frames
        // the menu - so an inventory covered by Show Player In Inventory runs
        // its framing at FOV 60 with nobody holding the gameplay value, and
        // the view stays narrow long after the menu is gone.
        float             preMenuFov_ = 0.0f;
        bool              preMenuFovValid_ = false;
        // ⚠⚠ CONTEXT FOR THE READING, LOGGED AND NEVER RESTORED. The 2026-08-31
        // field report names three suspects at once and the log as it stood
        // could not separate them: "when I enter combat and press Tab to go
        // into the inventory or magic menu and then exit, the camera gets
        // stuck... it doesn't happen outside of combat... the camera will zoom
        // in really fast... if I use the hotkey, none of this happens". That is
        // the uncaptured zoom, the two flags read in a DRAWN stance, and a menu
        // reached through the tween menu, and each of the three would look
        // identical in the log. These record what was true at the reading so
        // the close can print a before and after for all three.
        //
        // ⛔ THIS IS NOT A CAPTURE OF THE ZOOM. Handing the zoom back is a
        // separate decision that has not been made; storing it is only how we
        // find out whether making it would have helped. Nothing reads these
        // back into the camera, and nothing should start without that decision.
        float             preMenuZoomCur_ = 0.0f;
        float             preMenuZoomTgt_ = 0.0f;
        bool              preMenuInCombat_ = false;
        bool              preMenuViaTween_ = false;   // tween menu up at the reading
        int               preMenuCameraState_ = -1;   // RE::CameraState, or -1
        // ⚠⚠ THE PLAYER'S CAMERA FROM BEFORE THE TWEEN MENU TOOK IT. Tab opens
        // the tween menu, the tween menu takes the camera (kTween, its own field
        // of view), and the covered menu opens over it, so the reading above,
        // taken at that menu's open event, holds the TWEEN's numbers on the Tab
        // route. Field 2026-09-02, the reporter's diag3 log: 4 of 4 readings
        // 'tween menu UP', and the close handed kTween back into a state whose
        // menu was gone, STILL WRONG at +8.01s. This slot is read at the tween
        // menu's own open edge, carried into the reading above when a covered
        // menu opens over a live tween, and dropped when the tween closes. It is
        // never paid directly: the debt machinery sees one capture, exactly as
        // measured. Reasoning in CameraDebtPolicy.h.
        struct PreTweenReading {
            bool  valid = false;
            int   cameraState = -1;  // RE::CameraState, or -1
            float fov = 0.0f;
            float freeRot = 0.0f;
            float pitch = 0.0f;
            bool  animCam = false;
            bool  freeRotEnabled = false;
            float zoomCur = 0.0f;
            float zoomTgt = 0.0f;
            bool  stanceKnown = false;
            bool  weaponDrawn = false;
        };
        PreTweenReading   preTween_;
        void              SamplePreTween();
        // ⚠⚠ THE LAST GAMEPLAY FRAME, SAMPLED EVERY FRAME. Field 2026-09-02 on
        // this rig: the tween menu's open event reaches us with the camera
        // ALREADY in kTween at the tween's field of view (`fov 90.0, camera
        // kTween` at the edge), so a reading at that edge is the tween's too.
        // No event edge is early enough. So the reading is taken continuously
        // instead: every frame with no counted menu, no framing, no switch gap
        // and no tween state, and the tween's open edge copies the last one.
        // Ordering stops mattering. Same shape as preTween_, cleared on load.
        PreTweenReading   lastGameplay_;
        void              SampleGameplayCamera();
        // ⚠⚠ THE FROZEN CAMERA, AND IT IS THE CAPTURE AND NOT THE PAYMENT.
        // Every field above is read once per MENU session, at the MENU-OPEN
        // EVENT, before a framing touches the camera. The first armed tick was
        // the old moment and it is a frame too late: a view mod frames the menu
        // from its own open sink, so by then the reading is the framing's. The
        // measurement is in Bubble::TakePreMenuCameraCapture, which is still
        // called from the arm as a backstop for a menu that was already up.
        // Disarm used to clear all of
        // them - and Disarm is reached with the menu still on screen (a settings
        // save in the panel, the dormancy latch, a missing-3D frame, a switch),
        // after which the next tick re-armed and read the reading back off the
        // FRAMED camera. The close then handed the menu's own numbers to
        // gameplay: free rotation on with the angle standing still, which is a
        // camera that no longer turns with the character. Reasoning, the
        // measurement and the decision table are in CameraDebtPolicy.h.
        //
        // This latch says "this menu session has already read one", which is the
        // same statement as "a framing has run since". It is cleared in exactly
        // one place: beside the capture flags, on a teardown the policy calls a
        // genuine close, and in ForceReset.
        bool              captureTakenThisSession_ = false;
        // One line per menu session however many times a mid-menu teardown runs.
        // Disarm can be reached repeatedly inside one session and the keep is
        // the healthy answer, so it must not be able to fill the log.
        bool              captureKeptLogged_ = false;
        // ⚠⚠ THE CAMERA DEBT WATCH (2026-08-30, the two frozen-camera reports).
        // REPORT ONLY: nothing below is ever read back into the camera, so a
        // reading taken with these fields is a reading of the fault and not of
        // a repair. Disarm clears the capture above, and it is reached with the
        // MENU STILL OPEN by at least two routes - a settings save inside the
        // panel (the field log has the teardown 1.77 s ahead of the close) and
        // the dormancy latch under an unpaused menu, which is the "especially
        // during fights" half of the report. Everything the framing writes
        // after that point has nothing left to hand it back, so these hold what
        // the spent debt WOULD have been worth and the close edge prints the
        // difference. Cleared by ForceReset alone: the whole question is what
        // survives a teardown, so a teardown must not be able to clear it.
        bool              debtSpentEarly_ = false;
        std::uint64_t     debtSpentQpc_ = 0;  // how stale the capture was at the close
        float             debtShadowFov_ = 0.0f;
        float             debtShadowFreeRot_ = 0.0f;
        float             debtShadowPitch_ = 0.0f;
        bool              debtShadowAnimCam_ = false;
        bool              debtShadowFreeRotEnabled_ = false;
        // ⚠ THE CLOSE-EDGE SAMPLE, which decides whether the park's payment is
        // still the park's to make. The deferred exit (Space around you Off /
        // Scene view) pays ~150 ms into live gameplay, AFTER the view mod that
        // framed the menu has restored its own writes - a field that moved
        // between this sample and the payment was paid by its owner, and our
        // copy is stale whether it was clean or poisoned. Sampled in the close
        // event, above the zero-frame cut, so an in-dispatch payment compares
        // a value against itself and the guard is a no-op on every healthy
        // path.
        bool              atCloseViewValid_ = false;
        bool              atCloseAnimCam_ = false;
        bool              atCloseFreeRotEnabled_ = false;
        float             atCloseFreeRotX_ = 0.0f;
        float             atClosePitch_ = 0.0f;
        float             atCloseFov_ = 0.0f;
        float             spinTarget_ = 0.0f;     // accumulated drag intent (radians)
        float             spinYaw_ = 0.0f;        // eased value applied to the node
        bool              spinBasisValid_ = false;
        std::atomic<bool> spinDragging_{ false };  // F-14: right mouse held (input sink)
        std::atomic<float> spinStickX_{ 0.0f };    // F-14 v3: right-stick X, direct (no hold button)
        // The pivot camera's two drags. LATCHED ON THE PRESS EDGE: the zone is
        // tested once, when the button goes down, and that answer owns the whole
        // drag until release. Re-testing per frame would hand the drag back the
        // moment the cursor crossed out of the zone, which is exactly what a
        // player does when they orbit hard in one direction.
        std::atomic<bool> camOrbitDragging_{ false };  // left mouse, inside the zone
        // Middle mouse held from inside the zone: a drag pans, a motionless
        // press-release keeps the recentre click. camPanTravel_ accumulates
        // the hand's motion to tell the two apart; input thread only.
        std::atomic<bool> camPanDragging_{ false };
        float             camPanTravel_ = 0.0f;
        // Middle-pressed evidence since the last tick, the twin of
        // camLeftEvidence_: a latched pan with no evidence for two ticks is
        // over. Held buttons repeat every frame, so a real hold cannot look
        // like this.
        std::atomic<bool> camMiddleEvidence_{ false };
        int               camPanSilentTicks_ = 0;  // tick-side only
        // ⚠ ONE RELEASE OWED TO THE CAMERA. Our input sink clears the drag on
        // the button-up, but Scaleform sees that same release later through the
        // UI message queue. Without this the menu would receive a click on
        // whatever the cursor happened to be over when a swing ended, which is
        // the one way a camera drag can still change something.
        std::atomic<bool> camOwedLeftUp_{ false };
        // ⚠ THE HOLD'S DECISION WAS MADE, edge or ghost. FLICK hooks the input
        // dispatch itself (Hooks::ProcessInputQueue in its PDB) and swallows
        // every event while a window without kPassInputToGame is up. Fitting
        // Room's editor grants that flag only while a button is ALREADY held,
        // so the press EDGE dies inside the blocked frame and only the held
        // repeats reach this sink - the edge-latch alone left FR's camera dead
        // while ordinary menus worked. A repeat with no edge seen this hold is
        // that swallowed press, and the FIRST one carries the zone decision.
        // Latching the decision (not the repeat) keeps the fix that stopped
        // per-frame re-testing from killing swings that cross the menu.
        std::atomic<bool> camHoldDecided_{ false };
        // Left-pressed evidence since the last tick, because FLICK can swallow
        // the RELEASE the same way (passthrough drops the frame the button
        // opens). A latched drag with no evidence for two ticks is over.
        std::atomic<bool> camLeftEvidence_{ false };
        int               camSilentTicks_ = 0;  // tick-side only
        std::atomic<RE::INPUT_DEVICE> lastInputDevice_{ RE::INPUT_DEVICE::kKeyboard };  // preview prompt label
        std::atomic<int>  inputEvidence_{ 0 };     // F-14 v3: raw input events left to log this arm
        std::string       currentMenuName_;        // last-opened bubble menu (own-view coverage)
        // Did THIS session hide the HUD? Recorded rather than re-derived, so a
        // session that never hid it cannot hand back a compass the player had
        // switched off with something else.
        //
        // ⚠⚠ THIS OUTLIVES THE STUDIO SESSION FLAG ON PURPOSE. studioEntered_
        // is written false by three routes that never reach LeaveStudioSession,
        // so a debt tied to it is a debt that can be dropped. This one is only
        // ever cleared by RestoreHudIfHidden (which pays it) or by ForceReset
        // (where the movie holding it is already gone).
        bool              hidHud_ = false;
        RE::NiMatrix3     rootBaseRotate_{};      // root local rotation at arm (pin heading)
        // r54: spin the HORSE with the rider (same yaw; a mounted rider
        // shares the horse's x,y, so a z-yaw about each node's own origin
        // keeps them together). Captured at arm, restored at disarm.
        RE::NiPointer<RE::NiAVObject> spinHorseRoot_;
        RE::NiMatrix3     horseBaseRotate_{};
        // Open 3: the framed companion, same shape as the mount above with one
        // difference that drives the whole design - she can be NAMED and
        // UNNAMED while the menu is already open, because Fitting Room sets her
        // when the user picks a follower. So her basis is captured on the tick
        // her handle changes, not at arm.
        //
        // ⚠ THE ID IS NOT A CONVENIENCE. Acquire and release must key on the
        // same state, so the un-spin writes back through the STORED node
        // pointer and never re-resolves "whoever is the companion now" - a
        // release that resolved live would un-spin the wrong actor the moment
        // the selection changed, and leave the previous one spun forever.
        RE::NiPointer<RE::NiAVObject> spinCompanionRoot_;
        RE::NiMatrix3     companionBaseRotate_{};
        std::uint32_t     spinCompanionId_ = 0;  // who companionBaseRotate_ belongs to
        int               dipPhase_ = 0;          // F-12: 1 = black since open, fade-in due at arm
        int               dipHoldFrames_ = 0;     // F-12 v4: hold black N frames post-build, then reveal
        int               exitPhase_ = 0;         // F-12 v4 exit: 0 none, 1 hold (switch window), 2 fading to black
        std::uint64_t     exitQpc_ = 0;
        bool              pendingOwnViewRestore_ = false;  // F-15 r35: view restore deferred past the switch gap
        std::uint64_t     ownViewRestoreQpc_ = 0;
        bool              pendingExternalViewReconcile_ = false;
        bool              pendingWeaponRestore_ = false;  // F-26: preview sheathe deferred past the switch gap
        std::uint64_t     weaponRestoreQpc_ = 0;
        std::uint32_t     appliedRevision_ = 0;  // settings revision the studio look was built from
        int               appliedMode_ = 0;      // view mode the current culls were swept under (B-5)
        // r19c: the covered menus we actually COUNTED at their open event. The
        // close path decrements against this, never against the live predicate -
        // see the note at the menu event handler.
        std::unordered_set<std::string> countedMenus_;
        int               orphanFrames_ = 0;  // consecutive frames counted-but-not-actually-open
        bool              loggedUnpausedOnce_ = false;
        // r18: the arm decision is LATCHED per menu session, never re-read from
        // the global pause each frame. See the note in OnFrame.
        bool              sessionDormant_ = false;
        // r28b: the force-pause value the CURRENT session decided against. r18
        // latches the arm decision per session so an incidental pause change
        // cannot rebuild the scene ten times, and that is still right - but a
        // user ticking "keep these menus unpaused" IN the panel is not
        // incidental, and the panel lives inside the very menu whose session
        // did the latching. Without this the toggle silently does nothing until
        // you close and reopen, which is how it was reported.
        bool              lastForcePause_ = true;
        // r28g: latched at open. TRUE = this session is a menu Skyrim Souls
        // keeps live and the studio entered it for LIGHTING ONLY - never
        // paused, never armed, rig + colour filter and nothing else. Exists
        // because ShouldBubbleMenu drops Souls-live menus at the entry point,
        // which made every fresh open invisible to the r28 live-studio path:
        // the code lived in a dormant branch only reachable while a session is
        // counted, and no session was ever created. Two field runs produced
        // 25-line stub logs - the fingerprint of that silent early return.
        bool              sessionLiveOnly_ = false;
        // ⚠ THE INSPECT WATCH, AND IT EXISTS BECAUSE TWO MECHANISMS HAVE NOW
        // BEEN AIMED AT THIS AND MISSED. The first blanked a user event the
        // wheel does not carry; the second refused the notch at
        // Inventory3DManager and the item still entered inspect. Neither could
        // be told apart from "the gate works" without a line that says WHEN the
        // engine's zoom left zero and whether a wheel notch had just happened.
        // Input thread writes the stamp, the frame thread reads it.
        std::atomic<std::uint64_t> wheelSeenQpc_{ 0 };
        float             lastZoomProgress_ = 0.0f;
        std::uint32_t     inspectOpensLogged_ = 0;
        std::uint64_t     lastQpc_ = 0;
        std::uint64_t     armedTicks_ = 0;
        std::uint64_t     telemetryCountdown_ = 0;
    };
}

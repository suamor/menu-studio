#include "PCH.h"

#include "StudioCamera.h"

#include "ActionBar.h"
#include "Bubble.h"
#include "CameraCloseProbe.h"
#include "MountedSubjectPolicy.h"
#include "Offsets.h"
#include "OwnView.h"
#include "Settings.h"
#include "StudioCameraPolicy.h"
#include "VersionCheck.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace MTB::StudioCamera {

    namespace {

        namespace P = StudioCameraPolicy;
        namespace CCP = CameraCloseProbePolicy;
        namespace MSP = MountedSubjectPolicy;

        struct State {
            bool armed = false;

            // Nothing below is meaningful until captured is true, and nothing is
            // written to the camera before that either. See the header.
            bool captured = false;

            RE::NiPoint3  pivotBase{};   // subject node at capture, world space
            RE::NiPoint3  originalTranslate{};
            RE::NiMatrix3 originalRotate{};
            P::Orbit      base{};        // the framing we found, for the log
            float         minDistance = P::kMinDistance;  // this subject's own floor

            // ⚠ THE ONE THING CAPTURED FOR GOOD. It carries whatever axis
            // convention the engine's camera rotation uses, worked out once from
            // the shot we found, so every later frame is built as
            // basis(offset) * correction and never refers to the engine's
            // rotation again.
            //
            // That independence is not tidiness. The 2026-08-04 field run had
            // Show Player In Inventory owning these menus and rewriting the
            // camera EVERY FRAME: "the camera moved without us" fired in three
            // sessions out of four, 18ms after arming. Composing onto a rotation
            // that another mod reauthors each frame means the orbit is measured
            // against a moving reference and wobbles.
            P::Mat3 correction{};
            // 0 at capture, walking to 1 over kMountAimBlend seconds. Only a
            // mounted subject moves it; on foot the captured pair is honest and
            // there is nothing to walk away from.
            float mountAimBlend = 1.0f;

            // Targets the player is driving toward, and the eased values that
            // actually reach the camera. orbitTarget's distance field is dead
            // since the track took distance over; yaw and pitch still live
            // here.
            P::Orbit orbitTarget{};
            P::Orbit orbit{};
            float    heightTarget = 0.0f;
            float    height = 0.0f;
            float    lateralTarget = 0.0f;
            float    lateral = 0.0f;

            // The zoom track (docs/specs/2026-08-05-zoom-track-camera.md).
            // trackTarget is what the wheel and the callers drive; trackT is
            // the ONE eased value everything derives from - distance as a
            // log-lerp between the subject's floor and the soft boundary,
            // pivot height from the keyframes - so the two cannot fall out of
            // phase. trackT0 is the opening keyframe position, the framing
            // the menu opened with: a focus round returns there and recentre
            // restores it.
            float trackT = 0.0f;
            float trackTarget = 0.0f;
            // ⚠ THE EDITOR'S OPENING SHOT, DEFERRED BY A FRAME ON PURPOSE.
            // Writing trackTarget on the menu-open edge was not enough: the
            // log proved EnterEditorShot ran and the camera still opened wide,
            // because the same edge runs OwnView's framing and whatever the
            // menu we came FROM does on its way out, and one of them writes
            // this field after us. Consumed at the top of Tick instead, which
            // is after that churn has settled.
            bool  pendingEditorShot = false;
            float trackT0 = 0.0f;
            // Wheel travel taken before capture, in track units. The same
            // park the old delta-distance carried: pre-capture there is no
            // framing to stand on, so the ask waits and Capture folds it
            // onto the opening position.
            float preCaptureZoom = 0.0f;

            // The body-middle anchor, eased like the pivot so a breathing
            // chest drifts the composition instead of shaking it. When the
            // bones cannot answer, every keyframe sits on the face anchor and
            // the track is flat - the proven pre-law shot.
            float bodyMidEased = 0.0f;
            // The subject's feet-to-crown height, measured ONCE per subject
            // and then HELD. Negative means not yet measured. Lengths do not
            // bow: anchors built from the origin plus a held length stand
            // still while the head tracks the camera - the live crown bowed
            // the whole composition with her.
            float bodyHeight0 = -1.0f;
            // How far the ground sits below the subject's own ref origin, from
            // the same instant as the height above. Zero on foot, where the
            // origin IS the ground. Mounted the origin is the saddle and this
            // carries the drop to the hooves, which is the difference between
            // framing a rider and framing a 51-unit torso. See
            // MountedSubjectPolicy.h.
            //
            // ⚠ A DROP RATHER THAN A WORLD Z ON PURPOSE. Every consumer still
            // reads the live position exactly as it always did and adds this;
            // holding an absolute ground would freeze the anchors against a
            // subject that moved.
            float groundDrop0 = 0.0f;
            // Whether the ground above came off a mount. Not "is the player
            // mounted": a mount that was refused leaves this false, and the
            // far stop below must not be widened for a subject that was
            // measured from a saddle anyway.
            bool groundFromMount0 = false;
            // How high she stood when that height was measured. A step in
            // this is a change of footwear, which is the one thing that
            // makes the held measurement wrong. See StandHeight.
            float standHeight0 = 0.0f;
            // The last body-anchor outcome logged, so a found-then-missing
            // flip (the editor's head rebuild) reports once instead of per
            // frame.
            std::string loggedCrown;

            // The pivot actually applied, eased toward the live reading so a
            // breathing chest or a spun-around head nudges the shot instead of
            // shaking it. Seeded at capture so the untouched-arm identity
            // (applied == found, byte for byte) still holds on frame one.
            RE::NiPoint3 pivotEased{};
            bool         pivotSeeded = false;
            // A focused part's sideways offset from the subject's axis,
            // eased the same way and faded out as the shot widens.
            RE::NiPoint3 lateralEased{};

            // Has the player framed this shot by hand since the last reset?
            // Fitting Room asks, so a page change can keep a framing the
            // player built and drop one they never touched.
            bool panned = false;

            // A focus landed on a part that reads best in profile, and the
            // swing round to it is still owed. Held rather than done on the
            // spot because the direction comes from the live bones, which
            // only the tick has, and consumed ONCE so the player can orbit
            // away afterwards without the camera arguing.
            bool pendingSideView = false;

            // Is the yaw the camera is sitting at one WE put there with a side
            // view swing, rather than one the player dragged to?
            //
            // ⚠ THE SWING HAS TO BE UNDOABLE OR IT LEAKS ONTO THE NEXT PART.
            // Focusing a hand swings the shot round to that side, which is
            // right for a hand; focusing the head afterwards moves the pivot
            // and nothing touches the yaw, so the head arrives in the profile
            // the hand asked for and the player is looking at an ear (field
            // 2026-08-07, Fitting Room: "if i go to hands, camera rotates and
            // we see the hands from the side, but then if i go to the helmet we
            // go to the head but are at the same side profile rotation").
            //
            // ⚠ AND ONLY WHEN IT IS STILL OURS. A player who orbited by hand
            // after the swing has said where they want to look, and a focus
            // change is not permission to overrule that. AddOrbit clears this,
            // so the undo below can only ever take back the camera's own move.
            bool autoYaw = false;

            // What we last wrote, so a transform we did not author can be told
            // from one we did. See the note in Tick.
            RE::NiPoint3  lastWritten{};
            RE::NiMatrix3 lastWrittenRotate{};
            bool          haveLastWritten = false;

            // Was the shot taken in the character editor? That menu restores
            // its own camera and OwnView restores the framing, so our revert
            // has nothing left to undo there and actively harms - see Disarm.
            bool capturedInEditor = false;

            // A pivot node another mod asked for, overriding the player's
            // configured one until it clears it. Empty means nobody has.
            std::string focusNode;
            // The sphere the focus node's own geometry measured, in that
            // node's local frame, or radius 0 for "this focus is a plain bone
            // and behaves exactly as it always did". Measured once and then
            // carried, never re-walked per frame.
            P::Sphere focusBound{};
            // ⚠ THE MEASUREMENT IS DEFERRED, AND NOT AS AN OPTIMISATION. The
            // API call has no actor: the subject is threaded through Capture
            // and Apply as a parameter and there is no accessor for it here.
            // So FocusOnAttachment can only record the WANT, and the first
            // Apply with a live subject pays it. That also happens to be more
            // honest than measuring on the call would have been, because a
            // weapon's 3D need not be attached on the frame the caller asks -
            // field-proven 2026-08-07, where the first probe on a weapon row
            // measured nothing and the second, seconds later, measured 37.45.
            bool  focusMeasurePending{ false };
            float focusFallbackCloseness{ -1.0f };
            // The last pivot name whose resolution was logged, so the per-tick
            // ResolvePivot reports each node once instead of per frame.
            std::string loggedPivotNode;
            // A focus closeness asked for before there was a framing to ease
            // from, already mapped onto the track. Negative means none
            // pending. The park itself predates the track and stays: a
            // pre-capture ask must fire at capture, not be dropped.
            float pendingFocusT = -1.0f;

            // Who the shot was captured against. Fitting Room switches the
            // edited character mid-menu, which moves the pivot to a different
            // body while the distance and angles still describe the last one.
            std::uint32_t subjectId = 0;

            // Per-arm counters, reported once at disarm. "Zoom does not work"
            // and "zoom works and moves the camera four units" are the same
            // sentence from outside, and only these separate them. Deliberately
            // NOT a screen readout: the last one of those was rightly hated.
            std::uint32_t wheelSeen = 0;
            std::uint32_t wheelTaken = 0;
            std::uint32_t dragFrames = 0;
            // ⚠ SPLIT BY REASON, because the single total was useless. A high
            // refusal count is mostly ORDINARY MENU CLICKING: every left press
            // outside the drag region counts, so a busy session in an editor
            // reads as hundreds of refusals with nothing wrong. Only the
            // breakdown says whether the cursor could not be read, the region
            // rejected it, or a FLICK window owned it.
            std::uint32_t refusedNoCursor = 0;
            std::uint32_t refusedOutside = 0;
            std::uint32_t refusedOverWindow = 0;
            std::uint32_t refusedItemPreview = 0;
            // WHERE the region turned drags away, so "it refused me" can be
            // told from "I clicked in the item list, which it should refuse".
            float refusedUMin = 2.0f;
            float refusedUMax = -1.0f;
            float         distanceLow = 0.0f;
            float         distanceHigh = 0.0f;
            // ⚠ HOW FAR THE SHOT ACTUALLY TURNED. Every summary so far has read
            // "distance ran X to X" and been taken as proof of nothing moving,
            // when orbiting does not touch distance at all. Without these,
            // "input arrived and the camera moved" and "input arrived and
            // nothing happened" are the same line.
            float         yawLow = 0.0f, yawHigh = 0.0f;
            float         pitchLow = 0.0f, pitchHigh = 0.0f;
            std::uint32_t orbitEvents = 0;

            // ⚠ THE RE-ASSERT LEDGER, split by path. The first re-assert build
            // had NO counter at all, and its field run produced a log that
            // could not distinguish "the hook never fired" from "it fired and
            // changed nothing" - a wasted round on exactly the question a
            // counter answers for free. foreignFrames counts every tick that
            // found the camera moved from under us (the old detector logged
            // once and went quiet), so the next log says whether anything
            // still writes after ALL of our re-stamps.
            std::uint32_t reassertVtable = 0;
            std::uint32_t reassertPlayerSite = 0;
            std::uint32_t reassertMasterSite = 0;
            std::uint32_t foreignFrames = 0;
            // ⚠ editorCameraSuppressed WAS HERE AND IT WAS LYING. It counted
            // how often we stood the character editor's own camera down, and
            // after the 2026-08-06 retreat nothing increments it - our camera
            // never arms in that menu now. It kept printing "Stood the
            // character editor's own camera down 0 times" in every disarm
            // summary, which reads as a failing intervention rather than an
            // absent one. Removed with the clause in the summary.

            bool loggedCapture = false;
            bool loggedForeignWrite = false;
            // The live framing probe: one reading a second into the arm, once.
            bool          loggedLiveFraming = false;
            std::uint32_t liveFramingTicks = 0;
        };
        State g_s;

        // Set by FocusOnAttachment in the instant before it calls FocusOnNode,
        // and consumed there. A latch rather than an argument because
        // FocusOnNode's signature is the public API's, and rather than a State
        // field because it is answered and spent inside one call.
        //
        // It exists for the side view alone: a blade seen head on is an edge,
        // the same reason a hand is a knuckle-width silhouette, and the node an
        // attachment names ('WEAPON', a sheath) is not in kSegments so the
        // name-based test cannot know it wants the swing.
        bool g_nextFocusIsAttachment = false;

        [[nodiscard]] bool AnyRequest() {
            // A parked focus ask counts: it is a caller asking for a shot,
            // and without it here the capture it needs never happens and the
            // request sits unapplied forever. So does parked wheel travel,
            // which is what makes a first notch capture at all.
            return g_s.pendingFocusT >= 0.0f || g_s.preCaptureZoom != 0.0f ||
                   g_s.orbitTarget.yaw != g_s.orbit.yaw ||
                   g_s.orbitTarget.pitch != g_s.orbit.pitch ||
                   g_s.heightTarget != g_s.height || g_s.lateralTarget != g_s.lateral;
        }

        [[nodiscard]] P::Vec3 ToPolicy(const RE::NiPoint3& a_p) {
            return { a_p.x, a_p.y, a_p.z };
        }

        [[nodiscard]] P::Mat3 FromEngine(const RE::NiMatrix3& a_m) {
            P::Mat3 out;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    out.m[i][j] = a_m.entry[i][j];
                }
            }
            return out;
        }

        [[nodiscard]] RE::NiMatrix3 ToEngine(const P::Mat3& a_m) {
            RE::NiMatrix3 out;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    out.entry[i][j] = a_m.m[i][j];
                }
            }
            return out;
        }

        // The crown from bones: the head magic node when the skeleton has
        // one, else the head joint extrapolated up by one neck-to-head
        // segment. Shared by the body-middle anchor and the head's focus
        // anchor so the two cannot disagree about where the head ends.
        [[nodiscard]] bool CrownFromBones(RE::Actor* a_subject, float a_headZ,
                                          float& a_out) {
            if (auto* hmag =
                    a_subject->GetNodeByName("NPC Head MagicNode [Hmag]")) {
                a_out = (std::max)(a_headZ, hmag->world.translate.z);
                return true;
            }
            if (auto* neck = a_subject->GetNodeByName("NPC Neck [Neck]")) {
                a_out = a_headZ +
                        (std::max)(0.0f, a_headZ - neck->world.translate.z);
                return true;
            }
            return false;
        }

        // The ground the framed subject stands on, live.
        //
        // ⚠ THIS IS NOT THE REF ORIGIN, AND ON A HORSE IT NEVER WAS. Every
        // length here used to read GetPosition().z as the feet, which is true
        // on foot and false mounted, where it answers the saddle. The held
        // drop carries the difference; on foot it is zero and this is the same
        // read the file has always done. See MountedSubjectPolicy.h for the
        // measurement that found it.
        [[nodiscard]] float SubjectGroundZ(RE::Actor* a_subject) {
            return a_subject ? a_subject->GetPosition().z + g_s.groundDrop0
                             : 0.0f;
        }

        // Measure the subject's column, asking the mount for the ground when
        // there is one.
        //
        // ⚠ ACTOR STATE, NOT CAMERA STATE, AND THE DIFFERENCE IS THE POINT.
        // `stateId == kMount` answers "which camera must I force out of and
        // hand back", which is a camera's business and stays where it is. This
        // answers "is there a horse under the player and where does it stand",
        // and only the actor side can produce the mount whose position the
        // ground comes from. Two questions, two answers, one file each.
        //
        // The NiPointer is held across the read on purpose. Handing back a raw
        // pointer from a smart one and letting the count drop is how engine
        // lifetimes get hand-balanced, and this project has a rule about that.
        [[nodiscard]] MSP::Column MeasureColumn(RE::Actor* a_subject,
                                                float     a_crownZ) {
            MSP::Inputs in{ .riderOriginZ = a_subject->GetPosition().z,
                            .crownZ = a_crownZ };
            RE::NiPointer<RE::Actor> mount;
            if (a_subject->IsOnMount()) {
                // ⚠ THE TWO ARE ASKED SEPARATELY BECAUSE THEY MAY DISAGREE.
                // `IsOnMount()` and `GetMount()` are used interchangeably in
                // five places in this plugin on the assumption that they always
                // agree, and nothing has ever checked it. Folding them into one
                // condition would turn a disagreement into a silent on-foot
                // measurement of a mounted rider, which is the one outcome the
                // policy exists to make visible.
                in.hasMount = true;
                if (a_subject->GetMount(mount) && mount) {
                    in.mountOriginKnown = true;
                    in.mountOriginZ = mount->GetPosition().z;
                }
            }
            return MSP::Measure(in);
        }

        // How high the character stands off their own ground: the foot bone
        // above the ground, which mounted is the horse's and not the rider's
        // origin.
        //
        // ⚠ THIS IS THE HEEL DETECTOR, and it is a foot bone because a foot
        // bone is the one thing here that head-tracking cannot move. High
        // heels lift the whole skeleton so the shoe's sole meets the ground,
        // which raises every bone above the origin and makes the character
        // genuinely taller; some setups move the reference instead. Either
        // way this number steps, and nothing else in a paused menu makes it
        // step - so it is a signal, not a guess.
        //
        // ⚠ MOUNTED IT IS NOT A SIGNAL YET. A foot in a stirrup sways, and the
        // 12 August run caught it crossing the 4-unit gate nine times in one
        // two-second arm. Measuring from the ground rather than the saddle
        // removes the origin error, not the sway. See the heel gate note in
        // EnsureBodyHeight.
        [[nodiscard]] bool StandHeight(RE::Actor* a_subject, float& a_out) {
            if (!a_subject) {
                return false;
            }
            for (const char* name : { "NPC L Foot [Lft ]", "NPC R Foot [Rft ]" }) {
                if (auto* node = a_subject->GetNodeByName(name)) {
                    a_out = node->world.translate.z - SubjectGroundZ(a_subject);
                    return true;
                }
            }
            return false;
        }

        // The subject's height, measured once per subject from feet to crown
        // and then HELD in bodyHeight0. ⚠ HELD ON PURPOSE, and this is the
        // dip fix: the crown rides the head, the head bows to track a camera
        // sitting below her eye line, and an anchor fed by the LIVE crown
        // bowed the whole composition with her ("we dip for some reason
        // before we are fully zoomed in"). Lengths do not bow. The origin
        // plus a held length gives every anchor a stable stand; the XY stays
        // planted on the origin exactly as before, and breathing sway still
        // reaches the shot through the ordinary pivot easing.
        [[nodiscard]] bool EnsureBodyHeight(RE::Actor* a_subject) {
            if (g_s.bodyHeight0 > 0.0f) {
                // ⚠ HELD IS NOT FOREVER. Swapping to high heels mid-menu
                // makes the character taller under a measurement taken
                // barefoot, and every anchor built on it then sits low by
                // the heel - the field's "we get the proper angles if we
                // open the menu with high heels equipped, but if we switch
                // outfit it doesn't recalibrate". The hold exists to ignore
                // the head bowing toward the camera, not to ignore the
                // character changing shape, so the stance is what says when
                // to look again.
                float live = 0.0f;
                if (!StandHeight(a_subject, live) ||
                    std::abs(live - g_s.standHeight0) <= 4.0f) {
                    return true;
                }
                spdlog::info("studio camera: the character now stands {:.1f} units "
                             "higher than when the shot was measured (heels on or "
                             "off), so the proportions are read again.",
                             live - g_s.standHeight0);
                g_s.bodyHeight0 = -1.0f;
                g_s.groundDrop0 = 0.0f;
                g_s.groundFromMount0 = false;
                g_s.loggedCrown.clear();
            }
            const char* declined = nullptr;
            float       crown = 0.0f;
            auto*       head =
                a_subject ? a_subject->GetNodeByName("NPC Head [Head]") : nullptr;
            if (!a_subject) {
                declined = "no subject";
            } else if (!head) {
                declined = "no head bone";
            } else if (!CrownFromBones(a_subject, head->world.translate.z, crown)) {
                declined = "no crown reference bone";
            } else {
                const MSP::Column column = MeasureColumn(a_subject, crown);
                if (!column.plausible) {
                    declined = "implausible proportions";
                } else {
                    g_s.bodyHeight0 = column.height;
                    // Held alongside the height, and from the same instant:
                    // the three are one measurement, and comparing a new
                    // stance against an older height's stance would judge
                    // the wrong pair. ⚠ The drop has to land BEFORE the
                    // stance is read, because the stance is now measured
                    // from the ground this names.
                    g_s.groundDrop0 =
                        column.groundZ - a_subject->GetPosition().z;
                    g_s.groundFromMount0 = column.fromMount;
                    float stand = 0.0f;
                    g_s.standHeight0 = StandHeight(a_subject, stand) ? stand : 0.0f;
                    if (g_s.loggedCrown != "ok") {
                        g_s.loggedCrown = "ok";
                        // ⚠ THE WORDING UP TO THE FULL STOP IS LOAD-BEARING.
                        // Two handoffs and a survey tell the next session to
                        // grep `track anchors armed`, and one field reading of
                        // this line is the whole evidence base for the mounted
                        // work. The mount clause is appended, never spliced in.
                        if (column.fromMount) {
                            spdlog::info(
                                "studio camera: track anchors armed: crown z "
                                "{:.1f}, body height {:.1f}, standing {:.1f} off "
                                "the ground. Measured from the MOUNT, which "
                                "stands {:.1f} below the rider's own origin.",
                                crown, column.height, g_s.standHeight0,
                                -g_s.groundDrop0);
                        } else {
                            spdlog::info(
                                "studio camera: track anchors armed: crown z "
                                "{:.1f}, body height {:.1f}, standing {:.1f} off "
                                "the ground.",
                                crown, column.height, g_s.standHeight0);
                        }
                        // ⚠ A REFUSED MOUNT IS NOT A CLEAN MEASUREMENT. It
                        // leaves a mounted rider measured from the saddle,
                        // which is the original halving wearing the fix's
                        // clothes and passing every gate on the way through.
                        // Nothing on screen says so, so the log has to.
                        if (column.mountDeclined) {
                            spdlog::warn(
                                "studio camera: the mount was asked for the "
                                "ground and refused ({}), so this rider is "
                                "measured from the saddle and the body height "
                                "above is short by the height of the animal.",
                                column.mountDeclined);
                        }
                    }
                    return true;
                }
            }
            if (g_s.loggedCrown != declined) {
                g_s.loggedCrown = declined;
                spdlog::warn("studio camera: track anchors declined ({}), every "
                             "keyframe sits on the focus centre.",
                             declined);
            }
            return false;
        }

        // Where a bone's mass really sits, and how it wants to be seen. A
        // joint is one END of the thing it drives, so each entry names a far
        // end and how far along to aim; entries are tried in order, so a
        // bone can ask for a fingertip and settle for a knuckle on a
        // skeleton without fingers.
        // ⚠ THE MARGIN IS ABOUT MASS THE SKELETON CANNOT SEE. A bone tells
        // us where a joint is, never how much boot hangs off it: a heeled
        // sole drops most of a hand's width below the ankle and a fingertip
        // reaches past the last knuckle, and neither has a bone at its end.
        // So the parts at the end of a limb arrive a wheel notch wider than
        // the caller asked for, which is the field's "zoom out just a tiny
        // bit so we can still see the lower part of the shoe". Counted in
        // notches on purpose: a notch is the unit the hand already knows,
        // and it stays the same felt step at any distance because the track
        // is log-normalised.
        struct BoneSegment {
            const char* bone;
            const char* farEnd;
            float       along;  // 0 the joint itself, 1 the far joint
            bool        sideOn;
            float       arrivalNotches;
            const char* how;
        };
        inline constexpr BoneSegment kSegments[] = {
            // The boot hangs off the far end of the calf, whose joint is the
            // knee, and its sole hangs below that again.
            { "NPC L Calf [LClf]", "NPC L Foot [Lft ]", 1.0f, true, 1.0f, "the ankle" },
            { "NPC R Calf [RClf]", "NPC R Foot [Rft ]", 1.0f, true, 1.0f, "the ankle" },
            { "NPC L Foot [Lft ]", "NPC L Toe0 [LToe]", 0.5f, true, 1.0f, "the mid-foot" },
            { "NPC R Foot [Rft ]", "NPC R Toe0 [RToe]", 0.5f, true, 1.0f, "the mid-foot" },
            // The hand joint is the WRIST and the fingers hang below it, so
            // a wrist-centred close-up cuts them off - the field saw exactly
            // that and had to zoom out to see whole fingers. Aim at the
            // middle of the hand instead, measured to the middle fingertip.
            { "NPC L Hand [LHnd]", "NPC L Finger22 [LF22]", 0.5f, true, 1.0f, "the mid-hand" },
            { "NPC R Hand [RHnd]", "NPC R Finger22 [RF22]", 0.5f, true, 1.0f, "the mid-hand" },
            { "NPC L Hand [LHnd]", "NPC L Finger20 [LF20]", 0.7f, true, 1.0f, "the knuckles" },
            { "NPC R Hand [RHnd]", "NPC R Finger20 [RF20]", 0.7f, true, 1.0f, "the knuckles" },
            { "NPC L Thigh [LThg]", "NPC L Calf [LClf]", 0.5f, false, 0.0f, "the mid-thigh" },
            { "NPC R Thigh [RThg]", "NPC R Calf [RClf]", 0.5f, false, 0.0f, "the mid-thigh" },
        };

        // ⚠ A JOINT IS ONE END OF THE MASS IT DRIVES, NOT ITS MIDDLE - and
        // this skeleton's head cluster is worse: head joint, magic node and
        // neck joint ALL answer within a few units of the crown (07:49 log:
        // head -3518 == the crown line; 08:15: neck -3524.8 against crown
        // -3517.6), so no head-cluster joint can place a face at all. The
        // face is an authored fraction of the HELD body height instead - the
        // eye line - standing on the origin so head-tracking cannot swing
        // it. Limb bones anchor along their own segment: the boots slot
        // focuses the calf, whose joint is the KNEE and whose shoe hangs at
        // the segment's FAR end - the first pass anchored the segment's
        // middle and the field could not see the shoes at all. Anything
        // unlisted keeps the joint: wrists and spine joints already sit
        // mid-mass and the field never complained about them.
        // Everything hanging off a_root, as one sphere in a_root's OWN local
        // frame. Local rather than world because the answer has to survive the
        // arm moving: a world centre is stale the next frame and would force
        // this walk every frame, while a local offset holds until the player
        // selects something else.
        //
        // ⚠ THE AUTHORED modelBound, NOT worldBound. worldBound is the read
        // that already failed in this file: actor skeletons are flattened bone
        // trees, the engine does not maintain it there, and every bone answered
        // centre (0,0,0) radius 0 (log-proven 05:42, every node). modelBound is
        // written into the nif and needs nothing updated at runtime.
        //
        // ⚠ GEOMETRY ONLY. A weapon's own NiNodes carry no bound worth having,
        // so an empty subtree answers radius 0 and every caller reads that as
        // "there is nothing here to frame" rather than as a point at the
        // origin.
        [[nodiscard]] P::Sphere MeasureAttachment(RE::NiAVObject* a_root,
                                                  int& a_outGeometryCount) {
            a_outGeometryCount = 0;
            P::Sphere out{};
            if (!a_root) {
                return out;
            }
            const RE::NiMatrix3 rootRotT = a_root->world.rotate.Transpose();
            const float         rootScale =
                a_root->world.scale > 0.0f ? a_root->world.scale : 1.0f;

            const auto visit = [&](auto&& a_self, RE::NiAVObject* a_obj) -> void {
                if (!a_obj) {
                    return;
                }
                if (auto* geom = a_obj->AsGeometry()) {
                    // ⚠ GetModelData(), NOT THE BARE MEMBER. This plugin builds
                    // SE, AE and VR from one tree, so SKYRIM_CROSS_VR compiles
                    // the direct members out of BSGeometry entirely and the
                    // only way to the field is the accessor that relocates it.
                    const RE::NiBound& bound      = geom->GetModelData().modelBound;
                    const RE::NiPoint3 worldCentre = geom->world * bound.center;
                    const RE::NiPoint3 local =
                        (rootRotT * (worldCentre - a_root->world.translate)) / rootScale;
                    const float radius = bound.radius * geom->world.scale / rootScale;
                    if (radius > 0.0f) {
                        ++a_outGeometryCount;
                        out = P::UnionSpheres(
                            out, P::Sphere{ { local.x, local.y, local.z }, radius });
                    }
                }
                if (auto* node = a_obj->AsNode()) {
                    for (auto& child : node->GetChildren()) {
                        a_self(a_self, child.get());
                    }
                }
            };
            visit(visit, a_root);
            return out;
        }

        [[nodiscard]] float FocusAnchorZ(RE::Actor* a_subject,
                                         const std::string& a_name,
                                         RE::NiAVObject* a_node,
                                         const char*& a_outHow) {
            const float joint = a_node->world.translate.z;
            a_outHow = nullptr;
            if (a_name == "NPC Head [Head]") {
                if (EnsureBodyHeight(a_subject)) {
                    a_outHow = "the eye line";
                    const float face =
                        P::Clamp01(Settings::GetSingleton().cameraTrackFaceHeight);
                    // ⚠ MOUNTED, THE SURFACE IS THE SADDLE. The rule is the
                    // same one the standing case uses: the face sits a stated
                    // fraction of the way up the subject's own span above
                    // whatever they are standing on. On foot that surface is
                    // the ground. On a horse it is the ref she is sitting on,
                    // and her span above it is her own, not the animal's.
                    //
                    // ⚠ MEASURED FROM THE SURFACE AND NOT FROM THE CROWN, and
                    // one field round paid for the difference. Anchoring a drop
                    // below the crown assumes the crown is the top of her head,
                    // and it is not: this skeleton's head cluster answers around
                    // her EYES (measured 2026-08-13, her eyes sat 9.5 above an
                    // anchor placed 8.4 under the crown). The standing formula
                    // never depended on that and neither does this one.
                    if (g_s.groundFromMount0) {
                        const float ref = a_subject->GetPosition().z;
                        const float crown =
                            SubjectGroundZ(a_subject) + g_s.bodyHeight0;
                        return ref + face * (crown - ref);
                    }
                    return SubjectGroundZ(a_subject) + face * g_s.bodyHeight0;
                }
                return joint;
            }
            for (const auto& seg : kSegments) {
                if (a_name != seg.bone) {
                    continue;
                }
                // Not break-on-name: a bone may list more than one far end
                // (the hand wants a fingertip and settles for a knuckle), so
                // the first entry whose far end this skeleton actually has
                // is the one that answers.
                if (auto* far = a_subject->GetNodeByName(seg.farEnd)) {
                    a_outHow = seg.how;
                    return joint + seg.along * (far->world.translate.z - joint);
                }
            }
            return joint;
        }

        // Is this a part that reads flat from the front? A hand seen head-on
        // is a knuckle-width silhouette and a boot is a toe cap, so a focus
        // on one swings the shot round to that side of the character and
        // shows its profile. The field asked for it by name, on the hands
        // and the feet.
        [[nodiscard]] bool WantsSideView(const std::string& a_name) {
            for (const auto& seg : kSegments) {
                if (a_name == seg.bone) {
                    return seg.sideOn;
                }
            }
            return false;
        }

        // How much wider than asked this part arrives, in wheel notches.
        [[nodiscard]] float ArrivalNotches(const std::string& a_name) {
            for (const auto& seg : kSegments) {
                if (a_name == seg.bone) {
                    return seg.arrivalNotches;
                }
            }
            return 0.0f;
        }

        // The point the camera orbits. A named node's HEIGHT rather than the
        // actor's origin, because the origin sits at the feet and orbiting the
        // feet swings the head across the frame.
        //
        // ⚠ XY IS THE SUBJECT'S ORIGIN, NOT THE NODE'S. The origin is the axis
        // the preview spin turns around, and the head sits a hand's width off
        // it - so a pivot taken from the node's full position swung in a
        // circle whenever the character was spun, and the whole shot swayed
        // with it: the field's "jitter of the bubble". Head-tracking and idle
        // sway fed the same path at smaller amplitude. Taking only the height
        // keeps the face-zoom framing while the axis stays planted; what
        // height wobble remains is eased in Apply rather than followed raw.
        [[nodiscard]] bool ResolvePivot(RE::Actor* a_subject, RE::NiPoint3& a_out,
                                        RE::NiPoint3* a_outLateral = nullptr) {
            if (a_outLateral) {
                *a_outLateral = RE::NiPoint3{};
            }
            if (!a_subject) {
                return false;
            }
            a_out = a_subject->GetPosition();
            // ⚠ THE HEIGHT IS THE GROUND, THE XY IS THE ORIGIN. Both branches
            // below overwrite this z, so the only path that keeps it is the
            // missing-node fallback at the end (origin + 100) - and on a horse
            // the origin is the saddle, so +100 from there aims over the
            // rider's head. XY stays the origin because that is the axis the
            // preview spin turns around; see the note above.
            a_out.z = SubjectGroundZ(a_subject);
            // A caller's focus node outranks the configured one - it knows
            // what the player is actually looking at, which a static setting
            // cannot. Falls straight back to the setting when nothing is
            // focused or the named bone is not on this skeleton.
            const auto& name =
                g_s.focusNode.empty() ? Settings::GetSingleton().cameraPivotNode
                                      : g_s.focusNode;
            if (!name.empty()) {
                if (auto* node = a_subject->GetNodeByName(name)) {
                    // ⚠ THE worldBound READ IS STILL GONE, AND THE BRANCH
                    // BELOW IS NOT IT. Actor skeletons are FLATTENED BONE TREES
                    // and the engine does not maintain worldBound on their
                    // nodes: every bone answered centre (0,0,0) radius 0, so
                    // the OS-138a sanity clamp rejected the lift on EVERY
                    // request and the pivot was the joint all along - log-
                    // proven 05:42, every node. What the branch below uses is
                    // the AUTHORED modelBound of the geometry HANGING OFF the
                    // node, which lives in the nif and needs nothing updated at
                    // runtime, measured once by MeasureAttachment. Field-proven
                    // 2026-08-07: a drawn weapon on the hand answered 3 pieces
                    // at radius 37.45, and two weapons gave two radii. A BONE
                    // still measures nothing, so every armour focus takes the
                    // else branch and the segment middle answers exactly as it
                    // always did.
                    // ⚠ SIDEWAYS ONLY FOR A CALLER'S FOCUS, never for the
                    // configured pivot. The XY-on-the-origin rule is what
                    // stopped the whole shot swaying when the preview spin
                    // turned the character (the field's "jitter of the
                    // bubble"), and the configured head pivot is the path
                    // that reported it. A focus request is different: the
                    // caller has named a part and wants THAT framed, and a
                    // hand or foot is nowhere near the axis. Apply fades
                    // this out as the shot widens, so the wide composition
                    // still centres the character.
                    const char* how = nullptr;
                    if (g_s.focusBound.radius > 0.0f && !g_s.focusNode.empty()) {
                        // The measured centre, carried into the world by the
                        // node's live transform. All three axes: this is a
                        // thing whose middle we know, not a joint we are
                        // guessing the middle of.
                        const RE::NiPoint3 local{ g_s.focusBound.centre.x,
                                                  g_s.focusBound.centre.y,
                                                  g_s.focusBound.centre.z };
                        const RE::NiPoint3 world = node->world * local;
                        how   = "its measured centre";
                        a_out = world;
                        if (a_outLateral) {
                            const RE::NiPoint3 origin = a_subject->GetPosition();
                            a_outLateral->x = world.x - origin.x;
                            a_outLateral->y = world.y - origin.y;
                        }
                    } else {
                        a_out.z = FocusAnchorZ(a_subject, name, node, how);
                        if (a_outLateral && !g_s.focusNode.empty()) {
                            const RE::NiPoint3 origin = a_subject->GetPosition();
                            a_outLateral->x = node->world.translate.x - origin.x;
                            a_outLateral->y = node->world.translate.y - origin.y;
                        }
                    }
                    // The dedup key carries the anchor mode, so a far-end
                    // bone appearing or vanishing (the editor's head rebuild)
                    // reports once instead of hiding behind the found log.
                    const std::string key = how ? name : "~" + name;
                    if (g_s.loggedPivotNode != key) {
                        g_s.loggedPivotNode = key;
                        if (how) {
                            spdlog::info(
                                "studio camera: pivot '{}' anchors {} at z "
                                "{:.1f} (joint z {:.1f}).",
                                name.c_str(), how, a_out.z,
                                node->world.translate.z);
                        } else {
                            spdlog::info(
                                "studio camera: pivot '{}' at joint z {:.1f}.",
                                name.c_str(), a_out.z);
                        }
                    }
                    return true;
                }
                // ⚠ LOUD ON PURPOSE. The focus log line above only proves the
                // NAME was stored; this is the first line that proves the bone
                // was found or not, and the silent fallback below is exactly
                // the shape OS-138's "accepted rather than silently dropped"
                // inference could not see past. The dedup key carries the
                // outcome so a found-then-missing flip (the editor's head
                // rebuild) says so once instead of hiding behind the found log.
                if (g_s.loggedPivotNode != "!" + name) {
                    g_s.loggedPivotNode = "!" + name;
                    spdlog::warn(
                        "studio camera: no node '{}' on '{}', pivot falls back "
                        "to the origin + 100.",
                        name.c_str(), a_subject->GetName());
                }
            }
            // The fallback raises the actor's origin by most of a body height,
            // so a missing or misspelled node name costs framing rather than
            // pointing the camera at the floor.
            a_out.z += 100.0f;
            return true;
        }

        // The body-middle anchor: feet plus a stated fraction of the held
        // height. The crown behind that height comes from bones, never
        // bounds (actor skeletons carry NO worldBound - every node answers
        // zero, log-proven 05:42; do not relearn it), and the track reads no
        // frustum at all, which is its own simplification.
        [[nodiscard]] bool ResolveBodyMid(RE::Actor* a_subject, float& a_out) {
            if (!EnsureBodyHeight(a_subject)) {
                return false;
            }
            a_out = SubjectGroundZ(a_subject) +
                    P::Clamp01(Settings::GetSingleton().cameraTrackBodyMid) *
                        g_s.bodyHeight0;
            return true;
        }

        // The end of a focus round hands the shot back. Field 2026-08-05:
        // closing the Fitting Room editor left the shot parked on whatever
        // part was last focused, so "the camera doesn't reset" - the exit
        // reset the field asked for three times, stated once: the track
        // eases back to the OPENING keyframe position, the framing the menu
        // opened with. Not to where the shot sat when the round began: the
        // field kept ending rounds inside a face zoom THEY had wheeled to
        // before opening the editor, and restoring that read as no reset at
        // all.
        // ⚠ THE RETURN IS UNCONDITIONAL ON PURPOSE. The first cut cancelled
        // it on any manual wheel, reasoning the distance had become the
        // player's - and the field wheels constantly inside the editor, so
        // the return almost never fired and the exit still stranded the
        // shot on a body part. Exiting the editor means "give me my camera
        // back", whatever happened inside. The unconsumed pre-capture ask
        // is dropped too: a parked closeness from a round that ended must
        // not fire at the NEXT capture.
        void ReturnToOpening() {
            g_s.pendingFocusT = -1.0f;
            if (!g_s.captured) {
                return;
            }
            g_s.trackTarget = g_s.trackT0;
        }

        // The hand-built framing alone, without the round bookkeeping below.
        //
        // ⚠ SPLIT OUT BECAUSE A RE-AIM AT THE NODE ALREADY HELD NEEDS THE PAN
        // GONE AND THE ROUND LEFT ALONE. Calling the full ClearPan there would
        // wipe a side-view swing the current round is still owed, since the
        // node-change branch raises pendingSideView AFTER clearing.
        void ClearPanOffsets() {
            g_s.heightTarget = 0.0f;
            g_s.lateralTarget = 0.0f;
            g_s.panned = false;
        }

        // Drop a hand-built framing. Kept apart from the track's return
        // because the two are asked for separately: ending a focus round
        // wants both, changing which part is focused wants only this.
        void ClearPan() {
            ClearPanOffsets();
            g_s.pendingSideView = false;  // an owed swing dies with its round
            // ⚠ NOT autoYaw. ClearPan runs at the START of a focus change, and
            // FocusOnNode needs to know whether the yaw it is looking at was
            // ours before it decides whether to hand it back. Clearing the flag
            // here would answer "no" every time and the swing would never be
            // undone. It is cleared where the decision is made instead.
        }

        // One wheel notch's travel along the track. Clamped away from zero
        // so a mangled setting cannot make the wheel silently dead.
        [[nodiscard]] float TrackStep() {
            return (std::max)(0.005f, Settings::GetSingleton().cameraTrackStep);
        }

        // The track's two ends, defined below and needed by the composition
        // above them.
        [[nodiscard]] float MinimumDistance();
        [[nodiscard]] float SoftMaxDistance();
        [[nodiscard]] float ConfiguredMaxDistance();

        // A focus asks for a shot of a PART, and it says so as a closeness that
        // maps onto the track. Widening the far stop to fit a horse must not
        // drag those shots outward with it, so the closeness is resolved against
        // the boundary that would apply on foot and then converted back to a
        // place on the track we are actually running.
        [[nodiscard]] float FocusTrackT(float a_t) {
            const float floorDistance = MinimumDistance();
            const float softMax = SoftMaxDistance();
            const float configured = ConfiguredMaxDistance();
            if (softMax <= configured + 0.1f) {
                return a_t;  // nothing was widened, so nothing to undo
            }
            return P::TrackT(P::TrackDistance(a_t, floorDistance, configured),
                             floorDistance, softMax);
        }

        // Everything a composition is built from: the pivot (the subject's
        // axis in XY, the focus anchor in Z), the focused part's sideways
        // offset from that axis, the ground under the subject and their
        // middle.
        struct AnchorInputs {
            RE::NiPoint3 pivot{};
            RE::NiPoint3 lateral{};
            float        feetZ = 0.0f;
            float        bodyMid = 0.0f;
        };

        // The point the shot is built around at a given place on the track.
        // ⚠ ONE FUNCTION, TWO CALLERS, ON PURPOSE. Capture derives the
        // opening orbit from this and Apply composes every later frame from
        // it, and the untouched-arm identity holds only while the two agree
        // exactly. They drifted apart once already, when the law lived in
        // Apply alone.
        [[nodiscard]] RE::NiPoint3 AnchorAt(float a_t, const AnchorInputs& a_in) {
            const auto&  cfg = Settings::GetSingleton();
            RE::NiPoint3 out = a_in.pivot;
            const float  crown =
                a_in.feetZ + (g_s.bodyHeight0 > 0.0f ? g_s.bodyHeight0 : 0.0f);
            // How tall the picture is here decides how far the aim may sit
            // from the body's middle, which is the whole composition. See
            // ComposeShot: aim at what was asked for, give way to the body
            // only as fast as the growing frame forces you to.
            const float halfSpan =
                P::HalfSpanAt(a_t, MinimumDistance(), SoftMaxDistance(),
                              cfg.cameraTrackLensSlope);
            // Mounted, what the shot gives way to depends on how much of the
            // column the frame can actually hold. See GiveWayTarget.
            const float giveWay =
                g_s.groundFromMount0
                    ? MSP::GiveWayTarget(crown - MSP::kRiderSeatedHalfHeight,
                                         a_in.bodyMid, halfSpan,
                                         0.5f * (crown - a_in.feetZ))
                    : a_in.bodyMid;
            const auto shot = P::ComposeShot(a_in.pivot.z, a_in.feetZ, crown,
                                             giveWay, halfSpan,
                                             cfg.cameraTrackSlack);
            out.z = shot.anchorZ;
            const float w = P::Clamp01(cfg.cameraTrackLateral) * shot.lateral;
            out.x += w * a_in.lateral.x;
            out.y += w * a_in.lateral.y;
            return out;
        }

        // ⚠ DELIBERATELY A FLAT NUMBER, NOT THE SUBJECT'S BOUNDING SPHERE. That
        // sphere was tried and it is the wrong measure: it encloses the whole
        // skeleton with the arms out and whatever is slung on the back, which
        // comes to roughly 150 to 170 units on an ordinary character. Used as a
        // camera floor it sat just under a normal opening distance of 175 and
        // pinned the wheel after one notch, which read in the field as the
        // camera not moving at all.
        //
        // The pivot is the chest and a torso is about twenty units deep, so a
        // flat sixty clears the body with room for a head-and-shoulders shot.
        // fCameraMinDistance is the dial for anyone who disagrees.
        [[nodiscard]] float MinimumDistance() {
            return (std::max)(P::kMinDistance,
                              Settings::GetSingleton().cameraMinDistance);
        }

        // How far out the shot may go before the rubber band starts pulling.
        //
        // Taken from the SPACE the studio built when there is one. In the void
        // and the dressing room the world is culled and a dome of a known radius
        // is what the player is standing inside, so past it there is nothing to
        // see: the character shrinks against an empty shell and the backdrop
        // edge comes into frame. That dome is the honest boundary, and it moves
        // when the player resizes it.
        //
        // Kept inside the shell rather than level with it, because a camera
        // exactly on the surface sees the seam.
        // The boundary before any mounted widening: what the player's setting or
        // the built space says on its own.
        //
        // ⚠ A FOCUS MUST BE MEASURED AGAINST THIS ONE. Closeness maps onto the
        // whole track, so widening the far stop for a mount moved every focused
        // shot outward with it. A head focus asks for closeness 0, which used to
        // mean 200 units and silently became 273 - far enough that the shot
        // framed the horse instead of the face. The field found it exactly:
        // "specifically for head, all other parts are fine", because every other
        // part asks for a real closeness and lands short of the stop.
        [[nodiscard]] float ConfiguredMaxDistance() {
            const auto& cfg = Settings::GetSingleton();
            if (cfg.cameraMaxDistance > 0.0f) {
                return cfg.cameraMaxDistance;  // an explicit setting wins
            }
            if (cfg.IsVoidFamily() && cfg.backdropDomeRadius > 1.0f) {
                return cfg.backdropDomeRadius * 0.85f;
            }
            // No space, so no shell to measure. A plain full-length bound.
            return 420.0f;
        }

        [[nodiscard]] float SoftMaxDistance() {
            const auto& cfg = Settings::GetSingleton();
            const float chosen = ConfiguredMaxDistance();
            // ⚠ A RIDER ON A HORSE IS TALLER THAN ANY OF THOSE ANSWERS WERE
            // CHOSEN AGAINST, and the far stop that cannot hold the subject is
            // not a boundary, it is a wall. Measured 2026-08-13: the column is
            // 180.3 units and needs 277.4 to fit, against a shipped 200 that
            // holds 130. The arm opened at t 1.00 with the track already spent,
            // so the wheel did nothing and the pair never fit.
            //
            // ⚠ MOUNTED ONLY, AND THAT IS THE WHOLE POINT. Widening this for
            // everyone is on the do-not-retry list: on-foot framing is field
            // clean, an explicit setting is the player's word about the shot
            // they can see, and a body of 113 to 122 needs about 181 which
            // every default already clears. Nobody chose their boundary while
            // looking at a horse, so mounted is the one case where the setting
            // is answering a question it was never asked.
            //
            // A floor, never a ceiling: this can only push the stop further
            // out, so a player who set a wider one keeps it.
            if (!g_s.groundFromMount0) {
                return chosen;
            }
            return (std::max)(chosen,
                              P::DistanceThatHolds(g_s.bodyHeight0,
                                                   cfg.cameraTrackLensSlope));
        }

        // How far the pan may carry the frame off the subject, from the held
        // height so tall and small characters get the same felt room. The
        // floor keeps a degenerate setting from pinning the pan dead.
        //
        // Down gets its own, larger range: the pan composes onto the track's
        // pivot, which sits at the eye line at the close stop, so the shoes
        // are nearly a body away while the crown is a head away. See the
        // band's note in the policy.
        [[nodiscard]] float PanHeight() {
            return g_s.bodyHeight0 > 0.0f ? g_s.bodyHeight0 : 120.0f;
        }

        [[nodiscard]] float PanBound() {
            return (std::max)(10.0f, Settings::GetSingleton().cameraPanRange *
                                         PanHeight());
        }

        [[nodiscard]] float PanBoundDown() {
            return (std::max)(10.0f, Settings::GetSingleton().cameraPanRangeDown *
                                         PanHeight());
        }

        [[nodiscard]] RE::Actor* SubjectOrPlayer(RE::Actor* a_subject) {
            if (a_subject) {
                return a_subject;
            }
            return RE::PlayerCharacter::GetSingleton();
        }

        // How long the mounted aim takes to walk from the level shot the engine
        // handed us to the one that actually points at the subject. Long enough
        // to read as the camera settling, short enough that nobody waits.
        inline constexpr float kMountAimBlend = 0.30f;

        // The convention correction at a point in that walk. a_u of 0 solves it
        // against the true offset, which reproduces the captured shot exactly;
        // a_u of 1 solves it against a levelled reference, which is the honest
        // pair and aims at the anchor. Everything between is a proper rotation
        // rather than a blend of two matrices, because the REFERENCE is what
        // moves and the result is rebuilt from it each frame.
        [[nodiscard]] P::Mat3 MountAimCorrection(float a_u) {
            P::Orbit reference = g_s.base;
            if (g_s.groundFromMount0) {
                reference.pitch = g_s.base.pitch * (1.0f - P::Clamp01(a_u));
            }
            return P::Multiply(
                P::Transposed(P::BasisFromOffset(P::OffsetFromOrbit(reference))),
                FromEngine(g_s.originalRotate));
        }

        // Read the opening state off the shot that is already on screen.
        [[nodiscard]] bool Capture(RE::NiNode* a_root, RE::Actor* a_subject) {
            RE::NiPoint3 pivot;
            RE::NiPoint3 lateral;
            if (!ResolvePivot(a_subject, pivot, &lateral)) {
                return false;
            }
            // The anchor easings are seeded RAW: the track composes its
            // height after them in Apply, so seeding adjusted values here
            // would bake the keyframes in twice on frame one.
            g_s.pivotBase = pivot;
            g_s.pivotEased = pivot;  // frame one applies exactly what was found
            g_s.pivotSeeded = true;
            float bodyMid = pivot.z;
            if (!ResolveBodyMid(a_subject, bodyMid)) {
                bodyMid = pivot.z;  // flat track: every keyframe on the focus centre
            }
            g_s.bodyMidEased = bodyMid;
            g_s.lateralEased = lateral;

            g_s.originalTranslate = a_root->local.translate;
            g_s.originalRotate = a_root->local.rotate;

            // The base orbit and the convention correction are derived from
            // the TRACK'S pivot at the opening position t0, the same value
            // Apply composes each tick - identity on frame one needs the two
            // to agree, the same trick the pre-track code used. The anchor
            // height and t0 define each other (the height depends on where
            // along the track the shot sits, t0 is measured to the anchor),
            // so the pair is solved by a damped fixed point: the anchor moves
            // by at most half a body while the distance moves far less, and
            // the damping keeps even a steep near-vertical shot convergent.
            // What remains after six rounds is a sub-unit dolly along the
            // view axis, well under anything a frame can show.
            const float floorDistance = MinimumDistance();
            const float softMax = SoftMaxDistance();
            const AnchorInputs inputs{ pivot, lateral,
                                       a_subject ? SubjectGroundZ(a_subject)
                                                 : pivot.z,
                                       bodyMid };
            RE::NiPoint3 anchor = pivot;
            float        t0 = 0.5f;
            for (int i = 0; i < 6; ++i) {
                anchor = AnchorAt(t0, inputs);
                const RE::NiPoint3 d = g_s.originalTranslate - anchor;
                const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                t0 = 0.5f * (t0 + P::TrackT(dist, floorDistance, softMax));
            }
            anchor = AnchorAt(t0, inputs);

            const RE::NiPoint3 offset = g_s.originalTranslate - anchor;
            g_s.base = P::OrbitFromOffset(ToPolicy(offset));
            if (g_s.base.distance < 1.0f) {
                // The camera is sitting on the pivot, so there is no direction
                // to orbit around and every derived angle would be noise.
                // Declining is honest; the next tick tries again.
                return false;
            }

            // The convention correction, worked out from the shot we found. See
            // the note on the field it is stored in.
            //
            // ⚠⚠ THE PAIR THIS IS SOLVED FROM HAS TO BE HONEST, AND MOUNTED IT
            // IS NOT. The correction answers "given the camera was aiming along
            // MINUS THIS OFFSET, what convention turns one into the other". On
            // foot that holds: the shot we capture is already pointed at the
            // player. Mounted it does not. The rider's raise puts the lens 117
            // units above the anchor, so the offset carries a pitch of 0.46,
            // while the view direction the engine hands us is DEAD LEVEL
            // (measured 2026-08-13: we wrote 0.000 where aiming at the anchor
            // wanted -0.446). Solving with that pair teaches the correction to
            // cancel 0.46 of pitch, and every frame afterwards reproduces a
            // level camera perfectly. The composition was right the whole time,
            // the distance was right, the anchor was right, and the lens looked
            // over her head anyway.
            //
            // So the reference is levelled to match what the engine actually
            // did. The convention still comes from the engine's own rotation,
            // which is the whole reason this field exists; only the direction
            // it is paired against is corrected.
            // ⚠ SOLVED AT THE TRUE PITCH SO FRAME ONE IS STILL BYTE-IDENTICAL,
            // AND THE LEVELLING IS EASED IN AFTER. Correcting the pair outright
            // fixed the aim and broke the identity the whole capture is built
            // to preserve: the shot before the capture is level, the shot after
            // is aimed, and swapping between them on the frame the player first
            // drags is a visible snap. Field, 2026-08-13: "it looked fine when
            // I opened it, but when I dragged, the view immediately stuttered
            // to a different angle, not wildly different but jarring."
            //
            // So the capture reproduces exactly what was on screen, and Apply
            // walks the reference pitch to level over kMountAimBlend seconds.
            // See MountAimCorrection.
            g_s.mountAimBlend = g_s.groundFromMount0 ? 0.0f : 1.0f;
            g_s.correction = MountAimCorrection(0.0f);

            // ⚠ WHAT THE CAMERA IS AT DOES NOT HAVE TO BE A SENSIBLE PLACE TO
            // ORBIT FROM, and adopting it blind put the lens inside a shoulder
            // in the field. Show Player In Inventory frames these menus and it
            // frames THE PLAYER, while the pivot here is whoever the shot is
            // about, so with a follower framed the captured distance is just
            // however far she happened to be standing from the player's camera.
            // It read 44.9 units in one arm and 163.9 in another, on the same
            // follower. Keep the reading when it is usable, rescue it when it
            // is not.
            g_s.minDistance = floorDistance;  // binds the wheel too, not just the arm
            if (g_s.base.distance < floorDistance) {
                // ⚠ OPENS AT THE OPENING DISTANCE, NOT THE FLOOR. Pushing out to
                // the floor would leave the player already jammed against the
                // near limit with nowhere to scroll, which is a worse first
                // frame than the too-close one it replaced.
                //
                // ⚠ EXCEPT IN THE CHARACTER EDITOR, WHERE THE FLOOR IS THE
                // POINT. That menu's own opening framing measured 34.0-39.1
                // units from the head across one field round, against a floor
                // of 35.0 - a legitimate face shot sitting a hair under the
                // limit, not a nonsense SPII distance. Rescuing it out to the
                // opening distance threw the shot to the whole body on a coin
                // flip of pose noise (the 05:00:13 arm), and the editor branch
                // below asks for the face right back. The floor keeps the
                // shot where the menu put it, one imperceptible unit out.
                const float opened = Bubble::IsRaceMenuOpen()
                                         ? floorDistance
                                         : Settings::GetSingleton().cameraOpenDistance;
                spdlog::info("studio camera: the shot on screen was {:.1f} units from "
                             "'{}', too close to orbit, so opening at {:.1f} instead. "
                             "Whoever framed this menu did not frame the subject we "
                             "circle.",
                             g_s.base.distance,
                             a_subject ? a_subject->GetName() : "the subject", opened);
                g_s.base.distance = P::ClampDistance((std::max)(floorDistance, opened));
            }
            // The found shot's place on the track. Everything the track
            // derives from here on - distance tick to tick, the keyframed
            // pivot height, the return at the end of a focus round - runs
            // from this one value. A rescue above lands the recomputation on
            // the rescued distance, which is the deliberate jump.
            t0 = P::TrackT(g_s.base.distance, floorDistance, softMax);
            g_s.trackT0 = t0;
            g_s.trackT = t0;
            // Wheel travel parked before capture folds onto the opening
            // position: hard at the near stop, the band's give at the far.
            g_s.trackTarget = P::ClampTrack(t0 + g_s.preCaptureZoom,
                                            P::TrackCeiling(floorDistance, softMax));
            g_s.preCaptureZoom = 0.0f;
            g_s.orbit = g_s.base;
            g_s.orbitTarget.yaw += g_s.base.yaw;
            g_s.orbitTarget.pitch = P::ClampPitch(g_s.orbitTarget.pitch + g_s.base.pitch);
            g_s.captured = true;
            g_s.capturedInEditor = Bubble::IsRaceMenuOpen();
            // ⚠ THE CHARACTER EDITOR OPENS ON THE FACE, AND THE REASON IS ITS
            // OWN BUTTON BAR. That menu frames the face on open and its LAlt
            // prompt reads "Zoom Out" from the first frame accordingly. Our
            // re-stamp took the node and left the shot on the whole body, so
            // the prompt was offering to pull back from a shot that was
            // already pulled back, and the first press of an advertised
            // control did the opposite of what it said.
            //
            // Adopting the editor's own opening composition puts the label and
            // the picture back in agreement without touching that menu: LAlt
            // now pulls back to the body and presses again to return, which is
            // what "Zoom Out" then "Zoom In" describes.
            //
            // Set through the same state the toggle uses, so the first press
            // reads the flag we left and steps from there rather than fighting
            // it. A parked focus ask below still wins - an API caller framing a
            // specific node is more specific than a default opening shot.
            if (g_s.capturedInEditor) {
                // ⚠ ADOPTED FROM WHAT THE SHOT MEASURES, NOT IMPOSED. Capture
                // is the FIRST INPUT, not the menu's opening: pre-capture the
                // editor's own LAlt zoom still works, so by the time a drag
                // captures, the player may have zoomed that camera out on
                // purpose - and the menu's prompt has flipped to "Zoom In"
                // accordingly. Forcing the face here yanked a framing the
                // player had just built (2026-08-06 field: "stutters, jumps
                // instantly forward, then pulls out"), under a label
                // promising the opposite. The near half of the track IS the
                // close composition, so a near capture adopts exactly as
                // before; a far one starts the toggle out, and the first
                // press pulls IN - which is what the flipped prompt reads.
                // Judged on the post-fold target, not t0, so wheel travel
                // parked before the capture counts as the player's ask.
                if (g_s.trackTarget <= 0.5f) {
                    // Latched rather than written here, so it lands on the
                    // same frame and the same value whatever else writes this
                    // field on the frame the menu changed.
                    g_s.pendingEditorShot = true;
                }
            }
            // The parked focus ask, now that there is a framing to ease
            // from. Already in track terms, so it needs no measuring - just
            // the consume.
            if (g_s.pendingFocusT >= 0.0f) {
                // Resolved here rather than where it was parked: the boundary
                // it has to be measured against is only known once the subject
                // has been measured, which is this function's job.
                g_s.trackTarget = FocusTrackT(P::Clamp01(g_s.pendingFocusT));
                g_s.pendingFocusT = -1.0f;
            }
            g_s.subjectId = a_subject ? a_subject->GetFormID() : 0;
            g_s.distanceLow = g_s.base.distance;
            g_s.distanceHigh = g_s.base.distance;
            g_s.yawLow = g_s.yawHigh = g_s.base.yaw;
            g_s.pitchLow = g_s.pitchHigh = g_s.base.pitch;
            if (!g_s.loggedCapture) {
                g_s.loggedCapture = true;
                // The floor is IN this line because leaving it out cost a round.
                // "The camera will not move" and "the camera is pinned against
                // its near limit" look identical from the outside, and the only
                // thing separating them is how far the floor sits from where the
                // shot opened.
                spdlog::info("studio camera: took the shot at distance {:.1f}: t "
                             "{:.2f} on the track, floor {:.1f}, boundary {:.1f}; "
                             "yaw {:.2f}, pitch {:.2f} around '{}' on '{}'.",
                             g_s.base.distance, t0, g_s.minDistance, softMax,
                             g_s.base.yaw, g_s.base.pitch,
                             Settings::GetSingleton().cameraPivotNode,
                             a_subject ? a_subject->GetName() : "nobody");
                // ⚠ WHERE THE SUBJECT ACTUALLY LANDS IN THE PICTURE, because
                // every other number here describes the CAMERA and none of them
                // answers "is she in shot". Three field rounds on the mounted
                // framing were spent reasoning from distance, boom and pitch
                // toward a screen nobody could see from the log, and the
                // composition the arithmetic predicted disagreed with the
                // screenshot by half a frame. This is the line that would have
                // ended it on the first round. 0 is the bottom edge, 1 the top.
                const float probeSpan =
                    P::HalfSpanAt(g_s.trackTarget, g_s.minDistance, softMax,
                                  Settings::GetSingleton().cameraTrackLensSlope);
                const RE::NiPoint3 probeAnchor =
                    AnchorAt(g_s.trackTarget, inputs);
                const float probeHeight =
                    g_s.bodyHeight0 > 0.0f ? g_s.bodyHeight0 : 0.0f;
                const float frame = 2.0f * probeSpan;
                const float bottom = probeAnchor.z - probeSpan;
                const auto where = [&](float a_z) {
                    return frame > 0.0f ? (a_z - bottom) / frame : -1.0f;
                };
                spdlog::info(
                    "studio camera: framing check: the frame runs z {:.1f} to "
                    "{:.1f} around anchor {:.1f}; the subject's ground sits at "
                    "{:.2f} of it and the crown at {:.2f} (0 is the bottom edge, "
                    "1 the top). Subject {:.1f} tall against a frame {:.1f} tall.",
                    bottom, bottom + frame, probeAnchor.z, where(inputs.feetZ),
                    where(inputs.feetZ + probeHeight), probeHeight, frame);
            }
            return true;
        }

        void Revert(RE::NiNode* a_root) {
            if (!g_s.captured || !a_root) {
                return;
            }
            a_root->local.translate = g_s.originalTranslate;
            a_root->local.rotate = g_s.originalRotate;
        }

        void Apply(RE::NiNode* a_root, RE::Actor* a_subject, float a_dt, float a_rate) {
            // The deferred measurement, paid on the first tick that has both a
            // subject and the node attached. See focusMeasurePending.
            if (g_s.focusMeasurePending && a_subject && !g_s.focusNode.empty()) {
                if (auto* n = a_subject->GetNodeByName(g_s.focusNode)) {
                    int             geoms    = 0;
                    const P::Sphere measured = MeasureAttachment(n, geoms);
                    g_s.focusMeasurePending  = false;
                    if (measured.radius > 0.0f) {
                        g_s.focusBound  = measured;
                        const auto& cfg = Settings::GetSingleton();
                        const float t   = P::AttachmentTrackT(
                            measured.radius, cfg.cameraAttachmentFill,
                            cfg.cameraTrackLensSlope, MinimumDistance(), SoftMaxDistance());
                        spdlog::info("studio camera: '{}' measures {} geometry piece(s), "
                                     "radius {:.2f}, so the shot arrives at t {:.2f}.",
                                     g_s.focusNode, geoms, measured.radius, t);
                        if (g_s.captured) {
                            g_s.trackTarget = t;
                        } else {
                            g_s.pendingFocusT = t;
                        }
                    } else {
                        // Nothing hanging there, so this is a plain node focus
                        // and the caller's fallback decides the distance.
                        // Identical to what a Menu Studio without the
                        // attachment export would have done.
                        spdlog::info("studio camera: '{}' has no geometry to measure, so "
                                     "the focus falls back to the node at closeness "
                                     "{:.2f}.",
                                     g_s.focusNode, g_s.focusFallbackCloseness);
                        g_s.focusBound = P::Sphere{};
                        if (g_s.focusFallbackCloseness >= 0.0f) {
                            const float t = P::Clamp01(1.0f - g_s.focusFallbackCloseness);
                            if (g_s.captured) {
                                g_s.trackTarget = t;
                            } else {
                                g_s.pendingFocusT = t;
                            }
                        }
                    }
                }
                // Node not on the skeleton yet: the flag stays up and the next
                // tick tries again. A weapon's 3D need not be attached on the
                // frame the caller asked, which is the whole reason this is
                // deferred rather than done in the export.
            }
            // The pivot is re-read every tick rather than frozen at capture.
            // Under Skyrim Souls the world is running and the subject can walk,
            // and a frozen pivot would leave the camera orbiting empty air.
            RE::NiPoint3 pivotTarget;
            RE::NiPoint3 lateralTargetXY;
            if (!ResolvePivot(a_subject, pivotTarget, &lateralTargetXY)) {
                pivotTarget = g_s.pivotBase;
            }
            // Eased at the same rate as the angles, so a moving pivot and a
            // moving orbit arrive as one motion. This is the jitter fix's
            // second half: whatever wobble survives the axis anchoring - a
            // breathing chest, a crouch, a subject swap - drifts the shot
            // instead of shaking it.
            if (!g_s.pivotSeeded) {
                g_s.pivotEased = pivotTarget;
                g_s.pivotSeeded = true;
            } else {
                const P::Vec3 eased = P::Ease(
                    ToPolicy(g_s.pivotEased), ToPolicy(pivotTarget), a_dt, a_rate);
                g_s.pivotEased = RE::NiPoint3{ eased.x, eased.y, eased.z };
            }
            RE::NiPoint3 pivot = g_s.pivotEased;
            // ⚠ THE TRACK'S HEIGHT COMPOSES AFTER THE WOBBLE EASING, NOT
            // BEFORE IT. The dead-space law once adjusted the pivot TARGET,
            // which the easing then smoothed a second time: the zoom would
            // settle and the height kept drifting after it, a lag the field
            // could feel but not name ("our smoothing is conflicting with
            // the zoom curve"). Here every input is already smooth - the
            // eased anchors and the SAME eased t the distance derives from -
            // so the vertical travel is in phase with the zoom by
            // construction, which is the night's hardest-won lesson kept.
            float bodyMid = pivot.z;
            if (!ResolveBodyMid(a_subject, bodyMid)) {
                bodyMid = pivot.z;  // flat track: every keyframe on the focus centre
            }
            g_s.bodyMidEased = P::Ease(g_s.bodyMidEased, bodyMid, a_dt, a_rate);
            // The sideways offset eases like the pivot, so a focus that
            // changes part, or a hand that swings with the preview spin,
            // drifts the shot rather than snapping it.
            const P::Vec3 lat = P::Ease(ToPolicy(g_s.lateralEased),
                                        ToPolicy(lateralTargetXY), a_dt, a_rate);
            g_s.lateralEased = RE::NiPoint3{ lat.x, lat.y, 0.0f };

            // The owed profile swing. The direction comes from the part
            // itself: it sticks out of the character on the side it belongs
            // to, so putting the camera along that line looks in from that
            // side and shows the part edge-on to the body, whatever the
            // character has been spun to. Nothing to know about headings.
            //
            // ⚠ ONCE, AND ONLY WHEN THE PART REALLY IS OFF THE AXIS. A part
            // sitting on the axis gives a direction that is mostly noise,
            // and repeating the swing every tick would pin the shot and
            // fight the player's own drag.
            if (g_s.pendingSideView) {
                const float reach = std::sqrt(lateralTargetXY.x * lateralTargetXY.x +
                                              lateralTargetXY.y * lateralTargetXY.y);
                if (reach > 3.0f) {
                    g_s.pendingSideView = false;
                    g_s.orbitTarget.yaw = P::NearestAngle(
                        std::atan2(lateralTargetXY.x, lateralTargetXY.y),
                        g_s.orbit.yaw);
                    // This yaw is ours until the player drags. See autoYaw.
                    g_s.autoYaw = true;
                    spdlog::info("studio camera: '{}' reads flat from the front, "
                                 "so the shot swings to that side ({:.1f} units "
                                 "off the axis).",
                                 g_s.focusNode.c_str(), reach);
                }
            }

            const AnchorInputs inputs{
                pivot, g_s.lateralEased,
                a_subject ? SubjectGroundZ(a_subject) : pivot.z, g_s.bodyMidEased
            };
            pivot = AnchorAt(g_s.trackT, inputs);
            pivot.z += g_s.height;

            // Lateral pans across the view rather than along a world axis, so
            // the drag matches what the player sees whatever direction they
            // have orbited to.
            pivot.x += g_s.lateral * std::cos(g_s.orbit.yaw);
            pivot.y -= g_s.lateral * std::sin(g_s.orbit.yaw);

            // The mounted aim easing itself in. Rebuilt rather than blended so
            // every intermediate value is a real rotation; see
            // MountAimCorrection.
            if (g_s.mountAimBlend < 1.0f) {
                g_s.mountAimBlend = P::Clamp01(g_s.mountAimBlend +
                                               a_dt / kMountAimBlend);
                const float u = g_s.mountAimBlend;
                // Smoothed at both ends so the settle has no corner on it.
                g_s.correction = MountAimCorrection(u * u * (3.0f - 2.0f * u));
            }

            const P::Vec3 nowOffset = P::OffsetFromOrbit(g_s.orbit);

            // What the node is holding as we arrive, which is after everything
            // else in the frame has had its turn with it. Compared against what
            // we left there last tick, this is the only honest answer to "does
            // our rotation survive".
            const float rotOnArrival = a_root->local.rotate.entry[2][1];
            const float rotWeLeft =
                g_s.haveLastWritten ? g_s.lastWrittenRotate.entry[2][1] : 0.0f;

            a_root->local.translate =
                RE::NiPoint3{ pivot.x + nowOffset.x, pivot.y + nowOffset.y,
                              pivot.z + nowOffset.z };

            // Built from the current offset and the correction captured once,
            // so the engine's own rotation is never read again after the first
            // frame. With nothing dragged yet this reproduces the captured
            // rotation exactly, which is what keeps an untouched arm invisible.
            a_root->local.rotate =
                ToEngine(P::Multiply(P::BasisFromOffset(nowOffset), g_s.correction));

            g_s.lastWritten = a_root->local.translate;
            g_s.lastWrittenRotate = a_root->local.rotate;
            g_s.haveLastWritten = true;

            // ⚠ WHAT WAS ACTUALLY WRITTEN, ONCE, A SECOND IN. The capture-time
            // framing check reports what the composition ASKED FOR, and on the
            // mounted arm the two disagree: the shot was composed at a distance
            // of 228 and the node came out 105 from the subject. Every round
            // spent on the mounted framing so far has been arithmetic checked
            // against arithmetic, with nothing measuring the transform that
            // actually lands. This is that measurement.
            if (!g_s.loggedLiveFraming && ++g_s.liveFramingTicks > 60) {
                g_s.loggedLiveFraming = true;
                const float composed =
                    P::TrackDistance(g_s.trackT, g_s.minDistance,
                                     SoftMaxDistance());
                const RE::NiPoint3 written = a_root->local.translate;
                const float dx = written.x - pivot.x;
                const float dy = written.y - pivot.y;
                const float dz = written.z - pivot.z;
                // The same reading the capture-time check prints, taken live so
                // a FOCUSED shot can be judged too. A head focus that lands the
                // face high with the chest centred is a different fault from a
                // wide shot that does, and only these fractions tell them apart.
                const auto& cfg = Settings::GetSingleton();
                const float liveSpan =
                    P::HalfSpanAt(g_s.trackT, g_s.minDistance, SoftMaxDistance(),
                                  cfg.cameraTrackLensSlope);
                const float liveHeight =
                    g_s.bodyHeight0 > 0.0f ? g_s.bodyHeight0 : 0.0f;
                const float liveFrame = 2.0f * liveSpan;
                const float liveBottom = pivot.z - liveSpan;
                const auto  liveWhere = [&](float a_z) {
                    return liveFrame > 0.0f ? (a_z - liveBottom) / liveFrame
                                            : -1.0f;
                };
                spdlog::info(
                    "studio camera: live framing: orbit distance {:.1f} against "
                    "a track that says {:.1f} at t {:.2f} (target {:.2f}); the "
                    "node sits {:.1f} from the anchor. Anchor z {:.1f}, camera z "
                    "{:.1f}, pan height {:.1f}. Orbit yaw {:.2f} pitch {:.2f}. "
                    "View direction z: we left {:.3f} last tick and the node "
                    "handed back {:.3f} this one (0.000 is dead level; aiming "
                    "at the anchor wants {:.3f}). Focus '{}'. The frame holds "
                    "z {:.1f} to {:.1f}: ground at {:.2f}, crown at {:.2f}, the "
                    "eye line at {:.2f}, the body middle at {:.2f}.",
                    g_s.orbit.distance, composed, g_s.trackT, g_s.trackTarget,
                    std::sqrt(dx * dx + dy * dy + dz * dz), pivot.z, written.z,
                    g_s.height, g_s.orbit.yaw, g_s.orbit.pitch, rotWeLeft,
                    rotOnArrival, -std::sin(g_s.orbit.pitch),
                    g_s.focusNode.empty() ? "the usual pivot" : g_s.focusNode,
                    liveBottom, liveBottom + liveFrame, liveWhere(inputs.feetZ),
                    liveWhere(inputs.feetZ + liveHeight),
                    // ⚠ THE SAME FORMULA THE ANCHOR USES, not the on-foot one.
                    // Reported against the wrong formula this number would
                    // describe a face anchor nothing is aiming at.
                    liveWhere(inputs.feetZ +
                              (g_s.groundFromMount0 ? -g_s.groundDrop0 : 0.0f) +
                              P::Clamp01(cfg.cameraTrackFaceHeight) *
                                  (liveHeight + (g_s.groundFromMount0
                                                     ? g_s.groundDrop0
                                                     : 0.0f))),
                    liveWhere(g_s.bodyMidEased));
            }

            // ⚠ RECURSIVE, NOT UpdateWorldData. The renderer reads the
            // NiCamera CHILD of this node, and UpdateWorldData refreshes only
            // the node itself - a re-stamp made that way is invisible on
            // screen whenever someone else's full update composed the child
            // from their value first. That gap is half of why the first
            // re-assert build changed nothing in the field.
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            a_root->Update(ctx);
        }

        // Did something other than us move the camera since our last write?
        //
        // The design rests on the engine NOT rebuilding the camera per frame
        // while a bubble menu is open: Bubble calls camera->Update() once at
        // arm, and the note there records Show Player In Menus doing the same.
        // That is a reading of someone else's code, so it is checked rather
        // than trusted. If it is ever wrong the rotation we compose onto would
        // be stale, and this says so instead of drifting quietly.
        // ⚠ THIS USED TO WATCH THE POSITION ALONE AND REPORT A CLEAN ZERO WHILE
        // THE ROTATION WAS BEING TAKEN FROM US EVERY FRAME. Every mounted disarm
        // printed "0 frames still found a foreign value at tick time", which
        // reads as nothing fighting us, and three rounds of field testing were
        // spent trusting it. A camera is a place AND a direction, and half a
        // check is worse than no check because it answers with confidence.
        [[nodiscard]] bool ForeignWrite(RE::NiNode* a_root) {
            if (!g_s.haveLastWritten) {
                return false;
            }
            const RE::NiPoint3 d = a_root->local.translate - g_s.lastWritten;
            if ((d.x * d.x + d.y * d.y + d.z * d.z) > 0.25f) {
                return true;
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    if (std::abs(a_root->local.rotate.entry[i][j] -
                                 g_s.lastWrittenRotate.entry[i][j]) > 0.01f) {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] RE::NiNode* CameraRoot() {
            auto* camera = RE::PlayerCamera::GetSingleton();
            return camera ? camera->cameraRoot.get() : nullptr;
        }

        // The cursor, as fractions of the display.
        //
        // ⚠ THIS IS HARDER THAN IT LOOKS AND THREE READINGS HAVE ALREADY BEEN
        // TRIED. `_root._xmouse` is a LAGGING copy under a paused menu, measured
        // at r=0.60 against the real cursor, so it is not an option; see
        // [[scaleform-xmouse-is-stale-under-pause]]. `RE::MenuCursor` would be
        // ideal and does not exist in the CommonLib this project builds against.
        //
        // What is left is two sources with opposite weaknesses, so both are used
        // and they check each other:
        //
        //   1. The action bar republishes FUCK's own mouse position every draw.
        //      That reading is measured EXACT (r = 1.0000) and is at most one
        //      frame old, but it only exists while the bar is on screen.
        //   2. GetMouseState is what the ENGINE last pushed into the movie
        //      through NotifyMouseState, rather than something Flash computes
        //      while advancing, so it should survive a paused menu. "Should" is
        //      doing work in that sentence and nothing here has measured it.
        //
        // The proven one wins when present. The other runs anyway whenever both
        // are available and disagreement is logged once, so the field run that
        // tests the camera also settles whether source 2 is trustworthy, at the
        // cost of one extra virtual call per press.
        [[nodiscard]] bool MovieCursor(float& a_outU, float& a_outV) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            const auto& menu = Bubble::GetSingleton().CurrentMenuName();
            if (menu.empty() || !ui->IsMenuOpen(menu)) {
                return false;
            }
            const auto view = ui->GetMovieView(menu);
            auto* movie = view.get();
            if (!movie) {
                return false;
            }
            RE::GViewport vp;
            movie->GetViewport(&vp);
            if (vp.width <= 0 || vp.height <= 0) {
                return false;
            }
            float         x = 0.0f;
            float         y = 0.0f;
            std::uint32_t buttons = 0;
            movie->GetMouseState(0, &x, &y, &buttons);
            // ⚠ VIEWPORT, NOT THE STAGE RECT, AND GETTING THAT WRONG COST
            // SEVERAL ROUNDS. GetMouseState answers in the coordinates the movie
            // is RENDERED into, so dividing by GetVisibleFrameRect scaled every
            // reading by display width over stage width. On a 2560 display
            // against a 1280 stage that is exactly 2, and the earlier build
            // logged 1.556 against a measured 0.778, which is that factor
            // showing. A cursor reading of 1.5 sits outside a region ending at
            // 1.0, so every drag and every wheel notch was refused for as long
            // as this path was the one answering.
            a_outU = (x - static_cast<float>(vp.left)) / static_cast<float>(vp.width);
            a_outV = (y - static_cast<float>(vp.top)) / static_cast<float>(vp.height);
            return true;
        }

        // Grid Inventory states where its own window is, and it is the ONE
        // mod-supplied number here worth reading rather than stating. Its
        // GridInventory_ui.ini is auto-generated: 'main = x,y,w,h' in screen
        // pixels, rewritten the moment the player moves the window. Measured
        // on 2026-08-27, the file changed under us inside one session.
        //
        // ⚠⚠ 'main' ONLY, AND THAT IS THE WHOLE REASON THIS IS SAFE.
        // The file also carries 'settings' and 'ltbuy', and it says nothing
        // about which windows are OPEN, so excluding those would take a
        // third of the screen out of the drag region with a closed panel and
        // nothing on screen to explain it. 'main' is the grid itself: if the
        // menu is up, that window is up.
        //
        // ⚠ A STATED BOUNDARY WAS TRIED FIRST AND WAS WORSE. fGridZoneLeft
        // shipped at 0.66 and the field answer was that it felt like SkyUI's
        // dead zone all over again: that player's grid sits at u 0.325 to
        // 0.644, so a left boundary past it refuses two thirds of the screen
        // to protect a window occupying a third of it. A rectangle gives back
        // everything above, below and left of the grid. The stated keys stay
        // as the fallback for when the file cannot be read.
        struct GridRect {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            [[nodiscard]] bool Valid() const { return w > 0.0f && h > 0.0f; }
        };

        [[nodiscard]] GridRect ReadGridMainRect() {
            // Re-read on a timer rather than once: the file is rewritten when
            // the player drags the window, and a cached rectangle from the
            // start of the session would protect where the grid used to be.
            static GridRect s_rect;
            static auto     s_when = std::chrono::steady_clock::time_point{};
            const auto      now    = std::chrono::steady_clock::now();
            if (s_when != std::chrono::steady_clock::time_point{} &&
                now - s_when < std::chrono::seconds(2)) {
                return s_rect;
            }
            s_when = now;
            s_rect = {};
            std::ifstream in("Data/SKSE/Plugins/GridInventory_ui.ini");
            if (!in) {
                return s_rect;
            }
            std::string line;
            while (std::getline(in, line)) {
                // 'main = x,y,w,h' and nothing else. Keys beginning '!' are
                // that file's own settings and are not windows.
                const auto eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                std::string key = line.substr(0, eq);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto end = key.find_last_not_of(" \t");
                if (end != std::string::npos) {
                    key.erase(end + 1);
                }
                if (key != "main") {
                    continue;
                }
                GridRect    r;
                const char* p = line.c_str() + eq + 1;
                if (std::sscanf(p, " %f , %f , %f , %f", &r.x, &r.y, &r.w, &r.h) == 4 &&
                    r.Valid()) {
                    s_rect = r;
                }
                break;
            }
            return s_rect;
        }

        // Is the cursor over Grid Inventory's own window? False whenever the
        // answer cannot be trusted (no file, no published display extent),
        // which hands the decision back to the stated zone rather than
        // guessing.
        [[nodiscard]] bool CursorOverGridWindow(float a_u, float a_v, bool& a_known) {
            a_known = false;
            float w = 0.0f;
            float h = 0.0f;
            if (!ActionBar::PublishedDisplay(w, h)) {
                return false;
            }
            const GridRect r = ReadGridMainRect();
            if (!r.Valid()) {
                return false;
            }
            a_known = true;
            const float uMin = r.x / w;
            const float vMin = r.y / h;
            const float uMax = (r.x + r.w) / w;
            const float vMax = (r.y + r.h) / h;
            return a_u >= uMin && a_u <= uMax && a_v >= vMin && a_v <= vMax;
        }

        [[nodiscard]] bool ResolveCursor(float& a_outU, float& a_outV) {
            float barU = 0.0f;
            float barV = 0.0f;
            const bool haveBar = ActionBar::PublishedCursor(barU, barV);

            float movieU = 0.0f;
            float movieV = 0.0f;
            const bool haveMovie = MovieCursor(movieU, movieV);

            if (haveBar && haveMovie) {
                static bool s_logged = false;
                const float du = barU - movieU;
                const float dv = barV - movieV;
                if (!s_logged && (du * du + dv * dv) > 0.0025f) {  // ~5% of the screen
                    s_logged = true;
                    spdlog::info("studio camera: the movie's mouse state disagrees with "
                                 "the measured cursor ({:.3f},{:.3f} against "
                                 "{:.3f},{:.3f}). The measured one is in use, so this "
                                 "is a note rather than a fault: it means "
                                 "GetMouseState cannot be the fallback when the action "
                                 "bar is switched off.",
                                 movieU, movieV, barU, barV);
                }
            }
            if (haveBar) {
                a_outU = barU;
                a_outV = barV;
                return true;
            }
            if (haveMovie) {
                a_outU = movieU;
                a_outV = movieV;
                return true;
            }
            return false;
        }

    }  // namespace

    // ⚠ THE RE-ASSERT, second attempt, and the story of why the first one
    // changed nothing is worth the space because both halves look correct in
    // isolation.
    //
    // The first build put one write_vfunc on PlayerCamera vtable slot 2 and
    // re-stamped with UpdateWorldData. Field result: identical symptoms, every
    // Inventory session still dead. Two stacked faults:
    //
    //   1. AE DEVIRTUALIZED THE PER-FRAME PATH. Exhaustive E8 scan over both
    //      binaries: SE 1.5.97 has zero direct callers of the TESCamera::Update
    //      body - every SE caller dispatches through the vtable - but 1.6.1170
    //      calls it by direct E8 from exactly two places, the player-update
    //      chunk (40450, +0x69) and PlayerCamera's per-frame master update
    //      (50784, +0x1A6). Those direct calls never touch the vtable, so the
    //      slot-2 thunk slept through every engine-driven rebuild while
    //      claiming to be installed. [[ae-inlined-hook-sites-pitfall]] in a
    //      new costume: the vtable resolves, the hook installs, and the
    //      runtime walks around it.
    //
    //   2. THE RE-STAMP DID NOT REACH THE RENDER CAMERA. UpdateWorldData
    //      refreshes one node; the engine's own update tail runs a recursive
    //      pass that composes the NiCamera CHILD, which is what the renderer
    //      reads. So even where the thunk did fire, orig() pushed the foreign
    //      pose into the child and the re-stamp then fixed only the parent.
    //
    // The shape now: one re-stamp helper behind every door into the body that
    // writes cameraRoot - the vtable slot for virtual callers (and all of SE),
    // plus write_calls on both devirtualized AE sites, located by call target
    // through VersionCheck like the frame driver. Each path counts its hits so
    // the next log names the mechanism actually running instead of leaving it
    // to be argued about.
    //
    // It re-stamps what the last tick COMPUTED rather than recomputing, so it
    // stays a couple of writes and cannot introduce a second opinion about
    // where the camera belongs.
    namespace {
        void ReassertAfterEngineWrite(RE::TESCamera* a_this, std::uint32_t& a_hits) {
            if (!g_s.armed || !g_s.captured || !g_s.haveLastWritten) {
                return;
            }
            auto* camera = RE::PlayerCamera::GetSingleton();
            if (!camera || a_this != camera) {
                return;  // somebody else's camera, leave it alone
            }
            auto* root = camera->cameraRoot.get();
            if (!root) {
                return;
            }
            root->local.translate = g_s.lastWritten;
            root->local.rotate = g_s.lastWrittenRotate;
            ++a_hits;
            // Recursive on purpose - see the twin note in Apply.
            RE::NiUpdateData ctx;
            ctx.time = 0.0f;
            root->Update(ctx);
        }
    }

    // ⚠ THE PROBE PAIR IN EACH THUNK IS NOT PART OF THE RE-ASSERT. It answers
    // a different question, on the other side of the close: after the character
    // editor's menu is gone, is anything still writing this node, and through
    // which door? We are already standing in all four doorways, so measuring it
    // costs a compare either side of orig() rather than a new hook - and it
    // costs a single relaxed atomic load while the watch is closed, which is
    // every frame of normal play. See CameraCloseProbe.h.
    //
    // It reads BEFORE orig and judges AFTER, so it records what the ENGINE did,
    // never what our own re-assert below then does about it.
    struct CameraUpdateHook {
        static void thunk(RE::TESCamera* a_this) {
            const auto mark = CameraCloseProbe::MarkBefore();
            orig(a_this);
            CameraCloseProbe::NoteAfter(CCP::Door::kVtable, mark);
            ReassertAfterEngineWrite(a_this, g_s.reassertVtable);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;
    };

    // ⚠ THE CHARACTER EDITOR BRINGS ITS OWN CAMERA OBJECT, and that is why
    // none of the three doors above ever opened there. RaceSexMenu owns a
    // RaceSexCamera at +0xA8 of its runtime data - a TESCamera subclass with
    // its own vtable - so the editor drives THAT and PlayerCamera's update
    // never runs. The field log is unambiguous: 'RaceSex Menu' released with
    // "Re-stamped 0 engine writes (vtable 0, player site 0, master site 0)"
    // against "551 frames still found a foreign value". Every write we made
    // was landing and being replaced by a camera we were not watching.
    //
    // ⚠ OBSERVATION ONLY SINCE 2026-08-06, and the Camera tab is why. This
    // hook spent one era suppressing that camera (which trapped the player at
    // the name prompt - its Update is also the editor's exit state machine)
    // and one era writing last (which won the node and made the menu's whole
    // camera surface a lie: LAlt, its prompts, and a Camera tab that can only
    // ever drive THIS camera it was fighting). Three correct fixes in one
    // night each held and none ended it, because the war itself was the bug.
    // The studio camera now stands down in the editor - Bubble releases the
    // arm on the open edge and Tick refuses the menu - so there is nothing
    // left to defend here, and orig runs untouched.
    //
    // ⚠ THE DOOR THE WHOLE WATCH WAS BUILT FOR is why the hook stays. If
    // this camera is still moving the node after its menu closed, the
    // stranded framing is being actively maintained by a camera whose menu
    // is gone - which is a different bug from every restore that has been
    // tried.
    struct RaceSexCameraUpdateHook {
        static void thunk(RE::TESCamera* a_this) {
            const auto mark = CameraCloseProbe::MarkBefore();
            orig(a_this);
            CameraCloseProbe::NoteAfter(CCP::Door::kEditor, mark);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;
    };

    struct CameraPlayerSiteHook {
        static void thunk(RE::TESCamera* a_this) {
            const auto mark = CameraCloseProbe::MarkBefore();
            func(a_this);
            CameraCloseProbe::NoteAfter(CCP::Door::kPlayerSite, mark);
            ReassertAfterEngineWrite(a_this, g_s.reassertPlayerSite);
        }
        static inline REL::Relocation<decltype(&thunk)> func;
    };

    struct CameraMasterSiteHook {
        static void thunk(RE::TESCamera* a_this) {
            const auto mark = CameraCloseProbe::MarkBefore();
            func(a_this);
            CameraCloseProbe::NoteAfter(CCP::Door::kMasterSite, mark);
            ReassertAfterEngineWrite(a_this, g_s.reassertMasterSite);
        }
        static inline REL::Relocation<decltype(&thunk)> func;
    };

    void InstallHook() {
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCamera[0] };
        CameraUpdateHook::orig = vtbl.write_vfunc(0x02, CameraUpdateHook::thunk);

        // The two devirtualized sites, when this runtime has them (AE). Same
        // discipline as the frame driver: never write a site whose byte is no
        // longer an E8 - VersionCheck verified the target, this re-reads the
        // byte at the moment of the write.
        auto& trampoline = SKSE::GetTrampoline();
        int   sites = 0;
        const auto install = [&](const REL::RelocationID& a_container,
                                 std::ptrdiff_t a_offset, auto& a_func,
                                 auto a_thunk, const char* a_name) {
            if (a_offset == 0) {
                return;
            }
            REL::Relocation<std::uintptr_t> site{ a_container, a_offset };
            if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
                byte != 0xE8) {
                spdlog::warn("studio camera: {} site is 0x{:02X}, expected E8, "
                             "skipped.", a_name, byte);
                return;
            }
            a_func = trampoline.write_call<5>(
                site.address(), reinterpret_cast<std::uintptr_t>(a_thunk));
            ++sites;
        };
        install(Offsets::PlayerUpdateCameraCaller,
                VersionCheck::CameraPlayerCallOffset(), CameraPlayerSiteHook::func,
                CameraPlayerSiteHook::thunk, "player-update camera call");
        install(Offsets::PlayerCameraMasterUpdate,
                VersionCheck::CameraMasterCallOffset(), CameraMasterSiteHook::func,
                CameraMasterSiteHook::thunk, "master-update camera call");

        // The character editor's own camera. A vtable write like the one
        // at the top, but on RaceSexCamera rather than PlayerCamera - a
        // different object with a different vtable, which is exactly how
        // the editor was overwriting us unseen.
        REL::Relocation<std::uintptr_t> editorVtbl{ RE::VTABLE_RaceSexCamera[0] };
        RaceSexCameraUpdateHook::orig =
            editorVtbl.write_vfunc(0x02, RaceSexCameraUpdateHook::thunk);

        spdlog::info("studio camera: re-assert installed on PlayerCamera::Update "
                     "(vtable + {} devirtualized site{}), so a view mod refreshing "
                     "the camera cannot undo an orbit. AE calls this body directly "
                     "and skips the vtable, which is why the vtable alone did "
                     "nothing here.",
                     sites, sites == 1 ? "" : "s");
    }

    void Arm() {
        // Idempotent, because two callers reach it now: the menu-open event,
        // which is early enough for a first-frame drag to accumulate, and the
        // armed tick, which is the older path and covers a menu that was
        // already up. Resetting on the second would throw away whatever the
        // player had already asked for.
        if (g_s.armed) {
            return;
        }
        g_s = State{};
        g_s.armed = true;

        // ⚠⚠ THE REMEMBERED FRAMING GOES IN AS A PRE-CAPTURE ASK, AND IT NEEDS
        // NO NEW MACHINERY AT ALL. Field 2026-08-30: "How do I save a camera
        // position? I have to readjust every time as I want to have a full
        // character view instead of the top half."
        //
        // The naive answer - impose a shot at arm - would break the contract at
        // the top of this file, which is that nothing is written until the
        // player asks, so an untouched menu stays byte-identical to the shot the
        // view mod built. That contract is load-bearing and has been re-earned
        // twice (see the note above Capture).
        //
        // It does not have to be broken. These four fields are EXACTLY what a
        // drag, a wheel notch and a middle-drag leave here before a capture:
        // orbitTarget's yaw and pitch carry the swing, preCaptureZoom carries
        // wheel travel in track units, and the pan pair are ordinary offsets.
        // AnyRequest() reads all four, so seeding them IS the ask, Capture folds
        // them onto whatever base the menu turns out to have, and the result is
        // indistinguishable from the player making the same moves by hand. A
        // menu they never touched with nothing remembered still writes nothing.
        const auto& cfg = Settings::GetSingleton();
        if (cfg.rememberFraming && cfg.framingSaved) {
            g_s.orbitTarget.yaw = cfg.framingYaw;
            g_s.orbitTarget.pitch = cfg.framingPitch;
            g_s.preCaptureZoom = cfg.framingZoom;
            g_s.heightTarget = cfg.framingHeight;
            g_s.lateralTarget = cfg.framingLateral;
            // ⚠ NOT MARKED panned. That latch answers "did the player build
            // this shot in THIS session", which Fitting Room asks before
            // deciding whether a page change may re-frame. A remembered shot is
            // a starting point, not an answer to that question.
        }
    }

    namespace {
        // The shot the player leaves with, in the same currency Arm seeds.
        // Called from Disarm, before the state is dropped.
        void RememberFraming() {
            auto& cfg = Settings::GetSingleton();
            if (!cfg.rememberFraming || !g_s.captured) {
                return;
            }
            // ⚠ A FOCUSED SHOT IS NOT THE PLAYER'S FRAMING. FocusOnNode moves
            // the pivot and the distance on a caller's behalf - Fitting Room
            // framing a head it is editing - so the yaw and zoom on screen
            // describe that part, not a preference. Saving it would open every
            // later inventory on a close-up of an ear.
            if (!g_s.focusNode.empty()) {
                return;
            }
            // The editor drives its own camera and we stand down in it, so
            // whatever these read there is not ours to record.
            if (g_s.capturedInEditor) {
                return;
            }
            // Away from the framing the menu opened with, which is what Arm
            // puts back. ⚠ THE YAW IS WRAPPED because the easing is plain
            // arithmetic: a player who spun the shot round three times would
            // otherwise have the next menu spin three times to reach the same
            // heading. NearestAngle against zero is the shortest way to say it.
            const float yaw = P::NearestAngle(g_s.orbitTarget.yaw - g_s.base.yaw, 0.0f);
            const float pitch = P::ClampPitch(g_s.orbitTarget.pitch - g_s.base.pitch);
            const float zoom = g_s.trackTarget - g_s.trackT0;
            const float height = g_s.heightTarget;
            const float lateral = g_s.lateralTarget;

            // Did the player actually build something? An untouched capture
            // (a focus round that returned, a single refused notch) lands here
            // with all five at zero, and recording that as "their framing"
            // would write the INI on every menu close for nothing.
            constexpr float kAngleEpsilon = 0.002f;  // radians, well under a nudge
            constexpr float kTrackEpsilon = 0.005f;  // a quarter of one wheel notch
            constexpr float kPanEpsilon = 0.005f;    // share of subject height
            const bool built = std::fabs(yaw) > kAngleEpsilon ||
                               std::fabs(pitch) > kAngleEpsilon ||
                               std::fabs(zoom) > kTrackEpsilon ||
                               std::fabs(height) > kPanEpsilon ||
                               std::fabs(lateral) > kPanEpsilon;
            // Nothing built and nothing remembered: the ordinary close, and it
            // must not touch the file. Nothing built but something remembered
            // is the player having RECENTRED, which is them saying they want
            // the opening shot back - so that one is recorded, as a clear.
            if (!built && !cfg.framingSaved) {
                return;
            }
            const bool same = cfg.framingSaved == built &&
                              std::fabs(cfg.framingYaw - yaw) <= kAngleEpsilon &&
                              std::fabs(cfg.framingPitch - pitch) <= kAngleEpsilon &&
                              std::fabs(cfg.framingZoom - zoom) <= kTrackEpsilon &&
                              std::fabs(cfg.framingHeight - height) <= kPanEpsilon &&
                              std::fabs(cfg.framingLateral - lateral) <= kPanEpsilon;
            if (same) {
                // Re-opening on a remembered shot and leaving it alone lands
                // here every single close. Writing the same numbers back would
                // rewrite the INI, and bump the settings revision, for nothing.
                return;
            }
            cfg.framingSaved = built;
            cfg.framingYaw = built ? yaw : 0.0f;
            cfg.framingPitch = built ? pitch : 0.0f;
            cfg.framingZoom = built ? zoom : 0.0f;
            cfg.framingHeight = built ? height : 0.0f;
            cfg.framingLateral = built ? lateral : 0.0f;
            cfg.Save();
            if (built) {
                spdlog::info("studio camera: framing remembered - swing {:.3f} rad, "
                             "pitch {:.3f}, zoom {:+.3f} along the track, pan "
                             "({:.3f},{:.3f}). The next menu opens on it.",
                             yaw, pitch, zoom, lateral, height);
            } else {
                spdlog::info("studio camera: framing forgotten - the shot was left "
                             "where the menu opened it, so the next one opens "
                             "there too.");
            }
        }
    }

    void Disarm() {
        // Reported whenever the wheel was touched at all, captured or not,
        // because "the wheel did nothing" needs to distinguish four different
        // failures: the notches never arrived, the region refused them, they
        // were taken and the camera never engaged, or they were taken and moved
        // it by an amount too small to see.
        // ⚠ ANY attempt at all, because the narrower condition this replaced
        // stayed silent on exactly the sessions that failed: a run refused only
        // by the region printed nothing, so the case being reported was the one
        // case with no evidence.
        const std::uint32_t refusals = g_s.refusedNoCursor + g_s.refusedOutside +
                                       g_s.refusedOverWindow + g_s.refusedItemPreview;
        if (g_s.wheelSeen > 0 || g_s.captured || refusals > 0) {
            spdlog::info("studio camera: '{}' released after {} frames. Wheel notches "
                         "seen {}, taken {}. Refused: {} no cursor, {} outside the "
                         "region (across x {:.2f}..{:.2f}, region starts {:.2f}), {} "
                         "over a window, {} item preview. {} drag events turned the "
                         "shot {:.2f} rad of yaw and {:.2f} of pitch. Distance ran "
                         "{:.1f} to {:.1f}, floor {:.1f}, boundary {:.1f}. Re-stamped "
                         "{} engine writes (vtable {}, player site {}, master site "
                         "{}); {} frames still found a foreign value at tick time.",
                         Bubble::GetSingleton().CurrentMenuName(),
                         g_s.dragFrames, g_s.wheelSeen, g_s.wheelTaken,
                         g_s.refusedNoCursor, g_s.refusedOutside,
                         g_s.refusedUMax < 0.0f ? 0.0f : g_s.refusedUMin,
                         g_s.refusedUMax < 0.0f ? 0.0f : g_s.refusedUMax,
                         Settings::GetSingleton().cameraZoneLeft,
                         g_s.refusedOverWindow, g_s.refusedItemPreview,
                         g_s.orbitEvents, g_s.yawHigh - g_s.yawLow,
                         g_s.pitchHigh - g_s.pitchLow,
                         g_s.distanceLow, g_s.distanceHigh, g_s.minDistance,
                         SoftMaxDistance(),
                         g_s.reassertVtable + g_s.reassertPlayerSite +
                             g_s.reassertMasterSite,
                         g_s.reassertVtable, g_s.reassertPlayerSite,
                         g_s.reassertMasterSite, g_s.foreignFrames);
        }
        // ⚠ BEFORE THE REVERT AND BEFORE THE STATE IS DROPPED, because it
        // reads the shot the player is leaving with. Reverting first would put
        // the node back but the fields it reads are the ones it needs; dropping
        // the state first would leave it nothing at all.
        RememberFraming();
        // ⚠ THE CHARACTER EDITOR GETS NO REVERT, and the field is what
        // taught this. Everywhere else the revert is right: we wrote over a
        // shot a view mod built, and putting it back is how a menu we
        // touched ends exactly where it started. The editor is not that
        // case. It owns a camera of its own that restores itself on close,
        // and OwnView restores the framing a moment later, so the transform
        // we captured belongs to neither of them by the time this runs -
        // writing it back re-imposes a shot nothing is set up to recompute,
        // and it sat there looking wrong until the player moved the camera
        // and forced a rebuild. Leaving it alone lets the two systems that
        // do own it finish their own restores.
        if (g_s.captured && !g_s.capturedInEditor) {
            Revert(CameraRoot());
        } else if (g_s.capturedInEditor) {
            spdlog::info("studio camera: left the character editor without "
                         "reverting: that menu and own view each restore "
                         "their own, and our copy is stale by now.");
        }
        g_s = State{};
    }

    void DropOnLoad() { g_s = State{}; }

    bool Active() { return g_s.armed && g_s.captured; }

    void ResetOffsets() {
        ClearPan();
        // A recentre puts the orbit back to base outright, so there is no swing
        // left for a later focus change to hand back.
        g_s.autoYaw = false;
        if (!g_s.captured) {
            g_s.orbitTarget = P::Orbit{};
            g_s.preCaptureZoom = 0.0f;
            return;
        }
        g_s.orbitTarget = g_s.base;
        g_s.trackTarget = g_s.trackT0;  // the opening keyframe position
    }

    bool ShotWasPanned() { return g_s.panned; }

    void FocusOnNode(const char* a_nodeName, float a_closeness) {
        // The consent gate. A player who has switched the camera off has said
        // the shot is not ours to move, and an API call is not a louder voice
        // than that.
        if (!Settings::GetSingleton().studioCamera) {
            return;
        }
        // The character editor is not this camera's room (see Tick). Refused
        // rather than parked: a pendingFocusT parked here would outlive the
        // editor and fire into whatever menu comes next.
        if (Bubble::IsRaceMenuOpen()) {
            return;
        }
        // Consumed unconditionally and before anything can return, so a repeat
        // focus on the node already held cannot leave it latched for the next
        // caller.
        const bool attachment = g_nextFocusIsAttachment;
        g_nextFocusIsAttachment = false;
        const std::string node = a_nodeName ? a_nodeName : "";
        if (node != g_s.focusNode) {
            if (!g_s.focusNode.empty() && node.empty()) {
                // An empty name is a clear in API clothing; end the round
                // the same way ClearFocus does.
                ReturnToOpening();
            }
            // A new part means a new shot, so the pan starts clean. A
            // framing nudged onto a boot is meaningless once the camera is
            // circling a shoulder, and carrying it over hands the player a
            // fresh focus already knocked off centre.
            ClearPan();
            const bool wantsSide = attachment || WantsSideView(node);
            // ⚠ THE SWING IS UNDONE ON THE WAY OUT OF IT, not left for the
            // next part to inherit. A hand or a boot reads flat from the front
            // so the shot swings to its side; a head or a chest does not, and
            // arriving at one in the profile the previous part asked for shows
            // the player an ear. Only our own swing is taken back: AddOrbit
            // clears autoYaw, so a yaw the player dragged to survives a focus
            // change exactly as it did before.
            if (!wantsSide && g_s.autoYaw) {
                g_s.orbitTarget.yaw = P::NearestAngle(g_s.base.yaw, g_s.orbit.yaw);
                spdlog::info("studio camera: '{}' reads from the front, so the "
                             "side view the last part asked for is handed back.",
                             node.empty() ? "the usual pivot" : node.c_str());
            }
            if (!wantsSide) {
                g_s.autoYaw = false;
            }
            g_s.pendingSideView = wantsSide;
            g_s.focusNode = node;
            // A node change owns no measurement until one is taken. Without
            // this, focusing a weapon and then a helmet would carry the
            // weapon's sphere onto the head and pivot the shot at a sword's
            // length off the skull.
            //
            // ⚠ AND IT CANNOT CLEAR focusMeasurePending, which
            // FocusOnAttachment raises AFTER calling this. Clearing it here
            // would cancel the very measurement that call is asking for.
            g_s.focusBound = P::Sphere{};
            spdlog::info("studio camera: the shot is focused on '{}'.",
                         node.empty() ? "the usual pivot" : node);
        } else if (a_closeness >= 0.0f) {
            // ⚠ A RE-AIM AT THE NODE ALREADY HELD IS STILL A COMPOSED SHOT,
            // AND IT USED TO ARRIVE OFF CENTRE. Everything above is gated on
            // the node changing while the closeness below is applied on every
            // call, so asking for the same bone again moved the distance and
            // kept the pan. Root-caused off a Fitting Room log 2026-08-12: the
            // report reads "pan on the head, work on the torso, click the head
            // again, and it moves without ending up centred", and the log shows
            // all three asks naming 'NPC Head [Head]' with only the closeness
            // moving. Several of that editor's slots sit in one body region and
            // resolve to one bone, so the node never changed at all.
            //
            // This is the function's own contract rather than a policy about
            // whose framing wins: FocusOnNode(node, closeness) promises that
            // node at that closeness, and a live pan is not that shot. The
            // "leave a hand-built framing alone" rule stays where it belongs,
            // in the caller, which asks ShotWasPanned() before it asks at all.
            //
            // ⚠ A PIVOT-ONLY CALL KEEPS THE PAN, DELIBERATELY. Negative
            // closeness means "distance left to the player", which is how
            // FocusOnAttachment reaches here on its way to setting its own.
            // Only a caller supplying a real closeness is asking to be
            // composed, and only a composed shot starts centred.
            ClearPanOffsets();
        }
        if (a_closeness < 0.0f) {
            return;  // pivot only, distance left to the player
        }
        // Closeness maps straight onto the track: 1 is the near stop, 0 the
        // far one. The track owns what those mean in units, so a caller's
        // "as close as allowed" lands on the close composition by
        // construction. The margin then holds the shot off by a notch or so
        // for the parts whose mass runs past the bone that names them.
        const float margin = ArrivalNotches(node) * TrackStep() *
                             (std::max)(0.0f, Settings::GetSingleton().cameraFocusMargin);
        const float t = P::Clamp01(1.0f - a_closeness + margin);
        // ⚠ RE-ARMED ON EVERY ASK, NOT ON A NODE CHANGE. Several of Fitting
        // Room's slots resolve to one bone, so clicking through the head rows
        // re-issues the same node at a different closeness and the shot moves a
        // long way. Keyed on the node, this probe reported the first ask and
        // went quiet for every shot the player was actually looking at.
        g_s.loggedLiveFraming = false;
        g_s.liveFramingTicks = 0;
        spdlog::info("studio camera: focus '{}' asked for closeness {:.2f}, "
                     "arriving at t {:.2f}{}.",
                     node.c_str(), a_closeness, t,
                     margin > 0.0f ? " (a notch wider, the part runs past its bone)"
                                   : "");
        if (g_s.captured) {
            g_s.trackTarget = FocusTrackT(t);
            return;
        }
        // ⚠ HELD, NOT WRITTEN, UNTIL THERE IS A FRAMING TO EASE FROM. The
        // ask is already absolute in track terms, but consuming it before
        // capture would leave nothing for the capture to trigger on and no
        // opening position to return to afterwards - the park stays, and
        // Capture applies it once the opening framing is measured.
        g_s.pendingFocusT = t;
    }

    void FocusOnAttachment(const char* a_nodeName, float a_fallbackCloseness) {
        // The same two gates FocusOnNode carries, for the same reasons: a
        // player who switched the camera off has said the shot is not ours,
        // and the character editor is not this camera's room. Checked here as
        // well as there so the latch below is never left set on a refusal.
        if (!Settings::GetSingleton().studioCamera || Bubble::IsRaceMenuOpen()) {
            return;
        }
        // ⚠ PIVOT ONLY, WITH THE DISTANCE HELD BACK. FocusOnNode is what
        // stores the node, clears the pan and decides whether anything
        // changed, so it still runs; passing a negative closeness means it
        // takes no view on the distance, which is the half the measurement
        // owns. It also clears focusBound on a node change, which is exactly
        // right here: the old weapon's sphere must not describe the new node.
        g_nextFocusIsAttachment = true;
        FocusOnNode(a_nodeName, -1.0f);
        const std::string node = a_nodeName ? a_nodeName : "";
        if (node.empty()) {
            g_s.focusMeasurePending = false;
            return;
        }
        g_s.focusMeasurePending    = true;
        g_s.focusFallbackCloseness = a_fallbackCloseness;
    }

    void ClearFocus() {
        if (!g_s.focusNode.empty()) {
            g_s.focusNode.clear();
            g_s.focusBound          = P::Sphere{};
            g_s.focusMeasurePending = false;
            const bool returned = g_s.captured;
            // Ending the round is the recentre the middle mouse does: the
            // track goes back to the opening framing AND the hand-built pan
            // goes with it. Leaving the pan behind was the field's "reset
            // pan on FR exit" - a framing built for a boot has no meaning
            // once the whole character is back on screen.
            ClearPan();
            ReturnToOpening();
            spdlog::info("studio camera: focus released; the pivot is the "
                         "configured one again{}.",
                         returned ? " and the shot returns to the opening "
                                    "framing"
                                  : "");
        }
    }

    void AddOrbit(float a_dx, float a_dy) {
        if (!g_s.armed) {
            return;
        }
        ++g_s.orbitEvents;
        const auto& cfg = Settings::GetSingleton();
        // ⚠ BOTH SIGNS ARE DELIBERATE AND NEITHER IS A SETTING. They were
        // chosen by dragging, not by reasoning, and they are the pair the field
        // settled on. An invert toggle existed and was removed rather than
        // defaulted the other way: one correct answer beats a switch nobody
        // should have to find, and while it existed every rotation report would
        // have had to carry which way it was set before it meant anything.
        // The player has taken the yaw over; a later focus change must not take
        // it back. See autoYaw.
        g_s.autoYaw = false;
        g_s.orbitTarget.yaw += a_dx * cfg.cameraOrbitSensitivity;
        const float dPitch = a_dy * cfg.cameraOrbitSensitivity;
        // Same reasoning as AddZoom: before capture this is a delta about zero,
        // and clamping it against the absolute pitch limit is harmless only
        // because both are small. Clamped for real once the base is folded in.
        g_s.orbitTarget.pitch = g_s.captured
                                    ? P::ClampPitch(g_s.orbitTarget.pitch + dPitch)
                                    : g_s.orbitTarget.pitch + dPitch;
    }

    // ── REMOVED 2026-08-07: EnterEditorShot / ToggleEditorZoom /
    // SetEditorZoomHold, and the editorZoomHold + editorZoomReturn state they
    // drove. ────────────────────────────────────────────────────────────────
    //
    // They mapped the character editor's own LAlt zoom onto our track, for the
    // window where our re-stamp owned the camera node and the menu's advertised
    // control would otherwise have been writing into something we overwrote
    // every frame.
    //
    // The 2026-08-06 retreat ended that window. The studio camera now stands
    // down in that menu entirely: Bubble releases a live arm on the editor's
    // open edge, Tick refuses the menu, and LAlt goes back to the menu that
    // advertises it. So there is no longer a period where we own the node and
    // owe the editor a zoom mapping. All three had zero callers afterwards, and
    // the two state members became write-only.
    //
    // Deleted rather than left dormant because dead camera code is a trap: it
    // reads as live to whoever next opens this file, and it survived one night
    // of camera fixes precisely because everyone assumed it was doing something.

    void AddZoom(float a_steps) {
        ++g_s.wheelSeen;  // reached us at all, so the sink is seeing the wheel
        if (!g_s.armed) {
            return;
        }
        ++g_s.wheelTaken;  // and the layer was live to act on it
        // The wheel moves t by a fixed step per notch, so uniform feel is a
        // property of the design rather than something a curve has to
        // achieve: equal steps make equal distance RATIOS, which is how zoom
        // reads to a hand, and one notch is the same felt step at every
        // distance because the track is log-normalised.
        const float step = a_steps * TrackStep();
        // ⚠ BEFORE CAPTURE THERE IS NO PLACE ON THE TRACK TO STAND - the
        // found framing is what decides where the shot opens - so the travel
        // is parked and Capture folds it onto the opening position. The same
        // park the old delta-distance carried its own warning about.
        if (!g_s.captured) {
            g_s.preCaptureZoom -= step;
            return;
        }
        // The far side has give and the near side does not. Coming in, there
        // is a body in the way and passing through it is never what anyone
        // wanted; going out there is only empty room, so the boundary can
        // afford to be felt rather than hit - the ceiling is the same
        // overshoot band the wheel always had, expressed in t, and the
        // recoil in Tick draws the target back to the far stop.
        g_s.trackTarget = P::ClampTrack(
            g_s.trackTarget - step,
            P::TrackCeiling(g_s.minDistance, SoftMaxDistance()));
    }

    void AddPan(float a_dx, float a_dy) {
        if (!g_s.armed) {
            return;
        }
        const auto& cfg = Settings::GetSingleton();
        // ⚠ BOTH SIGNS ARE THE FIELD'S, like the orbit's: chosen by
        // dragging, not by metaphor, and it took three rounds to settle
        // them - a grab pair, then the vertical alone flipped, then both
        // flipped again on the field's word. Do not reason about these;
        // change them only when the field says so. Scaled by the shot's
        // distance so the motion tracks the cursor about 1:1 at any zoom;
        // the default factor is the screen's world width per mouse count at
        // the menus' optics.
        const float d =
            g_s.captured ? g_s.orbit.distance : cfg.cameraOpenDistance;
        const float k = cfg.cameraPanSensitivity * d;
        // Give past the boundary like the wheel's far stop; the recoil in
        // Tick brings an overshot pan back to the band's edge. Height and
        // lateral stay ordinary offsets composing after the track, so the
        // pan needs no state of its own and clears with the usual resets.
        const float give = PanBound() * P::kOvershoot;
        g_s.lateralTarget = P::ClampAbs(g_s.lateralTarget + a_dx * k, give);
        g_s.heightTarget = P::ClampBand(g_s.heightTarget + a_dy * k,
                                        -PanBoundDown() * P::kOvershoot, give);
        g_s.panned = true;
    }

    bool CursorInZone() {
        // ⚠ THE ITEM PREVIEW OWNS THE MOUSE WHILE IT IS UP. Inspecting an item
        // in 3D is its own drag-to-turn, wheel-to-zoom mode, and two cameras on
        // one mouse means the item and the character both move for every
        // gesture. zoomProgress is the engine's own answer to "is an item being
        // inspected", and it is the same gate MenuInputGate already uses to
        // hand the gamepad's rotate back to the item preview, so both stand
        // down on the same signal rather than on two guesses at it.
        if (auto* inv = RE::Inventory3DManager::GetSingleton();
            inv && inv->GetRuntimeData().zoomProgress > 0.0f) {
            ++g_s.refusedItemPreview;
            return false;
        }
        float u = 0.0f;
        float v = 0.0f;
        if (!ResolveCursor(u, v)) {
            // No cursor, no drag. Refusing is the safe direction: a wrongly
            // refused drag costs the player a second attempt, a wrongly taken
            // one eats a click they meant for the menu.
            ++g_s.refusedNoCursor;
            return false;
        }
        const auto& cfg = Settings::GetSingleton();
        // ⚠ GRID INVENTORY GETS ITS OWN REGION. 0.44 is measured against SkyUI's
        // item columns and means nothing to another mod's window, so a drag
        // starting on the grid was being taken as a camera swing.
        const bool grid = Bubble::GetSingleton().CurrentMenuName() == "GridInventoryMenu";
        // ⚠ THE RECTANGLE FIRST, THE STATED ZONE ONLY AS THE FALLBACK. Grid
        // Inventory publishes where its window is and keeps it current, so the
        // exclusion can be exactly the grid instead of a boundary drawn to the
        // right of it. Everything above, below and left of the window is a
        // drag again, which the stated boundary was throwing away.
        bool gridKnown = false;
        if (grid && CursorOverGridWindow(u, v, gridKnown)) {
            ++g_s.refusedOverWindow;
            return false;
        }
        const P::Zone zone =
            (grid && !gridKnown)
                ? P::Zone{ cfg.gridZoneLeft, cfg.gridZoneTop, cfg.gridZoneRight,
                           cfg.gridZoneBottom }
                : (grid ? P::Zone{ 0.0f, 0.0f, 1.0f, 1.0f }
                        : P::Zone{ cfg.cameraZoneLeft, cfg.cameraZoneTop,
                                   cfg.cameraZoneRight, cfg.cameraZoneBottom });
        // The strip and Fitting Room's editor both sit inside this region, and a
        // drag starting on either belongs to it. The bar publishes the answer
        // for every FLICK window, not only its own.
        if (!P::InZone(u, v, 1.0f, 1.0f, zone)) {
            ++g_s.refusedOutside;
            g_s.refusedUMin = (std::min)(g_s.refusedUMin, u);
            g_s.refusedUMax = (std::max)(g_s.refusedUMax, u);
            return false;
        }
        if (ActionBar::CursorOverBar(u, v)) {
            ++g_s.refusedOverWindow;
            return false;
        }
        // ⚠ THE INSTRUMENT FOR THE ONE NUMBER THAT IS NOT MEASURED. fGridZoneLeft
        // ships as a starting point rather than a finding, so the first drag
        // taken under Grid Inventory says where it was: a boundary that is still
        // cutting across the grid shows up as a u below the character rather
        // than as another field round spent guessing. Once per session, and only
        // in that menu.
        if (grid) {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                spdlog::info("studio camera: first drag taken in Grid Inventory at "
                             "({:.3f},{:.3f}); its window {} read from "
                             "GridInventory_ui.ini, so the exclusion is {}.",
                             u, v, gridKnown ? "WAS" : "was NOT",
                             gridKnown ? "the window itself"
                                       : "the stated fGridZone keys");
            }
        }
        return true;
    }

    void Tick(float a_dt, RE::Actor* a_subject) {
        if (!g_s.armed || !Settings::GetSingleton().studioCamera) {
            return;
        }
        auto* root = CameraRoot();
        if (!root) {
            return;
        }
        auto* subject = SubjectOrPlayer(a_subject);

        // ⚠ THE CHARACTER EDITOR IS NOT THIS CAMERA'S ROOM (2026-08-06).
        // That menu brings a complete camera of its own - LAlt, prompts, a
        // whole Camera tab - and owning the node made every one of those
        // controls a lie that a night of correct fixes could not end.
        // Bubble releases the arm on the editor's open edge; this gate is
        // the same decision defended locally, so an editor-first arm can
        // never capture and a parked ask can never fire in there.
        if (Bubble::IsRaceMenuOpen()) {
            return;
        }

        // The deferred opening shot. Consumed before anything below reads the
        // target, and only while the editor is still the menu on screen - a
        // latch that outlived its menu would yank a later shot to the face.
        if (g_s.pendingEditorShot) {
            g_s.pendingEditorShot = false;
            if (g_s.captured && Bubble::IsRaceMenuOpen()) {
                g_s.trackTarget = 0.0f;  // the same keyframe LAlt eases to
                // ⚠ THE PHASE SNAPS WITH THE TARGET. The 2026-08-06 field run
                // proved the target write lands and the ease completes
                // (distance ran 35.0 in both live-arm summaries) - and that
                // the ease itself was the bug: it starts from the INVENTORY'S
                // phase, so the editor opened on the wide inventory shot and
                // spent two seconds gliding in under a button bar already
                // reading "Zoom Out". A menu switch is a cut, not a camera
                // move; LAlt's ease is for a press INSIDE the editor.
                g_s.trackT = 0.0f;
                spdlog::info("studio camera: opening face composition applied "
                             "(track target 0, phase snapped).");
            }
        }

        if (!g_s.captured) {
            // Nothing has been asked for, so nothing is touched. This is what
            // makes an untouched menu byte-identical to the shot OwnView built.
            //
            // ⚠ THE CHARACTER EDITOR HAS NOW EARNED THIS RULE TWICE. Both
            // exceptions that capture-at-armed it are gone: the first depended
            // on OwnView framing the editor and went out with it (OwnView is
            // back in the editor since the tween acquittal - this exception is
            // NOT; see ArmOwnViewIfOurs), and the second (2026-08-05) owned the shot
            // from the arm to kill the opening lunge - and then spent three
            // field passes chasing what the lunge had been doing for free:
            // a distance that read as "way off" until the optics were done
            // (0.7 of the range IS a full-body at this FOV), then a yaw
            // looking at her profile, then a pan off the slider panel. Each
            // fix was correct and the sum was still a bespoke editor camera.
            // The player's call, and the right one: ONE system everywhere -
            // the menu frames its own opening, lunge included, and ours takes
            // over at the first drag, in the editor exactly as in Fitting
            // Room and every bubbled menu. LAlt keeps working either way:
            // the menu's own zoom until we capture, ours after
            // (SetEditorZoomHold is captured-gated).
            if (!AnyRequest()) {
                return;
            }
            if (!Capture(root, subject)) {
                return;
            }
        }

        // Somebody else is also writing this camera. That used to force a
        // re-capture, which measured the orbit against a reference that moved
        // every frame; now the position and rotation are both built from state
        // we own, so the other writer simply loses and this is a note.
        //
        // Kept because it names WHO the player is fighting when a menu behaves
        // oddly. The 2026-08-04 run had Show Player In Inventory framing these
        // menus and rewriting the camera every frame, which is not visible from
        // anywhere else in the log: own view declines silently when another mod
        // covers a menu.
        // ⚠ A SUBJECT CHANGE DELIBERATELY KEEPS THE PLAYER'S FRAMING. An earlier
        // build re-read the shot here, on the reasoning that a distance measured
        // against one character is meaningless for another. The field killed it:
        // the framed subject flips back and forth on its own, three times in
        // three seconds in the 2026-08-04 log, and every flip threw away the
        // angle and distance the player had just set. It read as the camera
        // being impossible to get into.
        //
        // Nothing needs re-reading anyway. Apply resolves the pivot fresh every
        // tick, so the camera simply swings to circle whoever is now the subject
        // at the distance and angle already chosen, which is what someone
        // comparing two characters would want.
        if (const std::uint32_t nowId = subject ? subject->GetFormID() : 0;
            nowId != g_s.subjectId) {
            g_s.subjectId = nowId;
            g_s.bodyHeight0 = -1.0f;  // a different body: remeasure the lengths
            g_s.groundDrop0 = 0.0f;   // and the ground it was measured from
            g_s.groundFromMount0 = false;
        }

        if (ForeignWrite(root)) {
            ++g_s.foreignFrames;  // counted every time; the line prints once
            if (!g_s.loggedForeignWrite) {
                g_s.loggedForeignWrite = true;
                spdlog::info("studio camera: another mod is writing this camera too, "
                             "so the framing only becomes ours once you drag or "
                             "scroll. Everything below this point is ours.");
            }
        }

        // One rate for every axis, so distance and angle arrive together
        // instead of the shot sliding into place after it has finished turning.
        const float rate = P::RateFromSmoothing(Settings::GetSingleton().cameraSmoothing);

        // The rubber band acts on the TARGET, in track units: past the far
        // stop the target is drawn back to t = 1, slower than the main easing
        // so the pull and the ordinary motion compose into one move instead
        // of two fighting a frame apart. The near stop is hard and gets no
        // band.
        g_s.trackTarget =
            P::Recoil(g_s.trackTarget, 1.0f, a_dt, P::RecoilRateFor(rate));

        // The pan's band, same give-and-return but symmetric: an overshot
        // pan is drawn back to the boundary, so the frame can sit off the
        // subject but never lose them.
        g_s.lateralTarget = P::RecoilAbs(g_s.lateralTarget, PanBound(), a_dt,
                                         P::RecoilRateFor(rate));
        g_s.heightTarget = P::RecoilBand(g_s.heightTarget, -PanBoundDown(),
                                         PanBound(), a_dt, P::RecoilRateFor(rate));

        // The one eased value. Distance here and pivot height in Apply both
        // derive from trackT, which is what keeps them in phase by
        // construction rather than by tuning.
        g_s.trackT = P::Ease(g_s.trackT, g_s.trackTarget, a_dt, rate);
        g_s.orbit.distance =
            P::TrackDistance(g_s.trackT, g_s.minDistance, SoftMaxDistance());
        g_s.orbit.yaw = P::Ease(g_s.orbit.yaw, g_s.orbitTarget.yaw, a_dt, rate);
        g_s.orbit.pitch = P::Ease(g_s.orbit.pitch, g_s.orbitTarget.pitch, a_dt, rate);
        g_s.height = P::Ease(g_s.height, g_s.heightTarget, a_dt, rate);
        g_s.lateral = P::Ease(g_s.lateral, g_s.lateralTarget, a_dt, rate);

        g_s.distanceLow = (std::min)(g_s.distanceLow, g_s.orbit.distance);
        g_s.distanceHigh = (std::max)(g_s.distanceHigh, g_s.orbit.distance);
        g_s.yawLow = (std::min)(g_s.yawLow, g_s.orbit.yaw);
        g_s.yawHigh = (std::max)(g_s.yawHigh, g_s.orbit.yaw);
        g_s.pitchLow = (std::min)(g_s.pitchLow, g_s.orbit.pitch);
        g_s.pitchHigh = (std::max)(g_s.pitchHigh, g_s.orbit.pitch);
        ++g_s.dragFrames;

        Apply(root, subject, a_dt, rate);
    }

}  // namespace MTB::StudioCamera

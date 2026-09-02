#pragma once

namespace MTB::PathCullPolicy {

    // F-30. The per-node decision for the interior path cull, computed from the
    // CELL DOWN rather than from the player up.
    //
    // The climb this replaces assumed the player's chain passes through the
    // cell's big object list, and in a room-bound interior it does not: the
    // engine parents him under the room's portal geometry, or a level higher
    // again, so the climb culled the dozen things sharing his room and left the
    // rest of the inn rendering (field 2026-08-20, `declutter path` lines:
    // depth 4 reached 1736-1780 siblings and worked, depth 5 through 'Portal
    // Shared Geometry' reached 67-80 and failed). Descending from cell3D and
    // culling every branch that does not lead to the cast makes the tree shape
    // stop mattering: portals, room bounds, extra levels and which model
    // Get3D() hands back are all just nodes the keep-set either contains or
    // does not.
    struct Input {
        // The node IS a cast member's own 3D root (player, framed companion).
        bool isCastRoot{ false };
        // The node lies on some cast member's chain up to cell3D (keep-set).
        bool isCastAncestor{ false };
        // One of our own studio-rig nodes (MTB_ prefix). A light whose OWN
        // node is culled is skipped by the light gathering (field r7), so the
        // rig must never be culled by the sweep that clears its stage.
        bool isOurs{ false };
        // Already culled before we arrived - another mod's hide, or our
        // player-hide. Left alone and never recorded, so the restore cannot
        // un-hide what someone else hid (the g_hidden lesson).
        bool alreadyCulled{ false };
    };

    enum class Action {
        kDescend,    // on the keep path: spare it, decide its children
        kKeepWhole,  // cast root: spare the entire subtree, stop descending
        // Off the keep path: cull it, record it - and keep culling all the way
        // down. Field 2026-08-20 04:33 proved a branch-root cull alone is not
        // enough: whatever renders a room-bound interior reaches rooms without
        // consulting the container ancestors, so the flags held for a whole
        // arm and the inn rendered anyway, while the depth-4 shape's ~1740
        // INDIVIDUAL culls worked. The engine layer implements the deep pass
        // by re-entering this decision with the keep-set inputs forced false.
        kCull,
        kLeave       // spare it without recording, stop descending
    };

    // ⚠⚠ THE CAST-ROOT TEST IS FIRST AND MUST STAY FIRST. A cast root is also
    // in the keep-set (the ancestor walk starts at it), and letting the
    // ancestor answer win would DESCEND INTO THE PLAYER and cull his own arms
    // and armour - the one way to get this fix badly wrong.
    [[nodiscard]] constexpr Action Decide(const Input& a_input) {
        if (a_input.isCastRoot) {
            return Action::kKeepWhole;
        }
        if (a_input.isCastAncestor) {
            return Action::kDescend;
        }
        if (a_input.isOurs || a_input.alreadyCulled) {
            return Action::kLeave;
        }
        return Action::kCull;
    }

}  // namespace MTB::PathCullPolicy

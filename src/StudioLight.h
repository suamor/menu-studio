#pragma once

namespace MTB {
    // "Studio lighting": while the bubble is armed in an interior, override
    // the cell's INTERIOR_DATA with the [Lighting] preset values - even
    // ambient/DALC fill, soft directional, fog (the fog color IS the void's
    // backdrop color) - clear the lighting-template inherit flags for the
    // overridden channels, neutralize the cell's imagespace (tint/HDR), and
    // force the Sky's interior re-ingest (the renderer only ingests cell
    // lighting at cell attach; docs/STATUS.md has the chain). Original data
    // saved on arm and restored on disarm/close/ForceReset.
    namespace StudioLight {
        void Apply();        // arm edge (no-op outside interiors / non-void modes)
        void LiveRefresh();  // mid-arm settings change: rewrite + re-ingest
        void Restore();      // disarm edge / ForceReset
        // r40: the sky MODE alone crosses the exit's unpaused window early -
        // a kNone frame there makes the engine stop the weather's rain loop
        // for good (transition-triggered audio). Close edge restores it;
        // a switch re-open parks it again.
        void RestoreSkyModeEarly();  // bubble-menu close edge
        void ReparkSkyMode();        // bubble-menu open while armed (switch)

        // r46 (user, field: the sun outside still lights the character).
        // OUTDOORS NOTHING ABOVE HAS EVER REACHED THE LIGHTING. Apply's cell
        // override needs INTERIOR_DATA an exterior does not have, and
        // Declutter's r33 light cut walks the CELL's scene graph, which the sun
        // does not live in - F-18 shipped saying so, "exteriors, sun untouched".
        // This parks the sun, the cloud light beside it and the sky's
        // directional ambient for the duration of a void, and releases them
        // again everywhere else.
        //
        // ⚠ STATE-BASED AND IDEMPOTENT, not an edge. It decides park-or-release
        // from the settings and the cell every time it is called, which is what
        // lets the armed tick call it as a re-assert: Sky::Update runs in the
        // unpaused switch and exit windows and repaints both of these from the
        // weather, and a park that only fired on the arm edge would lose them
        // there. Cheap enough for that - a few pointer reads and compares when
        // the answer has not changed.
        void SyncExteriorSun();
    }
}

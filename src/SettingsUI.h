#pragma once

namespace MTB::SettingsUI {

    // kDataLoaded, after Settings::Load(). Connects to FLICK, registers the
    // sidebar tool, and installs the action bar.
    void Register();

    // The panel's contents, with no window or popup of its own. The FLICK
    // sidebar tool draws it inside FLICK's chrome; the strip's gear draws it
    // inside a popup the bar opens itself, so the settings come out AT the cog
    // with no frame and no title - the shape Fitting Room's gear already uses.
    void DrawPanelBody();

    // Popup id, shared by the gear that opens it and the bar that draws it.
    // They have to meet in the same ImGui context, which is why both live on
    // the bar.
    inline constexpr const char* kPopupId = "##ms_settings";

}  // namespace MTB::SettingsUI

#pragma once

//
// langsel - the culture the overlay speaks, fetched and applied. LOOP THREAD ONLY.
//
// The decision is lang::decide (PURE, tested): the `language` override, the game's own
// GameUserSettings.ini `[Internationalization] Language=`, the Windows display language,
// English. This module only fetches the three inputs - one small file read and two Win32
// calls - and stores the answer with lang::set_active, which the render thread reads with
// a relaxed load. It touches no UObject, no D3D12 and no ImGui: the overlay's font follows
// the active culture on the render thread by itself.
//

#include <cstdint>

namespace lsel
{
    // Decides the culture now and makes it the active one, with one log line whenever the
    // answer - the culture, or who decided it - moved. True when the culture moved. Called
    // before every load of names-bearing data (the first marker load, a re-enable, F5), so
    // that load reads the names of the culture in force.
    bool refresh();

    // Every loop tick while the mod runs: refresh() when the `language` setting moved (the
    // F2 panel), or - looked at once a second - when the game rewrote its ini. True when the
    // culture moved, and the caller reloads the names-bearing data.
    bool on_update(std::uint64_t now);
} // namespace lsel

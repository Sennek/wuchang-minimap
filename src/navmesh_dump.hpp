#pragma once

//
// navmesh_dump - locates the game's Recast/Detour navmesh in memory and writes the
// live tiles out as JSON. Logs to ue4ss\UE4SS.log with the "[navmesh]" prefix.
//

namespace navmesh
{
    // Call once, after UE4SS reports the Unreal module is up.
    void on_unreal_init();

    // Call every UE4SS tick; self-throttling.
    void on_update();

    bool enabled();

    // Force a dump of every agent. Any thread.
    void request_dump();
} // namespace navmesh

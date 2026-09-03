#pragma once

//
// navmesh_dump - locates the game's Recast/Detour navmesh in memory and writes the
// live tiles out as JSON.
//
// Entry points are deliberately tiny: everything else is internal to
// navmesh_dump.cpp so the module can be dropped into any UE4SS C++ mod.
//
//   navmesh::on_unreal_init()  - call once, after UE4SS says the Unreal module is up.
//   navmesh::on_update()       - call every UE4SS tick; self-throttling.
//   navmesh::enabled()         - is the dumper turned on in config.ini?
//   navmesh::request_dump()    - force a dump of every agent (any thread).
//
// Behaviour, all of it logged to ue4ss\UE4SS.log with the "[navmesh]" prefix:
//   * every ~2 s it re-runs FindAllOf("RecastNavMesh"); 0 results is normal (main
//     menu / Lobby) and is reported once, then only when the count changes;
//   * the first time an actor is seen it discovers the offsets of
//     RecastNavMeshImpl -> dtNavMesh -> tiles by heuristic scan + validation;
//   * navmesh::request_dump() - the F2 Debug tab's button - forces a dump of all four
//     agents; there is no hotkey for it;
//   * a change in the set of live tiles triggers an automatic dump after a 3 s
//     debounce, which is what makes level streaming produce one file per area.
//

namespace navmesh
{
    void on_unreal_init();
    void on_update();
    bool enabled();
    void request_dump();
} // namespace navmesh

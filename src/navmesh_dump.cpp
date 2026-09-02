//
// navmesh_dump.cpp - find the game's Recast/Detour navmesh in memory and write the
// live tiles out as JSON.
//
// The whole module is written on the assumption that every struct layout it believes
// in is a guess. Nothing is hardcoded that can be derived, every derived value is
// validated against something the recon pass measured (TileSizeUU 1280, AgentRadius
// 34/60/90/120, the DNAV tile magic, tile bounds vs tile index), and every single raw
// read goes through mem::read (VirtualQuery + SEH). A wrong guess produces a log line,
// never a crash.
//
// Discovery chain, all of it logged:
//
//   ARecastNavMesh actor            FindAllOf("RecastNavMesh")
//     +-> FPImplRecastNavMesh*      scan the actor past its reflected properties for a
//     |                             pointer whose target's 2nd field is the actor
//     |                             itself (that is FPImplRecastNavMesh::NavMeshOwner)
//     +-> dtNavMesh*                first field of FPImplRecastNavMesh; validated by
//     |                             dtNavMeshParams (tileWidth == tileHeight ==
//     |                             TileSizeUU, sane maxTiles/maxPolys), float AND
//     |                             double layouts tried
//     +-> dtMeshTile[]              scan the pointer slots after m_params for an array
//     |                             holding pointers to DNAV headers; the gcd of the
//     |                             hit spacing recovers sizeof(dtMeshTile), which UE
//     |                             changes by adding fields
//     +-> dtMeshHeader              magic checked; bmin/bmax found by searching for six
//     |                             consecutive dtReal that form a <= one-tile box,
//     |                             scored against origin + index * tileWidth and
//     |                             against walkableRadius == AgentRadius
//     +-> polyCount/vertCount       searched in the int block and accepted only if
//                                   EVERY poly has 3..6 in-range indices and EVERY
//                                   vertex lies inside bmin..bmax
//

#include "navmesh_dump.hpp"

#include "mem.hpp"
#include "ue_min.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <String/StringType.hpp>

using namespace RC;
using RC::Unreal::FField;
using RC::Unreal::FFieldAccess;
using RC::Unreal::FProperty;
using RC::Unreal::UClass;
using RC::Unreal::UObject;
using RC::Unreal::UStruct;

namespace navmesh
{
    namespace
    {
        //==============================================================================
        // Configuration
        //==============================================================================

        // The agent whose navmesh becomes the minimap background. AgentRadius 34 is the
        // smallest of the four, so its mesh reaches furthest into narrow geometry.
        constexpr std::wstring_view kPrimaryAgent = L"Small";

        // ARecastNavMesh::TileSizeUU on this game. Used to validate dtNavMeshParams; the
        // value actually read off the actor by reflection wins whenever it is available.
        constexpr double kExpectedTileSizeUU = 1280.0;

        constexpr std::uint64_t kActorPollMs = 2000;  // FindAllOf + tile-set poll throttle
        constexpr std::uint64_t kDebounceMs = 3000;   // tile set changed -> auto dump
        constexpr std::uint64_t kHotkeyGuardMs = 500; // key-repeat guard

        // Where the blind scan for RecastNavMeshImpl starts / stops when reflection could
        // not tell us where the reflected properties end.
        constexpr std::size_t kScanStartFallback = 0x300;
        constexpr std::size_t kScanEndFallback = 0x1200;

        // Detour tile-header magic: the four characters D N A V.
        constexpr std::uint32_t kDtNavMeshMagic = 0x444E4156u;

        // Sanity envelopes. Deliberately wide: they only have to exclude garbage.
        constexpr int kMaxTilesLimit = 65536;
        constexpr int kMaxPolysPerTile = 32768;
        constexpr int kMaxVertsPerTile = 262144;
        constexpr double kBoundsSlackUU = 8.0;

        //==============================================================================
        // Logging
        //==============================================================================

        void emit(const std::wstring& line)
        {
            Output::send<LogLevel::Verbose>(STR("[navmesh] {}\n"), line);
        }

        template <typename... Args>
        void logf(std::wformat_string<Args...> fmt, Args&&... args)
        {
            emit(std::format(fmt, std::forward<Args>(args)...));
        }

        std::wstring hex(std::uint64_t v)
        {
            return std::format(L"0x{:X}", v);
        }

        std::wstring hex_ptr(const void* p)
        {
            return hex(reinterpret_cast<std::uint64_t>(p));
        }

        std::wstring opt_f(const std::optional<float>& v)
        {
            return v ? std::format(L"{:.1f}", static_cast<double>(*v)) : std::wstring{L"?"};
        }

        std::wstring opt_i(const std::optional<std::int32_t>& v)
        {
            return v ? std::format(L"{}", *v) : std::wstring{L"?"};
        }

        //==============================================================================
        // Reflection: property offsets of a UClass
        //==============================================================================

        struct PropInfo
        {
            std::size_t offset = 0;
            int element_size = 0;
        };

        struct ClassProps
        {
            std::unordered_map<std::wstring, PropInfo> by_name;
            // max(offset + element_size) across the whole super-struct chain: where the
            // reflected part of the object ends and the C++-only members begin.
            std::size_t reflected_end = 0;
            int structure_size = 0;
            int properties_size = 0;
            int walked_classes = 0;
        };

        // Walks the FField child-property list of the class and of every super struct.
        // A null head (a pure-native class) simply yields nothing - no error.
        ClassProps collect_props(UClass* cls)
        {
            ClassProps out{};
            if (cls == nullptr)
            {
                return out;
            }

            out.structure_size = cls->GetStructureSize();
            out.properties_size = cls->GetPropertiesSize();

            auto* current = static_cast<UStruct*>(cls);
            for (int depth = 0; current != nullptr && depth < 32; ++depth)
            {
                ++out.walked_classes;
                FField* field = current->GetChildProperties();
                for (int i = 0; field != nullptr && i < 4096; ++i)
                {
                    auto* prop = static_cast<FProperty*>(field);
                    const int offset = prop->GetOffset_Internal();
                    const int size = prop->GetElementSize();
                    if (offset >= 0 && size > 0 && offset < 0x100000)
                    {
                        const auto end = static_cast<std::size_t>(offset) + static_cast<std::size_t>(size);
                        out.reflected_end = (std::max)(out.reflected_end, end);
                        std::wstring name = field->GetName();
                        if (!name.empty() && !out.by_name.contains(name))
                        {
                            out.by_name.emplace(std::move(name), PropInfo{static_cast<std::size_t>(offset), size});
                        }
                    }
                    field = FFieldAccess::next(field);
                }
                current = current->GetSuperStruct();
            }
            return out;
        }

        std::optional<float> prop_float(const ClassProps& props, const void* obj, std::wstring_view name)
        {
            const auto it = props.by_name.find(std::wstring{name});
            if (it == props.by_name.end() || it->second.element_size != 4)
            {
                return std::nullopt;
            }
            float v = 0.0f;
            if (!mem::read_at(obj, it->second.offset, v) || !std::isfinite(v))
            {
                return std::nullopt;
            }
            return v;
        }

        std::optional<std::int32_t> prop_int(const ClassProps& props, const void* obj, std::wstring_view name)
        {
            const auto it = props.by_name.find(std::wstring{name});
            if (it == props.by_name.end() || it->second.element_size != 4)
            {
                return std::nullopt;
            }
            std::int32_t v = 0;
            if (!mem::read_at(obj, it->second.offset, v))
            {
                return std::nullopt;
            }
            return v;
        }

        //==============================================================================
        // dtNavMeshParams
        //==============================================================================

        // UE 5 compiles Detour with dtReal = double (large-world coordinates), but a
        // project can flip that back to float, so both layouts are tried and the one
        // that validates is logged.
        struct NavParams
        {
            double orig[3]{};
            double tile_width = 0.0;
            double tile_height = 0.0;
            std::int32_t max_tiles = 0;
            std::int32_t max_polys = 0;
        };

        template <typename Real>
        bool read_params_as(const void* dt, std::size_t off, NavParams& out)
        {
            struct Raw
            {
                Real orig[3];
                Real tile_width;
                Real tile_height;
                std::int32_t max_tiles;
                std::int32_t max_polys;
            };
            Raw raw{};
            if (!mem::read_at(dt, off, raw))
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                out.orig[i] = static_cast<double>(raw.orig[i]);
            }
            out.tile_width = static_cast<double>(raw.tile_width);
            out.tile_height = static_cast<double>(raw.tile_height);
            out.max_tiles = raw.max_tiles;
            out.max_polys = raw.max_polys;
            return true;
        }

        bool params_plausible(const NavParams& p, double expected_tile_size)
        {
            if (!std::isfinite(p.tile_width) || !std::isfinite(p.tile_height))
            {
                return false;
            }
            for (const double o : p.orig)
            {
                if (!std::isfinite(o) || std::fabs(o) > 1.0e7)
                {
                    return false;
                }
            }
            if (std::fabs(p.tile_width - p.tile_height) > 0.01)
            {
                return false;
            }
            if (std::fabs(p.tile_width - expected_tile_size) > 1.0)
            {
                return false;
            }
            if (p.max_tiles < 1 || p.max_tiles > kMaxTilesLimit)
            {
                return false;
            }
            return p.max_polys >= 1;
        }

        //==============================================================================
        // Discovered layouts
        //==============================================================================

        struct MeshLayout
        {
            bool impl_found = false;
            bool detour_found = false;

            std::size_t impl_offset = 0;   // actor + impl_offset -> FPImplRecastNavMesh*
            std::size_t params_offset = 0; // dtNavMesh + params_offset -> dtNavMeshParams
            bool params_double = true;

            bool tiles_found = false;
            std::size_t tiles_offset = 0; // dtNavMesh + tiles_offset -> dtMeshTile* (array)
            std::size_t tile_stride = 0;  // sizeof(dtMeshTile) as this build compiled it
            int tile_finds = 0;           // DNAV hits the stride was derived from

            std::wstring note; // human-readable reason when something is missing
        };

        struct HeaderLayout
        {
            bool found = false;
            bool header_double = true;         // dtReal inside dtMeshHeader
            bool verts_double = true;          // dtReal in the tile vertex array
            std::size_t bmin_offset = 0;       // bmax at bmin_offset + 3*sizeof(dtReal)
            std::size_t walkable_offset = 0;   // walkableHeight, immediately before bmin
            std::size_t poly_count_offset = 0; // vertCount at +4
            int verts_per_poly = 6;            // DT_VERTS_PER_POLYGON
            std::size_t poly_stride = 32;      // sizeof(dtPoly)
            std::wstring note;
        };

        bool points_at_dnav(const void* p)
        {
            std::uint32_t magic = 0;
            return mem::plausible_ptr(p) && mem::read<std::uint32_t>(p, magic) && magic == kDtNavMeshMagic;
        }

        //==============================================================================
        // Step 1: actor -> FPImplRecastNavMesh -> dtNavMesh
        //==============================================================================
        //
        // ARecastNavMesh declares `FPImplRecastNavMesh* RecastNavMeshImpl` right after its
        // reflected UPROPERTYs, and FPImplRecastNavMesh starts with
        //     dtNavMesh*       DetourNavMesh;
        //     ARecastNavMesh*  NavMeshOwner;
        //
        // so the recogniser is "a pointer slot inside the actor whose target's SECOND
        // pointer is the actor itself". That back-pointer is already almost unique; when
        // the first pointer also validates as a dtNavMesh the identification is certain.
        // No offset is hardcoded - the scan starts where reflection says the reflected
        // properties end and runs to the class's structure size.

        MeshLayout find_impl_and_detour(const void* actor, const ClassProps& props, double expected_tile_size)
        {
            MeshLayout out{};

            std::size_t scan_begin = props.reflected_end;
            std::size_t scan_end = static_cast<std::size_t>((std::max)(props.structure_size, props.properties_size));
            const bool from_reflection = scan_begin >= 0x28 && scan_end > scan_begin;
            if (!from_reflection)
            {
                scan_begin = kScanStartFallback;
                scan_end = kScanEndFallback;
            }
            // Round down to a pointer boundary with a little slack: the last reflected
            // property can be followed by padding, and the impl pointer may precede it in
            // declaration order on some builds.
            scan_begin = (scan_begin >= 0x40 ? scan_begin - 0x40 : 0x28) & ~static_cast<std::size_t>(7);
            scan_end = (std::min)(scan_end + 0x80, static_cast<std::size_t>(0x4000));

            logf(L"  scan {} for RecastNavMeshImpl in [{}, {}) - {}; reflected props end {}, StructureSize {}, "
                 L"PropertiesSize {}, {} props over {} classes",
                 hex_ptr(actor),
                 hex(scan_begin),
                 hex(scan_end),
                 from_reflection ? L"range from reflection" : L"FALLBACK range, reflection yielded nothing",
                 hex(props.reflected_end),
                 props.structure_size,
                 props.properties_size,
                 static_cast<int>(props.by_name.size()),
                 props.walked_classes);

            std::size_t impl_offset_null_detour = 0;
            bool have_null_detour = false;

            for (std::size_t off = scan_begin; off + 8 <= scan_end; off += 8)
            {
                void* impl = nullptr;
                if (!mem::read_ptr(static_cast<const std::uint8_t*>(actor) + off, impl))
                {
                    continue;
                }
                if (!mem::readable(impl, 16))
                {
                    continue;
                }

                void* owner = nullptr;
                if (!mem::read_at(impl, 8, owner) || owner != actor)
                {
                    continue;
                }

                // The back-pointer matched: this is FPImplRecastNavMesh.
                void* detour = nullptr;
                const bool detour_ok =
                    mem::read_at(impl, 0, detour) && mem::plausible_ptr(detour) && mem::readable(detour, 256);
                if (!detour_ok)
                {
                    if (!have_null_detour)
                    {
                        have_null_detour = true;
                        impl_offset_null_detour = off;
                    }
                    continue;
                }

                // Validate the dtNavMesh. Stock Detour has no vtable so m_params sits at
                // offset 0, but a UE addition would shift it - hence the small sweep.
                for (const std::size_t params_off : {std::size_t{0}, std::size_t{8}, std::size_t{16}})
                {
                    for (const bool as_double : {true, false})
                    {
                        NavParams p{};
                        const bool ok = as_double ? read_params_as<double>(detour, params_off, p)
                                                  : read_params_as<float>(detour, params_off, p);
                        if (!ok || !params_plausible(p, expected_tile_size))
                        {
                            continue;
                        }
                        out.impl_found = true;
                        out.detour_found = true;
                        out.impl_offset = off;
                        out.params_offset = params_off;
                        out.params_double = as_double;
                        logf(L"  FOUND RecastNavMeshImpl at actor+{}   impl={}   dtNavMesh={}",
                             hex(off),
                             hex_ptr(impl),
                             hex_ptr(detour));
                        logf(L"  dtNavMeshParams at dtNavMesh+{} as {}: orig ({:.2f} {:.2f} {:.2f}) tileWidth {:.2f} "
                             L"tileHeight {:.2f} maxTiles {} maxPolys {}",
                             hex(params_off),
                             as_double ? L"double (UE5 LWC)" : L"float",
                             p.orig[0],
                             p.orig[1],
                             p.orig[2],
                             p.tile_width,
                             p.tile_height,
                             p.max_tiles,
                             p.max_polys);
                        return out;
                    }
                }
            }

            if (have_null_detour)
            {
                out.impl_found = true;
                out.impl_offset = impl_offset_null_detour;
                out.note = L"RecastNavMeshImpl found but DetourNavMesh is null - no navmesh data streamed in yet";
                logf(L"  RecastNavMeshImpl at actor+{} but DetourNavMesh is null/unreadable - no tile data loaded yet",
                     hex(impl_offset_null_detour));
                return out;
            }

            out.note = L"no pointer inside the actor targeted a struct whose 2nd field is the actor itself";
            logf(L"  NOT FOUND: {}", out.note);
            return out;
        }

        //==============================================================================
        // Step 2: dtNavMesh -> dtMeshTile array (base pointer and stride)
        //==============================================================================
        //
        // UE adds fields to dtMeshTile (off-mesh segments, clusters, dynamic links), so
        // sizeof(dtMeshTile) cannot be assumed. What IS stable is the head of the struct:
        //     unsigned int  salt;
        //     unsigned int  linksFreeList;
        //     dtMeshHeader* header;      <- always at +8
        //     dtPoly*       polys;       <- +16
        //     dtReal*       verts;       <- +24
        //
        // dtNavMesh::init() builds the free list back to front, so tiles are handed out
        // from index 0 upwards and the live ones cluster at the start of the array.
        // Scanning the array for pointers-to-DNAV therefore yields several hits spaced
        // exactly sizeof(dtMeshTile) apart; the gcd of the deltas recovers the stride even
        // when some tiles in between are free.

        struct TileArray
        {
            bool found = false;
            const std::uint8_t* base = nullptr;
            std::size_t offset_in_mesh = 0;
            std::size_t stride = 0; // 0 = could not be measured (only one live tile)
            int finds = 0;
        };

        TileArray find_tile_array(const void* detour, std::size_t params_off, bool params_double, int max_tiles)
        {
            TileArray best{};

            const std::size_t params_size = params_double ? 48u : 28u;
            const std::size_t scan_from = (params_off + params_size - 16) & ~static_cast<std::size_t>(7);
            const std::size_t scan_to = params_off + 512;

            for (std::size_t off = scan_from; off + 8 <= scan_to; off += 8)
            {
                void* candidate = nullptr;
                if (!mem::read_ptr(static_cast<const std::uint8_t*>(detour) + off, candidate))
                {
                    continue;
                }
                if (!mem::readable(candidate, 64))
                {
                    continue;
                }

                // Byte offsets inside the candidate array that hold a pointer to a DNAV
                // header.
                std::vector<std::size_t> finds;
                const auto* bytes = static_cast<const std::uint8_t*>(candidate);
                constexpr std::size_t kScanBytes = 24 * 1024;
                for (std::size_t k = 0; k + 8 <= kScanBytes && finds.size() < 48; k += 8)
                {
                    void* maybe_header = nullptr;
                    if (mem::read_ptr(bytes + k, maybe_header) && points_at_dnav(maybe_header))
                    {
                        finds.push_back(k);
                    }
                }
                if (finds.empty())
                {
                    continue;
                }

                std::size_t stride = 0;
                if (finds.size() >= 2)
                {
                    std::size_t g = 0;
                    for (std::size_t i = 1; i < finds.size(); ++i)
                    {
                        g = std::gcd(g, finds[i] - finds[0]);
                    }
                    // A dtMeshTile is at least ~15 pointers plus ints, and never a kilobyte.
                    // The header must sit at +8 within every tile, hence finds[0] % g == 8.
                    if (g >= 96 && g <= 1024 && (g % 8) == 0 && (finds[0] % g) == 8)
                    {
                        stride = g;
                    }
                    else
                    {
                        logf(L"  tile-array candidate dtNavMesh+{} -> {}: {} DNAV hits but the implied stride {} is "
                             L"implausible, rejected",
                             hex(off),
                             hex_ptr(candidate),
                             static_cast<int>(finds.size()),
                             hex(g));
                        continue;
                    }
                }

                if (static_cast<int>(finds.size()) > best.finds)
                {
                    best.found = true;
                    best.base = bytes;
                    best.offset_in_mesh = off;
                    best.stride = stride;
                    best.finds = static_cast<int>(finds.size());
                }
            }

            if (best.found)
            {
                logf(L"  dtMeshTile array at dtNavMesh+{} -> {}   {} DNAV header(s) seen, sizeof(dtMeshTile) = {} "
                     L"({} bytes), maxTiles {}",
                     hex(best.offset_in_mesh),
                     hex_ptr(best.base),
                     best.finds,
                     hex(best.stride),
                     static_cast<int>(best.stride),
                     max_tiles);
            }
            else
            {
                logf(L"  dtMeshTile array NOT found: no pointer slot after dtNavMeshParams led to an array holding "
                     L"DNAV headers - the mesh most likely has zero live tiles right now");
            }
            return best;
        }

        //==============================================================================
        // Step 3: dtMeshHeader field discovery
        //==============================================================================
        //
        // dtMeshHeader is { magic, version, x, y, layer, userId, polyCount, vertCount,
        // ...more counts..., walkableHeight, walkableRadius, walkableClimb, bmin[3],
        // bmax[3], bvQuantFactor } and UE inserts extra count fields, so the byte offsets
        // of bmin and of polyCount both move. Both are found by search + validation.
        // magic/version/x/y are the only offsets taken on faith (0/4/8/12); x and y are
        // then cross-checked against origin + index * tileWidth.

        struct BoundsCandidate
        {
            std::size_t offset = 0;
            bool as_double = true;
            double bmin[3]{};
            double bmax[3]{};
            int score = 0;
        };

        template <typename Real>
        bool read3(const void* p, std::size_t off, double (&out)[3])
        {
            Real raw[3]{};
            if (!mem::read_at(p, off, raw))
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                const double v = static_cast<double>(raw[i]);
                if (!std::isfinite(v))
                {
                    return false;
                }
                out[i] = v;
            }
            return true;
        }

        std::optional<BoundsCandidate> find_bounds(const void* header,
                                                   const NavParams& params,
                                                   const std::optional<float>& agent_radius,
                                                   const std::optional<float>& agent_height,
                                                   std::int32_t tile_x,
                                                   std::int32_t tile_y)
        {
            std::optional<BoundsCandidate> best;

            for (const bool as_double : {true, false})
            {
                const std::size_t rs = as_double ? 8u : 4u;
                for (std::size_t off = 24; off + 6 * rs <= 512; off += rs)
                {
                    BoundsCandidate c{};
                    c.offset = off;
                    c.as_double = as_double;
                    const bool ok =
                        as_double ? (read3<double>(header, off, c.bmin) && read3<double>(header, off + 3 * rs, c.bmax))
                                  : (read3<float>(header, off, c.bmin) && read3<float>(header, off + 3 * rs, c.bmax));
                    if (!ok)
                    {
                        continue;
                    }

                    bool sane = true;
                    for (int i = 0; i < 3 && sane; ++i)
                    {
                        sane = std::fabs(c.bmin[i]) <= 1.0e7 && std::fabs(c.bmax[i]) <= 1.0e7;
                    }
                    if (!sane)
                    {
                        continue;
                    }

                    const double ex = c.bmax[0] - c.bmin[0];
                    const double ey = c.bmax[1] - c.bmin[1];
                    const double ez = c.bmax[2] - c.bmin[2];
                    // A tile is TileSizeUU across; allow 10 % for the border expansion.
                    const double max_xy = params.tile_width * 1.1;
                    if (!(ex > 1.0 && ey > 1.0 && ez > 0.0) || ex > max_xy || ey > max_xy || ez > 200000.0)
                    {
                        continue;
                    }

                    c.score = 1;

                    // The tile's world X/Y must be origin + index * tileWidth.
                    const double want_x = params.orig[0] + static_cast<double>(tile_x) * params.tile_width;
                    const double want_y = params.orig[1] + static_cast<double>(tile_y) * params.tile_height;
                    if (std::fabs(c.bmin[0] - want_x) < params.tile_width * 0.25)
                    {
                        c.score += 4;
                    }
                    if (std::fabs(c.bmin[1] - want_y) < params.tile_height * 0.25)
                    {
                        c.score += 4;
                    }

                    // walkableHeight / walkableRadius / walkableClimb sit right before bmin
                    // and must equal the actor's AgentHeight / AgentRadius.
                    if (off >= 3 * rs)
                    {
                        double walk[3]{};
                        const bool wok = as_double ? read3<double>(header, off - 3 * rs, walk)
                                                   : read3<float>(header, off - 3 * rs, walk);
                        if (wok)
                        {
                            if (agent_height && std::fabs(walk[0] - static_cast<double>(*agent_height)) < 2.0)
                            {
                                c.score += 3;
                            }
                            if (agent_radius && std::fabs(walk[1] - static_cast<double>(*agent_radius)) < 2.0)
                            {
                                c.score += 3;
                            }
                        }
                    }

                    if (!best || c.score > best->score)
                    {
                        best = c;
                    }
                }
            }
            return best;
        }

        //==============================================================================
        // dtPoly
        //==============================================================================
        //
        // { unsigned int firstLink; unsigned short verts[VPP]; unsigned short neis[VPP];
        //   unsigned short flags; unsigned char vertCount; unsigned char areaAndtype; }
        // VPP is DT_VERTS_PER_POLYGON (6 in UE), so sizeof(dtPoly) = 8 + 4 * VPP.

        struct PolyRaw
        {
            std::uint16_t verts[8]{};
            std::uint16_t flags = 0;
            std::uint8_t vert_count = 0;
            std::uint8_t area_and_type = 0;
        };

        bool read_poly(const void* polys, std::size_t index, std::size_t stride, int vpp, PolyRaw& out)
        {
            const auto* base = static_cast<const std::uint8_t*>(polys) + index * stride;
            for (int i = 0; i < vpp; ++i)
            {
                std::uint16_t v = 0;
                if (!mem::read_at(base, 4 + static_cast<std::size_t>(i) * 2, v))
                {
                    return false;
                }
                out.verts[i] = v;
            }
            const std::size_t tail = 4 + static_cast<std::size_t>(vpp) * 4;
            return mem::read_at(base, tail, out.flags) && mem::read_at(base, tail + 2, out.vert_count) &&
                   mem::read_at(base, tail + 3, out.area_and_type);
        }

        //==============================================================================
        // Extracted tile data
        //==============================================================================

        struct Poly
        {
            std::array<std::uint16_t, 8> v{};
            int n = 0;
            std::uint8_t area = 0;
            std::uint8_t type = 0;
            std::uint16_t flags = 0;
        };

        struct Tile
        {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t layer = 0;
            double bmin[3]{};
            double bmax[3]{};
            std::vector<std::array<double, 3>> verts;
            std::vector<Poly> polys;
        };

        // Reads vert_count * 3 dtReal and reports how many are inside the tile bounds.
        template <typename Real>
        int count_verts_inside(const void* verts, int vert_count, const double (&bmin)[3], const double (&bmax)[3])
        {
            int inside = 0;
            for (int i = 0; i < vert_count; ++i)
            {
                Real raw[3]{};
                if (!mem::read_at(verts, static_cast<std::size_t>(i) * 3 * sizeof(Real), raw))
                {
                    return inside;
                }
                bool ok = true;
                for (int a = 0; a < 3 && ok; ++a)
                {
                    const double val = static_cast<double>(raw[a]);
                    ok = std::isfinite(val) && val >= bmin[a] - kBoundsSlackUU && val <= bmax[a] + kBoundsSlackUU;
                }
                if (ok)
                {
                    ++inside;
                }
            }
            return inside;
        }

        //==============================================================================
        // Per-agent state
        //==============================================================================

        struct AgentState
        {
            std::wstring agent;      // Small / Big / BitFat / Giant
            std::wstring actor_name; // RecastNavMesh-Small
            std::wstring full_name;
            const void* actor = nullptr;

            ClassProps props;
            MeshLayout mesh;
            HeaderLayout header;

            std::optional<float> agent_radius;
            std::optional<float> agent_height;
            std::optional<float> tile_size_uu;
            std::optional<float> cell_size;
            std::optional<float> cell_height;
            std::optional<std::int32_t> tile_pool_size;
            std::optional<std::int32_t> poly_ref_tile_bits;
            std::optional<std::int32_t> poly_ref_poly_bits;
            std::optional<std::int32_t> poly_ref_salt_bits;

            NavParams params{};

            std::uint64_t tile_signature = 0;
            std::uint64_t signature_changed_at = 0;
            bool signature_seen = false;
            bool dumped_current_signature = false;
        };

        std::unordered_map<const void*, AgentState> g_agents;
        bool g_initialised = false;
        std::uint64_t g_last_poll = 0;
        std::uint64_t g_last_hotkey = 0;
        bool g_f6_was_down = false;
        int g_last_actor_count = -1;
        std::filesystem::path g_out_root;

        //==============================================================================
        // Output directory
        //==============================================================================

        std::filesystem::path resolve_out_root()
        {
            // main.dll lives in ...\ue4ss\Mods\WuchangMinimap\dlls, so the mod folder is
            // one level up. Fall back to the known install path, then to the cwd.
            HMODULE self = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(&resolve_out_root),
                                     &self) != 0 &&
                self != nullptr)
            {
                wchar_t buffer[MAX_PATH * 2]{};
                const DWORD n = ::GetModuleFileNameW(self, buffer, static_cast<DWORD>(std::size(buffer)));
                if (n > 0 && static_cast<std::size_t>(n) < std::size(buffer))
                {
                    std::error_code ec;
                    const std::filesystem::path dll{buffer};
                    const std::filesystem::path candidate = dll.parent_path().parent_path() / L"navmesh";
                    std::filesystem::create_directories(candidate, ec);
                    if (std::filesystem::exists(candidate))
                    {
                        return candidate;
                    }
                }
            }

            std::error_code ec;
            const std::filesystem::path fallback{
                LR"(E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\navmesh)"};
            std::filesystem::create_directories(fallback, ec);
            if (std::filesystem::exists(fallback))
            {
                return fallback;
            }
            return std::filesystem::current_path();
        }

        std::wstring timestamp()
        {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            return std::format(L"{:04}{:02}{:02}_{:02}{:02}{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }

        //==============================================================================
        // JSON writing
        //==============================================================================

        std::string to_utf8(std::wstring_view w)
        {
            if (w.empty())
            {
                return {};
            }
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0)
            {
                return {};
            }
            std::string out(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            // JSON strings must not carry raw backslashes (Windows paths do).
            std::string escaped;
            escaped.reserve(out.size() + 8);
            for (const char c : out)
            {
                if (c == '\\' || c == '"')
                {
                    escaped.push_back('\\');
                }
                escaped.push_back(c);
            }
            return escaped;
        }

        void json_opt_float(std::ofstream& f, const char* name, const std::optional<float>& v, bool& first)
        {
            f << (first ? "" : ", ");
            first = false;
            if (v)
            {
                f << std::format("\"{}\": {:.4f}", name, static_cast<double>(*v));
            }
            else
            {
                f << std::format("\"{}\": null", name);
            }
        }

        void json_opt_int(std::ofstream& f, const char* name, const std::optional<std::int32_t>& v, bool& first)
        {
            f << (first ? "" : ", ");
            first = false;
            if (v)
            {
                f << std::format("\"{}\": {}", name, *v);
            }
            else
            {
                f << std::format("\"{}\": null", name);
            }
        }

        bool write_json(const std::filesystem::path& path,
                        const AgentState& st,
                        const std::vector<Tile>& tiles,
                        std::size_t total_polys,
                        std::size_t total_verts,
                        std::wstring_view reason)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                return false;
            }

            f << "{\n";
            f << "  \"schema\": \"wuchang-navmesh-tiles/1\",\n";
            f << std::format("  \"generated\": \"{}\",\n", to_utf8(timestamp()));
            f << std::format("  \"agent\": \"{}\",\n", to_utf8(st.agent));
            f << std::format("  \"primary_agent\": {},\n", st.agent == kPrimaryAgent ? "true" : "false");
            f << std::format("  \"actor\": \"{}\",\n", to_utf8(st.actor_name));
            f << std::format("  \"actor_full_name\": \"{}\",\n", to_utf8(st.full_name));
            f << std::format("  \"actor_address\": \"{}\",\n", to_utf8(hex_ptr(st.actor)));
            if (!reason.empty())
            {
                f << std::format("  \"reason\": \"{}\",\n", to_utf8(reason));
            }

            f << "  \"settings\": {";
            bool first = true;
            json_opt_float(f, "AgentRadius", st.agent_radius, first);
            json_opt_float(f, "AgentHeight", st.agent_height, first);
            json_opt_float(f, "TileSizeUU", st.tile_size_uu, first);
            json_opt_float(f, "CellSize", st.cell_size, first);
            json_opt_float(f, "CellHeight", st.cell_height, first);
            json_opt_int(f, "TilePoolSize", st.tile_pool_size, first);
            json_opt_int(f, "PolyRefTileBits", st.poly_ref_tile_bits, first);
            json_opt_int(f, "PolyRefNavPolyBits", st.poly_ref_poly_bits, first);
            json_opt_int(f, "PolyRefSaltBits", st.poly_ref_salt_bits, first);
            f << "},\n";

            f << std::format("  \"dtnavmesh\": {{\"orig\": [{:.4f}, {:.4f}, {:.4f}], \"tile_width\": {:.4f}, "
                             "\"tile_height\": {:.4f}, \"max_tiles\": {}, \"max_polys\": {}}},\n",
                             st.params.orig[0],
                             st.params.orig[1],
                             st.params.orig[2],
                             st.params.tile_width,
                             st.params.tile_height,
                             st.params.max_tiles,
                             st.params.max_polys);

            f << std::format("  \"offsets\": {{\"impl_ptr\": \"{}\", \"params\": \"{}\", \"tiles_ptr\": \"{}\", "
                             "\"tile_stride\": \"{}\", \"tile_stride_from_hits\": {}, \"hdr_bmin\": \"{}\", "
                             "\"hdr_walkable\": \"{}\", \"hdr_poly_count\": \"{}\", \"poly_stride\": \"{}\", "
                             "\"verts_per_poly\": {}, \"params_layout\": \"{}\", \"header_layout\": \"{}\", "
                             "\"verts_layout\": \"{}\"}},\n",
                             to_utf8(hex(st.mesh.impl_offset)),
                             to_utf8(hex(st.mesh.params_offset)),
                             to_utf8(hex(st.mesh.tiles_offset)),
                             to_utf8(hex(st.mesh.tile_stride)),
                             st.mesh.tile_finds,
                             to_utf8(hex(st.header.bmin_offset)),
                             to_utf8(hex(st.header.walkable_offset)),
                             to_utf8(hex(st.header.poly_count_offset)),
                             to_utf8(hex(st.header.poly_stride)),
                             st.header.verts_per_poly,
                             st.mesh.params_double ? "double" : "float",
                             st.header.header_double ? "double" : "float",
                             st.header.verts_double ? "double" : "float");

            f << std::format("  \"tile_count\": {},\n  \"poly_count\": {},\n  \"vert_count\": {},\n",
                             tiles.size(),
                             total_polys,
                             total_verts);

            f << "  \"tiles\": [\n";
            for (std::size_t ti = 0; ti < tiles.size(); ++ti)
            {
                const Tile& t = tiles[ti];
                f << std::format("    {{\"x\": {}, \"y\": {}, \"layer\": {}, \"bmin\": [{:.3f}, {:.3f}, {:.3f}], "
                                 "\"bmax\": [{:.3f}, {:.3f}, {:.3f}],\n",
                                 t.x,
                                 t.y,
                                 t.layer,
                                 t.bmin[0],
                                 t.bmin[1],
                                 t.bmin[2],
                                 t.bmax[0],
                                 t.bmax[1],
                                 t.bmax[2]);
                f << "     \"verts\": [";
                for (std::size_t i = 0; i < t.verts.size(); ++i)
                {
                    if (i != 0)
                    {
                        f << ", ";
                    }
                    f << std::format("[{:.3f},{:.3f},{:.3f}]", t.verts[i][0], t.verts[i][1], t.verts[i][2]);
                }
                f << "],\n";
                f << "     \"polys\": [";
                for (std::size_t i = 0; i < t.polys.size(); ++i)
                {
                    if (i != 0)
                    {
                        f << ", ";
                    }
                    const Poly& p = t.polys[i];
                    f << "{\"v\": [";
                    for (int k = 0; k < p.n; ++k)
                    {
                        if (k != 0)
                        {
                            f << ",";
                        }
                        f << p.v[static_cast<std::size_t>(k)];
                    }
                    f << std::format("], \"area\": {}, \"type\": {}, \"flags\": {}}}",
                                     static_cast<int>(p.area),
                                     static_cast<int>(p.type),
                                     static_cast<int>(p.flags));
                }
                f << "]}";
                f << (ti + 1 == tiles.size() ? "\n" : ",\n");
            }
            f << "  ]\n";
            f << "}\n";
            return f.good();
        }

        //==============================================================================
        // Tile reading
        //==============================================================================

        // Learns the dtMeshHeader layout from one live tile. Logs every decision and the
        // one-line "PIN LINE" summary that lets us hardcode the offsets later if we want.
        bool learn_header_layout(const void* header, const void* polys, const void* verts, AgentState& st,
                                 std::int32_t tx, std::int32_t ty, std::int32_t tlayer)
        {
            const auto bounds = find_bounds(header, st.params, st.agent_radius, st.agent_height, tx, ty);
            if (!bounds)
            {
                st.header.note = L"could not locate bmin/bmax inside dtMeshHeader";
                logf(L"  header layout FAILED: {}", st.header.note);
                return false;
            }

            logf(L"  dtMeshHeader: bmin/bmax at +{} as {} (confidence score {}), tile x={} y={} layer={} bounds "
                 L"({:.1f} {:.1f} {:.1f}) .. ({:.1f} {:.1f} {:.1f})",
                 hex(bounds->offset),
                 bounds->as_double ? L"double" : L"float",
                 bounds->score,
                 tx,
                 ty,
                 tlayer,
                 bounds->bmin[0],
                 bounds->bmin[1],
                 bounds->bmin[2],
                 bounds->bmax[0],
                 bounds->bmax[1],
                 bounds->bmax[2]);

            const std::size_t rs = bounds->as_double ? 8u : 4u;

            for (std::size_t pc_off = 16; pc_off + 8 <= bounds->offset; pc_off += 4)
            {
                std::int32_t poly_count = 0;
                std::int32_t vert_count = 0;
                if (!mem::read_at(header, pc_off, poly_count) || !mem::read_at(header, pc_off + 4, vert_count))
                {
                    continue;
                }
                if (poly_count < 1 || poly_count > kMaxPolysPerTile || vert_count < 3 || vert_count > kMaxVertsPerTile)
                {
                    continue;
                }

                for (const bool verts_double : {true, false})
                {
                    const std::size_t vrs = verts_double ? 8u : 4u;
                    if (!mem::readable(verts, static_cast<std::size_t>(vert_count) * 3 * vrs))
                    {
                        continue;
                    }
                    const int inside = verts_double
                                           ? count_verts_inside<double>(verts, vert_count, bounds->bmin, bounds->bmax)
                                           : count_verts_inside<float>(verts, vert_count, bounds->bmin, bounds->bmax);
                    if (inside != vert_count)
                    {
                        continue;
                    }

                    for (const int vpp : {6, 8, 4})
                    {
                        const std::size_t stride = 8u + 4u * static_cast<std::size_t>(vpp);
                        if (!mem::readable(polys, static_cast<std::size_t>(poly_count) * stride))
                        {
                            continue;
                        }
                        bool all_ok = true;
                        for (std::int32_t i = 0; i < poly_count && all_ok; ++i)
                        {
                            PolyRaw pr{};
                            if (!read_poly(polys, static_cast<std::size_t>(i), stride, vpp, pr))
                            {
                                all_ok = false;
                                break;
                            }
                            // 2 = an off-mesh connection poly, still legal.
                            if (pr.vert_count < 2 || static_cast<int>(pr.vert_count) > vpp)
                            {
                                all_ok = false;
                                break;
                            }
                            for (int k = 0; k < static_cast<int>(pr.vert_count); ++k)
                            {
                                if (static_cast<std::int32_t>(pr.verts[k]) >= vert_count)
                                {
                                    all_ok = false;
                                    break;
                                }
                            }
                        }
                        if (!all_ok)
                        {
                            continue;
                        }

                        st.header.found = true;
                        st.header.header_double = bounds->as_double;
                        st.header.verts_double = verts_double;
                        st.header.bmin_offset = bounds->offset;
                        st.header.walkable_offset = bounds->offset >= 3 * rs ? bounds->offset - 3 * rs : 0;
                        st.header.poly_count_offset = pc_off;
                        st.header.verts_per_poly = vpp;
                        st.header.poly_stride = stride;

                        logf(L"  dtMeshHeader: polyCount at +{} = {}, vertCount at +{} = {}; tile verts are {}; "
                             L"sizeof(dtPoly) = {} with DT_VERTS_PER_POLYGON = {} - all {} polys and all {} verts "
                             L"validated",
                             hex(pc_off),
                             poly_count,
                             hex(pc_off + 4),
                             vert_count,
                             verts_double ? L"double" : L"float",
                             static_cast<int>(stride),
                             vpp,
                             poly_count,
                             vert_count);
                        logf(L"  PIN LINE {}: impl+{} params+{}/{} tiles+{} tileStride {} hdrBmin+{} hdrCounts+{}/{} "
                             L"dtPoly {}b vpp {} verts {}",
                             st.agent,
                             hex(st.mesh.impl_offset),
                             hex(st.mesh.params_offset),
                             st.mesh.params_double ? L"f64" : L"f32",
                             hex(st.mesh.tiles_offset),
                             hex(st.mesh.tile_stride),
                             hex(bounds->offset),
                             hex(pc_off),
                             bounds->as_double ? L"f64" : L"f32",
                             static_cast<int>(stride),
                             vpp,
                             verts_double ? L"f64" : L"f32");
                        return true;
                    }
                }
            }

            st.header.note = L"bmin/bmax located but no polyCount/vertCount candidate validated";
            logf(L"  header layout FAILED: {}", st.header.note);
            return false;
        }

        // Reads one tile. Returns false when the slot is free or validation fails.
        bool read_tile(const void* tile_ptr, AgentState& st, Tile& out)
        {
            void* header = nullptr;
            if (!mem::read_at(tile_ptr, 8, header) || !points_at_dnav(header))
            {
                return false;
            }
            void* polys = nullptr;
            void* verts = nullptr;
            if (!mem::read_at(tile_ptr, 16, polys) || !mem::read_at(tile_ptr, 24, verts))
            {
                return false;
            }
            if (!mem::plausible_ptr(polys) || !mem::plausible_ptr(verts))
            {
                return false;
            }

            std::int32_t tx = 0;
            std::int32_t ty = 0;
            std::int32_t tlayer = 0;
            if (!mem::read_at(header, 8, tx) || !mem::read_at(header, 12, ty) || !mem::read_at(header, 16, tlayer))
            {
                return false;
            }

            if (!st.header.found && !learn_header_layout(header, polys, verts, st, tx, ty, tlayer))
            {
                return false;
            }

            const std::size_t rs = st.header.header_double ? 8u : 4u;
            const bool bok = st.header.header_double
                                 ? (read3<double>(header, st.header.bmin_offset, out.bmin) &&
                                    read3<double>(header, st.header.bmin_offset + 3 * rs, out.bmax))
                                 : (read3<float>(header, st.header.bmin_offset, out.bmin) &&
                                    read3<float>(header, st.header.bmin_offset + 3 * rs, out.bmax));
            if (!bok)
            {
                return false;
            }

            std::int32_t poly_count = 0;
            std::int32_t vert_count = 0;
            if (!mem::read_at(header, st.header.poly_count_offset, poly_count) ||
                !mem::read_at(header, st.header.poly_count_offset + 4, vert_count))
            {
                return false;
            }
            if (poly_count < 0 || poly_count > kMaxPolysPerTile || vert_count < 0 || vert_count > kMaxVertsPerTile)
            {
                return false;
            }

            out.x = tx;
            out.y = ty;
            out.layer = tlayer;

            const std::size_t vrs = st.header.verts_double ? 8u : 4u;
            if (!mem::readable(verts, static_cast<std::size_t>(vert_count) * 3 * vrs))
            {
                return false;
            }
            out.verts.reserve(static_cast<std::size_t>(vert_count));
            for (std::int32_t i = 0; i < vert_count; ++i)
            {
                double v[3]{};
                const bool ok = st.header.verts_double
                                    ? read3<double>(verts, static_cast<std::size_t>(i) * 3 * vrs, v)
                                    : read3<float>(verts, static_cast<std::size_t>(i) * 3 * vrs, v);
                if (!ok)
                {
                    return false;
                }
                out.verts.push_back({v[0], v[1], v[2]});
            }

            if (!mem::readable(polys, static_cast<std::size_t>(poly_count) * st.header.poly_stride))
            {
                return false;
            }
            out.polys.reserve(static_cast<std::size_t>(poly_count));
            for (std::int32_t i = 0; i < poly_count; ++i)
            {
                PolyRaw pr{};
                if (!read_poly(polys, static_cast<std::size_t>(i), st.header.poly_stride, st.header.verts_per_poly, pr))
                {
                    return false;
                }
                const int n = static_cast<int>(pr.vert_count);
                if (n < 3 || n > st.header.verts_per_poly)
                {
                    continue; // off-mesh connection (2 verts) or a corrupt entry
                }
                Poly p{};
                p.n = n;
                bool indices_ok = true;
                for (int k = 0; k < n; ++k)
                {
                    if (static_cast<std::int32_t>(pr.verts[k]) >= vert_count)
                    {
                        indices_ok = false;
                        break;
                    }
                    p.v[static_cast<std::size_t>(k)] = pr.verts[k];
                }
                if (!indices_ok)
                {
                    continue;
                }
                p.area = static_cast<std::uint8_t>(pr.area_and_type & 0x3F);
                p.type = static_cast<std::uint8_t>(pr.area_and_type >> 6);
                p.flags = pr.flags;
                out.polys.push_back(p);
            }
            return true;
        }

        //==============================================================================
        // Reaching the mesh (cached) and enumerating tiles
        //==============================================================================

        struct MeshHandle
        {
            bool ok = false;
            const std::uint8_t* tiles = nullptr;
            int max_tiles = 0;
            std::size_t stride = 0;
            std::wstring reason;
        };

        // Re-validates (or re-discovers) the whole chain. Cheap on the happy path: two
        // pointer reads plus one dtNavMeshParams check, and the tile array is only
        // re-scanned when the cached offset stops working.
        MeshHandle reach_mesh(AgentState& st)
        {
            MeshHandle h{};
            const double expected_tile = st.tile_size_uu ? static_cast<double>(*st.tile_size_uu) : kExpectedTileSizeUU;

            if (!st.mesh.detour_found)
            {
                st.mesh = find_impl_and_detour(st.actor, st.props, expected_tile);
            }
            if (!st.mesh.detour_found)
            {
                h.reason = st.mesh.note.empty() ? std::wstring{L"dtNavMesh not reachable"} : st.mesh.note;
                return h;
            }

            void* impl = nullptr;
            if (!mem::read_at(st.actor, st.mesh.impl_offset, impl) || !mem::readable(impl, 16))
            {
                st.mesh.detour_found = false;
                h.reason = L"RecastNavMeshImpl pointer went bad";
                return h;
            }
            void* detour = nullptr;
            if (!mem::read_at(impl, 0, detour) || !mem::plausible_ptr(detour) || !mem::readable(detour, 256))
            {
                h.reason = L"DetourNavMesh is null - navmesh data is not loaded";
                return h;
            }

            NavParams p{};
            const bool ok = st.mesh.params_double ? read_params_as<double>(detour, st.mesh.params_offset, p)
                                                  : read_params_as<float>(detour, st.mesh.params_offset, p);
            if (!ok || !params_plausible(p, expected_tile))
            {
                logf(L"{}: dtNavMeshParams stopped validating at the pinned offset - re-discovering", st.agent);
                st.mesh.detour_found = false;
                st.mesh.tiles_found = false;
                h.reason = L"dtNavMeshParams no longer valid";
                return h;
            }
            st.params = p;

            // Cached tile array: verify the pointer slot still holds a usable array.
            if (st.mesh.tiles_found)
            {
                void* cached = nullptr;
                if (mem::read_ptr(static_cast<const std::uint8_t*>(detour) + st.mesh.tiles_offset, cached) &&
                    mem::readable(cached, 64))
                {
                    h.ok = true;
                    h.tiles = static_cast<const std::uint8_t*>(cached);
                    h.max_tiles = p.max_tiles;
                    h.stride = st.mesh.tile_stride;
                    return h;
                }
                st.mesh.tiles_found = false;
            }

            const TileArray array = find_tile_array(detour, st.mesh.params_offset, st.mesh.params_double, p.max_tiles);
            if (!array.found || array.stride == 0)
            {
                st.mesh.tiles_found = false;
                h.reason = array.found ? std::wstring{L"tile array found but only one live tile, stride unknown"}
                                       : std::wstring{L"dtNavMesh holds zero live tiles"};
                if (array.found)
                {
                    // Still usable for exactly one tile.
                    h.ok = true;
                    h.tiles = array.base;
                    h.max_tiles = 1;
                    h.stride = 1; // never indexed past tile 0
                    st.mesh.tiles_offset = array.offset_in_mesh;
                    st.mesh.tile_stride = 0;
                    st.mesh.tile_finds = array.finds;
                }
                return h;
            }

            st.mesh.tiles_found = true;
            st.mesh.tiles_offset = array.offset_in_mesh;
            st.mesh.tile_stride = array.stride;
            st.mesh.tile_finds = array.finds;

            h.ok = true;
            h.tiles = array.base;
            h.max_tiles = p.max_tiles;
            h.stride = array.stride;
            return h;
        }

        // Cheap pass: only counts live tiles and hashes their identity. Used every poll so
        // the expensive geometry extraction happens only when a dump is actually due.
        std::uint64_t tile_set_signature(const MeshHandle& h, int& live_out)
        {
            std::uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](std::uint64_t v) {
                hash ^= v;
                hash *= 1099511628211ull;
            };

            int live = 0;
            for (int i = 0; i < h.max_tiles; ++i)
            {
                const std::uint8_t* tile_ptr = h.tiles + static_cast<std::size_t>(i) * h.stride;
                if (!mem::readable(tile_ptr, 32))
                {
                    break;
                }
                void* header = nullptr;
                if (!mem::read_at(tile_ptr, 8, header) || !points_at_dnav(header))
                {
                    continue;
                }
                std::int32_t xyz[3]{};
                if (!mem::read_at(header, 8, xyz))
                {
                    continue;
                }
                ++live;
                mix(static_cast<std::uint32_t>(xyz[0]));
                mix(static_cast<std::uint32_t>(xyz[1]));
                mix(static_cast<std::uint32_t>(xyz[2]));
            }
            mix(static_cast<std::uint64_t>(live));
            live_out = live;
            return hash;
        }

        std::vector<Tile> collect_tiles(AgentState& st, const MeshHandle& h, int& skipped_out)
        {
            std::vector<Tile> tiles;
            int skipped = 0;
            for (int i = 0; i < h.max_tiles; ++i)
            {
                const std::uint8_t* tile_ptr = h.tiles + static_cast<std::size_t>(i) * h.stride;
                if (!mem::readable(tile_ptr, 32))
                {
                    break;
                }
                void* header = nullptr;
                if (!mem::read_at(tile_ptr, 8, header) || !points_at_dnav(header))
                {
                    continue;
                }
                Tile t{};
                if (read_tile(tile_ptr, st, t))
                {
                    tiles.push_back(std::move(t));
                }
                else
                {
                    ++skipped;
                }
            }
            skipped_out = skipped;
            return tiles;
        }

        //==============================================================================
        // One dump
        //==============================================================================

        void read_settings(AgentState& st)
        {
            st.agent_radius = prop_float(st.props, st.actor, L"AgentRadius");
            st.agent_height = prop_float(st.props, st.actor, L"AgentHeight");
            st.tile_size_uu = prop_float(st.props, st.actor, L"TileSizeUU");
            st.cell_size = prop_float(st.props, st.actor, L"CellSize");
            st.cell_height = prop_float(st.props, st.actor, L"CellHeight");
            st.tile_pool_size = prop_int(st.props, st.actor, L"TilePoolSize");
            st.poly_ref_tile_bits = prop_int(st.props, st.actor, L"PolyRefTileBits");
            st.poly_ref_poly_bits = prop_int(st.props, st.actor, L"PolyRefNavPolyBits");
            st.poly_ref_salt_bits = prop_int(st.props, st.actor, L"PolyRefSaltBits");

            logf(L"  {}: AgentRadius {} AgentHeight {} TileSizeUU {} CellSize {} CellHeight {} TilePoolSize {} "
                 L"PolyRefBits {}/{}/{}",
                 st.agent,
                 opt_f(st.agent_radius),
                 opt_f(st.agent_height),
                 opt_f(st.tile_size_uu),
                 opt_f(st.cell_size),
                 opt_f(st.cell_height),
                 opt_i(st.tile_pool_size),
                 opt_i(st.poly_ref_tile_bits),
                 opt_i(st.poly_ref_poly_bits),
                 opt_i(st.poly_ref_salt_bits));
        }

        void dump_agent(AgentState& st, bool forced)
        {
            const MeshHandle h = reach_mesh(st);

            std::vector<Tile> tiles;
            int skipped = 0;
            if (h.ok)
            {
                tiles = collect_tiles(st, h, skipped);
            }

            std::size_t total_polys = 0;
            std::size_t total_verts = 0;
            for (const Tile& t : tiles)
            {
                total_polys += t.polys.size();
                total_verts += t.verts.size();
            }

            std::wstring reason = h.reason;
            if (h.ok && tiles.empty() && reason.empty())
            {
                reason = L"dtNavMesh reached but it holds zero live tiles right now";
            }
            if (!st.header.found && !st.header.note.empty())
            {
                reason += (reason.empty() ? L"" : L"; ");
                reason += st.header.note;
            }

            if (tiles.empty() && !forced)
            {
                // Nothing to write and nobody asked; the log above already says why.
                return;
            }

            const std::wstring file = (tiles.empty() ? L"probe_" : L"tiles_") + timestamp() + L".json";
            const std::filesystem::path path = g_out_root / st.agent / file;

            if (write_json(path, st, tiles, total_polys, total_verts, reason))
            {
                logf(L"DUMP {}: {} tiles, {} polys, {} verts{} -> {}{}",
                     st.agent,
                     static_cast<int>(tiles.size()),
                     static_cast<int>(total_polys),
                     static_cast<int>(total_verts),
                     skipped > 0 ? std::format(L", {} tile(s) skipped after failing validation", skipped) : std::wstring{},
                     path.wstring(),
                     reason.empty() ? std::wstring{} : (L"   [" + reason + L"]"));
            }
            else
            {
                logf(L"DUMP {}: FAILED to write {}", st.agent, path.wstring());
            }
        }

        //==============================================================================
        // Actor discovery
        //==============================================================================

        std::wstring agent_from_actor_name(const std::wstring& name)
        {
            const auto dash = name.rfind(L'-');
            std::wstring agent = (dash == std::wstring::npos) ? name : name.substr(dash + 1);
            for (wchar_t& c : agent)
            {
                if (std::iswalnum(static_cast<std::wint_t>(c)) == 0 && c != L'_')
                {
                    c = L'_';
                }
            }
            return agent.empty() ? std::wstring{L"Unknown"} : agent;
        }

        void refresh_actors()
        {
            std::vector<UObject*> found;
            found.reserve(8);
            RC::Unreal::UObjectGlobals::FindAllOf(L"RecastNavMesh", found);

            std::vector<UObject*> live;
            live.reserve(found.size());
            for (UObject* obj : found)
            {
                if (obj != nullptr && mem::readable(obj, 64))
                {
                    live.push_back(obj);
                }
            }

            const int count = static_cast<int>(live.size());
            if (count != g_last_actor_count)
            {
                if (count == 0)
                {
                    logf(L"RecastNavMesh: 0 instances (waiting) - expected at the main menu / Lobby");
                }
                else
                {
                    logf(L"RecastNavMesh: {} instance(s)", count);
                }
                g_last_actor_count = count;
            }

            for (UObject* obj : live)
            {
                if (g_agents.contains(obj))
                {
                    continue;
                }
                AgentState st{};
                st.actor = obj;
                st.actor_name = obj->GetName();
                st.full_name = obj->GetFullName();
                st.agent = agent_from_actor_name(st.actor_name);

                UClass* cls = obj->GetClassPrivate();
                st.props = collect_props(cls);
                logf(L"new RecastNavMesh actor {} (agent \"{}\") at {}", st.full_name, st.agent, hex_ptr(obj));
                read_settings(st);

                const double expected_tile = st.tile_size_uu ? static_cast<double>(*st.tile_size_uu) : kExpectedTileSizeUU;
                st.mesh = find_impl_and_detour(st.actor, st.props, expected_tile);

                g_agents.emplace(static_cast<const void*>(obj), std::move(st));
            }

            // Forget actors that are gone (level change).
            for (auto it = g_agents.begin(); it != g_agents.end();)
            {
                const bool still_there =
                    std::find_if(live.begin(), live.end(), [&](UObject* o) { return static_cast<const void*>(o) == it->first; }) !=
                    live.end();
                if (!still_there)
                {
                    logf(L"RecastNavMesh actor {} disappeared, forgetting its layout", it->second.agent);
                    it = g_agents.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        //==============================================================================
        // Hotkey
        //==============================================================================

        bool this_process_is_foreground()
        {
            const HWND fg = ::GetForegroundWindow();
            if (fg == nullptr)
            {
                return false;
            }
            DWORD pid = 0;
            ::GetWindowThreadProcessId(fg, &pid);
            return pid == ::GetCurrentProcessId();
        }
    } // namespace

    //==================================================================================
    // Public API
    //==================================================================================

    void on_unreal_init()
    {
        g_out_root = resolve_out_root();
        g_initialised = true;
        logf(L"module up. Output root: {}", g_out_root.wstring());
        logf(L"F6 / CTRL+F6 forces a dump of every agent; an automatic dump follows {} ms after the set of live "
             L"tiles changes; the primary agent for the map is \"{}\"",
             static_cast<int>(kDebounceMs),
             kPrimaryAgent);
    }

    void on_update()
    {
        if (!g_initialised)
        {
            return;
        }

        const std::uint64_t now = ::GetTickCount64();

        // ---- hotkey -----------------------------------------------------------------
        const bool f6_down = (::GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool ctrl_down = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (f6_down && !g_f6_was_down && now - g_last_hotkey > kHotkeyGuardMs && this_process_is_foreground())
        {
            g_last_hotkey = now;
            logf(L"{}F6 pressed - forcing a dump of {} agent(s)",
                 ctrl_down ? L"CTRL+" : L"",
                 static_cast<int>(g_agents.size()));
            if (g_agents.empty())
            {
                logf(L"  nothing to dump: no RecastNavMesh actor is loaded. At the main menu this is expected - "
                     L"load a save first");
            }
            for (auto& entry : g_agents)
            {
                dump_agent(entry.second, true);
                entry.second.dumped_current_signature = true;
            }
        }
        g_f6_was_down = f6_down;

        // ---- periodic actor + tile-set poll ------------------------------------------
        if (now - g_last_poll < kActorPollMs)
        {
            return;
        }
        g_last_poll = now;

        refresh_actors();

        for (auto& entry : g_agents)
        {
            AgentState& st = entry.second;
            const MeshHandle h = reach_mesh(st);
            if (!h.ok)
            {
                continue;
            }
            int live = 0;
            const std::uint64_t sig = tile_set_signature(h, live);
            if (live == 0)
            {
                continue;
            }
            if (!st.signature_seen || sig != st.tile_signature)
            {
                st.signature_seen = true;
                st.tile_signature = sig;
                st.signature_changed_at = now;
                st.dumped_current_signature = false;
                logf(L"{}: live tile set changed - {} tiles; auto dump in {} ms unless it changes again",
                     st.agent,
                     live,
                     static_cast<int>(kDebounceMs));
                continue;
            }
            if (!st.dumped_current_signature && now - st.signature_changed_at >= kDebounceMs)
            {
                st.dumped_current_signature = true;
                dump_agent(st, false);
            }
        }
    }
} // namespace navmesh

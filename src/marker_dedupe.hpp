//
// marker_dedupe.hpp - "one id, one marker", as a pure function. Pure and free of
// Windows/Unreal so the offline test exe can link it.
//
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "markers_db.hpp"

namespace mdb
{
    // One removal, in ORIGINAL indices.
    struct DupDrop
    {
        std::size_t dropped = 0;
        std::size_t kept = 0;
        std::string id;
    };

    // Keeps the FIRST marker of each id, drops later copies, and builds `by_id` over
    // what is left (indices into the shrunk vector). Returns the number removed.
    inline int dedupe_by_id(std::vector<StaticMarker>& markers, std::unordered_map<std::string, int>& by_id,
                            std::vector<DupDrop>& drops)
    {
        by_id.clear();
        by_id.reserve(markers.size());
        drops.clear();

        std::vector<StaticMarker> kept;
        kept.reserve(markers.size());
        std::vector<std::size_t> kept_origin; // kept index -> original index
        kept_origin.reserve(markers.size());

        int dropped = 0;
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const auto it = by_id.find(markers[i].id);
            if (it != by_id.end())
            {
                ++dropped;
                drops.push_back(DupDrop{i, kept_origin[static_cast<std::size_t>(it->second)], markers[i].id});
                continue;
            }
            by_id.emplace(markers[i].id, static_cast<int>(kept.size()));
            kept_origin.push_back(i);
            kept.push_back(std::move(markers[i]));
        }
        // Unconditional: the loop moved every kept marker out of `markers`.
        markers = std::move(kept);
        return dropped;
    }
} // namespace mdb

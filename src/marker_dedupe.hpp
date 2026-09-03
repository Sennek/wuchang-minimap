//
// marker_dedupe.hpp - "one id, one marker", as a pure function.
//
// A duplicate id used to leave BOTH markers in the loaded database while the id -> index
// map kept only the first. The second was then drawn as a twin that could never be
// marked found, because every write of a found flag - the live sweep's auto-mark and the
// full map's manual click alike - resolves through that map. Nothing shipped has a
// duplicate; a community pack or a re-extraction will.
//
// It lives in its own header rather than inside markers.cpp so the offline test exe can
// link it: it is pure, it needs neither Windows nor Unreal, and it is exactly the kind
// of thing that must not be verified by launching the game.
//
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "markers_db.hpp"

namespace mdb
{
    // One removal, in ORIGINAL indices, so the caller can name the file each marker
    // came from.
    struct DupDrop
    {
        std::size_t dropped = 0;
        std::size_t kept = 0;
        std::string id;
    };

    // Keeps the FIRST marker of each id and removes every later copy, then builds
    // `by_id` over what is left (values are indices into the SHRUNK vector). The
    // caller's file enumeration is sorted, so "first" is stable across runs. Returns
    // the number of markers removed.
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
        // Unconditional: the loop MOVED every marker it kept out of `markers`, so what
        // is left behind is a vector of hollowed-out entries either way.
        markers = std::move(kept);
        return dropped;
    }
} // namespace mdb

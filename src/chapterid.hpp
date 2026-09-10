#pragma once

//
// chapterid - "which chapter is the player in?", as pure string logic. Not a bounds
// test: the five chapters' world extents overlap heavily, so a position alone cannot
// name one.
//
// The identifying signal is the streamed level set. A chapter's whole art streams in at
// once and two chapters' `Chapter<N>_Area` levels can be resident together, so `_Base` /
// `_Land` names are useless; the unambiguous signal is the navmesh/streaming cell
// packages
//
//     /Game/Maps/Generate/Chapter<N>/EX0/B<N>EX0_L0_X<x>_Y<y>_DL0_WP
//
// of which only the current chapter's 4-6 are loaded, the streaming window following
// the player.
//
// The DLC is the exception: the paks carry no `Maps/Generate/ChapterDLC` cells at all
// (480 `Generate/` entries, chapters 1-5 only), so a DLC area streams no cell package
// and has no cooked navmesh. Its only cooked levels are the 28
// `Maps/ChapterDLC_logic/ChapterDLC_*_{logic,AI}.umap`.
//
// Hence a tiered vote, not a sum: cell packages beat `_logic` levels, which beat a bare
// `Chapter<N>` mention, and a tier is consulted only when no higher tier saw anything.
//
// THE COVERAGE TIEBREAK. Two chapters' levels are resident together in the passages
// between chapters - and along a seam the next chapter's CELL packages stream in while
// this one's are still up, so even tier 3 is a contested count whose winner can be the
// chapter whose map stops short of the player. `coverage_tiebreak()` takes the contested
// candidates plus, per candidate, HOW MUCH ground that chapter's shipped asset has around
// the player (mapmanifest::Entry::cover_score) and hands the answer to the one that leads.
// `settle_chapter()` then weighs that against the chapter already resident, and
// `Hysteresis` makes an unled swap wait for N agreeing samples, because a swap costs
// ~340 MB of decode.
//
// Pure: no Windows, no UE4SS, no allocation.
//

#include <cstddef>
#include <string>
#include <string_view>

namespace chid
{
    constexpr int kNone = -1;      // nothing recognised
    constexpr int kDlc = 0;        // the DLC, which has no numbered chapter
    constexpr int kMaxChapter = 9; // chapters are single-digit in every parsed name

    // Signal strength, strongest first. `add()` keeps them in separate buckets.
    constexpr int kTierCell = 3;  // B<N>EX0_L0_X<x>_Y<y>_DL0_WP - the streaming cell
    constexpr int kTierLogic = 2; // Chapter<N>_<Area>_logic / _AI - area-scoped
    constexpr int kTierArt = 1;   // any other Chapter<N> / ChapterDLC mention

    struct Signal
    {
        int chapter = kNone;
        int tier = 0;

        bool valid() const
        {
            return tier > 0 && chapter >= kDlc && chapter <= kMaxChapter;
        }
    };

    namespace detail
    {
        // Case-sensitive ASCII substring search over char or wchar_t; level packages and
        // object paths are ASCII.
        template <class CharT>
        inline std::size_t find_ascii(std::basic_string_view<CharT> hay, const char* needle,
                                      std::size_t from = 0)
        {
            std::size_t n = 0;
            while (needle[n] != '\0')
            {
                ++n;
            }
            if (n == 0 || hay.size() < n)
            {
                return std::basic_string_view<CharT>::npos;
            }
            for (std::size_t i = from; i + n <= hay.size(); ++i)
            {
                std::size_t k = 0;
                while (k < n && hay[i + k] == static_cast<CharT>(needle[k]))
                {
                    ++k;
                }
                if (k == n)
                {
                    return i;
                }
            }
            return std::basic_string_view<CharT>::npos;
        }

        template <class CharT>
        inline bool is_digit(CharT c)
        {
            return c >= static_cast<CharT>('0') && c <= static_cast<CharT>('9');
        }

        // The digits at `i` as a chapter number, kNone unless there is exactly one:
        // chapters are 1..5, and a two-digit run is an index suffix.
        template <class CharT>
        inline int one_digit(std::basic_string_view<CharT> s, std::size_t i)
        {
            if (i >= s.size() || !is_digit(s[i]))
            {
                return kNone;
            }
            if (i + 1 < s.size() && is_digit(s[i + 1]))
            {
                return kNone;
            }
            const int v = static_cast<int>(s[i] - static_cast<CharT>('0'));
            return (v >= 1 && v <= kMaxChapter) ? v : kNone;
        }

        template <class CharT>
        inline bool area_scoped(std::basic_string_view<CharT> s)
        {
            return find_ascii(s, "_logic") != std::basic_string_view<CharT>::npos ||
                   find_ascii(s, "_AI") != std::basic_string_view<CharT>::npos;
        }

        template <class CharT>
        inline Signal classify_t(std::basic_string_view<CharT> name)
        {
            using View = std::basic_string_view<CharT>;

            // ---- tier 3: a streaming cell package, `B<N>EX0_L0_X<x>_Y<y>_DL0_WP` ----
            // Anchored on "EX0_L0_X" and read backwards to the 'B', matching both the
            // bare package name and the full object path.
            for (std::size_t at = find_ascii(name, "EX0_L0_X"); at != View::npos;
                 at = find_ascii(name, "EX0_L0_X", at + 1))
            {
                if (at < 2)
                {
                    continue;
                }
                const std::size_t digit = at - 1;
                if (!is_digit(name[digit]) || name[digit - 1] != static_cast<CharT>('B'))
                {
                    continue;
                }
                const int n = one_digit(name, digit);
                if (n != kNone)
                {
                    return Signal{n, kTierCell};
                }
            }

            // ---- the DLC, which never has a cell package -----------------------------
            if (find_ascii(name, "ChapterDLC") != View::npos)
            {
                return Signal{kDlc, area_scoped(name) ? kTierLogic : kTierArt};
            }

            // ---- tier 2 / 1: any Chapter<N> mention ---------------------------------
            for (std::size_t at = find_ascii(name, "Chapter"); at != View::npos;
                 at = find_ascii(name, "Chapter", at + 1))
            {
                const int n = one_digit(name, at + 7);
                if (n != kNone)
                {
                    return Signal{n, area_scoped(name) ? kTierLogic : kTierArt};
                }
            }
            return Signal{};
        }
    } // namespace detail

    inline Signal classify(std::string_view name)
    {
        return detail::classify_t<char>(name);
    }

    inline Signal classify(std::wstring_view name)
    {
        return detail::classify_t<wchar_t>(name);
    }

    // `"chapter3"` -> 3, `"chapterdlc"` -> kDlc, anything else -> kNone. Mirrors
    // build_map.py's `chapter_number()`; the fallback for a manifest with no number.
    inline int chapter_from_key(std::string_view key)
    {
        int value = kNone;
        for (char c : key)
        {
            if (c >= '0' && c <= '9')
            {
                if (value == kNone)
                {
                    value = 0;
                }
                value = value * 10 + (c - '0');
            }
        }
        if (value == kNone && detail::find_ascii<char>(key, "dlc") != std::string_view::npos)
        {
            return kDlc;
        }
        return (value >= 1 && value <= kMaxChapter) ? value : kNone;
    }

    //==================================================================================
    // The tiered vote
    //==================================================================================

    class Vote
    {
      public:
        void reset()
        {
            *this = Vote{};
        }

        void add(Signal s)
        {
            if (!s.valid())
            {
                return;
            }
            ++counts_[s.tier][s.chapter + 1];
            ++classified_;
        }

        void add(std::string_view name)
        {
            add(classify(name));
            ++seen_;
        }

        void add(std::wstring_view name)
        {
            add(classify(name));
            ++seen_;
        }

        int count(int chapter, int tier) const
        {
            if (tier < 1 || tier > kTierCell || chapter < kDlc || chapter > kMaxChapter)
            {
                return 0;
            }
            return counts_[tier][chapter + 1];
        }

        // The chapter with the most hits in the highest tier that saw anything; lower
        // tiers are never mixed in. Ties go to the lower chapter number.
        int best() const
        {
            for (int tier = kTierCell; tier >= 1; --tier)
            {
                int win = kNone;
                int most = 0;
                for (int ch = kDlc; ch <= kMaxChapter; ++ch)
                {
                    const int c = counts_[tier][ch + 1];
                    if (c > most)
                    {
                        most = c;
                        win = ch;
                    }
                }
                if (win != kNone)
                {
                    return win;
                }
            }
            return kNone;
        }

        int best_tier() const
        {
            for (int tier = kTierCell; tier >= 1; --tier)
            {
                for (int ch = kDlc; ch <= kMaxChapter; ++ch)
                {
                    if (counts_[tier][ch + 1] > 0)
                    {
                        return tier;
                    }
                }
            }
            return 0;
        }

        int best_count() const
        {
            const int ch = best();
            return ch == kNone ? 0 : count(ch, best_tier());
        }

        int seen() const
        {
            return seen_;
        }

        int classified() const
        {
            return classified_;
        }

      private:
        // [tier][chapter + 1]: kDlc (0) lives at index 1, index 0 is unused, so no
        // negative index is ever needed.
        int counts_[kTierCell + 1][kMaxChapter + 2]{};
        int seen_ = 0;
        int classified_ = 0;
    };

    //==================================================================================
    // The coverage tiebreak
    //==================================================================================

    // One chapter that scored at the vote's winning tier, and how much ground its shipped
    // map has around the player - mapdata::chapter_cover_score, 0..100.
    struct Candidate
    {
        int chapter = kNone;
        int count = 0;
        int cover_score = 0;
    };

    constexpr int kMaxCandidates = kMaxChapter + 2;

    // How far ahead one chapter's coverage has to be before it is worth ~340 MB of decode
    // on the spot. Two probes of the seventeen. Below it the answer still stands - it is
    // simply not urgent, and the streak decides. Naming a proposal needs no threshold at
    // all: see coverage_tiebreak.
    constexpr int kCoverLeadPct = 10;

    // What the contested vote should propose, from the assets alone. Nothing here knows
    // which chapter is resident - `settle_chapter()` owns that. In order:
    //
    //   1. fewer than two candidates scored - nothing is contested, the vote stands;
    //   2. one candidate has strictly more ground than every other - it wins, which is the
    //      whole point of the tiebreak;
    //   3. a tie at the top - nothing was learned, the vote stands.
    //
    // No margin here. Naming the better-covered chapter is free, and `settle_chapter()` is
    // where a thin lead is made to earn its swap; a margin in both places let the vote win
    // a seam the ground had already answered, 94 % against 76 %, on 2026-09-10.
    inline int coverage_tiebreak(const Candidate* candidates, int n, int vote_winner)
    {
        if (candidates == nullptr || n <= 0)
        {
            return vote_winner;
        }
        int scored = 0;
        int best = kNone;
        int best_score = -1;
        int second_score = -1;
        for (int i = 0; i < n; ++i)
        {
            const Candidate& c = candidates[i];
            if (c.count <= 0 || c.chapter < kDlc || c.chapter > kMaxChapter)
            {
                continue;
            }
            ++scored;
            if (c.cover_score > best_score)
            {
                second_score = best_score;
                best_score = c.cover_score;
                best = c.chapter;
            }
            else if (c.cover_score > second_score)
            {
                second_score = c.cover_score;
            }
        }
        if (scored < 2 || best_score <= second_score)
        {
            return vote_winner;
        }
        return best;
    }

    // A swap decodes ~340 MB, so the coverage answer has to hold still first: `settle()`
    // hands back `current` until the same `proposal` has arrived `need` times running.
    // A proposal equal to `current` clears the streak, so walking back over a boundary
    // costs nothing.
    class Hysteresis
    {
      public:
        void reset()
        {
            pending_ = kNone;
            streak_ = 0;
        }

        int settle(int current, int proposal, int need)
        {
            if (proposal == current || proposal == kNone)
            {
                reset();
                return current;
            }
            if (proposal != pending_)
            {
                pending_ = proposal;
                streak_ = 0;
            }
            ++streak_;
            if (streak_ >= (need > 1 ? need : 1))
            {
                reset();
                return proposal;
            }
            return current;
        }

        int pending() const
        {
            return pending_;
        }

        int streak() const
        {
            return streak_;
        }

      private:
        int pending_ = kNone;
        int streak_ = 0;
    };

    // The chapter to use this sample, and the only place the chapter already resident is
    // weighed: `proposal` is what `coverage_tiebreak()` made of the vote, and the two
    // scores are how much ground the resident chapter's asset and the proposal's have
    // around the player. In order:
    //
    //   1. nothing proposed, or the proposal is already resident - stay;
    //   2. nothing resident yet - take the proposal at once, there is nothing to lose;
    //   3. the proposal's ground leads the resident chapter's by kCoverLeadPct - swap at
    //      once. This is what crossing a real boundary looks like, and waiting there
    //      leaves the player looking at a blank map;
    //   4. the resident chapter's ground leads by that much - stay, however the vote
    //      counted levels. In the passages between chapters both chapters' levels are
    //      resident and the vote's winner is whichever streamed more of them, which is how
    //      the map used to go blank underfoot;
    //   5. neither leads by that much - the streak decides, because a swap frees and
    //      decodes ~340 MB.
    //
    // Rules 3 and 4 are the asymmetry that matters: ground AROUND the player outranks the
    // vote in both directions, so a chapter can be neither entered nor left by a level
    // count alone.
    inline int settle_chapter(Hysteresis& h, int current, int proposal, int current_score,
                              int proposal_score, int need)
    {
        if (proposal == kNone || proposal == current)
        {
            h.reset();
            return current;
        }
        if (current == kNone)
        {
            h.reset();
            return proposal;
        }
        if (proposal_score - current_score >= kCoverLeadPct)
        {
            h.reset();
            return proposal;
        }
        if (current_score - proposal_score >= kCoverLeadPct)
        {
            h.reset();
            return current;
        }
        return h.settle(current, proposal, need);
    }
} // namespace chid

#include "markers.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "highlight.hpp"
#include "mapdata.hpp"
#include "mem.hpp"
#include "mmstate.hpp"
#include "saveslot.hpp"
#include "scan_sched.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

using RC::Unreal::UObject;
using RC::Unreal::UStruct;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

namespace markers
{
    namespace
    {
        //==============================================================================
        // The class table - what the live sweep looks for
        //==============================================================================
        //
        // HOW THE SWEEP FINDS THESE (changed 2026-09-02, see lessons.md)
        // --------------------------------------------------------------
        // It used to be one `UObjectGlobals::FindAllOf` per game-thread pump, cycling
        // through this table. FindAllOf walks the WHOLE GUObjectArray, so a round cost
        // kClassCount full walks - measured in-game at 28.30 ms mean / 51.05 ms peak
        // PER PUMP, i.e. two to three dropped frames ten times a second.
        //
        // Now the sweep walks the object array itself, ONCE per round, in slices of
        // `markers_scan_chunk` slots per pump (src/scan_sched.hpp). Each slot costs a
        // validity check, a class-pointer load and one memoised lookup; the expensive
        // per-object work only runs for the few slots whose class is in this table.
        //
        // Classification is still by NAME up the super chain, so a subclass lands in
        // the right category exactly as FindAllOf's subclass matching used to arrange -
        // BP_PickupActor_C still brings in BP_DropItem_C, the runtime-spawned enemy
        // loot drop whose collect was watched end-to-end in run 3. The difference is
        // that the answer is memoised per UClass* and consulted, not recomputed.

        enum class Rule : std::uint8_t
        {
            None,          // no known state flag - never auto-marked
            UsedBool,      // chests / doors / mechanisms: `Used` (SavedStatuKey=statu_use)
            DoorOpenBool,  // BP_NewPuzzlesDoor_C: `DoorOpen`
            PickupDying,   // pickups: `dying` (early) or parked at (0,0,0) (durable)
            ActiveBool,    // fog gates: SavedStatuKey=status_active, so `Active` is the flag
            ControllerPawn // AI controller: the marker is its possessed Pawn
        };

        struct ClassSpec
        {
            const wchar_t* name;
            mdb::Cat cat;
            Rule rule;
            bool persist; // may be written to wuchang_minimap_found.txt
        };

        constexpr ClassSpec kClasses[] = {
            // Shrines. `Open` / `DefaultEnable` / `InInteract` were identical on all
            // three instances in run 3, so shrine ACTIVATION is still unidentified and
            // the rule stays None; the id comes from the CJK-named property instead.
            {L"BP_RebornFire_C", mdb::Cat::Shrine, Rule::None, true},
            // Chests: `Used` confirmed with both states side by side in run 3.
            {L"BP_treasurebox_C", mdb::Cat::Chest, Rule::UsedBool, true},
            {L"BP_ItemRedBox_C", mdb::Cat::Chest, Rule::UsedBool, true},
            // Pickups: `dying = false -> true`, then the actor is moved to (0,0,0) ~2 s
            // later and lingers until a GC. Absence from FindAllOf is NOT evidence of
            // a collect (an unloaded level looks identical), so it never auto-marks.
            {L"BP_PickupActor_C", mdb::Cat::Pickup, Rule::PickupDying, true},
            // Doors. BP_NewPuzzlesDoor_C is the one that also carries `GeemID` and
            // `New Fire Point ID` - a ready-made shortcut edge for a later map graph.
            {L"BP_NewPuzzlesDoor_C", mdb::Cat::Door, Rule::DoorOpenBool, true},
            {L"BP_DoorZhong_C", mdb::Cat::Door, Rule::UsedBool, true},
            // Fog gates. Strong inference, not yet observed in the passed state.
            {L"BP_Wumen_C", mdb::Cat::FogGate, Rule::ActiveBool, true},
            {L"BP_LadderV2_C", mdb::Cat::Ladder, Rule::None, false},
            {L"BP_WoodenElevator_C", mdb::Cat::Lift, Rule::None, false},
            // Enemies are LIVE ONLY and never persisted: the count of
            // Impl_BaseAIController_C is the count of live enemies, and the marker is
            // the pawn it possesses.
            {L"Impl_BaseAIController_C", mdb::Cat::Enemy, Rule::ControllerPawn, false},
        };

        constexpr int kClassCount = static_cast<int>(std::size(kClasses));

        // What a file in the markers directory must declare to be treated as a marker
        // manifest. Anything else in there (items.json, whatever comes next) is skipped
        // quietly rather than reported as broken.
        constexpr std::string_view kMarkerSchemaPrefix = "wuchang-minimap-markers";

        // BP_RebornFire_C's game-authored shrine id - the CJK-named "sitting-Buddha
        // point ID" property (U+5750 U+4F5B U+70B9 + "ID"), spelled with escapes so the
        // symbol survives any source-encoding accident. It is the ONLY property that
        // distinguishes sibling shrines (213 own properties, two of which differ), and
        // object names are index-suffixed and unstable - so it is the marker id.
        constexpr const wchar_t* kShrineIdProp = L"\u5750\u4F5B\u70B9ID";

        //==============================================================================
        // Small helpers
        //==============================================================================

        bool read_whole_file(const std::wstring& path, std::string& out)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size) == 0 || size.QuadPart < 0 || size.QuadPart > (32 << 20))
            {
                ::CloseHandle(h);
                return false;
            }
            out.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            const bool ok = out.empty() ||
                            (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 &&
                             read == out.size());
            ::CloseHandle(h);
            return ok;
        }

        bool write_whole_file(const std::wstring& path, const std::string& data)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            const bool ok = data.empty() ||
                            (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0 &&
                             written == data.size());
            ::CloseHandle(h);
            return ok;
        }

        std::wstring widen(std::string_view narrow)
        {
            return std::wstring{narrow.begin(), narrow.end()};
        }

        // The names this mod handles are ASCII (level names, object names, shrine ids).
        // Anything else is replaced rather than silently truncated, so a surprise never
        // produces two different ids for one actor.
        std::string narrow_ascii(std::wstring_view wide)
        {
            std::string out;
            out.reserve(wide.size());
            for (const wchar_t c : wide)
            {
                out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return out;
        }

        void copy_id(char* dst, std::size_t cap, const std::string& src)
        {
            if (cap == 0)
            {
                return;
            }
            const std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }

        //==============================================================================
        // Spinlock (std::mutex is unusable here - lessons.md)
        //==============================================================================

        class Spin
        {
          public:
            void lock() noexcept
            {
                for (int spin = 0; flag_.test_and_set(std::memory_order_acquire); ++spin)
                {
                    if ((spin & 0x3F) == 0x3F)
                    {
                        ::SwitchToThread();
                    }
                    else
                    {
                        YieldProcessor();
                    }
                }
            }
            void unlock() noexcept
            {
                flag_.clear(std::memory_order_release);
            }

          private:
            std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
        };

        class Guard
        {
          public:
            explicit Guard(Spin& s) noexcept : s_(s)
            {
                s_.lock();
            }
            ~Guard()
            {
                s_.unlock();
            }
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;

          private:
            Spin& s_;
        };

        //==============================================================================
        // The static database, published to the game thread
        //==============================================================================
        //
        // Immutable once published. A reload builds a fresh one and swaps the pointer;
        // the old one is deliberately leaked (bounded by the number of F5 presses)
        // because the game thread may be walking it and there is no safe point to free
        // it without a mutex. Same pattern mapdata uses for its chapter list.

        struct StaticDb
        {
            std::vector<mdb::StaticMarker> markers;
            std::unordered_map<std::string, int> by_id;

            // INTERNING, done once at load. `publish_round` used to hash a
            // std::string per marker per round (the found set, the live twins, the
            // absence streak) and to build a lower-cased copy of every marker's level
            // name; at 700-1500 markers a second that is the whole cost of the round.
            // These turn all of it into array indexing:
            //   levels        - the unique lower-cased level short names
            //   marker_level  - per marker, its index into `levels` (-1 = no level)
            std::vector<std::string> levels;
            std::vector<int> marker_level;
        };

        std::atomic<const StaticDb*> g_db{nullptr};

        //==============================================================================
        // The found tracker
        //==============================================================================
        //
        // The loop thread owns the master set and the file. The game thread owns its
        // own copy so it can decide, without a lock, whether a marker is already known.
        // Two spinlock-guarded mailboxes connect them:
        //
        //   inbox  loop -> game : the whole set after a load / reload (clear + ids)
        //   outbox game -> loop : ids the live sweep has just auto-marked
        //
        // Nothing is ever shared by pointer, so neither side can free memory the other
        // is reading, and the file is only ever touched from the loop thread.

        Spin g_inbox_lock;
        std::vector<std::string> g_inbox;
        bool g_inbox_clear = false;
        std::atomic<bool> g_inbox_pending{false};

        Spin g_outbox_lock;
        std::vector<std::string> g_outbox;
        std::atomic<bool> g_outbox_pending{false};

        // render -> loop : a manual found/not-found toggle from the full map's click
        // handler. Same shape as the outbox, but it can UNSET as well as set, so it
        // carries the wanted state alongside the id.
        struct ToggleReq
        {
            std::string id;
            bool found = false;
        };
        Spin g_toggle_lock;
        std::vector<ToggleReq> g_toggle;
        std::atomic<bool> g_toggle_pending{false};

        std::unordered_set<std::string> g_found_master; // loop thread only
        std::unordered_set<std::string> g_found_gt;     // game thread only
        bool g_found_dirty = false;                     // loop thread only
        std::uint64_t g_found_dirty_ms = 0;

        //==============================================================================
        // Stats
        //==============================================================================

        Spin g_stats_lock;
        Stats g_stats{};

        std::atomic<int> g_published_count{0};
        std::atomic<int> g_live_count{0};
        std::atomic<std::uint64_t> g_rounds{0};

        // Scan diagnostics. Written by the game thread, read by the loop and render
        // threads; every one of them is a lone scalar, so a relaxed atomic is the whole
        // synchronisation story - a torn *set* of numbers on the F2 panel would be
        // harmless anyway (they are refreshed 1 Hz).
        std::atomic<double> g_scan_slice_ms{0.0};
        std::atomic<double> g_scan_slice_ms_avg{0.0};
        std::atomic<double> g_scan_slice_ms_peak{0.0};
        std::atomic<double> g_scan_slice_ms_max{0.0};
        std::atomic<double> g_scan_round_ms{0.0};
        std::atomic<int> g_scan_round_slices{0};
        std::atomic<int> g_scan_round_objects{0};
        std::atomic<int> g_scan_total{0};
        std::atomic<int> g_scan_chunk{0};
        std::atomic<bool> g_scan_fallback{false};

        //==============================================================================
        // The published draw buffer
        //==============================================================================

        constexpr int kSlots = 3;
        std::vector<DrawMarker> g_slot[kSlots];
        std::atomic<int> g_slot_published{-1};
        int g_slot_next = 0; // game thread only

        //==============================================================================
        // Game-thread state
        //==============================================================================

        uer::LayoutCache g_layouts;

        // UClass* -> index into kClasses, or -1 for "not a marker class". Populated by
        // walking the class' super chain by name exactly once per class.
        std::unordered_map<const void*, int> g_class_spec;

        // UObject* -> its stable id. GetFullName() allocates and parses, and the sweep
        // sees the same few hundred actors every second, so this is worth caching.
        // Cleared on every world change (a recycled allocation could otherwise hand a
        // new actor the dead one's id).
        std::unordered_map<const void*, std::string> g_id_cache;

        struct LiveEntry
        {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            // Always one of kClasses[].name - a string literal with static storage, so
            // holding the pointer is safe and costs nothing.
            const wchar_t* cls = nullptr;
            mdb::Cat cat = mdb::Cat::Other;
            bool found = false;
            bool persist = false;
            bool pos_valid = false;
            std::uint64_t round = 0;
        };

        std::unordered_map<std::string, LiveEntry> g_live;

        const void* g_world = nullptr;
        std::uint64_t g_round = 0;

        //==============================================================================
        // Live copies of the sweep's caps (game thread)
        //==============================================================================
        //
        // The config key `markers_live_grace_rounds`
        // `markers_id_cache_max`, `markers_class_cache_max` and
        // `markers_fallback_max_per_class`. They are unpacked from the Config the pump
        // already copies, because process_marker() / publish_round() run per actor and
        // must not each take the config spinlock.
        std::uint64_t g_grace_rounds = 2;
        bool g_absence_on = true;
        int g_absence_rounds = 2;
        std::uint32_t g_absence_cats =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);
        std::size_t g_live_max = 8192;
        std::size_t g_id_cache_max = 8192;
        std::size_t g_class_cache_max = 262144;
        std::size_t g_fallback_max_per_class = 4096;

        //==============================================================================
        // Absence as evidence of a collect (game thread)
        //==============================================================================
        //
        // `g_levels` maps a loaded level's short name (lower-cased, because the marker
        // DB and UObject::GetFullName() need not agree on case) to the sweep round at
        // which it was FIRST seen loaded. A marker may only be auto-marked once a full
        // round has completed after that, so "not seen" cannot mean "its level had not
        // finished streaming when I looked".
        //
        // `g_absent_streak` is the debounce: consecutive confirming rounds per marker
        // id. An entry is erased the moment a round does not confirm, so the map only
        // ever holds markers that are on their way to being marked.
        std::unordered_map<std::string, std::uint64_t> g_levels;
        std::atomic<int> g_absence_marks{0};
        std::atomic<int> g_levels_loaded{0};

        //==============================================================================
        // The per-round index over the static DB (game thread)
        //==============================================================================
        //
        // All of these are sized to `g_idx_db->markers` (or to its `levels`) and are
        // rebuilt whenever the database pointer changes. They exist so that a publish is
        // array indexing rather than string hashing - see StaticDb above.
        const StaticDb* g_idx_db = nullptr;
        std::vector<std::uint8_t> g_found_static;   // per marker: it is in the found set
        std::vector<int> g_absent_streak_idx;       // per marker: confirming rounds in a row
        std::vector<const LiveEntry*> g_live_of_static; // per marker: this round's live twin
        std::vector<std::uint8_t> g_level_known;    // per unique level: is it loaded?
        std::vector<std::uint64_t> g_level_round;   // per unique level: round first seen loaded
        // The chapter-filtered subrange. Only recomputed when the detected chapter moves.
        std::vector<int> g_chapter_subset;
        int g_subset_chapter = chid::kNone;
        bool g_subset_valid = false;
        // The found set changed wholesale (a reload of wuchang_minimap_found.txt), so
        // g_found_static must be rebuilt before the next publish.
        bool g_found_index_dirty = true;

        // publish_round timing (game thread writes, F2 and the log read).
        std::atomic<double> g_publish_ms{0.0};
        std::atomic<double> g_publish_ms_avg{0.0};
        std::atomic<double> g_publish_ms_peak{0.0};
        double g_publish_ms_sum = 0.0;
        std::uint64_t g_publish_count = 0;

        // One definition, in the tested pure layer: the marker DB's level names and
        // UObject::GetFullName()'s need not agree on case, and both sides of that join
        // must lower-case identically.
        using mdb::lower_ascii;

        // The chunked object-array walk (game thread only).
        scan::Cursor g_cursor{};
        scan::RoundStats g_round_stats{};
        std::uint64_t g_last_slice_us = 0;
        std::uint64_t g_round_start_us = 0;
        bool g_round_open = false; // false = waiting for the next round's turn

        // Fallback path only: one FindAllOf per pump, cycling the class table. Used
        // when FUObjectArray::GetNumElements() cannot answer (0 or negative), which
        // would mean UE4SS has not resolved GUObjectArray - drawing nothing at all in
        // that case would be worse than a slow sweep.
        int g_next_class = 0;
        std::uint64_t g_last_class_ms = 0;

        std::uint64_t qpc_us()
        {
            static LARGE_INTEGER freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return f;
            }();
            LARGE_INTEGER t{};
            ::QueryPerformanceCounter(&t);
            if (freq.QuadPart <= 0)
            {
                return 0;
            }
            return static_cast<std::uint64_t>(t.QuadPart) * 1000000ull /
                   static_cast<std::uint64_t>(freq.QuadPart);
        }

        //==============================================================================
        // Raw reads on the game thread
        //==============================================================================

        bool read_bool_prop(UObject* obj, const wchar_t* name, bool& out)
        {
            const uer::ClassLayout* layout = g_layouts.get(obj);
            std::uint8_t b = 0;
            if (!uer::read_prop(layout, obj, name, b, 1))
            {
                return false;
            }
            out = (b != 0);
            return true;
        }

        // An FString UPROPERTY: { TCHAR* data; int32 num; int32 max }. `num` counts the
        // terminating null, so an empty string is num == 0 or 1.
        struct FStringRaw
        {
            void* data = nullptr;
            std::int32_t num = 0;
            std::int32_t max = 0;
        };

        bool read_fstring_prop(UObject* obj, const wchar_t* name, std::wstring& out)
        {
            const uer::ClassLayout* layout = g_layouts.get(obj);
            const uer::Prop* p = uer::find_prop(layout, name);
            if (p == nullptr || p->size != static_cast<int>(sizeof(FStringRaw)))
            {
                return false;
            }
            FStringRaw s{};
            if (!mem::read_at(obj, p->offset, s))
            {
                return false;
            }
            if (s.num <= 1 || s.num > 512 || !mem::plausible_ptr(s.data))
            {
                return false;
            }
            const std::size_t chars = static_cast<std::size_t>(s.num) - 1;
            std::wstring tmp;
            tmp.resize(chars);
            if (!mem::readable(s.data, chars * sizeof(wchar_t)) ||
                !mem::copy(s.data, tmp.data(), chars * sizeof(wchar_t)))
            {
                return false;
            }
            out.swap(tmp);
            return true;
        }

        // AActor has no reflected transform; the world transform of a world-placed
        // actor is its root component's RelativeLocation (no attach parent, so relative
        // IS world). That keeps the sweep to two guarded reads per actor - a
        // K2_GetActorLocation ProcessEvent for 129 pickups plus 95 enemies would be
        // orders of magnitude more expensive, and this runs inside the engine's own
        // call stack.
        bool actor_location(UObject* actor, double& x, double& y, double& z)
        {
            const uer::ClassLayout* layout = g_layouts.get(actor);
            UObject* root = uer::read_object_prop(layout, actor, L"RootComponent");
            if (root == nullptr)
            {
                return false;
            }
            const uer::ClassLayout* root_layout = g_layouts.get(root);
            uer::FVec3 v{};
            if (!uer::read_prop(root_layout, root, L"RelativeLocation", v, static_cast<int>(sizeof(uer::FVec3))))
            {
                return false;
            }
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            {
                return false;
            }
            x = v.x;
            y = v.y;
            z = v.z;
            return true;
        }

        // Walks the class and every super struct by NAME. Cached per UClass*, so the
        // wstring allocations happen once per class per level, not once per actor.
        int spec_for_class(UObject* obj)
        {
            RC::Unreal::UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return -1;
            }
            const auto it = g_class_spec.find(cls);
            if (it != g_class_spec.end())
            {
                return it->second;
            }
            int found = -1;
            auto* current = static_cast<UStruct*>(cls);
            for (int depth = 0; current != nullptr && depth < 48 && found < 0; ++depth)
            {
                if (!mem::readable(current, 0x40))
                {
                    break;
                }
                const std::wstring name = static_cast<UObject*>(current)->GetName();
                for (int i = 0; i < kClassCount; ++i)
                {
                    if (name == kClasses[i].name)
                    {
                        found = i;
                        break;
                    }
                }
                current = current->GetSuperStruct();
            }
            // The cap has to clear the WHOLE game's class count, not the marker
            // classes': the chunked walk asks about every class that owns an object, so
            // a cap of 8192 (what the FindAllOf sweep needed) would be hit mid-round and
            // throw away exactly the negative answers that make the walk cheap.
            if (g_class_spec.size() > g_class_cache_max)
            {
                g_class_spec.clear();
            }
            g_class_spec.emplace(cls, found);
            return found;
        }

        const std::string& id_for(UObject* obj)
        {
            const auto it = g_id_cache.find(obj);
            if (it != g_id_cache.end())
            {
                return it->second;
            }
            const std::string full = narrow_ascii(obj->GetFullName());
            const std::string level = mdb::level_from_full_name(full);
            std::string id = mdb::stable_id(level, narrow_ascii(obj->GetName()));
            if (g_id_cache.size() > g_id_cache_max)
            {
                g_id_cache.clear();
            }
            return g_id_cache.emplace(obj, std::move(id)).first->second;
        }

        void note_found(const std::string& id)
        {
            if (id.empty() || g_found_gt.contains(id))
            {
                return;
            }
            g_found_gt.insert(id);
            // Keep the interned found flags in step. One hash lookup for a find that
            // has just happened, instead of a lookup per marker per round.
            if (g_idx_db != nullptr)
            {
                const auto it = g_idx_db->by_id.find(id);
                if (it != g_idx_db->by_id.end() && it->second >= 0 &&
                    static_cast<std::size_t>(it->second) < g_found_static.size())
                {
                    g_found_static[static_cast<std::size_t>(it->second)] = 1;
                }
            }
            {
                Guard guard(g_outbox_lock);
                if (g_outbox.size() < 8192)
                {
                    g_outbox.push_back(id);
                }
            }
            g_outbox_pending.store(true, std::memory_order_release);
        }

        void drain_inbox()
        {
            if (!g_inbox_pending.exchange(false, std::memory_order_acquire))
            {
                return;
            }
            std::vector<std::string> ids;
            bool clear = false;
            {
                Guard guard(g_inbox_lock);
                ids.swap(g_inbox);
                clear = g_inbox_clear;
                g_inbox_clear = false;
            }
            if (clear)
            {
                g_found_gt.clear();
            }
            for (std::string& id : ids)
            {
                g_found_gt.insert(std::move(id));
            }
            // The whole set was replaced: rebuild the interned flags before the next
            // publish rather than doing a lookup per id here.
            g_found_index_dirty = true;
        }

        //==============================================================================
        // One marker actor (game thread)
        //==============================================================================
        //
        // Everything expensive about a marker - the layout cache, the RootComponent
        // location read, the state-flag read, the FullName-derived id - lives here, and
        // it only ever runs for an object whose class IS in kClasses. The chunked walk
        // steps over hundreds of thousands of slots per round and reaches this for a
        // few hundred of them.
        void process_marker(UObject* obj, const ClassSpec& s)
        {
            // The marker actor. For an AI controller it is the pawn it possesses.
            UObject* actor = obj;
            if (s.rule == Rule::ControllerPawn)
            {
                const uer::ClassLayout* layout = g_layouts.get(obj);
                actor = uer::read_object_prop(layout, obj, L"Pawn");
                if (actor == nullptr)
                {
                    return;
                }
            }

            LiveEntry e{};
            e.cat = s.cat;
            e.cls = s.name;
            e.persist = s.persist;
            e.round = g_round;
            if (actor_location(actor, e.x, e.y, e.z))
            {
                // (0,0,0) is not a position here: it is where the level saver parks
                // a collected pickup. Keep the entry (its `found` still matters) but
                // never draw it there.
                e.pos_valid = !(e.x == 0.0 && e.y == 0.0 && e.z == 0.0);
            }

            switch (s.rule)
            {
            case Rule::UsedBool:
            {
                bool used = false;
                if (read_bool_prop(actor, L"Used", used))
                {
                    e.found = used;
                }
                break;
            }
            case Rule::DoorOpenBool:
            {
                bool open = false;
                if (read_bool_prop(actor, L"DoorOpen", open))
                {
                    e.found = open;
                }
                break;
            }
            case Rule::ActiveBool:
            {
                bool active = false;
                if (read_bool_prop(actor, L"Active", active))
                {
                    e.found = active;
                }
                break;
            }
            case Rule::PickupDying:
            {
                bool dying = false;
                if (read_bool_prop(actor, L"dying", dying) && dying)
                {
                    e.found = true;
                }
                if (!e.pos_valid)
                {
                    e.found = true; // parked at the origin = already collected
                }
                break;
            }
            case Rule::ControllerPawn:
            case Rule::None:
            default:
                break;
            }

            // The id. Shrines carry a game-authored one; everything else is
            // <level short name>/<object name>, which is the join key the offline
            // extractor writes into markers/<chapter>.json.
            std::string id;
            if (s.cat == mdb::Cat::Shrine)
            {
                std::wstring shrine;
                if (read_fstring_prop(actor, kShrineIdProp, shrine) && !shrine.empty())
                {
                    id = narrow_ascii(shrine);
                }
            }
            if (id.empty())
            {
                id = id_for(actor);
            }
            if (id.empty())
            {
                return;
            }
            // NOTE: enemies are NOT namespaced. The offline extractor writes an
            // enemy's spawn point into the static DB under the same
            // <level>/<object name> id, and giving the live one a prefix of its own
            // would draw the spawn point and the live pawn as two markers. What
            // keeps enemies out of the collection tracker is `persist == false`,
            // not the id.

            if (e.found && e.persist)
            {
                note_found(id);
            }
            if (g_live.size() < g_live_max || g_live.contains(id))
            {
                g_live[id] = e;
            }
        }

        //==============================================================================
        // The chunked GUObjectArray walk (game thread)
        //==============================================================================
        //
        // One slice = one game-thread pump. Per slot the cost is a bounds-checked
        // FUObjectItem lookup, a validity test, the object's class pointer and one
        // memoised UClass* -> spec index lookup. Only a class that IS a marker class
        // pays for anything more.
        //
        // The rejects, cheapest first:
        //   * FUObjectItem::IsValid(false) - null slot, pending kill, or unreachable.
        //     Read through the object ARRAY, never through the object, so a slot whose
        //     allocation has already been freed is safe to look at.
        //   * spec_for_class() < 0        - not a marker class. The overwhelming case,
        //     and memoised per UClass*, so the super-chain name walk (wstring compares)
        //     runs once per class per level, not once per object per round.
        //   * IsValidObjectForFindXOf()   - CDOs and archetypes: exactly what FindAllOf
        //     used to filter out on our behalf.
        int scan_slice(const scan::Slice& slice)
        {
            int visited = 0;
            for (int i = slice.begin; i < slice.end; ++i)
            {
                ++visited;
                RC::Unreal::FUObjectItem* item = RC::Unreal::FUObjectArray::IndexToObject(i);
                if (item == nullptr || !item->IsValid(false))
                {
                    continue;
                }
                UObject* obj = item->GetUObject();
                if (obj == nullptr)
                {
                    continue;
                }
                const int si = spec_for_class(obj);
                if (si < 0)
                {
                    continue;
                }
                if (!UObjectGlobals::IsValidObjectForFindXOf(obj) || !mem::readable(obj, 0x40))
                {
                    continue;
                }
                process_marker(obj, kClasses[si]);
            }
            return visited;
        }

        //==============================================================================
        // Fallback: one FindAllOf per pump, cycling the class table (game thread)
        //==============================================================================
        //
        // Only reached when FUObjectArray::GetNumElements() cannot answer. This is the
        // old, slow path - 28.30 ms mean / 51.05 ms peak per pump when it was measured
        // in-game - and it exists purely so a UE4SS build that fails to resolve
        // GUObjectArray still draws markers instead of nothing.
        void sweep_class(int index)
        {
            const ClassSpec& spec = kClasses[index];
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(spec.name, found);
            if (found.empty())
            {
                return;
            }
            const std::size_t kMaxPerClass = g_fallback_max_per_class;
            const std::size_t count = found.size() < kMaxPerClass ? found.size() : kMaxPerClass;

            for (std::size_t i = 0; i < count; ++i)
            {
                UObject* obj = found[i];
                if (obj == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(obj) || !mem::readable(obj, 0x40))
                {
                    continue;
                }
                const int si = spec_for_class(obj);
                process_marker(obj, si >= 0 ? kClasses[si] : spec);
            }
        }

        //==============================================================================
        // Publishing the draw buffer (game thread, once per round)
        //==============================================================================

        // The chapter the static DB is filtered to, or chid::kNone for "show
        // everything". Recomputed on EVERY publish and deliberately NOT latched: a
        // chapter change must switch the visible set on the next round, and
        // `mapdata` already logs the switch itself.
        int filter_chapter_now()
        {
            if (!mm::cfg_cached().markers_filter_chapter)
            {
                return chid::kNone;
            }
            return mapdata::detected_chapter();
        }

        //==============================================================================
        // The per-round index (game thread)
        //==============================================================================

        // Size every per-marker array to the database that is actually loaded. Called
        // from publish_round when the pointer differs from the one the arrays describe,
        // which happens once at start-up and once per F5 reload.
        void rebuild_static_index(const StaticDb* db)
        {
            g_idx_db = db;
            const std::size_t n = db != nullptr ? db->markers.size() : 0;
            const std::size_t levels = db != nullptr ? db->levels.size() : 0;
            g_found_static.assign(n, 0);
            g_absent_streak_idx.assign(n, 0);
            g_live_of_static.assign(n, nullptr);
            g_level_known.assign(levels, 0);
            g_level_round.assign(levels, 0);
            g_chapter_subset.clear();
            g_subset_valid = false;
            g_found_index_dirty = true;
        }

        // Recompute the found flags from the authoritative string set. Only on a reload
        // of wuchang_minimap_found.txt or a database swap - a single new find sets its
        // own flag in note_found().
        void refresh_found_index(const StaticDb* db)
        {
            g_found_index_dirty = false;
            if (db == nullptr)
            {
                return;
            }
            std::fill(g_found_static.begin(), g_found_static.end(), static_cast<std::uint8_t>(0));
            for (const std::string& id : g_found_gt)
            {
                const auto it = db->by_id.find(id);
                if (it != db->by_id.end() && it->second >= 0 &&
                    static_cast<std::size_t>(it->second) < g_found_static.size())
                {
                    g_found_static[static_cast<std::size_t>(it->second)] = 1;
                }
            }
        }

        // The chapter-filtered subrange. The static DB holds all six chapters in one
        // flat set and only one of them is ever drawn, so the filter is applied ONCE per
        // chapter change instead of once per marker per round.
        void refresh_chapter_subset(const StaticDb* db, int filter_chapter)
        {
            g_subset_chapter = filter_chapter;
            g_subset_valid = true;
            g_chapter_subset.clear();
            if (db == nullptr)
            {
                return;
            }
            g_chapter_subset.reserve(db->markers.size());
            for (int i = 0; i < static_cast<int>(db->markers.size()); ++i)
            {
                if (mdb::marker_in_chapter(db->markers[static_cast<std::size_t>(i)].chapter, filter_chapter))
                {
                    g_chapter_subset.push_back(i);
                }
            }
        }

        // Perf counters (see perf.hpp). Namespace-scope ints rather than function
        // statics: a guarded static's first call would run the CRT's thread-safe-init
        // path on the game thread, and this mod does not touch the host CRT there.
        int g_pf_publish = -1;
        int g_pf_scan = -1;

        void publish_round()
        {
            const std::uint64_t t0 = qpc_us();
            if (g_pf_publish < 0)
            {
                g_pf_publish = mm::perf_register("publish_round", perf::Thread::Game);
            }

            // Drop live actors that have not answered for two rounds: their level was
            // unloaded, or (for an enemy) they died. Absence is NEVER treated as
            // "collected" - only the state flags do that.
            for (auto it = g_live.begin(); it != g_live.end();)
            {
                if (g_round >= g_grace_rounds && it->second.round + g_grace_rounds <= g_round)
                {
                    it = g_live.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            std::vector<DrawMarker>& dst = g_slot[g_slot_next];
            // clear() keeps the capacity, so after the first round this allocates
            // nothing at all.
            dst.clear();

            // The static DB holds ALL six chapters' markers in one flat set, and the
            // chapters' world bounds overlap (chapter 4 covers nearly all of chapter
            // 1), so an unfiltered publish paints foreign markers over the current
            // map. This is the single point where that is decided - the minimap, the
            // full map, the compass pips and the x-ray highlight all read the buffer
            // published here, so none of them needs a filter of its own.
            const int filter_chapter = filter_chapter_now();

            const StaticDb* db = g_db.load(std::memory_order_acquire);
            if (db != g_idx_db)
            {
                rebuild_static_index(db);
            }
            if (db != nullptr)
            {
                if (g_found_index_dirty)
                {
                    refresh_found_index(db);
                }
                if (!g_subset_valid || g_subset_chapter != filter_chapter)
                {
                    refresh_chapter_subset(db, filter_chapter);
                }

                // The live twins, resolved the cheap way round: one hash lookup per LIVE
                // actor (a few hundred) instead of one per STATIC marker (up to 3 601).
                std::fill(g_live_of_static.begin(), g_live_of_static.end(), nullptr);
                for (const auto& kv : g_live)
                {
                    const auto sit = db->by_id.find(kv.first);
                    if (sit != db->by_id.end() && sit->second >= 0 &&
                        static_cast<std::size_t>(sit->second) < g_live_of_static.size())
                    {
                        g_live_of_static[static_cast<std::size_t>(sit->second)] = &kv.second;
                    }
                }

                // The loaded-level state, resolved once per UNIQUE level name rather
                // than once per marker (and with no lower_ascii() allocation per marker:
                // the names were interned lower-cased at load).
                for (std::size_t i = 0; i < g_level_known.size(); ++i)
                {
                    const auto lit = g_levels.find(db->levels[i]);
                    g_level_known[i] = lit != g_levels.end() ? 1 : 0;
                    g_level_round[i] = lit != g_levels.end() ? lit->second : 0;
                }

                dst.reserve(g_chapter_subset.size() + g_live.size());
                for (const int mi : g_chapter_subset)
                {
                    const std::size_t idx = static_cast<std::size_t>(mi);
                    const mdb::StaticMarker& sm = db->markers[idx];
                    DrawMarker d{};
                    d.x = sm.x;
                    d.y = sm.y;
                    d.z = sm.z;
                    d.cat = static_cast<std::uint8_t>(sm.cat);
                    d.rarity = sm.rarity;
                    d.flags = kFlagStatic;
                    if (g_found_static[idx] != 0)
                    {
                        d.flags |= kFlagFound;
                    }
                    const LiveEntry* live = g_live_of_static[idx];
                    if (live != nullptr)
                    {
                        d.flags |= kFlagLive;
                        if (live->pos_valid)
                        {
                            d.x = live->x;
                            d.y = live->y;
                            d.z = live->z;
                        }
                        if (live->found)
                        {
                            d.flags |= kFlagFound;
                        }
                    }

                    // ---- ABSENCE AS EVIDENCE OF A COLLECT -------------------------
                    //
                    // Everything the pure predicate needs is here: this round has just
                    // walked the whole object array, `live` is what it found for this
                    // id, and the level table says whether the marker's level is loaded
                    // and for how long. See mdb::absence_marks() for the rule and the
                    // reason absence is normally NOT evidence.
                    mdb::AbsenceFacts facts{};
                    facts.feature_on = g_absence_on;
                    facts.cat_selected = mdb::cat_enabled(g_absence_cats, sm.cat);
                    facts.already_found = (d.flags & kFlagFound) != 0;
                    const int li = db->marker_level[idx];
                    if (li >= 0 && g_level_known[static_cast<std::size_t>(li)] != 0)
                    {
                        facts.level_known = true;
                        facts.full_round_since_level_load =
                            g_round > g_level_round[static_cast<std::size_t>(li)];
                    }
                    facts.twin_alive = live != nullptr && live->round == g_round && live->pos_valid &&
                                       !live->found;

                    int& streak = g_absent_streak_idx[idx];
                    if (!mdb::absence_round_confirms(facts))
                    {
                        streak = 0;
                    }
                    else
                    {
                        if (streak < 1000000)
                        {
                            ++streak;
                        }
                        if (mdb::absence_marks(facts, streak, g_absence_rounds))
                        {
                            d.flags |= kFlagFound;
                            streak = 0;
                            g_absence_marks.fetch_add(1, std::memory_order_relaxed);
                            note_found(sm.id);
                        }
                    }
                    copy_id(d.id, sizeof(d.id), sm.id);
                    // NAME FIRST. `cls` is always non-empty ("BP_PickupActor_C"), so the
                    // old `cls.empty() ? name : cls` meant every pickup's tooltip read
                    // "pickup_actor" while the DB carried a real display name for 1 050
                    // of them. markers/chapter*.json now carries a name for every entry
                    // (a real item name where the extractor could resolve one, the
                    // category label otherwise), so preferring it never makes a label
                    // worse. Longest shipped name is 31 ASCII chars; `label` is 40.
                    copy_id(d.label, sizeof(d.label), sm.name.empty() ? sm.cls : sm.name);
                    dst.push_back(d);
                }
            }

            // Live actors the static DB does not know about - which is everything
            // until tools/markers has produced markers/<chapter>.json, and always the
            // enemies.
            for (const auto& kv : g_live)
            {
                if (!kv.second.pos_valid)
                {
                    continue; // no usable position and no static entry to fall back on
                }
                if (db != nullptr)
                {
                    // Only skip the live actor when its static twin was actually
                    // drawn above. A live actor is by definition in the current
                    // world, so it is never filtered out itself - but its static
                    // entry may carry another chapter's number.
                    const auto sit = db->by_id.find(kv.first);
                    if (sit != db->by_id.end() && sit->second >= 0 &&
                        sit->second < static_cast<int>(db->markers.size()) &&
                        mdb::marker_in_chapter(db->markers[static_cast<std::size_t>(sit->second)].chapter,
                                               filter_chapter))
                    {
                        continue;
                    }
                }
                DrawMarker d{};
                d.x = kv.second.x;
                d.y = kv.second.y;
                d.z = kv.second.z;
                d.cat = static_cast<std::uint8_t>(kv.second.cat);
                d.flags = kFlagLive;
                if (kv.second.found || g_found_gt.contains(kv.first))
                {
                    d.flags |= kFlagFound;
                }
                copy_id(d.id, sizeof(d.id), kv.first);
                if (kv.second.cls != nullptr)
                {
                    copy_id(d.label, sizeof(d.label), narrow_ascii(kv.second.cls));
                }
                dst.push_back(d);
            }

            g_published_count.store(static_cast<int>(dst.size()), std::memory_order_relaxed);
            g_live_count.store(static_cast<int>(g_live.size()), std::memory_order_relaxed);
            g_slot_published.store(g_slot_next, std::memory_order_release);
            g_slot_next = (g_slot_next + 1) % kSlots;

            // What the publish cost. It was the one unsliced, unmeasured game-thread
            // burst left; the F2 round line and the periodic log print all three
            // numbers so a regression here is visible without a play session.
            const double ms = static_cast<double>(qpc_us() - t0) / 1000.0;
            g_publish_ms.store(ms, std::memory_order_relaxed);
            g_publish_ms_sum += ms;
            ++g_publish_count;
            g_publish_ms_avg.store(g_publish_ms_sum / static_cast<double>(g_publish_count),
                                   std::memory_order_relaxed);
            mm::perf_record(g_pf_publish, t0);
            if (ms > g_publish_ms_peak.load(std::memory_order_relaxed))
            {
                g_publish_ms_peak.store(ms, std::memory_order_relaxed);
            }
        }

        //==============================================================================
        // The fallback pump (game thread)
        //==============================================================================
        //
        // Byte-for-byte the pre-2026-09-02 behaviour: one FindAllOf per interval,
        // cycling the class table. It is only reached when GUObjectArray reports no
        // elements, i.e. when the chunked walk has nothing to walk. Its cost lands in
        // the same F2 counters, flagged by `scan_fallback` so a 28 ms reading is never
        // mistaken for the fast path.
        void legacy_pump(std::uint64_t now, const mm::Config& cfg)
        {
            g_scan_fallback.store(true, std::memory_order_relaxed);
            const int rounds_per_sec = cfg.markers_rounds_per_sec < 1 ? 1 : cfg.markers_rounds_per_sec;
            const std::uint64_t interval = static_cast<std::uint64_t>(1000 / (rounds_per_sec * kClassCount) + 1);
            if (now - g_last_class_ms < interval)
            {
                return;
            }
            g_last_class_ms = now;

            const std::uint64_t t0 = qpc_us();
            sweep_class(g_next_class);
            const double ms = static_cast<double>(qpc_us() - t0) / 1000.0;

            scan::note_slice(g_round_stats, ms, 0);
            g_scan_slice_ms.store(ms, std::memory_order_relaxed);
            if (ms > g_scan_slice_ms_max.load(std::memory_order_relaxed))
            {
                g_scan_slice_ms_max.store(ms, std::memory_order_relaxed);
            }

            ++g_next_class;
            if (g_next_class >= kClassCount)
            {
                g_next_class = 0;
                publish_round();
                ++g_round;
                g_rounds.store(g_round, std::memory_order_relaxed);
                g_scan_slice_ms_avg.store(g_round_stats.avg_ms(), std::memory_order_relaxed);
                g_scan_slice_ms_peak.store(g_round_stats.peak_ms, std::memory_order_relaxed);
                g_scan_round_ms.store(g_round_stats.total_ms, std::memory_order_relaxed);
                g_scan_round_slices.store(g_round_stats.slices, std::memory_order_relaxed);
                g_scan_round_objects.store(0, std::memory_order_relaxed);
                g_round_stats = scan::RoundStats{};
            }
        }

        //==============================================================================
        // Loading (loop thread)
        //==============================================================================

        std::wstring markers_dir()
        {
            return mm::mod_dir() + L"\\markers";
        }

        //------------------------------------------------------------------------------
        // WHICH found file (loop thread only)
        //------------------------------------------------------------------------------
        //
        // `g_found_key` is the save-slot key the loop thread last acted on. It is a
        // COPY, deliberately: the resolution runs on the game thread and the loop thread
        // must not change the file it is reading from half way through a load.

        std::string g_found_key;         // "" = the shared file
        std::string g_found_route = "unresolved";
        bool g_found_key_valid = false;  // has a key ever been taken from slotid?

        std::wstring found_path_for(const std::string& key)
        {
            const std::string name = slotid::found_filename(key);
            std::wstring wide;
            wide.reserve(name.size());
            for (char c : name)
            {
                wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return mm::mod_dir() + L"\\" + wide;
        }

        std::wstring found_path()
        {
            return found_path_for(g_found_key);
        }

        bool file_exists(const std::wstring& path)
        {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // First sight of a slot that has no file of its own: seed it from the shared
        // one, so upgrading from 0.9.3 does not read as "my whole collection is gone".
        // Once, and logged - a silent file copy is exactly the kind of thing that is
        // impossible to explain afterwards.
        void migrate_shared_into(const std::string& key)
        {
            if (key.empty())
            {
                return;
            }
            const std::wstring dst = found_path_for(key);
            if (file_exists(dst))
            {
                return;
            }
            const std::wstring src = found_path_for(std::string{});
            std::string text;
            if (!file_exists(src) || !read_whole_file(src, text) || text.empty())
            {
                return;
            }
            if (write_whole_file(dst, text))
            {
                mm::logf(L"markers: first sight of save slot '{}' - copied the shared found tracker "
                         L"({} bytes) into {}",
                         std::wstring(key.begin(), key.end()), text.size(), dst);
            }
            else
            {
                mm::logf(L"markers: could not seed {} from the shared found tracker (error {})", dst,
                         static_cast<unsigned>(::GetLastError()));
            }
        }

        // Takes whatever key slotid has resolved and, if it differs from the one in
        // force, migrates and swaps the file. Loop thread. Returns true when the file
        // changed, i.e. when the caller must reload it.
        bool adopt_slot_key()
        {
            const slotid::Status st = slotid::status();
            const std::string key{st.key};
            const std::string route = slotid::route_name(st.route);
            if (g_found_key_valid && key == g_found_key)
            {
                g_found_route = route;
                return false;
            }
            const bool first = !g_found_key_valid;
            g_found_key = key;
            g_found_route = route;
            g_found_key_valid = true;
            migrate_shared_into(key);
            mm::logf(L"markers: found tracker profile {} '{}' via {} ({}) -> {}",
                     first ? L"=" : L"changed to", std::wstring(key.begin(), key.end()),
                     std::wstring(route.begin(), route.end()),
                     std::wstring(st.note, st.note + std::strlen(st.note)), found_path_for(key));
            return true;
        }

        void publish_inbox(std::vector<std::string> ids, bool clear)
        {
            {
                Guard guard(g_inbox_lock);
                g_inbox_clear = g_inbox_clear || clear;
                for (std::string& id : ids)
                {
                    if (g_inbox.size() < 65536)
                    {
                        g_inbox.push_back(std::move(id));
                    }
                }
            }
            g_inbox_pending.store(true, std::memory_order_release);
        }

        void recompute_stats()
        {
            Stats s{};
            const StaticDb* db = g_db.load(std::memory_order_acquire);
            if (db != nullptr)
            {
                s.db_loaded = true;
                s.static_markers = static_cast<int>(db->markers.size());
                std::unordered_set<int> chapters;
                for (const mdb::StaticMarker& m : db->markers)
                {
                    const int ci = static_cast<int>(m.cat);
                    if (ci < 0 || ci >= mdb::kCatCount)
                    {
                        continue;
                    }
                    const bool found = g_found_master.contains(m.id);
                    ++s.cat[ci].total;
                    s.cat[ci].found += found ? 1 : 0;
                    // Chapter 0 is the bucket for a manifest whose "chapter" is not a
                    // number - the DLC one spells it "DLC". Those markers are counted,
                    // not dropped.
                    if (m.chapter >= 0 && m.chapter <= 8)
                    {
                        chapters.insert(m.chapter);
                        ++s.chapter[m.chapter][ci].total;
                        s.chapter[m.chapter][ci].found += found ? 1 : 0;
                    }
                }
                s.chapters_loaded = static_cast<int>(chapters.size());
            }
            s.found_ids = static_cast<int>(g_found_master.size());
            {
                const std::string name = slotid::found_filename(g_found_key);
                ::strncpy_s(s.found_file, sizeof(s.found_file), name.c_str(), _TRUNCATE);
                ::strncpy_s(s.found_route, sizeof(s.found_route), g_found_route.c_str(), _TRUNCATE);
            }
        s.absence_marks = g_absence_marks.load(std::memory_order_relaxed);
        s.levels_loaded = g_levels_loaded.load(std::memory_order_relaxed);
            s.filter_chapter = filter_chapter_now();
            s.published = g_published_count.load(std::memory_order_relaxed);
            s.live_entries = g_live_count.load(std::memory_order_relaxed);
            s.rounds = g_rounds.load(std::memory_order_relaxed);
            s.scan_slice_ms = g_scan_slice_ms.load(std::memory_order_relaxed);
            s.scan_slice_ms_avg = g_scan_slice_ms_avg.load(std::memory_order_relaxed);
            s.scan_slice_ms_peak = g_scan_slice_ms_peak.load(std::memory_order_relaxed);
            s.scan_slice_ms_max = g_scan_slice_ms_max.load(std::memory_order_relaxed);
            s.scan_round_ms = g_scan_round_ms.load(std::memory_order_relaxed);
            s.publish_ms = g_publish_ms.load(std::memory_order_relaxed);
            s.publish_ms_avg = g_publish_ms_avg.load(std::memory_order_relaxed);
            s.publish_ms_peak = g_publish_ms_peak.load(std::memory_order_relaxed);
            s.scan_round_slices = g_scan_round_slices.load(std::memory_order_relaxed);
            s.scan_round_objects = g_scan_round_objects.load(std::memory_order_relaxed);
            s.scan_total = g_scan_total.load(std::memory_order_relaxed);
            s.scan_chunk = g_scan_chunk.load(std::memory_order_relaxed);
            s.scan_fallback = g_scan_fallback.load(std::memory_order_relaxed);
            Guard guard(g_stats_lock);
            g_stats = s;
        }

        void load_static_db()
        {
            const std::wstring dir = markers_dir();

            // Enumerate the directory rather than probing chapter1..8: the offline
            // extractor also emits chapterdlc.json, and it will emit whatever the game
            // adds next. `*.sample.json` is documentation, not data, and is skipped.
            std::vector<std::wstring> files_found;
            WIN32_FIND_DATAW find{};
            const HANDLE h = ::FindFirstFileW((dir + L"\\*.json").c_str(), &find);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        continue;
                    }
                    const std::wstring name = find.cFileName;
                    if (name.find(L".sample.") != std::wstring::npos)
                    {
                        continue;
                    }
                    files_found.push_back(name);
                } while (::FindNextFileW(h, &find) != 0 && files_found.size() < 64);
                ::FindClose(h);
            }
            // Deterministic order, so the "first duplicate id wins" rule is stable
            // across runs.
            std::sort(files_found.begin(), files_found.end());

            auto db = std::make_unique<StaticDb>();
            int files = 0;
            for (const std::wstring& name : files_found)
            {
                const std::wstring path = dir + L"\\" + name;
                std::string text;
                if (!read_whole_file(path, text))
                {
                    continue;
                }
                mdb::ParseReport report{};
                const std::size_t before = db->markers.size();
                if (!mdb::parse_markers_json(text, db->markers, report))
                {
                    db->markers.resize(before);
                    // A sibling manifest of a DIFFERENT schema is not an error: the
                    // offline extractor also writes markers/items.json (the item
                    // display-name database, schema wuchang-minimap-items/1) into this
                    // directory, and the enumeration above deliberately takes every
                    // *.json so a chapter file the game adds later is picked up without
                    // a code change. Only a file that claims to BE a marker manifest is
                    // reported as broken.
                    if (!report.schema.empty() &&
                        report.schema.compare(0, kMarkerSchemaPrefix.size(), kMarkerSchemaPrefix) != 0)
                    {
                        mm::logf(L"markers: {} is not a marker manifest (schema {}) - skipped",
                                 path,
                                 widen(report.schema));
                    }
                    else
                    {
                        mm::logf(L"markers: {} rejected - {}", path, widen(report.error));
                    }
                    continue;
                }
                ++files;
                mm::logf(L"markers: {} -> {} marker(s) (chapter {}, schema {}{}{})",
                         path,
                         report.added,
                         widen(report.chapter_label.empty() ? std::string{"?"} : report.chapter_label),
                         widen(report.schema),
                         report.skipped != 0 ? std::format(L", {} skipped", report.skipped) : std::wstring{},
                         report.unknown_cat != 0 ? std::format(L", {} unknown category", report.unknown_cat)
                                                 : std::wstring{});
            }

            int duplicates = 0;
            for (int i = 0; i < static_cast<int>(db->markers.size()); ++i)
            {
                if (!db->by_id.emplace(db->markers[static_cast<std::size_t>(i)].id, i).second)
                {
                    ++duplicates;
                }
            }
            if (duplicates != 0)
            {
                mm::logf(L"markers: {} duplicate id(s) in the database - the first one wins", duplicates);
            }

            // Intern the level names. The absence rule asks "is this marker's level
            // loaded, and since when" for every marker of every round; interning turns
            // that from a lower_ascii() allocation plus a string hash per marker into one
            // hash per UNIQUE level per round and an array index per marker.
            mdb::intern_levels(db->markers, db->levels, db->marker_level);

            if (files == 0)
            {
                mm::logf(L"markers: no markers\\<chapter>.json under {} - the minimap will show LIVE markers "
                         L"only (shrines, chests, pickups and enemies that are currently streamed in). "
                         L"Build the static DB with tools/markers.",
                         dir);
            }
            else
            {
                mm::logf(L"markers: static database ready - {} marker(s) from {} file(s)", db->markers.size(), files);
            }
            g_db.store(db.release(), std::memory_order_release); // deliberately leaked on reload
        }

        void load_found_file()
        {
            g_found_master.clear();
            std::string text;
            const std::wstring path = found_path();
            std::vector<std::string> ids;
            if (read_whole_file(path, text))
            {
                mdb::found_parse(text, ids);
                mm::logf(L"markers: found tracker {} -> {} id(s)", path, ids.size());
            }
            else
            {
                mm::logf(L"markers: found tracker {} does not exist yet - it is written the first time "
                         L"something is auto-marked",
                         path);
            }
            for (const std::string& id : ids)
            {
                g_found_master.insert(id);
            }
            publish_inbox(ids, true);
            g_found_dirty = false;
        }

        void save_found_file()
        {
            std::vector<std::string> ids;
            ids.reserve(g_found_master.size());
            for (const std::string& id : g_found_master)
            {
                ids.push_back(id);
            }
            const std::string text = mdb::found_serialize(std::move(ids));
            const std::wstring path = found_path();
            if (write_whole_file(path, text))
            {
                mm::logf(L"markers: found tracker saved ({} id(s)) -> {}", g_found_master.size(), path);
            }
            else
            {
                mm::logf(L"markers: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            }
            g_found_dirty = false;
        }
    } // namespace

    //======================================================================================
    // Public API
    //======================================================================================

    View view()
    {
        const int slot = g_slot_published.load(std::memory_order_acquire);
        if (slot < 0 || slot >= kSlots)
        {
            return View{};
        }
        const std::vector<DrawMarker>& v = g_slot[slot];
        return View{v.data(), v.size()};
    }

    Stats stats()
    {
        Guard guard(g_stats_lock);
        return g_stats;
    }

    std::uint64_t rounds()
    {
        return g_rounds.load(std::memory_order_relaxed);
    }

    void on_unreal_init()
    {
        load_static_db();
        slotid::on_unreal_init();
        adopt_slot_key();
        load_found_file();
        recompute_stats();
        const mm::Config cfg = mm::config();
        mm::logf(L"markers: {} ({} live sweep, {} found tracker); categories = {}; "
                 L"scan = {} slot(s)/pump every {} ms, {} full round(s)/s max",
                 cfg.markers_enabled ? L"enabled" : L"DISABLED",
                 cfg.markers_live ? L"with" : L"without",
                 cfg.found_tracker ? L"with" : L"without",
                 widen(mdb::format_category_mask(cfg.markers_categories)),
                 cfg.markers_scan_chunk,
                 cfg.markers_scan_period_ms,
                 cfg.markers_rounds_per_sec);
    }

    void request_toggle_found(const char* id, bool found)
    {
        if (id == nullptr || id[0] == '\0')
        {
            return;
        }
        {
            Guard guard(g_toggle_lock);
            if (g_toggle.size() >= 256)
            {
                return; // somebody is holding the mouse button down on a marker
            }
            g_toggle.push_back(ToggleReq{std::string{id}, found});
        }
        g_toggle_pending.store(true, std::memory_order_release);
    }

    void reload()
    {
        load_static_db();
        slotid::rescan_files();
        adopt_slot_key();
        load_found_file();
        recompute_stats();
    }

    void on_update()
    {
        const std::uint64_t now = ::GetTickCount64();

        // The save-slot watch. A slot switch in Wuchang goes through the main menu and a
        // full level reload, which drops every cache and re-arms the game-thread routes;
        // this is where the loop thread notices the answer changed and swaps files. Any
        // pending write goes to the OLD file first - the finds it holds belong to the
        // save that was loaded when they happened.
        static std::uint64_t last_slot_check = 0;
        if (now - last_slot_check >= 1000)
        {
            last_slot_check = now;
            const std::string key_before = g_found_key;
            const bool valid_before = g_found_key_valid;
            if (valid_before && g_found_dirty && slotid::status().key != key_before)
            {
                save_found_file();
            }
            if (adopt_slot_key())
            {
                g_found_dirty = false; // the dirty set belonged to the previous file
                load_found_file();
                recompute_stats();
            }
        }

        if (g_outbox_pending.exchange(false, std::memory_order_acquire))
        {
            std::vector<std::string> ids;
            {
                Guard guard(g_outbox_lock);
                ids.swap(g_outbox);
            }
            int added = 0;
            for (std::string& id : ids)
            {
                if (g_found_master.insert(std::move(id)).second)
                {
                    ++added;
                }
            }
            if (added != 0)
            {
                g_found_dirty = true;
                g_found_dirty_ms = now;
                mm::logf(L"markers: auto-marked {} new marker(s) as found ({} total)", added,
                         g_found_master.size());
                recompute_stats();
            }
        }

        // Manual toggles from the full map. They go through the same master set and
        // the same debounced write as the auto-marks, and the whole set is republished
        // to the game thread so the draw buffer agrees on the next round.
        if (g_toggle_pending.exchange(false, std::memory_order_acquire))
        {
            std::vector<ToggleReq> reqs;
            {
                Guard guard(g_toggle_lock);
                reqs.swap(g_toggle);
            }
            int changed = 0;
            for (const ToggleReq& r : reqs)
            {
                if (r.id.empty())
                {
                    continue;
                }
                if (r.found)
                {
                    changed += g_found_master.insert(r.id).second ? 1 : 0;
                }
                else
                {
                    changed += g_found_master.erase(r.id) != 0 ? 1 : 0;
                }
            }
            if (changed != 0)
            {
                g_found_dirty = true;
                g_found_dirty_ms = now;
                std::vector<std::string> all;
                all.reserve(g_found_master.size());
                for (const std::string& id : g_found_master)
                {
                    all.push_back(id);
                }
                publish_inbox(std::move(all), true);
                recompute_stats();
                mm::logf(L"markers: {} manual found change(s) from the map ({} total)", changed,
                         g_found_master.size());
            }
        }

        const mm::Config& cfg = mm::cfg_cached();
        if (g_found_dirty && cfg.found_tracker &&
            now - g_found_dirty_ms >= static_cast<std::uint64_t>(cfg.found_save_debounce_ms))
        {
            save_found_file();
        }

        // One periodic line so the round's cost is in the log as well as in F2 - an
        // in-game session reports a log file, not a screenshot of the panel.
        static std::uint64_t last_round_log = 0;
        if (now - last_round_log >= 30000)
        {
            last_round_log = now;
            if (g_rounds.load(std::memory_order_relaxed) != 0)
            {
                mm::logf(L"markers: round {} - scan {:.1f} ms over {} pump(s), publish {:.3f} ms "
                         L"(avg {:.3f}, peak {:.3f}); {} published, {} live",
                         g_rounds.load(std::memory_order_relaxed),
                         g_scan_round_ms.load(std::memory_order_relaxed),
                         g_scan_round_slices.load(std::memory_order_relaxed),
                         g_publish_ms.load(std::memory_order_relaxed),
                         g_publish_ms_avg.load(std::memory_order_relaxed),
                         g_publish_ms_peak.load(std::memory_order_relaxed),
                         g_published_count.load(std::memory_order_relaxed),
                         g_live_count.load(std::memory_order_relaxed));
            }
        }

        // Cheap counters the panel shows; recomputing the whole per-chapter table is
        // only worth it when the found set changed, which is handled above.
        static std::uint64_t last_light = 0;
        if (now - last_light >= 1000)
        {
            last_light = now;
            Guard guard(g_stats_lock);
            g_stats.published = g_published_count.load(std::memory_order_relaxed);
            g_stats.absence_marks = g_absence_marks.load(std::memory_order_relaxed);
            g_stats.levels_loaded = g_levels_loaded.load(std::memory_order_relaxed);
            g_stats.live_entries = g_live_count.load(std::memory_order_relaxed);
            g_stats.rounds = g_rounds.load(std::memory_order_relaxed);
            g_stats.scan_slice_ms = g_scan_slice_ms.load(std::memory_order_relaxed);
            g_stats.scan_slice_ms_avg = g_scan_slice_ms_avg.load(std::memory_order_relaxed);
            g_stats.scan_slice_ms_peak = g_scan_slice_ms_peak.load(std::memory_order_relaxed);
            g_stats.scan_slice_ms_max = g_scan_slice_ms_max.load(std::memory_order_relaxed);
            g_stats.scan_round_ms = g_scan_round_ms.load(std::memory_order_relaxed);
            g_stats.publish_ms = g_publish_ms.load(std::memory_order_relaxed);
            g_stats.publish_ms_avg = g_publish_ms_avg.load(std::memory_order_relaxed);
            g_stats.publish_ms_peak = g_publish_ms_peak.load(std::memory_order_relaxed);
            g_stats.scan_round_slices = g_scan_round_slices.load(std::memory_order_relaxed);
            g_stats.scan_round_objects = g_scan_round_objects.load(std::memory_order_relaxed);
            g_stats.scan_total = g_scan_total.load(std::memory_order_relaxed);
            g_stats.scan_chunk = g_scan_chunk.load(std::memory_order_relaxed);
            g_stats.scan_fallback = g_scan_fallback.load(std::memory_order_relaxed);
        }
    }

    void set_loaded_levels(const std::vector<std::string>& short_names)
    {
        // Replace the set, keeping the round each surviving level was first seen at -
        // that timestamp is the whole point (see the block next to g_levels).
        std::unordered_map<std::string, std::uint64_t> next;
        next.reserve(short_names.size());
        for (const std::string& name : short_names)
        {
            if (name.empty() || next.size() >= 4096)
            {
                continue;
            }
            std::string key = lower_ascii(name);
            const auto old = g_levels.find(key);
            next.emplace(std::move(key), old != g_levels.end() ? old->second : g_round);
        }
        g_levels.swap(next);
        g_levels_loaded.store(static_cast<int>(g_levels.size()), std::memory_order_relaxed);
    }

    void drop_caches()
    {
        // HOOK: the highlight's camera manager belonged to the world that just went, and
        // gamestate calls this whenever it drops the pawn - so this is the one place
        // that already means "everything keyed to that world is dead".
        hl::drop_caches();
        slotid::drop_caches();
        g_layouts.clear();
        g_class_spec.clear();
        g_id_cache.clear();
        g_live.clear();
        // Both are keyed to the world that just went: a level name means nothing in the
        // next one, and a half-finished absence streak must not survive a load.
        g_levels.clear();
        std::fill(g_absent_streak_idx.begin(), g_absent_streak_idx.end(), 0);
        std::fill(g_live_of_static.begin(), g_live_of_static.end(), nullptr);
        std::fill(g_level_known.begin(), g_level_known.end(), static_cast<std::uint8_t>(0));
        g_subset_valid = false; // the chapter is re-detected in the next world
        g_levels_loaded.store(0, std::memory_order_relaxed);
        g_world = nullptr;
        g_next_class = 0;
        g_cursor = scan::Cursor{};
        g_round_stats = scan::RoundStats{};
        g_last_slice_us = 0;
        g_round_start_us = 0;
        g_round_open = false;
        g_live_count.store(0, std::memory_order_relaxed);
    }

    //======================================================================================
    // The game-thread pump
    //======================================================================================
    //
    // CALLED FROM EVERY ProcessEvent, not once per 100 ms. gamestate's own 10 Hz gate
    // used to bound this, and that was the reason the sweep could never be cheap: ten
    // pumps a second times a chunk small enough not to stall = a round that takes many
    // seconds. Now gamestate calls it on every pre-callback while the last validated
    // state still stands, and the throttling below - QPC, not GetTickCount64, whose
    // ~15.6 ms granularity is coarser than the slice period - decides the scan rate.
    //
    // Structure per call:
    //   1. a 1 kHz gate, so the thousands-per-second callback costs one QPC read;
    //   2. the config + world-change check;
    //   3. the round gate: a finished round waits its turn (markers_rounds_per_sec);
    //   4. the slice gate: at most one slice per markers_scan_period_ms;
    //   5. one slice of the object-array walk, timed;
    //   6. on wrap: publish the draw buffer and freeze the round's diagnostics.

    void game_thread_pump(std::uint64_t now, const void* world)
    {
        const std::uint64_t now_us = qpc_us();

        // 1. Hard ceiling on how often anything at all happens here. mm::config()
        //    copies the config under a spinlock; at ProcessEvent rate that alone would
        //    be thousands of lock round-trips a second for nothing.
        static std::uint64_t s_gate_us = 0;
        if (!scan::elapsed(now_us, s_gate_us, 1000))
        {
            return;
        }
        s_gate_us = now_us;

        // 2. The generation-cached, per-thread copy: one relaxed atomic load unless
        //    the config actually changed (see mm::cfg_cached).
        const mm::Config& cfg = mm::cfg_cached();
        g_grace_rounds = static_cast<std::uint64_t>(cfg.markers_live_grace_rounds);
        g_absence_on = cfg.markers_absence_marks;
        g_absence_rounds = cfg.markers_absence_rounds;
        g_absence_cats = cfg.markers_absence_categories;

        // ---- HOOK: the x-ray highlight's camera reader (src/highlight.cpp) ----------
        //
        // It needs the game thread and the same validated state this pump already runs
        // on, and `gamestate.cpp` is owned by another workstream this session - so it is
        // driven from here rather than from a second call site of its own. It returns
        // immediately (one atomic load) unless the highlight key is held or the compass
        // is on, and it never touches anything this module owns.
        hl::game_thread_pump(now, now_us, world, cfg);
        // ---- HOOK: the save-slot resolver (src/saveslot.cpp) ------------------------
        //
        // Same reasoning as the highlight hook above: it needs the game thread and the
        // validated state this pump already runs on. It returns after one atomic load
        // once a route has answered, and it stops asking entirely unless the world
        // changes (drop_caches re-arms it).
        slotid::game_thread_pump(now, world);
        // ---- end of hook ------------------------------------------------------------

        if (!cfg.markers_enabled || !cfg.markers_live)
        {
            return;
        }
        if (world != nullptr && g_world != nullptr && world != g_world)
        {
            drop_caches();
        }
        g_world = world;

        drain_inbox();

        const int total = RC::Unreal::FUObjectArray::GetNumElements();
        if (total <= 0)
        {
            legacy_pump(now, cfg);
            return;
        }
        g_scan_fallback.store(false, std::memory_order_relaxed);

        // 3. A round that finished early waits until its slot comes round again -
        //    refreshing the marker set faster than markers_rounds_per_sec buys nothing.
        if (!g_round_open)
        {
            if (!scan::round_due(now_us, g_round_start_us, cfg.markers_rounds_per_sec))
            {
                return;
            }
            g_round_open = true;
            g_round_start_us = now_us;
            g_round_stats = scan::RoundStats{};
        }

        // 4.
        if (!scan::slice_due(now_us, g_last_slice_us, cfg.markers_scan_period_ms))
        {
            return;
        }
        g_last_slice_us = now_us;

        // 5.
        const int chunk = scan::clamp_chunk(cfg.markers_scan_chunk);
        const scan::Slice slice = scan::next_slice(g_cursor, total, chunk);
        const std::uint64_t t0 = qpc_us();
        const int visited = slice.empty() ? 0 : scan_slice(slice);
        const double slice_ms = static_cast<double>(qpc_us() - t0) / 1000.0;

        if (g_pf_scan < 0)
        {
            g_pf_scan = mm::perf_register("marker scan slice", perf::Thread::Game);
        }
        mm::perf_record(g_pf_scan, t0);

        scan::note_slice(g_round_stats, slice_ms, visited);
        g_scan_slice_ms.store(slice_ms, std::memory_order_relaxed);
        g_scan_total.store(total, std::memory_order_relaxed);
        g_scan_chunk.store(chunk, std::memory_order_relaxed);
        if (slice_ms > g_scan_slice_ms_max.load(std::memory_order_relaxed))
        {
            g_scan_slice_ms_max.store(slice_ms, std::memory_order_relaxed);
        }

        // 6.
        if (scan::advance(g_cursor, slice, total))
        {
            publish_round();
            ++g_round;
            g_rounds.store(g_round, std::memory_order_relaxed);
            g_scan_slice_ms_avg.store(g_round_stats.avg_ms(), std::memory_order_relaxed);
            g_scan_slice_ms_peak.store(g_round_stats.peak_ms, std::memory_order_relaxed);
            g_scan_round_ms.store(g_round_stats.total_ms, std::memory_order_relaxed);
            g_scan_round_slices.store(g_round_stats.slices, std::memory_order_relaxed);
            g_scan_round_objects.store(g_round_stats.objects, std::memory_order_relaxed);
            g_round_open = false;
        }
    }
} // namespace markers

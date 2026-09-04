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

#include "atomicfile.hpp"
#include "highlight.hpp"
#include "marker_dedupe.hpp"
#include "mapdata.hpp"
#include "mem.hpp"
#include "mmstate.hpp"
#include "saveslot.hpp"
#include "recon.hpp"
#include "shrines.hpp"
#include "scan_sched.hpp"
#include "spinlock.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

using RC::Unreal::UClass;
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
        // The sweep walks GUObjectArray once per round, in slices of
        // `markers_scan_chunk` slots per pump (src/scan_sched.hpp). Each slot costs a
        // validity check, a class-pointer load and one memoised lookup; the expensive
        // per-object work only runs for slots whose class is in this table.
        //
        // Classification is by NAME up the super chain, memoised per UClass*, so a
        // subclass lands in its parent's category.

        enum class Rule : std::uint8_t
        {
            None,          // no known state flag - never auto-marked
            UsedBool,      // chests / doors / mechanisms: `Used` (SavedStatuKey=statu_use)
            DoorOpenBool,  // BP_NewPuzzlesDoor_C: `DoorOpen`
            PickupDying,   // pickups: `dying` (early) or parked at (0,0,0) (durable)
            ActiveBool,    // fog gates: SavedStatuKey=status_active, so `Active` is the flag
            ControllerPawn, // AI controller: the marker is its possessed Pawn
            Proximity,     // NPC / note: "met" = seen loaded within kMetRadius of the player
            BossPawn       // boss character: defeated when its controller's Health.Current <= 0
        };

        // Rule::Proximity "met" radius, Unreal units (1 uu = 1 cm) = 30 m.
        constexpr double kMetRadius = 3000.0;
        constexpr double kMetRadiusSq = kMetRadius * kMetRadius;

        // Rounds a LIVE-ONLY entry (persist == false: enemies) may go unanswered before
        // it is dropped. Persisted categories use `markers_live_grace_rounds` instead,
        // which debounces a sweep racing level streaming; an enemy has no static twin,
        // so a stale one is a corpse drawn as a threat.
        constexpr std::uint64_t kLiveOnlyGraceRounds = 1;

        struct ClassSpec
        {
            const wchar_t* name;
            mdb::Cat cat;
            Rule rule;
            bool persist; // may be written to wuchang_minimap_found.txt
        };

        constexpr ClassSpec kClasses[] = {
            // Shrine activation flag unknown; the id comes from a CJK-named property.
            {L"BP_RebornFire_C", mdb::Cat::Shrine, Rule::None, true},
            {L"BP_treasurebox_C", mdb::Cat::Chest, Rule::UsedBool, true},
            {L"BP_ItemRedBox_C", mdb::Cat::Chest, Rule::UsedBool, true},
            // Pickups: `dying = false -> true`, then the actor moves to (0,0,0) ~2 s
            // later and lingers until a GC. Absence is not evidence of a collect (an
            // unloaded level looks identical), so it never auto-marks.
            {L"BP_PickupActor_C", mdb::Cat::Pickup, Rule::PickupDying, true},
            // BP_NewPuzzlesDoor_C also carries `GeemID` and `New Fire Point ID`.
            {L"BP_NewPuzzlesDoor_C", mdb::Cat::Door, Rule::DoorOpenBool, true},
            {L"BP_DoorZhong_C", mdb::Cat::Door, Rule::UsedBool, true},
            // Fog gates. Inferred, not observed in the passed state.
            {L"BP_Wumen_C", mdb::Cat::FogGate, Rule::ActiveBool, true},
            {L"BP_LadderV2_C", mdb::Cat::Ladder, Rule::None, false},
            {L"BP_WoodenElevator_C", mdb::Cat::Lift, Rule::None, false},
            // `BP_NPC_C` is the interactable-character base and covers its 78
            // descendants in one entry, since spec_for_class() walks the super chain by
            // name. The five that are not people get exact entries below (an exact match
            // at depth 0 beats the base class).
            //
            // "Found" means MET: seen loaded within kMetRadius of the player. There is
            // no per-NPC saved flag reachable from here. Persisted.
            //
            // `DKDC_NPC_C` is a readable note, not a merchant: every placed instance
            // carries a read-point id, the blueprint has no character mesh, its only
            // interaction string is "Check" and it spawns the hint particle.
            {L"DKDC_NPC_C", mdb::Cat::Note, Rule::Proximity, true},
            {L"BP_NPC_C", mdb::Cat::Npc, Rule::Proximity, true},
            // Not people, despite deriving from BP_NPC_C. Categories match what
            // tools/markers/marker_classes.py puts in the static DB, so the live actor
            // and its static twin never disagree.
            {L"ItemCollectionBox_C", mdb::Cat::Pickup, Rule::PickupDying, true},
            {L"BP_KlesaCleaner_C", mdb::Cat::Other, Rule::None, false},
            {L"BP_PuzzlesDoor_C", mdb::Cat::Door, Rule::UsedBool, true},
            // `BP_PlacedBossAI_C` is the base of all 32 boss blueprints and of all 25
            // placed boss instances.
            //
            // "Found" means DEFEATED: a boss pawn's `Controller` is an
            // `Impl_BaseAIController_C` carrying a `Health` `ExtendedStatComponent_C`
            // with reflected `Current` / `Max` floats; a dead character reads
            // `Current = 0` while its actor is still in the object array. Persisted.
            {L"BP_PlacedBossAI_C", mdb::Cat::Boss, Rule::BossPawn, true},
            // Enemies are LIVE ONLY and never persisted: the count of
            // Impl_BaseAIController_C is the count of live enemies, and the marker is
            // the pawn it possesses.
            {L"Impl_BaseAIController_C", mdb::Cat::Enemy, Rule::ControllerPawn, false},
        };

        constexpr int kClassCount = static_cast<int>(std::size(kClasses));

        // Schema a file in the markers directory must declare to count as a marker
        // manifest. Anything else there is skipped quietly.
        constexpr std::string_view kMarkerSchemaPrefix = "wuchang-minimap-markers";

        // BP_RebornFire_C's game-authored shrine id - the CJK-named "sitting-Buddha
        // point ID" property (U+5750 U+4F5B U+70B9 + "ID"), escaped so the symbol
        // survives any source-encoding accident. The only property that distinguishes
        // sibling shrines, and object names are index-suffixed and unstable.
        constexpr const wchar_t* kShrineIdProp = L"\u5750\u4F5B\u70B9ID";

        //==============================================================================
        // Small helpers
        //==============================================================================

        constexpr unsigned long long kReadCapBytes = 32ull << 20;

        mmfile::ReadInfo read_whole_file_ex(const std::wstring& path, std::string& out)
        {
            return mmfile::read_whole_file(path, out, kReadCapBytes);
        }

        bool read_whole_file(const std::wstring& path, std::string& out)
        {
            const mmfile::ReadInfo info = read_whole_file_ex(path, out);
            if (info.status == mmfile::ReadStatus::Ok)
            {
                return true;
            }
            if (info.too_big)
            {
                mm::logf(L"markers: {} is {} byte(s), over the 32 MB cap - the file was SKIPPED",
                         path,
                         info.size);
            }
            else if (info.status == mmfile::ReadStatus::Failed)
            {
                mm::logf(L"markers: FAILED to read {} (error {}) - the file exists but could not be read, "
                         L"so it was SKIPPED (something else may have it open)",
                         path,
                         info.error);
            }
            return false;
        }

        // Atomic, with one generation of backup for the found tracker (atomicfile.hpp).
        bool write_whole_file(const std::wstring& path, const std::string& data, bool keep_backup, unsigned& err)
        {
            return mmfile::write_whole_file_atomic(path, data, keep_backup, &err);
        }

        std::wstring widen(std::string_view narrow)
        {
            return std::wstring{narrow.begin(), narrow.end()};
        }

        // Names handled here are ASCII (level names, object names, shrine ids).
        // Non-ASCII is replaced with '?' rather than truncated, so one actor never
        // yields two different ids.
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
        // The static database, published to the game thread
        //==============================================================================
        //
        // Immutable once published. A reload builds a fresh one and swaps the pointer;
        // the game thread may be walking the old one and there is no mutex to be had
        // here. The replaced db is RETIRED: the loop thread parks it with the publish
        // round it was replaced in and frees it once the game thread has completed
        // kRetireRounds more rounds AND kRetireMs have passed. publish_round() re-reads
        // `g_db` at the top of every round and rebuilds its index when the pointer
        // differs, so a reader three rounds past the swap cannot hold the old pointer.

        struct StaticDb
        {
            std::vector<mdb::StaticMarker> markers;
            std::unordered_map<std::string, int> by_id;

            // Interning, done once at load, so publish_round() indexes instead of
            // hashing per marker per round:
            //   levels        - the unique lower-cased level short names
            //   marker_level  - per marker, its index into `levels` (-1 = no level)
            std::vector<std::string> levels;
            std::vector<int> marker_level;
        };

        std::atomic<const StaticDb*> g_db{nullptr};

        // Databases replaced by a reload, waiting to be freed. Loop thread only.
        struct RetiredDb
        {
            const StaticDb* db = nullptr;
            std::uint64_t round = 0; // the publish round current when it was replaced
            std::uint64_t ms = 0;    // and when
        };
        std::vector<RetiredDb> g_retired;
        constexpr std::uint64_t kRetireRounds = 3;
        constexpr std::uint64_t kRetireMs = 3000;

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
        // Nothing is shared by pointer, so neither side can free memory the other is
        // reading, and the file is only ever touched from the loop thread.

        spin::Spinlock g_inbox_lock;
        std::vector<std::string> g_inbox;
        bool g_inbox_clear = false;
        std::atomic<bool> g_inbox_pending{false};

        spin::Spinlock g_outbox_lock;
        std::vector<std::string> g_outbox;
        std::atomic<bool> g_outbox_pending{false};

        // render -> loop : a manual found/not-found toggle from the full map's click
        // handler. It can unset as well as set, so it carries the wanted state.
        struct ToggleReq
        {
            std::string id;
            bool found = false;
        };
        spin::Spinlock g_toggle_lock;
        std::vector<ToggleReq> g_toggle;
        std::atomic<bool> g_toggle_pending{false};

        std::unordered_set<std::string> g_found_master; // loop thread only
        std::unordered_set<std::string> g_found_gt;     // game thread only
        bool g_found_dirty = false;                     // loop thread only
        // Marks (auto or manual) added since the found-tracker save line was last
        // printed. The save line reports and clears it.
        std::uint32_t g_marks_since_log = 0;
        std::uint64_t g_found_dirty_ms = 0;
        // The found file exists and could not be read; its contents are unknown, so it
        // must not be written over. Cleared by a retry that succeeds.
        bool g_found_unreadable = false;
        std::uint64_t g_found_retry_ms = 0;
        // A failed save keeps the dirty flag and retries with a doubling backoff on top
        // of the ordinary debounce (0 = no failure outstanding).
        std::uint64_t g_found_backoff_ms = 0;
        std::uint32_t g_found_fail_streak = 0;

        //==============================================================================
        // The shutdown snapshot (POD, staged ahead of time)
        //==============================================================================
        //
        // The flush happens at DLL_PROCESS_DETACH, where the process may be dying
        // abnormally and the heap may be broken - so nothing there may allocate, log or
        // take a lock. The loop thread keeps the exact bytes and path of the pending
        // write in plain static buffers; the detach path is CreateFileW / WriteFile /
        // MoveFileExW over them and nothing else.
        constexpr std::size_t kStageTextMax = 256 * 1024;
        constexpr std::size_t kStagePathMax = 1024;
        char g_stage_text[kStageTextMax];
        std::size_t g_stage_len = 0;
        wchar_t g_stage_path[kStagePathMax];
        std::atomic<bool> g_stage_valid{false};
        bool g_stage_dirty = false; // loop thread: the staged bytes are out of date
        std::atomic<bool> g_flushed_at_exit{false};

        //==============================================================================
        // Stats
        //==============================================================================

        spin::Spinlock g_stats_lock;
        Stats g_stats{};

        std::atomic<int> g_published_count{0};
        std::atomic<int> g_live_count{0};
        std::atomic<std::uint64_t> g_rounds{0};

        // Scan diagnostics. Written by the game thread, read by the loop and render
        // threads; each is a lone scalar, so a relaxed atomic suffices.
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
        // sees the same few hundred actors every second.
        //
        // THE POINTER KEY IS NOT ENOUGH ON ITS OWN. A collected pickup is flagged and
        // parked, not destroyed, and lingers until a GC - so its allocation can be
        // handed to a new actor of the same class, at the same address, mid-level.
        // Every hit therefore re-validates identity two independent ways:
        //   * the object's own NAME, which carries UE's process-unique numeric suffix
        //     (`BP_PickupActor_C_2147479402`) - one GetName() per hit; and
        //   * the GUObjectArray slot the object sat in when the id was taken
        //     (index + serial + class + destruction flags, via uer::alive) - read
        //     through the object array, safe even for a freed object.
        struct IdEntry
        {
            std::string id;
            std::string name; // narrow_ascii(obj->GetName()) at capture time
            uer::ObjRef ref;  // index + serial + class, for uer::alive()
        };
        std::unordered_map<const void*, IdEntry> g_id_cache;
        // Cached ids thrown away because the identity no longer matched, i.e. slot
        // reuse actually happening.
        std::uint32_t g_id_cache_stale = 0;
        // The answer for an actor that cannot be re-validated and so cannot be cached.
        // Game thread only, and deliberately NOT `static thread_local`, which would run
        // the CRT's TLS initialiser on the game thread.
        std::string g_id_uncached;

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
            // Two separate questions, never to be conflated: the position READ succeeded
            // (whatever it returned), and the position is usable, i.e. not the (0,0,0)
            // parking spot. "Found the actor, cannot locate it" is its own answer and is
            // not evidence about game state.
            bool pos_read = false;
            bool pos_valid = false;
            // The health read answered "zero". The entry is KEPT rather than erased: the
            // enemy's authored spawn point is in the static DB too, and erasing the live
            // entry would let the corpse's static twin be drawn at the spawn point. A
            // dead entry suppresses both halves at the publish point.
            bool dead = false;
            // The actor answered a visibility read this round, and what it said. Only
            // filled for the mobile ("walks away") categories - see actor_is_invisible.
            bool invisible_known = false;
            bool invisible = false;
            std::uint64_t round = 0;
            // Display name resolved for a LIVE-ONLY actor (an enemy's dropped loot).
            // Empty means the category word is drawn instead, never the class name.
            std::string label;
        };

        std::unordered_map<std::string, LiveEntry> g_live;

        // markers/items.json, {item id -> display name}. Read once per DB load on the
        // loop thread, then read-only from the game thread.
        std::unordered_map<int, std::string> g_item_names;
        // UObject* -> the item name resolved for it (empty = "asked, and there is none").
        // An actor's item id is set when it is spawned, so the answer cannot change.
        // Dropped with every other world-keyed cache.
        std::unordered_map<const void*, std::string> g_drop_name;
        // UClass* -> the property that reached the item id, so the walk happens once
        // per class.
        std::unordered_map<const void*, int> g_item_prop;

        const void* g_world = nullptr;
        std::uint64_t g_round = 0;

        // The player's position, refreshed once per SLICE from the published snapshot
        // (a seqlock read of a POD struct - no lock, no engine call). `g_player_ok` is
        // false whenever the reader has no validated gameplay pawn, and then nothing is
        // ever marked "met".
        double g_player_x = 0.0;
        double g_player_y = 0.0;
        double g_player_z = 0.0;
        bool g_player_ok = false;

        // Diagnostics that tell "the rule never fired" from "the property is not there".
        std::atomic<int> g_met_marks{0};      // NPC/note markers marked as met
        std::atomic<int> g_dead_dropped{0};   // live enemies dropped because health == 0
        std::atomic<int> g_boss_defeated{0};  // boss markers marked as defeated
        std::atomic<int> g_health_unknown{0}; // characters whose health could not be read
        std::atomic<int> g_shrine_lit_marks{0}; // shrine markers marked from UnlockedFirepoints
        // Static NPC markers hidden this round because the person has moved
        // on (their level is loaded and no live actor answers for the id).
        std::atomic<int> g_mobile_hidden{0};

        // ---- THE PER-ROUND CENSUSES ------------------------------------------------
        //
        // Each of these is a GAUGE - "how many right now" - not a running total. A total
        // of new marks reads 0 in every session after the one that discovered them.
        std::atomic<int> g_shrine_lit_found{0}; // shrines in this chapter that are lit AND found
        std::atomic<int> g_shrine_total{0};     // shrines in this chapter's static DB
        // `g_boss_found` / `g_boss_total`: this chapter's bosses that read as found now,
        // by any rule. `g_boss_from_save`: how many of those came from the save's boss
        // doors rather than a health read this session.
        std::atomic<int> g_boss_found{0};
        std::atomic<int> g_boss_total{0};
        std::atomic<int> g_boss_from_save{0};
        // Bosses whose marker carries no `bossdoor_*` id, so the save can never speak
        // for them (2 of 28 in the shipped manifests).
        std::atomic<int> g_boss_no_door{0};
        std::atomic<int> g_mobile_static{0};      // npc static markers considered
        std::atomic<int> g_mobile_live{0};        // npc live entries held
        std::atomic<int> g_mobile_joined{0};      // static markers whose live twin answered
        std::atomic<int> g_mobile_superseded{0};  // ... and stands more than kMovedUu away
        std::atomic<int> g_mobile_walked{0};      // a live twin answered but is unlocatable
        // Live twins standing at their authored spot that the game has made INVISIBLE,
        // over how many could be asked at all. `invisible 0 of 0 asked` is a dead rule;
        // `0 of 21` is a live rule saying the flags are false.
        std::atomic<int> g_mobile_invisible{0};
        std::atomic<int> g_mobile_vis_known{0};
        std::atomic<int> g_mobile_hidden_invis{0};  // ... and was hidden for it
        std::atomic<int> g_mobile_hidden_walked{0}; // ... and was hidden for it
        std::atomic<int> g_mobile_hidden_absent{0}; // hidden because nobody answered at all
        std::atomic<int> g_mobile_level_known{0}; // ... whose own level is resident
        // The "met" gauge: people and notes in this chapter that are in the found set,
        // over how many there are.
        std::atomic<int> g_met_found{0};
        std::atomic<int> g_met_total{0};
        std::atomic<int> g_dead_hidden{0};        // markers suppressed because they are dead

        // How far a live person must stand from their authored position before the
        // census calls the static hint superseded. Unreal units; 300 uu = 3 m.
        constexpr double kMovedUu = 300.0;
        constexpr double kMovedUuSq = kMovedUu * kMovedUu;

        //==============================================================================
        // Live copies of the sweep's caps (game thread)
        //==============================================================================
        //
        // Unpacked from the Config the pump already copies: process_marker() /
        // publish_round() run per actor and must not each take the config spinlock.
        std::uint64_t g_grace_rounds = 2;
        bool g_absence_on = true;
        int g_absence_rounds = 2;
        std::uint32_t g_absence_cats =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);
        std::size_t g_live_max = 8192;
        std::size_t g_id_cache_max = 8192;
        std::size_t g_class_cache_max = 262144;
        // Gates the "route on <class> is <property>" diagnostics to once per session.
        // Keyed by route plus class NAME - the UClass*-keyed route caches are dropped on
        // every level transition, so they cannot gate it. Never cleared.
        std::unordered_set<std::wstring> g_route_logged;

        bool first_time(const wchar_t* route, const std::wstring& cls)
        {
            return g_route_logged.insert(std::wstring{route} + L'\t' + cls).second;
        }
        std::size_t g_fallback_max_per_class = 4096;

        //==============================================================================
        // Absence as evidence of a collect (game thread)
        //==============================================================================
        //
        // `g_levels` maps a loaded level's short name (lower-cased; the marker DB and
        // UObject::GetFullName() need not agree on case) to the sweep round at which it
        // was FIRST seen loaded. A marker may only be auto-marked once a full round has
        // completed after that, so "not seen" cannot mean "its level had not finished
        // streaming when I looked".
        std::unordered_map<std::string, std::uint64_t> g_levels;
        std::atomic<int> g_absence_marks{0};
        std::atomic<int> g_levels_loaded{0};

        //==============================================================================
        // The per-round index over the static DB (game thread)
        //==============================================================================
        //
        // Sized to `g_idx_db->markers` (or to its `levels`) and rebuilt whenever the
        // database pointer changes, so a publish is array indexing, not string hashing.
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

        // Both sides of the level-name join must lower-case identically; one definition,
        // in the tested pure layer.
        using mdb::lower_ascii;

        // The chunked object-array walk (game thread only).
        scan::Cursor g_cursor{};
        scan::RoundStats g_round_stats{};
        std::uint64_t g_last_slice_us = 0;
        std::uint64_t g_round_start_us = 0;
        bool g_round_open = false; // false = waiting for the next round's turn

        // Fallback path only: one FindAllOf per pump, cycling the class table. Used when
        // FUObjectArray::GetNumElements() cannot answer (0 or negative), i.e. UE4SS has
        // not resolved GUObjectArray.
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

        // A reflected bool, bitfield-aware: `uer::read_bool_prop` asks `FBoolProperty`
        // for the bit, which a native bitfield needs (`uint8 bHidden : 1` on AActor
        // shares its byte with `bNetTemporary`, `bTearOff` and others). The plain byte
        // test is the fallback for a property whose bool info does not validate; a
        // blueprint-authored bool gets its own byte with mask 0x01.
        bool read_bool_prop(UObject* obj, const wchar_t* name, bool& out)
        {
            const uer::ClassLayout* layout = g_layouts.get(obj);
            if (uer::read_bool_prop(layout, obj, name, out))
            {
                return true;
            }
            std::uint8_t b = 0;
            if (!uer::read_prop(layout, obj, name, b, 1))
            {
                return false;
            }
            out = (b != 0);
            return true;
        }

        //==============================================================================
        // IS THIS PERSON ACTUALLY THERE? (game thread)
        //==============================================================================
        //
        // The game does not park or destroy a used-up NPC (the way it parks a collected
        // pickup at the origin): the actor stays where it was placed and is made
        // INVISIBLE, and the person you meet later is a different placed actor in
        // another sublevel. Bosses use the same mechanism - `ST_LevelScriptBossData`
        // carries a `隐藏击败过的尸体` ("hide the corpse of a defeated boss") flag.
        //
        // THE ROUTES, best first, all raw reads:
        //   1. `bHidden` - AActor's own flag, what `SetActorHiddenInGame` writes. A
        //      native bitfield, hence the masked bool read above.
        //   2. `bLocalHidden` - the game's own addition, beside `DCSHiddenStateTypes`.
        //   3. the root component's `bHiddenInGame`, then `bVisible` (inverted) - if the
        //      game hides the mesh rather than the actor.
        //
        // DELIBERATELY NOT `bPerformanceHidden`: it is the LOD / distance hide, set on
        // almost every actor, so believing it would delete the marker for every NPC who
        // is merely far away. Its value is logged in the route line.
        //
        // The winning route is cached per UClass* and logged once.

        // Defined below, next to the health diagnostic that first needed it.
        std::wstring safe_class_name(UObject* obj);

        constexpr const wchar_t* kActorHiddenProps[] = {L"bHidden", L"bLocalHidden"};
        constexpr int kActorHiddenCount = static_cast<int>(std::size(kActorHiddenProps));
        constexpr const wchar_t* kRootComponentProp = L"RootComponent";
        constexpr const wchar_t* kCompHiddenProp = L"bHiddenInGame";
        constexpr const wchar_t* kCompVisibleProp = L"bVisible";

        // UClass* -> which route answered:
        //   0..kActorHiddenCount-1  an actor flag, by index into kActorHiddenProps
        //   -2                      the root component's bHiddenInGame
        //   -3                      the root component's bVisible, inverted
        //   -1                      nothing on this class answers
        std::unordered_map<const void*, int> g_hidden_route;

        constexpr int kHiddenRouteNone = -1;
        constexpr int kHiddenRouteCompHidden = -2;
        constexpr int kHiddenRouteCompVisible = -3;

        // Is this actor invisible in the game right now? `answered` says whether any
        // route could be read at all; "could not read" is its own answer and never
        // collapses into a state.
        bool actor_is_invisible(UObject* actor, bool& answered)
        {
            answered = false;
            if (actor == nullptr)
            {
                return false;
            }
            UClass* cls = actor->GetClassPrivate();
            const uer::ClassLayout* layout = g_layouts.get(actor);
            bool value = false;

            const auto try_route = [&](int route) -> bool {
                if (route >= 0 && route < kActorHiddenCount)
                {
                    return uer::read_bool_prop(layout, actor, kActorHiddenProps[route], value);
                }
                if (route == kHiddenRouteCompHidden || route == kHiddenRouteCompVisible)
                {
                    UObject* root = uer::read_object_prop(layout, actor, kRootComponentProp);
                    if (root == nullptr)
                    {
                        return false;
                    }
                    const uer::ClassLayout* rl = g_layouts.get(root);
                    if (route == kHiddenRouteCompHidden)
                    {
                        return uer::read_bool_prop(rl, root, kCompHiddenProp, value);
                    }
                    bool visible = true;
                    if (!uer::read_bool_prop(rl, root, kCompVisibleProp, visible))
                    {
                        return false;
                    }
                    value = !visible;
                    return true;
                }
                return false;
            };

            const auto cached = cls != nullptr ? g_hidden_route.find(cls) : g_hidden_route.end();
            if (cached != g_hidden_route.end())
            {
                if (cached->second == kHiddenRouteNone || !try_route(cached->second))
                {
                    return false;
                }
                answered = true;
                return value;
            }

            static const int kRoutes[] = {0, 1, kHiddenRouteCompHidden, kHiddenRouteCompVisible};
            int winner = kHiddenRouteNone;
            for (const int r : kRoutes)
            {
                if (r < kActorHiddenCount && try_route(r))
                {
                    winner = r;
                    break;
                }
                if (r < 0 && try_route(r))
                {
                    winner = r;
                    break;
                }
            }
            if (cls != nullptr)
            {
                if (g_hidden_route.size() > g_class_cache_max)
                {
                    g_hidden_route.clear();
                }
                g_hidden_route.emplace(cls, winner);
                const wchar_t* name = winner >= 0 && winner < kActorHiddenCount
                                          ? kActorHiddenProps[winner]
                                          : (winner == kHiddenRouteCompHidden
                                                 ? L"RootComponent -> bHiddenInGame"
                                                 : (winner == kHiddenRouteCompVisible
                                                        ? L"RootComponent -> !bVisible"
                                                        : L"(none)"));
                // `bPerformanceHidden` is reported and NOT believed - see the note above.
                if (first_time(L"vis", safe_class_name(actor)))
                {
                    bool perf = false;
                    const bool perf_ok =
                        uer::read_bool_prop(layout, actor, L"bPerformanceHidden", perf);
                    mm::logf(L"markers: visibility route on '{}' is {} (reads {}; "
                             L"bPerformanceHidden {})",
                             safe_class_name(actor), name,
                             winner == kHiddenRouteNone ? L"nothing" : (value ? L"hidden" : L"visible"),
                             perf_ok ? (perf ? L"true" : L"false") : L"unreadable");
                }
            }
            if (winner == kHiddenRouteNone)
            {
                return false;
            }
            answered = true;
            return value;
        }

        // Is this character dead? (game thread)
        //
        // Wuchang keeps a character's health in an `ExtendedStatComponent_C` sub-object
        // on its AI CONTROLLER, not on the pawn. A component sub-object is a reflected
        // object property, so the read is `controller -> <health prop> -> CurrentValue`:
        // three cached-offset raw reads, no ProcessEvent. A dead character reads 0 while
        // its actor is still in the object array.
        //
        // The component is DISCOVERED rather than named: try the obvious `Health`
        // property, else walk the class' pointer-sized properties once, capture each
        // value through GUObjectArray (SEH-guarded, proves the target is a live UObject)
        // and accept the one whose class is an `ExtendedStatComponent`, preferring the
        // one named `Health`. The winning property name is cached per UClass*.
        //
        // The reflected floats are `CurrentValue` / `MaxValue`, and in UE5 a blueprint
        // "float" is an 8-byte `FDoubleProperty` (Large World Coordinates) - hence
        // `uer::read_numeric_prop`, which accepts either width.
        //
        // Returns FALSE when the answer is unknown (no such property, an unreadable
        // component, a nonsensical Max) - never "dead", which would hide living enemies.

        constexpr const wchar_t* kHealthProp = L"Health";
        constexpr const wchar_t* kStatComponentSubstr = L"ExtendedStatComponent";

        // The candidate spellings of the component's two reflected floats, best first.
        struct HealthFields
        {
            const wchar_t* current;
            const wchar_t* max;
        };
        constexpr HealthFields kHealthFields[] = {
            {L"CurrentValue", L"MaxValue"},
            {L"Current", L"Max"},
        };
        constexpr int kHealthFieldCount = static_cast<int>(std::size(kHealthFields));

        // UClass* -> the property name that reaches its health component; an empty
        // string means "walked, and there is none". Dropped in drop_caches().
        std::unordered_map<const void*, std::wstring> g_health_prop;
        // Component UClass* -> index into kHealthFields, or -1 for "no pair reads back".
        std::unordered_map<const void*, int> g_health_fields;
        // The property width that answered, purely for the one log line that says so.
        int g_health_width = 0;
        bool g_health_diag_done = false; // the one-shot failure diagnostic has been printed

        // The object's class name, or an empty string. Safe on a captured object only.
        std::wstring safe_class_name(UObject* obj)
        {
            uer::ObjRef ref{};
            if (!uer::capture(obj, ref))
            {
                return {};
            }
            return uer::class_name(ref);
        }

        // ONE-SHOT DIAGNOSTIC, printed the first time a health read fails: which object,
        // class and stage, plus a table of the class' candidate properties.
        void log_health_failure(UObject* owner, const uer::ClassLayout* layout, const wchar_t* stage,
                                bool list_numbers = false)
        {
            if (g_health_diag_done)
            {
                return;
            }
            g_health_diag_done = true;
            const std::wstring cls = safe_class_name(owner);
            mm::logf(L"markers: HEALTH READ FAILED on class '{}' at stage '{}' - listing the {} "
                     L"properties of that class so the route can be fixed:",
                     cls.empty() ? std::wstring{L"<unknown>"} : cls,
                     stage,
                     list_numbers ? L"numeric (4- AND 8-byte)" : L"object-valued");
            if (layout == nullptr)
            {
                mm::log(L"markers:   (the class has no readable property layout at all)");
                return;
            }
            // The number table: every 4- and 8-byte property with the value it reads
            // back and its width. An 8-byte entry may be a POINTER read as a double
            // (~1e-317 or absurdly large).
            if (list_numbers)
            {
                int listed = 0;
                for (const auto& kv : layout->props)
                {
                    if (!uer::prop_is_numeric_width(kv.second) || listed >= 96)
                    {
                        continue;
                    }
                    double v = 0.0;
                    int width = 0;
                    if (!uer::read_numeric_prop(layout, owner, kv.first.c_str(), v, &width))
                    {
                        continue;
                    }
                    ++listed;
                    mm::logf(L"markers:   {} = {} ({}-byte)", kv.first, v, width);
                }
                if (listed == 0)
                {
                    mm::log(L"markers:   (that class has no readable 4- or 8-byte property)");
                }
                return;
            }
            int listed = 0;
            for (const auto& kv : layout->props)
            {
                if (kv.second.size != static_cast<int>(sizeof(void*)) || listed >= 64)
                {
                    continue;
                }
                UObject* value = uer::read_object_prop(layout, owner, kv.first.c_str());
                uer::ObjRef ref{};
                if (value == nullptr || !uer::capture(value, ref))
                {
                    continue;
                }
                ++listed;
                mm::logf(L"markers:   {} -> {} (class {})",
                         kv.first,
                         ref.obj->GetName(),
                         uer::class_name(ref));
            }
            if (listed == 0)
            {
                mm::log(L"markers:   (no property on that class holds a live UObject)");
            }
        }

        // The health component of `owner`, discovered once per class. nullptr = there
        // is none reachable, and the caller must then answer "unknown".
        UObject* health_component(UObject* owner, const uer::ClassLayout* layout)
        {
            if (owner == nullptr || layout == nullptr)
            {
                return nullptr;
            }
            UClass* cls = owner->GetClassPrivate();
            if (cls == nullptr)
            {
                return nullptr;
            }
            const auto cached = g_health_prop.find(cls);
            if (cached != g_health_prop.end())
            {
                if (cached->second.empty())
                {
                    return nullptr;
                }
                return uer::read_object_prop(layout, owner, cached->second.c_str());
            }

            // 1. The obvious name, accepted only if the target really is a stat
            //    component; a same-named property of another type must not pin the
            //    route for the session.
            std::wstring winner;
            UObject* found = nullptr;
            UObject* direct = uer::read_object_prop(layout, owner, kHealthProp);
            if (direct != nullptr && safe_class_name(direct).find(kStatComponentSubstr) != std::wstring::npos)
            {
                winner = kHealthProp;
                found = direct;
            }

            // 2. Otherwise walk the class' pointer-sized properties once. A component
            //    named `Health` wins outright; any other stat component is kept as a
            //    fallback so a build that names it differently still answers.
            if (found == nullptr)
            {
                for (const auto& kv : layout->props)
                {
                    if (kv.second.size != static_cast<int>(sizeof(void*)))
                    {
                        continue;
                    }
                    UObject* value = uer::read_object_prop(layout, owner, kv.first.c_str());
                    uer::ObjRef ref{};
                    if (value == nullptr || !uer::capture(value, ref))
                    {
                        continue;
                    }
                    if (uer::class_name(ref).find(kStatComponentSubstr) == std::wstring::npos)
                    {
                        continue;
                    }
                    const bool is_health = ref.obj->GetName() == kHealthProp;
                    if (is_health || found == nullptr)
                    {
                        winner = kv.first;
                        found = value;
                    }
                    if (is_health)
                    {
                        break;
                    }
                }
            }

            if (g_health_prop.size() > g_class_cache_max)
            {
                g_health_prop.clear();
            }
            g_health_prop.emplace(cls, winner);
            if (found != nullptr && first_time(L"healthcomp", safe_class_name(owner)))
            {
                uer::ObjRef fref{};
                (void)uer::capture(found, fref);
                mm::logf(L"markers: health component route on '{}' is property '{}' -> {} (class {})",
                         safe_class_name(owner),
                         winner,
                         fref.obj != nullptr ? fref.obj->GetName() : std::wstring{L"?"},
                         safe_class_name(found));
            }
            return found;
        }

        // The component's current / max health, whichever pair of names this build
        // spells them with. The winning pair is cached per component class.
        bool read_health_pair(UObject* health, const uer::ClassLayout* hl, double& current, double& max)
        {
            if (health == nullptr || hl == nullptr)
            {
                return false;
            }
            const void* cls = health->GetClassPrivate();
            const auto cached = cls != nullptr ? g_health_fields.find(cls) : g_health_fields.end();
            if (cached != g_health_fields.end())
            {
                if (cached->second < 0)
                {
                    return false;
                }
                const HealthFields& f = kHealthFields[cached->second];
                return uer::read_numeric_prop(hl, health, f.current, current) &&
                       uer::read_numeric_prop(hl, health, f.max, max);
            }
            int winner = -1;
            int width = 0;
            for (int i = 0; i < kHealthFieldCount; ++i)
            {
                const HealthFields& f = kHealthFields[i];
                if (uer::read_numeric_prop(hl, health, f.current, current, &width) &&
                    uer::read_numeric_prop(hl, health, f.max, max))
                {
                    winner = i;
                    break;
                }
            }
            if (cls != nullptr)
            {
                if (g_health_fields.size() > g_class_cache_max)
                {
                    g_health_fields.clear();
                }
                g_health_fields.emplace(cls, winner);
                if (winner >= 0)
                {
                    g_health_width = width;
                    if (first_time(L"healthfields", safe_class_name(health)))
                    {
                        mm::logf(L"markers: health fields on '{}' are '{}' / '{}', {} bytes wide "
                                 L"(read {} / {})",
                                 safe_class_name(health),
                                 kHealthFields[winner].current,
                                 kHealthFields[winner].max,
                                 width,
                                 current,
                                 max);
                    }
                }
            }
            return winner >= 0;
        }

        bool controller_says_dead(UObject* controller, bool& answered)
        {
            answered = false;
            if (controller == nullptr)
            {
                return false;
            }
            const uer::ClassLayout* layout = g_layouts.get(controller);
            UObject* health = health_component(controller, layout);
            if (health == nullptr || !mem::readable(health, 0x40))
            {
                log_health_failure(controller, layout, L"no ExtendedStatComponent reachable from this class");
                return false;
            }
            const uer::ClassLayout* hl = g_layouts.get(health);
            double current = 0.0;
            double max = 0.0;
            const bool read_ok = read_health_pair(health, hl, current, max);
            const mdb::Health answer = mdb::health_answer(read_ok, current, max);
            if (answer == mdb::Health::Unknown)
            {
                log_health_failure(health,
                                   hl,
                                   read_ok ? L"'CurrentValue' / 'MaxValue' read back as nonsense (or "
                                             L"Max <= 0)"
                                           : L"the component has no numeric 'CurrentValue' / 'MaxValue' "
                                             L"(nor 'Current' / 'Max') at 8 or 4 bytes",
                                   !read_ok);
                return false;
            }
            answered = true;
            return answer == mdb::Health::Dead;
        }

        //==============================================================================
        // The item name of a runtime-spawned pickup (game thread)
        //==============================================================================
        //
        // A `BP_DropItem_C` is spawned while you play, so it has no entry in
        // `markers/chapter*.json` and no name of its own.
        //
        // `BP_PickupActor_C` and its descendants carry an inline `Items` array of
        // `int32 ID, int32 Amount` pairs, ids in the 10000..40000 band, `ids[0]` of the
        // first array being the item. At runtime it is a reflected `TArray` UPROPERTY:
        // a 16-byte header read plus one int32.
        //
        // Neither the element stride nor the position of `ID` inside the element can be
        // known offline (the element is a blueprint struct), so an answer is accepted
        // only when it is an id `markers/items.json` knows.
        //
        // Candidate arrays; the first whose leading int32 is a known item id wins, and
        // the winning property is cached per class.
        constexpr const wchar_t* kItemArrayProps[] = {
            L"Items",                    // what BP_PickupActor_C writes (1044/1046)
            L"首次拾取道具内容", // "first pickup contents"
            L"ItemResult",
            L"CustomedItems",
        };
        constexpr int kItemArrayCount = static_cast<int>(std::size(kItemArrayProps));

        // The byte offsets inside the first element at which `ID` may sit. 0 is the
        // serialized order; 4 covers a struct that leads with the amount.
        constexpr int kItemIdOffsets[] = {0, 4};

        bool lookup_item_name(int id, std::string& out)
        {
            const auto it = g_item_names.find(id);
            if (it == g_item_names.end())
            {
                return false;
            }
            out = it->second;
            return true;
        }

        // Reads `prop` as a TArray header and tries to pull a KNOWN item id out of its
        // first element. Returns the name.
        bool item_name_from_array(const uer::ClassLayout* layout, UObject* actor, const wchar_t* prop,
                                  std::string& out)
        {
            struct TArrayRaw
            {
                void* data = nullptr;
                std::int32_t num = 0;
                std::int32_t max = 0;
            };
            TArrayRaw arr{};
            if (!uer::read_prop(layout, actor, prop, arr, static_cast<int>(sizeof(TArrayRaw))))
            {
                return false;
            }
            if (arr.num <= 0 || arr.num > 256 || arr.max < arr.num || !mem::plausible_ptr(arr.data) ||
                !mem::readable(arr.data, 64))
            {
                return false;
            }
            for (const int off : kItemIdOffsets)
            {
                std::int32_t id = 0;
                if (!mem::read_at(arr.data, static_cast<std::size_t>(off), id))
                {
                    continue;
                }
                if (id > 0 && lookup_item_name(id, out))
                {
                    return true;
                }
            }
            return false;
        }

        // The display name of a pickup-family actor, or an empty string. Memoised per
        // actor and, for the property route, per class.
        const std::string& resolve_item_name(UObject* actor)
        {
            static const std::string kNone{};
            if (actor == nullptr || g_item_names.empty())
            {
                return kNone;
            }
            const auto cached = g_drop_name.find(actor);
            if (cached != g_drop_name.end())
            {
                return cached->second;
            }
            const uer::ClassLayout* layout = g_layouts.get(actor);
            UClass* cls = actor->GetClassPrivate();
            std::string name;
            int winner = -1;
            const auto known = cls != nullptr ? g_item_prop.find(cls) : g_item_prop.end();
            if (known != g_item_prop.end())
            {
                if (known->second >= 0 &&
                    item_name_from_array(layout, actor, kItemArrayProps[known->second], name))
                {
                    winner = known->second;
                }
            }
            else
            {
                for (int i = 0; i < kItemArrayCount; ++i)
                {
                    if (item_name_from_array(layout, actor, kItemArrayProps[i], name))
                    {
                        winner = i;
                        break;
                    }
                }
                if (cls != nullptr)
                {
                    if (g_item_prop.size() > g_class_cache_max)
                    {
                        g_item_prop.clear();
                    }
                    g_item_prop.emplace(cls, winner);
                    if (first_time(L"itemname", safe_class_name(actor)))
                    {
                        mm::logf(L"markers: item-name route on '{}' is {} (first resolved name '{}')",
                                 safe_class_name(actor),
                                 winner >= 0 ? std::wstring{kItemArrayProps[winner]}
                                             : std::wstring{L"(none - no array holds a known item id)"},
                                 widen(name));
                    }
                }
            }
            if (g_drop_name.size() > g_id_cache_max)
            {
                g_drop_name.clear();
            }
            return g_drop_name.emplace(actor, winner >= 0 ? std::move(name) : std::string{}).first->second;
        }

        // Is this shrine marker's id in the save's UnlockedFirepoints list?
        //
        // The offline extractor de-duplicates a shrine id the game itself reuses by
        // suffixing `@<level>/<obj>`, so the game's id is everything up to the '@'.
        // shr::is_unlocked() is case-insensitive: the save spells ids as the designers
        // typed them (`Task1` next to `digong01`).
        bool shrine_is_lit(const std::string& marker_id)
        {
            const std::size_t at = marker_id.find('@');
            if (at == std::string::npos)
            {
                return shr::is_unlocked(marker_id.c_str());
            }
            char buf[shr::kIdLen]{};
            const std::size_t n = at < sizeof(buf) - 1 ? at : sizeof(buf) - 1;
            std::memcpy(buf, marker_id.data(), n);
            buf[n] = '\0';
            return shr::is_unlocked(buf);
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

        // AActor has no reflected transform; the world transform of a world-placed actor
        // is its root component's RelativeLocation (no attach parent, so relative IS
        // world). Two guarded reads per actor, and no ProcessEvent.
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
            // The cap must clear the WHOLE game's class count, not the marker classes':
            // the chunked walk asks about every class that owns an object, and a cap hit
            // mid-round throws away the negative answers that make the walk cheap.
            if (g_class_spec.size() > g_class_cache_max)
            {
                g_class_spec.clear();
            }
            g_class_spec.emplace(cls, found);
            return found;
        }

        const std::string& id_for(UObject* obj)
        {
            const std::string name = narrow_ascii(obj->GetName());
            const auto it = g_id_cache.find(obj);
            if (it != g_id_cache.end())
            {
                if (it->second.name == name && uer::alive(it->second.ref))
                {
                    return it->second.id;
                }
                // Same address, different object: the allocation was recycled. Drop the
                // entry and derive the id again from what is there NOW.
                ++g_id_cache_stale;
                MM_LOGV(L"markers: the id cache entry for a recycled actor slot was dropped "
                        L"('{}' is now '{}'; {} so far this session)",
                        widen(it->second.name),
                        widen(name),
                        g_id_cache_stale);
                g_id_cache.erase(it);
            }
            const std::string full = narrow_ascii(obj->GetFullName());
            const std::string level = mdb::level_from_full_name(full);
            std::string id = mdb::stable_id(level, name);
            uer::ObjRef ref{};
            if (!uer::capture(obj, ref))
            {
                // Not capturable (a CDO, an archetype, or already being destroyed), so
                // it cannot be re-validated later. Answer, but do not cache.
                g_id_uncached = std::move(id);
                return g_id_uncached;
            }
            if (g_id_cache.size() > g_id_cache_max)
            {
                g_id_cache.clear();
            }
            return g_id_cache.emplace(obj, IdEntry{std::move(id), name, ref}).first->second.id;
        }

        void note_found(const std::string& id)
        {
            if (id.empty() || g_found_gt.contains(id))
            {
                return;
            }
            g_found_gt.insert(id);
            // Keep the interned found flags in step: one hash lookup per find, instead
            // of one per marker per round.
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
                spin::SpinGuard guard(g_outbox_lock);
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
                spin::SpinGuard guard(g_inbox_lock);
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
        // location read, the state-flag read, the FullName-derived id - lives here and
        // only runs for an object whose class IS in kClasses.
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
                // A boss is possessed by an ordinary Impl_BaseAIController_C too, so the
                // same pawn would otherwise land in g_live twice per round - as
                // Cat::Enemy via the controller and as Cat::Boss via its own class.
                // Whenever the pawn is itself a marker class, its own entry wins.
                if (spec_for_class(actor) >= 0)
                {
                    return;
                }
                // DEAD ENEMIES MUST NOT BE DRAWN. The corpse keeps its controller and
                // its position until a GC, so absence from the sweep is not the signal -
                // health is.
                bool answered = false;
                bool dead = controller_says_dead(obj, answered);
                if (!answered)
                {
                    // The pawn is asked too, so a build that hangs the stat component off
                    // the character still answers.
                    dead = controller_says_dead(actor, answered);
                }
                if (answered && dead)
                {
                    // MARKED DEAD, NOT ERASED (see LiveEntry::dead): an enemy's spawn
                    // point is a static marker, so erasing the live entry would bring
                    // the static one back. The entry is refreshed while the corpse is in
                    // the object array and ages out once a GC takes it.
                    const std::string& dead_id = id_for(actor);
                    const auto it = g_live.find(dead_id);
                    if (it != g_live.end())
                    {
                        if (!it->second.dead)
                        {
                            g_dead_dropped.fetch_add(1, std::memory_order_relaxed);
                        }
                        it->second.dead = true;
                        it->second.pos_valid = false; // never drawn, wherever it fell
                        it->second.round = g_round;
                    }
                    else if (g_live.size() < g_live_max)
                    {
                        LiveEntry d{};
                        d.cls = s.name;
                        d.cat = s.cat;
                        d.persist = s.persist;
                        d.dead = true;
                        d.round = g_round;
                        g_live.emplace(dead_id, d);
                        g_dead_dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                if (!answered)
                {
                    g_health_unknown.fetch_add(1, std::memory_order_relaxed);
                }
            }

            LiveEntry e{};
            e.cat = s.cat;
            e.cls = s.name;
            e.persist = s.persist;
            e.round = g_round;
            if (actor_location(actor, e.x, e.y, e.z))
            {
                // (0,0,0) is not a position: it is where the level saver parks a
                // collected pickup. Keep the entry, never draw it there.
                e.pos_read = true;
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
                // PARKED AT THE ORIGIN = COLLECTED, but only when the read actually
                // ANSWERED (0,0,0). The mark is persisted, so a failed read must never
                // reach here.
                if (e.pos_read && !e.pos_valid)
                {
                    e.found = true;
                }
                // The item's own name, for loot an enemy dropped, which has no entry in
                // the static DB. Memoised per actor.
                e.label = resolve_item_name(actor);
                break;
            }
            case Rule::Proximity:
            {
                // A used-up NPC keeps its actor, position and id and is simply made
                // INVISIBLE (actor_is_invisible), so the visibility read comes first: it
                // hides the marker at the publish point (case (e) of
                // mdb::mobile_twin_is_stale) and stops `met` firing for somebody you
                // cannot walk up to.
                //
                // MOBILE categories only, i.e. `npc`. A note (`DKDC_NPC_C`) is a thing on
                // a wall whose blueprint references no character mesh, so its visibility
                // flags are not evidence about a person.
                if (mdb::is_mobile_category(e.cat))
                {
                    bool answered = false;
                    const bool hidden = actor_is_invisible(actor, answered);
                    e.invisible_known = answered;
                    e.invisible = answered && hidden;
                }
                if (g_player_ok && e.pos_valid && !e.invisible)
                {
                    const double dx = e.x - g_player_x;
                    const double dy = e.y - g_player_y;
                    const double dz = e.z - g_player_z;
                    if (dx * dx + dy * dy + dz * dz <= kMetRadiusSq)
                    {
                        e.found = true;
                    }
                }
                break;
            }
            case Rule::BossPawn:
            {
                // Defeated = zero health. The stat component sits on the AI CONTROLLER,
                // so `APawn::Controller` is the first hop; the pawn is asked too, since
                // a dead boss may already be unpossessed.
                const uer::ClassLayout* layout = g_layouts.get(actor);
                UObject* controller = uer::read_object_prop(layout, actor, L"Controller");
                bool answered = false;
                bool dead = controller_says_dead(controller, answered);
                if (!answered)
                {
                    dead = controller_says_dead(actor, answered);
                }
                if (answered && dead)
                {
                    e.found = true;
                }
                else if (!answered)
                {
                    g_health_unknown.fetch_add(1, std::memory_order_relaxed);
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
            // Enemies are NOT namespaced: the static DB holds an enemy's spawn point
            // under the same <level>/<object name> id, so a prefix would draw the spawn
            // point and the live pawn as two markers. `persist == false`, not the id,
            // keeps enemies out of the collection tracker.

            if (e.found && e.persist)
            {
                const bool fresh = !g_found_gt.contains(id);
                note_found(id);
                if (fresh)
                {
                    if (s.rule == Rule::Proximity)
                    {
                        g_met_marks.fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (s.rule == Rule::BossPawn)
                    {
                        g_boss_defeated.fetch_add(1, std::memory_order_relaxed);
                        mm::logf(L"markers: boss defeated - {} marked as found",
                                 std::wstring(id.begin(), id.end()));
                    }
                }
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
        //     memoised per UClass*, so the super-chain name walk (wstring compares) runs
        //     once per class per level, not once per object per round.
        //   * IsValidObjectForFindXOf()   - CDOs and archetypes.
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
        // Only reached when FUObjectArray::GetNumElements() cannot answer. Each pump
        // costs a whole-array walk per class, so this is far slower than the chunked
        // path; it exists so a UE4SS build that cannot resolve GUObjectArray still draws
        // markers.
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
        // everything". Recomputed on EVERY publish, not latched: a chapter change must
        // switch the visible set on the next round.
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

        // Size every per-marker array to the loaded database. Called from publish_round
        // when the pointer differs from the one the arrays describe.
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

        // The chapter-filtered subrange. The static DB holds all six chapters in one flat
        // set, so the filter is applied once per chapter change, not per marker per round.
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

        // Perf counters (perf.hpp). Namespace-scope ints rather than function statics: a
        // guarded static's first call runs the CRT's thread-safe-init path, which must
        // not happen on the game thread.
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
                const std::uint64_t grace = it->second.persist ? g_grace_rounds : kLiveOnlyGraceRounds;
                if (g_round >= grace && it->second.round + grace <= g_round)
                {
                    it = g_live.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            std::vector<DrawMarker>& dst = g_slot[g_slot_next];
            dst.clear(); // keeps the capacity

            // Chapters' world bounds overlap (chapter 4 covers nearly all of chapter 1),
            // so an unfiltered publish paints foreign markers over the current map. This
            // is the single point where that is decided: the minimap, the full map, the
            // compass pips and the x-ray all read the buffer published here.
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
                // actor instead of one per STATIC marker.
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

                // The loaded-level state, resolved once per UNIQUE level name rather than
                // once per marker; the names are interned lower-cased at load.
                for (std::size_t i = 0; i < g_level_known.size(); ++i)
                {
                    const auto lit = g_levels.find(db->levels[i]);
                    g_level_known[i] = lit != g_levels.end() ? 1 : 0;
                    g_level_round[i] = lit != g_levels.end() ? lit->second : 0;
                }

                g_mobile_hidden.store(0, std::memory_order_relaxed);
                // The per-round censuses, all gauges.
                int lit_found = 0;
                int lit_total = 0;
                int mobile_static = 0;
                int mobile_joined = 0;
                int mobile_superseded = 0;
                int mobile_walked = 0;         // a live twin answered but is unlocatable
                int mobile_invisible = 0;      // a live twin is standing there, invisible
                int mobile_vis_known = 0;      // ... twins whose visibility could be READ
                int mobile_hidden_walked = 0;  // ... and was therefore hidden
                int mobile_hidden_absent = 0;  // hidden because nobody answered at all
                int mobile_hidden_invis = 0;   // hidden because the actor is invisible
                int mobile_level_known = 0;
                int met_found = 0;
                int met_total = 0;
                int dead_hidden = 0;
                int boss_total = 0;
                int boss_found = 0;
                int boss_from_save = 0;
                int boss_no_door = 0;
                // Read once per round rather than per marker.
                const bool boss_save_on = mm::cfg_cached().boss_defeat_from_save;
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
                    // ---- SHRINES ARE FOUND WHEN THEY ARE LIT ----------------------
                    //
                    // A shrine has no per-actor activation flag: the state is the game
                    // mode's `UnlockedFirepoints` list of shrine ids, which
                    // src/shrines.cpp reads at 1 Hz. The ids go into the found set rather
                    // than deriving the flag at draw time, so the map glyph, the minimap
                    // styling, the legend counts and the stats table all agree.
                    // Idempotent: the same id is only ever added.
                    else if (sm.cat == mdb::Cat::Shrine && shrine_is_lit(sm.id))
                    {
                        d.flags |= kFlagFound;
                        g_found_static[idx] = 1;
                        note_found(sm.id);
                    }
                    // A gauge over the chapter's shrines, whichever branch marked them:
                    // `lit N of M` answers "did the join work" directly.
                    if (sm.cat == mdb::Cat::Shrine)
                    {
                        ++lit_total;
                        if (shrine_is_lit(sm.id) && (d.flags & kFlagFound) != 0)
                        {
                            ++lit_found;
                        }
                    }

                    // ---- A BOSS KILLED BEFORE THE MOD WAS INSTALLED ---------------
                    //
                    // `Rule::BossPawn` needs the boss ACTOR to exist and be dead, and a
                    // killed boss never spawns again. The save speaks for those: the
                    // arena's `bossdoor_*` respawn point is in `UnlockedFirepoints`, and
                    // `markers/chapter*.json` carries that id per boss marker.
                    //
                    // DERIVED, NEVER PERSISTED: it is not certain the game unlocks a boss
                    // door on the kill rather than on the first attempt, and a persisted
                    // mark from an uncertain signal would survive fixing the rule.
                    if (sm.cat == mdb::Cat::Boss)
                    {
                        ++boss_total;
                        if (sm.bossdoor.empty())
                        {
                            ++boss_no_door;
                        }
                        else if (mdb::boss_found_from_save(boss_save_on, true, true,
                                                           shr::is_unlocked(sm.bossdoor.c_str())))
                        {
                            d.flags |= kFlagFound;
                            ++boss_from_save;
                        }
                    }
                    const LiveEntry* live = g_live_of_static[idx];

                    // A corpse hides its spawn point too: an enemy's authored position is
                    // in the static DB, so what the health read declared dead is off the
                    // map entirely until the entry ages out.
                    if (mdb::static_twin_is_hidden_by_corpse(live != nullptr,
                                                             live != nullptr && live->dead))
                    {
                        ++dead_hidden;
                        continue;
                    }

                    // ---- "LIVE" MEANS AN ACTOR ANSWERED THIS ROUND, WITH A POSITION -
                    //
                    // For a category that does not move, a twin from a round or two ago
                    // is as good as this round's, and that debounce is what stops a chest
                    // flickering when the sweep races level streaming. For a MOBILE
                    // category it is not: kFlagLive is the x-ray's permission to draw a
                    // person through a wall, and a stale entry (or one whose position
                    // read failed, leaving the AUTHORED position in place) must not carry
                    // it.
                    const bool live_here = live != nullptr && live->pos_valid &&
                                           (live->round == g_round || !mdb::is_mobile_category(sm.cat));
                    if (live != nullptr)
                    {
                        if (live->found)
                        {
                            d.flags |= kFlagFound;
                        }
                        if (live_here)
                        {
                            d.flags |= kFlagLive;
                            d.x = live->x;
                            d.y = live->y;
                            d.z = live->z;
                        }
                    }

                    // ---- A PERSON WHO HAS WALKED AWAY IS NOT DRAWN WHERE THEY WERE -
                    //
                    // Talk to a quest NPC and it relocates, usually to a different placed
                    // actor in another sublevel and so under a different marker id; the
                    // authored position then has no live twin and never will. When the
                    // marker's own level is loaded and a full round has finished since it
                    // loaded, "no live actor answered" is proof, and the hint is dropped
                    // from the published set. The met state is untouched: it lives in the
                    // found set, not in this entry.
                    const int mli = db->marker_level[idx];
                    mdb::MobileTwinFacts mob{};
                    mob.mobile = mdb::is_mobile_category(sm.cat);
                    // A twin that answered but could not be located is NOT an answer:
                    // it leaves the authored position on the entry.
                    mob.live_twin_this_round = live != nullptr && live->round == g_round && live->pos_valid;
                    // It is an answer of its own, though: an actor answering for this id
                    // proves its level is loaded, and the game parks a used-up actor at
                    // (0,0,0). "Answered, unlocatable" is the walked-away case and needs
                    // no level table.
                    mob.live_twin_unlocatable =
                        live != nullptr && live->round == g_round && !live->pos_valid;
                    // Case (e): the actor is standing right there and is invisible.
                    mob.live_twin_invisible =
                        live != nullptr && live->round == g_round && live->invisible;
                    if (mli >= 0 && g_level_known[static_cast<std::size_t>(mli)] != 0)
                    {
                        mob.level_known = true;
                        mob.full_round_since_level_load =
                            g_round > g_level_round[static_cast<std::size_t>(mli)];
                    }
                    // The per-round census behind the `people -` log line, which says
                    // which half of the join is failing: a static count with no joins
                    // means the ids do not match, joins with no supersedes means nobody
                    // has moved, and level_known 0 means the hide rule can never fire.
                    if (mob.mobile)
                    {
                        ++mobile_static;
                        if (mob.live_twin_this_round)
                        {
                            ++mobile_joined;
                            const double dx = live->x - sm.x;
                            const double dy = live->y - sm.y;
                            const double dz = live->z - sm.z;
                            if (dx * dx + dy * dy + dz * dz > kMovedUuSq)
                            {
                                ++mobile_superseded;
                            }
                        }
                        if (mob.live_twin_unlocatable)
                        {
                            ++mobile_walked;
                        }
                        if (mob.live_twin_invisible)
                        {
                            ++mobile_invisible;
                        }
                        // How many twins could be ASKED at all. `invisible 0` with
                        // `visibility known 0` means no route reads on this build and the
                        // rule is dead; `invisible 0` with `visibility known 21` means the
                        // flags are genuinely all false.
                        if (live != nullptr && live->round == g_round && live->invisible_known)
                        {
                            ++mobile_vis_known;
                        }
                        if (mob.level_known)
                        {
                            ++mobile_level_known;
                        }
                    }
                    if (mdb::mobile_twin_is_stale(mob))
                    {
                        g_mobile_hidden.fetch_add(1, std::memory_order_relaxed);
                        if (mob.live_twin_invisible)
                        {
                            ++mobile_hidden_invis;
                        }
                        else if (mob.live_twin_unlocatable)
                        {
                            ++mobile_hidden_walked;
                        }
                        else
                        {
                            ++mobile_hidden_absent;
                        }
                        continue;
                    }
                    // The `met` gauge, over the chapter's people and notes.
                    if (sm.cat == mdb::Cat::Npc || sm.cat == mdb::Cat::Note)
                    {
                        ++met_total;
                        if ((d.flags & kFlagFound) != 0)
                        {
                            ++met_found;
                        }
                    }
                    // The boss gauge, counted after the found file, the save door and
                    // this round's live health read have all had their say.
                    // `boss_total` is incremented earlier because a boss that a later
                    // `continue` drops from the buffer still exists on the map.
                    if (sm.cat == mdb::Cat::Boss && (d.flags & kFlagFound) != 0)
                    {
                        ++boss_found;
                    }

                    // ---- ABSENCE AS EVIDENCE OF A COLLECT -------------------------
                    //
                    // This round has just walked the whole object array, `live` is what
                    // it found for this id, and the level table says whether the marker's
                    // level is loaded and for how long. mdb::absence_marks() holds the
                    // rule.
                    mdb::AbsenceFacts facts{};
                    facts.feature_on = g_absence_on;
                    facts.cat_selected = mdb::cat_enabled(g_absence_cats, sm.cat);
                    facts.already_found = (d.flags & kFlagFound) != 0;
                    facts.level_known = mob.level_known;
                    facts.full_round_since_level_load = mob.full_round_since_level_load;
                    // AN ACTOR THAT ANSWERED IS PRESENT, wherever it is standing -
                    // requiring `pos_valid` here would make a chest whose position read
                    // failed look absent and get auto-marked collected. An actor parked
                    // at (0,0,0) is covered by `!live->found`, which the pickup rule set.
                    facts.twin_alive = live != nullptr && live->round == g_round && !live->found;

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
                    // NAME FIRST: `cls` is always non-empty ("BP_PickupActor_C"), while
                    // markers/chapter*.json carries a name for every entry (a real item
                    // name where the extractor resolved one, the category label
                    // otherwise). Longest shipped name is 31 ASCII chars; `label` is 40.
                    copy_id(d.label, sizeof(d.label), sm.name.empty() ? sm.cls : sm.name);
                    dst.push_back(d);
                }
                g_shrine_lit_found.store(lit_found, std::memory_order_relaxed);
                g_shrine_total.store(lit_total, std::memory_order_relaxed);
                g_mobile_static.store(mobile_static, std::memory_order_relaxed);
                g_mobile_joined.store(mobile_joined, std::memory_order_relaxed);
                g_mobile_superseded.store(mobile_superseded, std::memory_order_relaxed);
                g_mobile_walked.store(mobile_walked, std::memory_order_relaxed);
                g_mobile_invisible.store(mobile_invisible, std::memory_order_relaxed);
                g_mobile_vis_known.store(mobile_vis_known, std::memory_order_relaxed);
                g_mobile_hidden_invis.store(mobile_hidden_invis, std::memory_order_relaxed);
                g_mobile_hidden_walked.store(mobile_hidden_walked, std::memory_order_relaxed);
                g_mobile_hidden_absent.store(mobile_hidden_absent, std::memory_order_relaxed);
                g_mobile_level_known.store(mobile_level_known, std::memory_order_relaxed);
                g_met_found.store(met_found, std::memory_order_relaxed);
                g_met_total.store(met_total, std::memory_order_relaxed);
                g_dead_hidden.store(dead_hidden, std::memory_order_relaxed);
                g_boss_total.store(boss_total, std::memory_order_relaxed);
                g_boss_found.store(boss_found, std::memory_order_relaxed);
                g_boss_from_save.store(boss_from_save, std::memory_order_relaxed);
                g_boss_no_door.store(boss_no_door, std::memory_order_relaxed);
            }

            // Live actors the static DB does not know about - which is everything
            // until tools/markers has produced markers/<chapter>.json, and always the
            // enemies.
            int mobile_live = 0;
            for (const auto& kv : g_live)
            {
                if (mdb::is_mobile_category(kv.second.cat))
                {
                    ++mobile_live;
                }
                if (!mdb::live_only_is_drawn(kv.second.pos_valid, kv.second.dead))
                {
                    continue; // no usable position, or a corpse - see the predicate
                }
                if (db != nullptr)
                {
                    // Only skip the live actor when its static twin was actually drawn
                    // above. A live actor is in the current world by definition, so it is
                    // never filtered out itself, but its static entry may carry another
                    // chapter's number.
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
                // THE LABEL OF A LIVE-ONLY MARKER IS NEVER ITS CLASS NAME. A resolved
                // item name is used when there is one; otherwise the label stays EMPTY
                // and each drawing site turns that into the category's plain word
                // through mdb::display_label().
                if (!kv.second.label.empty())
                {
                    copy_id(d.label, sizeof(d.label), kv.second.label);
                }
                dst.push_back(d);
            }

            g_mobile_live.store(mobile_live, std::memory_order_relaxed);
            g_published_count.store(static_cast<int>(dst.size()), std::memory_order_relaxed);
            g_live_count.store(static_cast<int>(g_live.size()), std::memory_order_relaxed);
            g_slot_published.store(g_slot_next, std::memory_order_release);
            g_slot_next = (g_slot_next + 1) % kSlots;

            // What the publish cost. Unsliced game-thread work, so the F2 round line and
            // the periodic log print all three numbers.
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
        // One FindAllOf per interval, cycling the class table. Reached only when
        // GUObjectArray reports no elements, i.e. the chunked walk has nothing to walk.
        // Its cost lands in the same F2 counters, flagged by `scan_fallback`.
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
        // `g_found_key` is a COPY of the save-slot key the loop thread last acted on:
        // resolution runs on the game thread, and the loop thread must not change the
        // file it is reading half way through a load.

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

        // First sight of a slot with no file of its own: seed it from the shared one.
        // Once, and logged.
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
            unsigned err = 0;
            if (write_whole_file(dst, text, false, err))
            {
                mm::logf(L"markers: first sight of save slot '{}' - copied the shared found tracker "
                         L"({} bytes) into {}",
                         std::wstring(key.begin(), key.end()), text.size(), dst);
            }
            else
            {
                mm::logf(L"markers: could not seed {} from the shared found tracker (error {})", dst, err);
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
                spin::SpinGuard guard(g_inbox_lock);
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
                // EVERY DERIVED VIEW READS THE SAME RULES. The save-backed boss defeat is
                // not in the found FILE, so this table asks the same question the publish
                // point does; otherwise the map and the statistics disagree.
                const bool boss_save_on = mm::cfg_cached().boss_defeat_from_save;
                for (const mdb::StaticMarker& m : db->markers)
                {
                    const int ci = static_cast<int>(m.cat);
                    if (ci < 0 || ci >= mdb::kCatCount)
                    {
                        continue;
                    }
                    const bool found =
                        g_found_master.contains(m.id) ||
                        (m.cat == mdb::Cat::Boss &&
                         mdb::boss_found_from_save(boss_save_on, true, !m.bossdoor.empty(),
                                                   !m.bossdoor.empty() &&
                                                       shr::is_unlocked(m.bossdoor.c_str())));
                    ++s.cat[ci].total;
                    s.cat[ci].found += found ? 1 : 0;
                    // Chapter 0 is the bucket for a manifest whose "chapter" is not a
                    // number - the DLC one spells it "DLC".
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
            s.shrine_lit_marks = g_shrine_lit_marks.load(std::memory_order_relaxed);
            s.met_marks = g_met_marks.load(std::memory_order_relaxed);
            s.boss_defeated = g_boss_defeated.load(std::memory_order_relaxed);
            s.boss_found = g_boss_found.load(std::memory_order_relaxed);
            s.boss_total = g_boss_total.load(std::memory_order_relaxed);
            s.boss_from_save = g_boss_from_save.load(std::memory_order_relaxed);
            s.boss_no_door = g_boss_no_door.load(std::memory_order_relaxed);
            s.dead_dropped = g_dead_dropped.load(std::memory_order_relaxed);
            s.health_unknown = g_health_unknown.load(std::memory_order_relaxed);
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
            spin::SpinGuard guard(g_stats_lock);
            g_stats = s;
        }

        // markers/items.json -> {item id -> display name}, the other half of the join
        // resolve_item_name() makes for loot an enemy drops. A missing file is not an
        // error: it is a toolchain artifact, and without it those drops fall back to
        // their category label.
        void load_item_names(const std::wstring& dir)
        {
            g_item_names.clear();
            const std::wstring path = dir + L"\\items.json";
            std::string text;
            if (!read_whole_file(path, text))
            {
                mm::logf(L"markers: {} not found - loot dropped by enemies will be labelled by "
                         L"category rather than by item name",
                         path);
                return;
            }
            std::string error;
            if (!mdb::parse_items_json(text, g_item_names, error))
            {
                mm::logf(L"markers: {} rejected - {}", path, widen(error));
                g_item_names.clear();
                return;
            }
            mm::logf(L"markers: {} -> {} item name(s) for runtime drops", path, g_item_names.size());
        }

        // Cap on *.json files read from the markers folder, so a folder full of files
        // cannot stall the load. Hitting it is logged.
        constexpr std::size_t kMaxManifestFiles = 64;

        void load_static_db()
        {
            const std::wstring dir = markers_dir();

            // Enumerate the directory rather than probing chapter1..8: the offline
            // extractor also emits chapterdlc.json and whatever the game adds next.
            // `*.sample.json` is documentation, not data.
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
                } while (::FindNextFileW(h, &find) != 0 && files_found.size() < kMaxManifestFiles);
                ::FindClose(h);
            }
            if (files_found.size() >= kMaxManifestFiles)
            {
                mm::logf(L"markers: there are more than {} *.json files in {} - only the first {} "
                         L"(sorted by name) were read; the rest were IGNORED",
                         kMaxManifestFiles,
                         dir,
                         kMaxManifestFiles);
            }
            // Deterministic order, so the "first duplicate id wins" rule is stable
            // across runs.
            std::sort(files_found.begin(), files_found.end());

            auto db = std::make_unique<StaticDb>();
            // (one-past-last marker index, file name) in load order, to name the file in
            // a duplicate-id line.
            std::vector<std::pair<std::size_t, std::wstring>> file_ranges;
            const auto file_of = [&file_ranges](std::size_t index) -> std::wstring {
                for (const auto& [end, name] : file_ranges)
                {
                    if (index < end)
                    {
                        return name;
                    }
                }
                return L"?";
            };
            int files = 0;
            bool warned_legacy_cat = false; // one line per load, not one per file
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
                    // enumeration takes every *.json, and markers/items.json lives here
                    // too. Only a file claiming to BE a marker manifest is reported
                    // broken.
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
                file_ranges.emplace_back(db->markers.size(), name);
                mm::logf(L"markers: {} -> {} marker(s) (chapter {}, schema {}{}{})",
                         path,
                         report.added,
                         widen(report.chapter_label.empty() ? std::string{"?"} : report.chapter_label),
                         widen(report.schema),
                         report.skipped != 0 ? std::format(L", {} skipped", report.skipped) : std::wstring{},
                         report.unknown_cat != 0 ? std::format(L", {} unknown category", report.unknown_cat)
                                                 : std::wstring{});
                // A manifest spelling a renamed category the old way parses fine
                // (mdb::cat_from_legacy_name) but means the `markers/` folder is older
                // than the DLL.
                if (report.legacy_cat != 0 && !warned_legacy_cat)
                {
                    warned_legacy_cat = true;
                    mm::logf(L"markers: this markers/ folder still uses a category name "
                             L"from an older release ({} marker(s), e.g. 'merchant', now "
                             L"'note') - they were accepted, but the folder should be "
                             L"replaced with the one shipped beside this DLL",
                             report.legacy_cat);
                }
            }

            // Dedupe at load (marker_dedupe.hpp): the first copy of an id wins, and every
            // dropped copy is named with the file it came from.
            std::vector<mdb::DupDrop> drops;
            const int duplicates = mdb::dedupe_by_id(db->markers, db->by_id, drops);
            if (duplicates != 0)
            {
                int logged = 0;
                for (const mdb::DupDrop& drop : drops)
                {
                    if (logged >= 8)
                    {
                        break;
                    }
                    ++logged;
                    mm::logf(L"markers: duplicate id '{}' - the copy in {} was DROPPED, the one in {} is "
                             L"kept",
                             widen(drop.id),
                             file_of(drop.dropped),
                             file_of(drop.kept));
                }
                mm::logf(L"markers: {} duplicate id(s) dropped at load ({} named above; only the first "
                         L"copy of an id is kept, so every marker can be marked found)",
                         duplicates,
                         logged);
            }

            // Intern the level names: the absence rule's "is this marker's level loaded,
            // and since when" then costs one hash per UNIQUE level per round plus an
            // array index per marker.
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
            const StaticDb* const previous = g_db.exchange(db.release(), std::memory_order_acq_rel);
            if (previous != nullptr)
            {
                // Freed by retire_databases() a few rounds from now, not here: the game
                // thread may be inside publish_round() with this very pointer.
                g_retired.push_back(
                    RetiredDb{previous, g_rounds.load(std::memory_order_relaxed), ::GetTickCount64()});
            }
            load_item_names(dir);
        }

        std::string serialize_found()
        {
            std::vector<std::string> ids;
            ids.reserve(g_found_master.size());
            for (const std::string& id : g_found_master)
            {
                ids.push_back(id);
            }
            return mdb::found_serialize(std::move(ids));
        }

        // Park the pending write where the DLL_PROCESS_DETACH path can write it without
        // allocating. Loop thread only; `g_stage_valid` is the publish.
        void stage_found_snapshot()
        {
            const std::string text = serialize_found();
            const std::wstring path = found_path();
            if (text.size() >= kStageTextMax || path.size() >= kStagePathMax)
            {
                g_stage_valid.store(false, std::memory_order_release);
                return;
            }
            g_stage_valid.store(false, std::memory_order_release);
            std::memcpy(g_stage_text, text.data(), text.size());
            g_stage_len = text.size();
            std::memcpy(g_stage_path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            g_stage_valid.store(true, std::memory_order_release);
        }

        // Loop thread, from on_update. Frees the databases a reload replaced, once no
        // reader can still hold one.
        void retire_databases(std::uint64_t now)
        {
            if (g_retired.empty())
            {
                return;
            }
            const std::uint64_t round = g_rounds.load(std::memory_order_relaxed);
            std::size_t out = 0;
            for (std::size_t i = 0; i < g_retired.size(); ++i)
            {
                const RetiredDb& r = g_retired[i];
                const bool rounds_done = round >= r.round + kRetireRounds;
                const bool time_done = now - r.ms >= kRetireMs;
                if (rounds_done && time_done)
                {
                    MM_LOGV(L"markers: freed a retired marker database ({} marker(s), replaced {} ms and "
                            L"{} round(s) ago)",
                            r.db->markers.size(),
                            now - r.ms,
                            round - r.round);
                    delete r.db;
                    continue;
                }
                g_retired[out++] = r;
            }
            g_retired.resize(out);
        }

        void load_found_file()
        {
            std::string text;
            const std::wstring path = found_path();
            const mmfile::ReadInfo info = read_whole_file_ex(path, text);
            if (info.status == mmfile::ReadStatus::Failed || info.too_big)
            {
                // NOT "it does not exist yet". The set in memory is left as it is and
                // every write is refused until a retry can read the file.
                g_found_unreadable = true;
                g_found_retry_ms = ::GetTickCount64();
                mm::logf(L"markers: the found tracker {} EXISTS but could not be read ({}) - your "
                         L"collection progress is NOT lost and nothing will be written over that file "
                         L"until it can be read again (something else may have it open: anti-virus, "
                         L"a cloud sync client, a text editor). Retried every 10 s.",
                         path,
                         info.too_big ? std::format(L"it is {} byte(s), over the 32 MB cap", info.size)
                                      : std::format(L"error {}", info.error));
                return;
            }
            g_found_unreadable = false;
            g_found_master.clear();
            std::vector<std::string> ids;
            if (info.status == mmfile::ReadStatus::Ok)
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
            g_found_backoff_ms = 0;
            g_found_fail_streak = 0;
            g_stage_dirty = true;
        }

        // A read that works is UNIONED with whatever was marked while the file was
        // unreadable, so nothing found in between is lost.
        void retry_found_load()
        {
            std::string text;
            const std::wstring path = found_path();
            const mmfile::ReadInfo info = read_whole_file_ex(path, text);
            if (info.status == mmfile::ReadStatus::Failed || info.too_big)
            {
                return; // still locked; keep refusing to write
            }
            g_found_unreadable = false;
            std::vector<std::string> ids;
            if (info.status == mmfile::ReadStatus::Ok)
            {
                mdb::found_parse(text, ids);
            }
            int added = 0;
            for (const std::string& id : ids)
            {
                added += g_found_master.insert(id).second ? 1 : 0;
            }
            std::vector<std::string> all;
            all.reserve(g_found_master.size());
            for (const std::string& id : g_found_master)
            {
                all.push_back(id);
            }
            publish_inbox(all, true);
            recompute_stats();
            mm::logf(L"markers: the found tracker {} can be read again - {} id(s) from the file, {} kept "
                     L"from this session, {} total; saving is enabled again",
                     path,
                     ids.size(),
                     static_cast<int>(g_found_master.size()) - added,
                     g_found_master.size());
            g_found_dirty = true; // the union may differ from the file
            g_found_dirty_ms = ::GetTickCount64();
            g_found_backoff_ms = 0;
            g_found_fail_streak = 0;
            g_stage_dirty = true;
        }

        void save_found_file()
        {
            const std::wstring path = found_path();
            if (g_found_unreadable)
            {
                // load_found_file logs it once and the retry logs when it clears. The
                // dirty flag stays set.
                return;
            }
            const std::string text = serialize_found();
            unsigned err = 0;
            if (write_whole_file(path, text, true, err))
            {
                // One line per 30 s, not per save; the line carries the total, the marks
                // added since it was last printed, and how many saves it stands for.
                static std::uint64_t last_log = 0;
                static std::uint32_t coalesced = 0;
                const std::uint64_t now = ::GetTickCount64();
                if (last_log == 0 || now - last_log >= 30000)
                {
                    mm::logf(L"markers: found tracker saved ({} id(s), {} new since the last line{}) "
                             L"-> {}",
                             g_found_master.size(),
                             g_marks_since_log,
                             coalesced != 0 ? std::format(L", {} earlier save(s) coalesced", coalesced)
                                            : std::wstring{},
                             path);
                    last_log = now;
                    coalesced = 0;
                    g_marks_since_log = 0;
                }
                else
                {
                    ++coalesced;
                }
                g_found_dirty = false;
                g_found_backoff_ms = 0;
                g_found_fail_streak = 0;
                g_stage_valid.store(false, std::memory_order_release); // the file now holds it
                g_stage_dirty = false;
            }
            else
            {
                // THE MARKS STAY DIRTY, and the write is retried with a doubling backoff
                // on top of the ordinary debounce. Logged once per failure for the first
                // three, then every eighth.
                ++g_found_fail_streak;
                g_found_backoff_ms = g_found_backoff_ms == 0 ? 1000 : g_found_backoff_ms * 2;
                if (g_found_backoff_ms > 60000)
                {
                    g_found_backoff_ms = 60000;
                }
                g_found_dirty_ms = ::GetTickCount64();
                g_stage_dirty = true; // the staged copy is what the shutdown flush will write
                if (g_found_fail_streak <= 3 || (g_found_fail_streak % 8) == 0)
                {
                    mm::logf(L"markers: FAILED to write the found tracker {} (error {}, attempt {}) - the "
                             L"{} mark(s) are still held in memory and the write is retried in {} ms; "
                             L"{}.tmp may hold the data",
                             path,
                             err,
                             g_found_fail_streak,
                             g_found_master.size(),
                             g_found_backoff_ms,
                             path);
                }
            }
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
        spin::SpinGuard guard(g_stats_lock);
        return g_stats;
    }

    std::uint64_t rounds()
    {
        return g_rounds.load(std::memory_order_relaxed);
    }

    void on_unreal_init()
    {
        load_static_db();
        shr::load_table();
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
            spin::SpinGuard guard(g_toggle_lock);
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
        shr::load_table();
        slotid::rescan_files();
        adopt_slot_key();
        load_found_file();
        recompute_stats();
    }

    // The debounced write, forced. An ordinary loop-thread save, so it may allocate and
    // log like any other.
    void flush_found_tracker()
    {
        if (g_found_dirty && !g_found_unreadable && mm::cfg_cached().found_tracker)
        {
            mm::log(L"markers: flushing the found tracker before standing down");
            save_found_file();
        }
    }

    // DLL_PROCESS_DETACH. The process may be dying abnormally with a corrupted heap and
    // DllMain runs under the loader lock, so this allocates nothing, takes no lock and
    // logs nothing: it writes the bytes the loop thread staged, to the path it staged,
    // through the same temp-file-plus-rename the ordinary save uses. Idempotent.
    void flush_found_tracker_at_exit()
    {
        if (g_flushed_at_exit.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        if (!g_stage_valid.load(std::memory_order_acquire))
        {
            return;
        }
        wchar_t tmp[kStagePathMax + 8];
        std::size_t n = 0;
        while (n + 1 < kStagePathMax && g_stage_path[n] != 0)
        {
            tmp[n] = g_stage_path[n];
            ++n;
        }
        if (n == 0)
        {
            return;
        }
        tmp[n++] = L'.';
        tmp[n++] = L't';
        tmp[n++] = L'm';
        tmp[n++] = L'p';
        tmp[n] = 0;

        const HANDLE h = ::CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        const bool ok = g_stage_len == 0 ||
                        (::WriteFile(h, g_stage_text, static_cast<DWORD>(g_stage_len), &written, nullptr) != 0 &&
                         written == static_cast<DWORD>(g_stage_len));
        if (ok)
        {
            ::FlushFileBuffers(h);
        }
        ::CloseHandle(h);
        if (!ok)
        {
            return;
        }
        ::MoveFileExW(tmp, g_stage_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void on_update()
    {
        const std::uint64_t now = ::GetTickCount64();

        // The save-slot watch: the loop thread notices the resolved key changed and
        // swaps files. Any pending write goes to the OLD file first - the finds it holds
        // belong to the save that was loaded when they happened.
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
                spin::SpinGuard guard(g_outbox_lock);
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
                g_stage_dirty = true;
                // Counted, not logged per mark: the count rides on the save line, which
                // is what actually changes persisted state.
                g_marks_since_log += static_cast<std::uint32_t>(added);
                MM_LOGV(L"markers: auto-marked {} new marker(s) as found ({} total)", added,
                        g_found_master.size());
                recompute_stats();
            }
        }

        // Manual toggles from the full map go through the same master set and debounced
        // write as the auto-marks, and the whole set is republished to the game thread
        // so the draw buffer agrees on the next round.
        if (g_toggle_pending.exchange(false, std::memory_order_acquire))
        {
            std::vector<ToggleReq> reqs;
            {
                spin::SpinGuard guard(g_toggle_lock);
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
                g_stage_dirty = true;
                std::vector<std::string> all;
                all.reserve(g_found_master.size());
                for (const std::string& id : g_found_master)
                {
                    all.push_back(id);
                }
                publish_inbox(std::move(all), true);
                recompute_stats();
                g_marks_since_log += static_cast<std::uint32_t>(changed);
                mm::logf(L"markers: {} manual found change(s) from the map ({} total)", changed,
                         g_found_master.size());
            }
        }

        retire_databases(now);

        const mm::Config& cfg = mm::cfg_cached();

        // The found file exists and could not be read: nothing is written over it, and
        // the read is retried until it works.
        if (g_found_unreadable && now - g_found_retry_ms >= 10000)
        {
            g_found_retry_ms = now;
            retry_found_load();
        }

        // Keep the shutdown snapshot in step with the pending write. One serialisation
        // per burst of marks, on the loop thread, so DLL_PROCESS_DETACH only has to
        // copy bytes to a file.
        if (g_found_dirty && g_stage_dirty && cfg.found_tracker && !g_found_unreadable)
        {
            stage_found_snapshot();
            g_stage_dirty = false;
        }

        // A failed write adds its backoff to the ordinary debounce rather than dropping
        // the marks (they stay dirty until a write succeeds).
        if (g_found_dirty && cfg.found_tracker &&
            now - g_found_dirty_ms >=
                static_cast<std::uint64_t>(cfg.found_save_debounce_ms) + g_found_backoff_ms)
        {
            save_found_file();
        }

        // The periodic health summary: once a minute at `normal`, the found-rule
        // counters and the NPC census. The per-round timings are `verbose`.
        static std::uint64_t last_round_log = 0;
        const std::uint64_t census_period = mm::log_enabled(mm::LogLv::Verbose) ? 30000 : 60000;
        if (now - last_round_log >= census_period)
        {
            last_round_log = now;
            if (g_rounds.load(std::memory_order_relaxed) != 0)
            {
                MM_LOGV(L"markers: round {} - scan {:.1f} ms over {} pump(s), publish {:.3f} ms "
                        L"(avg {:.3f}, peak {:.3f}); {} published, {} live",
                        g_rounds.load(std::memory_order_relaxed),
                        g_scan_round_ms.load(std::memory_order_relaxed),
                        g_scan_round_slices.load(std::memory_order_relaxed),
                        g_publish_ms.load(std::memory_order_relaxed),
                        g_publish_ms_avg.load(std::memory_order_relaxed),
                        g_publish_ms_peak.load(std::memory_order_relaxed),
                        g_published_count.load(std::memory_order_relaxed),
                        g_live_count.load(std::memory_order_relaxed));
                // Which rule fired and which cannot read its property: a climbing
                // `health unknown` with zero dead/defeated means the Health component
                // route is wrong on this build.
                mm::logf(L"markers: rules - shrines lit {} of {} ({} marked this session), "
                         L"met {} of {} ({} marked this session), "
                         L"bosses defeated {} of {} ({} from save, {} with no door, "
                         L"{} killed this session), "
                         L"dead hidden {} ({} newly dead), health unknown {} (health field width {})",
                         g_shrine_lit_found.load(std::memory_order_relaxed),
                         g_shrine_total.load(std::memory_order_relaxed),
                         g_shrine_lit_marks.load(std::memory_order_relaxed),
                         g_met_found.load(std::memory_order_relaxed),
                         g_met_total.load(std::memory_order_relaxed),
                         g_met_marks.load(std::memory_order_relaxed),
                         g_boss_found.load(std::memory_order_relaxed),
                         g_boss_total.load(std::memory_order_relaxed),
                         g_boss_from_save.load(std::memory_order_relaxed),
                         g_boss_no_door.load(std::memory_order_relaxed),
                         g_boss_defeated.load(std::memory_order_relaxed),
                         g_dead_hidden.load(std::memory_order_relaxed),
                         g_dead_dropped.load(std::memory_order_relaxed),
                         g_health_unknown.load(std::memory_order_relaxed),
                         g_health_width);
                // THE NPC CENSUS, one line, naming which half of the join fails.
                // static = markers of that category in the chapter; live = live entries
                // held; joined = static markers a live actor answered for THIS round with
                // a usable position; superseded = those standing more than kMovedUu from
                // where they were authored; level resident = static markers whose own
                // sublevel is loaded (the level-based clause cannot fire below that);
                // walked away = a live actor answered for the id but could NOT be
                // located; the three halves of `hidden` say which clause did it.
                mm::logf(L"markers: people - static {}, live {}, joined {}, superseded {}, "
                         L"walked away {}, invisible {} of {} asked, level resident {}, "
                         L"hidden {} ({} invisible + {} walked + {} absent)",
                         g_mobile_static.load(std::memory_order_relaxed),
                         g_mobile_live.load(std::memory_order_relaxed),
                         g_mobile_joined.load(std::memory_order_relaxed),
                         g_mobile_superseded.load(std::memory_order_relaxed),
                         g_mobile_walked.load(std::memory_order_relaxed),
                         g_mobile_invisible.load(std::memory_order_relaxed),
                         g_mobile_vis_known.load(std::memory_order_relaxed),
                         g_mobile_level_known.load(std::memory_order_relaxed),
                         g_mobile_hidden.load(std::memory_order_relaxed),
                         g_mobile_hidden_invis.load(std::memory_order_relaxed),
                         g_mobile_hidden_walked.load(std::memory_order_relaxed),
                         g_mobile_hidden_absent.load(std::memory_order_relaxed));
            }
        }

        // Cheap counters the panel shows. The whole per-chapter table is recomputed only
        // when the found set changes, above.
        static std::uint64_t last_light = 0;
        if (now - last_light >= 1000)
        {
            last_light = now;
            spin::SpinGuard guard(g_stats_lock);
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
        // Replace the set, keeping the round each surviving level was first seen at.
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
        // Everything keyed to the world that just went.
        hl::drop_caches();
        slotid::drop_caches();
        shr::drop_caches();
        g_layouts.clear();
        g_class_spec.clear();
        g_health_prop.clear();   // routes keyed to a UClass* of that world
        g_health_fields.clear();
        g_hidden_route.clear();
        g_drop_name.clear();     // keyed on the ACTOR: a recycled allocation must not
        g_item_prop.clear();     // hand a new drop the old one's item name
        g_id_cache.clear();
        g_live.clear();
        // A level name means nothing in the next world, and a half-finished absence
        // streak must not survive a load.
        g_levels.clear();
        std::fill(g_absent_streak_idx.begin(), g_absent_streak_idx.end(), 0);
        std::fill(g_live_of_static.begin(), g_live_of_static.end(), nullptr);
        std::fill(g_level_known.begin(), g_level_known.end(), static_cast<std::uint8_t>(0));
        g_subset_valid = false; // the chapter is re-detected in the next world
        g_player_ok = false;
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
    // CALLED FROM EVERY ProcessEvent pre-callback while the last validated state stands.
    // The throttling below decides the scan rate, on QPC rather than GetTickCount64,
    // whose ~15.6 ms granularity is coarser than the slice period.
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

        // 1. Hard ceiling on how often anything at all happens here: mm::config() copies
        //    the config under a spinlock, and at ProcessEvent rate that alone would be
        //    thousands of lock round-trips a second.
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

        // These hooks need the game thread and the validated state this pump runs on,
        // and none of them touches anything this module owns.
        //
        // The x-ray highlight's camera reader: one atomic load unless the highlight key
        // is held or the compass is on.
        hl::game_thread_pump(now, now_us, world, cfg);
        // The save-slot resolver: one atomic load once a route has answered, and it
        // stops asking until the world changes (drop_caches re-arms it).
        slotid::game_thread_pump(now, world);
        // The shrine unlock state: four raw property reads at 1 Hz once the component is
        // found. Feeds "shrines lit" and the shrine list's travel offer.
        shr::game_thread_pump(now);
        // The one-press recon dump: one atomic load unless a dump was asked for.
        recon::game_thread_pump(world);

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

        // 3. A round that finished early waits until its slot comes round again.
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

        // 4b. The player's position for Rule::Proximity, once per slice: a seqlock read
        //     of a POD struct, not an engine call, and never per actor.
        {
            mm::Snapshot snap{};
            if (mm::read_snapshot(snap) && snap.has_pawn && snap.pawn_is_gameplay && !snap.transition)
            {
                g_player_x = snap.x;
                g_player_y = snap.y;
                g_player_z = snap.z;
                g_player_ok = true;
            }
            else
            {
                g_player_ok = false;
            }
        }

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

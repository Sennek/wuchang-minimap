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
#include "recon.hpp"
#include "shrines.hpp"
#include "scan_sched.hpp"
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
            ControllerPawn, // AI controller: the marker is its possessed Pawn
            Proximity,     // NPC / note: "met" = seen loaded within kMetRadius of the player
            BossPawn       // boss character: defeated when its controller's Health.Current <= 0
        };

        // "Met" radius for Rule::Proximity, in Unreal units (1 uu = 1 cm), i.e. 30 m.
        // Deliberately NOT a config key: adding one means editing config_keys.hpp, the
        // parser and the shipped config file, three files another workstream owns this
        // session. The number is the same order as `highlight_radius`'s default (3000)
        // and nothing about it is a taste decision - it only has to be "close enough
        // that you have certainly seen this person".
        constexpr double kMetRadius = 3000.0;
        constexpr double kMetRadiusSq = kMetRadius * kMetRadius;

        // How many rounds a LIVE-ONLY entry (persist == false: enemies) may go
        // unanswered before it is dropped. Persisted categories keep the configured
        // `markers_live_grace_rounds` debounce, which exists so a marker does not
        // flicker when a sweep races level streaming; an enemy has no static twin to
        // fall back on, so a stale one is a corpse drawn as a threat. One round is the
        // minimum that still means "the last completed sweep did not see it".
        constexpr std::uint64_t kLiveOnlyGraceRounds = 1;

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
            // ---- NPCs and reading points (2026-09-03) ---------------------------
            //
            // `BP_NPC_C` is the game's interactable-character base and it covers all
            // ~50 `*_NPC_C` blueprints in one entry, because spec_for_class() walks the
            // super chain by name. That is not a guess: `tools/markers/class_graph.py`
            // reads the `super` field out of every cooked `.uasset`'s export map and
            // lists 78 descendants of `BP_NPC_C` - every class the offline extractor
            // ever put in the `npc` or `note` bucket, and nothing else that is a
            // character.
            //
            // Five of those 78 are NOT people, so they get their own exact entries
            // (an exact match at depth 0 always beats the base class): the shrine
            // above, the reading point below, and the three after it. Anything else that
            // ever derives from BP_NPC_C lands in `npc`, which is the right default.
            //
            // "Found" means MET: the actor was seen loaded within kMetRadius of the
            // player. There is no per-NPC saved flag to read (and dialogue state is not
            // reachable from here), so proximity is the honest definition - and it is
            // persisted, because "I have been there" does not become false again.
            // `DKDC_NPC_C` is NOT a merchant (it shipped as one up to 0.9.4): every
            // placed instance carries a read-point id, the blueprint has no character
            // mesh, its only interaction string is "Check" and it spawns the hint
            // particle. It is one of the game's readable notes. The game's actual
            // merchant, Tao Qing, is an ordinary `BP_NPC_C` descendant and stays `npc`.
            {L"DKDC_NPC_C", mdb::Cat::Note, Rule::Proximity, true},
            {L"BP_NPC_C", mdb::Cat::Npc, Rule::Proximity, true},
            // Not people, despite deriving from BP_NPC_C. Categories match what
            // tools/markers/marker_classes.py puts in the static DB, so the live actor
            // and its static twin never disagree.
            {L"ItemCollectionBox_C", mdb::Cat::Pickup, Rule::PickupDying, true},
            {L"BP_KlesaCleaner_C", mdb::Cat::Other, Rule::None, false},
            {L"BP_PuzzlesDoor_C", mdb::Cat::Door, Rule::UsedBool, true},
            // ---- Bosses (2026-09-03) -------------------------------------------
            //
            // `BP_PlacedBossAI_C` is the base of all 32 boss blueprints (class_graph.py
            // again), and every one of the game's 25 placed boss instances is one of
            // them - so this single entry replaces the offline `*_BOSS_AI`-sublevel
            // heuristic at runtime as well as offline.
            //
            // "Found" means DEFEATED, read from the health the recon dumps already
            // prove is there: a boss pawn's `Controller` is an
            // `Impl_BaseAIController_C` carrying a `Health` `ExtendedStatComponent_C`
            // with reflected `Current` / `Max` floats, and a dead character reads
            // `Current = 0` while its actor is still in the object array
            // (`dump_20260902_102655_world.txt` line 33:
            // `Impl_BaseAIController_C_2147479402.Health Current=0.0 Max=100.0`).
            // It is persisted, so the mark survives leaving the arena.
            {L"BP_PlacedBossAI_C", mdb::Cat::Boss, Rule::BossPawn, true},
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
        // Marks (auto or manual) added since the found-tracker save line was last
        // printed. The save line reports and clears it, so at the default log level one
        // line per half-minute says everything the fifty-odd suppressed ones would have.
        std::uint32_t g_marks_since_log = 0;
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
            // The position READ succeeded (whatever it returned), and separately: the
            // position is usable, i.e. it is not the (0,0,0) parking spot.
            //
            // THESE ARE NOT THE SAME QUESTION, and conflating them is what made static
            // loot disappear. `pos_valid == false` used to mean both "the actor is parked
            // at the origin, so it has been collected" and "the read failed, so I have no
            // idea" - and the pickup rule then marked a perfectly untouched pickup as
            // COLLECTED (persisted to wuchang_minimap_found.txt, so permanently) purely
            // because a RootComponent read did not come back. Same shape as the kFlagLive
            // bug: "found the actor, cannot locate it" is its own answer and it is not
            // evidence about game state.
            bool pos_read = false;
            bool pos_valid = false;
            // The health read answered "zero". The entry is KEPT rather than erased,
            // because erasing it only removes the live position - the enemy's authored
            // spawn point is in the static DB too, and with the live entry gone the
            // corpse's static twin was drawn again at the spawn point, which is exactly
            // the "dead enemies do not disappear" the rule exists to fix. A dead entry
            // suppresses both halves at the publish point.
            bool dead = false;
            // The actor answered a visibility read this round, and what it said. Only
            // filled for the mobile ("walks away") categories - see actor_is_invisible.
            bool invisible_known = false;
            bool invisible = false;
            std::uint64_t round = 0;
            // The display name, when one could be resolved for a LIVE-ONLY actor (an
            // enemy's dropped loot). Empty means "nothing better than the category word
            // is known" - and the category word is what gets drawn, never the class name.
            std::string label;
        };

        std::unordered_map<std::string, LiveEntry> g_live;

        // markers/items.json, {item id -> display name}. Read once per DB load on the
        // loop thread, then read-only from the game thread.
        std::unordered_map<int, std::string> g_item_names;
        // UObject* -> the item name resolved for it (empty = "asked, and there is none").
        // Memoised because the sweep sees the same drop every round and the answer cannot
        // change: an actor's item id is set when it is spawned. Dropped with every other
        // world-keyed cache.
        std::unordered_map<const void*, std::string> g_drop_name;
        // The property that reached the item id on a given class, so the walk happens
        // once per class and the log says which route won.
        std::unordered_map<const void*, int> g_item_prop;

        const void* g_world = nullptr;
        std::uint64_t g_round = 0;

        // The player's position, refreshed once per SLICE from the published snapshot
        // (a seqlock read of a POD struct - no lock, no engine call). Rule::Proximity
        // needs it per actor, and taking it per actor would be a read of ~600 bytes a
        // few hundred times a round for a number that moves by centimetres in that
        // time. `g_player_ok` is false whenever the reader has no validated gameplay
        // pawn, and then nothing is ever marked "met".
        double g_player_x = 0.0;
        double g_player_y = 0.0;
        double g_player_z = 0.0;
        bool g_player_ok = false;

        // Diagnostics for the two rules added on 2026-09-03, so an in-game session can
        // tell "the rule never fired" from "the property is not there".
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
        // Every one of these is a GAUGE - "how many right now" - not a running total.
        // The distinction is the whole reason `shrines lit` read 0 for a session with
        // nineteen lit shrines: a total of new marks is zero on every session after the
        // one that discovered them. A gauge cannot lie that way.
        std::atomic<int> g_shrine_lit_found{0}; // shrines in this chapter that are lit AND found
        std::atomic<int> g_shrine_total{0};     // shrines in this chapter's static DB
        // The boss gauge. `g_boss_found` / `g_boss_total` is "how many of this
        // chapter's bosses read as found right now" whichever rule said so;
        // `g_boss_from_save` is how many of those came from the save's boss doors
        // rather than from a health read this session. Gauges, for the reason above:
        // `g_boss_defeated` is a count of NEW marks and reads 0 in every session after
        // the one that saw the kill - the third time this project has been bitten by
        // exactly that (shrines lit, met, and now this).
        std::atomic<int> g_boss_found{0};
        std::atomic<int> g_boss_total{0};
        std::atomic<int> g_boss_from_save{0};
        // Bosses whose marker carries no `bossdoor_*` id at all, so the save can never
        // speak for them (2 of 28 in the shipped manifests: the Realm of Madness Bai Kru
        // variant and the DLC Honglan).
        std::atomic<int> g_boss_no_door{0};
        std::atomic<int> g_mobile_static{0};      // npc static markers considered
        std::atomic<int> g_mobile_live{0};        // npc live entries held
        std::atomic<int> g_mobile_joined{0};      // static markers whose live twin answered
        std::atomic<int> g_mobile_superseded{0};  // ... and stands more than kMovedUu away
        std::atomic<int> g_mobile_walked{0};      // a live twin answered but is unlocatable
        // A live twin standing at its authored spot that the game has made INVISIBLE -
        // the mechanism run 4 proved is the real one - and how many twins could be asked
        // the question at all. `invisible 0 of 0 asked` is a dead rule; `0 of 21` is a
        // live rule saying the flags are false.
        std::atomic<int> g_mobile_invisible{0};
        std::atomic<int> g_mobile_vis_known{0};
        std::atomic<int> g_mobile_hidden_invis{0};  // ... and was hidden for it
        std::atomic<int> g_mobile_hidden_walked{0}; // ... and was hidden for it
        std::atomic<int> g_mobile_hidden_absent{0}; // hidden because nobody answered at all
        std::atomic<int> g_mobile_level_known{0}; // ... whose own level is resident
        // The "met" gauge: people and notes in this chapter that are in the found set,
        // over how many there are. A GAUGE, because the state is persisted - see the
        // g_met_marks comment.
        std::atomic<int> g_met_found{0};
        std::atomic<int> g_met_total{0};
        std::atomic<int> g_dead_hidden{0};        // markers suppressed because they are dead

        // How far a live person has to stand from their authored position before the
        // census calls the static hint superseded. 3 m: further than the metre or two
        // of idle wander, closer than any relocation worth reporting.
        constexpr double kMovedUu = 300.0;
        constexpr double kMovedUuSq = kMovedUu * kMovedUu;

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
        // FIRST OCCURRENCE MEANS ONCE PER SESSION.
        //
        // The four "route on <class> is <property>" diagnostics are exactly the lines a
        // bug report needs - they say which reflected property answered for visibility,
        // health and item names on this build - and each used to be printed once per
        // entry in its UClass*-keyed cache. Those caches are performance caches: they are
        // dropped on every level transition, so run 5 printed the same ~20 lines seven
        // times over, ~120 lines of a 2600-line log.
        //
        // This set is keyed by the route plus the class NAME, is never cleared, and costs
        // a few dozen short strings for the life of the session.
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

        // A reflected bool. BITFIELD-AWARE: this used to read the byte at the property
        // offset and test it for non-zero, which is right for a blueprint-authored bool
        // (each gets its own byte with mask 0x01) and WRONG for a native engine
        // bitfield - `uint8 bHidden : 1` on AActor shares its byte with `bNetTemporary`,
        // `bTearOff` and a dozen others, so the old read answered "is any flag in this
        // byte set?". `uer::read_bool_prop` asks `FBoolProperty` for the bit. The byte
        // test survives only as the fallback for a property whose bool info does not
        // validate, so the four blueprint flags this file has always read (`Used`,
        // `DoorOpen`, `Active`, `dying`) cannot regress.
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
        // Run 4's census killed the theory the moved-NPC rules were built on:
        //
        //   markers: people - static 53, live 24, joined 21, superseded 0,
        //                     walked away 0, level resident 21, hidden 0
        //
        // while the user was still seeing an NPC drawn where one used to stand. Every
        // one of the 21 joined twins answered WITH a usable position, and not one stood
        // more than 3 m from where it was authored - so cases (b), (c) and (d) of
        // `mdb::mobile_twin_is_stale` are all unreachable for them and case (a) drew the
        // marker. In other words this game does NOT park or destroy a used-up NPC (the
        // way it parks a collected pickup at the origin): the actor stays exactly where
        // it was placed and is made INVISIBLE, and the person you meet later is a
        // different placed actor in another sublevel. The same mechanism is authored for
        // bosses - `ST_LevelScriptBossData` carries a `隐藏击败过的尸体` ("hide the
        // corpse of a defeated boss") flag - so it is the game's idiom, not a guess.
        //
        // THE ROUTES, best first, and every one of them is a raw read:
        //   1. `bHidden` - AActor's own flag, what `SetActorHiddenInGame` writes. It is a
        //      native bitfield, which is why the bool read above had to learn about masks.
        //   2. `bLocalHidden` - the game's own addition, listed in every F8 dump right
        //      after `bHidden` and beside `DCSHiddenStateTypes`. A DCS-specific hide is
        //      exactly the sort of thing a quest system would use.
        //   3. the root component's `bHiddenInGame`, then `bVisible` (inverted) - if the
        //      game hides the mesh rather than the actor.
        //
        // DELIBERATELY NOT `bPerformanceHidden`. It is on almost every actor in this game
        // and it is the LOD / distance hide (it is the one property that differed between
        // three lit shrines - `lessons.md`), so believing it would delete the marker for
        // every NPC who is merely far away. Its value IS logged in the route line, so if
        // the next run shows the other four flat and this one moving, that is the answer
        // and it is one line of code away.
        //
        // The winning route is cached per UClass* and logged once, the same shape as the
        // health-component and item-name discoveries in this file.

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
        // route could be read at all - and as everywhere else in this file, "could not
        // read" is its own answer and never gets collapsed into a state.
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
        // WHERE THE NUMBER LIVES. Wuchang keeps a character's health in an
        // `ExtendedStatComponent_C` sub-object named `Health` on its AI CONTROLLER, not
        // on the pawn - `context/wuchang-classes.md` section 3 and every F8 world dump
        // list it as `Impl_BaseAIController_C_<n>.Health  Current=... Max=...`, with
        // `Current` / `Max` reflected floats. A component sub-object is a reflected
        // object property of the same name, so the whole read is
        // `controller -> Health -> Current`: three cached-offset raw reads, no
        // ProcessEvent, nothing that can re-enter the engine.
        //
        // WHY IT IS THE RIGHT SIGNAL. The dumps also contain the answer to "does a dead
        // enemy stay in the object array?" - `dump_20260902_102655_world.txt` line 33
        // has `Impl_BaseAIController_C_2147479402.Health Current=0.0 Max=100.0`
        // alongside five live siblings, i.e. a corpse the sweep would happily keep
        // drawing as a threat. That is the user's "dead enemies do not disappear".
        //
        // Returns FALSE when the answer is unknown (no such property, an unreadable
        // component, a nonsensical Max) - never "dead". A guess in that direction would
        // hide living enemies, which is strictly worse than the bug it fixes.
        //
        // WHAT WENT WRONG THE FIRST TIME (run 1, 2026-09-03): `health unknown` climbed to
        // 4 411 with `dead enemies dropped 0` and `bosses defeated 0` for a whole session
        // of killing things - so the route never answered once. The reason is in the
        // recon tool, not in the game: the dump line above was produced by
        // `find_all("ExtendedStatComponent_C")` and printed the component's FULL NAME, so
        // the `.Health` in `Impl_BaseAIController_C_<n>.Health` is the component's OBJECT
        // NAME (its outer path), and nothing in any dump ever said that the owning class
        // carries an `ObjectProperty` also called `Health`. `read_object_prop(controller,
        // "Health")` therefore returned nullptr on stage one, every time - the same
        // "a full name is not a property name" mistake as `APlayerCameraManager` having
        // no `GetViewTarget()`.
        //
        // So the component is DISCOVERED rather than named: try the obvious property
        // first, and when it is not there walk the class' own pointer-sized properties
        // once, capture each value through GUObjectArray (which is SEH-guarded and proves
        // the target is a live UObject before anything dereferences it) and accept the
        // one whose class is an `ExtendedStatComponent` - preferring the one whose object
        // name is `Health` when a character carries several stats. The winning property
        // NAME is cached per UClass*, so the walk happens once per class per world and
        // every read after that is the same three cached-offset raw reads as before.
        //
        // WHAT WENT WRONG THE SECOND TIME (run 2, 2026-09-03): stage one now SUCCEEDED -
        // `health component route on 'Impl_BaseAIController_C' is property 'Health' ->
        // Health (class ExtendedStatComponent_C)` - and the read failed one stage later,
        // at `the component has no float 'Current' / 'Max'`. Same root cause as the first
        // failure, one level down: `Current=` / `Max=` in the recon dump are the LUA
        // SCRIPT'S OWN LABELS. `WuchangRecon/Scripts/main.lua` reads
        // `read_prop_str(s, "CurrentValue")` and `read_prop_str(s, "MaxValue")` and prints
        // them as `Current=%s Max=%s`, so the dump never named a property at all. The
        // component's reflected floats are `CurrentValue` / `MaxValue`.
        //
        // Both spellings are tried, in that order, and the winning PAIR is cached per
        // component class - so a build that renames them costs one extra pair of missed
        // lookups per class instead of the whole feature.
        //
        // WHAT WENT WRONG THE THIRD TIME (run 3, 2026-09-03): the names were finally
        // right - and the read still failed, at the same stage, with
        // `health unknown 21624`. **The property is eight bytes wide.** In UE5 a
        // blueprint "float" is backed by `FDoubleProperty` (Large World Coordinates), and
        // `CurrentValue` / `MaxValue` on an `ExtendedStatComponent_C` are authored in a
        // blueprint - so asking for them with `expect_size == sizeof(float)` returned
        // FALSE, which is indistinguishable from "there is no such property".
        //
        // The diagnostic could not have caught it either, and that is the more important
        // half: it listed the class' FOUR-BYTE properties, so the run-3 log's evidence
        // table reads `StartIntervalTime`, `EndIntervalTime`, `UCSSerializationIndex` -
        // three unrelated timers - and the two properties the table existed to find were
        // invisible in it. Both halves are fixed here: the read goes through
        // `uer::read_numeric_prop` (8-byte double preferred, 4-byte float accepted, and
        // the width that answered is logged), and the failure table lists every 4- AND
        // 8-byte property with the number it reads back.

        constexpr const wchar_t* kHealthProp = L"Health";
        constexpr const wchar_t* kStatComponentSubstr = L"ExtendedStatComponent";

        // The candidate spellings of the component's two reflected floats, best first.
        struct HealthFields
        {
            const wchar_t* current;
            const wchar_t* max;
        };
        constexpr HealthFields kHealthFields[] = {
            {L"CurrentValue", L"MaxValue"}, // the names main.lua actually reads
            {L"Current", L"Max"},           // what the dump's labels looked like
        };
        constexpr int kHealthFieldCount = static_cast<int>(std::size(kHealthFields));

        // UClass* -> the property name that reaches its health component; an empty
        // string means "walked, and there is none". Keyed on the class, dropped with
        // every other world-keyed cache in drop_caches().
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

        // ONE-SHOT DIAGNOSTIC. Printed the first time a health read fails, and never
        // again: which object, which class, which stage, and - because the next in-game
        // run has to be able to fix this without a third one - every pointer-sized
        // property on the class whose value is a live UObject, with its class and object
        // name. That table is the answer to "what is this component actually called".
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
            // THE NUMBER TABLE. When the component is in hand and only the field names
            // (or their WIDTH) are wrong, the answer is the list of its numeric
            // properties and what each one reads back - a `MaxValue` of 269.1 beside a
            // `CurrentValue` of 269.1 names the pair without another play session.
            //
            // BOTH widths, and the width is printed. Run 3's version of this table asked
            // for four-byte properties only and therefore printed three unrelated timers
            // while the two eight-byte doubles it existed to find were invisible in it -
            // in UE5 a blueprint "float" is a double. An 8-byte entry may of course be a
            // POINTER read as a double (~1e-317 or absurdly large); that is what the
            // width column and the reader's judgement are for.
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
            //    component - a same-named property of some other type would otherwise
            //    pin the wrong route for the rest of the session.
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
        // spells them with. The winning pair is cached per component class, and the
        // route is logged once so the next log says which spelling won.
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
        // WHY. An enemy drops a `BP_DropItem_C`. It is spawned while you play, so it has
        // no entry in `markers/chapter*.json` and no name - and the x-ray labelled it
        // `BP_PickupActor_C 1 m`, which is the CLASS SPEC's base class out of
        // `kClasses[]`: not the actor's class, not a name, and not something a player
        // should ever be shown.
        //
        // WHERE THE ID IS. The offline extractor already reads it out of the cooked
        // package: `BP_PickupActor_C` and its descendants serialise an inline `Items`
        // array of `int32 ID, int32 Amount` pairs, ids in the 10000..40000 band, and
        // `ids[0]` of the FIRST array is the item (`context/item-names-research.md`; the
        // designer labels embedded in the exports validate it 25/25 and 1046/1046 exports
        // yield a real DataTable row). At runtime that same array is a reflected
        // `TArray` UPROPERTY, so it is a 16-byte header read plus one int32.
        //
        // WHAT IS VALIDATED AND WHY IT HAS TO BE. Neither the element STRIDE nor the
        // position of `ID` inside the element can be checked offline - the element is a
        // blueprint struct and could carry more members - so the answer is accepted only
        // when it is an id `markers/items.json` actually knows. That is the same
        // self-validating-signature rule the navmesh dumper's `dtNavMeshParams` scan and
        // the offline `ITEM_ARRAY_INDEX` table were both written under: never hard-code
        // an ordinal that a witness in the data can confirm instead.
        //
        // The candidate arrays, in the order the extractor found them meaningful. The
        // first one whose leading int32 is a known item id wins, and the winning property
        // is cached per class.
        constexpr const wchar_t* kItemArrayProps[] = {
            L"Items",                    // the one BP_PickupActor_C writes (1044/1046)
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
        // actor; the class-level route is memoised too and logged once.
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
        // The offline extractor de-duplicates a shrine id that the game itself reuses
        // by suffixing `@<level>/<obj>` (two ChapterDLC shrines genuinely share
        // `LiuHKK01`), so the marker id is not always the game's id - everything up to
        // the '@' is. shr::is_unlocked() is case-insensitive, which it has to be: the
        // save spells ids as the designers typed them (`Task1` next to `digong01`).
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
                // A BOSS is possessed by an ordinary Impl_BaseAIController_C too, so
                // without this the same pawn would be written to g_live twice in one
                // round - once as Cat::Enemy through the controller and once as
                // Cat::Boss through its own class - and which one survived would depend
                // on the order the object array happened to be walked in. Whenever the
                // pawn is itself a marker class, its own entry is the authoritative one.
                if (spec_for_class(actor) >= 0)
                {
                    return;
                }
                // DEAD ENEMIES MUST NOT BE DRAWN. The corpse keeps its controller and
                // its position until a GC (see controller_says_dead), so absence from
                // the sweep is not the signal - health is. An entry already in g_live
                // is erased outright rather than left to age out, so the marker is gone
                // on the very next publish.
                bool answered = false;
                bool dead = controller_says_dead(obj, answered);
                if (!answered)
                {
                    // The stat component was only ever SEEN on the controller; it is
                    // asked of the pawn too so a build that hangs it off the character
                    // still answers instead of counting into `health unknown` forever.
                    dead = controller_says_dead(actor, answered);
                }
                if (answered && dead)
                {
                    // MARKED DEAD, NOT ERASED. See LiveEntry::dead: an enemy's spawn
                    // point is a static marker, so erasing the live entry brings the
                    // static one back at the spawn point instead of clearing the map.
                    // The entry is refreshed every round the corpse is still in the
                    // object array and ages out with everything else once a GC takes it
                    // (at which point a respawned enemy's spawn hint is right again).
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
                // (0,0,0) is not a position here: it is where the level saver parks
                // a collected pickup. Keep the entry (its `found` still matters) but
                // never draw it there.
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
                // ANSWERED (0,0,0). Without the `pos_read` half, every pickup whose
                // RootComponent read failed for any reason was marked collected - and
                // the mark is PERSISTED, so it never came back. That is the user's
                // "a pickup lying on the ground right in front of me is not in the
                // x-ray": `highlight_show_found` hides collected loot, correctly, from
                // something that was never collected.
                if (e.pos_read && !e.pos_valid)
                {
                    e.found = true;
                }
                // The item's own name, for the case that has none in the static DB: loot
                // an enemy dropped. Memoised per actor, so this is one hash lookup per
                // pickup per round after the first sight of it.
                e.label = resolve_item_name(actor);
                break;
            }
            case Rule::Proximity:
            {
                // IS THE PERSON ACTUALLY THERE? A used-up NPC in this game keeps its
                // actor, its position and its id and is simply made INVISIBLE (see
                // actor_is_invisible - run 4's census is the proof). So the visibility
                // read comes first, and it does two things: it hides the marker at the
                // publish point (case (e) of mdb::mobile_twin_is_stale) and it stops
                // `met` firing for somebody you cannot walk up to.
                //
                // Only for the MOBILE categories, i.e. `npc`. A note (`DKDC_NPC_C`) is a
                // thing on a wall whose blueprint references no character mesh at all, so
                // whatever its visibility flags say is not evidence about a person, and
                // believing them could silently un-meet all 76 of them. The one rule, one
                // meaning: the same predicate decides drawing and meeting.
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
                // Defeated = zero health. The dumps only ever showed the stat component
                // on the AI CONTROLLER, so `APawn::Controller` is the first hop - but a
                // dead boss can have been unpossessed by the time we look, and nothing
                // proves the component cannot also hang off the character, so the pawn
                // itself is asked as well. Both answers come from the same discovered
                // route, so neither costs a second walk.
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
            // NOTE: enemies are NOT namespaced. The offline extractor writes an
            // enemy's spawn point into the static DB under the same
            // <level>/<object name> id, and giving the live one a prefix of its own
            // would draw the spawn point and the live pawn as two markers. What
            // keeps enemies out of the collection tracker is `persist == false`,
            // not the id.

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
                // A live-only entry (an enemy) gets the shortest possible debounce:
                // it has no static twin to fall back on, so keeping a stale one draws
                // a threat that is not there. Persisted categories keep the configured
                // grace, which is what stops a chest flickering when a sweep races
                // level streaming.
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

                // A gauge, not a total: it is "how many are hidden right now".
                g_mobile_hidden.store(0, std::memory_order_relaxed);
                // The per-round censuses, all gauges. `shrines lit` used to be a
                // count of NEW marks, which reads as 0 for every session after the
                // first - the shrines were already in the found set, so the else-if
                // that increments it never ran. See the g_shrine_lit_marks comment.
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
                // Read once per round rather than per marker: mm::cfg_cached() is a
                // thread_local copy behind a generation counter, but this is one
                // question and asking it 3 601 times is 3 601 branches.
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
                    // A shrine has no per-actor activation flag (that backlog item is
                    // closed - `context/saveslot-and-teleport-research.md` 2.3): the
                    // state is the game mode's `UnlockedFirepoints` list of shrine ids,
                    // which src/shrines.cpp already reads at 1 Hz. Before this the
                    // stats page's "Shrines lit" line used it but the found SET never
                    // did, so the per-category table said 0/12 with 18 shrines lit
                    // in-game and every lit shrine still drew as un-found on the map.
                    //
                    // Pushing the ids into the found set (rather than deriving the flag
                    // at draw time) is what makes the map glyph, the minimap styling,
                    // the legend counts and the stats table agree - they all read the
                    // one found set. It is idempotent: the same id is only ever added.
                    else if (sm.cat == mdb::Cat::Shrine && shrine_is_lit(sm.id))
                    {
                        d.flags |= kFlagFound;
                        g_found_static[idx] = 1;
                        note_found(sm.id);
                    }
                    // ---- THE SHRINE CENSUS IS A GAUGE, NOT A COUNT OF NEW MARKS ----
                    //
                    // `shrines lit` was incremented inside the else-if above, i.e. only
                    // when a shrine was marked for the FIRST time. The found file is
                    // persistent, so on the second and every later session every lit
                    // shrine is already in the found set, the else-if never runs, and
                    // the diagnostic reads `shrines lit 0` beside `shrines: 19 unlocked`
                    // in the same log - which is what a broken join looks like. Counted
                    // here instead, over the chapter's shrines, whichever branch marked
                    // them: `lit N of M` answers "did the join work" directly.
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
                    // `Rule::BossPawn` needs the boss ACTOR to exist and to be dead, and
                    // a boss the player has already killed never spawns again - so for
                    // every boss cleared before the mod existed that rule can never
                    // fire. The save can speak for them: the boss arena's own
                    // `bossdoor_*` respawn point is in `UnlockedFirepoints`, and
                    // `markers/chapter*.json` carries that id per boss marker
                    // (tools/markers/build_bossdoors.py, out of the game's own level
                    // scripts - the mapping is authored, not inferred).
                    //
                    // DERIVED, NEVER PERSISTED. Unlike the shrine rule this does NOT
                    // push the id into the found set: it is not certain that the game
                    // unlocks a boss door on the KILL rather than on the first attempt
                    // (context/boss-defeat-from-save.md), and a persisted mark from an
                    // uncertain signal survives fixing the rule - which is exactly the
                    // mess H4 left in wuchang_minimap_found_<slot>.txt. Recomputing it
                    // costs one 40-char list lookup per boss marker, nine per chapter.
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

                    // ---- A CORPSE HIDES ITS SPAWN POINT TOO -----------------------
                    //
                    // An enemy's authored position is in the static DB as well, so
                    // dropping only the live entry redrew the dead enemy at its spawn
                    // point. Whatever the health read declared dead is not on the map
                    // at all until the corpse is collected and the entry ages out.
                    if (mdb::static_twin_is_hidden_by_corpse(live != nullptr,
                                                             live != nullptr && live->dead))
                    {
                        ++dead_hidden;
                        continue;
                    }

                    // ---- "LIVE" MEANS AN ACTOR ANSWERED THIS ROUND, WITH A POSITION -
                    //
                    // For a category that does not move, a twin from a round or two ago
                    // is as good as this round's - that debounce is what stops a chest
                    // flickering when the sweep races level streaming. For a category
                    // that DOES move it is not: a stale entry, or one whose position
                    // read failed (`pos_valid == false` leaves the AUTHORED position in
                    // place), used to be published with kFlagLive set - and kFlagLive is
                    // exactly what the x-ray takes as permission to draw a person
                    // through a wall. That is how "NPC 2 m (found)" survived at a spot
                    // the NPC had left even after the static hint itself was dropped.
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
                    // NPCs move: talk to a quest NPC and it relocates,
                    // usually to a different placed actor in a different sublevel and so
                    // under a different marker id. The authored position then has no live
                    // twin and never will, and the x-ray happily labelled it "NPC 2 m
                    // (found)" at a spot the NPC had left. When the marker's own level is
                    // loaded and a full round has finished since it loaded, "no live actor
                    // answered" is not "I have not looked yet" - it is proof, and the hint
                    // is dropped from the published set (so the minimap, the full map, the
                    // compass and the x-ray all agree in one place). The met state is
                    // untouched: it lives in the found set, not in this entry.
                    const int mli = db->marker_level[idx];
                    mdb::MobileTwinFacts mob{};
                    mob.mobile = mdb::is_mobile_category(sm.cat);
                    // A twin that answered but could not be located is NOT an answer -
                    // it leaves the authored position on the entry, which is the thing
                    // this rule exists to stop being drawn.
                    mob.live_twin_this_round = live != nullptr && live->round == g_round && live->pos_valid;
                    // ...but it IS an answer of its own: an actor answering for this id
                    // proves its level is loaded, and this game parks a used-up actor at
                    // (0,0,0). So "answered, unlocatable" is the walked-away case, and it
                    // does not need the level table - which is what made the rule fire at
                    // all. Run 3 could only name 21 of 53 people's levels as resident, so
                    // for the other 32 the level-based clause could never be reached.
                    mob.live_twin_unlocatable =
                        live != nullptr && live->round == g_round && !live->pos_valid;
                    // Case (e): the actor is standing right there and is invisible. This
                    // is the case run 4 proved is the real one - `joined 21, superseded 0,
                    // walked away 0, hidden 0` with the user still seeing the marker.
                    mob.live_twin_invisible =
                        live != nullptr && live->round == g_round && live->invisible;
                    if (mli >= 0 && g_level_known[static_cast<std::size_t>(mli)] != 0)
                    {
                        mob.level_known = true;
                        mob.full_round_since_level_load =
                            g_round > g_level_round[static_cast<std::size_t>(mli)];
                    }
                    // The per-round census behind the `people -` log line. It is what
                    // tells the next in-game run WHICH half of the join is failing:
                    // a static count with no joins means the ids do not match, joins
                    // with no supersedes means nobody has moved, and level_known 0
                    // means the hide rule can never fire whatever else is true.
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
                        // flags are genuinely all false and the mechanism is a different
                        // one. That is the difference a single run has to be able to tell.
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
                    // ---- THE `met` GAUGE -------------------------------------------
                    //
                    // `met` used to be a count of NEW marks, which is zero on every
                    // session after the one that discovered them - the same lie
                    // `shrines lit 0` told, and it read `met 0` in run 3 after reading
                    // 19 in run 1 with nothing broken in between. Counted here as a
                    // gauge over the chapter's people and notes instead.
                    if (sm.cat == mdb::Cat::Npc || sm.cat == mdb::Cat::Note)
                    {
                        ++met_total;
                        if ((d.flags & kFlagFound) != 0)
                        {
                            ++met_found;
                        }
                    }
                    // The boss gauge, counted HERE - after the found file, the save
                    // door and this round's live health read have all had their say.
                    // `boss_total` is incremented earlier because a boss that a later
                    // `continue` drops from the buffer still exists on the map.
                    if (sm.cat == mdb::Cat::Boss && (d.flags & kFlagFound) != 0)
                    {
                        ++boss_found;
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
                    facts.level_known = mob.level_known;
                    facts.full_round_since_level_load = mob.full_round_since_level_load;
                    // AN ACTOR THAT ANSWERED IS PRESENT, wherever it is standing. The
                    // `pos_valid` requirement that used to be here meant a chest whose
                    // position read failed looked ABSENT to the debounce, and the absence
                    // rule then auto-marked it collected after two rounds - the second
                    // half of "static loot silently disappears from the x-ray". An actor
                    // parked at (0,0,0) does not reopen the hole: the pickup rule has
                    // already set `found` on it, and `!live->found` covers that.
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
                // THE LABEL OF A LIVE-ONLY MARKER IS NEVER ITS CLASS NAME. It used to be
                // `narrow_ascii(kv.second.cls)`, i.e. the CLASS SPEC's name out of
                // kClasses[] - so an enemy's dropped loot read `BP_PickupActor_C 1 m`
                // through the wall (and that is not even its class: the actor is a
                // `BP_DropItem_C`). A resolved item name is used when there is one;
                // otherwise the label is left EMPTY and every drawing site turns that
                // into the category's plain word through mdb::display_label().
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
                // EVERY DERIVED VIEW READS THE SAME RULES. The save-backed boss defeat
                // is deliberately not in the found FILE (see the publish-time comment),
                // so this table has to ask the same question the publish point does -
                // otherwise the map draws a boss hollow while the statistics say 0 of 9,
                // which is precisely the disagreement the shrine rule had to be fixed
                // for (lessons.md: a state the mod reads is not a state the mod uses).
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
            Guard guard(g_stats_lock);
            g_stats = s;
        }

        // markers/items.json -> {item id -> display name}, for the loot an enemy DROPS.
        // A runtime-spawned `BP_DropItem_C` has no static twin and therefore no name of
        // its own; its item id is readable off the actor (see resolve_item_name), and this
        // is the other half of the join. Missing file = no runtime item names, which is a
        // worse label and not an error: the file is a toolchain artifact and package.ps1
        // only ships it when it has been built.
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
                // A manifest still spelling a renamed category the old way parses fine
                // (mdb::cat_from_legacy_name), but it means the `markers/` folder is
                // older than the DLL - which is worth saying ONCE, not once per file.
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
            load_item_names(dir);
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
                // ONE LINE PER 30 s, NOT PER SAVE. The write is debounced already, but a
                // player looting a room still triggers one every few seconds (52 lines in
                // run 5). The line that survives carries everything the suppressed ones
                // would have said: the total, how many marks were added since it was last
                // printed, and how many saves it stands for.
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
        shr::load_table();
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
                // VERBOSE, and COUNTED. Auto-marks arrive in ones and twos as the
                // player walks (55 lines in run 5) and every one of them is followed
                // within a second by the save line below, which is the thing that
                // actually changed persisted state - so the count rides on that line
                // instead of getting one of its own.
                g_marks_since_log += static_cast<std::uint32_t>(added);
                MM_LOGV(L"markers: auto-marked {} new marker(s) as found ({} total)", added,
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
                g_marks_since_log += static_cast<std::uint32_t>(changed);
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
        // THE PERIODIC HEALTH SUMMARY. At `normal` this is once a minute and it is the
        // only recurring thing the mod writes: the found-rule counters and the NPC census
        // are what a bug report needs to see moving. The per-round TIMINGS below are the
        // running commentary and are `verbose`. Run 5 wrote 240 lines here; at `normal`
        // the same session writes 82.
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
                // The 2026-09-03 found-rules, in the same log a session reports. All
                // five together answer "which rule fired and which one cannot read its
                // property": a climbing `health unknown` with zero dead/defeated means
                // the Health component route is wrong on this build.
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
                // THE NPC CENSUS, one line, so the next run pins which half
                // of the join fails. static = markers of that category in the
                // chapter; live = live entries held; joined = static markers a live
                // actor answered for THIS round with a usable position; superseded =
                // those standing more than 3 m from where they were authored (i.e. the
                // person has walked); level = static markers whose own sublevel is
                // resident (the level-based clause cannot fire below that); walked away =
                // a live actor answered for the id but could NOT be located, which is
                // this game's normal state for a person who has moved on; and the two
                // halves of `hidden` say which clause did it. `hidden walked` climbing
                // while `level resident` stays low is the case run 3 could not express.
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
        shr::drop_caches();
        g_layouts.clear();
        g_class_spec.clear();
        g_health_prop.clear();   // the discovered route is keyed to a UClass* of that world
        g_health_fields.clear(); // ditto: the field spelling is cached per component class
        g_hidden_route.clear();  // ditto for the visibility route (keyed per UClass*)
        g_drop_name.clear();     // keyed on the ACTOR: a recycled allocation must not
        g_item_prop.clear();     // hand a new drop the old one's item name
        g_id_cache.clear();
        g_live.clear();
        // Both are keyed to the world that just went: a level name means nothing in the
        // next one, and a half-finished absence streak must not survive a load.
        g_levels.clear();
        std::fill(g_absent_streak_idx.begin(), g_absent_streak_idx.end(), 0);
        std::fill(g_live_of_static.begin(), g_live_of_static.end(), nullptr);
        std::fill(g_level_known.begin(), g_level_known.end(), static_cast<std::uint8_t>(0));
        g_subset_valid = false; // the chapter is re-detected in the next world
        g_player_ok = false;    // the snapshot belongs to the world that just went
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
        // ---- HOOK: the shrine unlock state (src/shrines.cpp) ------------------------
        //
        // Four raw property reads at 1 Hz once the component is found. It is what the
        // stats page means by "shrines lit" and what the shrine list uses to decide
        // whether travelling to a shrine may even be offered.
        shr::game_thread_pump(now);
        // ---- HOOK: the one-press recon dump (src/recon.cpp) -------------------------
        //
        // One atomic load unless the dump was asked for. It needs the game thread and
        // the pawn's world, both of which this pump already has.
        recon::game_thread_pump(world);
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

        // 4b. The player's position for Rule::Proximity, once per slice. A seqlock
        //     read of a POD struct that the game thread itself published a few
        //     milliseconds ago - not an engine call, and never per actor.
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

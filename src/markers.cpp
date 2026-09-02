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

#include "mapdata.hpp"
#include "mem.hpp"
#include "mmstate.hpp"
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
        // One FindAllOf call per game-thread pump, cycling through this table, so a
        // full round costs exactly kClassCount object-array walks spread over ~1 s.
        // That is the same order of cost gamestate already pays for its 4 Hz
        // FindAllOf("UserWidget") sweep, and it keeps the per-pump peak at one walk
        // instead of nine.
        //
        // FindAllOf matches subclasses too (lessons.md: FindAllOf("Actor") returns
        // 2173 objects of which only 1019 have the bare `Actor` class), so
        // BP_PickupActor_C also brings in BP_DropItem_C - the runtime-spawned enemy
        // loot drop whose collect was watched end-to-end in run 3. Every returned
        // object is still re-classified by walking its class' super chain by name, so
        // a subclass instance lands in the right category regardless of which entry
        // found it.

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
        std::atomic<double> g_sweep_ms{0.0};
        std::atomic<double> g_sweep_ms_peak{0.0};

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
            mdb::Cat cat = mdb::Cat::Other;
            bool found = false;
            bool persist = false;
            bool pos_valid = false;
            std::uint64_t round = 0;
        };

        std::unordered_map<std::string, LiveEntry> g_live;

        const void* g_world = nullptr;
        int g_next_class = 0;
        std::uint64_t g_last_class_ms = 0;
        std::uint64_t g_round = 0;

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
            if (g_class_spec.size() > 8192)
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
            if (g_id_cache.size() > 8192)
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
        }

        //==============================================================================
        // One class sweep (game thread)
        //==============================================================================

        void sweep_class(int index)
        {
            const ClassSpec& spec = kClasses[index];
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(spec.name, found);
            if (found.empty())
            {
                return;
            }
            constexpr std::size_t kMaxPerClass = 4096;
            const std::size_t count = found.size() < kMaxPerClass ? found.size() : kMaxPerClass;

            for (std::size_t i = 0; i < count; ++i)
            {
                UObject* obj = found[i];
                if (obj == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(obj) || !mem::readable(obj, 0x40))
                {
                    continue;
                }
                const int si = spec_for_class(obj);
                const ClassSpec& s = si >= 0 ? kClasses[si] : spec;

                // The marker actor. For an AI controller it is the pawn it possesses.
                UObject* actor = obj;
                if (s.rule == Rule::ControllerPawn)
                {
                    const uer::ClassLayout* layout = g_layouts.get(obj);
                    actor = uer::read_object_prop(layout, obj, L"Pawn");
                    if (actor == nullptr)
                    {
                        continue;
                    }
                }

                LiveEntry e{};
                e.cat = s.cat;
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
                    continue;
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
                if (g_live.size() < 8192 || g_live.contains(id))
                {
                    g_live[id] = e;
                }
            }
        }

        //==============================================================================
        // Publishing the draw buffer (game thread, once per round)
        //==============================================================================

        void publish_round()
        {
            // Drop live actors that have not answered for two rounds: their level was
            // unloaded, or (for an enemy) they died. Absence is NEVER treated as
            // "collected" - only the state flags do that.
            for (auto it = g_live.begin(); it != g_live.end();)
            {
                if (g_round >= 2 && it->second.round + 2 <= g_round)
                {
                    it = g_live.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            std::vector<DrawMarker>& dst = g_slot[g_slot_next];
            dst.clear();

            const StaticDb* db = g_db.load(std::memory_order_acquire);
            if (db != nullptr)
            {
                dst.reserve(db->markers.size() + g_live.size());
                for (const mdb::StaticMarker& sm : db->markers)
                {
                    DrawMarker d{};
                    d.x = sm.x;
                    d.y = sm.y;
                    d.z = sm.z;
                    d.cat = static_cast<std::uint8_t>(sm.cat);
                    d.flags = kFlagStatic;
                    if (g_found_gt.contains(sm.id))
                    {
                        d.flags |= kFlagFound;
                    }
                    const auto live = g_live.find(sm.id);
                    if (live != g_live.end())
                    {
                        d.flags |= kFlagLive;
                        if (live->second.pos_valid)
                        {
                            d.x = live->second.x;
                            d.y = live->second.y;
                            d.z = live->second.z;
                        }
                        if (live->second.found)
                        {
                            d.flags |= kFlagFound;
                        }
                    }
                    copy_id(d.id, sizeof(d.id), sm.id);
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
                if (db != nullptr && db->by_id.contains(kv.first))
                {
                    continue;
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
                dst.push_back(d);
            }

            g_published_count.store(static_cast<int>(dst.size()), std::memory_order_relaxed);
            g_live_count.store(static_cast<int>(g_live.size()), std::memory_order_relaxed);
            g_slot_published.store(g_slot_next, std::memory_order_release);
            g_slot_next = (g_slot_next + 1) % kSlots;
        }

        //==============================================================================
        // Loading (loop thread)
        //==============================================================================

        std::wstring markers_dir()
        {
            return mm::mod_dir() + L"\\markers";
        }

        std::wstring found_path()
        {
            return mm::mod_dir() + L"\\wuchang_minimap_found.txt";
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
                    if (m.chapter >= 1 && m.chapter <= 8)
                    {
                        chapters.insert(m.chapter);
                        ++s.chapter[m.chapter][ci].total;
                        s.chapter[m.chapter][ci].found += found ? 1 : 0;
                    }
                }
                s.chapters_loaded = static_cast<int>(chapters.size());
            }
            s.found_ids = static_cast<int>(g_found_master.size());
            s.published = g_published_count.load(std::memory_order_relaxed);
            s.live_entries = g_live_count.load(std::memory_order_relaxed);
            s.rounds = g_rounds.load(std::memory_order_relaxed);
            s.sweep_ms = g_sweep_ms.load(std::memory_order_relaxed);
            s.sweep_ms_peak = g_sweep_ms_peak.load(std::memory_order_relaxed);
            Guard guard(g_stats_lock);
            g_stats = s;
        }

        void load_static_db()
        {
            const std::wstring dir = markers_dir();

            // The chapter keys maps.json already named, plus chapter1..8 so the marker
            // DB can arrive before (or without) a map asset.
            std::vector<std::string> keys;
            for (const mapdata::Chapter& c : mapdata::chapters())
            {
                if (!c.key.empty())
                {
                    keys.push_back(c.key);
                }
            }
            for (int i = 1; i <= 8; ++i)
            {
                keys.push_back("chapter" + std::to_string(i));
            }

            auto db = std::make_unique<StaticDb>();
            std::vector<std::string> seen;
            int files = 0;
            for (const std::string& key : keys)
            {
                if (std::find(seen.begin(), seen.end(), key) != seen.end())
                {
                    continue;
                }
                seen.push_back(key);
                const std::wstring path = dir + L"\\" + widen(key) + L".json";
                std::string text;
                if (!read_whole_file(path, text))
                {
                    continue;
                }
                mdb::ParseReport report{};
                const std::size_t before = db->markers.size();
                if (!mdb::parse_markers_json(text, db->markers, report))
                {
                    mm::logf(L"markers: {} rejected - {}", path, widen(report.error));
                    db->markers.resize(before);
                    continue;
                }
                ++files;
                mm::logf(L"markers: {} -> {} marker(s) (chapter {}, schema {}{}{})",
                         path,
                         report.added,
                         report.chapter,
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

    void on_unreal_init()
    {
        load_static_db();
        load_found_file();
        recompute_stats();
        const mm::Config cfg = mm::config();
        mm::logf(L"markers: {} ({} live class sweep, {} found tracker); categories = {}",
                 cfg.markers_enabled ? L"enabled" : L"DISABLED",
                 cfg.markers_live ? L"with" : L"without",
                 cfg.found_tracker ? L"with" : L"without",
                 widen(mdb::format_category_mask(cfg.markers_categories)));
    }

    void reload()
    {
        load_static_db();
        load_found_file();
        recompute_stats();
    }

    void on_update()
    {
        const std::uint64_t now = ::GetTickCount64();

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

        const mm::Config cfg = mm::config();
        if (g_found_dirty && cfg.found_tracker &&
            now - g_found_dirty_ms >= static_cast<std::uint64_t>(cfg.found_save_debounce_ms))
        {
            save_found_file();
        }

        // Cheap counters the panel shows; recomputing the whole per-chapter table is
        // only worth it when the found set changed, which is handled above.
        static std::uint64_t last_light = 0;
        if (now - last_light >= 1000)
        {
            last_light = now;
            Guard guard(g_stats_lock);
            g_stats.published = g_published_count.load(std::memory_order_relaxed);
            g_stats.live_entries = g_live_count.load(std::memory_order_relaxed);
            g_stats.rounds = g_rounds.load(std::memory_order_relaxed);
            g_stats.sweep_ms = g_sweep_ms.load(std::memory_order_relaxed);
            g_stats.sweep_ms_peak = g_sweep_ms_peak.load(std::memory_order_relaxed);
        }
    }

    void drop_caches()
    {
        g_layouts.clear();
        g_class_spec.clear();
        g_id_cache.clear();
        g_live.clear();
        g_world = nullptr;
        g_next_class = 0;
        g_live_count.store(0, std::memory_order_relaxed);
    }

    void game_thread_pump(std::uint64_t now, const void* world)
    {
        const mm::Config cfg = mm::config();
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

        // One FindAllOf per interval, cycling through the class table: a whole round
        // costs kClassCount object-array walks and finishes in
        // 1 / markers_rounds_per_sec seconds. Spreading it keeps the per-pump peak at
        // one walk - a single 9-walk burst once a second would be a visible hitch.
        const int rounds_per_sec = cfg.markers_rounds_per_sec < 1 ? 1 : cfg.markers_rounds_per_sec;
        const std::uint64_t interval =
            static_cast<std::uint64_t>(1000 / (rounds_per_sec * kClassCount) + 1);
        if (now - g_last_class_ms < interval)
        {
            return;
        }
        g_last_class_ms = now;

        const int index = g_next_class;
        LARGE_INTEGER freq{};
        LARGE_INTEGER t0{};
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&t0);

        sweep_class(index);

        LARGE_INTEGER t1{};
        ::QueryPerformanceCounter(&t1);
        if (freq.QuadPart > 0)
        {
            const double ms =
                static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
            g_sweep_ms.store(ms, std::memory_order_relaxed);
            if (ms > g_sweep_ms_peak.load(std::memory_order_relaxed))
            {
                g_sweep_ms_peak.store(ms, std::memory_order_relaxed);
            }
        }

        g_next_class = index + 1;
        if (g_next_class >= kClassCount)
        {
            g_next_class = 0;
            publish_round();
            ++g_round;
            g_rounds.store(g_round, std::memory_order_relaxed);
        }
    }
} // namespace markers

//
// gamebinds - reads EnhancedActionMappings off the live PlayerInput. See gamebinds.hpp
// for the chain, the traps and the thread split.
//

#include "gamebinds.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

#include "gamebinds_map.hpp"
#include "mem.hpp"
#include "mmstate.hpp"
#include "spinlock.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

// The virtual keys in the PURE table are numbers so the tests can link it. This is where
// they meet the real macros.
static_assert(gb::vk_for_key("SpaceBar") == VK_SPACE, "SpaceBar");
static_assert(gb::vk_for_key("NumPadFour") == VK_NUMPAD4, "NumPadFour");
static_assert(gb::vk_for_key("LeftShift") == VK_LSHIFT, "LeftShift");
static_assert(gb::vk_for_key("LeftControl") == VK_LCONTROL, "LeftControl");
static_assert(gb::vk_for_key("LeftAlt") == VK_LMENU, "LeftAlt");
static_assert(gb::vk_for_key("Escape") == VK_ESCAPE, "Escape");
static_assert(gb::vk_for_key("Tab") == VK_TAB, "Tab");
static_assert(gb::vk_for_key("Enter") == VK_RETURN, "Enter");
static_assert(gb::vk_for_key("BackSpace") == VK_BACK, "BackSpace");
static_assert(gb::vk_for_key("F12") == VK_F12, "F12");
static_assert(gb::vk_for_key("One") == '1', "One");
static_assert(gb::vk_for_key("W") == 'W', "W");
static_assert(gb::vk_for_key("MiddleMouseButton") == VK_MBUTTON, "MiddleMouseButton");
static_assert(gb::vk_for_key("ThumbMouseButton") == VK_XBUTTON1, "ThumbMouseButton");
static_assert(gb::vk_for_key("PageDown") == VK_NEXT, "PageDown");
static_assert(gb::vk_matches(VK_SHIFT, VK_LSHIFT), "SHIFT is either side");
static_assert(gb::vk_matches(VK_RCONTROL, VK_CONTROL), "CTRL is either side");
static_assert(!gb::vk_matches(VK_SHIFT, VK_LCONTROL), "different keys");
static_assert(gb::vk_for_key("Gamepad_FaceButton_Bottom") == 0, "a pad button is not a key");
static_assert(gb::vk_for_key("MouseScrollUp") == 0, "the wheel is not a key");

namespace gb
{
    namespace
    {
        using RC::Unreal::FProperty;
        using RC::Unreal::UObject;
        using RC::Unreal::UStruct;
        namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

        constexpr const wchar_t* kMappingStruct = L"EnhancedActionKeyMapping";
        constexpr const wchar_t* kKeyStruct = L"Key";

        spin::Spinlock g_lock;
        Table g_table; // guarded by g_lock
        std::atomic<std::uint32_t> g_gen{0};

        uer::LayoutCache g_layouts;
        uer::ObjRef g_input; // the UEnhancedPlayerInput sub-object
        std::uint64_t g_last_poll = 0;
        std::uint64_t g_last_find = 0;
        int g_last_logged_rows = -1;

        // Everything derived by reflection, resolved once per PlayerInput object.
        struct Layout
        {
            bool ok = false;
            bool tried = false; // a failed resolve logs once, then stays quiet
            std::size_t array_off = 0;
            int stride = 0;
            std::size_t action_off = 0;
            std::size_t key_off = 0;
            std::size_t keyname_off = 0;
        };
        Layout g_layout;

        // Neither an FName nor an InputAction asset changes its text while the world
        // stands, so the 1 Hz poll allocates nothing once these are warm.
        std::unordered_map<std::uint64_t, std::string> g_key_text;
        std::unordered_map<const void*, std::string> g_action_text;

        void copy_into(char* dst, std::size_t cap, const std::string& src)
        {
            ::strncpy_s(dst, cap, src.c_str(), _TRUNCATE);
        }

        std::wstring widen(const std::string& s)
        {
            std::wstring out;
            out.reserve(s.size());
            for (char c : s)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return out;
        }

        void set_unresolved(const char* why)
        {
            bool changed = false;
            {
                spin::SpinGuard guard(g_lock);
                changed = g_table.valid || std::strcmp(g_table.status, why) != 0;
                g_table = Table{};
                copy_into(g_table.status, sizeof(g_table.status), why);
            }
            if (changed)
            {
                g_gen.fetch_add(1, std::memory_order_release);
                g_last_logged_rows = -1;
            }
        }

        //=== guarded calls into the engine ==========================================

        struct FNameArgs
        {
            std::uint64_t value = 0;
            std::wstring out;
            bool ok = false;
        };

        void fname_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<FNameArgs*>(a);
            // ToString() runs on OUR copy of the eight bytes, so a name table lookup can
            // never race the game freeing the mapping array underneath it.
            args->out = reinterpret_cast<RC::Unreal::FName*>(&args->value)->ToString();
            args->ok = true;
        }

        struct ObjNameArgs
        {
            UObject* obj = nullptr;
            std::wstring out;
            bool ok = false;
        };

        void objname_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<ObjNameArgs*>(a);
            args->out = args->obj->GetName();
            args->ok = true;
        }

        // FArrayProperty::Inner, then FStructProperty::Struct: the route from a
        // TArray<FStruct> property to the struct's own reflection object. Both accessors
        // only compute a member address; the values are read through mem::read and the
        // answer is validated by name afterwards.
        struct StructOfArgs
        {
            FProperty* prop = nullptr;
            bool is_array = false; // false: `prop` is already the struct property
            void* script_struct = nullptr;
            bool ok = false;
        };

        void struct_of_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<StructOfArgs*>(a);
            FProperty* sp = args->prop;
            if (args->is_array)
            {
                FProperty*& iref = static_cast<RC::Unreal::FArrayProperty*>(sp)->GetInner();
                FProperty* inner = nullptr;
                if (!mem::read(&iref, inner) || !mem::plausible_ptr(inner) ||
                    !mem::readable(inner, 0x40))
                {
                    return;
                }
                sp = inner;
            }
            RC::Unreal::TObjectPtr<RC::Unreal::UScriptStruct>& sref =
                static_cast<RC::Unreal::FStructProperty*>(sp)->GetStruct();
            void* ss = nullptr;
            if (!mem::read(&sref, ss) || !mem::plausible_ptr(ss) || !mem::readable(ss, 0x40))
            {
                return;
            }
            args->script_struct = ss;
            args->ok = true;
        }

        struct SizeArgs
        {
            UStruct* strct = nullptr;
            int size = 0;
            bool ok = false;
        };

        void struct_size_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<SizeArgs*>(a);
            args->ok = mem::read(&args->strct->GetPropertiesSize(), args->size);
        }

        std::string object_name(UObject* obj)
        {
            if (obj == nullptr || !mem::plausible_ptr(obj) || !mem::readable(obj, 0x40))
            {
                return {};
            }
            ObjNameArgs args{};
            args.obj = obj;
            if (!mem::guarded_call(&objname_trampoline, &args, nullptr, nullptr) || !args.ok)
            {
                return {};
            }
            return uer::narrow_ascii(args.out);
        }

        std::string fname_text(std::uint64_t value)
        {
            const auto it = g_key_text.find(value);
            if (it != g_key_text.end())
            {
                return it->second;
            }
            FNameArgs args{};
            args.value = value;
            std::string text;
            if (mem::guarded_call(&fname_trampoline, &args, nullptr, nullptr) && args.ok)
            {
                text = uer::narrow_ascii(args.out);
            }
            if (g_key_text.size() < 512)
            {
                g_key_text.emplace(value, text);
            }
            return text;
        }

        std::string action_name(const void* obj)
        {
            const auto it = g_action_text.find(obj);
            if (it != g_action_text.end())
            {
                return it->second;
            }
            std::string text = object_name(static_cast<UObject*>(const_cast<void*>(obj)));
            if (g_action_text.size() < 512)
            {
                g_action_text.emplace(obj, text);
            }
            return text;
        }

        // The struct property's UScriptStruct, once it has named itself as `expect`.
        UStruct* resolve_script_struct(FProperty* prop, bool is_array, const wchar_t* expect)
        {
            if (prop == nullptr || !mem::readable(prop, 0x40))
            {
                return nullptr;
            }
            StructOfArgs args{};
            args.prop = prop;
            args.is_array = is_array;
            if (!mem::guarded_call(&struct_of_trampoline, &args, nullptr, nullptr) || !args.ok)
            {
                return nullptr;
            }
            auto* obj = static_cast<UObject*>(args.script_struct);
            uer::ObjRef ref{};
            if (!uer::capture(obj, ref))
            {
                return nullptr;
            }
            if (uer::narrow_ascii(obj->GetName()) != uer::narrow_ascii(expect))
            {
                return nullptr;
            }
            return static_cast<UStruct*>(obj);
        }

        // Offset and element size of one property of a walked struct. Returns false when
        // the name is absent.
        bool struct_prop(const uer::ClassLayout& layout, const wchar_t* name, std::size_t& off,
                         int& size)
        {
            const uer::Prop* p = uer::find_prop(&layout, name);
            if (p == nullptr)
            {
                return false;
            }
            off = p->offset;
            size = p->size;
            return true;
        }

        //=== resolution =============================================================

        UObject* find_player_input()
        {
            UObject* pc = UObjectGlobals::FindFirstOf(L"DCSPlayerController_C");
            if (pc == nullptr)
            {
                pc = UObjectGlobals::FindFirstOf(L"PlayerController");
            }
            if (pc != nullptr && uer::valid_for_find_xof(pc) &&
                mem::readable(pc, 0x40))
            {
                UObject* pi = uer::read_object_prop(g_layouts.get(pc), pc, L"PlayerInput");
                if (pi != nullptr)
                {
                    return pi;
                }
            }
            // The sub-object on its own, should a build rename the controller class.
            return UObjectGlobals::FindFirstOf(L"EnhancedPlayerInput");
        }

        // Everything the row walk needs, derived and validated once. Logs the whole chain
        // the first time it answers, and the reason exactly once when it does not.
        bool resolve_layout()
        {
            if (g_layout.ok)
            {
                return true;
            }
            const bool first = !g_layout.tried;
            g_layout.tried = true;
            const auto give_up = [&](const wchar_t* why) {
                if (first)
                {
                    mm::logf(L"gamebinds: the key bindings cannot be read - {}. The Keys tab "
                             L"falls back to its built-in list of likely game binds.",
                             std::wstring{why});
                }
                return false;
            };

            const uer::ClassLayout* cl = g_layouts.get(g_input.obj);
            const uer::Prop* arr = uer::find_prop(cl, L"EnhancedActionMappings");
            if (arr == nullptr || arr->size != 16 || arr->field == nullptr)
            {
                return give_up(L"this PlayerInput has no readable EnhancedActionMappings array "
                               L"(not an Enhanced Input build?)");
            }
            UStruct* mapping = resolve_script_struct(arr->field, true, kMappingStruct);
            if (mapping == nullptr)
            {
                return give_up(L"the array's element type is not FEnhancedActionKeyMapping");
            }
            SizeArgs sz{};
            sz.strct = mapping;
            if (!mem::guarded_call(&struct_size_trampoline, &sz, nullptr, nullptr) || !sz.ok ||
                sz.size < 16 || sz.size > 1024)
            {
                return give_up(L"FEnhancedActionKeyMapping reports an implausible size");
            }

            const uer::ClassLayout ml = uer::walk_struct(mapping);
            std::size_t action_off = 0;
            std::size_t key_off = 0;
            int action_size = 0;
            int key_size = 0;
            if (!struct_prop(ml, L"Action", action_off, action_size) ||
                !struct_prop(ml, L"Key", key_off, key_size))
            {
                return give_up(L"FEnhancedActionKeyMapping has no Action / Key property");
            }
            const uer::Prop* key_prop = uer::find_prop(&ml, L"Key");
            // FKey caches a TSharedPtr<FKeyDetails>, so only the FName inside it is read.
            std::size_t keyname_off = 0;
            int keyname_size = 0;
            UStruct* fkey = resolve_script_struct(key_prop->field, false, kKeyStruct);
            if (fkey != nullptr)
            {
                const uer::ClassLayout kl = uer::walk_struct(fkey);
                if (!struct_prop(kl, L"KeyName", keyname_off, keyname_size))
                {
                    fkey = nullptr;
                }
            }
            if (fkey == nullptr)
            {
                return give_up(L"FKey has no reflected KeyName property");
            }

            const std::size_t stride = static_cast<std::size_t>(sz.size);
            if (action_size != static_cast<int>(sizeof(void*)) || keyname_size != 8 ||
                action_off + sizeof(void*) > stride || key_off + static_cast<std::size_t>(key_size) > stride ||
                keyname_off + 8 > static_cast<std::size_t>(key_size))
            {
                return give_up(L"the derived offsets do not fit inside the element stride");
            }

            g_layout.ok = true;
            g_layout.array_off = arr->offset;
            g_layout.stride = sz.size;
            g_layout.action_off = action_off;
            g_layout.key_off = key_off;
            g_layout.keyname_off = keyname_off;
            mm::logf(L"gamebinds: {} -> EnhancedActionMappings at +0x{:X}; "
                     L"FEnhancedActionKeyMapping stride {} bytes, Action +0x{:X} ({} bytes), "
                     L"Key +0x{:X} ({} bytes), Key.KeyName +0x{:X}",
                     uer::class_name(g_input), arr->offset, sz.size, action_off, action_size,
                     key_off, key_size, keyname_off);
            return true;
        }

        struct ArrRaw
        {
            void* data = nullptr;
            std::int32_t num = 0;
            std::int32_t max = 0;
        };

        void publish(const Table& next)
        {
            bool changed = false;
            {
                spin::SpinGuard guard(g_lock);
                changed = g_table.valid != next.valid || g_table.rows != next.rows ||
                          g_table.unbound != next.unbound || g_table.truncated != next.truncated ||
                          std::memcmp(g_table.row, next.row,
                                      static_cast<std::size_t>(next.rows) * sizeof(Row)) != 0;
                if (changed)
                {
                    g_table = next;
                }
            }
            if (!changed)
            {
                return;
            }
            g_gen.fetch_add(1, std::memory_order_release);
            if (next.rows != g_last_logged_rows)
            {
                g_last_logged_rows = next.rows;
                mm::logf(L"gamebinds: {} bound key(s) read from EnhancedActionMappings, "
                         L"{} unbound row(s){}",
                         next.rows, next.unbound, next.truncated ? L" - TRUNCATED" : L"");
            }
            else
            {
                mm::log(L"gamebinds: the game's key bindings changed");
            }
            // Only on a change: at 1 Hz the whole table would otherwise fill the log.
            for (int i = 0; i < next.rows; ++i)
            {
                MM_LOGV(L"gamebinds: [{}] {} = {}", i, widen(std::string{next.row[i].action}),
                        widen(std::string{next.row[i].key}));
            }
        }

        void read_rows()
        {
            ArrRaw hdr{};
            if (!mem::read_at(g_input.obj, g_layout.array_off, hdr))
            {
                set_unresolved("EnhancedActionMappings could not be read");
                return;
            }
            const std::size_t stride = static_cast<std::size_t>(g_layout.stride);
            if (hdr.num < 0 || hdr.num > uer::kMaxArrayItems || hdr.max < hdr.num)
            {
                set_unresolved("EnhancedActionMappings has an implausible length");
                return;
            }
            if (hdr.num > 0 &&
                (!mem::plausible_ptr(hdr.data) ||
                 !mem::readable(hdr.data, static_cast<std::size_t>(hdr.num) * stride)))
            {
                set_unresolved("the EnhancedActionMappings buffer is not readable");
                return;
            }

            Table next{};
            next.valid = true;
            copy_into(next.status, sizeof(next.status), "PlayerInput.EnhancedActionMappings");
            const auto* base = static_cast<const std::uint8_t*>(hdr.data);
            for (std::int32_t i = 0; i < hdr.num; ++i)
            {
                const std::uint8_t* elem = base + static_cast<std::size_t>(i) * stride;
                std::uint64_t name_bits = 0;
                if (!mem::read_at(elem, g_layout.key_off + g_layout.keyname_off, name_bits))
                {
                    continue;
                }
                const std::string key = fname_text(name_bits);
                if (key.empty() || key == kNoneKey)
                {
                    ++next.unbound; // 18 of the 76 rows on this build are unbound slots
                    continue;
                }
                void* action = nullptr;
                if (!mem::read_at(elem, g_layout.action_off, action))
                {
                    continue;
                }
                const std::string act = action_name(action);
                if (next.rows >= kMaxRows)
                {
                    next.truncated = true;
                    break;
                }
                Row& row = next.row[next.rows++];
                copy_into(row.key, sizeof(row.key), key);
                copy_into(row.action, sizeof(row.action), act);
            }
            publish(next);
        }
    } // namespace

    std::uint32_t generation()
    {
        return g_gen.load(std::memory_order_acquire);
    }

    Table table()
    {
        spin::SpinGuard guard(g_lock);
        return g_table;
    }

    void drop_caches()
    {
        g_layouts.clear();
        g_input.reset();
        g_layout = Layout{};
        g_key_text.clear();
        g_action_text.clear();
        g_last_poll = 0;
        g_last_find = 0;
        set_unresolved("world changed - not read yet");
    }

    void game_thread_pump(std::uint64_t now)
    {
        // 1 Hz: a remap needs a trip through the options menu.
        if (g_last_poll != 0 && now - g_last_poll < 1000)
        {
            return;
        }
        g_last_poll = now;

        if (g_input.empty() || !uer::alive(g_input))
        {
            // The PlayerInput is recreated with the controller, so every offset derived
            // from the old one goes with it.
            g_input.reset();
            g_layout = Layout{};
            if (g_last_find != 0 && now - g_last_find < 5000)
            {
                return;
            }
            g_last_find = now;
            UObject* input = find_player_input();
            uer::ObjRef ref{};
            if (input == nullptr || !uer::capture(input, ref))
            {
                set_unresolved("no PlayerInput in the world");
                return;
            }
            g_input = ref;
            MM_LOGV(L"gamebinds: PlayerInput resolved ({})", uer::class_name(g_input));
        }

        if (!resolve_layout())
        {
            set_unresolved("the EnhancedActionMappings layout did not resolve");
            return;
        }
        read_rows();
    }
} // namespace gb

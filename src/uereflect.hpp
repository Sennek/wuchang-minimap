#pragma once

//
// uereflect - the slice of UE reflection the minimap needs, on top of ue_min.hpp.
// Game-thread only. Property offsets come from the FField child-property chain, cached per
// UClass*. Calls go through GetFunctionByNameInChain + ProcessEvent: a null UFunction means
// the function does not exist on this class (never evidence about game state), and a
// ProcessEvent from inside UE4SS's ProcessEvent pre-callback re-enters that callback, so the
// caller MUST hold a re-entrancy guard.

#include <Windows.h>

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mem.hpp"
#include "mmstate.hpp"
#include "ue_min.hpp"

namespace uer
{
    using RC::Unreal::FBoolProperty;
    using RC::Unreal::FField;
    using RC::Unreal::FFieldAccess;
    using RC::Unreal::FProperty;
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;
    using RC::Unreal::UStruct;

    struct Prop
    {
        std::size_t offset = 0;
        int size = 0;
        // The reflected FProperty, for a read needing more than offset and size - today only
        // the bool BITFIELD decode (bool_info). Never moved or freed while instances exist.
        FProperty* field = nullptr;
    };

    // Transparent hash: a lookup takes a std::wstring_view and allocates nothing. MSVC's
    // small-string buffer holds seven wchars, so L"RootComponent" would otherwise heap-allocate
    // on the GAME THREAD on every call. Only a MISS builds the owned key.
    struct WStrHash
    {
        using is_transparent = void;

        std::size_t operator()(std::wstring_view s) const noexcept
        {
            return std::hash<std::wstring_view>{}(s);
        }
        std::size_t operator()(const std::wstring& s) const noexcept
        {
            return std::hash<std::wstring_view>{}(std::wstring_view{s});
        }
    };

    using PropMap = std::unordered_map<std::wstring, Prop, WStrHash, std::equal_to<>>;

    struct ClassLayout
    {
        PropMap props;
        int walked = 0;
    };

    // Walks a UStruct and every super struct. A UClass, a UFunction and the UScriptStruct
    // behind a USTRUCT are all UStructs, so one walk serves all three. A pure-native
    // struct yields nothing, not an error.
    inline ClassLayout walk_struct(UStruct* start)
    {
        ClassLayout out{};
        if (start == nullptr)
        {
            return out;
        }
        UStruct* current = start;
        for (int depth = 0; current != nullptr && depth < 48; ++depth)
        {
            ++out.walked;
            FField* field = current->GetChildProperties();
            for (int i = 0; field != nullptr && i < 8192; ++i)
            {
                auto* prop = static_cast<FProperty*>(field);
                const int offset = prop->GetOffset_Internal();
                const int size = prop->GetElementSize();
                if (offset >= 0 && size > 0 && offset < 0x100000)
                {
                    std::wstring name = field->GetName();
                    if (!name.empty() && !out.props.contains(name))
                    {
                        out.props.emplace(std::move(name),
                                          Prop{static_cast<std::size_t>(offset), size, prop});
                    }
                }
                field = FFieldAccess::next(field);
            }
            current = current->GetSuperStruct();
        }
        return out;
    }

    inline ClassLayout walk_class(UClass* cls)
    {
        return walk_struct(static_cast<UStruct*>(cls));
    }

    // Per-UClass layout cache. Classes are never moved or freed while instances exist.
    class LayoutCache
    {
      public:
        const ClassLayout* get(UObject* obj)
        {
            if (obj == nullptr)
            {
                return nullptr;
            }
            UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return nullptr;
            }
            const auto it = cache_.find(cls);
            if (it != cache_.end())
            {
                return &it->second;
            }
            // Never clear this cache: it would dangle every `const ClassLayout*` a caller still
            // holds. Growing is safe - unordered_map never moves the nodes it already has.
            if (cache_.size() == kCacheCap && !capped_logged_)
            {
                capped_logged_ = true;
                mm::logf(L"class layout cache passed {} classes - it keeps growing on purpose "
                         L"(clearing it would dangle a ClassLayout* a caller still holds); it is "
                         L"dropped whole on every level transition",
                         kCacheCap);
            }
            return &cache_.emplace(cls, walk_class(cls)).first->second;
        }

        void clear()
        {
            cache_.clear();
        }

      private:
        static constexpr std::size_t kCacheCap = 4096;
        std::unordered_map<UClass*, ClassLayout> cache_;
        bool capped_logged_ = false;
    };

    inline const Prop* find_prop(const ClassLayout* layout, const wchar_t* name)
    {
        if (layout == nullptr)
        {
            return nullptr;
        }
        // Heterogeneous lookup: no std::wstring is built for the key (see WStrHash).
        const auto it = layout->props.find(std::wstring_view{name});
        return it == layout->props.end() ? nullptr : &it->second;
    }

    // A reflected BOOL is a bitfield: `uint8 bHidden : 1` shares its byte with a dozen flags,
    // and the bit is named by `FBoolProperty::ByteOffset` + `FieldMask`. Whether an FProperty IS
    // an FBoolProperty cannot be asked without guessing an engine struct's layout, so it is
    // validated instead: element size 1, FieldSize 1, ByteOffset < 8, and FieldMask a single set
    // bit (or 0xFF for a native `bool` member). `ok == false` means "not a bool I can read",
    // never "false".
    struct BoolInfo
    {
        bool ok = false;
        std::uint8_t byte_off = 0;
        std::uint8_t mask = 0;
    };

    inline BoolInfo bool_info(const Prop* p)
    {
        BoolInfo bi{};
        if (p == nullptr || p->field == nullptr || p->size != 1)
        {
            return bi;
        }
        auto* bp = static_cast<FBoolProperty*>(p->field);
        std::uint8_t mask = 0;
        std::uint8_t byte_off = 0;
        std::uint8_t field_size = 0;
        if (!mem::copy(&bp->GetFieldMask(), &mask, sizeof(mask)) ||
            !mem::copy(&bp->GetByteOffset(), &byte_off, sizeof(byte_off)) ||
            !mem::copy(&bp->GetFieldSize(), &field_size, sizeof(field_size)))
        {
            return bi;
        }
        const bool one_bit = mask != 0 && (mask & static_cast<std::uint8_t>(mask - 1)) == 0;
        if (field_size != 1 || byte_off >= 8 || !(one_bit || mask == 0xFF))
        {
            return bi;
        }
        bi.ok = true;
        bi.byte_off = byte_off;
        bi.mask = mask;
        return bi;
    }

    // Reads a reflected bool (bitfield or native). `out` is untouched on failure.
    inline bool read_bool_prop(const ClassLayout* layout, const void* obj, const wchar_t* name,
                               bool& out)
    {
        const Prop* p = find_prop(layout, name);
        if (p == nullptr || obj == nullptr)
        {
            return false;
        }
        const BoolInfo bi = bool_info(p);
        if (!bi.ok)
        {
            return false;
        }
        std::uint8_t byte = 0;
        if (!mem::read_at(obj, p->offset + bi.byte_off, byte))
        {
            return false;
        }
        out = (byte & bi.mask) != 0;
        return true;
    }

    // ---- typed property reads (all guarded; a wrong offset returns false) ----------

    template <typename T>
    inline bool read_prop(const ClassLayout* layout, const void* obj, const wchar_t* name, T& out, int expect_size = 0)
    {
        const Prop* p = find_prop(layout, name);
        if (p == nullptr || obj == nullptr)
        {
            return false;
        }
        if (expect_size != 0 && p->size != expect_size)
        {
            return false;
        }
        return mem::read_at(obj, p->offset, out);
    }

    // A reflected NUMERIC UPROPERTY read as a double whatever width this build declares it with,
    // reporting the width found. **In UE5 a Blueprint "float" is a `double`**: since Large World
    // Coordinates the `float` pin type is backed by `FDoubleProperty` (8 bytes) unless declared
    // in C++ as a real `float`, so a 4-byte `expect_size` returns FALSE and looks like "absent".
    inline bool read_numeric_prop(const ClassLayout* layout, const void* obj, const wchar_t* name,
                                  double& out, int* width = nullptr)
    {
        const Prop* p = find_prop(layout, name);
        if (p == nullptr || obj == nullptr)
        {
            return false;
        }
        if (p->size == static_cast<int>(sizeof(double)))
        {
            double v = 0.0;
            if (!mem::read_at(obj, p->offset, v))
            {
                return false;
            }
            out = v;
            if (width != nullptr)
            {
                *width = 8;
            }
            return true;
        }
        if (p->size == static_cast<int>(sizeof(float)))
        {
            float v = 0.0f;
            if (!mem::read_at(obj, p->offset, v))
            {
                return false;
            }
            out = static_cast<double>(v);
            if (width != nullptr)
            {
                *width = 4;
            }
            return true;
        }
        return false; // not a 4- or 8-byte scalar: not a number we can read
    }

    // Is this property one of the two widths read_numeric_prop understands? Used by the failure
    // diagnostics, which must list every number on a class.
    inline bool prop_is_numeric_width(const Prop& p)
    {
        return p.size == static_cast<int>(sizeof(float)) || p.size == static_cast<int>(sizeof(double));
    }

    // A TObjectPtr<T> / T* UPROPERTY. Rejects the bogus values a wrong offset produces.
    inline UObject* read_object_prop(const ClassLayout* layout, const void* obj, const wchar_t* name)
    {
        void* raw = nullptr;
        if (!read_prop(layout, obj, name, raw))
        {
            return nullptr;
        }
        if (!mem::plausible_ptr(raw) || !mem::readable(raw, sizeof(void*) * 4))
        {
            return nullptr;
        }
        return static_cast<UObject*>(raw);
    }

    // ---- function calls ------------------------------------------------------------

    // UE5 FVector / FRotator are three doubles (LWC); the whole param block IS the return value.
    struct FVec3
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct FRot3
    {
        double pitch = 0.0;
        double yaw = 0.0;
        double roll = 0.0;
    };

    // Defined below with the other signature helpers.
    inline int func_param_size(UFunction* fn);

    // Cached per class: lookup runs at 10 Hz for the same three names. `parm_size` is the
    // reflected parameter-block size, so the SIGNATURE can be checked without a second walk;
    // `rejected` remembers a refusal, so a bad signature costs one log line per (class, name).
    class FuncCache
    {
      public:
        struct Entry
        {
            UFunction* fn = nullptr;
            int parm_size = -1;
            bool rejected = false;
        };

        // References into an unordered_map are stable across rehashing: valid until clear().
        Entry* lookup(UObject* obj, const wchar_t* name)
        {
            if (obj == nullptr)
            {
                return nullptr;
            }
            UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return nullptr;
            }
            const auto it = cache_.find(KeyView{cls, std::wstring_view{name}});
            if (it != cache_.end())
            {
                return &it->second;
            }
            Entry e{};
            e.fn = obj->GetFunctionByNameInChain(name);
            e.parm_size = e.fn != nullptr ? func_param_size(e.fn) : -1;
            // Never clear here. A caller holds the Entry*/UFunction* across the call it is
            // about to make, so clearing is a use-after-free. The cap is only VISIBLE.
            if (cache_.size() == kCacheCap && !capped_logged_)
            {
                capped_logged_ = true;
                mm::logf(L"UFunction cache passed {} (class, name) pairs - it keeps growing on "
                         L"purpose (clearing it would dangle a UFunction* a caller is about to "
                         L"call); it is dropped whole on every level transition",
                         kCacheCap);
            }
            Entry* stored = &cache_.emplace(Key{cls, std::wstring{name}}, e).first->second;
            if (e.fn == nullptr)
            {
                // One line per (class, name): a rename on a new build otherwise degrades quietly.
                log_once(cls, name,
                         L"does not exist on this class - whatever needs it is off "
                         L"(renamed on this game build?)");
            }
            return stored;
        }

        UFunction* get(UObject* obj, const wchar_t* name)
        {
            Entry* e = lookup(obj, name);
            return (e != nullptr && !e->rejected) ? e->fn : nullptr;
        }

        // SIGNATURE GATE for a caller handing ProcessEvent a fixed-size parameter block.
        // ProcessEvent copies the function's own block size in and out of that buffer, so a
        // mismatched length smashes the game thread's stack. A mismatch is treated as MISSING.
        UFunction* get_checked(UObject* obj, const wchar_t* name, std::size_t bytes)
        {
            Entry* e = lookup(obj, name);
            if (e == nullptr || e->fn == nullptr || e->rejected)
            {
                return nullptr;
            }
            if (e->parm_size != static_cast<int>(bytes))
            {
                e->rejected = true;
                log_once(cls_of(obj), name,
                         std::format(L"has a {}-byte reflected parameter block, not the {} bytes "
                                     L"this mod hands ProcessEvent - the call is NOT made (the "
                                     L"signature changed on this game build?)",
                                     e->parm_size,
                                     bytes));
                return nullptr;
            }
            return e->fn;
        }

        void clear()
        {
            cache_.clear();
            // `logged_` is deliberately NOT cleared: it is what makes "once per session" true.
        }

      private:
        static constexpr std::size_t kCacheCap = 4096;

        struct Key
        {
            UClass* cls;
            std::wstring name;
        };

        struct KeyView
        {
            UClass* cls;
            std::wstring_view name;
        };

        // Heterogeneous key: a lookup must not build a std::wstring (it would allocate at 10 Hz).
        struct KeyHash
        {
            using is_transparent = void;

            std::size_t operator()(const Key& k) const noexcept
            {
                return mix(k.cls, std::wstring_view{k.name});
            }
            std::size_t operator()(const KeyView& k) const noexcept
            {
                return mix(k.cls, k.name);
            }

          private:
            static std::size_t mix(UClass* cls, std::wstring_view name) noexcept
            {
                return std::hash<const void*>{}(cls) ^
                       (std::hash<std::wstring_view>{}(name) * 1099511628211ull);
            }
        };

        struct KeyEq
        {
            using is_transparent = void;

            bool operator()(const Key& a, const Key& b) const noexcept
            {
                return a.cls == b.cls && a.name == b.name;
            }
            bool operator()(const Key& a, const KeyView& b) const noexcept
            {
                return a.cls == b.cls && std::wstring_view{a.name} == b.name;
            }
            bool operator()(const KeyView& a, const Key& b) const noexcept
            {
                return a.cls == b.cls && a.name == std::wstring_view{b.name};
            }
            bool operator()(const KeyView& a, const KeyView& b) const noexcept
            {
                return a.cls == b.cls && a.name == b.name;
            }
        };

        static UClass* cls_of(UObject* obj)
        {
            return obj != nullptr ? obj->GetClassPrivate() : nullptr;
        }

        // One line per (class, function), whatever happens afterwards.
        void log_once(UClass* cls, const wchar_t* name, const std::wstring& what)
        {
            std::wstring cname = L"<unknown class>";
            if (cls != nullptr && mem::readable(cls, 0x40))
            {
                cname = static_cast<UObject*>(cls)->GetName();
            }
            if (!logged_.insert(cname + L"::" + name).second)
            {
                return;
            }
            mm::logf(L"reflection: {}::{}() {}", cname, name, what);
        }

        std::unordered_map<Key, Entry, KeyHash, KeyEq> cache_;
        std::unordered_set<std::wstring> logged_;
        bool capped_logged_ = false;
    };

    // POD trampoline for mem::guarded_call, so the __try in mem.cpp is legal.
    inline void process_event_trampoline(void* obj, void* fn, void* params)
    {
        static_cast<UObject*>(obj)->ProcessEvent(static_cast<UFunction*>(fn), params);
    }

    // Calls a zero-argument getter whose whole parameter block is the return value. The
    // ProcessEvent goes inside an SEH guard: an object freed by a level transition mid-frame
    // becomes `false` instead of a crash dump.
    template <typename Ret>
    inline bool call_getter(FuncCache& funcs, UObject* obj, const wchar_t* name, Ret& out)
    {
        if (obj == nullptr)
        {
            return false;
        }
        // The reflected parameter block must be exactly the block handed over - see get_checked.
        UFunction* fn = funcs.get_checked(obj, name, sizeof(Ret));
        if (fn == nullptr)
        {
            return false;
        }
        Ret scratch{};
        if (!mem::guarded_call(&process_event_trampoline, obj, fn, &scratch))
        {
            return false;
        }
        out = scratch;
        return true;
    }

    // An FString is { TCHAR* Data; int32 ArrayNum; int32 ArrayMax } = 16 bytes, and a TArray<T>
    // is the same shape with T* Data. Neither is reflected beyond the property offset, so both
    // are read raw - guarded, with a sanity cap on the count.

    struct FStringRaw
    {
        const wchar_t* data = nullptr;
        std::int32_t num = 0;
        std::int32_t max = 0;
    };

    inline constexpr int kMaxFStringChars = 1024;
    inline constexpr int kMaxArrayItems = 4096;

    inline bool read_fstring_at(const void* addr, std::wstring& out)
    {
        FStringRaw raw{};
        if (!mem::read(addr, raw))
        {
            return false;
        }
        if (raw.num <= 0 || raw.num > kMaxFStringChars || raw.max < raw.num)
        {
            // num == 0 with a null pointer is a legitimately EMPTY FString, not a bad read.
            if (raw.num == 0 && raw.data == nullptr)
            {
                out.clear();
                return true;
            }
            return false;
        }
        if (!mem::plausible_ptr(raw.data) ||
            !mem::readable(raw.data, static_cast<std::size_t>(raw.num) * sizeof(wchar_t)))
        {
            return false;
        }
        std::wstring text;
        text.resize(static_cast<std::size_t>(raw.num));
        if (!mem::copy(raw.data, text.data(), text.size() * sizeof(wchar_t)))
        {
            return false;
        }
        // UE stores the terminating NUL inside ArrayNum.
        while (!text.empty() && text.back() == L'\0')
        {
            text.pop_back();
        }
        out = std::move(text);
        return true;
    }

    inline bool read_fstring_prop(const ClassLayout* layout, const void* obj, const wchar_t* name,
                                  std::wstring& out)
    {
        const Prop* p = find_prop(layout, name);
        if (p == nullptr || obj == nullptr || p->size != 16)
        {
            return false;
        }
        return read_fstring_at(static_cast<const std::uint8_t*>(obj) + p->offset, out);
    }

    // A TArray<FString> property. Each element is a 16-byte FString read in place.
    inline bool read_str_array_prop(const ClassLayout* layout, const void* obj, const wchar_t* name,
                                    std::vector<std::wstring>& out)
    {
        const Prop* p = find_prop(layout, name);
        if (p == nullptr || obj == nullptr || p->size != 16)
        {
            return false;
        }
        FStringRaw hdr{}; // same shape as TArray's header
        if (!mem::read_at(obj, p->offset, hdr))
        {
            return false;
        }
        out.clear();
        if (hdr.num == 0)
        {
            return hdr.data == nullptr || mem::plausible_ptr(hdr.data);
        }
        if (hdr.num < 0 || hdr.num > kMaxArrayItems || hdr.max < hdr.num || !mem::plausible_ptr(hdr.data))
        {
            return false;
        }
        const auto* base = reinterpret_cast<const std::uint8_t*>(hdr.data);
        if (!mem::readable(base, static_cast<std::size_t>(hdr.num) * 16u))
        {
            return false;
        }
        out.reserve(static_cast<std::size_t>(hdr.num));
        for (std::int32_t i = 0; i < hdr.num; ++i)
        {
            std::wstring one;
            if (!read_fstring_at(base + static_cast<std::size_t>(i) * 16u, one))
            {
                continue;
            }
            out.push_back(std::move(one));
        }
        return true;
    }

    // A UFunction is a UStruct whose child properties ARE its parameters, in declaration order,
    // so the real signature is one walk away. Never call one with a guessed signature.

    struct ParamInfo
    {
        std::wstring name;
        int offset = 0;
        int size = 0;
    };

    inline std::vector<ParamInfo> func_params(UFunction* fn)
    {
        std::vector<ParamInfo> out;
        if (fn == nullptr || !mem::readable(fn, 0x40))
        {
            return out;
        }
        FField* field = static_cast<UStruct*>(fn)->GetChildProperties();
        for (int i = 0; field != nullptr && i < 64; ++i)
        {
            auto* prop = static_cast<FProperty*>(field);
            ParamInfo info{};
            info.offset = prop->GetOffset_Internal();
            info.size = prop->GetElementSize();
            info.name = field->GetName();
            out.push_back(std::move(info));
            field = FFieldAccess::next(field);
        }
        return out;
    }

    // The parameter-block size ProcessEvent expects; 0 for no parameters and no return value.
    inline int func_param_size(UFunction* fn)
    {
        if (fn == nullptr || !mem::readable(fn, 0x40))
        {
            return -1;
        }
        return static_cast<UStruct*>(fn)->GetPropertiesSize();
    }

    // Wide -> narrow, ASCII only. `std::string(w.begin(), w.end())` emits C4244.
    inline std::string narrow_ascii(std::wstring_view w)
    {
        std::string out;
        out.reserve(w.size());
        for (wchar_t c : w)
        {
            out.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
        }
        return out;
    }

    // Object liveness: a cached UObject* does NOT survive a level transition, and comparing
    // GetClassPrivate() against a remembered UClass* is not enough because freed memory usually
    // still holds the old bytes. GUObjectArray's FUObjectItem slots live in UE's permanently
    // committed object array and stay safe to read after the object died, so ObjRef keeps the
    // internal index and checks index -> FUObjectItem -> flags and back-pointer first.

    struct ObjRef
    {
        UObject* obj = nullptr;
        UClass* cls = nullptr;
        int index = -1;
        int serial = 0;

        bool empty() const
        {
            return obj == nullptr;
        }

        void reset()
        {
            *this = ObjRef{};
        }
    };

    namespace detail
    {
        struct CaptureArgs
        {
            UObject* obj = nullptr;
            UClass* cls = nullptr;
            int index = -1;
            int serial = 0;
            bool ok = false;
        };

        inline void capture_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<CaptureArgs*>(a);
            UObject* obj = args->obj;
            args->index = obj->GetInternalIndex();
            args->cls = obj->GetClassPrivate();
            if (obj->HasAnyFlags(static_cast<RC::Unreal::EObjectFlags>(
                    RC::Unreal::RF_BeginDestroyed | RC::Unreal::RF_FinishDestroyed |
                    RC::Unreal::RF_ClassDefaultObject | RC::Unreal::RF_ArchetypeObject)))
            {
                return;
            }
            RC::Unreal::FUObjectItem* item = RC::Unreal::FUObjectArray::IndexToObject(args->index);
            if (item == nullptr || item->GetUObject() != obj || !item->IsValid(false))
            {
                return;
            }
            args->serial = item->GetSerialNumber();
            args->ok = args->cls != nullptr;
        }

        struct AliveArgs
        {
            UObject* obj = nullptr;
            UClass* cls = nullptr;
            int index = -1;
            int serial = 0;
            bool ok = false;
        };

        inline void alive_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<AliveArgs*>(a);
            RC::Unreal::FUObjectItem* item = RC::Unreal::FUObjectArray::IndexToObject(args->index);
            if (item == nullptr)
            {
                return;
            }
            // Read the slot first: safe even after the object's allocation was handed back.
            if (item->GetUObject() != args->obj)
            {
                return; // slot recycled for a different object
            }
            if (!item->IsValid(false) || item->IsUnreachable() || item->IsPendingKill())
            {
                return;
            }
            if (args->serial != 0 && item->GetSerialNumber() != args->serial)
            {
                return;
            }
            // Only now the object itself.
            if (args->obj->GetClassPrivate() != args->cls)
            {
                return;
            }
            if (args->obj->HasAnyFlags(static_cast<RC::Unreal::EObjectFlags>(
                    RC::Unreal::RF_BeginDestroyed | RC::Unreal::RF_FinishDestroyed)))
            {
                return;
            }
            args->ok = true;
        }

        struct FindXOfArgs
        {
            UObject* obj = nullptr;
            bool ok = false;
        };

        inline void find_xof_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<FindXOfArgs*>(a);
            UObject* obj = args->obj;
            if (obj->HasAnyFlags(static_cast<RC::Unreal::EObjectFlags>(
                    RC::Unreal::RF_ClassDefaultObject | RC::Unreal::RF_ArchetypeObject)))
            {
                return;
            }
            args->ok = !obj->IsUnreachable();
        }

        struct WorldArgs
        {
            UObject* obj = nullptr;
            const void* world = nullptr;
        };

        inline void world_trampoline(void* a, void*, void*)
        {
            auto* args = static_cast<WorldArgs*>(a);
            args->world = args->obj->GetWorld();
        }
    } // namespace detail

    // What UE4SS's FindAllOf / FindFirstOf count as a real instance: not a class default
    // object, not an archetype, not already unreachable. The pinned UE4SS build exports no
    // equivalent (`UObjectGlobals::IsValidObjectForFindXOf` arrives in a later commit), so
    // the mod owns the test - same two rejects, plus the readability and SEH guards every
    // other engine read here carries.
    inline bool valid_for_find_xof(UObject* obj)
    {
        if (obj == nullptr || !mem::readable(obj, 0x40))
        {
            return false;
        }
        detail::FindXOfArgs args{};
        args.obj = obj;
        if (!mem::guarded_call(&detail::find_xof_trampoline, &args, nullptr, nullptr))
        {
            return false;
        }
        return args.ok;
    }

    // Remembers an object by pointer + GUObjectArray index (+ serial). Rejects CDOs,
    // archetypes and dying objects.
    inline bool capture(UObject* obj, ObjRef& out)
    {
        out.reset();
        if (obj == nullptr || !mem::readable(obj, 0x40))
        {
            return false;
        }
        detail::CaptureArgs args{};
        args.obj = obj;
        if (!mem::guarded_call(&detail::capture_trampoline, &args, nullptr, nullptr) || !args.ok)
        {
            return false;
        }
        out.obj = obj;
        out.cls = args.cls;
        out.index = args.index;
        out.serial = args.serial;
        return true;
    }

    inline bool alive(const ObjRef& ref)
    {
        if (ref.obj == nullptr || ref.cls == nullptr || ref.index < 0)
        {
            return false;
        }
        if (!mem::readable(ref.obj, 0x40))
        {
            return false;
        }
        detail::AliveArgs args{};
        args.obj = ref.obj;
        args.cls = ref.cls;
        args.index = ref.index;
        args.serial = ref.serial;
        if (!mem::guarded_call(&detail::alive_trampoline, &args, nullptr, nullptr))
        {
            return false;
        }
        return args.ok;
    }

    // The object's UWorld*, an identity token for "did the level change". nullptr is "unknown".
    inline const void* world_of(const ObjRef& ref)
    {
        if (!alive(ref))
        {
            return nullptr;
        }
        detail::WorldArgs args{};
        args.obj = ref.obj;
        if (!mem::guarded_call(&detail::world_trampoline, &args, nullptr, nullptr))
        {
            return nullptr;
        }
        return args.world;
    }

    // The class's short name, e.g. `BP_CombatCharacter_Player_Final_C`. Allocates.
    inline std::wstring class_name(const ObjRef& ref)
    {
        if (ref.cls == nullptr)
        {
            return {};
        }
        if (!mem::readable(ref.cls, 0x40))
        {
            return {};
        }
        return static_cast<UObject*>(ref.cls)->GetName();
    }
} // namespace uer

#pragma once

//
// uereflect - the small slice of UE reflection the minimap needs, on top of ue_min.hpp.
//
// Property offsets are read through the FField child-property chain (exactly as
// navmesh_dump.cpp does it) and cached per UClass*, because the game-thread pump runs
// at 10 Hz and must not walk 30 super-structs every time.
//
// Function calls go through UObject::GetFunctionByNameInChain + UObject::ProcessEvent.
// Two rules, both from lessons.md:
//   * a null UFunction means "the function does not exist on this class" - never treat
//     a nil/empty answer as evidence about the game state;
//   * ProcessEvent from inside UE4SS's ProcessEvent pre-callback re-enters the
//     callback, so the caller MUST hold a re-entrancy guard.
//
// EVERYTHING in here is game-thread only.
//

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "mem.hpp"
#include "ue_min.hpp"

namespace uer
{
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
    };

    struct ClassLayout
    {
        std::unordered_map<std::wstring, Prop> props;
        int walked = 0;
    };

    // Walks the class and every super struct. A pure-native class simply yields
    // nothing; that is not an error.
    inline ClassLayout walk_class(UClass* cls)
    {
        ClassLayout out{};
        if (cls == nullptr)
        {
            return out;
        }
        auto* current = static_cast<UStruct*>(cls);
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
                        out.props.emplace(std::move(name), Prop{static_cast<std::size_t>(offset), size});
                    }
                }
                field = FFieldAccess::next(field);
            }
            current = current->GetSuperStruct();
        }
        return out;
    }

    // Per-UClass layout cache. Keyed on the UClass pointer: classes are never moved
    // and never freed while instances exist.
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
            if (cache_.size() > 4096)
            {
                cache_.clear(); // pathological; keeps the map bounded
            }
            return &cache_.emplace(cls, walk_class(cls)).first->second;
        }

        void clear()
        {
            cache_.clear();
        }

      private:
        std::unordered_map<UClass*, ClassLayout> cache_;
    };

    inline const Prop* find_prop(const ClassLayout* layout, const wchar_t* name)
    {
        if (layout == nullptr)
        {
            return nullptr;
        }
        const auto it = layout->props.find(name);
        return it == layout->props.end() ? nullptr : &it->second;
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

    // A TObjectPtr<T> / T* UPROPERTY. Rejects the obviously-bogus values a wrong
    // offset produces so the caller never dereferences garbage.
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

    // UE5 FVector / FRotator are three doubles (LWC). These are the parameter blocks
    // for the zero-argument getters: the whole block IS the return value.
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

    // Per-object function lookup is cheap enough (a name hash on the class chain) but
    // it is called at 10 Hz for the same three names, so cache per class.
    class FuncCache
    {
      public:
        UFunction* get(UObject* obj, const wchar_t* name)
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
            const Key key{cls, name};
            const auto it = cache_.find(key);
            if (it != cache_.end())
            {
                return it->second;
            }
            UFunction* fn = obj->GetFunctionByNameInChain(name);
            if (cache_.size() > 4096)
            {
                cache_.clear();
            }
            cache_.emplace(key, fn);
            return fn;
        }

        void clear()
        {
            cache_.clear();
        }

      private:
        struct Key
        {
            UClass* cls;
            std::wstring name;
            bool operator==(const Key& o) const
            {
                return cls == o.cls && name == o.name;
            }
        };

        struct KeyHash
        {
            std::size_t operator()(const Key& k) const
            {
                return std::hash<const void*>{}(k.cls) ^ (std::hash<std::wstring>{}(k.name) * 1099511628211ull);
            }
        };

        std::unordered_map<Key, UFunction*, KeyHash> cache_;
    };

    // Calls a zero-argument getter whose whole parameter block is the return value.
    template <typename Ret>
    inline bool call_getter(FuncCache& funcs, UObject* obj, const wchar_t* name, Ret& out)
    {
        UFunction* fn = funcs.get(obj, name);
        if (fn == nullptr)
        {
            return false;
        }
        Ret scratch{};
        obj->ProcessEvent(fn, &scratch);
        out = scratch;
        return true;
    }
} // namespace uer

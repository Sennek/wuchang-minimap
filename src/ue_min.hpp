#pragma once

//
// Minimal hand-written declarations of the RC::Unreal reflection API.
//
// WHY THIS FILE EXISTS
// --------------------
// UE4SS's real Unreal headers live in `deps/first/Unreal`, which is the private
// UEPseudo repository (see README.md - it needs Epic Games GitHub access). We do not
// have them, so `#include <Unreal/UObjectGlobals.hpp>` is not an option.
//
// But the shipped `UE4SS.dll` *exports* the whole reflection API (4239 exports), and
// `sdk/lib/UE4SS.lib` - synthesised from its export table - can resolve any of them.
// An MSVC mangled name depends only on namespace, class name, function name,
// parameter types, cv/ref qualifiers and the access specifier; it never depends on
// class layout. So re-declaring the handful of members we call, with the same names
// and signatures, produces byte-identical symbols and links against the real code.
//
// RULES FOR THIS FILE
// -------------------
//  * The classes below are deliberately EMPTY. Never instantiate one, never take
//    sizeof, never dereference. They exist only to carry member-function symbols.
//  * The inheritance chains mirror the real ones and are all single, non-virtual and
//    at offset 0, so `this` needs no adjustment when a call lands on a base class.
//  * Every declaration is annotated with the exact symbol from `sdk/UE4SS.def` it
//    must match. If the linker ever reports an unresolved external, diff its mangled
//    name against the comment - that pinpoints the drifted signature immediately.
//  * `FField::GetNext` is PRIVATE in UEPseudo, and the access specifier *is* part of
//    the mangled name, so it must stay private here too; FFieldAccess is the friend
//    that reaches it.
//

#include <string>
#include <string_view>
#include <vector>

namespace RC::Unreal
{
    class UClass;
    class UStruct;
    class FField;

    class UObjectBase
    {
      public:
        // ?GetClassPrivate@UObjectBase@Unreal@RC@@QEAAAEAPEAVUClass@23@XZ
        UClass*& GetClassPrivate();
    };

    class UObject : public UObjectBase
    {
      public:
        // ?GetName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
        std::wstring GetName() const;

        // ?GetFullName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@PEAV123@@Z
        std::wstring GetFullName(UObject* stop_outer = nullptr) const;
    };

    class UField : public UObject
    {
    };

    class UStruct : public UField
    {
      public:
        // ?GetSuperStruct@UStruct@Unreal@RC@@QEAAAEAPEAV123@XZ
        UStruct*& GetSuperStruct();

        // ?GetChildProperties@UStruct@Unreal@RC@@QEAAAEAPEAVFField@23@XZ
        FField*& GetChildProperties();

        // ?GetPropertiesSize@UStruct@Unreal@RC@@QEAAAEAHXZ
        int& GetPropertiesSize();

        // ?GetStructureSize@UStruct@Unreal@RC@@QEBAHXZ
        int GetStructureSize() const;
    };

    class UClass : public UStruct
    {
    };

    class FField
    {
      public:
        // ?GetName@FField@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
        std::wstring GetName() const;

      private:
        // ?GetNext@FField@Unreal@RC@@AEAAAEAPEAV123@XZ   <- private in UEPseudo
        FField*& GetNext();

        friend struct FFieldAccess;
    };

    // The only legal way to walk the FField linked list from outside.
    struct FFieldAccess
    {
        static FField* next(FField* field)
        {
            return field == nullptr ? nullptr : field->GetNext();
        }
    };

    class FProperty : public FField
    {
      public:
        // ?GetOffset_Internal@FProperty@Unreal@RC@@QEBAAEBHXZ
        const int& GetOffset_Internal() const;

        // ?GetElementSize@FProperty@Unreal@RC@@QEBAAEBHXZ
        const int& GetElementSize() const;
    };

    namespace UObjectGlobals
    {
        // ?FindAllOf@UObjectGlobals@Unreal@RC@@YAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@
        //   AEAV?$vector@PEAVUObject@Unreal@RC@@V?$allocator@PEAVUObject@Unreal@RC@@@std@@@5@@Z
        //
        // Finds every live instance of the class with this *short* name (and of its
        // subclasses). Returns nothing for a name that has no loaded class, which is
        // exactly what happens at the main menu for "RecastNavMesh".
        void FindAllOf(std::wstring_view class_name, std::vector<UObject*>& out);
    } // namespace UObjectGlobals
} // namespace RC::Unreal

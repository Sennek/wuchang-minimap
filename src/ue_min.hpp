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

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace RC::Unreal
{
    class UClass;
    class UStruct;
    class FField;
    class UFunction;
    class UObject;
    class UWorld;
    struct FUObjectItem;

    // Unscoped enum, so it mangles as `W4EObjectFlags@23@` exactly like UEPseudo's.
    // Only the values the mod tests are spelled out.
    enum EObjectFlags
    {
        RF_NoFlags = 0x00000000,
        RF_ClassDefaultObject = 0x00000010,
        RF_ArchetypeObject = 0x00000020,
        RF_BeginDestroyed = 0x00008000,
        RF_FinishDestroyed = 0x00010000,
    };

    class UObjectBase
    {
      public:
        // ?GetClassPrivate@UObjectBase@Unreal@RC@@QEAAAEAPEAVUClass@23@XZ
        UClass*& GetClassPrivate();

        // ?GetInternalIndex@UObjectBase@Unreal@RC@@QEBA?BHXZ
        //
        // The object's slot in GUObjectArray. Captured while the object is known good,
        // it is the key that makes a later liveness test possible without touching the
        // object's own (possibly freed) memory.
        const int GetInternalIndex() const;

        // ?GetObjectItem@UObjectBase@Unreal@RC@@QEAAPEAUFUObjectItem@23@XZ
        FUObjectItem* GetObjectItem();
    };

    // The GUObjectArray slot. This lives in UE's permanently-committed object array,
    // not in the object's own allocation, so reading it is safe even after the object
    // has been destroyed and freed - which is exactly why the validation goes through
    // here rather than through the cached UObject*.
    struct FUObjectItem
    {
        // ?GetUObject@FUObjectItem@Unreal@RC@@QEBAPEAVUObject@23@XZ
        UObject* GetUObject() const;

        // ?IsValid@FUObjectItem@Unreal@RC@@QEBA_N_N@Z   (bEvenIfPendingKill)
        bool IsValid(bool even_if_pending_kill) const;

        // ?IsUnreachable@FUObjectItem@Unreal@RC@@QEBA_NXZ
        bool IsUnreachable() const;

        // ?IsPendingKill@FUObjectItem@Unreal@RC@@QEBA_NXZ
        bool IsPendingKill() const;

        // ?GetSerialNumber@FUObjectItem@Unreal@RC@@QEBAAEBHXZ
        const int& GetSerialNumber() const;
    };

    class FUObjectArray
    {
      public:
        // ?IndexToObject@FUObjectArray@Unreal@RC@@SAPEAUFUObjectItem@23@H@Z
        //
        // Bounds-checked: an out-of-range index yields nullptr.
        static FUObjectItem* IndexToObject(int index);

        // ?GetNumElements@FUObjectArray@Unreal@RC@@SAHXZ
        //
        // How many slots GUObjectArray currently holds (used, free and never-used).
        // It is the loop bound of the chunked marker walk: iterating [0, N) and
        // rejecting the slots whose FUObjectItem is not valid costs one pass over the
        // array instead of the one-pass-per-class that FindAllOf charges.
        //
        // The value GROWS as levels stream in and can drop after a GC, so it is
        // re-read every slice and the cursor clamped against it - never cached.
        static int GetNumElements();
    };

    class UObject : public UObjectBase
    {
      public:
        // ?HasAnyFlags@UObject@Unreal@RC@@QEAA_NW4EObjectFlags@23@@Z
        bool HasAnyFlags(EObjectFlags flags);

        // ?IsUnreachable@UObject@Unreal@RC@@QEAA_NXZ
        bool IsUnreachable();

        // ?GetWorld@UObject@Unreal@RC@@QEBAPEAVUWorld@23@XZ
        //
        // Used only as a cheap *identity* signal: when the pawn's world pointer changes,
        // a level transition happened and every cached pointer is suspect.
        UWorld* GetWorld() const;

        // ?GetName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
        std::wstring GetName() const;

        // ?GetFullName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@PEAV123@@Z
        std::wstring GetFullName(UObject* stop_outer = nullptr) const;

        // ?GetFunctionByNameInChain@UObject@Unreal@RC@@QEAAPEAVUFunction@23@PEB_W@Z
        //
        // Looks the UFunction up on this object's class and every super class. Returns
        // nullptr when the name does not exist, which is the only way to tell - a
        // BlueprintCallable getter that is not there must never be *assumed* present
        // (lessons.md: APlayerCameraManager has no GetViewTarget()).
        UFunction* GetFunctionByNameInChain(const wchar_t* name);

        // ?ProcessEvent@UObject@Unreal@RC@@QEAAXPEAVUFunction@23@PEAX@Z
        //
        // Calls the function with `params` pointing at the function's parameter block
        // (parameters in declaration order, then the return value at the end - for the
        // zero-argument getters we use, `params` is just the return value).
        //
        // MUST only be called from the game thread, i.e. from inside our ProcessEvent
        // pre-callback pump - and the pump has to guard against re-entrancy, because
        // every call made from it fires the pre-callback again.
        void ProcessEvent(UFunction* function, void* params);
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

    // A UFunction IS a UStruct, and that is the whole point: its parameter list is its
    // child-property chain, so `GetChildProperties()` + `GetPropertiesSize()` recover
    // the reflected signature (parameter names, offsets and sizes) of any function at
    // runtime. lessons.md forbids calling a UFunction with a guessed signature; this is
    // what makes the alternative - read the real one and compare - possible.
    //
    // No members of its own are declared: UFunction's own accessors (FunctionFlags,
    // NumParms, ...) are not needed, and every symbol we would add is one more thing
    // that can fail to link.
    class UFunction : public UStruct
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

    // A reflected bool is a BITFIELD, and its byte offset alone does not identify it.
    // `uint8 bHidden : 1` shares a byte with `bNetTemporary`, `bTearOff` and the rest, so
    // `GetOffset_Internal()` names the byte and these three name the bit inside it:
    // address = obj + Offset_Internal + ByteOffset, value = (*address & FieldMask) != 0.
    // A native `bool` member has FieldMask 0xFF.
    //
    // Only the three accessors are declared, and they are the CONST overloads returning
    // `const uint8&` - the mangled name carries the access specifier and the constness,
    // so both halves matter (`QEBAAEBEXZ`, from `sdk/UE4SS.def`).
    //
    // NOTE ON SAFETY. There is no cheap way to ask UE4SS "is this FProperty an
    // FBoolProperty?" - `FField::IsA` and `FBoolProperty::StaticClass` both traffic in
    // `FFieldClassVariant`, a struct whose layout we would have to guess, which is
    // exactly the mistake `lessons.md` records for `FPImplRecastNavMesh`. So these are
    // called on any FProperty and the ANSWER is validated instead
    // (uer::bool_info): element size 1, field size 1, byte offset < 8, and a mask that is
    // a single set bit or 0xFF. Taking the address of the returned reference does not
    // dereference it, and the read itself goes through the SEH-guarded mem::read_at.
    class FBoolProperty : public FProperty
    {
      public:
        // ?GetFieldMask@FBoolProperty@Unreal@RC@@QEBAAEBEXZ
        const unsigned char& GetFieldMask() const;

        // ?GetByteOffset@FBoolProperty@Unreal@RC@@QEBAAEBEXZ
        const unsigned char& GetByteOffset() const;

        // ?GetFieldSize@FBoolProperty@Unreal@RC@@QEBAAEBEXZ
        const unsigned char& GetFieldSize() const;
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

        // ?FindFirstOf@UObjectGlobals@Unreal@RC@@YAPEAVUObject@23@PEB_W@Z
        UObject* FindFirstOf(const wchar_t* class_name);

        // ?IsValidObjectForFindXOf@UObjectGlobals@Unreal@RC@@YA_NPEAVUObject@23@@Z
        bool IsValidObjectForFindXOf(UObject* object);
    } // namespace UObjectGlobals

    namespace Hook
    {
        // ?RegisterProcessEventPreCallback@Hook@Unreal@RC@@YAXV?$function@$$A6AXPEAVUObject@Unreal@RC@@
        //   PEAVUFunction@23@PEAX@Z@std@@@Z
        //
        // WHY WE NEED THIS
        // ----------------
        // `CppUserModBase::on_update` is called from UE4SS's own event-loop thread, NOT
        // from the game thread. That was proven on 2026-09-02: while the game thread sat
        // blocked in WaitForSingleObject during a GPU crash dump, our [navmesh] poll kept
        // logging every 300 ms. So anything that traverses UObjects (FindAllOf walks the
        // FUObjectHashTables) or reads engine allocations races with level streaming and
        // with the GC.
        //
        // UE4SS's ProcessEvent pre-callback, by contrast, fires *inside*
        // UObject::ProcessEvent - i.e. always on the thread that is executing the script
        // VM, which for gameplay is the game thread. It is therefore usable as a
        // game-thread pump: register once, throttle inside, and do all traversal there.
        // (`UE4SSProgram::queue_event` is NOT an alternative - it queues onto the same
        // event-loop thread.)
        void RegisterProcessEventPreCallback(std::function<void(UObject*, UFunction*, void*)> callback);
    } // namespace Hook
} // namespace RC::Unreal

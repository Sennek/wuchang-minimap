#pragma once

//
// Minimal hand-written declarations of the RC::Unreal reflection API, linked against
// `sdk/lib/UE4SS.lib` (synthesised from UE4SS.dll's export table). An MSVC mangled
// name depends only on namespace, class name, function name, parameter types, cv/ref
// qualifiers and the access specifier - never on class layout - so re-declaring the
// members we call with matching signatures produces byte-identical symbols.
//
// RULES FOR THIS FILE
//  * The classes below are EMPTY. Never instantiate, never sizeof, never dereference.
//    They exist only to carry member-function symbols.
//  * Inheritance chains mirror the real ones: single, non-virtual, at offset 0, so
//    `this` needs no adjustment when a call lands on a base class.
//  * Every declaration is annotated with the exact symbol from `sdk/UE4SS.def` it must
//    match. On an unresolved external, diff its mangled name against the comment.
//  * `FField::GetNext` is PRIVATE in UEPseudo and the access specifier is part of the
//    mangled name, so it must stay private here; FFieldAccess is the friend.
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
        // Slot in GUObjectArray. Captured while the object is known good, it permits a
        // later liveness test without touching the object's own (possibly freed) memory.
        const int GetInternalIndex() const;

        // ?GetOuterPrivate@UObjectBase@Unreal@RC@@QEAAAEAPEAVUObject@23@XZ
        //
        // The owning object, one raw field read. A widget's chain is
        // child -> WidgetTree -> the UserWidget that owns the tree, up to the game
        // instance, which is how the UI-event path reaches a menu's root from any of its
        // children.
        UObject*& GetOuterPrivate();

        // ?GetObjectItem@UObjectBase@Unreal@RC@@QEAAPEAUFUObjectItem@23@XZ
        FUObjectItem* GetObjectItem();
    };

    // The GUObjectArray slot. Lives in UE's permanently-committed object array, not in
    // the object's own allocation, so reading it is safe even after the object has been
    // destroyed and freed. Validation goes through here, not the cached UObject*.
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
        // Slots GUObjectArray currently holds (used, free and never-used); the loop
        // bound of the chunked marker walk. Grows as levels stream in and can drop
        // after a GC: re-read every slice and clamp the cursor against it, never cache.
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
        // Cheap *identity* signal only: a changed pawn world pointer means a level
        // transition, so every cached pointer is suspect.
        UWorld* GetWorld() const;

        // ?GetName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
        std::wstring GetName() const;

        // ?GetFullName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@PEAV123@@Z
        std::wstring GetFullName(UObject* stop_outer = nullptr) const;

        // ?GetFunctionByNameInChain@UObject@Unreal@RC@@QEAAPEAVUFunction@23@PEB_W@Z
        //
        // Looks the UFunction up on this object's class and every super class; nullptr
        // when the name does not exist. Never assume a BlueprintCallable getter is
        // present - APlayerCameraManager has no GetViewTarget().
        UFunction* GetFunctionByNameInChain(const wchar_t* name);

        // ?ProcessEvent@UObject@Unreal@RC@@QEAAXPEAVUFunction@23@PEAX@Z
        //
        // `params` points at the function's parameter block: parameters in declaration
        // order, then the return value at the end (for a zero-argument getter, `params`
        // is just the return value).
        //
        // GAME THREAD ONLY, i.e. from inside our ProcessEvent pre-callback pump. The
        // pump must guard against re-entrancy: every call made from it re-fires the
        // pre-callback.
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

    // The reflection object behind a USTRUCT. Its child-property chain and
    // `GetPropertiesSize()` are the element layout and the stride of a
    // `TArray<FSomeStruct>`, which is the only honest way to walk one.
    class UScriptStruct : public UStruct
    {
    };

    // UE5's pointer wrapper. Empty like every class here: it exists so the mangled name
    // of `FStructProperty::GetStruct` matches, and the pointer it holds is read through
    // mem::read, never by dereferencing this.
    template <typename T>
    class TObjectPtr
    {
    };

    // A UFunction IS a UStruct: its parameter list is its child-property chain, so
    // `GetChildProperties()` + `GetPropertiesSize()` recover the reflected signature
    // (parameter names, offsets, sizes) at runtime. Never call a UFunction with a
    // guessed signature - read the real one and compare.
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

    // The element property of a TArray UPROPERTY. As with FBoolProperty there is no
    // cheap "is this FProperty an FArrayProperty?" test, so the ANSWER is validated
    // instead: the returned pointer must be a readable FProperty whose owning struct
    // then names itself. Taking the address of the returned reference does not
    // dereference it; the read goes through mem::read.
    class FArrayProperty : public FProperty
    {
      public:
        // ?GetInner@FArrayProperty@Unreal@RC@@QEAAAEAPEAVFProperty@23@XZ
        FProperty*& GetInner();
    };

    // The key and value properties of a TMap UPROPERTY. Validated exactly like
    // FArrayProperty: each pointer must read back as a plausible, readable FProperty that
    // then reports a 4-byte element size, which is what turns "80 bytes here is an
    // FScriptMap of int32 to int32" into an answer instead of a guess.
    class FMapProperty : public FProperty
    {
      public:
        // ?GetKeyProp@FMapProperty@Unreal@RC@@QEAAAEAPEAVFProperty@23@XZ
        FProperty*& GetKeyProp();

        // ?GetValueProp@FMapProperty@Unreal@RC@@QEAAAEAPEAVFProperty@23@XZ
        FProperty*& GetValueProp();
    };

    // The USTRUCT behind a struct UPROPERTY - the route from `TArray<FStruct>` to the
    // struct's own reflected layout. Validated the same way: the UScriptStruct must
    // capture as a live object and its name must be the expected one.
    class FStructProperty : public FProperty
    {
      public:
        // ?GetStruct@FStructProperty@Unreal@RC@@QEAAAEAV?$TObjectPtr@VUScriptStruct@Unreal@RC@@@23@XZ
        TObjectPtr<UScriptStruct>& GetStruct();
    };

    // An FName is a pair of 32-bit indices into the global name table, not a string.
    // EMPTY like the rest of this file: the only legal use is to call ToString() on a
    // pointer to eight bytes copied out of the game's memory.
    class FName
    {
      public:
        // ?ToString@FName@Unreal@RC@@QEAA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
        std::wstring ToString();
    };

    // A reflected bool is a BITFIELD; its byte offset alone does not identify it.
    // `uint8 bHidden : 1` shares a byte with `bNetTemporary`, `bTearOff` and the rest:
    // address = obj + Offset_Internal + ByteOffset, value = (*address & FieldMask) != 0.
    // A native `bool` member has FieldMask 0xFF.
    //
    // These are the CONST overloads returning `const uint8&` - the mangled name carries
    // both the access specifier and the constness (`QEBAAEBEXZ`, from `sdk/UE4SS.def`).
    //
    // SAFETY. There is no cheap "is this FProperty an FBoolProperty?" test: `FField::IsA`
    // and `FBoolProperty::StaticClass` both traffic in `FFieldClassVariant`, whose layout
    // we would have to guess. So these are called on any FProperty and the ANSWER is
    // validated instead (uer::bool_info): element size 1, field size 1, byte offset < 8,
    // mask a single set bit or 0xFF. Taking the address of the returned reference does
    // not dereference it; the read goes through the SEH-guarded mem::read_at.
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
        // Every live instance of the class with this *short* name, and of its
        // subclasses. Empty for a name with no loaded class (e.g. "RecastNavMesh" at
        // the main menu).
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
        // `CppUserModBase::on_update` runs on UE4SS's own event-loop thread, NOT the
        // game thread, so UObject traversal there (FindAllOf walks the
        // FUObjectHashTables) races level streaming and the GC.
        //
        // This pre-callback fires *inside* UObject::ProcessEvent, i.e. on the thread
        // executing the script VM, which for gameplay is the game thread. It is the
        // game-thread pump: register once, throttle inside, do all traversal there.
        // `UE4SSProgram::queue_event` is NOT an alternative - it queues onto the same
        // event-loop thread.
        void RegisterProcessEventPreCallback(std::function<void(UObject*, UFunction*, void*)> callback);
    } // namespace Hook
} // namespace RC::Unreal

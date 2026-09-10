#pragma once

//
// gamebinds - the player's real key bindings, read out of the running game.
//
// The game is stock UE 5.1 Enhanced Input with no user-settings object, so a remap in the
// options menu is visible in exactly one place: the flattened
// `UEnhancedPlayerInput::EnhancedActionMappings` array on the local player's `PlayerInput`.
// The shipped `UInputMappingContext` assets keep the defaults, so they are not read.
//
//     APlayerController.PlayerInput  (UEnhancedPlayerInput)
//       -> EnhancedActionMappings    TArray<FEnhancedActionKeyMapping>
//            .Action  UInputAction*  the identity of the binding (IP_FlashAtk)
//            .Key     FKey           .KeyName is the only readable field (NumPadFour)
//
// Every offset comes from reflection: the array property's offset from the class walk, the
// element stride from the `UScriptStruct`'s `GetPropertiesSize()`, and `Action` / `Key` /
// `KeyName` from walking that struct. Nothing is hand-mirrored, and the derived layout is
// logged and validated against the array's own span before a single row is read. `FKey` is not
// eight bytes - it caches a `TSharedPtr<FKeyDetails>` - so only the `FName` inside is touched.
//
// The array is rebuilt on every remap and the `UEnhancedPlayerInput` is recreated with the
// controller, so nothing is cached by index and the whole chain is re-resolved when the
// object dies. Rows repeat in both directions: one action can hold several keys and one key
// several actions, which is why the panel's question is only ever "does this key appear at all".
//
// THREADS: game thread runs game_thread_pump() / drop_caches() - raw reads and GetName()
// only; any thread runs table() / generation() - a spinlocked copy / atomic.
//

#include <cstdint>

namespace gb
{
    // 76 rows on this build, with room for a patch to add contexts.
    inline constexpr int kMaxRows = 128;
    inline constexpr int kKeyLen = 32;
    inline constexpr int kActionLen = 40;

    struct Row
    {
        char key[kKeyLen]{};       // the FKey name, e.g. "NumPadFour"
        char action[kActionLen]{}; // the UInputAction object name, e.g. "IP_FlashAtk"
    };

    struct Table
    {
        bool valid = false;     // false = the array has not been read this session
        bool truncated = false; // the game's array was longer than kMaxRows
        int rows = 0;           // bound rows kept; unbound (`None`) ones are dropped
        int unbound = 0;        // rows dropped because no key is bound to them
        char status[64]{};      // why it is not valid, or which route answered
        Row row[kMaxRows]{};
    };

    // ANY THREAD. Bumped whenever the published table changes; 0 until the first read.
    // The panel copies the table only when this moves.
    std::uint32_t generation();

    // ANY THREAD. ~9 KB, so callers cache it against generation().
    Table table();

    // GAME THREAD ONLY, from markers::game_thread_pump. Polls at 1 Hz.
    void game_thread_pump(std::uint64_t now);

    // GAME THREAD ONLY, from markers::drop_caches. Drops every cached pointer and offset.
    void drop_caches();
} // namespace gb

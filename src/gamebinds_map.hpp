#pragma once

//
// gamebinds_map - PURE. The dictionary between the game's Enhanced Input vocabulary and
// this mod's: an `FKey` name as the engine spells it (`NumPadFour`, `SpaceBar`,
// `LeftMouseButton`, `Gamepad_FaceButton_Bottom`) to the Windows virtual key a hotkey
// binding carries (mm::key_vk), plus the label the Keys tab shows. It also owns the
// static "keys something else probably wants" list the panel falls back on before the
// live table has been read.
//
// The FKey names are the ones the F5 recon dump printed; the engine's own headers are
// not the reference here. A name with no keyboard or mouse equivalent - every
// `Gamepad_*`, the mouse axes, the scroll wheel - maps to virtual key 0, which means
// "this can never clash with a mod hotkey".
//
// No Windows, no UE4SS, no D3D12: tests/markers_test.cpp links it. The virtual keys are
// written as numbers for that reason; src/gamebinds.cpp static_asserts them against the
// real VK_ macros.
//

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "lang_strings.hpp"

namespace gb
{
    using lang::S;

    struct KeyMap
    {
        const char* fkey; // the engine's FKey name
        int vk;           // Windows virtual key, 0 = no keyboard/mouse equivalent
        S label;          // S::Count = show the FKey name itself
    };

    // Everything the game can put in `EnhancedActionMappings` that a mod hotkey could
    // also be bound to, plus the gamepad names, which are here only for their labels.
    inline constexpr KeyMap kKeyMap[] = {
        {"A", 0x41, S::Count},   {"B", 0x42, S::Count},   {"C", 0x43, S::Count},   {"D", 0x44, S::Count},
        {"E", 0x45, S::Count},   {"F", 0x46, S::Count},   {"G", 0x47, S::Count},   {"H", 0x48, S::Count},
        {"I", 0x49, S::Count},   {"J", 0x4A, S::Count},   {"K", 0x4B, S::Count},   {"L", 0x4C, S::Count},
        {"M", 0x4D, S::Count},   {"N", 0x4E, S::Count},   {"O", 0x4F, S::Count},   {"P", 0x50, S::Count},
        {"Q", 0x51, S::Count},   {"R", 0x52, S::Count},   {"S", 0x53, S::Count},   {"T", 0x54, S::Count},
        {"U", 0x55, S::Count},   {"V", 0x56, S::Count},   {"W", 0x57, S::Count},   {"X", 0x58, S::Count},
        {"Y", 0x59, S::Count},   {"Z", 0x5A, S::Count},
        // The digit row is spelled out in words; the numpad is a different virtual key.
        {"Zero", 0x30, S::Key0},  {"One", 0x31, S::Key1},   {"Two", 0x32, S::Key2},
        {"Three", 0x33, S::Key3}, {"Four", 0x34, S::Key4},  {"Five", 0x35, S::Key5},
        {"Six", 0x36, S::Key6},   {"Seven", 0x37, S::Key7}, {"Eight", 0x38, S::Key8},
        {"Nine", 0x39, S::Key9},
        {"NumPadZero", 0x60, S::KeyNum0},  {"NumPadOne", 0x61, S::KeyNum1},
        {"NumPadTwo", 0x62, S::KeyNum2},   {"NumPadThree", 0x63, S::KeyNum3},
        {"NumPadFour", 0x64, S::KeyNum4},  {"NumPadFive", 0x65, S::KeyNum5},
        {"NumPadSix", 0x66, S::KeyNum6},   {"NumPadSeven", 0x67, S::KeyNum7},
        {"NumPadEight", 0x68, S::KeyNum8}, {"NumPadNine", 0x69, S::KeyNum9},
        {"Multiply", 0x6A, S::KeyNumMultiply},    {"Add", 0x6B, S::KeyNumAdd},
        {"Subtract", 0x6D, S::KeyNumSubtract},    {"Decimal", 0x6E, S::KeyNumDecimal},
        {"Divide", 0x6F, S::KeyNumDivide},
        {"F1", 0x70, S::Count},  {"F2", 0x71, S::Count},  {"F3", 0x72, S::Count},  {"F4", 0x73, S::Count},
        {"F5", 0x74, S::Count},  {"F6", 0x75, S::Count},  {"F7", 0x76, S::Count},  {"F8", 0x77, S::Count},
        {"F9", 0x78, S::Count},  {"F10", 0x79, S::Count}, {"F11", 0x7A, S::Count}, {"F12", 0x7B, S::Count},
        {"Escape", 0x1B, S::KeyEscape},        {"Tab", 0x09, S::KeyTab},
        {"SpaceBar", 0x20, S::KeySpace},    {"Enter", 0x0D, S::KeyEnter},
        {"BackSpace", 0x08, S::KeyBackspace}, {"CapsLock", 0x14, S::KeyCapsLock},
        {"LeftShift", 0xA0, S::KeyLeftShift},   {"RightShift", 0xA1, S::KeyRightShift},
        {"LeftControl", 0xA2, S::KeyLeftCtrl},  {"RightControl", 0xA3, S::KeyRightCtrl},
        {"LeftAlt", 0xA4, S::KeyLeftAlt},       {"RightAlt", 0xA5, S::KeyRightAlt},
        {"Up", 0x26, S::KeyUp},       {"Down", 0x28, S::KeyDown},
        {"Left", 0x25, S::KeyLeft},   {"Right", 0x27, S::KeyRight},
        {"Insert", 0x2D, S::KeyInsert},           {"Delete", 0x2E, S::KeyDelete},
        {"Home", 0x24, S::KeyHome},             {"End", 0x23, S::KeyEnd},
        {"PageUp", 0x21, S::KeyPageUp},    {"PageDown", 0x22, S::KeyPageDown},
        {"NumLock", 0x90, S::KeyNumLock},  {"ScrollLock", 0x91, S::KeyScrollLock},
        {"Pause", 0x13, S::KeyPause},
        {"Semicolon", 0xBA, S::KeySemicolon},       {"Equals", 0xBB, S::KeyEquals},
        {"Comma", 0xBC, S::KeyComma},           {"Hyphen", 0xBD, S::KeyHyphen},
        {"Period", 0xBE, S::KeyPeriod},          {"Slash", 0xBF, S::KeySlash},
        {"Tilde", 0xC0, S::KeyTilde},           {"LeftBracket", 0xDB, S::KeyLeftBracket},
        {"Backslash", 0xDC, S::KeyBackslash},      {"RightBracket", 0xDD, S::KeyRightBracket},
        {"Apostrophe", 0xDE, S::KeyApostrophe},
        {"LeftMouseButton", 0x01, S::KeyLeftMouse},
        {"RightMouseButton", 0x02, S::KeyRightMouse},
        {"MiddleMouseButton", 0x04, S::KeyMiddleMouse},
        {"ThumbMouseButton", 0x05, S::KeyMouse4},
        {"ThumbMouseButton2", 0x06, S::KeyMouse5},
        // No virtual key: a hotkey can never be bound to these, so they never clash.
        {"MouseScrollUp", 0, S::KeyScrollUp},   {"MouseScrollDown", 0, S::KeyScrollDown},
        {"MouseX", 0, S::KeyMouseX},            {"MouseY", 0, S::KeyMouseY},
        {"Mouse2D", 0, S::KeyMouse},
        {"Gamepad_FaceButton_Bottom", 0, S::KeyPadA},
        {"Gamepad_FaceButton_Right", 0, S::KeyPadB},
        {"Gamepad_FaceButton_Left", 0, S::KeyPadX},
        {"Gamepad_FaceButton_Top", 0, S::KeyPadY},
        {"Gamepad_LeftShoulder", 0, S::KeyPadLb},
        {"Gamepad_RightShoulder", 0, S::KeyPadRb},
        {"Gamepad_LeftTrigger", 0, S::KeyPadLt},
        {"Gamepad_RightTrigger", 0, S::KeyPadRt},
        {"Gamepad_LeftThumbstick", 0, S::KeyPadLs},
        {"Gamepad_RightThumbstick", 0, S::KeyPadRs},
        {"Gamepad_DPad_Up", 0, S::KeyPadDpadUp},
        {"Gamepad_DPad_Down", 0, S::KeyPadDpadDown},
        {"Gamepad_DPad_Left", 0, S::KeyPadDpadLeft},
        {"Gamepad_DPad_Right", 0, S::KeyPadDpadRight},
        {"Gamepad_Special_Left", 0, S::KeyPadView},
        {"Gamepad_Special_Right", 0, S::KeyPadMenu},
        {"Gamepad_LeftX", 0, S::KeyPadLeftX},
        {"Gamepad_LeftY", 0, S::KeyPadLeftY},
        {"Gamepad_RightX", 0, S::KeyPadRightX},
        {"Gamepad_RightY", 0, S::KeyPadRightY},
        {"Gamepad_Left2D", 0, S::KeyPadLeftStick},
        {"Gamepad_Right2D", 0, S::KeyPadRightStick},
    };

    inline constexpr std::size_t kKeyMapCount = sizeof(kKeyMap) / sizeof(kKeyMap[0]);

    // The engine's own "nothing is bound here" FName, which fills 18 of the 76 rows.
    inline constexpr const char* kNoneKey = "None";

    // 0 when the name is unknown, or is a stick / wheel / pad control no hotkey can use.
    constexpr int vk_for_key(std::string_view fkey)
    {
        for (std::size_t i = 0; i < kKeyMapCount; ++i)
        {
            if (fkey == kKeyMap[i].fkey)
            {
                return kKeyMap[i].vk;
            }
        }
        return 0;
    }

    // The FKey name for a virtual key, or nullptr when the game cannot name that key.
    constexpr const char* fkey_for_vk(int vk)
    {
        if (vk == 0)
        {
            return nullptr;
        }
        for (std::size_t i = 0; i < kKeyMapCount; ++i)
        {
            if (kKeyMap[i].vk == vk)
            {
                return kKeyMap[i].fkey;
            }
        }
        return nullptr;
    }

    // What the Keys tab prints for an FKey name, in the active language. Unknown names
    // print as they came.
    inline const char* label_for_key(const char* fkey)
    {
        if (fkey == nullptr)
        {
            return "";
        }
        for (std::size_t i = 0; i < kKeyMapCount; ++i)
        {
            if (std::string_view{fkey} == kKeyMap[i].fkey && kKeyMap[i].label != S::Count)
            {
                return lang::tr(kKeyMap[i].label);
            }
        }
        return fkey;
    }

    // `SHIFT` is the same physical press as `LeftShift` or `RightShift`: the config file
    // accepts the side-agnostic spelling and the game always names a side, so a bare
    // comparison would miss the clash. Symmetric.
    constexpr bool vk_matches(int a, int b)
    {
        if (a == 0 || b == 0)
        {
            return false;
        }
        if (a == b)
        {
            return true;
        }
        const auto pair_of = [](int side_agnostic, int lo, int hi, int x, int y) {
            return (x == side_agnostic && (y == lo || y == hi)) ||
                   (y == side_agnostic && (x == lo || x == hi));
        };
        return pair_of(0x10, 0xA0, 0xA1, a, b) || // VK_SHIFT
               pair_of(0x11, 0xA2, 0xA3, a, b) || // VK_CONTROL
               pair_of(0x12, 0xA4, 0xA5, a, b);   // VK_MENU
    }

    //=== Action names =============================================================
    // The mapping array identifies a binding by the `UInputAction` object's name; the
    // per-row `PlayerMappableOptions.Name` lies (`gamePadFlashAttack` sits on a keyboard
    // row). An action with no entry here shows its raw name.

    struct ActionLabel
    {
        const char* action;
        S label;
    };

    inline constexpr ActionLabel kActionLabels[] = {
        {"IP_Attack", S::ActAttack},
        {"IP_HeavyAttack", S::ActHeavyAttack},
        {"IP_FlashAtk", S::ActFlashAttack},
        {"IP_WeaponSkill", S::ActWeaponSkill},
        {"IP_WeaponStyleSkill", S::ActWeaponStyleSkill},
        {"IP_Roll", S::ActDodge},
        {"IP_Sprint", S::ActSprint},
        {"IP_Walking", S::ActWalk},
        {"IP_Inventory", S::ActPauseMenu},
        {"IP_UseTool", S::ActUseTool},
        {"IP_UseSpell", S::ActUseSpell},
        {"IP_OpenPosePanel", S::ActPosePanel},
        {"IP_QuickUse1", S::ActItemSlot1},
        {"IP_QuickUse2", S::ActItemSlot2},
        {"IP_QuickUse3", S::ActItemSlot3},
        {"IP_QuickUse4", S::ActItemSlot4},
        {"IP_QuickUseSpell1", S::ActSpellSlot1},
        {"IP_QuickUseSpell2", S::ActSpellSlot2},
        {"IP_QuickUseSpell3", S::ActSpellSlot3},
        {"IP_QuickUseSpell4", S::ActSpellSlot4},
        {"IP_PlayerMoveForward", S::ActMoveForward},
        {"IP_PlayerMoveForwardW", S::ActMoveForward},
        {"IP_PlayerMoveForwardS", S::ActMoveBack},
        {"IP_PlayerMoveRight", S::ActMoveRight},
        {"IP_PlayerMoveRightD", S::ActMoveRight},
        {"IP_PlayerMoveRightA", S::ActMoveLeft},
        {"IA_DrawWeapon", S::ActDrawWeapon},
        {"IA_ToggleTargeting", S::ActLockOn},
        {"IA_SwitchWeaponType", S::ActSwitchWeapon},
        {"IA_SwitchSpellType", S::ActSwitchSpell},
        {"IA_SwitchToolUp", S::ActNextTool},
        {"IA_SwitchToolDown", S::ActPreviousTool},
        {"IA_PlayerHLook", S::ActLook},
        {"IA_PlayerVLook", S::ActLook},
        {"IA_PlayerHLook_Mouse", S::ActLook},
        {"IA_PlayerVLook_Mouse", S::ActLook},
        {"IA_Alt", S::ActAltModifier},
        {"IA_Ctrl", S::ActCtrlModifier},
        {"IA_Shift", S::ActShiftModifier},
        {"IP_Thumbstick_Right", S::ActRightStick},
    };

    inline constexpr std::size_t kActionLabelCount =
        sizeof(kActionLabels) / sizeof(kActionLabels[0]);

    // In the active language; an action this table does not know shows its raw name.
    inline const char* action_label(const char* action)
    {
        if (action == nullptr)
        {
            return "";
        }
        for (std::size_t i = 0; i < kActionLabelCount; ++i)
        {
            if (std::string_view{action} == kActionLabels[i].action)
            {
                return lang::tr(kActionLabels[i].label);
            }
        }
        return action;
    }

    // Every game action bound to `vk`, as one comma-separated line, or "" for none.
    // `rows` is any array whose elements have `key` and `action` members readable as
    // C strings - gb::Row at runtime, a literal table in the tests. One key legitimately
    // drives several actions and one action several keys, so a label is printed once,
    // and the line is capped so a stick axis cannot fill the panel.
    template <typename RowT>
    inline std::string clash_text(const RowT* rows, int count, int vk)
    {
        if (rows == nullptr || vk == 0)
        {
            return {};
        }
        constexpr int kMaxNamed = 4;
        const char* seen[kMaxNamed]{};
        int named = 0;
        std::string out;
        for (int i = 0; i < count; ++i)
        {
            if (!vk_matches(vk, vk_for_key(rows[i].key)))
            {
                continue;
            }
            const char* label = action_label(rows[i].action);
            if (label[0] == '\0')
            {
                label = lang::tr(S::ActUnnamed);
            }
            bool dupe = false;
            for (int j = 0; j < named; ++j)
            {
                dupe = dupe || std::strcmp(seen[j], label) == 0;
            }
            if (dupe)
            {
                continue;
            }
            if (named == kMaxNamed)
            {
                out += lang::tr(S::ListMore);
                break;
            }
            if (named != 0)
            {
                out += lang::tr(S::ListSep);
            }
            out += label;
            seen[named++] = label;
        }
        return out;
    }

    //=== The fallback list ========================================================
    // Used only while the live mapping array has not been read - at the main menu, or on
    // a build where the walk stopped answering. It is a guess about a default keyboard
    // layout, so it is deliberately vague where the game is; the last entries are other
    // injected software, which the live array can never know about.

    struct FallbackBind
    {
        int vk;
        S what;
    };

    inline constexpr FallbackBind kFallbackBinds[] = {
        {0x57, S::ActMoveForward},      {0x41, S::ActMoveLeft},
        {0x53, S::ActMoveBack},         {0x44, S::ActMoveRight},
        {0x20, S::ActDodge},             {0x10, S::ActSprint},
        {0xA0, S::ActSprint},            {0x11, S::ActWalk},
        {0xA2, S::ActWalk},              {0x45, S::ActInteract},
        {0x46, S::ActAnActionBind},    {0x51, S::ActAnActionBind},
        {0x52, S::ActAnActionBind},    {0x47, S::ActAnActionBind},
        {0x09, S::ActInventory},         {0x1B, S::ActPauseMenu},
        {0x31, S::ActAnItemSlot},      {0x32, S::ActAnItemSlot},
        {0x33, S::ActAnItemSlot},      {0x34, S::ActAnItemSlot},
        {0x35, S::ActAnItemSlot},
        {0x75, S::ActRenoDx},
        {0x78, S::ActEngineScreenshot},
        {0x79, S::ActUe4ssConsole},
        {0x7A, S::ActEngineFullscreen},
        {0x7B, S::ActSteamScreenshot},
    };

    inline constexpr std::size_t kFallbackBindCount =
        sizeof(kFallbackBinds) / sizeof(kFallbackBinds[0]);

    // In the active language; nullptr = nothing known wants this virtual key.
    inline const char* fallback_clash(int vk)
    {
        for (std::size_t i = 0; i < kFallbackBindCount; ++i)
        {
            if (kFallbackBinds[i].vk == vk)
            {
                return lang::tr(kFallbackBinds[i].what);
            }
        }
        return nullptr;
    }
} // namespace gb

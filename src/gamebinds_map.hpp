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

namespace gb
{
    struct KeyMap
    {
        const char* fkey;   // the engine's FKey name
        int vk;             // Windows virtual key, 0 = no keyboard/mouse equivalent
        const char* label;  // "" = show the FKey name itself
    };

    // Everything the game can put in `EnhancedActionMappings` that a mod hotkey could
    // also be bound to, plus the gamepad names, which are here only for their labels.
    inline constexpr KeyMap kKeyMap[] = {
        {"A", 0x41, ""},   {"B", 0x42, ""},   {"C", 0x43, ""},   {"D", 0x44, ""},
        {"E", 0x45, ""},   {"F", 0x46, ""},   {"G", 0x47, ""},   {"H", 0x48, ""},
        {"I", 0x49, ""},   {"J", 0x4A, ""},   {"K", 0x4B, ""},   {"L", 0x4C, ""},
        {"M", 0x4D, ""},   {"N", 0x4E, ""},   {"O", 0x4F, ""},   {"P", 0x50, ""},
        {"Q", 0x51, ""},   {"R", 0x52, ""},   {"S", 0x53, ""},   {"T", 0x54, ""},
        {"U", 0x55, ""},   {"V", 0x56, ""},   {"W", 0x57, ""},   {"X", 0x58, ""},
        {"Y", 0x59, ""},   {"Z", 0x5A, ""},
        // The digit row is spelled out in words; the numpad is a different virtual key.
        {"Zero", 0x30, "0"},  {"One", 0x31, "1"},   {"Two", 0x32, "2"},
        {"Three", 0x33, "3"}, {"Four", 0x34, "4"},  {"Five", 0x35, "5"},
        {"Six", 0x36, "6"},   {"Seven", 0x37, "7"}, {"Eight", 0x38, "8"},
        {"Nine", 0x39, "9"},
        {"NumPadZero", 0x60, "Num 0"},  {"NumPadOne", 0x61, "Num 1"},
        {"NumPadTwo", 0x62, "Num 2"},   {"NumPadThree", 0x63, "Num 3"},
        {"NumPadFour", 0x64, "Num 4"},  {"NumPadFive", 0x65, "Num 5"},
        {"NumPadSix", 0x66, "Num 6"},   {"NumPadSeven", 0x67, "Num 7"},
        {"NumPadEight", 0x68, "Num 8"}, {"NumPadNine", 0x69, "Num 9"},
        {"Multiply", 0x6A, "Num *"},    {"Add", 0x6B, "Num +"},
        {"Subtract", 0x6D, "Num -"},    {"Decimal", 0x6E, "Num ."},
        {"Divide", 0x6F, "Num /"},
        {"F1", 0x70, ""},  {"F2", 0x71, ""},  {"F3", 0x72, ""},  {"F4", 0x73, ""},
        {"F5", 0x74, ""},  {"F6", 0x75, ""},  {"F7", 0x76, ""},  {"F8", 0x77, ""},
        {"F9", 0x78, ""},  {"F10", 0x79, ""}, {"F11", 0x7A, ""}, {"F12", 0x7B, ""},
        {"Escape", 0x1B, "Esc"},        {"Tab", 0x09, ""},
        {"SpaceBar", 0x20, "Space"},    {"Enter", 0x0D, ""},
        {"BackSpace", 0x08, "Backspace"}, {"CapsLock", 0x14, "Caps Lock"},
        {"LeftShift", 0xA0, "Left Shift"},   {"RightShift", 0xA1, "Right Shift"},
        {"LeftControl", 0xA2, "Left Ctrl"},  {"RightControl", 0xA3, "Right Ctrl"},
        {"LeftAlt", 0xA4, "Left Alt"},       {"RightAlt", 0xA5, "Right Alt"},
        {"Up", 0x26, "Up arrow"},       {"Down", 0x28, "Down arrow"},
        {"Left", 0x25, "Left arrow"},   {"Right", 0x27, "Right arrow"},
        {"Insert", 0x2D, ""},           {"Delete", 0x2E, ""},
        {"Home", 0x24, ""},             {"End", 0x23, ""},
        {"PageUp", 0x21, "Page Up"},    {"PageDown", 0x22, "Page Down"},
        {"NumLock", 0x90, "Num Lock"},  {"ScrollLock", 0x91, "Scroll Lock"},
        {"Pause", 0x13, ""},
        {"Semicolon", 0xBA, ";"},       {"Equals", 0xBB, "="},
        {"Comma", 0xBC, ","},           {"Hyphen", 0xBD, "-"},
        {"Period", 0xBE, "."},          {"Slash", 0xBF, "/"},
        {"Tilde", 0xC0, "`"},           {"LeftBracket", 0xDB, "["},
        {"Backslash", 0xDC, "\\"},      {"RightBracket", 0xDD, "]"},
        {"Apostrophe", 0xDE, "'"},
        {"LeftMouseButton", 0x01, "Left mouse"},
        {"RightMouseButton", 0x02, "Right mouse"},
        {"MiddleMouseButton", 0x04, "Middle mouse"},
        {"ThumbMouseButton", 0x05, "Mouse 4"},
        {"ThumbMouseButton2", 0x06, "Mouse 5"},
        // No virtual key: a hotkey can never be bound to these, so they never clash.
        {"MouseScrollUp", 0, "Scroll up"},   {"MouseScrollDown", 0, "Scroll down"},
        {"MouseX", 0, "Mouse X"},            {"MouseY", 0, "Mouse Y"},
        {"Mouse2D", 0, "Mouse"},
        {"Gamepad_FaceButton_Bottom", 0, "Pad A"},
        {"Gamepad_FaceButton_Right", 0, "Pad B"},
        {"Gamepad_FaceButton_Left", 0, "Pad X"},
        {"Gamepad_FaceButton_Top", 0, "Pad Y"},
        {"Gamepad_LeftShoulder", 0, "Pad LB"},
        {"Gamepad_RightShoulder", 0, "Pad RB"},
        {"Gamepad_LeftTrigger", 0, "Pad LT"},
        {"Gamepad_RightTrigger", 0, "Pad RT"},
        {"Gamepad_LeftThumbstick", 0, "Pad LS"},
        {"Gamepad_RightThumbstick", 0, "Pad RS"},
        {"Gamepad_DPad_Up", 0, "Pad D-pad up"},
        {"Gamepad_DPad_Down", 0, "Pad D-pad down"},
        {"Gamepad_DPad_Left", 0, "Pad D-pad left"},
        {"Gamepad_DPad_Right", 0, "Pad D-pad right"},
        {"Gamepad_Special_Left", 0, "Pad View"},
        {"Gamepad_Special_Right", 0, "Pad Menu"},
        {"Gamepad_LeftX", 0, "Pad left stick X"},
        {"Gamepad_LeftY", 0, "Pad left stick Y"},
        {"Gamepad_RightX", 0, "Pad right stick X"},
        {"Gamepad_RightY", 0, "Pad right stick Y"},
        {"Gamepad_Left2D", 0, "Pad left stick"},
        {"Gamepad_Right2D", 0, "Pad right stick"},
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

    // What the Keys tab prints for an FKey name. Unknown names print as they came.
    constexpr const char* label_for_key(const char* fkey)
    {
        if (fkey == nullptr)
        {
            return "";
        }
        for (std::size_t i = 0; i < kKeyMapCount; ++i)
        {
            if (std::string_view{fkey} == kKeyMap[i].fkey && kKeyMap[i].label[0] != '\0')
            {
                return kKeyMap[i].label;
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
        const char* label;
    };

    inline constexpr ActionLabel kActionLabels[] = {
        {"IP_Attack", "attack"},
        {"IP_HeavyAttack", "heavy attack"},
        {"IP_FlashAtk", "flash attack"},
        {"IP_WeaponSkill", "weapon skill"},
        {"IP_WeaponStyleSkill", "weapon style skill"},
        {"IP_Roll", "dodge"},
        {"IP_Sprint", "sprint"},
        {"IP_Walking", "walk"},
        {"IP_Inventory", "the pause menu"},
        {"IP_UseTool", "use tool"},
        {"IP_UseSpell", "use spell"},
        {"IP_OpenPosePanel", "the pose panel"},
        {"IP_QuickUse1", "item slot 1"},
        {"IP_QuickUse2", "item slot 2"},
        {"IP_QuickUse3", "item slot 3"},
        {"IP_QuickUse4", "item slot 4"},
        {"IP_QuickUseSpell1", "spell slot 1"},
        {"IP_QuickUseSpell2", "spell slot 2"},
        {"IP_QuickUseSpell3", "spell slot 3"},
        {"IP_QuickUseSpell4", "spell slot 4"},
        {"IP_PlayerMoveForward", "move forward"},
        {"IP_PlayerMoveForwardW", "move forward"},
        {"IP_PlayerMoveForwardS", "move back"},
        {"IP_PlayerMoveRight", "move right"},
        {"IP_PlayerMoveRightD", "move right"},
        {"IP_PlayerMoveRightA", "move left"},
        {"IA_DrawWeapon", "draw weapon"},
        {"IA_ToggleTargeting", "lock on"},
        {"IA_SwitchWeaponType", "switch weapon"},
        {"IA_SwitchSpellType", "switch spell"},
        {"IA_SwitchToolUp", "next tool"},
        {"IA_SwitchToolDown", "previous tool"},
        {"IA_PlayerHLook", "look"},
        {"IA_PlayerVLook", "look"},
        {"IA_PlayerHLook_Mouse", "look"},
        {"IA_PlayerVLook_Mouse", "look"},
        {"IA_Alt", "the Alt modifier"},
        {"IA_Ctrl", "the Ctrl modifier"},
        {"IA_Shift", "the Shift modifier"},
        {"IP_Thumbstick_Right", "the right stick"},
    };

    inline constexpr std::size_t kActionLabelCount =
        sizeof(kActionLabels) / sizeof(kActionLabels[0]);

    constexpr const char* action_label(const char* action)
    {
        if (action == nullptr)
        {
            return "";
        }
        for (std::size_t i = 0; i < kActionLabelCount; ++i)
        {
            if (std::string_view{action} == kActionLabels[i].action)
            {
                return kActionLabels[i].label;
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
                label = "an unnamed action";
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
                out += ", ...";
                break;
            }
            if (named != 0)
            {
                out += ", ";
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
        const char* what;
    };

    inline constexpr FallbackBind kFallbackBinds[] = {
        {0x57, "move forward"},      {0x41, "move left"},
        {0x53, "move back"},         {0x44, "move right"},
        {0x20, "dodge"},             {0x10, "sprint"},
        {0xA0, "sprint"},            {0x11, "walk"},
        {0xA2, "walk"},              {0x45, "interact"},
        {0x46, "an action bind"},    {0x51, "an action bind"},
        {0x52, "an action bind"},    {0x47, "an action bind"},
        {0x09, "inventory"},         {0x1B, "the pause menu"},
        {0x31, "an item slot"},      {0x32, "an item slot"},
        {0x33, "an item slot"},      {0x34, "an item slot"},
        {0x35, "an item slot"},
        {0x75, "RenoDX / DLSS 5 (it ignores modifiers)"},
        {0x78, "an engine screenshot bind"},
        {0x79, "the UE4SS console"},
        {0x7A, "the engine fullscreen bind"},
        {0x7B, "the Steam screenshot key"},
    };

    inline constexpr std::size_t kFallbackBindCount =
        sizeof(kFallbackBinds) / sizeof(kFallbackBinds[0]);

    // nullptr = nothing known wants this virtual key.
    constexpr const char* fallback_clash(int vk)
    {
        for (std::size_t i = 0; i < kFallbackBindCount; ++i)
        {
            if (kFallbackBinds[i].vk == vk)
            {
                return kFallbackBinds[i].what;
            }
        }
        return nullptr;
    }
} // namespace gb

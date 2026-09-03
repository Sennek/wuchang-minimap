-- WuchangMinimap - UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, x64, DX12)
--
-- This project does NOT include("RE-UE4SS") the way the official docs describe,
-- because UE4SS cannot be built from source on this machine: its `deps/first/Unreal`
-- submodule points at `Re-UE4SS/UEPseudo`, a private repository that requires Epic
-- Games GitHub organisation access. See README.md for the full story.
--
-- Instead we compile against the headers of an RE-UE4SS checkout pinned to the exact
-- commit of the UE4SS build installed in the game, and link against an import library
-- synthesised from that build's UE4SS.dll export table (tools/gen_ue4ss_importlib.ps1).

set_project("WuchangMinimap")
-- Metadata only. The version the DLL, the F2 panel and tools/package.ps1 all use is
-- the single #define in src/version.hpp; package.ps1 -Version rewrites both.
set_version("1.0.0")
set_xmakever("2.9.3")

set_allowedplats("windows")
set_allowedarchs("x64")

-- UE4SS names its shipping configuration Game__Shipping__Win64; we keep that name so
-- the build command matches the UE4SS documentation.
set_allowedmodes("Game__Shipping__Win64", "Game__Debug__Win64")
set_defaultmode("Game__Shipping__Win64")

-- Machine-specific: where the RE-UE4SS checkout lives. The default is the original dev
-- box; set the WUCHANG_UE4SS_ROOT environment variable, or pass
-- `--ue4ss_root=<path>` to `xmake f` (build.ps1 -Ue4ssRoot does this), to move it.
local ue4ss_default = os.getenv("WUCHANG_UE4SS_ROOT") or "F:/Tools/RE-UE4SS"

option("ue4ss_root")
    set_default(ue4ss_default)
    set_showmenu(true)
    set_description("Path to the RE-UE4SS checkout (pinned to the commit of the installed UE4SS.dll).")
option_end()

local ue4ss_root = get_config("ue4ss_root") or ue4ss_default

-- Every UE4SS header we need, in lookup order. sdk/shim MUST come first: it supplies a
-- stand-in for <GUI/GUI.hpp> whose real version drags in the unavailable UEPseudo headers.
local function ue4ss_includedirs()
    local first = ue4ss_root .. "/deps/first/"
    return {
        "sdk/shim",
        ue4ss_root .. "/UE4SS/include",
        ue4ss_root .. "/UE4SS/generated_include",
        first .. "File/include",
        first .. "DynamicOutput/include",
        first .. "String/include",
        first .. "Input/include",
        first .. "Constructs/include",
        first .. "Helpers/include",
        first .. "Function/include",
        first .. "SinglePassSigScanner/include",
        first .. "ASMHelper/include",
        first .. "IniParser/include",
        first .. "JSON/include",
        first .. "MProgram/include",
        first .. "ScopedTimer/include",
        first .. "Profiler/include",
        -- fmt 11.2.0, the version UE4SS itself uses. DynamicOutput/Output.hpp includes
        -- <fmt/core.h>; the formatting happens inside our DLL so header-only is fine.
        "third_party/fmt/include",
    }
end

-- Shared compile settings for every target in this project.
-- The CRT choice is load bearing: the shipped UE4SS.dll imports MSVCP140.dll and
-- VCRUNTIME140.dll, so the mod must use the dynamic release CRT (/MD) or the two will
-- disagree about std:: types across the DLL boundary.
local function common_settings()
    set_arch("x64")
    set_plat("windows")
    set_runtimes("MD")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "UNICODE", "_UNICODE")
    -- Required, not cosmetic: fmt's base.h has a `static_assert(... "Unicode support
    -- requires compiling with /utf-8")` that fires on MSVC's default codepage.
    add_cxflags("/utf-8", {tools = {"cl"}})
    add_cflags("/utf-8", {tools = {"cl"}})
    if is_mode("Game__Debug__Win64") then
        set_optimize("none")
        set_symbols("debug")
    else
        set_optimize("fastest")
        set_symbols("debug") -- keep the .pdb: it is what makes a crash dump readable
        add_defines("NDEBUG")
    end
end

----------------------------------------------------------------------------------------
-- third_party: Dear ImGui (core + DX12 and Win32 backends)
----------------------------------------------------------------------------------------
target("imgui")
    set_kind("static")
    set_languages("cxx20")
    set_group("third_party")
    common_settings()
    add_includedirs("third_party/imgui", {public = true})
    -- XINPUT IS THE LOOP THREAD'S JOB (lessons.md, and src/gamepad.cpp is where it is
    -- done). With ImGuiConfigFlags_NavEnableGamepad set, imgui_impl_win32 polls XInput
    -- itself from ImGui_ImplWin32_NewFrame - i.e. from inside Present, where polling an
    -- empty slot costs about a millisecond. The mod already has the pad state and feeds
    -- it into io itself (overlay.cpp, feed_pad_nav), so the backend's copy is switched
    -- off rather than left to duplicate the work on the wrong thread.
    add_defines("IMGUI_IMPL_WIN32_DISABLE_GAMEPAD", {public = true})
    add_files(
        "third_party/imgui/imgui.cpp",
        "third_party/imgui/imgui_draw.cpp",
        "third_party/imgui/imgui_tables.cpp",
        "third_party/imgui/imgui_widgets.cpp",
        "third_party/imgui/imgui_demo.cpp",
        "third_party/imgui/misc/cpp/imgui_stdlib.cpp",
        "third_party/imgui/backends/imgui_impl_dx12.cpp",
        "third_party/imgui/backends/imgui_impl_win32.cpp")
    -- imgui_impl_dx12 calls D3D12SerializeRootSignature; imgui_impl_win32 pokes dwmapi.
    add_syslinks("d3d12", "dxgi", "d3dcompiler", "dwmapi", {public = true})
target_end()

----------------------------------------------------------------------------------------
-- third_party: MinHook
----------------------------------------------------------------------------------------
target("minhook")
    set_kind("static")
    set_languages("c11")
    set_group("third_party")
    common_settings()
    add_includedirs("third_party/minhook/include", {public = true})
    add_files("third_party/minhook/src/*.c", "third_party/minhook/src/hde/*.c")
target_end()

----------------------------------------------------------------------------------------
-- The mod itself. UE4SS loads <mod>/dlls/main.dll, so the basename is `main`.
----------------------------------------------------------------------------------------
target("WuchangMinimap")
    set_kind("shared")
    set_basename("main")
    set_languages("cxx23")
    set_exceptions("cxx")
    set_group("mods")
    set_warnings("all")
    common_settings()
    add_deps("imgui", "minhook")

    add_includedirs("src")
    add_includedirs(ue4ss_includedirs())

    -- fmt must be header-only here: we have no compiled fmt from the UE4SS build.
    -- RC_IS_ANSI selects the char type used by DynamicOutput; UE4SS ships the wide build.
    -- The codecvt silencer is for UE4SS's own Helpers/String.hpp, which still uses the
    -- C++17-deprecated std::wstring_convert; it is the only C4996 in the build.
    add_defines("FMT_HEADER_ONLY=1", "RC_IS_ANSI=0", "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING")

    add_files("src/*.cpp")

    -- Import library synthesised from the installed UE4SS.dll export table.
    add_links("sdk/lib/UE4SS.lib")

    -- windowscodecs/ole32: WIC decodes the chapter PNGs (src/mapdata.cpp) - no
    -- vendored image decoder needed. d3d12/dxgi come in publicly from the imgui
    -- target, but the overlay calls D3D12CreateDevice / CreateDXGIFactory1 itself.
    -- version: GetFileVersionInfoW, for the startup bug-report header's game-exe and
    -- UE4SS build numbers.
    add_syslinks("d3d12", "dxgi", "windowscodecs", "ole32", "version")

    after_build(function (target)
        print("WuchangMinimap -> %s", target:targetfile())
    end)
target_end()

----------------------------------------------------------------------------------------
-- Offline tests. Everything about the markers that does NOT need the engine - the
-- markers/<chapter>.json loader, the category filter mask the config file and the F2
-- checkboxes share, and the wuchang_minimap_found.txt round-trip - is verified here,
-- in a console exe that links only src/markers_db.cpp. No UE4SS, no D3D12, no
-- UE4SS.lib: it runs on the build machine while the game is closed, which is the whole
-- reason markers_db.cpp is kept free of Windows and Unreal.
--
--     xmake build markers_test && xmake run markers_test markers
----------------------------------------------------------------------------------------
target("markers_test")
    set_kind("binary")
    set_languages("cxx23")
    set_exceptions("cxx")
    set_group("tests")
    set_warnings("all")
    set_default(false) -- built explicitly (and by build.ps1), not by a bare `xmake`
    common_settings()
    add_includedirs("src")
    add_files("src/markers_db.cpp", "src/mapview.cpp", "src/compass.cpp", "tests/markers_test.cpp")
target_end()

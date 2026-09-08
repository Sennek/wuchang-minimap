-- WuchangMinimap - UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, x64, DX12)
--
-- No include("RE-UE4SS"): UE4SS cannot be built from source here, since its
-- `deps/first/Unreal` submodule points at the private `Re-UE4SS/UEPseudo`. The build
-- compiles against the headers of an RE-UE4SS checkout pinned to the commit of the
-- installed UE4SS build, and links an import library synthesised from that build's
-- UE4SS.dll export table (tools/gen_ue4ss_importlib.ps1).

set_project("WuchangMinimap")
-- Metadata only; src/version.hpp holds the version everything else reads.
-- package.ps1 -Version rewrites both.
set_version("1.1.0")
-- The tested xmake version; docs/DEVELOPMENT.md quotes the same number.
set_xmakever("3.1.1")

set_allowedplats("windows")
set_allowedarchs("x64")

-- UE4SS's own configuration names, so the build command matches its documentation.
set_allowedmodes("Game__Shipping__Win64", "Game__Debug__Win64")
set_defaultmode("Game__Shipping__Win64")

-- Machine-specific. Override with WUCHANG_UE4SS_ROOT or `xmake f --ue4ss_root=<path>`
-- (build.ps1 -Ue4ssRoot).
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
        -- fmt 11.2.0, the version UE4SS uses. DynamicOutput/Output.hpp includes
        -- <fmt/core.h>; formatting happens inside this DLL, so header-only is fine.
        "third_party/fmt/include",
    }
end

-- Shared compile settings. The CRT choice is load bearing: the shipped UE4SS.dll
-- imports MSVCP140.dll and VCRUNTIME140.dll, so the mod needs the dynamic release CRT
-- (/MD) or the two disagree about std:: types across the DLL boundary.
local function common_settings()
    set_arch("x64")
    set_plat("windows")
    set_runtimes("MD")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "UNICODE", "_UNICODE")
    -- Required: fmt's base.h static_asserts on MSVC's default codepage.
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

-- Hardening + reproducibility flags for the two LINKED targets (the mod DLL and the
-- offline test exe). Not for the static third_party targets: xmake hands their ldflags
-- to lib.exe.
--
-- /guard:cf - Control Flow Guard, safe in a mod that hooks the host process: it only
--   rewrites this module's indirect calls, MinHook's VirtualAlloc'd trampolines are
--   legal targets unless the process enables CFG strict mode, and the game calling our
--   hook validates against its own bitmap, not ours.
-- /DYNAMICBASE /HIGHENTROPYVA - ASLR at full 64-bit entropy. MSVC x64 defaults, stated
--   so a flag change cannot silently drop them and `dumpbin /headers` shows the intent.
-- /PDBALTPATH:%_PDB% - put only the PDB's file NAME in the debug directory, not the
--   builder's absolute path. A local debugger still finds it; a symbol server works by
--   GUID.
--
-- Two xmake traps:
--   1. A `{tools = {"link"}}` filter on add_ldflags does NOT match the MSVC linker -
--      xmake silently drops every flag carrying one. The project is
--      set_allowedplats("windows"), so unconditional MSVC flags are safe.
--   2. add_ldflags is for BINARY targets; a shared library takes add_shflags.
-- Verify, do not assume:
--     xmake -v -r                     the link.exe line must show all four flags
--     strings main.dll | grep pdb     must be bare "main.pdb", not a build path
local function hardened_link()
    add_cxflags("/guard:cf", {tools = {"cl"}})
    -- Both spellings: xmake routes an .exe through ldflags and a .dll through shflags.
    local link = {"/guard:cf", "/DYNAMICBASE", "/HIGHENTROPYVA", "/PDBALTPATH:%_PDB%"}
    add_ldflags(link, {force = true})
    add_shflags(link, {force = true})
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
    -- XInput is the loop thread's job (src/gamepad.cpp). With
    -- ImGuiConfigFlags_NavEnableGamepad set, imgui_impl_win32 would poll XInput from
    -- ImGui_ImplWin32_NewFrame, i.e. inside Present, where an empty slot costs about a
    -- millisecond. overlay.cpp feeds the already-polled pad state into io instead.
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
    -- "error" = /WX. src/ must stay warning-free; third_party/ builds under its own
    -- targets at the default level.
    set_warnings("all", "error")
    common_settings()
    hardened_link()
    add_deps("imgui", "minhook")

    add_includedirs("src")
    add_includedirs(ue4ss_includedirs())

    -- fmt is header-only: there is no compiled fmt from the UE4SS build. RC_IS_ANSI
    -- picks DynamicOutput's char type, and UE4SS ships the wide build. The codecvt
    -- silencer covers UE4SS's Helpers/String.hpp, the build's only C4996.
    add_defines("FMT_HEADER_ONLY=1", "RC_IS_ANSI=0", "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING")

    add_files("src/*.cpp")

    -- Import library synthesised from the installed UE4SS.dll export table.
    add_links("sdk/lib/UE4SS.lib")

    -- windowscodecs/ole32: WIC decodes the chapter PNGs. d3d12/dxgi arrive publicly
    -- from the imgui target, but the overlay calls D3D12CreateDevice /
    -- CreateDXGIFactory1 itself. version: GetFileVersionInfoW for the bug-report header.
    add_syslinks("d3d12", "dxgi", "windowscodecs", "ole32", "version")

    after_build(function (target)
        print("WuchangMinimap -> %s", target:targetfile())
    end)
target_end()

----------------------------------------------------------------------------------------
-- Offline tests: everything about the markers that does not need the engine - the
-- markers/<chapter>.json loader, the shared category filter mask, the
-- wuchang_minimap_found.txt round-trip. No UE4SS, no D3D12, no UE4SS.lib, so it runs
-- on the build machine with the game closed.
--
--     xmake build markers_test && xmake run markers_test markers
----------------------------------------------------------------------------------------
target("markers_test")
    set_kind("binary")
    set_languages("cxx23")
    set_exceptions("cxx")
    set_group("tests")
    set_warnings("all", "error")
    set_default(false) -- built explicitly (and by build.ps1), not by a bare `xmake`
    common_settings()
    hardened_link()
    add_includedirs("src")
    add_files("src/markers_db.cpp", "src/mapview.cpp", "src/compass.cpp", "tests/markers_test.cpp")
    -- The tests' one Windows dependency: src/pngdecode.hpp, so test_map_assets() can
    -- decode the shipped map PNGs through exactly the code the mod uses.
    add_syslinks("windowscodecs", "ole32")
target_end()

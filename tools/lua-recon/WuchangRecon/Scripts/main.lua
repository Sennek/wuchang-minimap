--[[
    WuchangRecon - UE4SS Lua recon mod for Wuchang: Fallen Feathers (UE 5.1.1)
    Part of the wuchang-minimap project. Read-only: it never calls a setter, never
    spawns or destroys anything. Every section is wrapped in pcall so a failing
    reflection call can never take the game down.

    Hotkeys - each is registered twice, plain and with CTRL, so if the game or the
    engine swallows the plain key the CTRL variant still works.
    (CTRL+O stays UE4SS' own GUI toggle.)
      F8  / CTRL+F8  - world dump     (player, camera, view target, world/levels/
                                       streaming, actor census, keyword-filtered
                                       actors, MARKER VALUES, function signatures,
                                       navmesh, DebugCommand_C)
      F9  / CTRL+F9  - UI dump        (all UUserWidget instances + visibility)
      F7  / CTRL+F7  - tracker toggle (one CSV line per second to out\track.csv)
      F11 / CTRL+F11 - navmesh probe  (41x41 ProjectPointToNavigation grid)
      F12 / CTRL+F12 - pickup watch   (toggle; snapshots pickup/chest actors every
                                       2 s and logs actors that vanished and props
                                       that changed - settles "collected" vs
                                       "destroyed" when you pick something up)

    NOT F10: UE4SS' ConsoleEnablerMod registers F10 as one of the game's console
    keys on this build, so F10 would open the UE console as well.
    Plain F9 may also hit the engine screenshot bind and plain F11 the engine
    fullscreen toggle - the CTRL variants avoid both.
    NOT F6 either: that is the WuchangMinimap C++ mod's navmesh-dump hotkey.

    Output: <mod dir>\out\dump_<yyyymmdd_hhmmss>_<kind>.txt
            <mod dir>\out\navprobe_<yyyymmdd_hhmmss>.csv
            <mod dir>\out\track.csv
            <mod dir>\out\pickupwatch_<yyyymmdd_hhmmss>.txt
    A one-line confirmation for every dump goes to ue4ss\UE4SS.log.
--]]

local UEHelpers = require("UEHelpers")

local MOD_NAME = "WuchangRecon"

--------------------------------------------------------------------------------
-- tunables
--------------------------------------------------------------------------------
local CFG = {
    -- actor census / keyword filter
    MAX_ACTORS_SCANNED       = 200000, -- hard stop, safety only
    MAX_MATCHES_PER_CLASS    = 60,     -- per matching class, print at most this many instances
    MAX_MATCH_LINES          = 8000,   -- overall cap on filtered-actor lines
    -- marker section (class + VALUES + distance to player)
    MAX_MARKERS_PER_CLASS    = 200,    -- per marker class, print at most this many instances
    MAX_VALUE_PROPS          = 40,     -- scalar props printed per marker instance
    -- pickup watch (F12)
    WATCH_PERIOD_MS          = 2000,
    WATCH_MAX_ACTORS         = 4000,
    -- property dumps
    MAX_PROPS_PER_STRUCT     = 400,
    MAX_CLASS_CHAIN          = 40,
    -- navmesh probe grid
    PROBE_HALF_STEPS         = 20,     -- 20 => 41x41
    PROBE_STEP_UU            = 100.0,
    PROBE_EXTENT             = { X = 100.0, Y = 100.0, Z = 500.0 },
    -- tracker
    TRACK_PERIOD_MS          = 1000,
    -- automatic menu-time dump
    AUTO_FIRST_DELAY_S       = 20,
    AUTO_POLL_S              = 5,
    AUTO_GIVE_UP_S           = 180,
}

-- class OR full name substrings that mark an actor as "interesting" (lowercase).
-- The game's own Pinyin/Chinese vocabulary MUST be in here or whole marker
-- categories go missing with no warning (BP_Wumen_C matched no English keyword in
-- the first recon pass and got zero positions).
local KEYWORDS = {
    "shrine", "fire", "altar", "save", "chest", "box", "treasure", "item",
    "pickup", "loot", "drop", "collect", "boss", "enemy", "monster", "npc",
    "merchant", "shop", "door", "gate", "ladder", "elevator", "lift", "rope",
    "portal", "teleport", "trigger", "volume", "nav", "recast", "camera",
    "spline",
    -- added 2026-09-02
    "wumen", "fog", "mist", "plume", "transit", "puzzle",
    "rebornfire", "digong", "dici", "zhuanjing", "qicaishi",
}

-- Marker classes that get the full treatment in the MARKERS section: every scalar
-- property VALUE plus the distance to the player. A dump that lists property names
-- cannot answer "is this chest already opened?" - only values can.
local MARKER_CLASSES = {
    -- pickups
    "BP_PickupActor_C", "BP_PickUpPT_C", "BP_AutoPickUp_C", "ItemCollectionBox_C",
    -- chests
    "BP_treasurebox_C", "BP_ItemRedBox_C",
    -- shrines / fast travel
    "BP_RebornFire_C",
    -- fog gates
    "BP_Wumen_C",
    -- doors / shortcuts
    "BP_NewPuzzlesDoor_C", "BP_Door_C", "BP_DoorBase_C",
}

-- Property types worth printing a VALUE for. Struct/object/array properties come
-- back from UE4SS as the type object, not the value, so they are skipped here.
local SCALAR_PROP_TYPES = {
    BoolProperty = true, IntProperty = true, Int8Property = true,
    Int16Property = true, Int64Property = true, UInt16Property = true,
    UInt32Property = true, UInt64Property = true, FloatProperty = true,
    DoubleProperty = true, ByteProperty = true, EnumProperty = true,
    NameProperty = true, StrProperty = true, TextProperty = true,
}

-- UFunction names worth a full signature dump on the pawn / controller chain.
local FN_PATTERNS = {
    "Teleport", "Cheat", "Fly", "Ghost", "Walk", "Cell", "Load", "Level",
    "Stream", "Debug", "Camera", "ViewTarget", "Time", "God", "Unlock",
}

-- cheap state flags worth printing when they exist on a matched actor
local FLAG_PROPS = {
    "bIsOpened", "bOpened", "bCollected", "bActivated", "bUnlocked",
    "IsOpen", "State",
}

-- classes to look for in the navmesh section
local NAV_CLASSES = {
    "RecastNavMesh", "NavigationData", "NavigationSystemV1", "NavigationSystemBase",
    "NavMeshBoundsVolume", "NavModifierVolume", "NavLinkProxy", "NavArea",
    "RecastNavMeshDataChunk", "NavigationGraph", "NavRelevantComponent",
}

-- classes to look for in the world-partition / streaming section
local WP_CLASSES = {
    "WorldPartition", "WorldPartitionSubsystem", "WorldPartitionRuntimeCell",
    "WorldPartitionRuntimeSpatialHash", "WorldPartitionRuntimeHash",
    "WorldPartitionStreamingPolicy", "DataLayer", "DataLayerInstance",
    "DataLayerAsset", "DataLayerSubsystem", "WorldPartitionLevelStreamingDynamic",
    "LevelStreaming", "LevelStreamingDynamic", "LevelStreamingAlwaysLoaded",
    "LevelStreamingKismet", "Level", "WorldComposition", "LevelBounds",
}

-- BlueprintGeneratedClass name substrings worth listing (lowercase)
local CLASS_KEYWORDS = {
    "debugcommand", "firepoint", "shrine", "minimap", "map", "nav", "player",
    "combatcharacter", "chapter", "area", "chest", "boss", "menu", "hud",
}

--------------------------------------------------------------------------------
-- logging
--------------------------------------------------------------------------------
local function log(fmt, ...)
    local msg
    if select("#", ...) > 0 then
        local ok, s = pcall(string.format, fmt, ...)
        msg = ok and s or tostring(fmt)
    else
        msg = tostring(fmt)
    end
    pcall(print, "[" .. MOD_NAME .. "] " .. msg .. "\n")
end

--------------------------------------------------------------------------------
-- output directory resolution
--------------------------------------------------------------------------------
local HARDCODED_GAME_WIN64 =
    "E:\\Program Files (x86)\\Steam\\steamapps\\common\\Wuchang Fallen Feathers\\Project_Plague\\Binaries\\Win64"

local OUT_DIR = nil

local function dir_writable(dir)
    if not dir or dir == "" then return false end
    local probe = dir .. "\\.wr_probe"
    local f = io.open(probe, "w")
    if not f then return false end
    f:write("ok")
    f:close()
    os.remove(probe)
    return true
end

local function try_mkdir(dir)
    if not dir or dir == "" then return end
    if os and os.execute then
        pcall(os.execute, 'mkdir "' .. dir .. '" >nul 2>nul')
    end
end

local function resolve_out_dir()
    local cands = {}
    local seen = {}
    local function add(d)
        if d and d ~= "" and not seen[d] then
            seen[d] = true
            cands[#cands + 1] = d
        end
    end

    -- 1. derive from package.path (UE4SS puts "<mod>\Scripts\?.lua" on it)
    local pp = tostring(package and package.path or "")
    for entry in pp:gmatch("[^;]+") do
        local base = entry:match("^(.*)[\\/][Ss]cripts[\\/]%?%.lua$")
        if base and base:lower():find(MOD_NAME:lower(), 1, true) then
            add(base .. "\\out")
        end
    end
    -- 2. absolute, known install location
    add(HARDCODED_GAME_WIN64 .. "\\ue4ss\\Mods\\" .. MOD_NAME .. "\\out")
    -- 3. relative to whatever the process CWD is
    add("ue4ss\\Mods\\" .. MOD_NAME .. "\\out")
    add(".\\ue4ss\\Mods\\" .. MOD_NAME .. "\\out")
    add("Mods\\" .. MOD_NAME .. "\\out")
    add(".\\Mods\\" .. MOD_NAME .. "\\out")
    -- 4. last resort: the mod root itself
    add(HARDCODED_GAME_WIN64 .. "\\ue4ss\\Mods\\" .. MOD_NAME)

    for _, d in ipairs(cands) do
        if dir_writable(d) then return d end
    end
    for _, d in ipairs(cands) do
        try_mkdir(d)
        if dir_writable(d) then return d end
    end
    return nil
end

local function stamp()
    local ok, s = pcall(os.date, "%Y%m%d_%H%M%S")
    return ok and s or "nodate"
end

-- buffered writer; a dump is built in memory then flushed once
local function new_writer(kind)
    if not OUT_DIR then return nil, "no output directory" end
    local path = OUT_DIR .. "\\dump_" .. stamp() .. "_" .. tostring(kind) .. ".txt"
    local buf = {}
    local w = { path = path, n = 0 }
    function w:line(fmt, ...)
        local s
        if select("#", ...) > 0 then
            local ok, r = pcall(string.format, fmt, ...)
            s = ok and r or tostring(fmt)
        else
            s = tostring(fmt)
        end
        self.n = self.n + 1
        buf[#buf + 1] = s
    end
    function w:blank() buf[#buf + 1] = "" end
    function w:header(title)
        buf[#buf + 1] = ""
        buf[#buf + 1] = "================================================================================"
        buf[#buf + 1] = "== " .. title
        buf[#buf + 1] = "================================================================================"
    end
    function w:close()
        local f = io.open(self.path, "w")
        if not f then return nil, "cannot open " .. tostring(self.path) end
        f:write(table.concat(buf, "\n"), "\n")
        f:close()
        return self.path
    end
    return w
end

local function new_named_writer(filename)
    if not OUT_DIR then return nil end
    return OUT_DIR .. "\\" .. filename
end

--------------------------------------------------------------------------------
-- safe reflection helpers
--------------------------------------------------------------------------------
local function safe(fn, ...)
    local ok, r = pcall(fn, ...)
    if ok then return r end
    return nil
end

local function isvalid(o)
    if o == nil then return false end
    local ok, r = pcall(function() return o:IsValid() end)
    if ok then return r and true or false end
    return pcall(function() return o:GetFullName() end)
end

local function fname_str(fn)
    local ok, s = pcall(function() return fn:ToString() end)
    if ok and type(s) == "string" then return s end
    return nil
end

local function sname(o)
    local s = safe(function() return fname_str(o:GetFName()) end)
    if s then return s end
    return "<?>"
end

local function fullname(o)
    local s = safe(function() return o:GetFullName() end)
    if type(s) == "string" then return s end
    return "<?>"
end

local function classof(o)
    return safe(function() return o:GetClass() end)
end

local function classname(o)
    local c = classof(o)
    if c then return sname(c) end
    return "<?>"
end

local function addr_hex(o)
    local a = safe(function() return o:GetAddress() end)
    if type(a) == "number" then
        local ok, s = pcall(string.format, "0x%X", a)
        if ok then return s end
        return tostring(a)
    end
    return "<?>"
end

local function is_cdo(o)
    local fn = fullname(o)
    return fn:find("Default__", 1, true) ~= nil
end

local function class_chain(o, upto)
    local parts = {}
    pcall(function()
        local c = o:GetClass()
        local guard = 0
        while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
            local n = sname(c)
            parts[#parts + 1] = n
            if upto and n == upto then break end
            c = c:GetSuperStruct()
            guard = guard + 1
        end
    end)
    if #parts == 0 then return "<?>" end
    return table.concat(parts, " -> ")
end

local function fmt_vec(v)
    local ok, s = pcall(function()
        return string.format("%.2f %.2f %.2f", v.X, v.Y, v.Z)
    end)
    if ok then return s end
    return "<?>"
end

local function fmt_rot(r)
    local ok, s = pcall(function()
        return string.format("P=%.2f Y=%.2f R=%.2f", r.Pitch, r.Yaw, r.Roll)
    end)
    if ok then return s end
    return "<?>"
end

-- generic value formatter for reflected property values
local function fmt_value(v)
    local t = type(v)
    if t == "nil" then return "nil" end
    if t == "number" or t == "boolean" then return tostring(v) end
    if t == "string" then return '"' .. v .. '"' end
    if t ~= "userdata" and t ~= "table" then return "<" .. t .. ">" end

    -- FName / FText / FString
    local ok, s = pcall(function() return v:ToString() end)
    if ok and type(s) == "string" then return s end
    -- FVector / FRotator-ish
    local okv, sv = pcall(function() return string.format("(%.2f %.2f %.2f)", v.X, v.Y, v.Z) end)
    if okv then return sv end
    local okr, sr = pcall(function() return string.format("(P=%.2f Y=%.2f R=%.2f)", v.Pitch, v.Yaw, v.Roll) end)
    if okr then return sr end
    -- TArray
    local oka, n = pcall(function() return v:GetArrayNum() end)
    if oka and type(n) == "number" then return "TArray[" .. n .. "]" end
    -- UObject
    local okf, f = pcall(function() return v:GetFullName() end)
    if okf and type(f) == "string" then return f end
    -- UE4SS type tag
    local okt, ty = pcall(function() return v:type() end)
    if okt and ty then return "<" .. tostring(ty) .. ">" end
    if t == "table" then
        local acc = {}
        for k, vv in pairs(v) do
            acc[#acc + 1] = tostring(k) .. "=" .. tostring(vv)
            if #acc > 8 then break end
        end
        return "{" .. table.concat(acc, ", ") .. "}"
    end
    return "<userdata>"
end

local function read_prop(obj, name)
    local ok, v = pcall(function() return obj[name] end)
    if not ok then return nil, false end
    return v, true
end


local function prop_name(p)
    local s = safe(function() return fname_str(p:GetFName()) end)
    if s then return s end
    local f = safe(function() return p:GetFullName() end)
    if type(f) == "string" then return f end
    return "<?>"
end

local function prop_type(p)
    local c = safe(function() return p:GetClass() end)
    if c then
        local s = safe(function() return fname_str(c:GetFName()) end)
        if s then return s end
        local s2 = safe(function() return c:GetName() end)
        if type(s2) == "string" then return s2 end
        local s3 = safe(function() return tostring(c) end)
        if s3 then return s3 end
    end
    local f = safe(function() return p:GetFullName() end)
    if type(f) == "string" then return (f:match("^(%S+)") or "<?>") end
    return "<?>"
end

-- Reading a property that does not exist does NOT raise in UE4SS - it hands back
-- an opaque userdata. So "does this actor have bIsOpened?" must be answered from
-- the class layout, not by trying to read it. Cached per UClass address.
local propCache = {}

local function class_prop_map(cls)
    if cls == nil then return {} end
    local key = safe(function() return cls:GetAddress() end) or tostring(cls)
    local m = propCache[key]
    if m then return m end
    m = {}
    local guard, c = 0, cls
    while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
        pcall(function()
            c:ForEachProperty(function(pp)
                local n = prop_name(pp)
                if n and n ~= "<?>" and m[n] == nil then m[n] = prop_type(pp) end
            end)
        end)
        c = safe(function() return c:GetSuperStruct() end)
        guard = guard + 1
    end
    propCache[key] = m
    return m
end

local function has_prop(obj, name)
    return class_prop_map(classof(obj))[name] ~= nil
end

-- Property names declared BELOW AActor in the chain, i.e. the game's own ones.
-- Without this cut every actor listing repeats ~85 inherited AActor properties.
local ENGINE_STOP_CLASSES = {
    Actor = true, Object = true, Info = true, Brush = true, Volume = true,
    NavigationData = true,
}
local ownPropCache = {}

local function class_gameplay_props(cls)
    if cls == nil then return {} end
    local key = safe(function() return cls:GetAddress() end) or tostring(cls)
    local cached = ownPropCache[key]
    if cached then return cached end
    local names, seen = {}, {}
    local guard, c = 0, cls
    while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
        if ENGINE_STOP_CLASSES[sname(c)] then break end
        pcall(function()
            c:ForEachProperty(function(pp)
                local n = prop_name(pp)
                if n and n ~= "<?>" and not seen[n] then
                    seen[n] = true
                    names[#names + 1] = n
                end
            end)
        end)
        c = safe(function() return c:GetSuperStruct() end)
        guard = guard + 1
    end
    table.sort(names)
    ownPropCache[key] = names
    return names
end

-- nil when the property is absent from the class, or reads back opaque
local function read_prop_str(obj, name)
    if not has_prop(obj, name) then return nil end
    local v, present = read_prop(obj, name)
    if not present then return nil end
    local ok, s = pcall(fmt_value, v)
    if not ok then return "<fmt error>" end
    if s == "<userdata>" then return nil end
    return s
end

-- UE4SS hands an FRotator back as a plain Lua table whose fields are
-- Pitch / Yaw / **roll** - note the lowercase "roll". Reading .Roll silently
-- yields nil, which is what made rotation look unreadable at first.
local function rot_fields(r)
    if r == nil then return nil end
    local ok, pi, ya, ro = pcall(function()
        return r.Pitch, r.Yaw, (r.Roll ~= nil and r.Roll or r.roll)
    end)
    if ok and type(ya) == "number" then return pi or 0.0, ya, ro or 0.0 end
    return nil
end

local function get_rot(actor)
    local r = safe(function() return actor:K2_GetActorRotation() end)
    if r ~= nil then
        local pi, ya, ro = rot_fields(r)
        if ya then return pi, ya, ro, "K2_GetActorRotation" end
        local g = safe(function() return r:get() end)
        if g ~= nil then
            local p2, y2, r2 = rot_fields(g)
            if y2 then return p2, y2, r2, "K2_GetActorRotation:get()" end
        end
    end
    local f = safe(function() return actor:GetActorForwardVector() end)
    if f ~= nil then
        local ok, pi, ya = pcall(function()
            local yaw = math.deg(math.atan(f.Y, f.X))
            local pitch = math.deg(math.atan(f.Z, math.sqrt(f.X * f.X + f.Y * f.Y)))
            return pitch, yaw
        end)
        if ok and type(ya) == "number" then return pi, ya, 0.0, "GetActorForwardVector" end
    end
    return nil, nil, nil, nil
end

local function fmt_rot_of(actor)
    local pi, ya, ro, how = get_rot(actor)
    if ya == nil then return "<?>" end
    return string.format("P=%.2f Y=%.2f R=%.2f  (via %s)", pi or 0.0, ya, ro or 0.0, how)
end

-- dump every reflected property of obj, walking the whole class chain
local function dump_all_props(w, obj, indent)
    indent = indent or "    "
    local cls = classof(obj)
    if not cls then w:line("%s<no class>", indent) return end
    local total = 0
    local guard = 0
    local c = cls
    while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
        local cn = sname(c)
        local own = {}
        pcall(function()
            c:ForEachProperty(function(p)
                own[#own + 1] = p
                if #own >= CFG.MAX_PROPS_PER_STRUCT then return true end
            end)
        end)
        if #own > 0 then
            w:line("%s-- [%s] %d properties", indent, cn, #own)
            for _, p in ipairs(own) do
                local pn = prop_name(p)
                local pt = prop_type(p)
                local pv = read_prop_str(obj, pn)
                if pv == nil then pv = "<not readable>" end
                w:line("%s  %-44s %-26s = %s", indent, pn, pt, pv)
                total = total + 1
                if total >= CFG.MAX_PROPS_PER_STRUCT then
                    w:line("%s  ... property cap %d reached", indent, CFG.MAX_PROPS_PER_STRUCT)
                    return
                end
            end
        end
        c = safe(function() return c:GetSuperStruct() end)
        guard = guard + 1
    end
    if total == 0 then w:line("%s<no reflected properties>", indent) end
end

-- list every UFunction of obj's class chain, with parameters
local function dump_all_functions(w, cls, indent, with_params)
    indent = indent or "    "
    if not cls or not isvalid(cls) then w:line("%s<no class>", indent) return end
    local guard = 0
    local c = cls
    while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
        local cn = sname(c)
        local fns = {}
        pcall(function()
            c:ForEachFunction(function(f) fns[#fns + 1] = f end)
        end)
        table.sort(fns, function(a, b) return sname(a) < sname(b) end)
        w:line("%s-- [%s] %d functions", indent, cn, #fns)
        for _, f in ipairs(fns) do
            if with_params then
                local params = {}
                pcall(function()
                    f:ForEachProperty(function(p)
                        params[#params + 1] = prop_name(p) .. ":" .. prop_type(p)
                    end)
                end)
                w:line("%s  %s(%s)", indent, sname(f), table.concat(params, ", "))
            else
                w:line("%s  %s", indent, sname(f))
            end
        end
        c = safe(function() return c:GetSuperStruct() end)
        guard = guard + 1
        -- stop climbing once we are in engine base classes; their function lists are noise
        if cn == "Actor" or cn == "Object" or cn == "PlayerController" then break end
    end
end

--------------------------------------------------------------------------------
-- FindAllOf / FindFirstOf wrappers
--------------------------------------------------------------------------------
local function find_all(shortName)
    local ok, r = pcall(FindAllOf, shortName)
    if ok and type(r) == "table" then return r end
    return nil
end

local function find_first(shortName)
    local ok, r = pcall(FindFirstOf, shortName)
    if ok and r ~= nil and isvalid(r) then return r end
    return nil
end

--------------------------------------------------------------------------------
-- world / player accessors
--------------------------------------------------------------------------------
local function get_world()
    local w = safe(UEHelpers.GetWorld)
    if w and isvalid(w) then return w end
    return nil
end

local function get_pc()
    local pc = safe(UEHelpers.GetPlayerController)
    if pc and isvalid(pc) then return pc end
    return nil
end

local function get_player()
    local p = safe(UEHelpers.GetPlayer)
    if p and isvalid(p) then return p end
    -- fallback: the game's own pawn class
    local list = find_all("BP_CombatCharacter_Player_Final_C")
    if list then
        for _, o in pairs(list) do
            if isvalid(o) and not is_cdo(o) then return o end
        end
    end
    local pc = get_pc()
    if pc then
        local pawn = safe(function() return pc.Pawn end)
        if pawn and isvalid(pawn) then return pawn end
    end
    return nil
end

local function unquote(s)
    if type(s) ~= "string" then return s end
    local inner = s:match('^"(.*)"$')
    return inner or s
end

local function current_level_name()
    local gs = safe(UEHelpers.GetGameplayStatics)
    local w = get_world()
    if not gs or not w then return "<?>" end
    local ok, s = pcall(function() return gs:GetCurrentLevelName(w, true) end)
    if ok then
        local str = unquote(fmt_value(s))
        if str and str ~= "nil" and str ~= "" then return str end
    end
    local ok2, s2 = pcall(function() return gs:GetCurrentLevelName(w, false) end)
    if ok2 then
        local str = unquote(fmt_value(s2))
        if str and str ~= "nil" and str ~= "" then return str end
    end
    return "<?>"
end

--------------------------------------------------------------------------------
-- navmesh projection
--------------------------------------------------------------------------------
local NAV_CALL = nil -- cached { obj = <UObject>, fn = "<name>" }

local function nav_candidates()
    local out = {}
    local cdo = safe(function()
        return StaticFindObject("/Script/NavigationSystem.Default__NavigationSystemV1")
    end)
    if cdo and isvalid(cdo) then out[#out + 1] = cdo end
    local inst = find_first("NavigationSystemV1")
    if inst then out[#out + 1] = inst end
    return out
end

local NAV_FN_NAMES = { "K2_ProjectPointToNavigation", "ProjectPointToNavigation" }

-- returns hit(bool), projected table {X,Y,Z} or nil, error string or nil
local function nav_project(x, y, z, extent)
    local world = get_world()
    if not world then return false, nil, "no world" end
    extent = extent or CFG.PROBE_EXTENT

    local function attempt(obj, fn)
        local out = { X = 0.0, Y = 0.0, Z = 0.0 }
        local ok, r1, r2 = pcall(function()
            return obj[fn](obj, world, { X = x, Y = y, Z = z }, out, nil, nil,
                           { X = extent.X, Y = extent.Y, Z = extent.Z })
        end)
        if not ok then return nil, tostring(r1) end
        -- out params may come back as extra return values or be written into `out`
        local proj = nil
        if type(r2) == "userdata" or type(r2) == "table" then
            local okv = pcall(function() return r2.X + r2.Y + r2.Z end)
            if okv then proj = { X = r2.X, Y = r2.Y, Z = r2.Z } end
        end
        if not proj then
            local okv = pcall(function() return out.X + out.Y + out.Z end)
            if okv then proj = { X = out.X, Y = out.Y, Z = out.Z } end
        end
        local hit = r1
        if type(hit) ~= "boolean" then hit = (hit and true or false) end
        return { hit = hit, proj = proj }, nil
    end

    if NAV_CALL then
        local res, err = attempt(NAV_CALL.obj, NAV_CALL.fn)
        if res then return res.hit, res.proj, nil end
        NAV_CALL = nil
        if err then return false, nil, err end
    end

    local lastErr = "no NavigationSystemV1 object found"
    for _, obj in ipairs(nav_candidates()) do
        for _, fn in ipairs(NAV_FN_NAMES) do
            local res, err = attempt(obj, fn)
            if res then
                NAV_CALL = { obj = obj, fn = fn }
                return res.hit, res.proj, nil
            end
            lastErr = err or lastErr
        end
    end
    return false, nil, lastErr
end

--------------------------------------------------------------------------------
-- section: player + camera
--------------------------------------------------------------------------------
local function sec_player(w)
    w:header("PLAYER / CONTROLLER / CAMERA")

    local p = get_player()
    if not p then
        w:line("no player pawn found (UEHelpers.GetPlayer and BP_CombatCharacter_Player_Final_C both empty)")
    else
        w:line("pawn full name  : %s", fullname(p))
        w:line("pawn class      : %s", classname(p))
        w:line("pawn class chain: %s", class_chain(p))
        w:line("pawn address    : %s", addr_hex(p))
        local loc = safe(function() return p:K2_GetActorLocation() end)
        w:line("location        : %s", loc and fmt_vec(loc) or "<?>")
        w:line("rotation        : %s", fmt_rot_of(p))
        local fwd = safe(function() return p:GetActorForwardVector() end)
        w:line("forward vector  : %s", fwd and fmt_vec(fwd) or "<?>")
        local rawRot = safe(function() return p:K2_GetActorRotation() end)
        w:line("raw K2_GetActorRotation -> %s", rawRot ~= nil and fmt_value(rawRot) or "nil")
        local root = safe(function() return p.RootComponent end)
        if root and isvalid(root) then
            w:line("RootComponent   : %s  (%s)", classname(root), fullname(root))
            local rl = safe(function() return root:K2_GetComponentLocation() end)
            if rl then w:line("root location   : %s", fmt_vec(rl)) end
        else
            w:line("RootComponent   : <none>")
        end
        local ctrl = safe(function() return p.Controller end)
        if ctrl and isvalid(ctrl) then
            w:line("Controller      : %s  (%s)", classname(ctrl), fullname(ctrl))
            w:line("Controller chain: %s", class_chain(ctrl))
        end
        -- game-specific stat component, for later HUD work
        local stats = find_all("ExtendedStatComponent_C")
        if stats then
            local n = 0
            for _, s in pairs(stats) do
                if isvalid(s) and not is_cdo(s) then
                    n = n + 1
                    if n <= 12 then
                        w:line("stat component  : %s  Current=%s Max=%s", fullname(s),
                               tostring(read_prop_str(s, "CurrentValue")),
                               tostring(read_prop_str(s, "MaxValue")))
                    end
                end
            end
            w:line("ExtendedStatComponent_C instances: %d", n)
        end
    end

    w:blank()
    local pc = get_pc()
    if not pc then
        w:line("no PlayerController")
    else
        w:line("PlayerController      : %s  (%s)", classname(pc), fullname(pc))
        w:line("PlayerController chain: %s", class_chain(pc))
        local pcm = safe(function() return pc.PlayerCameraManager end)
        if pcm and isvalid(pcm) then
            w:line("PlayerCameraManager  : %s  (%s)", classname(pcm), fullname(pcm))
            w:line("PCM class chain      : %s", class_chain(pcm))
            local cl = safe(function() return pcm:GetCameraLocation() end)
            local cr = safe(function() return pcm:GetCameraRotation() end)
            w:line("camera location      : %s", cl and fmt_vec(cl) or "<?>")
            local cpi, cya, cro = rot_fields(cr)
            if cya then
                w:line("camera rotation      : P=%.2f Y=%.2f R=%.2f", cpi, cya, cro)
            else
                w:line("camera rotation      : <unreadable: %s>",
                       cr ~= nil and fmt_value(cr) or "nil")
            end
            w:line("PCM actor rotation   : %s", fmt_rot_of(pcm))
            local pcmFwd = safe(function() return pcm:GetActorForwardVector() end)
            w:line("PCM forward vector   : %s", pcmFwd and fmt_vec(pcmFwd) or "<?>")
            local fov = safe(function() return pcm:GetFOVAngle() end)
            if fov == nil then fov = read_prop_str(pcm, "DefaultFOV") end
            w:line("camera FOV           : %s", tostring(fov))
            -- APlayerCameraManager has NO GetViewTarget(); reading it here returned
            -- nil and UE4SS does not raise, which is why every earlier dump said
            -- "view target: <none>". The real sources are the PlayerController's
            -- GetViewTarget() UFunction and PCM.ViewTarget.Target.
            local vtStruct = safe(function() return pcm.ViewTarget end)
            if vtStruct ~= nil then
                local tgt = safe(function() return vtStruct.Target end)
                if tgt ~= nil and isvalid(tgt) then
                    w:line("PCM.ViewTarget.Target: %s  (%s)", classname(tgt), fullname(tgt))
                    w:line("  chain              : %s", class_chain(tgt))
                else
                    w:line("PCM.ViewTarget.Target: <not readable>  (ViewTarget raw: %s)",
                           fmt_value(vtStruct))
                end
            else
                w:line("PCM.ViewTarget       : <absent>")
            end

            -- CameraCachePrivate.POV is the live camera POV (FMinimalViewInfo).
            local cache = safe(function() return pcm.CameraCachePrivate end)
            if cache ~= nil then
                local pov = safe(function() return cache.POV end)
                if pov ~= nil then
                    local pl = safe(function() return pov.Location end)
                    local pr = safe(function() return pov.Rotation end)
                    local pf = safe(function() return pov.FOV end)
                    w:line("POV location         : %s", pl and fmt_vec(pl) or "<?>")
                    local ppi, pya, pro = rot_fields(pr)
                    if pya then
                        w:line("POV rotation         : P=%.2f Y=%.2f R=%.2f", ppi, pya, pro)
                    else
                        w:line("POV rotation         : <unreadable: %s>",
                               pr ~= nil and fmt_value(pr) or "nil")
                    end
                    w:line("POV FOV              : %s", pf ~= nil and fmt_value(pf) or "<?>")
                else
                    w:line("CameraCachePrivate.POV: <not readable> (raw: %s)", fmt_value(cache))
                end
            else
                w:line("CameraCachePrivate   : <absent>")
            end
        else
            w:line("PlayerCameraManager  : <none>")
        end

        -- The view target lives on the CONTROLLER, not the camera manager.
        local vt = safe(function() return pc:GetViewTarget() end)
        if vt ~= nil and isvalid(vt) then
            w:line("PC:GetViewTarget()   : %s  (%s)", classname(vt), fullname(vt))
            w:line("  chain              : %s", class_chain(vt))
            local vloc = safe(function() return vt:K2_GetActorLocation() end)
            if vloc then w:line("  location           : %s", fmt_vec(vloc)) end
            local p2 = get_player()
            if p2 and vt ~= p2 then
                w:line("  NOTE: the view target is NOT the player pawn - a cutscene or a"
                       .. " scripted camera is active")
            end
        else
            w:line("PC:GetViewTarget()   : <none>  (raw: %s)",
                   vt ~= nil and fmt_value(vt) or "nil")
        end
        local vtPawn = read_prop_str(pc, "AcknowledgedPawn")
        if vtPawn then w:line("AcknowledgedPawn     : %s", vtPawn) end
        local hud = safe(function() return pc.MyHUD end)
        if hud and isvalid(hud) then
            w:line("HUD                  : %s  (%s)", classname(hud), fullname(hud))
        end
    end
end

--------------------------------------------------------------------------------
-- section: world / levels / streaming
--------------------------------------------------------------------------------
local function sec_world(w)
    w:header("WORLD / LEVELS / STREAMING")

    local world = get_world()
    if not world then
        w:line("no world")
        return
    end
    w:line("world full name   : %s", fullname(world))
    w:line("world class       : %s", classname(world))
    w:line("world address     : %s", addr_hex(world))
    w:line("GetCurrentLevelName: %s", current_level_name())

    local outer = safe(function() return world:GetOuter() end)
    if outer then w:line("world outer (pkg) : %s", fullname(outer)) end

    local pl = safe(function() return world.PersistentLevel end)
    if pl and isvalid(pl) then
        w:line("PersistentLevel   : %s", fullname(pl))
        local plo = safe(function() return pl:GetOuter() end)
        if plo then w:line("  outer package   : %s", fullname(plo)) end
        local an = safe(function() return pl.Actors end)
        local okn, n = pcall(function() return an:GetArrayNum() end)
        if okn then w:line("  Actors array    : %d", n) end
    else
        w:line("PersistentLevel   : <none>")
    end

    -- StreamingLevels
    w:blank()
    local sl = safe(function() return world.StreamingLevels end)
    if sl then
        local n = safe(function() return sl:GetArrayNum() end) or 0
        w:line("StreamingLevels   : %d", n)
        pcall(function()
            sl:ForEach(function(idx, elem)
                local o = elem
                local got = safe(function() return elem:get() end)
                if got ~= nil then o = got end
                if o == nil or not isvalid(o) then
                    w:line("  [%d] <invalid>", idx)
                    return
                end
                w:line("  [%d] %s", idx, classname(o))
                w:line("       full        : %s", fullname(o))
                for _, pn in ipairs({ "PackageName", "PackageNameToLoad", "LevelColor",
                                      "bShouldBeLoaded", "bShouldBeVisible", "bIsVisible",
                                      "bShouldBlockOnLoad", "StreamingPriority", "LevelTransform" }) do
                    local v = read_prop_str(o, pn)
                    if v ~= nil then w:line("       %-12s: %s", pn, v) end
                end
                for _, fn in ipairs({ "IsLevelLoaded", "IsLevelVisible", "IsStreamingStatePending",
                                      "GetWorldAssetPackageFName", "GetLoadedLevel" }) do
                    local ok, r = pcall(function() return o[fn](o) end)
                    if ok then w:line("       %-12s: %s", fn, fmt_value(r)) end
                end
            end)
        end)
    else
        w:line("StreamingLevels   : <property not readable>")
    end

    -- all loaded ULevel objects
    w:blank()
    local levels = find_all("Level")
    if levels then
        local names = {}
        for _, l in pairs(levels) do
            if isvalid(l) and not is_cdo(l) then names[#names + 1] = fullname(l) end
        end
        table.sort(names)
        w:line("loaded ULevel objects: %d", #names)
        for _, n in ipairs(names) do w:line("  %s", n) end
    end

    -- world partition / data layers
    w:blank()
    w:line("-- world partition / streaming objects (UE5.1) --")
    for _, cn in ipairs(WP_CLASSES) do
        if cn ~= "Level" and cn ~= "LevelStreaming" then
            local list = find_all(cn)
            local rows = {}
            if list then
                for _, o in pairs(list) do
                    if isvalid(o) and not is_cdo(o) then
                        local extra = {}
                        for _, pn in ipairs({ "bIsAlwaysLoaded", "bIsEnabled", "bIsInitialized",
                                              "RuntimeState", "StreamingState", "Level", "DataLayerAsset",
                                              "DebugName", "bClientOnlyVisible" }) do
                            local v = read_prop_str(o, pn)
                            if v ~= nil then extra[#extra + 1] = pn .. "=" .. v end
                        end
                        rows[#rows + 1] = string.format("  %-46s %s%s", classname(o), fullname(o),
                            #extra > 0 and ("  [" .. table.concat(extra, " ") .. "]") or "")
                    end
                end
            end
            if #rows > 0 then
                table.sort(rows)
                w:line("%s: %d", cn, #rows)
                for i = 1, math.min(#rows, 400) do w:line("%s", rows[i]) end
                if #rows > 400 then w:line("  ... %d more", #rows - 400) end
            else
                w:line("%s: 0", cn)
            end
        end
    end

    -- every reflected property of UWorld, cheap and very informative
    w:blank()
    w:line("-- UWorld reflected properties --")
    dump_all_props(w, world, "  ")
end

--------------------------------------------------------------------------------
-- section: actor census + keyword filter
--------------------------------------------------------------------------------
local function first_keyword(lowerStr)
    for _, k in ipairs(KEYWORDS) do
        if lowerStr:find(k, 1, true) then return k end
    end
    return nil
end

-- flags that actually exist on this actor's class, formatted
local function flag_str(a)
    local flags = {}
    for _, pn in ipairs(FLAG_PROPS) do
        local v = read_prop_str(a, pn)
        if v ~= nil then flags[#flags + 1] = pn .. "=" .. v end
    end
    if #flags == 0 then return "" end
    return "  {" .. table.concat(flags, " ") .. "}"
end

local function sec_actors(w)
    w:header("ACTOR CENSUS")

    local actors = find_all("Actor")
    if not actors then
        w:line("FindAllOf(\"Actor\") returned nothing")
        return
    end

    local hist = {}          -- class -> count
    -- bucket A: the CLASS name matches a keyword -> a real gameplay actor
    local mA, kwA = {}, {}
    -- bucket B: only the OBJECT name matches -> mostly HISM_/SM_ level geometry
    local mB, kwB, nB = {}, {}, {}
    local scanned, skipped = 0, 0

    for _, a in pairs(actors) do
        scanned = scanned + 1
        if scanned > CFG.MAX_ACTORS_SCANNED then break end
        if not isvalid(a) then
            skipped = skipped + 1
        else
            local fn = fullname(a)
            if fn:find("Default__", 1, true) then
                skipped = skipped + 1
            else
                local cn = classname(a)
                hist[cn] = (hist[cn] or 0) + 1
                local kc = first_keyword(cn:lower())
                if kc then
                    local b = mA[cn]
                    if not b then b = {}; mA[cn] = b; kwA[cn] = kc end
                    if #b < CFG.MAX_MATCHES_PER_CLASS then b[#b + 1] = a end
                else
                    local kn = first_keyword(fn:lower())
                    if kn then
                        local b = mB[cn]
                        if not b then b = {}; mB[cn] = b; kwB[cn] = kn; nB[cn] = 0 end
                        nB[cn] = nB[cn] + 1
                        if #b < 8 then b[#b + 1] = a end
                    end
                end
            end
        end
    end

    -- histogram
    local rows = {}
    for cn, c in pairs(hist) do rows[#rows + 1] = { cn = cn, c = c } end
    table.sort(rows, function(x, y)
        if x.c ~= y.c then return x.c > y.c end
        return x.cn < y.cn
    end)
    w:line("actors scanned: %d   (skipped CDO/invalid: %d)   distinct classes: %d",
           scanned, skipped, #rows)
    w:blank()
    w:line("-- class histogram (count desc) --")
    for _, r in ipairs(rows) do w:line("  %7d  %s", r.c, r.cn) end

    w:blank()
    w:line("-- keywords: %s --", table.concat(KEYWORDS, ", "))

    -- bucket A: class-name matches, full detail
    w:blank()
    w:line("== A. actors whose CLASS name matches a keyword (the interesting ones) ==")
    w:line("   per-class instance cap: %d", CFG.MAX_MATCHES_PER_CLASS)
    local ca = {}
    for cn in pairs(mA) do ca[#ca + 1] = cn end
    table.sort(ca)
    if #ca == 0 then w:line("   (none)") end
    local lines = 0
    for _, cn in ipairs(ca) do
        local b = mA[cn]
        w:blank()
        w:line("[%s]  total=%d  shown=%d  matched-on=%s", cn, hist[cn] or 0, #b, kwA[cn] or "?")
        local pn = class_gameplay_props(classof(b[1]))
        if #pn > 0 then
            local shown = pn
            if #pn > 80 then
                shown = {}
                for i = 1, 80 do shown[i] = pn[i] end
                shown[81] = "..."
            end
            w:line("   own properties (%d, AActor/UObject ones omitted): %s",
                   #pn, table.concat(shown, ", "))
        else
            w:line("   own properties: none (pure engine class)")
        end
        for _, a in ipairs(b) do
            if lines >= CFG.MAX_MATCH_LINES then w:line("  ... total line cap reached") break end
            lines = lines + 1
            local loc = safe(function() return a:K2_GetActorLocation() end)
            w:line("  %s | loc %s%s", fullname(a), loc and fmt_vec(loc) or "<?>", flag_str(a))
        end
        if lines >= CFG.MAX_MATCH_LINES then break end
    end

    -- bucket B: object-name-only matches, compact
    w:blank()
    w:line("== B. actors whose OBJECT name only matches (mostly HISM_/SM_ geometry) ==")
    w:line("   counts per class, up to 8 examples each")
    local cb = {}
    for cn in pairs(mB) do cb[#cb + 1] = cn end
    table.sort(cb)
    if #cb == 0 then w:line("   (none)") end
    for _, cn in ipairs(cb) do
        w:blank()
        w:line("[%s]  name-matches=%d  first-keyword=%s", cn, nB[cn] or 0, kwB[cn] or "?")
        for _, a in ipairs(mB[cn]) do
            local loc = safe(function() return a:K2_GetActorLocation() end)
            w:line("  %s | loc %s", sname(a), loc and fmt_vec(loc) or "<?>")
        end
    end
end

--------------------------------------------------------------------------------
-- section: markers with VALUES + distance to player
--------------------------------------------------------------------------------
-- A dump that lists property NAMES cannot answer state questions. Wuchang's
-- marker actors carry Used / IsShowMesh / Active / DoorOpen / Persistent /
-- SavedStatuKey - knowing the names told us nothing about whether a chest was
-- open. So: every scalar property VALUE, plus the distance to the player so a
-- known-yes and a known-no instance can be told apart in the same press.

local function dist_to(a, ref)
    if not ref then return nil end
    local loc = safe(function() return a:K2_GetActorLocation() end)
    if not loc then return nil end
    local ok, d = pcall(function()
        local dx, dy, dz = loc.X - ref.X, loc.Y - ref.Y, loc.Z - ref.Z
        return math.sqrt(dx * dx + dy * dy + dz * dz)
    end)
    if ok then return d end
    return nil
end

-- "Name=value" for every scalar own property that exists on the actor's class.
local function scalar_values(a)
    local cls = classof(a)
    if not cls then return {}, 0 end
    local types = class_prop_map(cls)
    local names = class_gameplay_props(cls)
    local out, skipped = {}, 0
    for _, pn in ipairs(names) do
        if SCALAR_PROP_TYPES[types[pn]] then
            if #out >= CFG.MAX_VALUE_PROPS then
                skipped = skipped + 1
            else
                local v = read_prop_str(a, pn)
                out[#out + 1] = pn .. "=" .. (v ~= nil and v or "<unreadable>")
            end
        end
    end
    return out, skipped
end

local function sec_markers(w)
    w:header("MARKER CLASSES - VALUES AND DISTANCE TO PLAYER")

    local p = get_player()
    local ref = p and safe(function() return p:K2_GetActorLocation() end) or nil
    if ref then
        w:line("player at %s   (distances below are 3D, in uu; 100 uu = 1 m)", fmt_vec(ref))
    else
        w:line("no player location - distances will be omitted")
    end
    w:line("per-class instance cap: %d   scalar props per instance cap: %d",
           CFG.MAX_MARKERS_PER_CLASS, CFG.MAX_VALUE_PROPS)

    for _, cn in ipairs(MARKER_CLASSES) do
        local insts = find_all(cn)
        local live = {}
        if insts then
            for _, a in pairs(insts) do
                if isvalid(a) and not is_cdo(a) then live[#live + 1] = a end
            end
        end
        w:blank()
        if #live == 0 then
            w:line("[%s]  0 instances", cn)
        else
            -- nearest first: that is where the user was standing when they pressed F8
            table.sort(live, function(x, y)
                local dx = dist_to(x, ref) or 1e12
                local dy = dist_to(y, ref) or 1e12
                return dx < dy
            end)
            local proto = classof(live[1])
            local types = proto and class_prop_map(proto) or {}
            local own = proto and class_gameplay_props(proto) or {}
            local scalars = 0
            for _, pn in ipairs(own) do
                if SCALAR_PROP_TYPES[types[pn]] then scalars = scalars + 1 end
            end
            w:line("[%s]  %d instances   own props %d (%d scalar)   chain: %s",
                   cn, #live, #own, scalars, class_chain(live[1]))
            local shown = 0
            for _, a in ipairs(live) do
                if shown >= CFG.MAX_MARKERS_PER_CLASS then
                    w:line("  ... %d more instances not shown", #live - shown)
                    break
                end
                shown = shown + 1
                local loc = safe(function() return a:K2_GetActorLocation() end)
                local d = dist_to(a, ref)
                w:line("  %s | loc %s | dist %s",
                       fullname(a),
                       loc and fmt_vec(loc) or "<?>",
                       d and string.format("%.0f", d) or "?")
                local vals, extra = scalar_values(a)
                if #vals > 0 then
                    w:line("      %s%s", table.concat(vals, "  "),
                           extra > 0 and string.format("   (+%d more)", extra) or "")
                else
                    w:line("      (no scalar own properties)")
                end
            end
        end
    end
end

--------------------------------------------------------------------------------
-- section: function signatures on the pawn / controller chain
--------------------------------------------------------------------------------
-- Only the names that matter for the minimap work: teleporting, cheat movement,
-- level/cell streaming, camera and view target, debug entry points.

local function fn_matches(name)
    for _, pat in ipairs(FN_PATTERNS) do
        if name:find(pat, 1, true) then return pat end
    end
    return nil
end

local function dump_matching_functions(w, cls, label)
    w:blank()
    if not cls or not isvalid(cls) then
        w:line("-- %s: <no class>", label)
        return
    end
    w:line("-- %s (chain walked until Object) --", label)
    local guard, c, total = 0, cls, 0
    while c ~= nil and isvalid(c) and guard < CFG.MAX_CLASS_CHAIN do
        local cn = sname(c)
        local hits = {}
        pcall(function()
            c:ForEachFunction(function(f)
                local n = sname(f)
                if n and fn_matches(n) then hits[#hits + 1] = f end
            end)
        end)
        table.sort(hits, function(a, b) return sname(a) < sname(b) end)
        if #hits > 0 then
            w:line("  [%s]", cn)
            for _, f in ipairs(hits) do
                local params = {}
                pcall(function()
                    f:ForEachProperty(function(pp)
                        params[#params + 1] = prop_type(pp) .. " " .. prop_name(pp)
                    end)
                end)
                w:line("    %s(%s)", sname(f), table.concat(params, ", "))
                total = total + 1
            end
        end
        if cn == "Object" then break end
        c = safe(function() return c:GetSuperStruct() end)
        guard = guard + 1
    end
    if total == 0 then
        w:line("  (no function matched %s)", table.concat(FN_PATTERNS, "|"))
    end
end

local function sec_functions(w)
    w:header("FUNCTION SIGNATURES (Teleport|Cheat|Fly|Ghost|Walk|Cell|Load|Level|Stream|Debug|Camera|ViewTarget|Time|God|Unlock)")
    w:line("parameter list order is the UFunction's own property order; the LAST"
           .. " parameter is normally the return value.")

    local p = get_player()
    dump_matching_functions(w, p and classof(p) or nil, "pawn class chain")

    local pc = get_pc()
    dump_matching_functions(w, pc and classof(pc) or nil, "PlayerController class chain")

    if pc then
        local pcm = safe(function() return pc.PlayerCameraManager end)
        if pcm then
            dump_matching_functions(w, classof(pcm), "PlayerCameraManager class chain")
        end
    end
end

--------------------------------------------------------------------------------
-- section: loaded blueprint classes (keyword filtered)
--------------------------------------------------------------------------------
local function sec_classes(w)
    w:header("LOADED CLASSES (keyword filtered)")

    -- FindAllOf("BlueprintGeneratedClass") came back empty on this build, so try
    -- several routes and report which one actually produced objects.
    -- Verified on this build: FindAllOf does NOT return UClass objects, FindObjects does.
    local routes = {
        { "FindObjects(0,BlueprintGeneratedClass)",
          function() return FindObjects(0, "BlueprintGeneratedClass", nil, 0, 0, false) end },
        { "FindObjects(0,Class)",
          function() return FindObjects(0, "Class", nil, 0, 0, false) end },
        { "FindAllOf(BlueprintGeneratedClass)", function() return FindAllOf("BlueprintGeneratedClass") end },
        { "FindAllOf(Class)",                   function() return FindAllOf("Class") end },
    }

    local list, used = nil, nil
    for _, r in ipairs(routes) do
        local ok, res = pcall(r[2])
        local n = 0
        if ok and type(res) == "table" then
            for _ in pairs(res) do n = n + 1 end
        end
        w:line("route %-44s -> %s", r[1], ok and (n .. " object(s)") or ("error: " .. tostring(res)))
        if list == nil and n > 0 then list = res; used = r[1] end
    end

    if not list then
        w:line("no route produced class objects; class enumeration unavailable on this build")
        return
    end

    w:blank()
    w:line("using route: %s", used)
    local all, hits = 0, {}
    for _, c in pairs(list) do
        if isvalid(c) then
            all = all + 1
            local n = sname(c)
            local ln = n:lower()
            for _, k in ipairs(CLASS_KEYWORDS) do
                if ln:find(k, 1, true) then hits[#hits + 1] = fullname(c) break end
            end
        end
    end
    table.sort(hits)
    w:line("classes seen: %d   matching %s: %d",
           all, table.concat(CLASS_KEYWORDS, "/"), #hits)
    for _, h in ipairs(hits) do w:line("  %s", h) end
end

--------------------------------------------------------------------------------
-- section: navmesh
--------------------------------------------------------------------------------
local function sec_navmesh(w)
    w:header("NAVMESH / NAVIGATION")

    local anyRecast = false
    local navSeen = {}
    for _, cn in ipairs(NAV_CLASSES) do
        local list = find_all(cn)
        local objs, dup = {}, 0
        if list then
            for _, o in pairs(list) do
                if isvalid(o) and not is_cdo(o) then
                    local key = addr_hex(o)
                    if navSeen[key] then
                        dup = dup + 1
                    else
                        navSeen[key] = true
                        objs[#objs + 1] = o
                    end
                end
            end
        end
        w:line("%-26s : %d new instance(s)%s", cn, #objs,
               dup > 0 and string.format("  (+%d already listed above)", dup) or "")
        for _, o in ipairs(objs) do
            w:blank()
            w:line("  >>> %s", fullname(o))
            w:line("      class      : %s", classname(o))
            w:line("      class chain: %s", class_chain(o))
            w:line("      ADDRESS    : %s   <-- for the C++ RecastNavMeshImpl dumper", addr_hex(o))
            local loc = safe(function() return o:K2_GetActorLocation() end)
            if loc then w:line("      actor loc  : %s", fmt_vec(loc)) end
            if cn == "RecastNavMesh" or cn == "NavigationData" then
                anyRecast = true
                w:line("      -- all reflected properties --")
                dump_all_props(w, o, "      ")
            elseif cn == "NavigationSystemV1" then
                w:line("      -- all reflected properties --")
                dump_all_props(w, o, "      ")
            else
                -- compact: only a few interesting props
                for _, pn in ipairs({ "bEnabled", "AgentRadius", "AgentHeight", "TileSizeUU",
                                      "CellSize", "CellHeight", "RuntimeGeneration",
                                      "bDrawPolygons", "bFixedTilePoolSize", "TilePoolSize",
                                      "AreaClass", "SupportedAgents", "MainNavData" }) do
                    local v = read_prop_str(o, pn)
                    if v ~= nil then w:line("      %-22s = %s", pn, v) end
                end
            end
        end
        w:blank()
    end

    -- ARecastNavMesh classes present but not instantiated?
    local rcls = safe(function() return StaticFindObject("/Script/NavigationSystem.RecastNavMesh") end)
    w:line("UClass /Script/NavigationSystem.RecastNavMesh : %s",
           (rcls and isvalid(rcls)) and (fullname(rcls) .. " @ " .. addr_hex(rcls)) or "<not loaded>")
    local ncls = safe(function() return StaticFindObject("/Script/NavigationSystem.NavigationSystemV1") end)
    w:line("UClass /Script/NavigationSystem.NavigationSystemV1 : %s",
           (ncls and isvalid(ncls)) and (fullname(ncls) .. " @ " .. addr_hex(ncls)) or "<not loaded>")

    -- projection probe at the player
    w:blank()
    w:line("-- ProjectPointToNavigation probe at the player --")
    local p = get_player()
    if not p then
        w:line("no player pawn -> probe skipped")
    else
        local loc = safe(function() return p:K2_GetActorLocation() end)
        if not loc then
            w:line("player location unreadable -> probe skipped")
        else
            local px, py, pz = loc.X, loc.Y, loc.Z
            w:line("player at %.2f %.2f %.2f", px, py, pz)
            for _, ex in ipairs({ { 100, 100, 500 }, { 500, 500, 1000 }, { 2000, 2000, 2000 } }) do
                local hit, proj, err = nav_project(px, py, pz,
                    { X = ex[1] + 0.0, Y = ex[2] + 0.0, Z = ex[3] + 0.0 })
                w:line("  extent (%d,%d,%d) -> hit=%s proj=%s%s",
                       ex[1], ex[2], ex[3], tostring(hit),
                       proj and string.format("%.2f %.2f %.2f", proj.X, proj.Y, proj.Z) or "nil",
                       err and ("  ERR: " .. err) or "")
            end
            if NAV_CALL then
                w:line("  working call: %s on %s", NAV_CALL.fn, fullname(NAV_CALL.obj))
            else
                w:line("  no working ProjectPointToNavigation call found")
            end
        end
    end
    w:line("any RecastNavMesh/NavigationData instance found: %s", tostring(anyRecast))
end

--------------------------------------------------------------------------------
-- section: DebugCommand_C and friends
--------------------------------------------------------------------------------
local function sec_debugcommand(w)
    w:header("DebugCommand_C / FUNCTION SURFACE")

    local dc = nil
    local list = find_all("DebugCommand_C")
    if list then
        local n = 0
        for _, o in pairs(list) do
            if isvalid(o) then
                n = n + 1
                w:line("instance %d: %s%s", n, fullname(o), is_cdo(o) and "  (CDO)" or "")
                if dc == nil or (is_cdo(dc) and not is_cdo(o)) then dc = o end
            end
        end
        w:line("DebugCommand_C instances: %d", n)
    else
        w:line("FindAllOf(\"DebugCommand_C\") returned nothing")
    end

    if dc then
        w:blank()
        w:line("-- DebugCommand_C functions (with parameters) --")
        dump_all_functions(w, classof(dc), "  ", true)
        w:blank()
        w:line("-- DebugCommand_C properties --")
        dump_all_props(w, dc, "  ")
    else
        -- class might be loaded even with no instance
        local anyBP = find_all("BlueprintGeneratedClass")
        if anyBP then
            for _, c in pairs(anyBP) do
                if isvalid(c) and sname(c) == "DebugCommand_C" then
                    w:line("class object found without an instance: %s", fullname(c))
                    w:line("-- DebugCommand_C functions (with parameters) --")
                    dump_all_functions(w, c, "  ", true)
                    break
                end
            end
        end
    end

    -- player class functions
    w:blank()
    local p = get_player()
    if p then
        w:line("-- player class function names (%s) --", classname(p))
        dump_all_functions(w, classof(p), "  ", false)
    else
        w:line("-- player class functions: no pawn --")
    end

    w:blank()
    local pc = get_pc()
    if pc then
        w:line("-- player controller class function names (%s) --", classname(pc))
        dump_all_functions(w, classof(pc), "  ", false)
    else
        w:line("-- player controller functions: no controller --")
    end

    -- cheat manager / console
    w:blank()
    local cm = find_first("CheatManager")
    w:line("CheatManager instance: %s", cm and fullname(cm) or "<none>")
    local gv = safe(UEHelpers.GetGameViewportClient)
    w:line("GameViewportClient   : %s", (gv and isvalid(gv)) and fullname(gv) or "<none>")
end

--------------------------------------------------------------------------------
-- F8: world dump
--------------------------------------------------------------------------------
local busy = false

local function do_world_dump(kind)
    if busy then log("dump already running, ignored"); return end
    busy = true
    local w, err = new_writer(kind or "world")
    if not w then
        log("cannot create dump file: %s", tostring(err))
        busy = false
        return
    end
    w:line("WuchangRecon world dump")
    w:line("kind      : %s", tostring(kind or "world"))
    w:line("time      : %s", tostring(safe(os.date, "%Y-%m-%d %H:%M:%S")))
    w:line("out dir   : %s", tostring(OUT_DIR))
    w:line("UEHelpers : %s", tostring(safe(UEHelpers.GetUEHelpersVersion)))

    local sections = {
        { "player", sec_player },
        { "world", sec_world },
        { "actors", sec_actors },
        { "markers", sec_markers },
        { "functions", sec_functions },
        { "classes", sec_classes },
        { "navmesh", sec_navmesh },
        { "debugcommand", sec_debugcommand },
    }
    for _, s in ipairs(sections) do
        local ok, e = pcall(s[2], w)
        if not ok then
            w:header("SECTION " .. s[1] .. " FAILED")
            w:line("%s", tostring(e))
            log("section %s failed: %s", s[1], tostring(e))
        end
    end

    local path, werr = w:close()
    busy = false
    if path then
        log("world dump written: %s (%d lines)", path, w.n)
    else
        log("world dump FAILED to write: %s", tostring(werr))
    end
end

--------------------------------------------------------------------------------
-- F9: UI dump
--------------------------------------------------------------------------------
local VIS_NAMES = { [0] = "Visible", [1] = "Collapsed", [2] = "Hidden",
                    [3] = "HitTestInvisible", [4] = "SelfHitTestInvisible" }

local function do_ui_dump(kind)
    if busy then log("dump already running, ignored"); return end
    busy = true
    local w, err = new_writer(kind or "ui")
    if not w then
        log("cannot create dump file: %s", tostring(err))
        busy = false
        return
    end
    w:line("WuchangRecon UI dump")
    w:line("kind    : %s", tostring(kind or "ui"))
    w:line("time    : %s", tostring(safe(os.date, "%Y-%m-%d %H:%M:%S")))
    w:line("level   : %s", current_level_name())

    local ok, e = pcall(function()
        local list = find_all("UserWidget")
        if not list then
            w:line("FindAllOf(\"UserWidget\") returned nothing")
            return
        end
        local rows = {}
        local cdoCount = 0
        for _, u in pairs(list) do
            if isvalid(u) then
                if is_cdo(u) then
                    cdoCount = cdoCount + 1
                else
                    local cn = classname(u)
                    local inVp = safe(function() return u:IsInViewport() end)
                    local vis = safe(function() return u:IsVisible() end)
                    local visE = safe(function() return u:GetVisibility() end)
                    local visS = (type(visE) == "number" and (VIS_NAMES[visE] or tostring(visE)))
                                 or tostring(visE)
                    local parent = safe(function() return u:GetParent() end)
                    local pcn = (parent and isvalid(parent)) and classname(parent) or "<none>"
                    local outer = safe(function() return u:GetOuter() end)
                    rows[#rows + 1] = {
                        cn = cn,
                        line = string.format(
                            "  %-52s inViewport=%-5s isVisible=%-5s visibility=%-20s parent=%-32s\n      full  : %s\n      outer : %s",
                            cn, tostring(inVp), tostring(vis), visS, pcn,
                            fullname(u), outer and fullname(outer) or "<none>"),
                    }
                end
            end
        end
        table.sort(rows, function(a, b)
            if a.cn ~= b.cn then return a.cn < b.cn end
            return a.line < b.line
        end)
        w:line("UserWidget instances: %d  (CDOs skipped: %d)", #rows, cdoCount)

        -- class histogram first, that is what the overlay needs for menu detection
        w:blank()
        w:line("-- widget class histogram --")
        local hist, order = {}, {}
        for _, r in ipairs(rows) do
            if not hist[r.cn] then hist[r.cn] = 0; order[#order + 1] = r.cn end
            hist[r.cn] = hist[r.cn] + 1
        end
        table.sort(order)
        for _, cn in ipairs(order) do w:line("  %4d  %s", hist[cn], cn) end

        w:blank()
        w:line("-- widget instances (sorted by class) --")
        for _, r in ipairs(rows) do w:line("%s", r.line) end

        -- widget class chains, once per class, for spotting base menu classes
        w:blank()
        w:line("-- widget class chains (one per class) --")
        local doneCls = {}
        for _, u in pairs(list) do
            if isvalid(u) and not is_cdo(u) then
                local cn = classname(u)
                if not doneCls[cn] then
                    doneCls[cn] = true
                    w:line("  %-52s %s", cn, class_chain(u, "UserWidget"))
                end
            end
        end
    end)
    if not ok then
        w:header("UI DUMP FAILED")
        w:line("%s", tostring(e))
    end

    -- extras: HUD, viewport widget stack
    pcall(function()
        w:blank()
        w:line("-- other UI objects --")
        for _, cn in ipairs({ "HUD", "GameViewportClient", "WidgetComponent", "SlateWidgetStyleContainerBase" }) do
            local list = find_all(cn)
            local n = 0
            if list then
                for _, o in pairs(list) do
                    if isvalid(o) and not is_cdo(o) then
                        n = n + 1
                        if n <= 20 then w:line("  %-30s %s", classname(o), fullname(o)) end
                    end
                end
            end
            w:line("  %s total: %d", cn, n)
        end
    end)

    local path, werr = w:close()
    busy = false
    if path then
        log("UI dump written: %s (%d lines)", path, w.n)
    else
        log("UI dump FAILED to write: %s", tostring(werr))
    end
end

--------------------------------------------------------------------------------
-- F10: periodic tracker
--------------------------------------------------------------------------------
local track_on = false
local track_loop_live = false
local track_path = nil
local track_t0 = nil
local track_n = 0

local function track_sample()
    local p = get_player()
    local loc = p and safe(function() return p:K2_GetActorLocation() end) or nil
    local yaw = nil
    if p then local _, y = get_rot(p); yaw = y end
    local lvl = current_level_name()
    local vtc = "<none>"
    local pc = get_pc()
    if pc then
        local pcm = safe(function() return pc.PlayerCameraManager end)
        if pcm and isvalid(pcm) then
            local vt = safe(function() return pcm:GetViewTarget() end)
            if vt and isvalid(vt) then vtc = classname(vt) end
        end
    end
    local t = os.time()
    if track_t0 == nil then track_t0 = t end
    track_n = track_n + 1
    local line = string.format("%d,%d,%d,%s,%.2f,%.2f,%.2f,%.2f,%s",
        track_n, t, t - track_t0,
        (tostring(lvl or "?"):gsub(",", ";")),
        loc and loc.X or 0.0, loc and loc.Y or 0.0, loc and loc.Z or 0.0,
        yaw or 0.0,
        (tostring(vtc or "?"):gsub(",", ";")))
    local f = io.open(track_path, "a")
    if f then f:write(line, "\n") f:close() end
end

local function track_start()
    track_path = new_named_writer("track.csv")
    if not track_path then log("tracker: no output dir"); return end
    -- write a header if the file is new
    local probe = io.open(track_path, "r")
    if probe then
        probe:close()
    else
        local f = io.open(track_path, "w")
        if f then f:write("n,epoch,t,level,x,y,z,yaw,viewTargetClass\n") f:close() end
    end
    track_t0 = nil
    track_n = 0
    track_on = true
    if track_loop_live then
        log("tracker ON (loop already live) -> %s", track_path)
        return
    end
    track_loop_live = true
    LoopAsync(CFG.TRACK_PERIOD_MS, function()
        if not track_on then
            track_loop_live = false
            return true -- stop the loop
        end
        ExecuteInGameThread(function()
            if track_on then pcall(track_sample) end
        end)
        return false
    end)
    log("tracker ON (%d ms) -> %s", CFG.TRACK_PERIOD_MS, track_path)
end

local function track_toggle()
    if track_on then
        track_on = false
        log("tracker OFF (file: %s)", tostring(track_path))
    else
        track_start()
    end
end

--------------------------------------------------------------------------------
-- F11: navmesh probe grid
--------------------------------------------------------------------------------
local function do_navprobe()
    if busy then log("dump already running, ignored"); return end
    busy = true

    local p = get_player()
    local loc = p and safe(function() return p:K2_GetActorLocation() end) or nil
    if not loc then
        log("navprobe: no player location, aborted")
        busy = false
        return
    end
    if not OUT_DIR then
        log("navprobe: no output dir")
        busy = false
        return
    end

    local path = OUT_DIR .. "\\navprobe_" .. stamp() .. ".csv"
    local rows = {}
    rows[#rows + 1] = "x,y,z,hit,projx,projy,projz"

    local half = CFG.PROBE_HALF_STEPS
    local step = CFG.PROBE_STEP_UU
    local hits, total, firstErr = 0, 0, nil

    for iy = -half, half do
        for ix = -half, half do
            local x = loc.X + ix * step
            local y = loc.Y + iy * step
            local z = loc.Z
            local hit, proj, err = nav_project(x, y, z, CFG.PROBE_EXTENT)
            total = total + 1
            if err and not firstErr then firstErr = err end
            if hit then hits = hits + 1 end
            local pj = hit and proj or nil
            rows[#rows + 1] = string.format("%.2f,%.2f,%.2f,%d,%s,%s,%s",
                x, y, z, hit and 1 or 0,
                pj and string.format("%.2f", pj.X) or "",
                pj and string.format("%.2f", pj.Y) or "",
                pj and string.format("%.2f", pj.Z) or "")
        end
    end

    local f = io.open(path, "w")
    if f then
        f:write("# WuchangRecon navprobe\n")
        f:write(string.format("# center %.2f %.2f %.2f  grid %dx%d  step %.1f uu  extent %.0f/%.0f/%.0f\n",
            loc.X, loc.Y, loc.Z, half * 2 + 1, half * 2 + 1, step,
            CFG.PROBE_EXTENT.X, CFG.PROBE_EXTENT.Y, CFG.PROBE_EXTENT.Z))
        f:write(string.format("# level %s  hits %d/%d%s\n", current_level_name(), hits, total,
            firstErr and ("  firstError=" .. firstErr) or ""))
        f:write(table.concat(rows, "\n"), "\n")
        f:close()
        log("navprobe written: %s  hits %d/%d%s", path, hits, total,
            firstErr and ("  firstError=" .. firstErr) or "")
    else
        log("navprobe FAILED to open %s", path)
    end
    busy = false
end

--------------------------------------------------------------------------------
-- F12: pickup watch
--------------------------------------------------------------------------------
-- Open question this exists to close: when the player collects a pickup, is the
-- actor DESTROYED or does it survive with a flag flipped (Persistent /
-- IsShowMesh / Used / SavedStatuKey)? Auto-marking "found" items on the minimap
-- depends on the answer. Procedure: stand near a pickup, press F12, collect it,
-- watch the log / out\pickupwatch_*.txt, press F12 again.

local watch_on = false
local watch_loop_live = false
local watch_path = nil
local watch_prev = nil
local watch_tick = 0

local function watch_write(fmt, ...)
    local ok, s = pcall(string.format, fmt, ...)
    local line = ok and s or tostring(fmt)
    log("watch: %s", line)
    if not watch_path then return end
    local f = io.open(watch_path, "a")
    if f then
        f:write(line, "\n")
        f:close()
    end
end

-- fullname -> { cls, loc, vals = { name = value } }
local function watch_snapshot()
    local snap, n = {}, 0
    for _, cn in ipairs(MARKER_CLASSES) do
        local insts = find_all(cn)
        if insts then
            for _, a in pairs(insts) do
                if n >= CFG.WATCH_MAX_ACTORS then break end
                if isvalid(a) and not is_cdo(a) then
                    n = n + 1
                    local vals = {}
                    for _, kv in ipairs((scalar_values(a))) do
                        local k, v = kv:match("^([^=]+)=(.*)$")
                        if k then vals[k] = v end
                    end
                    local loc = safe(function() return a:K2_GetActorLocation() end)
                    snap[fullname(a)] = {
                        cls = cn,
                        loc = loc and fmt_vec(loc) or "?",
                        vals = vals,
                    }
                end
            end
        end
    end
    return snap, n
end

local function watch_diff(old, new)
    local changes = 0
    for key, o in pairs(old) do
        local nw = new[key]
        if nw == nil then
            watch_write("DESTROYED  %s  [%s]  last loc %s", key, o.cls, o.loc)
            changes = changes + 1
        else
            for pn, ov in pairs(o.vals) do
                local nv = nw.vals[pn]
                if nv ~= nil and nv ~= ov then
                    watch_write("CHANGED    %s  [%s]  %s: %s -> %s", key, o.cls, pn, ov, nv)
                    changes = changes + 1
                end
            end
            if o.loc ~= nw.loc then
                watch_write("MOVED      %s  [%s]  %s -> %s", key, o.cls, o.loc, nw.loc)
                changes = changes + 1
            end
        end
    end
    for key, nw in pairs(new) do
        if old[key] == nil then
            watch_write("APPEARED   %s  [%s]  loc %s", key, nw.cls, nw.loc)
            changes = changes + 1
        end
    end
    return changes
end

local function watch_sample()
    if not watch_on then return end
    watch_tick = watch_tick + 1
    local ok, snap, n = pcall(watch_snapshot)
    if not ok then
        watch_write("snapshot failed: %s", tostring(snap))
        return
    end
    if watch_prev == nil then
        watch_prev = snap
        watch_write("baseline: %d watched actors", n)
        return
    end
    local okd, changes = pcall(watch_diff, watch_prev, snap)
    if okd and changes == 0 then
        -- keep the log quiet but prove the watch is alive every ~30 s
        if watch_tick % 15 == 0 then
            watch_write("tick %d: %d actors, no change", watch_tick, n)
        end
    elseif not okd then
        watch_write("diff failed: %s", tostring(changes))
    end
    watch_prev = snap
end

local function watch_start()
    watch_prev = nil
    watch_tick = 0
    watch_path = OUT_DIR and (OUT_DIR .. "\\pickupwatch_" .. stamp() .. ".txt") or nil
    if watch_path then
        local f = io.open(watch_path, "w")
        if f then
            f:write("# WuchangRecon pickup watch\n")
            f:write(string.format("# started %s  level %s  period %d ms\n",
                tostring(safe(os.date, "%Y-%m-%d %H:%M:%S")), current_level_name(),
                CFG.WATCH_PERIOD_MS))
            f:write("# classes: " .. table.concat(MARKER_CLASSES, ", ") .. "\n")
            f:close()
        else
            watch_path = nil
        end
    end
    log("pickup watch ON  (every %d ms; file: %s)", CFG.WATCH_PERIOD_MS,
        tostring(watch_path))

    if watch_loop_live then return end
    watch_loop_live = true
    local ok, e = pcall(LoopAsync, CFG.WATCH_PERIOD_MS, function()
        if not watch_on then
            watch_loop_live = false
            return true
        end
        pcall(ExecuteInGameThread, function() pcall(watch_sample) end)
        return false
    end)
    if not ok then
        watch_loop_live = false
        log("pickup watch loop failed to start: %s", tostring(e))
    end
end

local function watch_toggle()
    watch_on = not watch_on
    if watch_on then
        watch_start()
    else
        log("pickup watch OFF after %d ticks (file: %s)", watch_tick, tostring(watch_path))
    end
end

--------------------------------------------------------------------------------
-- automatic menu-time dump
--------------------------------------------------------------------------------
local auto_done = false
local auto_elapsed = 0

local function start_auto_dump()
    LoopAsync(CFG.AUTO_POLL_S * 1000, function()
        if auto_done then return true end
        auto_elapsed = auto_elapsed + CFG.AUTO_POLL_S
        if auto_elapsed < CFG.AUTO_FIRST_DELAY_S then return false end
        if auto_elapsed > CFG.AUTO_GIVE_UP_S then
            auto_done = true
            log("auto menu dump: gave up after %d s (no world)", auto_elapsed)
            return true
        end
        ExecuteInGameThread(function()
            if auto_done then return end
            local world = get_world()
            if not world then
                if auto_elapsed % 30 == 0 then
                    log("auto menu dump: still waiting for a world (%d s)", auto_elapsed)
                end
                return
            end
            auto_done = true
            log("auto menu dump: world is up (%s) at %d s, dumping",
                current_level_name(), auto_elapsed)
            pcall(do_ui_dump, "menu_ui")
            pcall(do_world_dump, "menu_world")
        end)
        return false
    end)
end

--------------------------------------------------------------------------------
-- init
--------------------------------------------------------------------------------
local function init()
    OUT_DIR = resolve_out_dir()
    if OUT_DIR then
        log("output directory: %s", OUT_DIR)
    else
        log("WARNING: no writable output directory found; dumps will be skipped")
    end

    -- F10 is deliberately avoided: ConsoleEnablerMod maps it to the game console.
    local binds = {
        { Key.F8,  "world dump",     function() pcall(do_world_dump, "world") end },
        { Key.F9,  "UI dump",        function() pcall(do_ui_dump, "ui") end },
        { Key.F7,  "tracker",        function() pcall(track_toggle) end },
        { Key.F11, "navmesh probe",  function() pcall(do_navprobe) end },
        { Key.F12, "pickup watch",   function() pcall(watch_toggle) end },
    }
    local plain, ctrl = 0, 0
    for _, b in ipairs(binds) do
        local ok, e = pcall(RegisterKeyBind, b[1], b[3])
        if ok then plain = plain + 1
        else log("failed to bind %s: %s", b[2], tostring(e)) end
        -- CTRL variant, in case the game or the engine eats the plain key
        local ok2, e2 = pcall(function()
            RegisterKeyBind(b[1], { ModifierKey.CONTROL }, b[3])
        end)
        if ok2 then ctrl = ctrl + 1
        else log("failed to bind CTRL+%s: %s", b[2], tostring(e2)) end
    end
    log("keybinds registered: %d plain, %d with CTRL", plain, ctrl)

    pcall(start_auto_dump)

    log("WuchangRecon loaded: F8 world, F9 UI, F7 track, F11 navprobe, F12 pickup watch"
        .. " (CTRL+key also works; F10 avoided - it is a console key;"
        .. " F6 belongs to the WuchangMinimap C++ mod)")
end

local ok, e = pcall(init)
if not ok then
    log("INIT FAILED: %s", tostring(e))
end

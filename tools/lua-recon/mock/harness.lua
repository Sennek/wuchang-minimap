--[[
    Offline test harness for WuchangRecon/Scripts/main.lua.

    It fakes just enough of the UE4SS Lua API to run every dump section outside the
    game: FindAllOf / FindFirstOf / StaticFindObject / RegisterKeyBind / LoopAsync /
    ExecuteInGameThread / Key / ModifierKey, plus a UEHelpers module and a small
    object model with classes, super-struct chains, properties (with types AND
    values) and UFunctions with parameters.

    Two modes, both of which must pass:
      MODE=friendly  everything works; the harness then checks that the dumps
                     actually contain the sections we care about
      MODE=hostile   every single reflection call raises; nothing may escape pcall
                     and the mod must still write a file and register its keybinds

    Run it through mock/run.py (which sets WR_TMP, WR_MODE and package.path).
--]]

local MODE = os.getenv("WR_MODE") or "friendly"
local TMP = os.getenv("WR_TMP") or "."
local HOSTILE = (MODE == "hostile")

local log = {}
local function note(s)
    log[#log + 1] = s
end

--------------------------------------------------------------------------------
-- object model
--------------------------------------------------------------------------------

local function boom()
    error("hostile mock: reflection is unavailable", 0)
end

local addr = 0x10000000

local function new_fname(s)
    return setmetatable({}, {
        __index = {
            ToString = function()
                if HOSTILE then boom() end
                return s
            end,
        },
    })
end

-- prop = { name, type, value }
local function new_prop(p)
    local o = {}
    o.GetFName = function() if HOSTILE then boom() end return new_fname(p.name) end
    o.GetFullName = function() if HOSTILE then boom() end return p.type .. " " .. p.name end
    o.GetClass = function()
        if HOSTILE then boom() end
        local c = {}
        c.GetFName = function() return new_fname(p.type) end
        c.GetName = function() return p.type end
        return setmetatable({}, { __index = c })
    end
    return setmetatable({}, { __index = o })
end

-- fn = { name, params = { {name=,type=}, ... } }
local function new_fn(f)
    local o = {}
    o.GetFName = function() if HOSTILE then boom() end return new_fname(f.name) end
    o.GetFullName = function() if HOSTILE then boom() end return "Function " .. f.name end
    o.ForEachProperty = function(_, cb)
        if HOSTILE then boom() end
        for _, p in ipairs(f.params or {}) do
            cb(new_prop({ name = p.name, type = p.type, value = nil }))
        end
    end
    o.IsValid = function() return true end
    return setmetatable({}, { __index = o })
end

-- cls = { name, super = <cls or nil>, props = { {name,type,value} }, fns = { {name,params} } }
local classes = {}

local function new_class(cls)
    if classes[cls.name] then return classes[cls.name] end
    addr = addr + 0x1000
    local myaddr = addr
    local o = {}
    o.GetFName = function() if HOSTILE then boom() end return new_fname(cls.name) end
    o.GetFullName = function() if HOSTILE then boom() end return "Class " .. cls.name end
    o.GetName = function() if HOSTILE then boom() end return cls.name end
    o.GetAddress = function() if HOSTILE then boom() end return myaddr end
    o.IsValid = function() if HOSTILE then boom() end return true end
    o.GetSuperStruct = function()
        if HOSTILE then boom() end
        return cls.super and new_class(cls.super) or nil
    end
    o.ForEachProperty = function(_, cb)
        if HOSTILE then boom() end
        for _, p in ipairs(cls.props or {}) do
            if cb(new_prop(p)) then return end
        end
    end
    o.ForEachFunction = function(_, cb)
        if HOSTILE then boom() end
        for _, f in ipairs(cls.fns or {}) do
            if cb(new_fn(f)) then return end
        end
    end
    local wrapped = setmetatable({}, { __index = o })
    classes[cls.name] = wrapped
    return wrapped
end

-- obj = { name, cls = <cls spec>, values = {k=v}, loc = {X,Y,Z} }
local function new_obj(obj)
    addr = addr + 0x100
    local myaddr = addr
    local cls = new_class(obj.cls)

    -- every property declared anywhere in the chain, so property reads work
    local values = {}
    local c = obj.cls
    while c do
        for _, p in ipairs(c.props or {}) do
            if values[p.name] == nil then values[p.name] = p.value end
        end
        c = c.super
    end
    -- obj.values is consulted LIVE (not copied) so a test can flip a flag mid-run
    -- and the mod's next read sees the new value, the way the game would.
    local live = obj.values or {}

    local methods = {}
    methods.GetFName = function() if HOSTILE then boom() end return new_fname(obj.name) end
    methods.GetFullName = function()
        if HOSTILE then boom() end
        return obj.cls.name .. " /Game/Maps/ProjectMain.ProjectMain:PersistentLevel." .. obj.name
    end
    methods.GetName = function() if HOSTILE then boom() end return obj.name end
    methods.GetClass = function() if HOSTILE then boom() end return cls end
    methods.GetAddress = function() if HOSTILE then boom() end return myaddr end
    methods.IsValid = function() if HOSTILE then boom() end return true end
    methods.K2_GetActorLocation = function()
        if HOSTILE then boom() end
        local l = obj.loc or { X = 0, Y = 0, Z = 0 }
        return { X = l.X, Y = l.Y, Z = l.Z }
    end
    -- UE4SS hands an FRotator back with a LOWERCASE "roll"; the mod must cope.
    methods.K2_GetActorRotation = function()
        if HOSTILE then boom() end
        return { Pitch = 0.0, Yaw = 42.0, roll = 0.0 }
    end
    methods.GetActorForwardVector = function()
        if HOSTILE then boom() end
        return { X = 1.0, Y = 0.0, Z = 0.0 }
    end
    methods.GetFOVAngle = function() if HOSTILE then boom() end return 60.0 end
    methods.GetCameraLocation = function() if HOSTILE then boom() end return { X = 1, Y = 2, Z = 3 } end
    methods.GetCameraRotation = function()
        if HOSTILE then boom() end
        return { Pitch = -5.0, Yaw = 42.0, roll = 0.0 }
    end
    methods.GetViewTarget = function()
        if HOSTILE then boom() end
        return obj.view_target
    end
    methods.GetName = methods.GetName

    return setmetatable({}, {
        __index = function(_, key)
            if methods[key] ~= nil then return methods[key] end
            if HOSTILE then boom() end
            local v = live[key]
            if v == nil then v = values[key] end
            if v ~= nil then return v end
            -- A property that does not exist returns opaque userdata in UE4SS, never
            -- an error. Emulate that with a table nothing can read.
            return setmetatable({}, { __index = function() return nil end })
        end,
    })
end

--------------------------------------------------------------------------------
-- the fake world
--------------------------------------------------------------------------------

local C_Object = { name = "Object", props = {}, fns = {} }
local C_Actor = {
    name = "Actor",
    super = C_Object,
    props = { { name = "bHidden", type = "BoolProperty", value = false } },
    fns = { { name = "K2_TeleportTo", params = { { name = "DestLocation", type = "StructProperty" }, { name = "DestRotation", type = "StructProperty" }, { name = "ReturnValue", type = "BoolProperty" } } } },
}
local C_Pawn = { name = "Pawn", super = C_Actor, props = {}, fns = {} }
local C_Character = {
    name = "Character",
    super = C_Pawn,
    props = {},
    fns = {
        { name = "ClientCheatFly", params = {} },
        { name = "ClientCheatGhost", params = {} },
        { name = "ClientCheatWalk", params = {} },
    },
}
local C_Player = {
    name = "BP_CombatCharacter_Player_Final_C",
    super = C_Character,
    props = {
        { name = "PlayerLevel", type = "IntProperty", value = 37 },
        { name = "Controller", type = "ObjectProperty", value = nil },
    },
    fns = { { name = "DebugSetCellLoad", params = { { name = "CellX", type = "IntProperty" }, { name = "CellY", type = "IntProperty" }, { name = "bLoad", type = "BoolProperty" } } } },
}
local C_Controller = { name = "Controller", super = C_Actor, props = {}, fns = {} }
local C_PC = {
    name = "PlayerController",
    super = C_Controller,
    props = {
        { name = "AcknowledgedPawn", type = "ObjectProperty", value = nil },
        { name = "Player", type = "ObjectProperty", value = nil },
        { name = "PlayerInput", type = "ObjectProperty", value = nil },
        { name = "InputComponent", type = "ObjectProperty", value = nil },
    },
    fns = {
        { name = "GetViewTarget", params = { { name = "ReturnValue", type = "ObjectProperty" } } },
        { name = "SetViewTargetWithBlend", params = { { name = "NewViewTarget", type = "ObjectProperty" } } },
        { name = "ClientCommitMapChange", params = {} },
        { name = "ServerUpdateLevelVisibility", params = { { name = "LevelVisibility", type = "StructProperty" } } },
    },
}
local C_DCSPC = { name = "DCSPlayerController_C", super = C_PC, props = {}, fns = {} }
local C_PCM = {
    name = "PlayerCameraManager",
    super = C_Actor,
    props = {
        { name = "DefaultFOV", type = "FloatProperty", value = 80.0 },
        -- Struct properties: UE4SS hands most of these back as the ScriptStruct type
        -- object, but ViewTarget/CameraCachePrivate are readable in practice, so the
        -- mock supplies readable stand-ins to exercise the happy path.
        { name = "ViewTarget", type = "StructProperty", value = nil },
        { name = "CameraCachePrivate", type = "StructProperty", value = nil },
    },
    fns = { { name = "SetGameCameraCutThisFrame", params = {} } },
}

local function marker_class(name, extra)
    local props = {
        { name = "Used", type = "BoolProperty", value = false },
        { name = "IsShowMesh", type = "BoolProperty", value = true },
        { name = "SavedStatuKey", type = "NameProperty", value = "None" },
        { name = "ItemID", type = "IntProperty", value = 1001 },
        { name = "Transform", type = "StructProperty", value = nil },
    }
    for _, p in ipairs(extra or {}) do props[#props + 1] = p end
    return { name = name, super = C_Actor, props = props, fns = {} }
end

local C_Pickup = marker_class("BP_PickupActor_C")
local C_PickupPT = marker_class("BP_PickUpPT_C")
local C_Chest = marker_class("BP_treasurebox_C", { { name = "DoorOpen", type = "BoolProperty", value = false } })
local C_Shrine = marker_class("BP_RebornFire_C", { { name = "Active", type = "BoolProperty", value = true } })
local C_Wumen = marker_class("BP_Wumen_C", { { name = "Persistent", type = "BoolProperty", value = true } })
local C_Recast = {
    name = "RecastNavMesh",
    super = C_Actor,
    props = {
        { name = "TileSizeUU", type = "FloatProperty", value = 1280.0 },
        { name = "AgentRadius", type = "FloatProperty", value = 34.0 },
        { name = "PolyRefTileBits", type = "IntProperty", value = 23 },
    },
    fns = {},
}

local player = new_obj({ name = "BP_CombatCharacter_Player_Final_C_1", cls = C_Player, loc = { X = 18142.33, Y = 5609.49, Z = -1550.33 } })
local pcm = new_obj({
    name = "PlayerCameraManager_0",
    cls = C_PCM,
    loc = { X = 0, Y = 0, Z = 0 },
    values = {
        ViewTarget = { Target = player },
        CameraCachePrivate = {
            POV = {
                Location = { X = 18100.0, Y = 5500.0, Z = -1400.0 },
                Rotation = { Pitch = -12.0, Yaw = 42.0, roll = 0.0 },
                FOV = 62.5,
            },
        },
    },
})
-- Consulted live, so the Enhanced Input objects built further down can be hung
-- off the controller after it exists.
PC_VALUES = { PlayerCameraManager = pcm, MyHUD = nil }
local pc = new_obj({
    name = "DCSPlayerController_C_0",
    cls = C_DCSPC,
    loc = { X = 0, Y = 0, Z = 0 },
    values = PC_VALUES,
    view_target = player,
})

local world = new_obj({ name = "ProjectMain", cls = { name = "World", super = C_Object, props = {}, fns = {} } })

-- These two are the subjects of the F12 pickup-watch test: pickup_a vanishes and
-- pickup_b's Used flag flips, mid-run, in mutate_world() below.
PICKUP_B_VALUES = { Used = false }
local pickup_a = new_obj({ name = "BP_PickupActor_C_1", cls = C_Pickup, loc = { X = 18200, Y = 5600, Z = -1550 } })
local pickup_b = new_obj({ name = "BP_PickupActor_C_2", cls = C_Pickup, loc = { X = 19000, Y = 6000, Z = -1500 }, values = PICKUP_B_VALUES })

local instances = {
    ["BP_CombatCharacter_Player_Final_C"] = { player },
    ["PlayerController"] = { pc },
    ["DCSPlayerController_C"] = { pc },
    ["PlayerCameraManager"] = { pcm },
    ["World"] = { world },
    ["BP_PickupActor_C"] = { pickup_a, pickup_b },
    ["BP_PickUpPT_C"] = { new_obj({ name = "BP_PickUpPT_C_1", cls = C_PickupPT, loc = { X = 18500, Y = 5700, Z = -1540 } }) },
    ["BP_treasurebox_C"] = { new_obj({ name = "BP_treasurebox_C_1", cls = C_Chest, loc = { X = 18300, Y = 5000, Z = -1560 } }) },
    ["BP_RebornFire_C"] = { new_obj({ name = "BP_RebornFire_C_1", cls = C_Shrine, loc = { X = 18100, Y = 5500, Z = -1555 } }) },
    ["BP_Wumen_C"] = { new_obj({ name = "BP_Wumen_C_1", cls = C_Wumen, loc = { X = 20000, Y = 4000, Z = -1400 } }) },
    ["RecastNavMesh"] = {
        new_obj({ name = "RecastNavMesh-Small", cls = C_Recast }),
        new_obj({ name = "RecastNavMesh-Big", cls = C_Recast }),
    },
}
-- Actor = everything, the way FindAllOf("Actor") behaves in game.
do
    local all = {}
    for k, v in pairs(instances) do
        if k ~= "World" then
            for _, o in ipairs(v) do all[#all + 1] = o end
        end
    end
    instances["Actor"] = all
end

--------------------------------------------------------------------------------
-- the fake Enhanced Input stack
--------------------------------------------------------------------------------
-- A TArray answers GetArrayNum and ForEach, and hands each element over inside a
-- handle that has to be :get()'d - the shape UE4SS actually uses.
local function new_array(items)
    local o = {}
    o.GetArrayNum = function() if HOSTILE then boom() end return #items end
    o.ForEach = function(_, cb)
        if HOSTILE then boom() end
        for i, v in ipairs(items) do cb(i, { get = function() return v end }) end
    end
    return setmetatable({}, { __index = o })
end

-- entries = { {k = <key>, v = <value>}, ... }
local function new_map(entries)
    local o = {}
    o.ForEach = function(_, cb)
        if HOSTILE then boom() end
        for _, e in ipairs(entries) do
            cb({ get = function() return e.k end }, { get = function() return e.v end })
        end
    end
    return setmetatable({}, { __index = o })
end

-- A struct value: named fields, and opaque userdata-alike for anything absent.
local function new_struct(fields)
    return setmetatable({}, {
        __index = function(_, key)
            if HOSTILE then boom() end
            local v = fields[key]
            if v ~= nil then return v end
            return setmetatable({}, { __index = function() return nil end })
        end,
    })
end

local C_InputAction = {
    name = "InputAction",
    super = C_Object,
    props = {
        { name = "ValueType", type = "EnumProperty", value = "Digital" },
        { name = "bConsumeInput", type = "BoolProperty", value = true },
        { name = "ActionDescription", type = "TextProperty", value = "" },
    },
    fns = {},
}
local C_IMC = {
    name = "InputMappingContext",
    super = C_Object,
    props = { { name = "Mappings", type = "ArrayProperty", value = nil } },
    fns = {},
}
local C_PlayerInput = { name = "PlayerInput", super = C_Object, props = {}, fns = {} }
local C_EnhancedPlayerInput = {
    name = "EnhancedPlayerInput",
    super = C_PlayerInput,
    props = {
        { name = "AppliedInputContexts", type = "MapProperty", value = nil },
        { name = "EnhancedActionMappings", type = "ArrayProperty", value = nil },
    },
    fns = {},
}
local C_LocalPlayer = {
    name = "LocalPlayer",
    super = C_Object,
    props = { { name = "PlayerController", type = "ObjectProperty", value = nil } },
    fns = {},
}
local C_EISubsystem = {
    name = "EnhancedInputLocalPlayerSubsystem",
    super = C_Object,
    props = { { name = "UserSettings", type = "ObjectProperty", value = nil } },
    fns = {},
}
local C_UserSettings = {
    name = "EnhancedInputUserSettings",
    super = C_Object,
    props = { { name = "CurrentProfileIdentifier", type = "NameProperty", value = "Default" } },
    fns = {},
}
local C_KeyBindSave = {
    name = "BP_KeyBindSettings_C",
    super = C_Object,
    props = { { name = "SavedKey", type = "NameProperty", value = "F" } },
    fns = {},
}

local function new_action(name)
    return new_obj({ name = name, cls = C_InputAction })
end

local ACT_Interact = new_action("IA_Interact")
local ACT_Inventory = new_action("IP_Inventory")
local ACT_Jump = new_action("IA_Jump")

local function new_mapping(action, keyName, mappableName)
    return new_struct({
        Action = action,
        Key = new_struct({ KeyName = new_fname(keyName) }),
        bIsPlayerMappable = true,
        PlayerMappableOptions = new_struct({
            Name = new_fname(mappableName),
            DisplayName = mappableName,
        }),
        Triggers = new_array({}),
        Modifiers = new_array({}),
    })
end

local imc_default = new_obj({
    name = "IMC_Default",
    cls = C_IMC,
    values = {
        Mappings = new_array({
            new_mapping(ACT_Interact, "E", "Interact"),
            new_mapping(ACT_Jump, "SpaceBar", "Jump"),
        }),
    },
})
local imc_menu = new_obj({
    name = "IMC_Menu",
    cls = C_IMC,
    values = {
        Mappings = new_array({ new_mapping(ACT_Inventory, "Tab", "Inventory") }),
    },
})

local player_input = new_obj({
    name = "EnhancedPlayerInput_0",
    cls = C_EnhancedPlayerInput,
    values = {
        AppliedInputContexts = new_map({ { k = imc_default, v = 0 }, { k = imc_menu, v = 10 } }),
        -- the post-remap list: Interact has been moved off E onto G
        EnhancedActionMappings = new_array({
            new_mapping(ACT_Interact, "G", "Interact"),
            new_mapping(ACT_Jump, "SpaceBar", "Jump"),
            new_mapping(ACT_Inventory, "Tab", "Inventory"),
        }),
        ActionMappings = new_array({}),
        AxisMappings = new_array({}),
    },
})

local local_player = new_obj({ name = "LocalPlayer_0", cls = C_LocalPlayer, values = { PlayerController = pc } })
local ei_subsystem = new_obj({ name = "EnhancedInputLocalPlayerSubsystem_0", cls = C_EISubsystem })
local user_settings = new_obj({ name = "EnhancedInputUserSettings_0", cls = C_UserSettings })
local keybind_save = new_obj({ name = "BP_KeyBindSettings_C_0", cls = C_KeyBindSave })

PC_VALUES.Player = local_player
PC_VALUES.PlayerInput = player_input
PC_VALUES.InputComponent = new_obj({
    name = "EnhancedInputComponent_0",
    cls = { name = "EnhancedInputComponent", super = C_Object, props = {}, fns = {} },
})

-- Added AFTER the Actor aggregation above on purpose: none of these is an actor.
instances["InputMappingContext"] = { imc_default, imc_menu }
instances["InputAction"] = { ACT_Interact, ACT_Inventory, ACT_Jump }
instances["EnhancedPlayerInput"] = { player_input }
instances["PlayerInput"] = { player_input }
instances["LocalPlayer"] = { local_player }
instances["EnhancedInputLocalPlayerSubsystem"] = { ei_subsystem }
instances["EnhancedInputUserSettings"] = { user_settings }
instances["EnhancedInputComponent"] = { PC_VALUES.InputComponent }
instances["BP_KeyBindSettings_C"] = { keybind_save }

-- What FindObjects(0, "Class", ...) hands back: UClass objects, not instances.
local CLASS_OBJECTS = {
    C_IMC, C_InputAction, C_EnhancedPlayerInput, C_EISubsystem, C_UserSettings,
    C_KeyBindSave, C_Recast, C_Player,
}

-- StaticFindObject answers for the ScriptStruct / UClass layout probes.
local OBJECT_PATHS = {
    ["/Script/EnhancedInput.EnhancedActionKeyMapping"] = {
        name = "EnhancedActionKeyMapping",
        props = {
            { name = "Action", type = "ObjectProperty" },
            { name = "Key", type = "StructProperty" },
            { name = "bIsPlayerMappable", type = "BoolProperty" },
            { name = "PlayerMappableOptions", type = "StructProperty" },
            { name = "Triggers", type = "ArrayProperty" },
            { name = "Modifiers", type = "ArrayProperty" },
        },
    },
    ["/Script/InputCore.Key"] = {
        name = "Key",
        props = { { name = "KeyName", type = "NameProperty" } },
    },
    ["/Script/EnhancedInput.EnhancedPlayerInput"] = C_EnhancedPlayerInput,
    ["/Script/EnhancedInput.InputMappingContext"] = C_IMC,
}

--------------------------------------------------------------------------------
-- the fake UE4SS API
--------------------------------------------------------------------------------

function FindAllOf(name)
    if HOSTILE then boom() end
    local t = instances[name]
    if not t or #t == 0 then return nil end
    local copy = {}
    for i, v in ipairs(t) do copy[i] = v end
    return copy
end

function FindFirstOf(name)
    if HOSTILE then boom() end
    local t = instances[name]
    return t and t[1] or nil
end

function FindObjects()
    if HOSTILE then boom() end
    local out = {}
    for i, c in ipairs(CLASS_OBJECTS) do out[i] = new_class(c) end
    return out
end

function StaticFindObject(path)
    if HOSTILE then boom() end
    local spec = OBJECT_PATHS[path]
    if spec then return new_class(spec) end
    return nil
end

BINDS = {}
function RegisterKeyBind(key, a, b)
    local cb = b or a
    if type(cb) ~= "function" then error("RegisterKeyBind: no callback") end
    local modified = (b ~= nil)
    BINDS[#BINDS + 1] = { key = key, ctrl = modified, cb = cb }
end

LOOPS = {}
function LoopAsync(ms, fn)
    LOOPS[#LOOPS + 1] = { ms = ms, fn = fn }
end

function ExecuteInGameThread(fn)
    fn()
end

function ExecuteAsync(fn)
    fn()
end

Key = {}
for i = 1, 24 do Key["F" .. i] = 200 + i end
Key.O = 79
ModifierKey = { CONTROL = 1, SHIFT = 2, ALT = 4 }

package.preload["UEHelpers"] = function()
    return {
        GetUEHelpersVersion = function()
            if HOSTILE then boom() end
            return "mock-1.0"
        end,
        GetPlayer = function()
            if HOSTILE then boom() end
            return player
        end,
        GetPlayerController = function()
            if HOSTILE then boom() end
            return pc
        end,
        GetWorld = function()
            if HOSTILE then boom() end
            return world
        end,
        GetGameplayStatics = function()
            if HOSTILE then boom() end
            return nil
        end,
        GetWorldContextObject = function()
            if HOSTILE then boom() end
            return world
        end,
    }
end

--------------------------------------------------------------------------------
-- run
--------------------------------------------------------------------------------

print("== harness mode: " .. MODE .. " ==")

-- Watch the mod's own log for the pickup-watch state. How many presses it takes to
-- get the watch ON depends on how many keybinds were fired above and on the mod's
-- press coalescing, which the pickup-watch phase must not have to know about.
local watch_is_on = false
do
    local realprint = print
    print = function(...)
        local parts = {}
        for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
        local line = table.concat(parts, " ")
        if line:find("pickup watch ON", 1, true) then
            watch_is_on = true
        elseif line:find("pickup watch OFF", 1, true) then
            watch_is_on = false
        end
        realprint(...)
    end
end

local chunk, err = loadfile(TMP .. "\\WuchangRecon\\Scripts\\main.lua")
if not chunk then
    print("LOAD FAILED: " .. tostring(err))
    os.exit(1)
end

local ok, e = pcall(chunk)
if not ok then
    print("MOD RAISED AT LOAD: " .. tostring(e))
    os.exit(1)
end

print(string.format("keybinds registered: %d", #BINDS))
print(string.format("async loops started: %d", #LOOPS))

local function pump(times)
    for _, l in ipairs(LOOPS) do
        for _ = 1, (times or 1) do
            local okl, stop = pcall(l.fn)
            if not okl then
                print("LOOP RAISED: " .. tostring(stop))
                os.exit(1)
            end
            if stop == true then break end
        end
    end
end

-- Simulate the player collecting the first pickup: the actor disappears and the
-- second one has its Used flag flipped. This is exactly what the F12 watch has to
-- notice and report.
local function mutate_world()
    local t = instances["BP_PickupActor_C"]
    if t and #t > 1 then
        table.remove(t, 1) -- BP_PickupActor_C_1 is gone: "collected == destroyed"
    end
    PICKUP_B_VALUES.Used = true -- and the other one flipped a flag
    print("world mutated: pickup 1 destroyed, pickup 2 Used -> true")
end

-- Fire every keybind, pumping the async loops in between so toggled features
-- (tracker, pickup watch) actually get to run. Pass 2 presses everything again,
-- which turns the toggles back off.
for pass = 1, 2 do
    for _, b in ipairs(BINDS) do
        local okk, ee = pcall(b.cb)
        if not okk then
            print(string.format("KEYBIND %s (ctrl=%s) RAISED: %s", tostring(b.key), tostring(b.ctrl), tostring(ee)))
            os.exit(1)
        end
        pump(1)
    end
    print(string.format("pass %d survived", pass))
end

-- Dedicated pickup-watch phase: turn the watch on with a SINGLE press, let it take
-- its baseline, mutate the world, let it diff, then turn it off.
local f12 = nil
for _, b in ipairs(BINDS) do
    if b.key == Key.F12 and not b.ctrl then f12 = b end
end
if f12 then
    print("-- pickup watch phase --")
    -- Get past the coalescing window, then make sure the watch starts from OFF.
    pump(20)
    if watch_is_on then
        pcall(f12.cb)
        pump(20)
    end
    local okt, ee = pcall(f12.cb)
    if not okt then
        print("F12 RAISED: " .. tostring(ee))
        os.exit(1)
    end
    pump(1) -- baseline
    mutate_world()
    pump(1) -- diff
    pump(1)
    local ok2 = pcall(f12.cb)
    if not ok2 then
        print("F12 (off) RAISED")
        os.exit(1)
    end
else
    print("F12 bind not found")
    os.exit(1)
end

pump(3)
print("HARNESS OK")

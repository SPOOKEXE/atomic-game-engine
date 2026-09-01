--!strict
-- Headless Bladeborne world for genuine connected-client load stages. The
-- transport harness only connects; every connected character's mind runs here.

local Players = game:GetService("Players")
local workspace = game:GetService("Workspace")
local Modules = script :: any
local BladeborneRuntime = require(Modules.BladeborneRuntime)

Players.MaxPlayers = 1200

local spawn = Instance.new("SpawnLocation")
spawn.Name = "SpawnLocation"
spawn.Size = Vector3.new(40, 1, 40)
spawn.Position = Vector3.new(0, 0.5, 0)
spawn.Anchored = true
spawn.Parent = workspace

local floor = Instance.new("Part")
floor.Name = "BladeborneLoadFloor"
floor.Size = Vector3.new(400, 2, 400)
floor.Position = Vector3.new(0, -1, 0)
floor.Anchored = true
floor.Parent = workspace

BladeborneRuntime.start({
	MindAllPlayers = true,
	ProfilesEnabled = false,
	BindInput = false,
	HudEnabled = false,
})

print("bladeborne load world: connected player minds enabled")

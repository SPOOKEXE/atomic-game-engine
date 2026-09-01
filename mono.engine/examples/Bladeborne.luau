--!strict
-- Bladeborne Floor 0's Atomic runtime. World art comes from the imported place;
-- combat, bots and demo setup live in Luau.

local Players = game:GetService("Players")
local workspace = game:GetService("Workspace")
local Modules = script :: any
local BladeborneRuntime = require(Modules.BladeborneRuntime)

local demo = Instance.new("Model")
demo.Name = "BladeborneDemo"
demo.Parent = workspace

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour
	part.Anchored = true
	part.Parent = demo
	return part
end

local spawn = workspace:FindFirstChild("SpawnLocation", true)
if spawn == nil then
	spawn = block("SpawnLocation", Vector3.new(18, 1, 18), Vector3.new(0, 0.5, 0), Color3.fromRGB(45, 48, 58))
	block("TrainingFloor", Vector3.new(70, 2, 70), Vector3.new(0, -1, 0), Color3.fromRGB(18, 20, 24))
end

local localPlayer = Players.LocalPlayer :: Player?
if localPlayer ~= nil and localPlayer.Character == nil then
	localPlayer:LoadCharacter()
end

local BOT_COUNT = 8
for index = 1, BOT_COUNT do
	local bot = Instance.new("Player")
	bot.Name = `Husk {index}`
	bot:SetAttribute("BladeborneBot", true)
	bot:SetAttribute("BladeborneFaction", if index % 2 == 0 then "warden" else "husk")
	bot.Parent = Players
	local character = bot:LoadCharacter()
	if character ~= nil then
		local angle = (index - 1) * math.pi * 2 / BOT_COUNT
		local origin = (spawn :: BasePart).Position
		character:PivotTo(CFrame.new(origin + Vector3.new(math.cos(angle) * 7, 3, math.sin(angle) * 7)))
	end
end

BladeborneRuntime.start()
print(`bladeborne demo: sword light chain live, {BOT_COUNT} husks thinking`)

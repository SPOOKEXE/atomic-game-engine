--!strict
-- Five thousand independent script actors, each on its own Heartbeat.
--
-- Most scenes run one script that loops over everything it owns. Games run
-- many small ones, each with its own connection and its own instances. This
-- demo builds that shape: every actor owns one part and connects its own
-- Heartbeat handler, which reads an attribute, reads and writes its part's
-- properties, and now and then writes an attribute back. The frame time
-- measures signal dispatch, the per-call cost of the property bindings and
-- attribute storage, and the replication of the rows they change.
--
-- The actors are closures in this one builder rather than separate `Script`
-- instances: a server `Script` added at runtime is not started (only the
-- client role runs new scripts), so separate instances would never run.

local ACTORS = 5_000
local COLUMNS = 100
local SPACING = 1.5
local BOB = 0.6

local workspace = game:GetService("Workspace")
local RunService = game:GetService("RunService")

local rows = ACTORS / COLUMNS

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(COLUMNS * SPACING + 20, 1, rows * SPACING + 20)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
floor.Color = Color3.new(0.12, 0.13, 0.16)
floor.Parent = workspace

local spawn = Instance.new("SpawnLocation")
spawn.Anchored = true
spawn.Size = Vector3.new(6, 1, 6)
spawn.CFrame = CFrame.new(Vector3.new(0, 0.5, rows * SPACING * 0.5 + 6))
spawn.Parent = workspace

local template = Instance.new("Part")
template.Name = "Actor"
template.Anchored = true
template.Size = Vector3.new(0.8, 0.8, 0.8)


local function actor(index: number)
	local column = (index - 1) % COLUMNS
	local row = math.floor((index - 1) / COLUMNS)
	local base = Vector3.new((column - (COLUMNS - 1) / 2) * SPACING, 1, (row - (rows - 1) / 2) * SPACING)

	local part = template:Clone() :: Part
	part.CFrame = CFrame.new(base)
	part:SetAttribute("Phase", index * 0.37)
	part:SetAttribute("Beats", 0)
	part.Parent = workspace

	local beats = 0
	RunService.Heartbeat:Connect(function(delta: number)
		local phase = (part:GetAttribute("Phase") :: number) + delta * 2
		part:SetAttribute("Phase", phase)
		local height = math.sin(phase) * BOB
		part.CFrame = CFrame.new(base + Vector3.new(0, height, 0))
		if part.Transparency ~= 0 then
			part.Transparency = 0
		end
		beats += 1
		-- A slower write, as a game's score or state field would be.
		if beats % 30 == 0 then
			part:SetAttribute("Beats", beats)
			local shade = (phase * 0.05) % 1
			part.Color = Color3.new(0.3 + shade * 0.6, 0.6, 0.9 - shade * 0.5)
		end
	end)
end

for index = 1, ACTORS do
	actor(index)
end
template:Destroy()

local starterPlayerScripts = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
assert(starterPlayerScripts ~= nil, "StarterPlayerScripts is missing")
local demoCamera = starterPlayerScripts:FindFirstChild("DemoCamera")
assert(demoCamera ~= nil, "DemoCamera is missing")
demoCamera:SetAttribute("Name", "Camera")
demoCamera:SetAttribute("FieldOfView", 70)
demoCamera:SetAttribute("FarPlaneZ", 1000)
demoCamera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(0, 70, 110), Vector3.new(0, 0, 0)))

print(string.format("stressscripts: %d actors, one Heartbeat connection each", ACTORS))

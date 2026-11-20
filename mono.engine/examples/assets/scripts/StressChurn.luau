--!strict
-- Thousands of instances created and destroyed every second.
--
-- Every other stress demo builds its world once and then moves it. This one
-- never stops building: each Heartbeat spawns a batch of falling blocks, a
-- batch of anchored pillars and a batch of small models, and destroys every
-- one whose lifetime has run out. The live count stays near steady, so the
-- frame time measures the churn itself:
--
-- - entity creation, id reuse and destruction;
-- - structural replication (new and removed instances, not moved rows);
-- - the physics static index, which a new or removed anchored part rebuilds;
-- - tree changes, since each model is a parent with children;
-- - emitter block claim and reclaim, from the emitters some blocks carry.

local FALLING_PER_TICK = 40
local PILLARS_PER_TICK = 8
local MODELS_PER_TICK = 4
local PARTS_PER_MODEL = 6
local EMITTER_EVERY = 10

local FALLING_LIFE = 4.0
local PILLAR_LIFE = 2.0
local MODEL_LIFE = 3.0
local AREA = 120

local workspace = game:GetService("Workspace")
local RunService = game:GetService("RunService")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(AREA + 40, 1, AREA + 40)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
floor.Color = Color3.new(0.12, 0.13, 0.16)
floor.Parent = workspace

local spawn = Instance.new("SpawnLocation")
spawn.Anchored = true
spawn.Size = Vector3.new(6, 1, 6)
spawn.CFrame = CFrame.new(Vector3.new(0, 0.5, AREA * 0.5 + 10))
spawn.Parent = workspace

local random = Random.new(7)

-- Each live instance and the time it dies, in spawn order. Lifetimes within a
-- list are equal, so the oldest is always at the front.
type Living = { instance: Instance, dies: number }
local falling: { Living } = {}
local pillars: { Living } = {}
local models: { Living } = {}
local fallingFront = 1
local pillarFront = 1
local modelFront = 1

local function spot(height: number): Vector3
	local half = AREA * 0.5
	return Vector3.new(random:NextNumber(-half, half), height, random:NextNumber(-half, half))
end

local function colour(): Color3
	return Color3.new(random:NextNumber(0.3, 1), random:NextNumber(0.3, 1), random:NextNumber(0.3, 1))
end

local spawned = 0

local function spawnFalling(now: number)
	local block = Instance.new("Part")
	block.Size = Vector3.new(1, 1, 1)
	block.CFrame = CFrame.new(spot(random:NextNumber(10, 30)))
	block.Color = colour()
	spawned += 1
	if spawned % EMITTER_EVERY == 0 then
		local emitter = Instance.new("ParticleEmitter")
		emitter.Rate = 20
		emitter.Lifetime = NumberRange.new(1, 1)
		emitter.Speed = NumberRange.new(1, 2)
		emitter.Parent = block
	end
	block.Parent = workspace
	table.insert(falling, { instance = block, dies = now + FALLING_LIFE })
end

local function spawnPillar(now: number)
	local pillar = Instance.new("Part")
	pillar.Anchored = true
	local height = random:NextNumber(2, 8)
	pillar.Size = Vector3.new(1.5, height, 1.5)
	pillar.CFrame = CFrame.new(spot(height * 0.5))
	pillar.Color = colour()
	pillar.Parent = workspace
	table.insert(pillars, { instance = pillar, dies = now + PILLAR_LIFE })
end

local function spawnModel(now: number)
	local model = Instance.new("Model")
	local base = spot(random:NextNumber(4, 12))
	for index = 1, PARTS_PER_MODEL do
		local part = Instance.new("Part")
		part.Anchored = true
		part.Size = Vector3.new(0.8, 0.8, 0.8)
		part.CFrame = CFrame.new(base + Vector3.new(index * 0.9, 0, 0))
		part.Color = colour()
		part.Parent = model
	end
	model.Parent = workspace
	table.insert(models, { instance = model, dies = now + MODEL_LIFE })
end

-- Destroys the expired front of one list and returns the new front. Compacts
-- the list once half of it is dead, so it does not grow for ever.
local function expire(list: { Living }, front: number, now: number): number
	while front <= #list and list[front].dies <= now do
		list[front].instance:Destroy()
		front += 1
	end
	if front > 1 and front > #list / 2 then
		table.move(list, front, #list, 1)
		for index = #list - front + 2, #list do
			list[index] = nil
		end
		front = 1
	end
	return front
end

local starterPlayerScripts = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
assert(starterPlayerScripts ~= nil, "StarterPlayerScripts is missing")
local demoCamera = starterPlayerScripts:FindFirstChild("DemoCamera")
assert(demoCamera ~= nil, "DemoCamera is missing")
demoCamera:SetAttribute("Name", "Camera")
demoCamera:SetAttribute("FieldOfView", 70)
demoCamera:SetAttribute("FarPlaneZ", 1000)
demoCamera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(0, 60, 110), Vector3.new(0, 0, 0)))

local elapsed = 0
RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta
	for _ = 1, FALLING_PER_TICK do
		spawnFalling(elapsed)
	end
	for _ = 1, PILLARS_PER_TICK do
		spawnPillar(elapsed)
	end
	for _ = 1, MODELS_PER_TICK do
		spawnModel(elapsed)
	end
	fallingFront = expire(falling, fallingFront, elapsed)
	pillarFront = expire(pillars, pillarFront, elapsed)
	modelFront = expire(models, modelFront, elapsed)
end)

print(
	string.format(
		"stresschurn: %d falling, %d pillars and %d models of %d a tick",
		FALLING_PER_TICK,
		PILLARS_PER_TICK,
		MODELS_PER_TICK,
		PARTS_PER_MODEL
	)
)

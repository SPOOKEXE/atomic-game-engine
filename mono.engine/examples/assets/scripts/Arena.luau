--!strict
-- The other end of a teleport, and a ring to stand in.
--
-- **A second world exists so that `TeleportService` has somewhere to go.** The
-- Teleport bus has been able to move a payload between two worlds since v0.2 and
-- the studio has run several worlds in parallel since then - and there were no
-- two worlds a player could be in, because there were no players. A teleport
-- with one world is a delete.
--
-- Paired with `Playground.luau`. `ArenaPad.luau` is the pad back; it is a
-- separate file for the reason `PlaygroundPad.luau` gives - a scene that names a
-- destination is a scene that only works where that destination exists, and the
-- playground is also what `scripts/demos/run-local-server.sh` hosts on its own.

local ARENA_RADIUS = 60
local WALL_HEIGHT = 12

local workspace = game:GetService("Workspace")

local FLOOR = Color3.new(0.30, 0.24, 0.22)
local PAD = Color3.new(0.85, 0.45, 0.15)
local WALL = Color3.new(0.42, 0.34, 0.30)

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour
	part.Anchored = true
	part.Parent = workspace
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block("Ground", Vector3.new(ARENA_RADIUS * 2, 4, ARENA_RADIUS * 2), Vector3.new(0, -2 + GROUND_LIFT, 0), FLOOR)

-- Where an arriving character stands. The name is the contract:
-- `scene::FindSpawn` looks for exactly this under `Workspace`.
block("SpawnLocation", Vector3.new(14, 1, 14), Vector3.new(0, 0.5, 0), PAD)

-- Four walls, so the arena reads as a room rather than as a slab. Set back from
-- the edge, because a wall exactly on the boundary of the floor is a wall you
-- can fall past.
local INSET = ARENA_RADIUS - 4
block("WallNorth", Vector3.new(INSET * 2, WALL_HEIGHT, 2), Vector3.new(0, WALL_HEIGHT / 2, -INSET), WALL)
block("WallSouth", Vector3.new(INSET * 2, WALL_HEIGHT, 2), Vector3.new(0, WALL_HEIGHT / 2, INSET), WALL)
block("WallWest", Vector3.new(2, WALL_HEIGHT, INSET * 2), Vector3.new(-INSET, WALL_HEIGHT / 2, 0), WALL)
block("WallEast", Vector3.new(2, WALL_HEIGHT, INSET * 2), Vector3.new(INSET, WALL_HEIGHT / 2, 0), WALL)

-- Something to jump onto, so the arena is worth walking around in.
for index = 1, 6 do
	local angle = (index / 6) * math.pi * 2
	block(
		"Pillar" .. index,
		Vector3.new(6, 4 + index, 6),
		Vector3.new(math.cos(angle) * 24, (4 + index) / 2, math.sin(angle) * 24),
		WALL
	)
end

local TARGET_COLOR = Color3.new(0.75, 0.85, 0.95)
local targets: { { Core: Part, Humanoid: Humanoid, ResetAt: number } } = {}
local function target(name: string, at: Vector3)
	local model = Instance.new("Model")
	model.Name = name
	local core = Instance.new("Part")
	core.Name = "Core"
	core.Size = Vector3.new(4, 6, 4)
	core.Position = at
	core.Color = TARGET_COLOR
	core.Anchored = true
	core.Parent = model
	local humanoid = Instance.new("Humanoid")
	humanoid.MaxHealth = 100
	humanoid.Health = 100
	humanoid.Parent = model
	model.Parent = workspace
	table.insert(targets, { Core = core, Humanoid = humanoid, ResetAt = 0 })
end

target("TargetCenter", Vector3.new(0, 3, -36))
target("TargetLeft", Vector3.new(-18, 3, -38))
target("TargetRight", Vector3.new(18, 3, -38))

local hud = Instance.new("ScreenGui")
hud.Name = "ArenaHud"
local crosshair = Instance.new("TextLabel")
crosshair.Name = "Crosshair"
crosshair.Position = UDim2.new(0.5, -12, 0.5, -18)
crosshair.Size = UDim2.new(0, 24, 0, 36)
crosshair.BackgroundTransparency = 1
crosshair.Text = "+"
crosshair.TextSize = 28
crosshair.TextColor3 = Color3.new(1, 1, 1)
crosshair.Parent = hud
local hint = Instance.new("TextLabel")
hint.Name = "Hint"
hint.Position = UDim2.new(0, 20, 1, -38)
hint.Size = UDim2.new(0, 320, 0, 24)
hint.BackgroundTransparency = 1
hint.Text = "CLICK / R2 TO FIRE  ·  REQUIRES ARENA SERVER"
hint.TextSize = 14
hint.TextColor3 = Color3.new(1, 1, 1)
hint.Parent = hud
hud.Parent = game:GetService("StarterGui")

-- The arena owns its weapon rules. A client supplies aim, while this world
-- uses its current physics state to choose reach, obstruction, damage, and
-- the resulting replicated state. Shots do not rewind to a prior tick.
local RunService = game:GetService("RunService")
if not RunService:IsServer() then
	return
end

local Players = game:GetService("Players")
local HttpService = game:GetService("HttpService")
local fire = Instance.new("RemoteEvent")
fire.Name = "ArenaFire"
fire.Parent = game:GetService("ReplicatedStorage")

local SHOT_RANGE = 100
local SHOT_DAMAGE = 25
local CAMERA_REACH = 45
local SHOT_INTERVAL = 0.15
local HIT_COLOR = Color3.new(1, 0.2, 0.1)
local elapsed = 0
local lastShot: { [number]: number } = {}
Players.PlayerRemoving:Connect(function(player: Instance)
	lastShot[(player :: Player).UserId] = nil
end)
RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta
	for _, entry in targets do
		if entry.ResetAt > 0 and elapsed >= entry.ResetAt then
			entry.ResetAt = 0
			entry.Humanoid.Health = entry.Humanoid.MaxHealth
			entry.Core.Color = TARGET_COLOR
		end
	end
end)

local function finite(value: unknown): boolean
	return typeof(value) == "number" and value == value and math.abs(value) < 100000
end

fire.OnServerEvent:Connect(function(payload: string, sender: Player?)
	if sender == nil or sender.Parent == nil or not sender.Parent:IsA("Players") or #payload > 256 then return end
	local character = sender.Character
	if character == nil then return end
	local root = character:FindFirstChild("HumanoidRootPart") :: BasePart?
	local shooter = character:FindFirstChildOfClass("Humanoid") :: Humanoid?
	if root == nil or shooter == nil or shooter.Health <= 0 then return end

	local previous = lastShot[sender.UserId]
	if previous ~= nil and elapsed - previous < SHOT_INTERVAL then return end
	local decodedOk, aim = pcall(function() return HttpService:JSONDecode(payload) end)
	if not decodedOk or typeof(aim) ~= "table" then return end
	if not finite(aim.x) or not finite(aim.y) or not finite(aim.z)
		or not finite(aim.dx) or not finite(aim.dy) or not finite(aim.dz) then return end

	local origin = Vector3.new(aim.x, aim.y, aim.z)
	local direction = Vector3.new(aim.dx, aim.dy, aim.dz)
	local directionLength = direction.Magnitude
	if (origin - root.Position).Magnitude > CAMERA_REACH
		or directionLength < 0.999 or directionLength > 1.001 then return end
	lastShot[sender.UserId] = elapsed

	-- Cast from the character so a camera behind a wall cannot fire through it.
	local muzzle = root.Position + Vector3.new(0, 3, 0)
	local aimPoint = origin + direction * SHOT_RANGE
	local travel = aimPoint - muzzle
	if travel.Magnitude <= 0 then return end
	local hit = workspace:Raycast(muzzle, travel.Unit * SHOT_RANGE)
	if hit == nil or not hit.Instance:IsA("BasePart") then return end
	local part = hit.Instance :: BasePart
	local target = part.Parent
	if target == character then return end
	part.Color = HIT_COLOR
	if target == nil then return end
	local humanoid = target:FindFirstChildOfClass("Humanoid") :: Humanoid?
	if humanoid ~= nil and humanoid.Health > 0 then
		humanoid.Health = math.max(0, humanoid.Health - SHOT_DAMAGE)
		if humanoid.Health == 0 then
			for _, entry in targets do
				if entry.Humanoid == humanoid then
					entry.ResetAt = elapsed + 3
					break
				end
			end
		end
	end
end)

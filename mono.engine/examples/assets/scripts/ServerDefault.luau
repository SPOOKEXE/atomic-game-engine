--!strict
-- The headless server's default moving field. Velocity is an attribute so a
-- snapshot can start this script again without losing each cube's direction.

local workspace = game:GetService("Workspace")
local RunService = game:GetService("RunService")

local HALF_EXTENT = 64
local countValue = workspace:GetAttribute("ServerDefaultCount")
local count = if typeof(countValue) == "number" then math.max(0, math.floor(countValue)) else 512

local existingField = workspace:FindFirstChild("ServerDefault")
local field: Instance
if existingField == nil then
	local created = Instance.new("Folder")
	created.Name = "ServerDefault"
	created.Parent = workspace
	field = created
else
	field = existingField
end

local cubes: { Instance } = {}
local positions: { CFrame } = {}
-- Each cube's velocity, kept in Luau so the beat does not read the attribute
-- per cube per beat. It follows the attribute when anything else writes it,
-- and the beat writes the attribute only on a bounce.
local velocities: { Vector3 } = {}
-- FindFirstChild scans siblings, including on a fresh field as it grows.
-- Index existing children once so both creation and snapshot restore scale.
local existingByName: { [string]: Instance } = {}
for _, child in field:GetChildren() do
	if existingByName[child.Name] == nil then
		existingByName[child.Name] = child
	end
end

for index = 1, count do
	local name = `Cube{index}`
	local existing = existingByName[name]
	local cube: Part
	if existing == nil then
		local random = Random.new(index)
		cube = Instance.new("Part")
		cube.Name = name
		cube.Anchored = true
		cube.CanCollide = false
		cube.Size = Vector3.one
		cube.Position = Vector3.new(
			random:NextNumber(-HALF_EXTENT, HALF_EXTENT),
			random:NextNumber(-HALF_EXTENT, HALF_EXTENT),
			random:NextNumber(-HALF_EXTENT, HALF_EXTENT)
		)
		cube.Color = Color3.new(
			random:NextNumber(0.15, 0.90),
			random:NextNumber(0.20, 0.80),
			random:NextNumber(0.35, 0.95)
		)
		cube:SetAttribute("Velocity", Vector3.new(
			random:NextNumber(-10, 10),
			random:NextNumber(-10, 10),
			random:NextNumber(-10, 10)
		))
		cube.Parent = field
	else
		assert(existing:IsA("Part"), `wrong class for {name}`)
		cube = existing :: Part
	end

	local velocity = cube:GetAttribute("Velocity")
	assert(typeof(velocity) == "Vector3", `missing velocity on {name}`)
	cubes[index] = cube
	positions[index] = cube.CFrame
	velocities[index] = velocity :: Vector3
	cube:GetAttributeChangedSignal("Velocity"):Connect(function()
		local changed = cube:GetAttribute("Velocity")
		if typeof(changed) == "Vector3" then
			velocities[index] = changed
		end
	end)
end

RunService.Heartbeat:Connect(function(delta: number)
	if delta == 0 then
		return
	end

	for index, cube in cubes do
		local position = positions[index].Position
		local direction = velocities[index]
		local x = position.X + direction.X * delta
		local y = position.Y + direction.Y * delta
		local z = position.Z + direction.Z * delta
		local vx, vy, vz = direction.X, direction.Y, direction.Z

		if x > HALF_EXTENT then
			x, vx = HALF_EXTENT, -math.abs(vx)
		elseif x < -HALF_EXTENT then
			x, vx = -HALF_EXTENT, math.abs(vx)
		end
		if y > HALF_EXTENT then
			y, vy = HALF_EXTENT, -math.abs(vy)
		elseif y < -HALF_EXTENT then
			y, vy = -HALF_EXTENT, math.abs(vy)
		end
		if z > HALF_EXTENT then
			z, vz = HALF_EXTENT, -math.abs(vz)
		elseif z < -HALF_EXTENT then
			z, vz = -HALF_EXTENT, math.abs(vz)
		end

		positions[index] = CFrame.new(x, y, z)
		if vx ~= direction.X or vy ~= direction.Y or vz ~= direction.Z then
			local bounced = Vector3.new(vx, vy, vz)
			velocities[index] = bounced
			cube:SetAttribute("Velocity", bounced)
		end
	end

	workspace:BulkMoveTo(cubes, positions)
end)

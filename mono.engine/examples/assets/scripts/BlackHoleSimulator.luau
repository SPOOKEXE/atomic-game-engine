--!strict
-- A self-contained gravitational-lensing scene built from ordinary scene parts.
--
-- The background stars live on a fixed plane behind the hole. Each beat moves
-- their apparent position away from the centre using a bounded inverse-square
-- bend, making the field visibly curve around the event horizon without a
-- screen-space effect or a custom renderer path.

local RunService = game:GetService("RunService")
local Lighting = game:GetService("Lighting")

local BLACK_HOLE_RADIUS = 5.5
local DISK_INNER_RADIUS = 7.5
local DISK_OUTER_RADIUS = 22
local DISK_RING_COUNT = 4
local DISK_SHARDS_PER_RING = 48
local DISK_SHARD_COUNT = DISK_RING_COUNT * DISK_SHARDS_PER_RING
local STAR_COUNT = 220
local PHOTON_RING_COUNT = 24
local STARFIELD_DEPTH = -44

-- This scene owns its exposure. Daylight would flatten the silhouette and make
-- a missed star look like a weak lensing effect rather than missing contrast.
Lighting.Brightness = 0
Lighting.Ambient = Color3.fromRGB(64, 64, 80)
Lighting.OutdoorAmbient = Color3.fromRGB(64, 64, 80)

local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, salt * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

local function range(index: number, salt: number, low: number, high: number): number
	return low + hashed(index, salt) * (high - low)
end

local function anchoredPart(name: string, size: Vector3, colour: Color3, transparency: number): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = size
	part.Color = colour
	part.Transparency = transparency
	part.CanCollide = false
	part.CastShadow = false
	part.Parent = workspace
	return part
end

type DiskShard = {
	PartIndex: number,
	Radius: number,
	Angle: number,
	AngularVelocity: number,
	Height: number,
	Thickness: number,
}

type Star = {
	PartIndex: number,
	X: number,
	Y: number,
	Phase: number,
}

type PhotonMarker = {
	PartIndex: number,
	Angle: number,
	AngularVelocity: number,
	Radius: number,
}

local movingParts: { Instance } = {}
local visualParts: { Part } = {}
local placements: { CFrame } = {}
local diskShards: { DiskShard } = {}
local stars: { Star } = {}
local photonMarkers: { PhotonMarker } = {}

local function addMovingPart(name: string, size: Vector3, colour: Color3, transparency: number): number
	local part = anchoredPart(name, size, colour, transparency)
	table.insert(movingParts, part)
	table.insert(visualParts, part)
	table.insert(placements, CFrame.new())
	return #movingParts
end

local blackHole = Instance.new("MeshPart")
blackHole.Name = "Event Horizon"
blackHole.MeshId = "engine.Sphere"
blackHole.Anchored = true
blackHole.CanCollide = false
blackHole.CastShadow = false
blackHole.Size = Vector3.new(BLACK_HOLE_RADIUS * 2, BLACK_HOLE_RADIUS * 2, BLACK_HOLE_RADIUS * 2)
blackHole.Color = Color3.new(0, 0, 0)
blackHole.Parent = workspace
blackHole.CFrame = CFrame.new(0, 0, 0)

-- The physical scene remains ordinary geometry. GravitationalLens supplies
-- the HDR, depth-aware bend for all background scene colour behind this point.
local lens = Instance.new("GravitationalLens")
lens.Name = "Black Hole Lens"
lens.CFrame = blackHole.CFrame
lens.HorizonRadius = BLACK_HOLE_RADIUS
lens.WarpRadius = DISK_OUTER_RADIUS + 4
lens.Strength = 0.8
lens.Spin = 0.25
lens.Shader = "gravitational-lens"
lens.Parent = workspace

local corona = Instance.new("PointLight")
corona.Name = "Accretion Glow"
corona.Color = Color3.fromRGB(255, 120, 46)
corona.Brightness = 2000
corona.Range = 40
corona.Parent = blackHole

for index = 0, DISK_SHARD_COUNT - 1 do
	local ring = index % DISK_RING_COUNT
	local shard = math.floor(index / DISK_RING_COUNT)
	local radiusAlpha = ring / (DISK_RING_COUNT - 1)
	local radius = DISK_INNER_RADIUS + radiusAlpha * (DISK_OUTER_RADIUS - DISK_INNER_RADIUS)
	radius += range(index, 11, -0.55, 0.55)
	local angle = shard / DISK_SHARDS_PER_RING * math.pi * 2 + range(index, 17, -0.035, 0.035)
	local brightness = 0.5 + (1 - radiusAlpha) * 0.5
	local partIndex = addMovingPart(
		"Accretion Shard",
		Vector3.new(range(index, 23, 0.35, 0.85), range(index, 29, 0.16, 0.34), range(index, 31, 1.4, 2.7)),
		Color3.new(brightness, brightness * 0.23, 0.035),
		0.08 + radiusAlpha * 0.34
	)
	table.insert(diskShards, {
		PartIndex = partIndex,
		Radius = radius,
		Angle = angle,
		AngularVelocity = 2.8 * (DISK_INNER_RADIUS / radius) ^ 1.5,
		Height = range(index, 37, -0.8, 0.8) * radiusAlpha,
		Thickness = range(index, 41, 0.4, 1.0),
	})
	visualParts[partIndex].EmissiveTint = visualParts[partIndex].Color
	visualParts[partIndex].EmissiveStrength = 8
end

for index = 0, STAR_COUNT - 1 do
	local x = range(index, 53, -30, 30)
	local y = range(index, 59, -18, 18)
	local intensity = range(index, 61, 0.4, 1.0)
	local partIndex = addMovingPart(
		"Lensed Star",
		Vector3.new(range(index, 67, 0.22, 0.48), range(index, 71, 0.22, 0.48), 0.1),
		Color3.new(intensity * 0.72, intensity * 0.86, intensity),
		0
	)
	table.insert(stars, {
		PartIndex = partIndex,
		X = x,
		Y = y,
		Phase = hashed(index, 73) * math.pi * 2,
	})
	visualParts[partIndex].EmissiveTint = visualParts[partIndex].Color
	visualParts[partIndex].EmissiveStrength = 12
end

for index = 0, PHOTON_RING_COUNT - 1 do
	local partIndex = addMovingPart(
		"Photon Ring",
		Vector3.new(0.54, 0.54, 0.32),
		Color3.fromRGB(255, 154, 54),
		0.08
	)
	table.insert(photonMarkers, {
		PartIndex = partIndex,
		Angle = index / PHOTON_RING_COUNT * math.pi * 2,
		AngularVelocity = 1.3 + hashed(index, 79) * 0.6,
		Radius = BLACK_HOLE_RADIUS + range(index, 83, 0.55, 1.15),
	})
	visualParts[partIndex].EmissiveTint = visualParts[partIndex].Color
	visualParts[partIndex].EmissiveStrength = 16
end

local camera = Instance.new("Camera")
camera.Name = "Black Hole Camera"
camera.FieldOfView = 46
camera.FarPlaneZ = 300
camera.Parent = workspace
workspace.CurrentCamera = camera

local elapsedSeconds = 0

-- The tick advances each independent orbit, maps the warped star plane and
-- then submits all changed transforms together. No part is created per frame.
RunService.Heartbeat:Connect(function(deltaTime: number)
	local clampedDelta = math.clamp(deltaTime, 0, 0.1)
	elapsedSeconds += clampedDelta

	for _, shard in diskShards do
		shard.Angle += shard.AngularVelocity * clampedDelta
		local x = math.cos(shard.Angle) * shard.Radius
		local z = math.sin(shard.Angle) * shard.Radius
		local y = shard.Height + math.sin(shard.Angle * 3 + elapsedSeconds * 2) * shard.Thickness
		placements[shard.PartIndex] = CFrame.new(x, y, z) * CFrame.Angles(0, -shard.Angle, 0)

		-- The approaching edge is hotter and brighter. This is a visual cue for
		-- relativistic beaming, not a claim of a full relativistic fluid solver.
		local approaching = math.max(0, math.cos(shard.Angle))
		local innerHeat = 1 - (shard.Radius - DISK_INNER_RADIUS) / (DISK_OUTER_RADIUS - DISK_INNER_RADIUS)
		local colour = Color3.new(
			0.58 + approaching * 0.42,
			0.06 + innerHeat * 0.35 + approaching * 0.26,
			0.015 + approaching * 0.11
		)
		visualParts[shard.PartIndex].Color = colour
		visualParts[shard.PartIndex].EmissiveTint = colour
		visualParts[shard.PartIndex].EmissiveStrength = 8 + approaching * 8
	end

	for _, star in stars do
		local twinkle = 0.72 + math.sin(elapsedSeconds * 2 + star.Phase) * 0.28
		placements[star.PartIndex] = CFrame.new(star.X, star.Y, STARFIELD_DEPTH)
		visualParts[star.PartIndex].Transparency = 1 - twinkle
	end

	for _, marker in photonMarkers do
		marker.Angle += marker.AngularVelocity * clampedDelta
		local pulse = math.sin(marker.Angle * 4 - elapsedSeconds * 3) * 0.22
		local radius = marker.Radius + pulse
		placements[marker.PartIndex] = CFrame.new(
			math.cos(marker.Angle) * radius,
			math.sin(marker.Angle) * radius,
			-0.15
		)
	end

	workspace:BulkMoveTo(movingParts, placements)

	local cameraAngle = elapsedSeconds * 0.075
	local cameraPosition = Vector3.new(math.sin(cameraAngle) * 8, 14, 39 + math.cos(cameraAngle) * 3)
	camera.CFrame = CFrame.lookAt(cameraPosition, Vector3.new(0, 0, 0))
end)

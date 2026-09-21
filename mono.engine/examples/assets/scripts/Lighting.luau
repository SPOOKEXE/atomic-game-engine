--!strict
-- A compact visual check for every property the Lighting service exposes.
--
-- The white blocks recede into blue fog, their upward faces receive the blue
-- outdoor term, and the noon sun casts the strongest directional term. Editing
-- any value below gives one frame where its runtime consumer is easy to see.

local Lighting = game:GetService("Lighting")

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = CFrame.new(at)
	part.Color = colour
	part.Anchored = true
	part.Parent = workspace
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block("Ground", Vector3.new(70, 1, 110), Vector3.new(0, -0.5 + GROUND_LIFT, -35), Color3.fromRGB(118, 118, 118))

for index = 0, 4 do
	local depth = -index * 18
	block(
		"LightingMarker" .. index,
		Vector3.new(7, 7, 7),
		Vector3.new(if index % 2 == 0 then -5 else 5, 3.5, depth),
		Color3.fromRGB(235, 235, 235)
	)
end

Lighting.Ambient = Color3.fromRGB(18, 20, 28)
Lighting.OutdoorAmbient = Color3.fromRGB(75, 105, 170)
Lighting.Brightness = 2.5
Lighting.ClockTime = 12
Lighting.GeographicLatitude = 0
Lighting.FogColor = Color3.fromRGB(72, 118, 175)
Lighting.FogStart = 24
Lighting.FogEnd = 82

local eye = Instance.new("Camera")
eye.Name = "LightingCamera"
eye.FieldOfView = 62
eye.CFrame = CFrame.lookAt(Vector3.new(20, 15, 22), Vector3.new(0, 3, -35))
eye.Parent = workspace
workspace.CurrentCamera = eye

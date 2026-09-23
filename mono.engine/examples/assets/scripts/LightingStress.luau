--!strict

-- A fixed high-count lighting course for release frame captures.
--
-- Point and spot lamps plus fog volumes are countable authored objects. Bloom,
-- depth of field and god rays are world settings, while one atmosphere and one
-- cloud provider are selected from Lighting, so this scene enables each once.

local Lighting = game:GetService("Lighting")

local LIGHTS_PER_KIND = 256
local VOLUME_COUNT = 256

Lighting.ClockTime = 18.4
Lighting.GeographicLatitude = 28
Lighting.Brightness = 2.5
Lighting.Ambient = Color3.fromRGB(18, 22, 34)
Lighting.OutdoorAmbient = Color3.fromRGB(42, 54, 80)
Lighting.FogColor = Color3.fromRGB(70, 88, 120)
Lighting.FogStart = 90
Lighting.FogEnd = 420
Lighting.BloomThreshold = 0.7
Lighting.BloomIntensity = 1.25
Lighting.BloomRadius = 10
Lighting.DepthOfFieldIntensity = 0.7
Lighting.DepthOfFieldFocusDistance = 72
Lighting.DepthOfFieldFocusRange = 18
Lighting.DepthOfFieldRadius = 9
Lighting.GodRayIntensity = 0.8
Lighting.GodRayThreshold = 0.65
Lighting.GodRayRadius = 72

local sky = Instance.new("SkyboxCompute")
sky.Name = "Lighting Stress Sky"
sky.Shader = Enum.SkyboxComputeShader.Sunset
sky.SunSize = 0.09
sky.Seed = 247
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Lighting Stress Atmosphere"
atmosphere.Shader = Enum.AtmosphereProceduralShader.Earth
atmosphere.Color = Color3.fromRGB(112, 139, 178)
atmosphere.Decay = Color3.fromRGB(61, 78, 112)
atmosphere.Density = 0.28
atmosphere.Haze = 1.4
atmosphere.Glare = 0.6
atmosphere.Samples = 16
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Lighting Stress Clouds"
clouds.Shader = Enum.CloudComputeShader.Cumulus
clouds.Color = Color3.fromRGB(138, 151, 174)
clouds.Cover = 0.62
clouds.Density = 0.76
clouds.WindDirection = Vector2.new(0.8, 0.35)
clouds.WindSpeed = 10
clouds.CellSize = 0.22
clouds.Detail = 0.7
clouds.Height = 0.46
clouds.Thickness = 0.56
clouds.Steps = 24
clouds.Seed = 811
clouds.Parent = Lighting

local function part(name: string, size: Vector3, position: Vector3, colour: Color3): Part
	local result = Instance.new("Part")
	result.Name = name
	result.Size = size
	result.Position = position
	result.Color = colour
	result.Anchored = true
	result.Parent = workspace
	return result
end

part("Lighting Stress Floor", Vector3.new(220, 1, 300), Vector3.new(0, -0.5, -120), Color3.fromRGB(27, 32, 48))

-- A broad receiver field lets camera collection rank light influence against
-- real visible bounds instead of only the camera point.
for row = 0, 15 do
	for column = 0, 15 do
		local height = 3 + (row + column) % 5
		part(
			"Lighting Stress Receiver",
			Vector3.new(3, height, 3),
			Vector3.new((column - 7.5) * 12, height * 0.5, -20 - row * 14),
			Color3.fromRGB(86 + row * 3, 72 + column * 4, 94 + (row + column) * 2)
		)
	end
end

for index = 0, LIGHTS_PER_KIND - 1 do
	local column = index % 32
	local row = math.floor(index / 32)
	local position = Vector3.new((column - 15.5) * 6, 7 + (row % 3) * 4, -16 - row * 22)
	local fixture = part("Lighting Stress Lamp", Vector3.new(0.5, 0.5, 0.5), position, Color3.new(1, 1, 1))
	fixture.Visible = false

	local point = Instance.new("PointLight")
	point.Name = "Lighting Stress Point " .. index
	point.Color = Color3.fromRGB(255, 116 + (index % 5) * 24, 74 + (index % 7) * 20)
	point.Brightness = 38
	point.Range = 26
	point.Parent = fixture

	local spot = Instance.new("SpotLight")
	spot.Name = "Lighting Stress Spot " .. index
	spot.Color = Color3.fromRGB(78 + (index % 6) * 22, 134 + (index % 4) * 26, 255)
	spot.Brightness = 42
	spot.Range = 30
	spot.Angle = 82
	spot.Face = Enum.NormalId.Bottom
	spot.Parent = fixture
end

-- The first sixteen overlap the course and are the expensive GPU payload. The
-- remaining authored volumes fan out through the course to exercise selection.
for index = 0, VOLUME_COUNT - 1 do
	local volume = Instance.new("Volume")
	volume.Name = "Lighting Stress Fog " .. index
	local column = index % 16
	local row = math.floor(index / 16)
	volume.Position = Vector3.new((column - 7.5) * 10, 5 + (row % 3) * 2, -36 - row * 14)
	volume.Bounds = if index < 16 then Vector3.new(82, 20, 120) else Vector3.new(8, 6, 8)
	volume.Shape = if index % 2 == 0 then Enum.VolumeShape.Ellipsoid else Enum.VolumeShape.Box
	volume.Color = Color3.fromRGB(105 + (index % 4) * 20, 126 + (index % 5) * 14, 166 + (index % 3) * 20)
	volume.Density = 0.3
	volume.Extinction = 0.42
	volume.Falloff = 0.55
	volume.NoiseScale = 0.18
	volume.NoiseStrength = 0.66
	volume.Steps = 48
	volume.ShadowSteps = 16
	volume.Seed = 1000 + index
	volume.Parent = workspace
end

local camera = Instance.new("Camera")
camera.Name = "Lighting Stress Camera"
camera.CFrame = CFrame.lookAt(Vector3.new(48, 20, 54), Vector3.new(0, 7, -96))
camera.FieldOfView = 62
camera.FarPlaneZ = 600
camera.Parent = workspace
workspace.CurrentCamera = camera

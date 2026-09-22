--!strict

-- A bright sun, procedural atmosphere and clouds make it easy to inspect the
-- HDR bloom filter against the environment rather than against a flat colour.

local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 17.25
Lighting.GeographicLatitude = 34
Lighting.Brightness = 4.5
Lighting.BloomThreshold = 0.8
Lighting.BloomIntensity = 1.35
Lighting.BloomRadius = 10

local sky = Instance.new("SkyboxCompute")
sky.Name = "Bloom Sun"
sky.SunSize = 0.09
sky.Seed = 91
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Bloom Atmosphere"
atmosphere.Density = 0.24
atmosphere.Haze = 1.2
atmosphere.Glare = 0.7
atmosphere.Samples = 12
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Bloom Clouds"
clouds.Cover = 0.46
clouds.Density = 0.72
clouds.WindSpeed = 9
clouds.Parent = Lighting

local floor = Instance.new("Part")
floor.Name = "Bloom Floor"
floor.Size = Vector3.new(80, 1, 80)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(23, 28, 42)
floor.Anchored = true
floor.Parent = workspace

-- A dense emissive field is the stress case for the same HDR filter that
-- blooms the computed sun through clouds and atmosphere.
for index = 1, 512 do
	local orb = Instance.new("Part")
	orb.Name = "Bloom Orb " .. index
	orb.Size = Vector3.new(1.5, 1.5, 1.5)
	local column = (index - 1) % 32
	local row = math.floor((index - 1) / 32)
	orb.Position = Vector3.new((column - 15.5) * 2.4, 2 + (row % 3), -8 - row * 2)
	orb.Color = Color3.fromRGB(255, 174, 82)
	orb.EmissiveStrength = 5
	orb.Anchored = true
	orb.Parent = workspace
end

local camera = Instance.new("Camera")
camera.Name = "Bloom Camera"
camera.CFrame = CFrame.lookAt(Vector3.new(24, 13, 35), Vector3.new(0, 4, -8))
camera.FieldOfView = 58
camera.Parent = workspace
workspace.CurrentCamera = camera

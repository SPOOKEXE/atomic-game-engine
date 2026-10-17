--!strict

-- This view combines the HDR environment with the two depth-aware lighting
-- controls so a scene author can inspect their interaction with fog and bloom.

local Lighting = game:GetService("Lighting")
Lighting.ClockTime = 11.5
Lighting.GeographicLatitude = 0
Lighting.Brightness = 5
Lighting.FogColor = Color3.fromRGB(98, 129, 166)
Lighting.FogStart = 18
Lighting.FogEnd = 92
Lighting.BloomThreshold = 0.9
Lighting.BloomIntensity = 0.8
Lighting.BloomRadius = 8
Lighting.DepthOfFieldIntensity = 0.85
Lighting.DepthOfFieldFocusDistance = 24
Lighting.DepthOfFieldFocusRange = 6
Lighting.DepthOfFieldRadius = 8
Lighting.GodRayIntensity = 0.7
Lighting.GodRayThreshold = 0.8
Lighting.GodRayRadius = 64

local sky = Instance.new("SkyboxCompute")
sky.Name = "Rays Sky"
sky.SunSize = 0.1
sky.Seed = 47
sky.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Rays Clouds"
clouds.Cover = 0.52
clouds.Density = 0.68
clouds.WindSpeed = 7
clouds.Parent = Lighting

for index = 1, 18 do
	local pillar = Instance.new("Part")
	pillar.Name = "Focus Pillar " .. index
	pillar.Size = Vector3.new(2, 2 + index * 0.7, 2)
	pillar.Position = Vector3.new((index % 6 - 2.5) * 4, pillar.Size.Y * 0.5, -8 - index * 4)
	pillar.Color = Color3.fromRGB(221, 191, 124)
	pillar.Anchored = true
	pillar.Parent = workspace
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(Vector3.new(13, 8, 29), Vector3.new(0, 7, -30))
camera.FieldOfView = 55
camera.Parent = workspace
workspace.CurrentCamera = camera

--!strict

-- This view combines the HDR environment with the two depth-aware lighting
-- controls so a scene author can inspect their interaction with fog and bloom.

local Lighting = game:GetService("Lighting")
local fog = Instance.new("FogEffect")
fog.Name = "Rays Fog"
fog.Enabled = true
fog.Parent = Lighting
local bloom = Instance.new("BloomEffect")
bloom.Name = "Rays Bloom"
bloom.Enabled = true
bloom.Parent = Lighting
local depthOfField = Instance.new("DepthOfFieldEffect")
depthOfField.Name = "Rays Depth Of Field"
depthOfField.Enabled = true
depthOfField.Parent = Lighting
local godRays = Instance.new("GodRaysEffect")
godRays.Name = "Rays God Rays"
godRays.Enabled = true
godRays.Parent = Lighting
Lighting.ClockTime = 11.5
Lighting.GeographicLatitude = 0
Lighting.Brightness = 5
fog.Color = Color3.fromRGB(98, 129, 166)
fog.Start = 18
fog.End = 92
bloom.Threshold = 0.9
bloom.Intensity = 0.8
bloom.Radius = 8
depthOfField.Intensity = 0.85
depthOfField.FocusDistance = 24
depthOfField.FocusRange = 6
depthOfField.Radius = 8
godRays.Intensity = 0.7
godRays.Threshold = 0.8
godRays.Radius = 64

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

local starterPlayerScripts = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
assert(starterPlayerScripts ~= nil, "the starter player scripts container is missing")
local camera = assert(starterPlayerScripts:FindFirstChild("DemoCamera"), "the demo camera companion is missing")
camera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(13, 8, 29), Vector3.new(0, 7, -30)))
camera:SetAttribute("FieldOfView", 55)

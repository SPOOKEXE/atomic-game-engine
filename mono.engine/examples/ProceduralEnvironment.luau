--!strict
-- Built-in environment compute examples. Select each Instance in Studio and
-- use its Shader dropdown to switch between the shipped programs.

local Lighting = game:GetService("Lighting")

local sky = Instance.new("SkyboxCompute")
sky.Name = "Computed Sky"
sky.Shader = Enum.SkyboxComputeShader.Nebula
sky.StarDensity = 0.35
sky.SunSize = 0.035
sky.Seed = 41
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Procedural Atmosphere"
atmosphere.Shader = Enum.AtmosphereProceduralShader.Alien
atmosphere.Color = Color3.fromRGB(104, 188, 154)
atmosphere.Decay = Color3.fromRGB(72, 37, 118)
atmosphere.Density = 0.2
atmosphere.Haze = 1.4
atmosphere.Glare = 0.3
atmosphere.Samples = 16
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Computed Clouds"
clouds.Shader = Enum.CloudComputeShader.Voxel
clouds.Color = Color3.fromRGB(210, 226, 238)
clouds.Cover = 0.48
clouds.Density = 0.8
clouds.CellSize = 0.3
clouds.Detail = 0.7
clouds.Height = 0.68
clouds.Thickness = 0.2
clouds.Steps = 32
clouds.Seed = 73
clouds.Parent = Lighting

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Size = Vector3.new(110, 1, 110)
ground.CFrame = CFrame.new(0, -0.5, -24)
ground.Color = Color3.fromRGB(52, 58, 72)
ground.Anchored = true
ground.Parent = workspace

for index = 0, 11 do
	local marker = Instance.new("Part")
	marker.Name = "ComputeMarker" .. index
	marker.Size = Vector3.new(4, 3 + index * 0.35, 4)
	marker.CFrame = CFrame.new((index % 4 - 1.5) * 11, marker.Size.Y * 0.5, -8 - math.floor(index / 4) * 14)
	marker.Color = Color3.fromRGB(82 + index * 10, 105 + index * 7, 150 + index * 6)
	marker.Anchored = true
	marker.Parent = workspace
end

local eye = Instance.new("Camera")
eye.Name = "ProceduralEnvironmentCamera"
eye.FieldOfView = 70
eye.CFrame = CFrame.lookAt(Vector3.new(26, 15, 24), Vector3.new(0, 5, -24))
eye.Parent = workspace
workspace.CurrentCamera = eye

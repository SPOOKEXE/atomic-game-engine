--!strict
-- A CDN cubemap plus the resident procedural atmosphere and cloud layers.
--
-- Bake and publish the six Allsky faces under `allsky/cartoon-blue/`, then add
-- that store under Edit > Preferences > Content. The renderer demands only
-- these six selected names and rebuilds the shared environment texture once
-- when they arrive.

local Lighting = game:GetService("Lighting")

local sky = Instance.new("SkyboxTextures")
sky.Name = "Cartoon Blue Sky"
sky.Front = "allsky/cartoon-blue/front.atex"
sky.Back = "allsky/cartoon-blue/back.atex"
sky.Left = "allsky/cartoon-blue/left.atex"
sky.Right = "allsky/cartoon-blue/right.atex"
sky.Up = "allsky/cartoon-blue/up.atex"
sky.Down = "allsky/cartoon-blue/down.atex"
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Blue Scattering"
atmosphere.Density = 0.22
atmosphere.Haze = 0.8
atmosphere.Glare = 0.15
atmosphere.Rayleigh = 1.0
atmosphere.Mie = 0.12
atmosphere.Samples = 12
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Voxel Clouds"
clouds.Cover = 0.42
clouds.Density = 0.7
clouds.WindSpeed = 12
clouds.WindDirection = Vector2.new(-0.4, 1)
clouds.CellSize = 0.35
clouds.Detail = 0.65
clouds.Height = 0.72
clouds.Thickness = 0.18
clouds.Steps = 28
clouds.Parent = Lighting

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Size = Vector3.new(100, 1, 100)
ground.CFrame = CFrame.new(0, -0.5, -20)
ground.Color = Color3.fromRGB(95, 105, 118)
ground.Anchored = true
ground.Parent = workspace

for index = 0, 8 do
	local marker = Instance.new("Part")
	marker.Name = "Marker" .. index
	marker.Size = Vector3.new(4, 4 + index * 0.5, 4)
	marker.CFrame = CFrame.new((index % 3 - 1) * 12, marker.Size.Y * 0.5, -8 - math.floor(index / 3) * 15)
	marker.Color = Color3.fromRGB(150 + index * 8, 120 + index * 5, 105 + index * 4)
	marker.Anchored = true
	marker.Parent = workspace
end

local eye = Instance.new("Camera")
eye.Name = "EnvironmentCamera"
eye.FieldOfView = 68
eye.CFrame = CFrame.lookAt(Vector3.new(25, 14, 22), Vector3.new(0, 5, -24))
eye.Parent = workspace
workspace.CurrentCamera = eye

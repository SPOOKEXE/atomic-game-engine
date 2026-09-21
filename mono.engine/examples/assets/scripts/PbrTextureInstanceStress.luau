--!strict
-- PBR texture instancing stress test: one unique 4096-square image on every sphere.
--
-- The spheres share one EditableImage content id. Compare this run with
-- PbrTextureUniqueStress to separate instance work from texture residency.
--
--     client --script PbrTextureInstanceStress.luau

local Lighting = game:GetService("Lighting")

local TEXTURE_SIZE = 4096
local SPHERE_COUNT = 100
local GRID_SIDE = 10
local SPACING = 7
local SPHERE_SIZE = 5

local texture = Instance.new("EditableImage")
texture.Name = "InstancedPbr4K"
assert(texture:Resize(TEXTURE_SIZE, TEXTURE_SIZE), "InstancedPbr4K resize was refused")
assert(
	texture:DrawRectangle(
		Vector2.new(0, 0),
		Vector2.new(TEXTURE_SIZE, TEXTURE_SIZE),
		Color3.fromRGB(76, 132, 206)
	),
	"InstancedPbr4K paint was refused"
)

local function SpherePosition(index: number): Vector3
	local column = (index - 1) % GRID_SIDE
	local row = math.floor((index - 1) / GRID_SIDE)
	return Vector3.new(
		(column - (GRID_SIDE - 1) / 2) * SPACING,
		SPHERE_SIZE / 2,
		(row - (GRID_SIDE - 1) / 2) * SPACING
	)
end

for index = 1, SPHERE_COUNT do
	local sphere = Instance.new("MeshPart")
	sphere.Name = `InstancedPbrSphere_{index}`
	sphere.Anchored = true
	sphere.MeshId = "engine.Sphere"
	sphere.Size = Vector3.new(SPHERE_SIZE, SPHERE_SIZE, SPHERE_SIZE)
	sphere.Position = SpherePosition(index)
	sphere.Color = Color3.new(1, 1, 1)
	-- Every part names the same image, so only the sphere and material instance
	-- count changes while the renderer can share the resident texture.
	sphere.TextureID = texture.ContentId
	sphere.Parent = workspace
end

local floor = Instance.new("Part")
floor.Name = "InstancedPbrStressGround"
floor.Anchored = true
floor.Size = Vector3.new(78, 1, 78)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(18, 22, 30)
floor.Parent = workspace

local lamp = Instance.new("Part")
lamp.Name = "InstancedPbrStressLamp"
lamp.Anchored = true
lamp.Size = Vector3.new(0.5, 0.5, 0.5)
lamp.Position = Vector3.new(-22, 28, 18)
lamp.Color = Color3.fromRGB(235, 244, 255)
lamp.EmissiveTint = lamp.Color
lamp.EmissiveStrength = 6
lamp.Parent = workspace

local point = Instance.new("PointLight")
point.Color = lamp.Color
point.Brightness = 900
point.Range = 60
point.Parent = lamp

Lighting.Brightness = 0.8
Lighting.ClockTime = 14
Lighting.Ambient = Color3.fromRGB(24, 28, 38)
Lighting.OutdoorAmbient = Color3.fromRGB(42, 48, 62)

local camera = Instance.new("Camera")
camera.Name = "InstancedPbrStressCamera"
camera.FieldOfView = 48
camera.CFrame = CFrame.lookAt(Vector3.new(0, 38, 62), Vector3.new(0, 2, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

print(`pbr texture instance stress: one {TEXTURE_SIZE}x{TEXTURE_SIZE} image on {SPHERE_COUNT} spheres ready`)

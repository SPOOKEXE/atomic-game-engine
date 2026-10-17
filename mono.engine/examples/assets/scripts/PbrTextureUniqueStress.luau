--!strict
-- PBR texture residency stress test: one unique 4096-square image per sphere.
--
-- Each image is deliberately a separate EditableImage. The 100 images account
-- for about 6.25 GiB of RGBA8 pixel storage before renderer allocations, so
-- this scene is a memory and upload benchmark, not a normal sample scene.
--
--     client --texture-budget-mib 9216 --script PbrTextureUniqueStress.luau

local Lighting = game:GetService("Lighting")

local TEXTURE_SIZE = 4096
local SPHERE_COUNT = 100
local GRID_SIDE = 10
local SPACING = 7
local SPHERE_SIZE = 5

local function NewTexture(index: number): EditableImage
	local image = Instance.new("EditableImage")
	image.Name = `UniquePbr4K_{index}`
	assert(image:Resize(TEXTURE_SIZE, TEXTURE_SIZE), `{image.Name} resize was refused`)

	-- A solid field is enough to make each content id and resident allocation
	-- unique. DrawRectangle keeps the benchmark's startup work in the engine's
	-- image path instead of spending the run in a Luau pixel loop.
	local red = 32 + (index * 47) % 192
	local green = 32 + (index * 83) % 192
	local blue = 32 + (index * 131) % 192
	assert(
		image:DrawRectangle(
			Vector2.new(0, 0),
			Vector2.new(TEXTURE_SIZE, TEXTURE_SIZE),
			Color3.fromRGB(red, green, blue)
		),
		`{image.Name} paint was refused`
	)
	return image
end

local function SpherePosition(index: number): Vector3
	local column = (index - 1) % GRID_SIDE
	local row = math.floor((index - 1) / GRID_SIDE)
	return Vector3.new(
		(column - (GRID_SIDE - 1) / 2) * SPACING,
		SPHERE_SIZE / 2,
		(row - (GRID_SIDE - 1) / 2) * SPACING
	)
end

local images: { EditableImage } = {}
for index = 1, SPHERE_COUNT do
	local image = NewTexture(index)
	images[index] = image

	local sphere = Instance.new("MeshPart")
	sphere.Name = `UniquePbrSphere_{index}`
	sphere.Anchored = true
	sphere.MeshId = "engine.Sphere"
	sphere.Size = Vector3.new(SPHERE_SIZE, SPHERE_SIZE, SPHERE_SIZE)
	sphere.Position = SpherePosition(index)
	sphere.Color = Color3.new(1, 1, 1)
	-- TextureID leaves the part on the deferred PBR path, matching the main
	-- PBR demo while making the colour map the only per-sphere variable.
	sphere.TextureID = image.ContentId
	sphere.Parent = workspace
end

local floor = Instance.new("Part")
floor.Name = "UniquePbrStressGround"
floor.Anchored = true
floor.Size = Vector3.new(78, 1, 78)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(18, 22, 30)
floor.Parent = workspace

local lamp = Instance.new("Part")
lamp.Name = "UniquePbrStressLamp"
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
camera.Name = "UniquePbrStressCamera"
camera.FieldOfView = 48
camera.CFrame = CFrame.lookAt(Vector3.new(0, 38, 62), Vector3.new(0, 2, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

print(`pbr unique texture stress: {SPHERE_COUNT} {TEXTURE_SIZE}x{TEXTURE_SIZE} images ready`)

--!strict
-- Procedural PBR stone maps and geometric masonry relief in one lit view.
--
-- Every orb shares live EditableImage maps and the default deferred PBR route.
-- The mesh samples the same stone field as the maps, so mortar remains recessed
-- in the silhouette. The warm spot and cool point lights make the material
-- response readable from opposite sides of the row.
--
-- ## Run it
--
--     client --script PbrShaderMaterialDemo.luau

local Lighting = game:GetService("Lighting")

local IMAGE_SIZE = 256
-- Seven 256-square RGBA maps use 1.75 MiB. This keeps individual chipped faces
-- legible at the close demonstration camera while the relief mesh changes the silhouette.

type PbrMaps = {
	Colour: EditableImage,
	Normal: EditableImage,
	Roughness: EditableImage,
	Occlusion: EditableImage,
	Height: EditableImage,
	Metalness: EditableImage,
	Emissive: EditableImage,
}

local function NewImage(name: string): EditableImage
	local image = Instance.new("EditableImage")
	image.Name = name
	assert(image:Resize(IMAGE_SIZE, IMAGE_SIZE), `{name} size was refused`)
	return image
end

local COURSES = 14
local BLOCKS_PER_COURSE = 13

local function HashStone(column: number, course: number): number
	local value = math.sin(column * 127.1 + course * 311.7) * 43758.5453
	return value - math.floor(value)
end

local function GrainAt(x: number, y: number): number
	local value = math.sin(x * 12.9898 + y * 78.233) * 43758.5453
	return value - math.floor(value)
end

local function StoneHeight(u: number, v: number): number
	local course = math.min(math.floor(v * COURSES), COURSES - 1)
	local shifted = u * BLOCKS_PER_COURSE + (course % 2) * 0.5
	local column = math.floor(shifted) % BLOCKS_PER_COURSE
	local faceU = shifted % 1
	local faceV = (v * COURSES) % 1
	local seed = HashStone(column, course)
	local sideChip = 0.040 + math.abs(math.sin(faceV * math.pi * (1.5 + seed * 3.0))) * 0.014
	local courseChip = 0.052 + math.abs(math.sin(faceU * math.pi * (1.0 + seed * 3.0))) * 0.016
	if faceU < sideChip or faceU > 1.0 - sideChip or faceV < courseChip or faceV > 1.0 - courseChip then
		return 0.06
	end
	local broad = math.sin(faceU * math.pi * (1.2 + seed * 1.5) + faceV * math.pi * (1.5 - seed))
	local chip = math.sin(faceU * math.pi * (5.0 + seed * 5.0) + faceV * math.pi * (4.0 + seed * 4.0))
	local fracture = math.sin(faceU * math.pi * 13.0 - faceV * math.pi * 11.0 + seed * 27.0)
	return math.clamp(0.50 + broad * 0.10 + chip * 0.060 + fracture * 0.025 + (seed - 0.5) * 0.12, 0, 1)
end

local function PaintMaps(): PbrMaps
	local maps: PbrMaps = {
		Colour = NewImage("PbrDemo_Colour"),
		Normal = NewImage("PbrDemo_Normal"),
		Roughness = NewImage("PbrDemo_Roughness"),
		Occlusion = NewImage("PbrDemo_Occlusion"),
		Height = NewImage("PbrDemo_Height"),
		Metalness = NewImage("PbrDemo_Metalness"),
		Emissive = NewImage("PbrDemo_Emissive"),
	}
	local images: { [string]: EditableImage } = {
		Colour = maps.Colour,
		Normal = maps.Normal,
		Roughness = maps.Roughness,
		Occlusion = maps.Occlusion,
		Height = maps.Height,
		Metalness = maps.Metalness,
		Emissive = maps.Emissive,
	}

	local pixels: { [string]: buffer } = {}
	for name, image in images do
		pixels[name] = buffer.create(IMAGE_SIZE * IMAGE_SIZE * 4)
	end
	local function put(target: buffer, offset: number, red: number, green: number, blue: number)
		buffer.writeu8(target, offset, math.floor(math.clamp(red, 0, 255)))
		buffer.writeu8(target, offset + 1, math.floor(math.clamp(green, 0, 255)))
		buffer.writeu8(target, offset + 2, math.floor(math.clamp(blue, 0, 255)))
		buffer.writeu8(target, offset + 3, 255)
	end
	for y = 0, IMAGE_SIZE - 1 do
		for x = 0, IMAGE_SIZE - 1 do
			local u = x / (IMAGE_SIZE - 1)
			local v = y / (IMAGE_SIZE - 1)
			local rock = StoneHeight(u, v)
			local grain = GrainAt(x, y) - 0.5
			local left = StoneHeight((u - 1 / IMAGE_SIZE + 1) % 1, v)
			local up = StoneHeight(u, math.max(v - 1 / IMAGE_SIZE, 0))
			local offset = (y * IMAGE_SIZE + x) * 4
			put(pixels.Colour, offset, 16 + rock * 62 + grain * 24, 15 + rock * 58 + grain * 20, 13 + rock * 52 + grain * 16)
			put(pixels.Normal, offset, 128 + (rock - left) * 300 + grain * 20, 128 + (rock - up) * 300 + grain * 20, 255)
			local roughness = 135 + (1 - rock) * 95 + grain * 32
			put(pixels.Roughness, offset, roughness, roughness, roughness)
			local occlusion = 125 + rock * 130
			put(pixels.Occlusion, offset, occlusion, occlusion, occlusion)
			-- Relief is geometric. Keep the height channel neutral so its screen-space
			-- parallax branch cannot tear across this curved, tiled demonstration mesh.
			put(pixels.Height, offset, 0, 0, 0)
			put(pixels.Metalness, offset, 0, 0, 0)
			put(pixels.Emissive, offset, 0, 0, 0)
		end
	end
	for name, image in images do
		assert(image:FromBuffer(pixels[name]), `{name} buffer was refused`)
	end
	return maps
end

local maps = PaintMaps()

local function ApplyMaps(part: MeshPart)
	part.TextureID = maps.Colour.ContentId
	part.NormalMap = maps.Normal.ContentId
	part.RoughnessMap = maps.Roughness.ContentId
	part.OcclusionMap = maps.Occlusion.ContentId
	part.HeightMap = maps.Height.ContentId
	part.MetalnessMap = maps.Metalness.ContentId
	part.EmissiveMap = maps.Emissive.ContentId
	part.EmissiveTint = Color3.new(1, 1, 1)
	part.EmissiveStrength = 2.2
end

local function ConfigureLod(part: MeshPart)
	-- These ratios describe retained decimated geometry only. Studio's distance
	-- preferences choose when a resident coarse page becomes eligible.
	part.AutoLodStrategy = Enum.AutoMeshLODStrategy.Decimated
	part.AutoLodLevels = 4
	part.AutoLod1Ratio = 0.70
	part.AutoLod2Ratio = 0.40
	part.AutoLod3Ratio = 0.10
	part.AutoLodTargetQuadArea = 4
end

local function RockMesh(name: string, position: Vector3): MeshPart
	local mesh = Instance.new("EditableMesh")
	mesh.Name = `{name}_ReliefMesh`
	local vertices = {}
	local indices = {}
	local rings = 84
	local segments = 112
	for ring = 0, rings do
		local v = ring / rings
		local theta = v * math.pi
		for segment = 0, segments do
			local u = segment / segments
			local phi = u * math.pi * 2
			local direction = Vector3.new(math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
			local radius = 2.75 + (StoneHeight(u, v) - 0.5) * 0.24
			table.insert(vertices, { Position = direction * radius, Normal = direction, UV = Vector2.new(u, v) })
		end
	end
	for ring = 0, rings - 1 do
		for segment = 0, segments - 1 do
			local a = ring * (segments + 1) + segment
			local b = a + 1
			local c = a + segments + 1
			table.insert(indices, a)
			table.insert(indices, b)
			table.insert(indices, c)
			table.insert(indices, b)
			table.insert(indices, c + 1)
			table.insert(indices, c)
		end
	end
	assert(mesh:SetGeometry(vertices, indices), `{name} relief mesh was refused`)
	local part = Instance.new("MeshPart")
	part.Name = name
	part.Anchored = true
	part.MeshId = mesh.ContentId
	part.Size = Vector3.new(5.8, 5.8, 5.8)
	part.Position = position
	part.Color = Color3.new(1, 1, 1)
	ApplyMaps(part)
	ConfigureLod(part)
	part.Parent = workspace
	return part
end

-- Default PBR is left without a Material child. It therefore proves the
-- deferred route directly from MeshPart map properties.
RockMesh("DefaultPbr", Vector3.new(-3, 3.0, 0))

RockMesh("CoolStonePbr", Vector3.new(-9, 3.0, 0))

RockMesh("FillStonePbr", Vector3.new(9, 3.0, 0))

RockMesh("WarmStonePbr", Vector3.new(3, 3.0, 0))

local ground = Instance.new("Part")
ground.Name = "PbrDemoGround"
ground.Anchored = true
ground.Size = Vector3.new(24, 0.5, 20)
ground.Position = Vector3.new(0, -0.25, -2)
ground.Color = Color3.fromRGB(18, 23, 31)
ground.Parent = workspace

local coolLamp = Instance.new("Part")
coolLamp.Name = "CoolPointLamp"
coolLamp.Anchored = true
coolLamp.Size = Vector3.new(0.4, 0.4, 0.4)
coolLamp.Position = Vector3.new(-4, 7, 5)
coolLamp.Color = Color3.fromRGB(220, 230, 255)
coolLamp.EmissiveTint = coolLamp.Color
coolLamp.EmissiveStrength = 6
coolLamp.Parent = workspace

local point = Instance.new("PointLight")
point.Color = coolLamp.Color
point.Brightness = 260
point.Range = 16
point.Parent = coolLamp

local warmLamp = Instance.new("Part")
warmLamp.Name = "WarmSpotLamp"
warmLamp.Anchored = true
warmLamp.Size = Vector3.new(0.4, 0.4, 0.4)
warmLamp.Position = Vector3.new(5, 4, 4)
warmLamp.Color = Color3.fromRGB(255, 166, 82)
warmLamp.EmissiveTint = warmLamp.Color
warmLamp.EmissiveStrength = 6
warmLamp.Parent = workspace

local spot = Instance.new("SpotLight")
spot.Color = warmLamp.Color
spot.Brightness = 100
spot.Range = 18
spot.Angle = 70
spot.Face = Enum.NormalId.Front
spot.Parent = warmLamp

local fillLamp = Instance.new("Part")
fillLamp.Name = "StoneFillLamp"
fillLamp.Anchored = true
fillLamp.Size = Vector3.new(0.3, 0.3, 0.3)
fillLamp.Position = Vector3.new(10, 5, 5)
fillLamp.Color = Color3.fromRGB(180, 195, 220)
fillLamp.Parent = workspace

local fill = Instance.new("PointLight")
fill.Color = fillLamp.Color
fill.Brightness = 110
fill.Range = 14
fill.Parent = fillLamp

Lighting.Brightness = 0.95
Lighting.ClockTime = 15
Lighting.Ambient = Color3.fromRGB(28, 30, 34)
Lighting.OutdoorAmbient = Color3.fromRGB(38, 42, 48)
Lighting.FogColor = Color3.fromRGB(2, 4, 9)
Lighting.FogStart = 80
Lighting.FogEnd = 120

local camera = Instance.new("Camera")
camera.Name = "PbrShaderMaterialCamera"
camera.FieldOfView = 40
camera.CFrame = CFrame.lookAt(Vector3.new(-5, 4.8, 10), Vector3.new(-5, 3.0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

print("pbr shader material demo: procedural stone relief and local lights ready")

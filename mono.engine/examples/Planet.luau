--!strict
-- A deterministic quadsphere planet split into camera-selected quadtree patches.
--
-- **What this proves.** The six cube faces use the spherified-cube mapping rather
-- than latitude-longitude poles, every selected quadtree leaf owns one
-- `EditableMesh`, and changing the active camera changes only the derived leaf
-- set. The geometry is generated through the same transactional `SetGeometry`
-- path as authored meshes. Nothing stores or replicates the generated chunks.
--
-- The surface follows the layered simple and ridged filters from Sebastian
-- Lague's procedural planet series. The series deliberately stops short of LOD;
-- this demo continues its cube-face split into a bounded quadtree and gives each
-- leaf a radial skirt so adjacent levels cannot expose the background.
--
-- ## Run it
--
--     client --script Planet.luau

local RunService = game:GetService("RunService")
local Lighting = game:GetService("Lighting")

local PLANET_RADIUS = 70
local PATCH_RESOLUTION = 17
local MAXIMUM_DEPTH = 3
local SKIRT_DEPTH = 1.5
local SEED = 27183
local SELECTION_INTERVAL_TICKS = 45
local ORBIT_RADIUS = 165
local ORBIT_HEIGHT = 34

local BIOME_COUNT = 4
local LUT_WIDTH = 128
local LAND_START = LUT_WIDTH / 2
local MAXIMUM_ELEVATION = 0.22
local MAXIMUM_OCEAN_DEPTH = 0.1

type NoiseLayer = {
	Kind: "Simple" | "Ridged",
	Enabled: boolean,
	Strength: number,
	BaseRoughness: number,
	Roughness: number,
	Persistence: number,
	Centre: Vector3,
	MinValue: number,
	Octaves: number,
	WeightMultiplier: number,
	UseFirstLayerAsMask: boolean,
}

type Face = {
	Name: string,
	LocalUp: Vector3,
	AxisA: Vector3,
	AxisB: Vector3,
}

type Node = {
	Face: Face,
	Depth: number,
	X: number,
	Y: number,
	U0: number,
	V0: number,
	Size: number,
	Key: string,
	Priority: number,
}

type MeshVertex = {
	Position: Vector3,
	Normal: Vector3?,
	UV: Vector2?,
	Color: Color3?,
	Alpha: number?,
}

type RawVertex = {
	Position: Vector3,
	Normal: Vector3,
	UV: Vector2,
}

type Chunk = {
	Part: MeshPart,
	Mesh: EditableMesh,
}

local function Clamp01(value: number): number
	return math.clamp(value, 0, 1)
end

local function LerpColour(from: Color3, to: Color3, alpha: number): Color3
	local amount = Clamp01(alpha)
	return Color3.new(
		from.R + (to.R - from.R) * amount,
		from.G + (to.G - from.G) * amount,
		from.B + (to.B - from.B) * amount
	)
end

-- The fixed hash table behind `math.noise` repeats every 256 units. These
-- offsets stay inside one period and seed the layers without changing the
-- kernel scripts already rely on.
local function SeedOffset(salt: number): Vector3
	local function coordinate(axis: number): number
		local mixed = (SEED * (salt * 37 + axis * 101) + salt * 7919 + axis * 104729) % 251
		return mixed - 125
	end

	return Vector3.new(coordinate(1), coordinate(2), coordinate(3))
end

local noiseLayers: { NoiseLayer } = {
	{
		Kind = "Simple",
		Enabled = true,
		Strength = 0.19,
		BaseRoughness = 0.9,
		Roughness = 2.05,
		Persistence = 0.5,
		Centre = SeedOffset(1),
		MinValue = 0.48,
		Octaves = 5,
		WeightMultiplier = 0,
		UseFirstLayerAsMask = false,
	},
	{
		Kind = "Ridged",
		Enabled = true,
		Strength = 0.13,
		BaseRoughness = 2.1,
		Roughness = 2.35,
		Persistence = 0.52,
		Centre = SeedOffset(2),
		MinValue = 0.34,
		Octaves = 4,
		WeightMultiplier = 0.82,
		UseFirstLayerAsMask = true,
	},
}

local function EvaluateLayer(layer: NoiseLayer, point: Vector3): (number, number)
	local frequency = layer.BaseRoughness
	local amplitude = 1
	local accumulated = 0
	local weight = 1

	for _ = 1, layer.Octaves do
		local sample = point * frequency + layer.Centre
		local value = math.noise(sample.X, sample.Y, sample.Z)
		if layer.Kind == "Ridged" then
			value = 1 - math.abs(value)
			value = value * value * weight
			weight = Clamp01(value * layer.WeightMultiplier)
		else
			value = (value + 1) * 0.5
		end

		accumulated += value * amplitude
		frequency *= layer.Roughness
		amplitude *= layer.Persistence
	end

	local signed = (accumulated - layer.MinValue) * layer.Strength
	return math.max(0, signed), signed
end

local function ElevationAt(direction: Vector3): (number, number)
	local first = 0
	local signedFirst = 0
	if noiseLayers[1].Enabled then
		first, signedFirst = EvaluateLayer(noiseLayers[1], direction)
	end
	local elevation = first

	for index = 2, #noiseLayers do
		local layer = noiseLayers[index]
		if layer.Enabled then
			local mask = if layer.UseFirstLayerAsMask then first else 1
			local layerElevation = EvaluateLayer(layer, direction)
			elevation += layerElevation * mask
		end
	end

	return elevation, signedFirst
end

-- Equal-area cube inflation. Plain `cube.Unit` keeps too many samples near the
-- eight cube corners, which is precisely where a quadtree patch should not lose
-- effective resolution.
local function Spherify(cube: Vector3): Vector3
	local xx = cube.X * cube.X
	local yy = cube.Y * cube.Y
	local zz = cube.Z * cube.Z
	local x = cube.X * math.sqrt(math.max(0, 1 - yy * 0.5 - zz * 0.5 + yy * zz / 3))
	local y = cube.Y * math.sqrt(math.max(0, 1 - zz * 0.5 - xx * 0.5 + zz * xx / 3))
	local z = cube.Z * math.sqrt(math.max(0, 1 - xx * 0.5 - yy * 0.5 + xx * yy / 3))
	return Vector3.new(x, y, z).Unit
end

local function MakeFace(name: string, localUp: Vector3): Face
	local axisA = Vector3.new(localUp.Y, localUp.Z, localUp.X)
	return {
		Name = name,
		LocalUp = localUp,
		AxisA = axisA,
		AxisB = localUp:Cross(axisA),
	}
end

local faces: { Face } = {
	MakeFace("PX", Vector3.new(1, 0, 0)),
	MakeFace("NX", Vector3.new(-1, 0, 0)),
	MakeFace("PY", Vector3.new(0, 1, 0)),
	MakeFace("NY", Vector3.new(0, -1, 0)),
	MakeFace("PZ", Vector3.new(0, 0, 1)),
	MakeFace("NZ", Vector3.new(0, 0, -1)),
}

local function DirectionAt(face: Face, u: number, v: number): Vector3
	return Spherify(face.LocalUp + face.AxisA * u + face.AxisB * v)
end

local function SurfaceAt(face: Face, u: number, v: number): (Vector3, Vector3, number, number)
	local direction = DirectionAt(face, u, v)
	local elevation, signedElevation = ElevationAt(direction)
	return direction * PLANET_RADIUS * (1 + elevation), direction, elevation, signedElevation
end

local function BiomeAt(direction: Vector3): number
	local boundaryNoise = math.noise(
		direction.X * 2.6 + 31,
		direction.Y * 2.6 - 17,
		direction.Z * 2.6 + 73
	)
	local latitude = Clamp01(math.abs(direction.Y) + boundaryNoise * 0.075)
	return latitude * (BIOME_COUNT - 1)
end

-- One texture packs ocean into its first half and land into its second. Rows
-- are biomes, so the fragment shader gets both dimensions from the UV channel
-- the built-in vertex stage already forwards.
local colourLut = Instance.new("EditableImage")
colourLut.Name = "PlanetColourLUT"
assert(colourLut:Resize(LUT_WIDTH, BIOME_COUNT), "planet LUT size was refused")

local oceanDeep = Color3.fromRGB(8, 25, 58)
local oceanShallow = Color3.fromRGB(24, 116, 156)
local landLow: { Color3 } = {
	Color3.fromRGB(191, 151, 83),
	Color3.fromRGB(64, 116, 65),
	Color3.fromRGB(54, 88, 64),
	Color3.fromRGB(145, 151, 150),
}
local landHigh: { Color3 } = {
	Color3.fromRGB(77, 132, 61),
	Color3.fromRGB(118, 139, 82),
	Color3.fromRGB(122, 117, 104),
	Color3.fromRGB(240, 244, 247),
}

for row = 0, BIOME_COUNT - 1 do
	for x = 0, LUT_WIDTH - 1 do
		local colour: Color3
		if x < LAND_START then
			colour = LerpColour(oceanDeep, oceanShallow, x / (LAND_START - 1))
		else
			colour = LerpColour(
				landLow[row + 1],
				landHigh[row + 1],
				(x - LAND_START) / (LUT_WIDTH - LAND_START - 1)
			)
		end
		colourLut:DrawRectangle(Vector2.new(x, row), Vector2.new(1, 1), colour)
	end
end

local surfaceShader = Instance.new("ShaderScript")
surfaceShader.Name = "PlanetSurface"
surfaceShader.Source = [[
#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 4) in vec2 inTexCoord;
layout(location = 7) flat in vec3 inSurfaceColour;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;
layout(set = 2, binding = 3) uniform sampler2D beamMap;
layout(set = 2, binding = 4) uniform sampler2D normalMap;
layout(set = 2, binding = 5) uniform sampler2D roughnessMap;
layout(set = 2, binding = 6) uniform sampler2D occlusionMap;
layout(set = 2, binding = 7) uniform sampler2D emissiveMap;
layout(set = 2, binding = 8) uniform sampler2D heightMap;
layout(set = 2, binding = 9) uniform sampler2D metalnessMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Direct;
	vec4 Flags;
} lighting;

layout(set = 3, binding = 1) uniform Lights {
	vec4 LightRows[49];
} lights;

layout(set = 3, binding = 2) uniform Beams {
	vec4 BeamRows[41];
} beams;

const float BIOME_ROWS = 4.0;

void main() {
	float land = step(0.5005, inTexCoord.y);
	float ramp = inTexCoord.y;
	float biome = (clamp(inTexCoord.x, 0.0, BIOME_ROWS - 1.0) + 0.5) / BIOME_ROWS;
	vec3 albedo = texture(colourMap, vec2(clamp(ramp, 0.0, 1.0), biome)).rgb;
	vec3 normal = normalize(inNormal);
	vec3 toLight = -normalize(lighting.Direction.xyz);
	float lambert = max(dot(normal, toLight), 0.0);
	float oceanGlint = (1.0 - land) * pow(lambert, 18.0) * 0.45;
	vec3 lit = albedo * inColour.rgb * inSurfaceColour;
	lit *= lighting.Ambient.rgb + lighting.Direct.rgb * lambert;
	lit += vec3(oceanGlint);
	outColour = vec4(lit, max(inColour.a, lighting.Flags.w));
}
]]
surfaceShader.Parent = workspace

local function NodeKey(face: Face, depth: number, x: number, y: number): string
	return `{face.Name}_D{depth}_X{x}_Y{y}`
end

local function RootNode(face: Face): Node
	return {
		Face = face,
		Depth = 0,
		X = 0,
		Y = 0,
		U0 = -1,
		V0 = -1,
		Size = 2,
		Key = NodeKey(face, 0, 0, 0),
		Priority = 0,
	}
end

local function ChildNode(parent: Node, dx: number, dy: number): Node
	local size = parent.Size * 0.5
	local depth = parent.Depth + 1
	local x = parent.X * 2 + dx
	local y = parent.Y * 2 + dy
	return {
		Face = parent.Face,
		Depth = depth,
		X = x,
		Y = y,
		U0 = parent.U0 + dx * size,
		V0 = parent.V0 + dy * size,
		Size = size,
		Key = NodeKey(parent.Face, depth, x, y),
		Priority = 0,
	}
end

local function ChunkGeometry(node: Node): ({ MeshVertex }, { number }, Vector3, Vector3)
	local rawVertices: { RawVertex } = {}
	local indices: { number } = {}
	local lowest = Vector3.new(math.huge, math.huge, math.huge)
	local highest = Vector3.new(-math.huge, -math.huge, -math.huge)

	local function include(position: Vector3)
		lowest = Vector3.new(
			math.min(lowest.X, position.X),
			math.min(lowest.Y, position.Y),
			math.min(lowest.Z, position.Z)
		)
		highest = Vector3.new(
			math.max(highest.X, position.X),
			math.max(highest.Y, position.Y),
			math.max(highest.Z, position.Z)
		)
	end

	local function addVertex(position: Vector3, normal: Vector3, uv: Vector2): number
		table.insert(rawVertices, { Position = position, Normal = normal, UV = uv })
		include(position)
		return #rawVertices - 1
	end

	for gy = 0, PATCH_RESOLUTION - 1 do
		local acrossY = gy / (PATCH_RESOLUTION - 1)
		for gx = 0, PATCH_RESOLUTION - 1 do
			local acrossX = gx / (PATCH_RESOLUTION - 1)
			local u = node.U0 + node.Size * acrossX
			local v = node.V0 + node.Size * acrossY
			local position, direction, elevation, signedElevation = SurfaceAt(node.Face, u, v)
			local heightCoordinate = if signedElevation <= 0
				then 0.5 * Clamp01(1 + signedElevation / MAXIMUM_OCEAN_DEPTH)
				else 0.5 + 0.5 * Clamp01(elevation / MAXIMUM_ELEVATION)
			addVertex(
				position,
				direction,
				Vector2.new(BiomeAt(direction), heightCoordinate)
			)
		end
	end

	-- `axisA x axisB` points along `localUp`, so `(a,b,c)` is
	-- counter-clockwise from outside on all six faces.
	for gy = 0, PATCH_RESOLUTION - 2 do
		for gx = 0, PATCH_RESOLUTION - 2 do
			local a = gx + gy * PATCH_RESOLUTION
			local b = a + 1
			local c = a + PATCH_RESOLUTION
			local d = c + 1
			table.insert(indices, a)
			table.insert(indices, b)
			table.insert(indices, c)
			table.insert(indices, b)
			table.insert(indices, d)
			table.insert(indices, c)
		end
	end

	-- One counter-clockwise perimeter, without duplicated corners. Its inset
	-- copy makes a small radial wall beneath the patch and hides T-junctions
	-- where a fine leaf meets a coarse one.
	local perimeter: { number } = {}
	for x = 0, PATCH_RESOLUTION - 1 do
		table.insert(perimeter, x)
	end
	for y = 1, PATCH_RESOLUTION - 1 do
		table.insert(perimeter, y * PATCH_RESOLUTION + PATCH_RESOLUTION - 1)
	end
	for x = PATCH_RESOLUTION - 2, 0, -1 do
		table.insert(perimeter, (PATCH_RESOLUTION - 1) * PATCH_RESOLUTION + x)
	end
	for y = PATCH_RESOLUTION - 2, 1, -1 do
		table.insert(perimeter, y * PATCH_RESOLUTION)
	end

	local skirt: { number } = {}
	for _, outerId in perimeter do
		local outer = rawVertices[outerId + 1]
		table.insert(
			skirt,
			addVertex(outer.Position - outer.Normal * SKIRT_DEPTH, outer.Normal, outer.UV)
		)
	end

	for index = 1, #perimeter do
		local following = index % #perimeter + 1
		local outer0 = perimeter[index]
		local outer1 = perimeter[following]
		local inner0 = skirt[index]
		local inner1 = skirt[following]
		table.insert(indices, outer0)
		table.insert(indices, inner0)
		table.insert(indices, outer1)
		table.insert(indices, outer1)
		table.insert(indices, inner0)
		table.insert(indices, inner1)
	end

	local centre = (lowest + highest) * 0.5
	local size = highest - lowest
	local vertices: { MeshVertex } = {}
	for _, vertex in rawVertices do
		table.insert(vertices, {
			Position = vertex.Position - centre,
			Normal = vertex.Normal,
			UV = vertex.UV,
		})
	end

	return vertices, indices, centre, size
end


local live: { [string]: Chunk } = {}

local function BuildChunk(node: Node)
	if live[node.Key] ~= nil then
		return
	end

	local vertices, indices, centre, size = ChunkGeometry(node)
	local mesh = Instance.new("EditableMesh")
	mesh.Name = `PlanetMesh_{node.Key}`
	assert(mesh:SetGeometry(vertices, indices), `planet mesh {node.Key} was cancelled`)

	local part = Instance.new("MeshPart")
	part.Name = `PlanetChunk_{node.Key}`
	part.Anchored = true
	part.CanCollide = false
	part.Color = Color3.new(1, 1, 1)
	part.Position = centre
	part.Size = size
	part.MeshId = mesh.ContentId
	part.TextureID = colourLut.ContentId
	part.Parent = workspace

	local material = Instance.new("Material")
	material.Name = "PlanetMaterial"
	material.Shader = surfaceShader.Name
	material.Parent = part

	live[node.Key] = { Part = part, Mesh = mesh }
end

local function DestroyChunk(key: string)
	local chunk = live[key]
	if chunk == nil then
		return
	end

	chunk.Part:Destroy()
	chunk.Mesh:Destroy()
	live[key] = nil
end

local function ShouldSplit(node: Node, cameraPosition: Vector3): (boolean, number)
	local centreDirection = DirectionAt(
		node.Face,
		node.U0 + node.Size * 0.5,
		node.V0 + node.Size * 0.5
	)
	local cameraDirection = if cameraPosition.Magnitude > 0 then cameraPosition.Unit else Vector3.new(0, 0, 1)
	local facing = centreDirection:Dot(cameraDirection)
	local surface = centreDirection * PLANET_RADIUS * (1 + ElevationAt(centreDirection))
	local distance = (cameraPosition - surface).Magnitude
	local patchSpan = PLANET_RADIUS * node.Size
	local split = node.Depth < MAXIMUM_DEPTH and facing > -0.3 and distance < patchSpan * 3.2
	return split, distance
end

local function SelectLeaves(node: Node, cameraPosition: Vector3, leaves: { Node })
	local split, distance = ShouldSplit(node, cameraPosition)
	if not split then
		node.Priority = distance
		table.insert(leaves, node)
		return
	end

	for dy = 0, 1 do
		for dx = 0, 1 do
			SelectLeaves(ChildNode(node, dx, dy), cameraPosition, leaves)
		end
	end
end

local desired: { [string]: boolean } = {}
local pending: { Node } = {}

local function ActiveCameraPosition(): Vector3
	local current = workspace.CurrentCamera
	return if current ~= nil then (current :: Camera).CFrame.Position else Vector3.new(0, 0, ORBIT_RADIUS)
end

local function PlanSelection()
	if #pending > 0 then
		return
	end

	local leaves: { Node } = {}
	local cameraPosition = ActiveCameraPosition()
	for _, face in faces do
		SelectLeaves(RootNode(face), cameraPosition, leaves)
	end

	desired = {}
	pending = {}
	for _, node in leaves do
		desired[node.Key] = true
		if live[node.Key] == nil then
			table.insert(pending, node)
		end
	end

	-- Nearest leaves arrive first. The key breaks equal-distance ties so two
	-- worlds publish the same ticket order.
	table.sort(pending, function(left: Node, right: Node): boolean
		if left.Priority == right.Priority then
			return left.Key < right.Key
		end
		return left.Priority < right.Priority
	end)
end

local function RetireCoveredChunks()
	if #pending > 0 then
		return
	end

	for key in live do
		if desired[key] ~= true then
			DestroyChunk(key)
		end
	end
end

local eye = Instance.new("Camera")
eye.Name = "PlanetCamera"
eye.FieldOfView = 58
eye.FarPlaneZ = 700
eye.CFrame = CFrame.lookAt(
	Vector3.new(0, ORBIT_HEIGHT, ORBIT_RADIUS),
	Vector3.new(0, 0, 0)
)
eye.Parent = workspace
workspace.CurrentCamera = eye

Lighting.Brightness = 0.75
Lighting.ClockTime = 14.5
Lighting.Ambient = Color3.fromRGB(18, 23, 34)
Lighting.OutdoorAmbient = Color3.fromRGB(28, 34, 48)
Lighting.FogColor = Color3.fromRGB(4, 7, 16)
Lighting.FogStart = 360
Lighting.FogEnd = 650

-- Six coarse roots make the globe complete before refinement starts. Finer
-- leaves are built before their covering roots are retired, so a slow machine
-- sees temporary overlap rather than a hole through the planet. The camera is
-- installed first because each mesh transaction yields; an embedding Studio
-- may replace CurrentCamera while those commits are still arriving.
for _, face in faces do
	BuildChunk(RootNode(face))
end

local tick = 0
local reconciling = false

local function MoveDemoCamera()
	-- Tick time, not wall time: the camera chooses the leaf set, so a clock here
	-- would make two recorded runs generate different worlds.
	local angle = tick * 0.0025
	local position = Vector3.new(
		math.sin(angle) * ORBIT_RADIUS,
		ORBIT_HEIGHT + math.sin(angle * 0.7) * 16,
		math.cos(angle) * ORBIT_RADIUS
	)
	eye.CFrame = CFrame.lookAt(position, Vector3.new(0, 0, 0))
end

RunService.Heartbeat:Connect(function()
	tick += 1
	MoveDemoCamera()

	-- `SetGeometry` yields to the deterministic script barrier. Heartbeat
	-- callbacks are separate coroutines, so this latch keeps one producer while
	-- the current patch is committing.
	if reconciling then
		return
	end
	reconciling = true

	if #pending == 0 and (tick == 1 or tick % SELECTION_INTERVAL_TICKS == 0) then
		PlanSelection()
	end

	if #pending > 0 then
		local nextNode = table.remove(pending, 1) :: Node
		BuildChunk(nextNode)
	end

	if #pending == 0 then
		RetireCoveredChunks()
	end

	reconciling = false
end)

print(
	`planet: seed {SEED}, radius {PLANET_RADIUS}, `
		.. `{PATCH_RESOLUTION}x{PATCH_RESOLUTION} quadtree patches through depth {MAXIMUM_DEPTH}`
)

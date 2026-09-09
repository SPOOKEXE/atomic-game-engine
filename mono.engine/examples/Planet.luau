--!strict
-- A deterministic system of procedural planets: one terran quadsphere, one
-- cratered airless moon, and one banded gas giant with rings.
--
-- **What this proves.** The six cube faces use the spherified-cube mapping rather
-- than latitude-longitude poles, every selected quadtree leaf owns one
-- `EditableMesh`, and changing the active camera changes only the derived leaf
-- set. The geometry is generated through the same transactional `SetGeometry`
-- path as authored meshes. Nothing stores or replicates the generated chunks.
-- Three bodies share one selector, one LUT layout and one shader contract, so a
-- seeded variant generator populates a system in one call rather than one planet
-- in three.
--
-- The surface follows the layered simple, ridged and crater filters from
-- Sebastian Lague's procedural planet series. The series deliberately stops
-- short of LOD; this demo continues its cube-face split into a bounded quadtree
-- per body and gives each leaf a radial skirt so adjacent levels cannot expose
-- the background.
--
-- **What this does not prove.** The skirts plus snapped coarse edges below hide
-- T-junctions; they are not the 16-pattern stitched transition set the plan
-- requires before calling any surface crack-free. The geometric error is a span
-- heuristic, not the bounded estimator. The single `AtmosphereProcedural` is
-- driven from the terran body; the giant and the moon share the sky it makes.
--
-- ## Run it
--
--     client --script Planet.luau
--
-- Authoring is the RECIPE block below plus the deterministic tour at the bottom:
-- edit constants and re-run for a new system, or watch tick 3600 reseed Haven
-- through the same drop-and-rebuild path an authoring edit would take.

local RunService = game:GetService("RunService")
local Lighting = game:GetService("Lighting")

type FaceRenderMask = "All" | "Top" | "Bottom" | "Left" | "Right" | "Front" | "Back"

-- --- authoring recipe -------------------------------------------------------
--
-- One table per body. A variant generator randomises these rows under
-- plausibility constraints; the demo fixes three rows so the run is recorded.
local FACE_RENDER_MASK: FaceRenderMask = "All"
local SELECTION_INTERVAL_TICKS = 45
local ORBIT_RADIUS = 165
local ORBIT_HEIGHT = 34

-- Projected-error selection. The plan starts at split-above-2px; this demo uses
-- 12px because Luau samples every vertex and a 2px demand at 17x17 patches asks
-- for the maximum depth almost everywhere. The number is budget, not physics.
local SCREEN_HEIGHT_PX = 1080
local SPLIT_PIXELS = 12
local ERROR_FACTOR = 0.10
local MAX_LEAVES_PER_BODY = 900

-- Deterministic reseed showcase. At this tick Haven's seed steps once through
-- ClearBody plus PlanSelection, proving the authoring rebuild path. Set to -1
-- to disable.
local AUTO_RESEED_TICK = 3600
local RESEED_SEED = 55555

-- Chunk-boundary overlay. One SelectionBox per live leaf, coloured by depth, so
-- a screenshot shows the quadtree the camera asked for.
local OVERLAY_ENABLED = true

type NoiseLayer = {
	Kind: "Simple" | "Ridged" | "Crater",
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
	Salt: number,
}

type BodyRecipe = {
	Name: string,
	Radius: number,
	Centre: Vector3,
	PatchResolution: number,
	MaxDepth: number,
	Seed: number,
	Layers: { NoiseLayer },
	BiomeRows: number,
	LutWidth: number,
	TiltDegrees: number,
	GasGiant: boolean,
	MaxRelief: number,
	OceanDeep: Color3,
	OceanShallow: Color3,
	LandLow: { Color3 },
	LandHigh: { Color3 },
}

local Bodies: { BodyRecipe } = {
	{
		Name = "Haven",
		Radius = 70,
		Centre = Vector3.new(0, 0, 0),
		PatchResolution = 17,
		MaxDepth = 3,
		Seed = 27183,
		Layers = {
			{
				Kind = "Simple",
				Enabled = true,
				Strength = 0.19,
				BaseRoughness = 0.9,
				Roughness = 2.05,
				Persistence = 0.5,
				Centre = Vector3.new(0, 0, 0),
				MinValue = 0.48,
				Octaves = 5,
				WeightMultiplier = 0,
				UseFirstLayerAsMask = false,
				Salt = 1,
			},
			{
				Kind = "Ridged",
				Enabled = true,
				Strength = 0.13,
				BaseRoughness = 2.1,
				Roughness = 2.35,
				Persistence = 0.52,
				Centre = Vector3.new(0, 0, 0),
				MinValue = 0.34,
				Octaves = 4,
				WeightMultiplier = 0.82,
				UseFirstLayerAsMask = true,
				Salt = 2,
			},
			{
				Kind = "Crater",
				Enabled = true,
				Strength = 0.03,
				BaseRoughness = 3.4,
				Roughness = 2.2,
				Persistence = 0.5,
				Centre = Vector3.new(0, 0, 0),
				MinValue = 0.1,
				Octaves = 3,
				WeightMultiplier = 0.6,
				UseFirstLayerAsMask = true,
				Salt = 3,
			},
		},
		BiomeRows = 4,
		LutWidth = 128,
		TiltDegrees = 23.5,
		GasGiant = false,
		MaxRelief = 22,
		OceanDeep = Color3.fromRGB(8, 25, 58),
		OceanShallow = Color3.fromRGB(24, 116, 156),
		LandLow = {
			Color3.fromRGB(191, 151, 83),
			Color3.fromRGB(64, 116, 65),
			Color3.fromRGB(54, 88, 64),
			Color3.fromRGB(145, 151, 150),
		},
		LandHigh = {
			Color3.fromRGB(77, 132, 61),
			Color3.fromRGB(118, 139, 82),
			Color3.fromRGB(122, 117, 104),
			Color3.fromRGB(240, 244, 247),
		},
	},
	{
		Name = "Cinder",
		Radius = 14,
		Centre = Vector3.new(150, 34, 30),
		PatchResolution = 13,
		MaxDepth = 2,
		Seed = 90321,
		Layers = {
			{
				Kind = "Simple",
				Enabled = true,
				Strength = 0.16,
				BaseRoughness = 1.1,
				Roughness = 2.1,
				Persistence = 0.5,
				Centre = Vector3.new(0, 0, 0),
				MinValue = 0.42,
				Octaves = 4,
				WeightMultiplier = 0,
				UseFirstLayerAsMask = false,
				Salt = 1,
			},
			{
				Kind = "Crater",
				Enabled = true,
				Strength = 0.12,
				BaseRoughness = 2.6,
				Roughness = 2.3,
				Persistence = 0.55,
				Centre = Vector3.new(0, 0, 0),
				MinValue = 0.05,
				Octaves = 4,
				WeightMultiplier = 0.7,
				UseFirstLayerAsMask = false,
				Salt = 2,
			},
		},
		BiomeRows = 3,
		LutWidth = 96,
		TiltDegrees = 5,
		GasGiant = false,
		MaxRelief = 6,
		OceanDeep = Color3.fromRGB(28, 28, 32),
		OceanShallow = Color3.fromRGB(74, 74, 80),
		LandLow = {
			Color3.fromRGB(96, 96, 102),
			Color3.fromRGB(110, 110, 116),
			Color3.fromRGB(122, 122, 128),
		},
		LandHigh = {
			Color3.fromRGB(150, 150, 156),
			Color3.fromRGB(172, 172, 178),
			Color3.fromRGB(198, 198, 204),
		},
	},
	{
		Name = "Zephyr",
		Radius = 95,
		Centre = Vector3.new(-240, 60, -200),
		PatchResolution = 13,
		MaxDepth = 1,
		Seed = 41770,
		Layers = {},
		BiomeRows = 4,
		LutWidth = 128,
		TiltDegrees = 12,
		GasGiant = true,
		MaxRelief = 0,
		OceanDeep = Color3.fromRGB(196, 170, 140),
		OceanShallow = Color3.fromRGB(226, 208, 178),
		LandLow = {
			Color3.fromRGB(214, 188, 152),
			Color3.fromRGB(178, 128, 92),
			Color3.fromRGB(232, 220, 196),
			Color3.fromRGB(160, 110, 80),
		},
		LandHigh = {
			Color3.fromRGB(240, 228, 200),
			Color3.fromRGB(220, 170, 120),
			Color3.fromRGB(246, 240, 224),
			Color3.fromRGB(200, 140, 100),
		},
	},
}

type Face = {
	Name: string,
	LocalUp: Vector3,
	AxisA: Vector3,
	AxisB: Vector3,
}

type Node = {
	Body: number,
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
	Node: Node,
	BasePosition: Vector3,
	Overlay: SelectionBox?,
}

local function Clamp01(value: number): number
	return math.clamp(value, 0, 1)
end

local function SmoothStep(edge0: number, edge1: number, x: number): number
	local t = Clamp01((x - edge0) / (edge1 - edge0))
	return t * t * (3 - 2 * t)
end

local function LerpColour(from: Color3, to: Color3, alpha: number): Color3
	local amount = Clamp01(alpha)
	return Color3.new(
		from.R + (to.R - from.R) * amount,
		from.G + (to.G - from.G) * amount,
		from.B + (to.B - from.B) * amount
	)
end

-- Deterministic value in [0, 1) from an index and a salt. Not `math.random`: a
-- shared generator makes the scene depend on creation order, and this has to be
-- the same system every run.
local function Hash01(index: number, salt: number): number
	local mixed = (index * 0x9E3779B1 + salt * 0x85EBCA77) % 4294967296
	mixed = bit32.bxor(mixed, bit32.rshift(mixed, 15))
	mixed = (mixed * 0x2545F491) % 4294967296
	mixed = bit32.bxor(mixed, bit32.rshift(mixed, 13))
	return (mixed % 100000) / 100000
end

-- The fixed hash table behind `math.noise` repeats every 256 units. These
-- offsets stay inside one period and seed the layers without changing the
-- kernel scripts already rely on.
local function SeedOffset(seed: number, salt: number): Vector3
	local function coordinate(axis: number): number
		local mixed = (seed * (salt * 37 + axis * 101) + salt * 7919 + axis * 104729) % 251
		return mixed - 125
	end

	return Vector3.new(coordinate(1), coordinate(2), coordinate(3))
end

local function InitBodySeeds()
	for _, body in Bodies do
		for _, layer in body.Layers do
			layer.Centre = SeedOffset(body.Seed, layer.Salt)
		end
	end
end

InitBodySeeds()

local function TiltAxis(degrees: number): Vector3
	local radians = math.rad(degrees)
	return Vector3.new(math.sin(radians), math.cos(radians), 0).Unit
end

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
		elseif layer.Kind == "Crater" then
			-- An analytic bowl-plus-rim around the |noise| ridges: a depression
			-- inside radius 0.45 with a raised rim at it. Impact physics this is
			-- not; a cheap repeatable crater field it is.
			local distance = math.abs(value)
			local bowl = -0.55 * (1 - SmoothStep(0, 0.45, distance))
			local rimOffset = (distance - 0.45) * (distance - 0.45)
			local rim = 0.6 * math.exp(-rimOffset / 0.004)
			value = (bowl + rim) * weight
			weight = Clamp01(0.35 + value * layer.WeightMultiplier)
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

local function ElevationAt(body: BodyRecipe, direction: Vector3): (number, number)
	if body.GasGiant then
		return 0, 0
	end
	local first = 0
	local signedFirst = 0
	if #body.Layers >= 1 and body.Layers[1].Enabled then
		first, signedFirst = EvaluateLayer(body.Layers[1], direction)
	end
	local elevation = first

	for index = 2, #body.Layers do
		local layer = body.Layers[index]
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

-- A face mask turns high-resolution authoring into a focused inspection. The
-- skipped faces never enter selection, so this reduces generated work instead
-- of hiding five already-built meshes.
local function FaceEnabled(face: Face): boolean
	if FACE_RENDER_MASK == "All" then
		return true
	end
	return (FACE_RENDER_MASK == "Top" and face.Name == "PY")
		or (FACE_RENDER_MASK == "Bottom" and face.Name == "NY")
		or (FACE_RENDER_MASK == "Left" and face.Name == "NX")
		or (FACE_RENDER_MASK == "Right" and face.Name == "PX")
		or (FACE_RENDER_MASK == "Front" and face.Name == "PZ")
		or (FACE_RENDER_MASK == "Back" and face.Name == "NZ")
end

local function FaceOfDirection(direction: Vector3): Face
	local best = faces[1]
	local bestDot = direction:Dot(best.LocalUp)
	for index = 2, #faces do
		local dot = direction:Dot(faces[index].LocalUp)
		if dot > bestDot then
			bestDot = dot
			best = faces[index]
		end
	end
	return best
end

local function UvOnFace(face: Face, direction: Vector3): (number, number)
	local denom = direction:Dot(face.LocalUp)
	if math.abs(denom) < 1e-6 then
		return 0, 0
	end
	local cube = direction * (1 / denom)
	return cube:Dot(face.AxisA), cube:Dot(face.AxisB)
end

local function DirectionAt(face: Face, u: number, v: number): Vector3
	return Spherify(face.LocalUp + face.AxisA * u + face.AxisB * v)
end

local function SurfaceAt(body: BodyRecipe, face: Face, u: number, v: number): (Vector3, Vector3, number, number)
	local direction = DirectionAt(face, u, v)
	local elevation, signedElevation = ElevationAt(body, direction)
	return body.Centre + direction * body.Radius * (1 + elevation), direction, elevation, signedElevation
end

local function BiomeAt(body: BodyRecipe, direction: Vector3): number
	local axis = TiltAxis(body.TiltDegrees)
	if body.GasGiant then
		-- Banded flow: latitude drives the band, flow noise wobbles it. Mesh
		-- rotation animates the result; the field itself stays still so the
		-- selection and the LUT never disagree about a direction.
		local latitude = direction:Dot(axis)
		local flow = math.noise(
			direction.X * 3.1 + body.Seed % 97,
			direction.Y * 3.1 - 41,
			direction.Z * 3.1 + 17
		)
		local band = Clamp01(0.5 + 0.5 * math.sin(latitude * 14 + flow * 3.2))
		return band * (body.BiomeRows - 1)
	end
	local boundaryNoise = math.noise(
		direction.X * 2.6 + 31,
		direction.Y * 2.6 - 17,
		direction.Z * 2.6 + 73
	)
	-- Polar caps follow the tilted axis, not world Y: the biome pole is the
	-- planet's own pole.
	local latitude = Clamp01(math.abs(direction:Dot(axis)) + boundaryNoise * 0.075)
	return latitude * (body.BiomeRows - 1)
end

-- One texture per body packs ocean into its first half and land into its
-- second. Rows are biomes, so the fragment shader gets both dimensions from
-- the UV channel the built-in vertex stage already forwards.
local function BuildLut(body: BodyRecipe): EditableImage
	local lut = Instance.new("EditableImage")
	lut.Name = `PlanetColourLUT_{body.Name}`
	assert(lut:Resize(body.LutWidth, body.BiomeRows), `planet LUT for {body.Name} was refused`)
	local landStart = body.LutWidth / 2

	for row = 0, body.BiomeRows - 1 do
		for x = 0, body.LutWidth - 1 do
			local colour: Color3
			if x < landStart then
				colour = LerpColour(body.OceanDeep, body.OceanShallow, x / (landStart - 1))
			else
				colour = LerpColour(
					body.LandLow[row + 1],
					body.LandHigh[row + 1],
					(x - landStart) / (body.LutWidth - landStart - 1)
				)
			end
			lut:DrawRectangle(Vector2.new(x, row), Vector2.new(1, 1), colour)
		end
	end

	return lut
end

local bodyLuts: { EditableImage } = {}
for _, body in Bodies do
	table.insert(bodyLuts, BuildLut(body))
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
	// Night-side settlements: a deterministic cell hash over the biome/height
	// UV, lit only where the sun is down and the ground is land.
	float night = smoothstep(0.05, -0.15, lambert);
	vec2 cell = floor(inTexCoord * vec2(240.0, 90.0));
	float sparkle = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
	lit += vec3(1.0, 0.75, 0.45) * (night * land * step(0.86, sparkle) * 0.9);
	outColour = vec4(lit, max(inColour.a, lighting.Flags.w));
}
]]
surfaceShader.Parent = workspace

-- The giant gets its own fragment path: stretched bands and limb darkening,
-- and deliberately no night lights, oceans or specular.
local gasShader = Instance.new("ShaderScript")
gasShader.Name = "PlanetSurfaceGas"
gasShader.Source = [[
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
	float band = clamp(inTexCoord.y, 0.0, 1.0);
	float biome = (clamp(inTexCoord.x, 0.0, BIOME_ROWS - 1.0) + 0.5) / BIOME_ROWS;
	// Fine striping over the baked bands so the giant holds up closer than
	// four LUT rows would on their own.
	float stripe = 0.96 + 0.04 * sin(band * 220.0);
	vec3 albedo = texture(colourMap, vec2(band, biome)).rgb * stripe;
	vec3 normal = normalize(inNormal);
	vec3 toLight = -normalize(lighting.Direction.xyz);
	float lambert = max(dot(normal, toLight), 0.0);
	vec3 lit = albedo * inColour.rgb * inSurfaceColour;
	lit *= lighting.Ambient.rgb + lighting.Direct.rgb * pow(lambert, 0.75);
	outColour = vec4(lit, max(inColour.a, lighting.Flags.w));
}
]]
gasShader.Parent = workspace

local OVERLAY_COLOURS: { Color3 } = {
	Color3.fromRGB(90, 200, 255),
	Color3.fromRGB(120, 255, 170),
	Color3.fromRGB(255, 220, 120),
	Color3.fromRGB(255, 140, 120),
}

local function NodeKey(bodyName: string, face: Face, depth: number, x: number, y: number): string
	return `{bodyName}_{face.Name}_D{depth}_X{x}_Y{y}`
end

local function RootNode(bodyIndex: number, face: Face): Node
	return {
		Body = bodyIndex,
		Face = face,
		Depth = 0,
		X = 0,
		Y = 0,
		U0 = -1,
		V0 = -1,
		Size = 2,
		Key = NodeKey(Bodies[bodyIndex].Name, face, 0, 0, 0),
		Priority = 0,
	}
end

local function ChildNode(bodyIndex: number, parent: Node, dx: number, dy: number): Node
	local size = parent.Size * 0.5
	local depth = parent.Depth + 1
	local x = parent.X * 2 + dx
	local y = parent.Y * 2 + dy
	return {
		Body = bodyIndex,
		Face = parent.Face,
		Depth = depth,
		X = x,
		Y = y,
		U0 = parent.U0 + dx * size,
		V0 = parent.V0 + dy * size,
		Size = size,
		Key = NodeKey(Bodies[bodyIndex].Name, parent.Face, depth, x, y),
		Priority = parent.Priority,
	}
end

type EdgeMask = {
	West: boolean,
	East: boolean,
	South: boolean,
	North: boolean,
}

-- The neighbour cell at the same depth across one edge, found through 3D
-- rather than a hand-kept adjacency table: push a probe just over the edge,
-- spherify it, and ask which face owns that direction. One code path serves
-- all 12 cube edges and all 8 corners, and it cannot disagree with the face
-- bases because it derives from them.
local function NeighbourCell(node: Node, edge: string): (Face, number, number)
	local su = node.U0 + node.Size * 0.5
	local sv = node.V0 + node.Size * 0.5
	if edge == "West" then
		su = node.U0 - node.Size * 0.02
	elseif edge == "East" then
		su = node.U0 + node.Size * 1.02
	elseif edge == "South" then
		sv = node.V0 - node.Size * 0.02
	else
		sv = node.V0 + node.Size * 1.02
	end
	local cube = node.Face.LocalUp + node.Face.AxisA * su + node.Face.AxisB * sv
	local direction = Spherify(cube)
	local owner = FaceOfDirection(direction)
	local uu, vv = UvOnFace(owner, direction)
	local cells = 2 ^ node.Depth
	local cell = 2 / cells
	local x = math.clamp(math.floor((uu + 1) / cell), 0, cells - 1)
	local y = math.clamp(math.floor((vv + 1) / cell), 0, cells - 1)
	return owner, x, y
end

-- The leaf covering a cell, or the coarser ancestor leaf when the cell itself
-- is not resident. Ancestor-or-self is what makes balancing and stitch-snaps
-- read the same tree the renderer draws.
local function CoveringNode(
	bodyIndex: number,
	face: Face,
	depth: number,
	x: number,
	y: number,
	leaves: { [string]: Node }
): Node?
	local bodyName = Bodies[bodyIndex].Name
	local cx = x
	local cy = y
	local d = depth
	while d >= 0 do
		local candidate = leaves[NodeKey(bodyName, face, d, cx, cy)]
		if candidate ~= nil then
			return candidate
		end
		cx = math.floor(cx / 2)
		cy = math.floor(cy / 2)
		d -= 1
	end
	return nil
end

-- Neighbour leaves may differ by at most one level, including across cube
-- faces. A coarse leaf beside a leaf two levels finer splits until the rule
-- holds; the loop is bounded and the leaf budget caps pathological recipes.
local function BalanceLeaves(leaves: { Node }): { Node }
	local map: { [string]: Node } = {}
	for _, leaf in leaves do
		map[leaf.Key] = leaf
	end

	for _ = 1, 8 do
		local changed = false
		local snapshot: { Node } = {}
		for _, leaf in map do
			table.insert(snapshot, leaf)
		end
		for _, leaf in snapshot do
			if map[leaf.Key] == nil then
				continue
			end
			local body = Bodies[leaf.Body]
			local edges = { "West", "East", "South", "North" }
			for _, edge in edges do
				local owner, nx, ny = NeighbourCell(leaf, edge)
				local covering = CoveringNode(leaf.Body, owner, leaf.Depth, nx, ny, map)
				if covering ~= nil and covering.Depth < leaf.Depth - 1 and covering.Depth < body.MaxDepth then
					if map[covering.Key] == nil then
						continue
					end
					map[covering.Key] = nil
					for dy = 0, 1 do
						for dx = 0, 1 do
							local child = ChildNode(leaf.Body, covering, dx, dy)
							if map[child.Key] == nil then
								map[child.Key] = child
							end
						end
					end
					changed = true
				end
			end
		end
		if not changed then
			break
		end
		local count = 0
		for _ in map do
			count += 1
		end
		if count > MAX_LEAVES_PER_BODY * #Bodies then
			break
		end
	end

	local balanced: { Node } = {}
	for _, leaf in map do
		table.insert(balanced, leaf)
	end
	return balanced
end

local function CoarseEdgeMask(node: Node, leaves: { [string]: Node }): EdgeMask
	local function isCoarse(edge: string): boolean
		local owner, nx, ny = NeighbourCell(node, edge)
		local covering = CoveringNode(node.Body, owner, node.Depth, nx, ny, leaves)
		return covering ~= nil and covering.Depth < node.Depth
	end
	return {
		West = isCoarse("West"),
		East = isCoarse("East"),
		South = isCoarse("South"),
		North = isCoarse("North"),
	}
end

local statVertices = 0
local statKernelEvals = 0
local statGeometryMs = 0

local function ChunkGeometry(
	body: BodyRecipe,
	node: Node,
	nodeResolution: number,
	mask: EdgeMask
): ({ MeshVertex }, { number }, Vector3, Vector3)
	local startClock = elapsedTime()
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

	local maxOceanDepth = 0.1
	local maxElevation = 0.22
	for gy = 0, nodeResolution - 1 do
		local acrossY = gy / (nodeResolution - 1)
		for gx = 0, nodeResolution - 1 do
			local acrossX = gx / (nodeResolution - 1)
			local u = node.U0 + node.Size * acrossX
			local v = node.V0 + node.Size * acrossY
			local position, direction, elevation, signedElevation = SurfaceAt(body, node.Face, u, v)
			local biome = BiomeAt(body, direction)
			local heightCoordinate: number
			if body.GasGiant then
				-- The band value rides the land half; the ocean half is never
				-- addressed by giant UVs.
				heightCoordinate = 0.5 + 0.5 * Clamp01(biome / math.max(body.BiomeRows - 1, 1))
			elseif signedElevation <= 0 then
				heightCoordinate = 0.5 * Clamp01(1 + signedElevation / maxOceanDepth)
			else
				heightCoordinate = 0.5 + 0.5 * Clamp01(elevation / maxElevation)
			end
			addVertex(
				position,
				direction,
				Vector2.new(biome, heightCoordinate)
			)
		end
	end

	-- Coarse-edge snapping: where the neighbour is a level coarser, odd edge
	-- vertices sit on the smooth field while the neighbour draws the straight
	-- segment between its own corners. Pulling them to the segment midpoint
	-- trades a small geometric error for a watertight join; the radial skirt
	-- below backs it up rather than being the whole answer.
	local function snapEdgeVertex(id: number, prevId: number, nextId: number)
		local prev = rawVertices[prevId + 1]
		local next = rawVertices[nextId + 1]
		rawVertices[id + 1] = {
			Position = (prev.Position + next.Position) * 0.5,
			Normal = rawVertices[id + 1].Normal,
			UV = rawVertices[id + 1].UV,
		}
	end

	if mask.South then
		for gx = 1, nodeResolution - 2, 2 do
			snapEdgeVertex(gx, gx - 1, gx + 1)
		end
	end
	if mask.North then
		local row = (nodeResolution - 1) * nodeResolution
		for gx = 1, nodeResolution - 2, 2 do
			snapEdgeVertex(row + gx, row + gx - 1, row + gx + 1)
		end
	end
	if mask.West then
		for gy = 1, nodeResolution - 2, 2 do
			snapEdgeVertex(gy * nodeResolution, (gy - 1) * nodeResolution, (gy + 1) * nodeResolution)
		end
	end
	if mask.East then
		for gy = 1, nodeResolution - 2, 2 do
			local id = gy * nodeResolution + nodeResolution - 1
			snapEdgeVertex(id, id - nodeResolution, id + nodeResolution)
		end
	end

	-- `axisA x axisB` points along `localUp`, so `(a,b,c)` is
	-- counter-clockwise from outside on all six faces.
	for gy = 0, nodeResolution - 2 do
		for gx = 0, nodeResolution - 2 do
			local a = gx + gy * nodeResolution
			local b = a + 1
			local c = a + nodeResolution
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
	local skirtDepth = if body.GasGiant then 0.8 else 1.5
	local perimeter: { number } = {}
	for x = 0, nodeResolution - 1 do
		table.insert(perimeter, x)
	end
	for y = 1, nodeResolution - 1 do
		table.insert(perimeter, y * nodeResolution + nodeResolution - 1)
	end
	for x = nodeResolution - 2, 0, -1 do
		table.insert(perimeter, (nodeResolution - 1) * nodeResolution + x)
	end
	for y = nodeResolution - 2, 1, -1 do
		table.insert(perimeter, y * nodeResolution)
	end

	local skirt: { number } = {}
	for _, outerId in perimeter do
		local outer = rawVertices[outerId + 1]
		table.insert(
			skirt,
			addVertex(outer.Position - outer.Normal * skirtDepth, outer.Normal, outer.UV)
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

	statVertices += nodeResolution * nodeResolution
	local octaveSum = 0
	for _, layer in body.Layers do
		if layer.Enabled then
			octaveSum += layer.Octaves
		end
	end
	statKernelEvals += nodeResolution * nodeResolution * octaveSum
	statGeometryMs += elapsedTime() - startClock

	return vertices, indices, centre, size
end

local live: { [string]: Chunk } = {}

local function BuildChunk(node: Node, leaves: { [string]: Node })
	if live[node.Key] ~= nil then
		return
	end

	local body = Bodies[node.Body]
	local mask = CoarseEdgeMask(node, leaves)
	local vertices, indices, centre, size = ChunkGeometry(body, node, body.PatchResolution, mask)
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
	part.TextureID = bodyLuts[node.Body].ContentId
	part.Parent = workspace

	local material = Instance.new("Material")
	material.Name = "PlanetMaterial"
	material.Shader = if body.GasGiant then gasShader.Name else surfaceShader.Name
	material.Parent = part

	local overlay: SelectionBox? = nil
	if OVERLAY_ENABLED then
		local box = Instance.new("SelectionBox")
		box.Adornee = part
		box.Color3 = OVERLAY_COLOURS[math.min(node.Depth + 1, #OVERLAY_COLOURS)]
		box.SurfaceColor3 = OVERLAY_COLOURS[math.min(node.Depth + 1, #OVERLAY_COLOURS)]
		box.LineThickness = 0.05
		box.SurfaceTransparency = 0.9
		box.AlwaysOnTop = true
		box.Parent = part
		overlay = box
	end

	live[node.Key] = { Part = part, Mesh = mesh, Node = node, BasePosition = centre, Overlay = overlay }
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

local desired: { [string]: Node } = {}
local pending: { Node } = {}

local function ActiveCameraPosition(): Vector3
	local current = workspace.CurrentCamera
	return if current ~= nil then (current :: Camera).CFrame.Position else Vector3.new(0, 0, ORBIT_RADIUS)
end

local function ActiveCameraLook(): Vector3
	local current = workspace.CurrentCamera
	return if current ~= nil then (current :: Camera).CFrame.LookVector else Vector3.new(0, 0, -1)
end

local function ActiveCameraFov(): number
	local current = workspace.CurrentCamera
	return if current ~= nil then (current :: Camera).FieldOfView else 58
end

-- Projected geometric error selection. Error is a span heuristic, ten percent
-- of the patch arc, until the bounded estimator the plan requires is ready.
-- Split demand merges the finest request per region later; hidden and
-- back-facing patches keep their coarse roots.
local function ShouldSplit(body: BodyRecipe, node: Node, cameraPosition: Vector3, cameraLook: Vector3): (boolean, number)
	local centreDirection = DirectionAt(
		node.Face,
		node.U0 + node.Size * 0.5,
		node.V0 + node.Size * 0.5
	)
	local toCamera = cameraPosition - body.Centre
	local cameraDirection = if toCamera.Magnitude > 0 then toCamera.Unit else Vector3.new(0, 0, 1)
	local facing = centreDirection:Dot(cameraDirection)
	local surface = body.Centre + centreDirection * body.Radius * (1 + ElevationAt(body, centreDirection))
	local offset = surface - cameraPosition
	local distance = offset.Magnitude
	if node.Depth >= body.MaxDepth or facing <= -0.3 then
		return false, distance
	end
	-- Frustum gate: a patch well behind the camera keeps its coarse root even
	-- if it is near. The margin leaves a coarse ring around the frame so a fast
	-- orbit never shows a hole at the screen edge.
	if distance > 1e-3 then
		local viewCos = offset.Unit:Dot(cameraLook)
		local patchSpan = body.Radius * node.Size * 0.7854
		if viewCos < -0.15 and distance > patchSpan * 4 then
			return false, distance
		end
	end
	local patchSpan = body.Radius * node.Size * 0.7854
	local error = patchSpan * ERROR_FACTOR
	local halfFov = math.rad(ActiveCameraFov() * 0.5)
	local projected = error * SCREEN_HEIGHT_PX / (2 * math.max(distance, 1e-3) * math.tan(halfFov))
	return projected > SPLIT_PIXELS, distance
end

local function SelectLeaves(
	bodyIndex: number,
	node: Node,
	cameraPosition: Vector3,
	cameraLook: Vector3,
	leaves: { Node }
)
	local body = Bodies[bodyIndex]
	local split, distance = ShouldSplit(body, node, cameraPosition, cameraLook)
	if not split then
		node.Priority = distance
		table.insert(leaves, node)
		return
	end

	for dy = 0, 1 do
		for dx = 0, 1 do
			SelectLeaves(bodyIndex, ChildNode(bodyIndex, node, dx, dy), cameraPosition, cameraLook, leaves)
		end
	end
end

local function PlanSelection()
	if #pending > 0 then
		return
	end

	local leaves: { Node } = {}
	local cameraPosition = ActiveCameraPosition()
	local cameraLook = ActiveCameraLook()
	for bodyIndex, _ in Bodies do
		for _, face in faces do
			if FaceEnabled(face) then
				SelectLeaves(bodyIndex, RootNode(bodyIndex, face), cameraPosition, cameraLook, leaves)
			end
		end
	end

	leaves = BalanceLeaves(leaves)

	desired = {}
	pending = {}
	for _, node in leaves do
		desired[node.Key] = node
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

local function ContainsNode(ancestor: Node, descendant: Node): boolean
	if ancestor.Body ~= descendant.Body then
		return false
	end
	if ancestor.Face.Name ~= descendant.Face.Name or ancestor.Depth > descendant.Depth then
		return false
	end

	local span = 2 ^ (descendant.Depth - ancestor.Depth)
	return math.floor(descendant.X / span) == ancestor.X
		and math.floor(descendant.Y / span) == ancestor.Y
end

-- A coarse patch is retired as soon as the current leaf set covers its whole
-- area. Waiting for every requested patch lets old roots fight their finished
-- children in the depth buffer for hundreds of ticks on a detailed view. The
-- parent stays pinned while its children build: retirement only removes chunks
-- the live leaf set already covers, so coverage is never dropped for a promise.
local function CoveredByLiveLeaves(node: Node): boolean
	local covered = 0
	for _, leaf in desired do
		if leaf.Body ~= node.Body then
			continue
		end
		if leaf.Depth <= node.Depth then
			if ContainsNode(leaf, node) and live[leaf.Key] ~= nil then
				return true
			end
		elseif ContainsNode(node, leaf) and live[leaf.Key] ~= nil then
			covered += 4 ^ (node.Depth - leaf.Depth)
		end
	end
	return covered >= 1
end

local function RetireCoveredChunks()
	local retired: { string } = {}
	for key, chunk in live do
		if desired[key] == nil and CoveredByLiveLeaves(chunk.Node) then
			table.insert(retired, key)
		end
	end
	table.sort(retired)
	for _, key in retired do
		DestroyChunk(key)
	end
end

local function ClearBody(bodyIndex: number)
	local doomed: { string } = {}
	for key, chunk in live do
		if chunk.Node.Body == bodyIndex then
			table.insert(doomed, key)
		end
	end
	table.sort(doomed)
	for _, key in doomed do
		DestroyChunk(key)
	end
	for key, node in desired do
		if node.Body == bodyIndex then
			desired[key] = nil
		end
	end
	local kept: { Node } = {}
	for _, node in pending do
		if node.Body ~= bodyIndex then
			table.insert(kept, node)
		end
	end
	pending = kept
end

local function CountLive(bodyIndex: number): number
	local count = 0
	for _, chunk in live do
		if chunk.Node.Body == bodyIndex then
			count += 1
		end
	end
	return count
end

local eye = Instance.new("Camera")
eye.Name = "PlanetCamera"
eye.FieldOfView = 58
eye.FarPlaneZ = 1500
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
Lighting.FogStart = 380
Lighting.FogEnd = 950

-- The atmosphere binding is manual in this demo: one global
-- `AtmosphereProcedural` driven from Haven's radius (70) with a shell height
-- near eight percent of it. A multi-body binding needs the engine-side
-- per-planet shell the plan describes.
local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Haven Atmosphere"
atmosphere.Density = 0.22
atmosphere.Haze = 0.8
atmosphere.Glare = 0.15
atmosphere.Rayleigh = 1.0
atmosphere.Mie = 0.12
atmosphere.Samples = 12
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Haven Clouds"
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

-- Zephyr's ring system: 120 deterministic shards in a tilted plane, sized and
-- shaded by index hash so the run is recorded. Static, unlit by any system the
-- engine would have to know about.
do
	local giant = Bodies[3]
	local tilt = math.rad(27)
	local cosT = math.cos(tilt)
	local sinT = math.sin(tilt)
	for index = 0, 119 do
		local angle = Hash01(index, 501) * math.pi * 2
		local radius = giant.Radius * (1.4 + Hash01(index, 502) * 0.9)
		local px = math.cos(angle) * radius
		local pz = math.sin(angle) * radius
		local py = pz * sinT
		local pzTilted = pz * cosT
		local shard = Instance.new("Part")
		shard.Name = "ZephyrRing"
		shard.Anchored = true
		shard.CanCollide = false
		local edge = 1.2 + Hash01(index, 503) * 2.6
		shard.Size = Vector3.new(edge, 0.3, edge * (0.5 + Hash01(index, 504)))
		shard.Position = giant.Centre + Vector3.new(px, py, pzTilted)
		local tone = 150 + math.floor(Hash01(index, 505) * 70)
		shard.Color = Color3.fromRGB(tone, tone - 12, tone - 28)
		shard.Parent = workspace
	end
end

-- Vegetation scatter on Haven: a Fibonacci sphere of candidates, kept where
-- the field is low land and level. Density-driven and filtered by altitude, as
-- the plan's placement domain requires; published as plain parts, not meshes.
do
	local haven = Bodies[1]
	local placed = 0
	local candidates = 220
	for index = 0, candidates - 1 do
		if placed >= 70 then
			break
		end
		local y = 1 - 2 * (index + 0.5) / candidates
		local radial = math.sqrt(math.max(0, 1 - y * y))
		local phi = index * 2.39996
		local direction = Vector3.new(math.cos(phi) * radial, y, math.sin(phi) * radial)
		local elevation, signed = ElevationAt(haven, direction)
		if signed > 0.005 and elevation < 0.12 then
			-- Slope proxy: two tangent taps. Steep ground stays bare whatever
			-- height it is at, which is what stops a mountainside from being
			-- green to the snow line.
			local tangent = direction:Cross(Vector3.new(0, 1, 0))
			if tangent.Magnitude < 1e-3 then
				tangent = direction:Cross(Vector3.new(1, 0, 0))
			end
			tangent = tangent.Unit
			local bitangent = direction:Cross(tangent).Unit
			local eps = 0.01
			local e1 = ElevationAt(haven, (direction + tangent * eps).Unit)
			local e2 = ElevationAt(haven, (direction + bitangent * eps).Unit)
			if math.abs(e1 - elevation) + math.abs(e2 - elevation) < 0.02 then
				local tree = Instance.new("Part")
				tree.Name = "HavenVegetation"
				tree.Anchored = true
				tree.CanCollide = false
				local height = 0.5 + Hash01(index, 601) * 0.9
				tree.Size = Vector3.new(0.5, height, 0.5)
				tree.Position = haven.Centre + direction * haven.Radius * (1 + elevation) + direction * height * 0.5
				if Hash01(index, 602) < 0.2 then
					tree.Color = Color3.fromRGB(121, 85, 58)
				else
					tree.Color = Color3.fromRGB(46 + math.floor(Hash01(index, 603) * 30), 110, 58)
				end
				tree.Parent = workspace
				placed += 1
			end
		end
	end
	print(`planet: scattered {placed} vegetation tufts on Haven`)
end

-- Rodrigues rotation of an offset around an axis. Used to turn Zephyr's chunks
-- slowly: tick time, not wall time, so two recorded runs generate the same
-- flow.
local function RotateAroundAxis(offset: Vector3, axis: Vector3, angle: number): Vector3
	local c = math.cos(angle)
	local s = math.sin(angle)
	local cross = axis:Cross(offset)
	local dot = axis:Dot(offset)
	return offset * c + cross * s + axis * (dot * (1 - c))
end

local zephyrAxis = TiltAxis(Bodies[3].TiltDegrees)

local function TurnGasGiant(tick: number)
	local angle = tick * 0.0006
	for _, chunk in live do
		if chunk.Node.Body == 3 then
			local offset = chunk.BasePosition - Bodies[3].Centre
			;(chunk.Part :: any).Position = Bodies[3].Centre + RotateAroundAxis(offset, zephyrAxis, angle);
		end
	end
end

-- Six coarse roots per body make every globe complete before refinement
-- starts. Finer leaves are built before their covering roots are retired, so a
-- slow machine sees temporary overlap rather than a hole through the planet.
-- The camera is installed first because each mesh transaction yields; an
-- embedding Studio may replace CurrentCamera while those commits are still
-- arriving.
for bodyIndex, _ in Bodies do
	for _, face in faces do
		if FaceEnabled(face) then
			BuildChunk(RootNode(bodyIndex, face), {})
		end
	end
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
	TurnGasGiant(tick)

	-- Deterministic reseed showcase: Haven steps to a new seed through the
	-- authoring rebuild path, proving recipe edits reuse no stale state.
	if tick == AUTO_RESEED_TICK then
		local haven = Bodies[1]
		haven.Seed = RESEED_SEED
		for _, layer in haven.Layers do
			layer.Centre = SeedOffset(haven.Seed, layer.Salt)
		end
		ClearBody(1)
		print(`planet: reseeded Haven to {RESEED_SEED} at tick {tick}`)
	end

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
		BuildChunk(nextNode, desired)
	end

	RetireCoveredChunks()

	reconciling = false

	-- Per-stage timing readout, reporting only: wall time never steers a
	-- selection, so the numbers vary between machines while the leaf sets do
	-- not.
	if tick % 300 == 0 then
		print(
			`planet: tick {tick} live Haven={CountLive(1)} Cinder={CountLive(2)} Zephyr={CountLive(3)} `
				.. `pending={#pending} sampled={statVertices} verts `
				.. `kernels={statKernelEvals} geometry={string.format("%.1f", statGeometryMs * 1000)}ms`
		)
		statVertices = 0
		statKernelEvals = 0
		statGeometryMs = 0
	end
end)

print(
	`planet: Haven seed {Bodies[1].Seed} r={Bodies[1].Radius}, `
		.. `Cinder seed {Bodies[2].Seed} r={Bodies[2].Radius}, `
		.. `Zephyr r={Bodies[3].Radius} bands, `
		.. `{Bodies[1].PatchResolution}x{Bodies[1].PatchResolution} quadtree patches with error selection`
)

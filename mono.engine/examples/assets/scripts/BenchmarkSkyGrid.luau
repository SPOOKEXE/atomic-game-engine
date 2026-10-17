--!strict
-- A moving PBR course through eight floating, procedurally displaced terrain balls.
--
-- This is a benchmark scene, not a static gallery. Every ball owns an
-- `EditableMesh`, all eight material routes name a distinct runtime fragment
-- program, and the camera repeatedly flies through the seam between each pair.
-- That keeps editable mesh upload, shader compilation, material binding,
-- lighting and frustum rejection visible together in a repeatable capture.
--
-- ## Run it
--
--     client --script BenchmarkSkyGrid.luau --stats --graph
--
-- Profile the steady middle of a flight in `release`. The course is deliberately
-- fixed: changing its count or layout makes frame captures incomparable.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local BALL_COUNT = 8
local BALL_RADIUS = 9.5
local BALL_SEAM = 6.0
local ROW_SPACING = 31
local ROW_COUNT = BALL_COUNT / 2
local MESH_RINGS = 44
local MESH_SEGMENTS = 64
local MAP_SIZE = 256
local CAMERA_HEIGHT = 12
local CAMERA_SPEED = 15
local CAMERA_LOOK_AHEAD = 42
local FLIGHT_PERIOD_TICKS = 600

type PbrMaps = {
	Colour: EditableImage,
	Normal: EditableImage,
	Roughness: EditableImage,
	Occlusion: EditableImage,
	Metalness: EditableImage,
	Emissive: EditableImage,
}

type ShaderStyle = {
	Name: string,
	Tint: Color3,
	Frequency: number,
	Contour: number,
	Metalness: number,
}

-- Eight separate sources are intentional. One parameterised shader would show
-- one pipeline variant, while this course is here to make the renderer compile
-- and bind all eight material programs in one bounded scene.
local STYLES: { ShaderStyle } = {
	{ Name = "SkyGridBasalt", Tint = Color3.fromRGB(90, 128, 166), Frequency = 5.0, Contour = 0.12, Metalness = 0.04 },
	{ Name = "SkyGridAmber", Tint = Color3.fromRGB(205, 132, 62), Frequency = 7.0, Contour = 0.18, Metalness = 0.08 },
	{ Name = "SkyGridVerdant", Tint = Color3.fromRGB(76, 161, 105), Frequency = 9.0, Contour = 0.10, Metalness = 0.02 },
	{ Name = "SkyGridCobalt", Tint = Color3.fromRGB(70, 127, 210), Frequency = 11.0, Contour = 0.22, Metalness = 0.15 },
	{ Name = "SkyGridViolet", Tint = Color3.fromRGB(155, 100, 204), Frequency = 13.0, Contour = 0.16, Metalness = 0.06 },
	{ Name = "SkyGridCopper", Tint = Color3.fromRGB(190, 109, 71), Frequency = 15.0, Contour = 0.24, Metalness = 0.34 },
	{ Name = "SkyGridGlacier", Tint = Color3.fromRGB(120, 190, 214), Frequency = 17.0, Contour = 0.08, Metalness = 0.10 },
	{ Name = "SkyGridEmber", Tint = Color3.fromRGB(212, 84, 66), Frequency = 19.0, Contour = 0.20, Metalness = 0.18 },
}

local function NewImage(name: string): EditableImage
	local image = Instance.new("EditableImage")
	image.Name = name
	assert(image:Resize(MAP_SIZE, MAP_SIZE), `{name} size was refused`)
	return image
end

local function Hash(x: number, y: number): number
	local value = math.sin(x * 127.1 + y * 311.7) * 43758.5453
	return value - math.floor(value)
end

local function TerrainSample(u: number, v: number): number
	local ridges = math.abs(math.sin(u * math.pi * 9.0 + math.sin(v * math.pi * 5.0) * 1.8))
	local shelves = math.sin(v * math.pi * 14.0 + u * math.pi * 3.0) * 0.5 + 0.5
	local cells = Hash(math.floor(u * 31), math.floor(v * 23)) - 0.5
	return math.clamp(0.36 + ridges * 0.34 + shelves * 0.18 + cells * 0.18, 0, 1)
end

local function PaintMaps(): PbrMaps
	local maps: PbrMaps = {
		Colour = NewImage("SkyGridPbr_Colour"),
		Normal = NewImage("SkyGridPbr_Normal"),
		Roughness = NewImage("SkyGridPbr_Roughness"),
		Occlusion = NewImage("SkyGridPbr_Occlusion"),
		Metalness = NewImage("SkyGridPbr_Metalness"),
		Emissive = NewImage("SkyGridPbr_Emissive"),
	}
	local images: { [string]: EditableImage } = {
		Colour = maps.Colour,
		Normal = maps.Normal,
		Roughness = maps.Roughness,
		Occlusion = maps.Occlusion,
		Metalness = maps.Metalness,
		Emissive = maps.Emissive,
	}
	local pixels: { [string]: buffer } = {}
	for name, _ in images do
		pixels[name] = buffer.create(MAP_SIZE * MAP_SIZE * 4)
	end

	local function Put(target: buffer, offset: number, red: number, green: number, blue: number)
		buffer.writeu8(target, offset, math.floor(math.clamp(red, 0, 255)))
		buffer.writeu8(target, offset + 1, math.floor(math.clamp(green, 0, 255)))
		buffer.writeu8(target, offset + 2, math.floor(math.clamp(blue, 0, 255)))
		buffer.writeu8(target, offset + 3, 255)
	end

	for y = 0, MAP_SIZE - 1 do
		for x = 0, MAP_SIZE - 1 do
			local u = x / (MAP_SIZE - 1)
			local v = y / (MAP_SIZE - 1)
			local height = TerrainSample(u, v)
			local left = TerrainSample(math.max(u - 1 / MAP_SIZE, 0), v)
			local up = TerrainSample(u, math.max(v - 1 / MAP_SIZE, 0))
			local grain = Hash(x, y) - 0.5
			local offset = (y * MAP_SIZE + x) * 4
			Put(pixels.Colour, offset, 34 + height * 86 + grain * 18, 45 + height * 104 + grain * 16, 36 + height * 73 + grain * 14)
			Put(pixels.Normal, offset, 128 + (height - left) * 470, 128 + (height - up) * 470, 255)
			local roughness = 102 + (1 - height) * 116 + grain * 14
			Put(pixels.Roughness, offset, roughness, roughness, roughness)
			local occlusion = 126 + height * 122
			Put(pixels.Occlusion, offset, occlusion, occlusion, occlusion)
			Put(pixels.Metalness, offset, 12 + height * 30, 12 + height * 30, 12 + height * 30)
			local glow = math.max(0, height - 0.82) * 28
			Put(pixels.Emissive, offset, glow, glow * 0.38, glow * 0.12)
		end
	end

	for name, image in images do
		assert(image:FromBuffer(pixels[name]), `{name} buffer was refused`)
	end
	return maps
end

local function ShaderSource(style: ShaderStyle): string
	return string.format([[
#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;
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
	vec4 BaseColour;
	vec4 Surface;
	vec4 Material;
	vec4 Flipbook;
	vec4 Mirror;
	vec4 PaneNormal;
	vec4 SeamPlane;
	vec4 OutdoorAmbient;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
	vec4 MaterialExtra;
	uvec4 RenderFeatures;
} lighting;

layout(set = 3, binding = 1) uniform Lights {
	vec4 Position[16];
	vec4 Colour[16];
	vec4 Direction[16];
	vec4 Count;
} lights;

layout(set = 3, binding = 2) uniform Beams {
	vec4 BeamRows[41];
} beams;

vec3 LocalLight(vec3 normal) {
	vec3 total = vec3(0.0);
	int count = int(lights.Count.x);
	for (int index = 0; index < 16; index++) {
		if (index >= count) {
			break;
		}
		vec3 offset = lights.Position[index].xyz - inWorldPosition;
		float range = lights.Position[index].w;
		float distanceToLight = length(offset);
		if (range <= 0.0 || distanceToLight > range) {
			continue;
		}
		vec3 direction = offset / max(distanceToLight, 1e-4);
		float lambert = max(dot(normal, direction), 0.0);
		float ratio = distanceToLight / range;
		float window = max(1.0 - ratio * ratio, 0.0);
		float falloff = window * window / (1.0 + distanceToLight * distanceToLight);
		float cone = lights.Direction[index].w;
		if (cone > -1.0) {
			float aligned = dot(-direction, normalize(lights.Direction[index].xyz));
			falloff *= smoothstep(cone, mix(cone, 1.0, 0.1), aligned);
		}
		total += lights.Colour[index].rgb * lambert * falloff;
	}
	return total;
}

// EditableMesh carries UVs but no tangent stream. Reconstructing the frame
// from derivatives keeps the generated normal map in tangent space on every
// terrain ball instead of incorrectly treating its RGB vector as world space.
mat3 CotangentFrame(vec3 normal, vec3 position, vec2 uv) {
	vec3 positionX = dFdx(position);
	vec3 positionY = dFdy(position);
	vec2 uvX = dFdx(uv);
	vec2 uvY = dFdy(uv);
	vec3 tangent = positionX * uvY.y - positionY * uvX.y;
	vec3 bitangent = -positionX * uvY.x + positionY * uvX.x;
	float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-8));
	return mat3(tangent * scale, bitangent * scale, normal);
}

float DistributionGGX(vec3 normal, vec3 halfway, float roughness) {
	float alpha = roughness * roughness;
	float alphaSquared = alpha * alpha;
	float normalHalf = max(dot(normal, halfway), 0.0);
	float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(3.14159265 * denominator * denominator, 1e-5);
}

float GeometrySchlickGGX(float cosine, float roughness) {
	float radius = roughness + 1.0;
	float factor = radius * radius * 0.125;
	return cosine / max(cosine * (1.0 - factor) + factor, 1e-5);
}

vec3 FresnelSchlick(float cosine, vec3 base) {
	return base + (1.0 - base) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

void main() {
	vec3 base = texture(colourMap, inTexCoord).rgb * vec3(%.4f, %.4f, %.4f);
	vec3 mapNormal = texture(normalMap, inTexCoord).xyz * 2.0 - 1.0;
	vec3 normal = normalize(CotangentFrame(normalize(inNormal), inWorldPosition, inTexCoord) * mapNormal);
	float roughness = clamp(texture(roughnessMap, inTexCoord).r, 0.04, 0.98);
	float occlusion = texture(occlusionMap, inTexCoord).r;
	float metalness = clamp(texture(metalnessMap, inTexCoord).r + %.4f, 0.0, 1.0);
	vec3 toLight = -normalize(lighting.Direction.xyz);
	vec3 viewDirection = normalize(lighting.Eye.xyz - inWorldPosition);
	vec3 halfVector = normalize(toLight + viewDirection);
	float lambert = max(dot(normal, toLight), 0.0);
	vec3 albedo = base * inColour.rgb * inSurfaceColour;
	float distribution = DistributionGGX(normal, halfVector, roughness);
	float geometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
		GeometrySchlickGGX(lambert, roughness);
	vec3 baseReflectance = mix(vec3(0.04), albedo, metalness);
	vec3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0), baseReflectance);
	vec3 specular = distribution * geometry * fresnel /
		max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
	vec3 diffuse = albedo * (1.0 - fresnel) * (1.0 - metalness);
	float contour = smoothstep(%.4f, 1.0, abs(sin(inTexCoord.x * %.4f + inTexCoord.y * %.4f)));
	vec3 ambient = mix(albedo, baseReflectance, metalness) * lighting.Ambient.rgb * occlusion;
	vec3 direct = (diffuse * lambert + specular) * lighting.Direct.rgb;
	vec3 lit = ambient + direct + diffuse * LocalLight(normal);
	lit += baseReflectance * contour * (0.08 + 0.16 * (1.0 - roughness));
	lit += texture(emissiveMap, inTexCoord).rgb * (0.15 + contour * 0.35);
	outColour = vec4(lit, max(inColour.a, lighting.Flags.w));
}
]], style.Tint.R, style.Tint.G, style.Tint.B, style.Metalness, style.Contour, style.Frequency, style.Frequency * 0.63)
end

local function CreateShaders()
	for _, style in STYLES do
		local shader = Instance.new("ShaderScript")
		shader.Name = style.Name
		shader.Source = ShaderSource(style)
		shader.Parent = workspace
	end
end

local function ApplyPbrMaps(part: MeshPart, maps: PbrMaps)
	part.TextureID = maps.Colour.ContentId
	part.NormalMap = maps.Normal.ContentId
	part.RoughnessMap = maps.Roughness.ContentId
	part.OcclusionMap = maps.Occlusion.ContentId
	part.MetalnessMap = maps.Metalness.ContentId
	part.EmissiveMap = maps.Emissive.ContentId
	part.EmissiveTint = Color3.new(1, 1, 1)
	part.EmissiveStrength = 0.8
end

local function BallCentre(index: number): Vector3
	local row = math.floor((index - 1) / 2)
	local side = (index - 1) % 2
	local x = if side == 0 then -(BALL_RADIUS + BALL_SEAM * 0.5) else BALL_RADIUS + BALL_SEAM * 0.5
	return Vector3.new(x, 14 + (row % 2) * 3, row * ROW_SPACING)
end

local function BuildTerrainBall(index: number, maps: PbrMaps)
	local mesh = Instance.new("EditableMesh")
	mesh.Name = `SkyGridTerrainMesh_{index}`
	local vertices = {}
	local indices = {}
	local phase = index * 0.37

	for ring = 0, MESH_RINGS do
		local v = ring / MESH_RINGS
		local theta = v * math.pi
		for segment = 0, MESH_SEGMENTS do
			local u = segment / MESH_SEGMENTS
			local phi = u * math.pi * 2
			local direction = Vector3.new(math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
			local terraces = TerrainSample((u + phase) % 1, v)
			local crags = math.sin(phi * (5 + index % 4) + theta * 3.0) * math.sin(theta * 8.0 - phase * 9.0)
			local radius = BALL_RADIUS + (terraces - 0.5) * 1.9 + crags * 0.36
			table.insert(vertices, { Position = direction * radius, Normal = direction, UV = Vector2.new(u, v) })
		end
	end

	for ring = 0, MESH_RINGS - 1 do
		for segment = 0, MESH_SEGMENTS - 1 do
			local a = ring * (MESH_SEGMENTS + 1) + segment
			local b = a + 1
			local c = a + MESH_SEGMENTS + 1
			table.insert(indices, a)
			table.insert(indices, b)
			table.insert(indices, c)
			table.insert(indices, b)
			table.insert(indices, c + 1)
			table.insert(indices, c)
		end
	end

	assert(mesh:SetGeometry(vertices, indices), `SkyGrid terrain mesh {index} transaction was cancelled`)
	local part = Instance.new("MeshPart")
	part.Name = `SkyGridBall_{index}`
	part.Anchored = true
	part.CanCollide = false
	part.Size = Vector3.new(BALL_RADIUS * 2.3, BALL_RADIUS * 2.3, BALL_RADIUS * 2.3)
	part.Position = BallCentre(index)
	part.MeshId = mesh.ContentId
	part.Color = Color3.new(1, 1, 1)
	ApplyPbrMaps(part, maps)
	part.Parent = workspace

	local material = Instance.new("Material")
	material.Name = `SkyGridMaterial_{index}`
	material.Shader = STYLES[index].Name
	material.Parent = part
end

local maps = PaintMaps()
CreateShaders()
for index = 1, BALL_COUNT do
	BuildTerrainBall(index, maps)
end

local key = Instance.new("Part")
key.Name = "SkyGridFlightSeam"
key.Anchored = true
key.CanCollide = false
key.Size = Vector3.new(0.15, 0.15, ROW_SPACING * (ROW_COUNT - 1) + BALL_RADIUS * 2)
key.Position = Vector3.new(0, 5, ROW_SPACING * (ROW_COUNT - 1) * 0.5)
key.Color = Color3.fromRGB(90, 190, 255)
key.EmissiveTint = key.Color
key.EmissiveStrength = 1.8
key.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "SkyGridBenchmarkCamera"
camera.FieldOfView = 66
camera.FarPlaneZ = 260
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.Brightness = 0.8
Lighting.ClockTime = 14
Lighting.Ambient = Color3.fromRGB(24, 28, 38)
Lighting.OutdoorAmbient = Color3.fromRGB(44, 52, 64)
Lighting.FogColor = Color3.fromRGB(3, 7, 14)
Lighting.FogStart = 120
Lighting.FogEnd = 260

local sun = Instance.new("Part")
sun.Name = "SkyGridKeyLight"
sun.Anchored = true
sun.Size = Vector3.new(0.4, 0.4, 0.4)
sun.Position = Vector3.new(-8, 38, 25)
sun.Color = Color3.fromRGB(255, 202, 142)
sun.EmissiveTint = sun.Color
sun.EmissiveStrength = 8
sun.Parent = workspace

local light = Instance.new("PointLight")
light.Color = sun.Color
light.Brightness = 420
light.Range = 60
light.Parent = sun

-- A tick counter makes this course reproducible. A wall-clock camera would
-- choose a different culling state each run, which defeats its use as a trace.
local flightTicks = 0
-- Ten simulated seconds keeps the reset outside the close inspection region,
-- while an integer tick period lets a capture replay the exact same cull path.
local flightLength = CAMERA_SPEED * FLIGHT_PERIOD_TICKS / 60

RunService.Heartbeat:Connect(function()
	flightTicks += 1
	local travelled = (flightTicks * CAMERA_SPEED / 60) % flightLength - BALL_RADIUS
	local eye = Vector3.new(0, CAMERA_HEIGHT, travelled)
	camera.CFrame = CFrame.lookAt(eye, Vector3.new(0, CAMERA_HEIGHT - 1.5, travelled + CAMERA_LOOK_AHEAD))
end)

print(`benchmark skygrid: {BALL_COUNT} terrain meshes, {#STYLES} shader variants, {MESH_RINGS}x{MESH_SEGMENTS} terrain resolution`)

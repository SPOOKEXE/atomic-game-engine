--!strict
-- Mesh PBR maps, built-in mesh shaders, and a runtime shader in one lit view.
--
-- Every surface in this scene shares live EditableImage maps. The centre sphere
-- takes the default deferred PBR route, the left and right cubes select the
-- engine's built-in toon and unlit variants, and the rear sphere uses a
-- ShaderScript that samples every material slot itself. The warm spot and cool
-- point lights make roughness, metalness, normals, occlusion, height and
-- emissive detail visible in a single GPU capture.
--
-- ## Run it
--
--     client --script PbrShaderMaterialDemo.luau

local Lighting = game:GetService("Lighting")

local IMAGE_SIZE = 64
local TILE_SIZE = 8

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

	for y = 0, IMAGE_SIZE - TILE_SIZE, TILE_SIZE do
		for x = 0, IMAGE_SIZE - TILE_SIZE, TILE_SIZE do
			local column = x / TILE_SIZE
			local row = y / TILE_SIZE
			local even = (column + row) % 2 == 0
			local rectangle = Vector2.new(x, y)
			local extent = Vector2.new(TILE_SIZE, TILE_SIZE)

			maps.Colour:DrawRectangle(
				rectangle,
				extent,
				if even then Color3.fromRGB(40, 150, 190) else Color3.fromRGB(216, 104, 38)
			)
			-- Opposing tangent-space slopes make the normal response unambiguous.
			maps.Normal:DrawRectangle(
				rectangle,
				extent,
				if even then Color3.fromRGB(172, 92, 236) else Color3.fromRGB(82, 166, 236)
			)
			maps.Roughness:DrawRectangle(
				rectangle,
				extent,
				Color3.fromRGB(30 + column * 28, 30 + column * 28, 30 + column * 28)
			)
			maps.Occlusion:DrawRectangle(
				rectangle,
				extent,
				if row % 2 == 0 then Color3.fromRGB(255, 255, 255) else Color3.fromRGB(96, 96, 96)
			)
			maps.Height:DrawRectangle(
				rectangle,
				extent,
				if even then Color3.fromRGB(224, 224, 224) else Color3.fromRGB(42, 42, 42)
			)
			maps.Metalness:DrawRectangle(
				rectangle,
				extent,
				if column < 4 then Color3.new(0, 0, 0) else Color3.new(1, 1, 1)
			)
			maps.Emissive:DrawRectangle(rectangle, extent, Color3.new(0, 0, 0))
		end
	end

	-- One emissive mark survives in darkness, which isolates that map from the
	-- direct and ambient lighting terms around it.
	maps.Emissive:DrawCircle(Vector2.new(IMAGE_SIZE * 0.5, IMAGE_SIZE * 0.5), 13, Color3.fromRGB(30, 180, 255))
	return maps
end

local maps = PaintMaps()

local samplerShader = Instance.new("ShaderScript")
samplerShader.Name = "PbrMapSampler"
samplerShader.Source = [[
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

void main() {
	vec2 uv = fract(inTexCoord);
	vec3 albedo = texture(colourMap, uv).rgb;
	vec3 normalSample = texture(normalMap, uv).rgb * 2.0 - 1.0;
	float roughness = texture(roughnessMap, uv).r;
	float occlusion = texture(occlusionMap, uv).r;
	float height = texture(heightMap, uv).r;
	float metalness = texture(metalnessMap, uv).r;
	vec3 emissive = texture(emissiveMap, uv).rgb;
	float normalLight = clamp(normalSample.z * 0.5 + 0.5, 0.0, 1.0);
	vec3 lit = albedo * (0.25 + 0.75 * normalLight) * mix(0.35, 1.0, occlusion);
	lit *= mix(0.45, 1.1, roughness);
	lit = mix(lit, vec3(lit.b, lit.r, lit.g), metalness * 0.35);
	lit += emissive * (0.3 + height * 0.7);
	lit *= lighting.Ambient.rgb + lighting.Direct.rgb;
	outColour = vec4(lit * inColour.rgb * inSurfaceColour, max(inColour.a, lighting.Flags.w));
}
]]
samplerShader.Parent = workspace

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

local function Mesh(name: string, position: Vector3, meshId: string): MeshPart
	local part = Instance.new("MeshPart")
	part.Name = name
	part.Anchored = true
	part.MeshId = meshId
	part.Size = Vector3.new(3.4, 3.4, 3.4)
	part.Position = position
	part.Color = Color3.new(1, 1, 1)
	ApplyMaps(part)
	part.Parent = workspace
	return part
end

local function SelectShader(part: MeshPart, shader: string)
	local material = Instance.new("Material")
	material.Name = `{part.Name}_{shader}`
	material.Shader = shader
	material.Parent = part
end

-- Default PBR is left without a Material child. It therefore proves the
-- deferred route directly from MeshPart map properties.
Mesh("DefaultPbr", Vector3.new(-2, 2.25, 0), "engine.Sphere")

local toon = Mesh("ToonPbrTexture", Vector3.new(-6, 2.0, 0), "engine.Cube")
SelectShader(toon, "toon")

local unlit = Mesh("UnlitPbrTexture", Vector3.new(6, 2.0, 0), "engine.Cube")
SelectShader(unlit, "unlit")

local sampled = Mesh("RuntimeShaderMaps", Vector3.new(2, 2.25, 0), "engine.Sphere")
SelectShader(sampled, samplerShader.Name)

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
coolLamp.Position = Vector3.new(-6, 5, 3)
coolLamp.Color = Color3.fromRGB(70, 190, 255)
coolLamp.EmissiveTint = coolLamp.Color
coolLamp.EmissiveStrength = 6
coolLamp.Parent = workspace

local point = Instance.new("PointLight")
point.Color = coolLamp.Color
point.Brightness = 280
point.Range = 14
point.Parent = coolLamp

local warmLamp = Instance.new("Part")
warmLamp.Name = "WarmSpotLamp"
warmLamp.Anchored = true
warmLamp.Size = Vector3.new(0.4, 0.4, 0.4)
warmLamp.Position = Vector3.new(2, 6, 5)
warmLamp.Color = Color3.fromRGB(255, 166, 82)
warmLamp.EmissiveTint = warmLamp.Color
warmLamp.EmissiveStrength = 6
warmLamp.Parent = workspace

local spot = Instance.new("SpotLight")
spot.Color = warmLamp.Color
spot.Brightness = 520
spot.Range = 18
spot.Angle = 70
spot.Face = Enum.NormalId.Front
spot.Parent = warmLamp

Lighting.Brightness = 0.55
Lighting.ClockTime = 15
Lighting.Ambient = Color3.fromRGB(12, 16, 27)
Lighting.OutdoorAmbient = Color3.fromRGB(20, 32, 54)
Lighting.FogColor = Color3.fromRGB(2, 4, 9)
Lighting.FogStart = 80
Lighting.FogEnd = 120

local camera = Instance.new("Camera")
camera.Name = "PbrShaderMaterialCamera"
camera.FieldOfView = 52
camera.CFrame = CFrame.lookAt(Vector3.new(0, 5.5, 17), Vector3.new(0, 2.0, -2.0))
camera.Parent = workspace
workspace.CurrentCamera = camera

print("pbr shader material demo: default PBR, toon, unlit, runtime maps, point light, and spot light ready")

--!strict
-- Runtime-authored shaders on a mesh, a surface camera pane, and the screen.
--
-- A `ShaderScript` is selected by its instance name. Meshes and surface panes
-- use the opaque fragment contract through `Material.Shader`. A camera uses the
-- separate tonemap contract through `Lighting.PostProcessShader`, because it
-- processes the completed view rather than one mesh draw.

local surfaceShader = Instance.new("ShaderScript")
surfaceShader.Name = "ExampleSurface"
surfaceShader.Source = [[
#version 450

layout(location = 1) in vec4 inColour;
layout(location = 3) in vec4 inSurfacePosition;
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
	vec3 colour = inColour.rgb;
	if (lighting.Flags.z > 0.5) {
		vec3 projected = inSurfacePosition.xyz / max(inSurfacePosition.w, 0.000001);
		vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
		vec3 image = texture(surfaceMap, uv).rgb;
		colour = mix(image, vec3(image.r * 1.15, image.g * 0.82, image.b * 1.25), 0.55);
	} else {
		float bands = step(0.5, fract(gl_FragCoord.y * 0.08));
		colour *= mix(vec3(0.55, 0.85, 1.25), vec3(1.2, 0.65, 0.95), bands);
	}
	outColour = vec4(colour, max(inColour.a, lighting.Flags.w));
}
]]
surfaceShader.Parent = workspace

local postShader = Instance.new("ShaderScript")
postShader.Name = "ExamplePost"
postShader.Source = [[
#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D colourImage;

void main() {
	vec4 source = texture(colourImage, inUv);
	float vignette = 1.0 - smoothstep(0.25, 0.85, distance(inUv, vec2(0.5)));
	vec3 graded = vec3(source.r * 1.08, source.g * 0.92, source.b * 1.18) * vignette;
	outColour = vec4(pow(max(graded, vec3(0.0)), vec3(1.0 / 2.2)), source.a);
}
]]
postShader.Parent = workspace

local mesh = Instance.new("MeshPart")
mesh.Name = "ShaderMesh"
mesh.Anchored = true
mesh.MeshId = "engine.Sphere"
mesh.Size = Vector3.new(4, 4, 4)
mesh.CFrame = CFrame.new(Vector3.new(-4, 2, 0))
mesh.Color = Color3.new(0.85, 0.9, 1.0)
mesh.Parent = workspace

local meshMaterial = Instance.new("Material")
meshMaterial.Name = "ShaderMaterial"
meshMaterial.Shader = surfaceShader.Name
meshMaterial.Parent = mesh

local pane = Instance.new("Part")
pane.Name = "ShaderSurface"
pane.Anchored = true
pane.Size = Vector3.new(8, 5, 0.3)
pane.CFrame = CFrame.new(Vector3.new(5, 3, -1))
pane.Parent = workspace

local paneMaterial = Instance.new("Material")
paneMaterial.Name = "SurfaceMaterial"
paneMaterial.Shader = surfaceShader.Name
paneMaterial.Parent = pane

local surfaceCamera = Instance.new("SurfaceCamera")
surfaceCamera.Name = "ShaderSurfaceCamera"
surfaceCamera.SurfaceSize = Vector3.new(1024, 640, 0)
surfaceCamera.Face = Enum.NormalId.Front
surfaceCamera.Parent = pane

local camera = Instance.new("Camera")
camera.Name = "ShaderCamera"
camera.CFrame = CFrame.lookAt(Vector3.new(0, 5, 14), Vector3.new(0, 2, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

local lighting = game:GetService("Lighting")
lighting.PostProcessShader = postShader.Name

print("shaders: Luau source, mesh, surface camera pane, and camera postprocess ready")

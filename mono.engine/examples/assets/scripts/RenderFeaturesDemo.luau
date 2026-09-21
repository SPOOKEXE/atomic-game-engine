--!strict

-- Pair this scene with examples/pipelines/RenderFeatures.pipeline. The scene
-- authors all three feature-policy layers, both LOD sources, and the names of
-- the lazy shader nodes in that graph.

local Lighting = game:GetService("Lighting")

local Feature = {
	Shadows = 1,
	AmbientOcclusion = 2,
	Emission = 4,
	Reflections = 8,
	OcclusionCulling = 128,
	ComputeEffects = 1024,
	PostProcessing = 2048,
}

Lighting.Ambient = Color3.fromRGB(16, 20, 30)
Lighting.OutdoorAmbient = Color3.fromRGB(48, 62, 88)
Lighting.Brightness = 2.2
Lighting.ClockTime = 13.5
Lighting.RenderFeatureEnableMask = Feature.Shadows
	+ Feature.AmbientOcclusion
	+ Feature.Emission
	+ Feature.Reflections
	+ Feature.ComputeEffects
	+ Feature.PostProcessing

local floor = Instance.new("Part")
floor.Name = "RenderFeatureFloor"
floor.Size = Vector3.new(28, 1, 28)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(52, 58, 70)
floor.Anchored = true
floor.Parent = workspace

local lod = Instance.new("MeshPart")
lod.Name = "RenderFeatureLodMesh"
lod.MeshId = "engine.Cube"
lod.Size = Vector3.new(5, 5, 5)
lod.Position = Vector3.new(0, 3, 0)
lod.Color = Color3.fromRGB(80, 180, 255)
lod.Anchored = true

-- These names stand in for artifacts published by the automatic bake path.
lod.AutoLodStrategy = Enum.AutoMeshLODStrategy.Decimated
lod.AutoLodLevels = 4
lod.AutoLod1MeshId = "engine.Sphere"
lod.AutoLod2MeshId = "engine.Cylinder"
lod.AutoLod3MeshId = "engine.Wedge"
lod.AutoLod1Ratio = 0.55
lod.AutoLod2Ratio = 0.24
lod.AutoLod3Ratio = 0.09
lod.AutoLodTargetQuadArea = 3

-- Custom level one wins. The empty level two is nil in the component and
-- falls back to engine.Cylinder from AutoMeshLOD.
lod.CustomLodLevels = 4
lod.CustomLod1MeshId = "engine.Plane"
lod.CustomLod2MeshId = nil
lod.CustomLod3MeshId = "engine.CornerWedge"
lod.CustomLod1Ratio = 0.6
lod.CustomLod3Ratio = 0.08

lod.RenderFeatureEnableMask = Feature.Shadows
	+ Feature.OcclusionCulling
	+ Feature.ComputeEffects
	+ Feature.PostProcessing
lod.ComputeEffectNode = "demo-compute"
lod.PostProcessEffectNode = "demo-post"
lod.Parent = workspace

local inherited = Instance.new("MeshPart")
inherited.Name = "RenderFeatureInheritedMesh"
inherited.MeshId = "engine.Sphere"
inherited.Size = Vector3.new(3, 3, 3)
inherited.Position = Vector3.new(7, 2, 0)
inherited.Color = Color3.fromRGB(255, 128, 80)
inherited.Anchored = true
inherited.RenderFeatureDisableMask = Feature.Reflections
inherited.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "RenderFeaturesCamera"
camera.CFrame = CFrame.lookAt(Vector3.new(15, 10, 20), Vector3.new(0, 2, 0))
camera.FieldOfView = 58
camera.RenderFeatureEnableMask = Feature.OcclusionCulling
camera.RenderFeatureDisableMask = Feature.Reflections
camera.Parent = workspace
workspace.CurrentCamera = camera

print("render features demo pipeline: RenderFeatures.pipeline")
print(
	"lod custom/auto/fallback:",
	lod.CustomLod1MeshId,
	lod.AutoLod1MeshId,
	lod.CustomLod2MeshId,
	lod.AutoLod2MeshId
)
print("effects:", lod.ComputeEffectNode, lod.PostProcessEffectNode)

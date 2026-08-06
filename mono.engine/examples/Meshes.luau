--!strict
-- Imported meshes, built-ins, textures, and a mirror.
--
-- Mesh names must match the published manifest names.
--
-- ## What has to line up for a mesh to appear
--
-- The manifest name is the lookup key; an unknown name falls back to a cube.
--
-- ## Run it
--
--     assetc --input ART --output content --model-size 1
--     cdn --publish content --store store --signing-key HEX
--     client --cdn dir:store --publisher-key PUBLIC --script Meshes.luau
--
-- Bake imports with `--model-size 1`; `Size` then denotes metres.
--
-- Without `--cdn`, unresolved imports use the built-in fallback.

local PLATE = 24
local SURFACE_SIZE = Vector3.new(1024, 1024, 0)

-- The names a publisher gives these are the names this asks for. There is no
-- table anywhere that maps one to the other — the manifest's name *is* the
-- mesh's name, which is `AGENTS.md` rule 4 doing its job across a signature.
local IMPORTS = {
	{ mesh = "props/fox.amesh", texture = "", offset = -5 },
	{ mesh = "characters/xiao_pack/芭芭拉.amesh", texture = "", offset = 5 },
}

-- Generated meshes that need no published asset.
local BUILTINS = {
	"engine.Cube",
	"engine.Sphere",
	"engine.Cylinder",
	"engine.Wedge",
	"engine.CornerWedge",
	"engine.Plane",
}

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(PLATE, 0.5, PLATE)
floor.CFrame = CFrame.new(Vector3.new(0, -0.25, 0))
floor.Color = Color3.new(0.30, 0.32, 0.36)
floor.Parent = workspace

-- Built-ins provide a visible mesh-table check.
for index, name in ipairs(BUILTINS) do
	local part = Instance.new("MeshPart")
	part.Name = name
	part.Anchored = true
	part.MeshId = name
	part.Size = Vector3.new(2.4, 2.4, 2.4)
	part.CFrame = CFrame.new(Vector3.new((index - 3.5) * 2.6, 1.5, -7))
	part.Color = Color3.new(0.85, 0.80, 0.70)
	part.Parent = workspace
end

-- Imported meshes use textures named by their baked submeshes.
for _, import in ipairs(IMPORTS) do
	local part = Instance.new("MeshPart")
	part.Name = import.mesh
	part.Anchored = true
	part.MeshId = import.mesh
	if import.texture ~= "" then
		part.TextureID = import.texture
	end

	-- Four metres, and metres only because the bake was `--model-size 1` — see
	-- the header. `assetc` fitted every import's longest axis to one and
	-- centred it, so this scales a unit mesh exactly as it would scale a
	-- built-in cube, which is what makes a PMX character and a glTF fox the
	-- same height.
	part.Size = Vector3.new(4, 4, 4)
	part.CFrame = CFrame.new(Vector3.new(import.offset, 2, 0))
	part.Parent = workspace

	-- **Tagged, so the second mirror below has a group to redirect.** The tag
	-- is a name here and a bit in the component; `scene::TagTable` is where the
	-- two meet, and the draw loop only ever sees the bit.
	part:AddTag("Imported")
end

-- **The mirror, in front of everything.** `ROADMAP.md` v0.9 asks in as many
-- words to "put in front mirrors to see if it works with texture rendering",
-- and this is that check: the pane samples a surface texture and the meshes
-- behind it sample their own, so a frame where the reflection is right and the
-- meshes are not — or the other way round — says which of the two paths broke.
local pane = Instance.new("Part")
pane.Name = "Mirror"
pane.Anchored = true
pane.Size = Vector3.new(16, 9, 0.3)
pane.CFrame = CFrame.new(Vector3.new(0, 4.5, 9))
pane.Color = Color3.new(0.9, 0.95, 1.0)
pane.Parent = workspace

local reflection = Instance.new("SurfaceCamera")
reflection.Name = "MirrorCamera"
reflection.SurfaceSize = SURFACE_SIZE

-- `Front` is -Z, so a pane at +Z looks back at the origin. Parenting is what
-- makes it a mirror at all — `scene::AimSurfaceCameras` places it, fits its
-- lens to the pane and hands out the surface slot.
reflection.Face = Enum.NormalId.Front
reflection.Parent = pane

-- **The second mirror, and the one that proves the filter.** It is authored
-- exactly like the first except for one property: `TagFilter` names a tag, so
-- this surface draws only the parts carrying it. The built-ins in the row
-- behind are absent from its reflection and present in the other's, which is
-- what "a redirected pipeline for tagged objects" means when you can see it.
local redirected = Instance.new("Part")
redirected.Name = "ImportedOnlyMirror"
redirected.Anchored = true
redirected.Size = Vector3.new(7, 5, 0.3)
redirected.CFrame = CFrame.new(Vector3.new(-9, 2.5, 6))
redirected.Color = Color3.new(1.0, 0.95, 0.85)
redirected.Parent = workspace

local groupCamera = Instance.new("SurfaceCamera")
groupCamera.Name = "ImportedOnlyCamera"
groupCamera.SurfaceSize = SURFACE_SIZE
groupCamera.Face = Enum.NormalId.Front

-- One property, and it is the whole feature.
groupCamera.TagFilter = "Imported"
groupCamera.Parent = redirected

local view = Instance.new("Camera")
view.Name = "Eye"
view.CFrame = CFrame.lookAt(Vector3.new(0, 5, 18), Vector3.new(0, 2.5, 0))
view.Parent = workspace

-- **Parenting a camera is not aiming with it.** `ActiveCamera` names the live
-- one, so a camera that is only parented is a row nothing reads and the client
-- keeps orbiting its own placeholder — which is what was happening here, and it
-- looks like bad framing rather than a missing line.
workspace.CurrentCamera = view

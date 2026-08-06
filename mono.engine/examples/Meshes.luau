--!strict
-- Imported meshes, their textures, and a mirror in front of them.
--
-- `Mirrors-1-world.luau` proves the rendering path over boxes. This proves the
-- **content** path: every shape here is either a built-in the engine generates
-- or a mesh a publisher baked and a client fetched, and the pane in the middle
-- reflects both.
--
-- ## What has to line up for a mesh to appear
--
-- Four things, and each of them is a different module's:
--
--   1. `assetc` reads a `.glb`, a `.pmx` or an `.obj` and writes a `.amesh`
--      beside it — `bake::ReadModel` and `assets::Mesh`;
--   2. `cdn --publish` signs the tree and `client --cdn` fetches, verifies and
--      hands the bytes over — `delivery::AssetClient`;
--   3. the client registers each one in the renderer's mesh table under the
--      name the manifest carried;
--   4. this script sets `MeshPart.MeshId` to that same name.
--
-- **Step four is a string and the other three are machinery.** If the string
-- does not match what the manifest published, the part draws as a cube — which
-- is the deliberate fallback and is what you see when the pipeline is fine and
-- the name is wrong.
--
-- ## Run it
--
--     assetc --input ART --output content
--     cdn --publish content --store store --signing-key HEX
--     client --cdn dir:store --publisher-key PUBLIC --script Meshes.luau
--
-- With no `--cdn` at all it still runs: every `MeshId` resolves to nothing, and
-- the scene is the built-ins plus five cubes standing in for the imports. That
-- is the honest degraded state rather than an error, and it is worth seeing.

local PLATE = 24
local SURFACE_SIZE = Vector3.new(1024, 1024, 0)

-- The names a publisher gives these are the names this asks for. There is no
-- table anywhere that maps one to the other — the manifest's name *is* the
-- mesh's name, which is `AGENTS.md` rule 4 doing its job across a signature.
local IMPORTS = {
	{ mesh = "props/fox.amesh", texture = "", offset = -5 },
	{ mesh = "characters/xiao_pack/芭芭拉.amesh", texture = "", offset = 5 },
}

-- The six shapes the engine generates rather than fetches. They need no origin
-- and no publisher, which is what makes them the thing to check first when
-- nothing appears.
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

-- The built-ins, in a row, so a glance says whether the mesh table is working
-- at all. A sphere that draws as a cube means the name resolved to the
-- fallback; a sphere that draws as a sphere means every step below it is fine.
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

-- The imports. A `MeshPart` with no `TextureID` samples whatever texture each
-- of the mesh's own submeshes named at bake time — which is how a character
-- with twenty sheets works without this file knowing any of them.
for _, import in ipairs(IMPORTS) do
	local part = Instance.new("MeshPart")
	part.Name = import.mesh
	part.Anchored = true
	part.MeshId = import.mesh
	if import.texture ~= "" then
		part.TextureID = import.texture
	end

	-- Four metres, because `assetc --model-size` fitted every import into a
	-- four-metre box and a `MeshPart`'s size scales the unit mesh. A model
	-- imported at its authored scale would need this to be something else, and
	-- the two numbers agreeing is what makes a PMX character and a glTF fox the
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
reflection.Face = "Front"
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
groupCamera.Face = "Front"

-- One property, and it is the whole feature.
groupCamera.TagFilter = "Imported"
groupCamera.Parent = redirected

local view = Instance.new("Camera")
view.Name = "Eye"
view.CFrame = CFrame.new(Vector3.new(0, 5, 18))
view.Parent = workspace

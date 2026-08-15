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

-- **The imported half is discovered, not named.** This file used to hold two
-- string literals for meshes that only existed if somebody had baked that exact
-- tree - and on any other store both parts drew a cube, which is also what a
-- mesh still streaming in looks like. `ContentService:GetMeshes()` reports what
-- this world actually holds, so what stands on the plate is proof rather than a
-- hope. `MeshGrid.luau` carries the argument in full.
--
-- Two of them, because this scene is the *narrow* check - one mesh in front of
-- a mirror - and `MeshGrid.luau` is the wide one.
local IMPORT_SLOTS = 2

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
--
-- **Built as they arrive**, because delivery spans frames - the catalogue is
-- empty when this chunk runs. See the loop at the bottom of the file.
local imported: { [string]: MeshPart } = {}
local importedCount = 0

local function addImport(mesh: string, slot: number)
	local part = Instance.new("MeshPart")
	part.Name = mesh
	part.Anchored = true

	-- A name `ContentService` reported, so it is by construction a mesh this
	-- world holds.
	part.MeshId = mesh

	-- **No `TextureID`.** A `MeshPart` that names none samples whatever texture
	-- each of its own submeshes recorded at bake time, which is how a character
	-- with six sheets works without this file knowing any of them.

	-- Four metres, and metres only because the bake was `--model-size 1` - see
	-- the header. `assetc` fitted every import's longest axis to one and
	-- centred it, so this scales a unit mesh exactly as it would scale a
	-- built-in cube, which is what makes a PMX character and a glTF fox the
	-- same height.
	part.Size = Vector3.new(4, 4, 4)
	part.CFrame = CFrame.new(Vector3.new((slot - 1.5) * 10, 2, 0))
	part.Parent = workspace

	-- **Tagged, so the second mirror below has a group to redirect.** The tag
	-- is a name here and a bit in the component; `scene::TagTable` is where the
	-- two meet, and the draw loop only ever sees the bit.
	part:AddTag("Imported")

	imported[mesh] = part
end

-- **One part that asks for a sheet nobody published**, which is the live demo of
-- the other half of the texture story. The renderer has three answers and this
-- scene now shows all three side by side: the built-ins above name no texture
-- and draw as the default white plastic, the imports name sheets that arrive and
-- draw as themselves, and this one names something that will never come and
-- wears the purple checkerboard.
--
-- **The distinction it is here to prove is the timing one.** Until `D00107` the
-- imports wore that same checkerboard for the frames their sheets took to land,
-- so a load looked like a scene full of mistakes. The marker means *nothing is
-- coming* now, so this part is purple from the first frame and the imports never
-- are - and the difference is visible without reading a log.
--
-- It reuses a built-in mesh rather than importing one, because the point is the
-- texture and a mesh that also had to arrive would confuse which half is being
-- shown.
local absent = Instance.new("MeshPart")
absent.Name = "MissingTexture"
absent.Anchored = true
absent.MeshId = "engine.Cube"
absent.TextureID = "examples/there-is-no-such-sheet.atex"
absent.Size = Vector3.new(3, 3, 3)
absent.CFrame = CFrame.new(Vector3.new(0, 1.8, -14))
absent.Parent = workspace

-- **The mirror, in front of everything.** `ROADMAP.md` v0.9 asks in as many
-- words to "put in front mirrors to see if it works with texture rendering",
-- and this is that check: the pane samples a surface texture and the meshes
-- behind it sample their own, so a frame where the reflection is right and the
-- meshes are not - or the other way round - says which of the two paths broke.
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
-- makes it a mirror at all - `scene::AimSurfaceCameras` places it, fits its
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
-- keeps orbiting its own placeholder - which is what was happening here, and it
-- looks like bad framing rather than a missing line.
workspace.CurrentCamera = view

-- **The imported meshes appear as they arrive.** Content is fetched over
-- frames, so nothing published is in this world when the chunk above runs -
-- polling is the honest shape, because there is no callback a fetch could fire
-- without becoming visible mid-tick and rule 5 forbids exactly that.
local RunService = game:GetService("RunService")

local known = 0

RunService.Heartbeat:Connect(function()
	local meshes = ContentService:GetMeshes()
	if #meshes == known then
		return
	end
	known = #meshes

	for index, mesh in ipairs(meshes) do
		if importedCount >= IMPORT_SLOTS then
			break
		end
		if not imported[mesh] then
			importedCount += 1
			addImport(mesh, importedCount)
			print(`meshes: {mesh} - {ContentService:GetTriangleCount(mesh)} triangle(s)`)
		end
		-- `index` is unused beyond the ordering `GetMeshes` already applied;
		-- naming it keeps the loop reading as "in sorted order".
		local _ = index
	end

	if importedCount == 0 then
		print("meshes: nothing published in this world - run with --cdn to see imports")
	end
end)

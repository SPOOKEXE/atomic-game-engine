--!strict
-- A pyramid built one vertex and one triangle at a time, and edited live.
--
-- **What this proves that an imported mesh cannot.** `Meshes.luau` and
-- `MeshGrid.luau` are about content that arrived from a file; nothing in this
-- scene ever touches disk. `Instance.new("EditableMesh")` is created,
-- `AddVertex`/`AddTriangle` build the geometry, and `mesh.ContentId` is handed
-- straight to a `MeshPart` the same way a published mesh's asset name would
-- be - proving the two are interchangeable from the renderer's own point of
-- view.
--
-- **And it keeps editing after the part is already on screen.** The apex
-- rises and falls on `RunService.Heartbeat`, through the same `SetVertexPosition`
-- an author would call once. `client::EditableMeshUploader` re-converts and
-- re-uploads whenever `EditableMesh.Revision` moves, so this is the "real
-- time" half of the feature and not only the "creation" half.
--
-- ## Run it
--
--     client --script EditableMesh.luau

local RunService = game:GetService("RunService")

-- The base square, counter-clockwise from above - `v0, v1, v2, v3` traces
-- back-left, back-right, front-right, front-left. The apex sits over the
-- centre. Each side triangle winds `(next, current, apex)` rather than
-- `(current, next, apex)`; both were checked against the base loop's own
-- winding by hand, because a pyramid is small enough to get wrong from
-- intuition alone and `render/AGENTS.md`'s rule shows up as an invisible
-- face rather than an error when it is.
local HALF = 1.5
local BASE_Y = 0.0

local mesh = Instance.new("EditableMesh")

local v0 = mesh:AddVertex(Vector3.new(-HALF, BASE_Y, -HALF))
local v1 = mesh:AddVertex(Vector3.new(HALF, BASE_Y, -HALF))
local v2 = mesh:AddVertex(Vector3.new(HALF, BASE_Y, HALF))
local v3 = mesh:AddVertex(Vector3.new(-HALF, BASE_Y, HALF))

-- Rebuilt every apex move - see the render step below. Kept as an upvalue so
-- both the construction below and the edit loop share one id.
local APEX_HEIGHT = 3.0
local apex = mesh:AddVertex(Vector3.new(0, APEX_HEIGHT, 0))

assert(v0 and v1 and v2 and v3 and apex, "AddVertex refused a vertex on a fresh EditableMesh")

-- The base, facing down and out.
mesh:AddTriangle(v0 :: number, v1 :: number, v2 :: number)
mesh:AddTriangle(v0 :: number, v2 :: number, v3 :: number)

-- The four sides, each `(next, current, apex)` around the loop.
mesh:AddTriangle(v1 :: number, v0 :: number, apex :: number)
mesh:AddTriangle(v2 :: number, v1 :: number, apex :: number)
mesh:AddTriangle(v3 :: number, v2 :: number, apex :: number)
mesh:AddTriangle(v0 :: number, v3 :: number, apex :: number)

-- A colour per vertex, so the pyramid reads as more than a grey wedge.
-- `assets::MeshVertex` has no colour of its own, so `client::BuildMeshData`
-- averages each *triangle's* three corners and gives every distinct result its
-- own submesh run - which is why the four sides here come out as four shades
-- rather than as one blended amber.
mesh:SetVertexColor(v0 :: number, Color3.fromRGB(214, 158, 74))
mesh:SetVertexColor(v1 :: number, Color3.fromRGB(196, 84, 72))
mesh:SetVertexColor(v2 :: number, Color3.fromRGB(214, 158, 74))
mesh:SetVertexColor(v3 :: number, Color3.fromRGB(196, 84, 72))
mesh:SetVertexColor(apex :: number, Color3.fromRGB(255, 214, 130))

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(24, 1, 24)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(118, 120, 126)
floor.Parent = workspace

local part = Instance.new("MeshPart")
part.Name = "Pyramid"
part.Anchored = true
part.Size = Vector3.new(HALF * 2, APEX_HEIGHT, HALF * 2)
part.Position = Vector3.new(0, 0, 0)

-- **The one line that makes an `EditableMesh` a thing this part draws.**
-- `MeshId` is Roblox's spelling and an ordinary `core::Name` property - the
-- renderer resolves it exactly as it resolves a published mesh's asset name,
-- with no second code path for "this one was built in Luau."
part.MeshId = mesh.ContentId
part.Parent = workspace

-- **Live editing, on the same door construction used.** No new API: an
-- author calling this once to fix a typo and this loop calling it sixty
-- times a second are the same function, and both are picked up by the next
-- upload because both move `EditableMesh.Revision`.
RunService.Heartbeat:Connect(function()
	local wave = APEX_HEIGHT + math.sin(elapsedTime() * 2.0) * 0.8
	mesh:SetVertexPosition(apex :: number, Vector3.new(0, wave, 0))
end)

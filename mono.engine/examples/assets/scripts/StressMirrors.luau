--!strict
-- A rotating mirror ball with one outward reflecting pane on every ico-sphere facet.
-- Coloured pillars and the floor give every facet surrounding geometry to reflect.
-- The world's surface budget covers every facet, including those revealed as the ball turns.
-- One bounce reflects the room without multiplying the work into mirror-in-mirror views.
--
-- Run with client --script StressMirrors.luau, or import it from Studio's example menu.
-- SUBDIVISIONS controls the facet count: 20, 80, 320, then 1,280. RADIUS scales the ball.
-- The default two subdivisions keep the circumscribed mirror plates small enough to overlap
-- at their seams while retaining a spherical silhouette.

local SUBDIVISIONS = 2
local RADIUS = 12

-- Zero mirrors every facet. A positive count limits cameras for a smaller stress scene.
local MIRROR_LIMIT = 0

-- The texture each mirror renders into. Square, because a facet is as tall as
-- it is wide, and 512 rather than 1024 because a ping-ponged pair at 1024 is
-- 4 MB of target and this scene asks for dozens of them at once. The renderer
-- scales each one down further by how much of the screen its pane covers, so
-- this is a ceiling rather than the size most of them get.
local SURFACE_SIZE = Vector3.new(512, 512, 0)

-- How high the ball hangs, measured to its centre, and how fast it turns.
local HEIGHT = RADIUS + 10
local SPIN = 0.35

local RunService = game:GetService("RunService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- **One level, and the header carries the whole argument.** A ball is the worst
-- shape there is for the automatic rule: every pane can see most of the others,
-- so the measurement says "deeper" at every level and the passes multiply.
workspace.SurfaceBounces = 1

-- --- the ico-sphere ----------------------------------------------------------
--
-- Built here rather than imported, because the roadmap asks for the two
-- parameters and a `.mesh` file has neither. The recipe is the ordinary one: an
-- icosahedron, each triangle split into four, every vertex pushed out onto the
-- sphere.

type Face = { number }

-- The golden ratio, which is what makes twelve points on a sphere regular.
local PHI = (1 + math.sqrt(5)) / 2

local vertices: { Vector3 } = {
	Vector3.new(-1, PHI, 0),
	Vector3.new(1, PHI, 0),
	Vector3.new(-1, -PHI, 0),
	Vector3.new(1, -PHI, 0),
	Vector3.new(0, -1, PHI),
	Vector3.new(0, 1, PHI),
	Vector3.new(0, -1, -PHI),
	Vector3.new(0, 1, -PHI),
	Vector3.new(PHI, 0, -1),
	Vector3.new(PHI, 0, 1),
	Vector3.new(-PHI, 0, -1),
	Vector3.new(-PHI, 0, 1),
}

local faces: { Face } = {
	{ 1, 12, 6 },
	{ 1, 6, 2 },
	{ 1, 2, 8 },
	{ 1, 8, 11 },
	{ 1, 11, 12 },
	{ 2, 6, 10 },
	{ 6, 12, 5 },
	{ 12, 11, 3 },
	{ 11, 8, 7 },
	{ 8, 2, 9 },
	{ 4, 10, 5 },
	{ 4, 5, 3 },
	{ 4, 3, 7 },
	{ 4, 7, 9 },
	{ 4, 9, 10 },
	{ 5, 10, 6 },
	{ 3, 5, 12 },
	{ 7, 3, 11 },
	{ 9, 7, 8 },
	{ 10, 9, 2 },
}

-- **Split, with the midpoints shared between the two faces that own each
-- edge.** Without the cache every subdivision makes two vertices where there is
-- one, and the ball is a shell of triangles that meet nowhere - which draws
-- correctly, seams and all, and quadruples the vertex count for nothing.
for _ = 1, SUBDIVISIONS do
	local split: { Face } = {}
	local shared: { [string]: number } = {}

	local function midpoint(a: number, b: number): number
		local key = if a < b then `{a}_{b}` else `{b}_{a}`
		local found = shared[key]
		if found ~= nil then
			return found
		end

		table.insert(vertices, (vertices[a] + vertices[b]) * 0.5)
		shared[key] = #vertices
		return #vertices
	end

	for _, face in faces do
		local ab = midpoint(face[1], face[2])
		local bc = midpoint(face[2], face[3])
		local ca = midpoint(face[3], face[1])

		-- The corner triangles keep their parent's winding by construction; the
		-- middle one is the three midpoints in the same order.
		table.insert(split, { face[1], ab, ca })
		table.insert(split, { face[2], bc, ab })
		table.insert(split, { face[3], ca, bc })
		table.insert(split, { ab, bc, ca })
	end

	faces = split
end

-- Pushed out onto the sphere. A midpoint of two unit vectors is *inside* the
-- sphere, so without this the ball is a subdivided icosahedron rather than a
-- rounder one - which is the whole difference a subdivision level buys.
for index, at in vertices do
	vertices[index] = at.Unit * RADIUS
end

workspace.MaxSurfaces = #faces

-- --- the core mesh -----------------------------------------------------------
--
-- **A solid ball behind the facets, and it is not decoration.** A shell of
-- tiles alone is lit from one side and open at every seam, so the far side's
-- tile backs show through and the thing reads as a cloud of plates. A solid
-- core is also what makes a reflection legible, because a reflection needs
-- something to be *on*.
--
-- It is also the second half of the subdivision knob. At three levels this is a
-- 1,280 triangle mesh built vertex by vertex in Luau and re-uploaded by
-- `engine::render::EditableMeshUploader`, which is a real exercise of the path
-- `EditableMesh.luau` demonstrates at five vertices.

local mesh = Instance.new("EditableMesh")
mesh.Parent = ReplicatedStorage

-- How far in the core sits, as a fraction of the radius.
--
-- **Derived rather than typed, because the number it has to stay under moves
-- with `SUBDIVISIONS`.** Each tile lies on its own triangle's *plane*, and that
-- plane is nearer the centre than the vertices are - a chord is inside its own
-- arc - so a core drawn at the full radius would stand proud of every facet.
-- How much nearer depends on how big the triangles are: the bare icosahedron's
-- faces sit at 0.795 of the radius and the numbers climb towards 1 with every
-- level. A constant fitted to one level was wrong at the others.
--
-- Two per cent under the closest face plane, which is comfortably more than a
-- tile's own half-thickness at every size this scene is usable at.
local innermost = RADIUS
for _, face in faces do
	local centre = (vertices[face[1]] + vertices[face[2]] + vertices[face[3]]) * (1 / 3)
	innermost = math.min(innermost, centre.Magnitude)
end

local CORE_SCALE = (innermost * 0.98) / RADIUS

local coreIds: { number } = {}
for _, at in vertices do
	local id = mesh:AddVertex(at * CORE_SCALE)
	table.insert(coreIds, id :: number)
end

-- **A dot product and a cross product written out, because `Vector3` has
-- neither.** The declared surface is X, Y, Z, `Magnitude` and `Unit` - see
-- `bindings/engine.d.luau` - so a winding check has to do its own arithmetic.
local function cross(a: Vector3, b: Vector3): Vector3
	return Vector3.new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X)
end

local function dot(a: Vector3, b: Vector3): number
	return a.X * b.X + a.Y * b.Y + a.Z * b.Z
end

-- **Each triangle checked against the outward direction rather than trusted.**
-- The base list above is the standard one and its winding is correct; the
-- failure if it were not is a mesh drawn inside out, which back-face culling
-- turns into an invisible ball rather than into an error. Six lines is a
-- cheaper way to be sure than a reader counting twenty triples by hand, and it
-- also covers a subdivision rule that flipped one of the four children.
for _, face in faces do
	local a = vertices[face[1]]
	local b = vertices[face[2]]
	local c = vertices[face[3]]

	if dot(cross(b - a, c - a), a + b + c) >= 0 then
		mesh:AddTriangle(coreIds[face[1]], coreIds[face[2]], coreIds[face[3]])
	else
		mesh:AddTriangle(coreIds[face[1]], coreIds[face[3]], coreIds[face[2]])
	end
end

for _, id in coreIds do
	mesh:SetVertexColor(id, Color3.fromRGB(24, 24, 28))
end

local core = Instance.new("MeshPart")
core.Name = "Core"
core.Anchored = true
core.Size = Vector3.new(RADIUS * 2, RADIUS * 2, RADIUS * 2) * CORE_SCALE
core.Position = Vector3.new(0, HEIGHT, 0)
core.Color = Color3.fromRGB(24, 24, 28)
core.MeshId = mesh.ContentId
core.Parent = workspace

-- --- the facets --------------------------------------------------------------
--
-- One tile per triangle, lying *on* that triangle's own plane and covering the
-- whole of it.
--
-- **The ball is filled rather than tiled with gaps, and that is a change of
-- kind.** These used to be squares 0.62 of an edge across, dropped at each
-- triangle's middle with whatever roll `CFrame.lookAt` chose for them. A square
-- is not a triangle, so they could not meet: the ball came out as loose plates
-- over a dark core with a hole at every seam, which is exactly what a mirror
-- ball must not look like - a reflection needs a continuous surface to be read
-- across, and half the ball being core is half the mirrors missing.
--
-- A `Part` is a box and cannot be a triangle either, so the answer is the other
-- one: each tile is the triangle's own *bounding box* in the triangle's own
-- plane. Every point of the face is under a mirror, adjacent tiles overlap
-- across the edge they share rather than leaving one open, and the overlap is
-- two plates crossing at the dihedral angle rather than two coplanar surfaces
-- fighting for a depth value.
--
-- **`Front` is -Z and a pane reflects along its own -Z**, which is what
-- `CFrame.lookAt(at, at + normal)` produces: the look direction is the frame's
-- -Z, so aiming it away from the centre points the reflecting face outward.
-- Getting this backwards gives a ball whose facets reflect its own inside,
-- which looks exactly like a ball whose facets reflect nothing.
--
-- **The roll is the face's own and no longer arbitrary.** `lookAt` picks a roll
-- from a default up vector, so two neighbouring tiles used to be turned to
-- unrelated angles - which is a second, quieter source of gaps, because two
-- squares that meet at one rotation do not meet at another. Passing this face's
-- own in-plane axis as `up` makes the box the one that was measured.

type Tile = { Body: Part, Local: CFrame }

local tiles: { Tile } = {}

-- The two lists `BulkMoveTo` takes. The bodies never change; the placements are
-- overwritten in place every frame.
local bodies: { Instance } = {}
local placements: { CFrame } = {}

-- Which facets get a camera, spread rather than taken from the front of the
-- list. Subdivision emits a base triangle's four children together, so the
-- first few faces of a subdivided ball are all children of the same few base
-- triangles - every camera on one cap and the rest of the ball plain.
--
-- At the default `MIRROR_LIMIT` of zero this is every face and the stride is
-- one; the spread only matters for the cheap version.
local wanted = if MIRROR_LIMIT <= 0 then #faces else math.min(MIRROR_LIMIT, #faces)
local stride = math.max(math.floor(#faces / wanted), 1)
local mirrored = 0

for index, face in faces do
	local a = vertices[face[1]]
	local b = vertices[face[2]]
	local c = vertices[face[3]]

	local centre = (a + b + c) * (1 / 3)

	-- **The face's own normal rather than the direction of its middle.** The
	-- two agree exactly only for an equilateral triangle, and a subdivided
	-- icosphere's are not: the four children of a base face come out in two
	-- shapes. Taking the middle's direction tilts a tile off its own plane by a
	-- degree or so, which is enough to open a seam along one edge.
	local normal = cross(b - a, c - a).Unit
	if dot(normal, centre) < 0 then
		normal = normal * -1
	end

	-- Two axes in that plane. The first is an edge, so the box is aligned to
	-- the triangle rather than to the world.
	local acrossX = (b - a).Unit
	local acrossY = cross(normal, acrossX)

	-- The triangle's extent along each of them, measured from the middle.
	local leastX, mostX = math.huge, -math.huge
	local leastY, mostY = math.huge, -math.huge
	for _, corner in { a, b, c } do
		local offset = corner - centre
		local alongX = dot(offset, acrossX)
		local alongY = dot(offset, acrossY)
		leastX = math.min(leastX, alongX)
		mostX = math.max(mostX, alongX)
		leastY = math.min(leastY, alongY)
		mostY = math.max(mostY, alongY)
	end

	-- Thin, because a plate that meets its neighbours at an angle shows its
	-- edge and a thick one shows a lot of it. Scaled from the facet so
	-- `RADIUS` and `SUBDIVISIONS` both carry the tiles without a second
	-- constant to keep in step.
	local edge = (b - a).Magnitude
	local thickness = math.max(edge * 0.04, 0.05)

	-- The box's own middle, which is not the triangle's - a triangle is not
	-- symmetric about its centroid - lifted half a thickness so the tile's
	-- inner face rests on the plane rather than straddling it.
	local at = centre + acrossX * ((leastX + mostX) / 2) + acrossY * ((leastY + mostY) / 2) + normal * (thickness / 2)

	local frame = CFrame.lookAt(at, at + normal, acrossY)

	local tile = Instance.new("Part")
	tile.Name = `Facet_{index}`
	tile.Anchored = true
	tile.Size = Vector3.new(mostX - leastX, mostY - leastY, thickness)
	tile.CFrame = CFrame.new(Vector3.new(0, HEIGHT, 0)) * frame
	tile.Color = Color3.new(0.88, 0.90, 0.94)
	tile.Parent = workspace

	if (index - 1) % stride == 0 and mirrored < wanted then
		local eye = Instance.new("SurfaceCamera")
		eye.Name = `FacetCamera_{index}`
		eye.SurfaceSize = SURFACE_SIZE
		eye.Face = Enum.NormalId.Front
		eye.Parent = tile
		mirrored += 1
	end

	table.insert(tiles, { Body = tile, Local = frame })
	bodies[#tiles] = tile
	placements[#tiles] = tile.CFrame
end

-- --- something to reflect ----------------------------------------------------
--
-- **A room with structure in it, and it is load-bearing.** A ball hanging over
-- an empty plane reflects an empty plane, so a mirror that works and one that
-- draws a flat tint are the same picture. Coloured pillars in a ring mean each
-- facet catches a recognisable slice of the room, and a facet showing the wrong
-- slice is visible by eye.

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(160, 1, 160)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5 + GROUND_LIFT, 0))
floor.Color = Color3.new(0.16, 0.17, 0.20)
floor.Parent = workspace

local PILLARS = 10
for index = 1, PILLARS do
	local angle = (index / PILLARS) * math.pi * 2
	local pillar = Instance.new("Part")
	pillar.Name = `Pillar_{index}`
	pillar.Anchored = true
	pillar.Size = Vector3.new(3, 6 + index * 1.6, 3)
	pillar.CFrame = CFrame.new(Vector3.new(math.cos(angle) * 42, (6 + index * 1.6) / 2, math.sin(angle) * 42))
	pillar.Color = Color3.new(0.15 + index * 0.08, 0.85 - index * 0.06, 0.35 + index * 0.05)
	pillar.Parent = workspace
end

print(string.format("stressmirrors: %d facets at subdivision %d, %d of them mirrors", #faces, SUBDIVISIONS, mirrored))

-- --- the camera --------------------------------------------------------------
--
-- Outside the pillar ring and level with the ball's middle, so the facets in
-- the frame are the ones facing the viewer rather than the ones facing the
-- floor. A camera looking up from underneath sees the half of the ball whose
-- mirrors are all reflecting the ground.

local starterPlayerScripts = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
assert(starterPlayerScripts ~= nil, "StarterPlayerScripts is missing")
local demoCamera = starterPlayerScripts:FindFirstChild("DemoCamera")
assert(demoCamera ~= nil, "DemoCamera is missing")
demoCamera:SetAttribute("Name", "Camera")
demoCamera:SetAttribute("FieldOfView", 60)
demoCamera:SetAttribute("FarPlaneZ", 600)
demoCamera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(0, HEIGHT + 4, 46), Vector3.new(0, HEIGHT, 0)))

-- --- the turn ----------------------------------------------------------------
--
-- **The ball turns, and that is what makes it a mirror test rather than a
-- picture of one.** A still mirror can be wrong in a way that looks deliberate;
-- a turning one sweeps each facet through the whole room, so a reflection that
-- is placed wrongly slides the wrong way and a facet that is not reflecting at
-- all stays the same grey while its neighbours change.

local elapsed = 0

RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta

	local ball = CFrame.new(Vector3.new(0, HEIGHT, 0)) * CFrame.fromOrientation(0, elapsed * SPIN, elapsed * SPIN * 0.4)

	core.CFrame = ball

	for at, tile in tiles do
		placements[at] = ball * tile.Local
	end

	-- **One call for all three hundred and twenty facets.** A per-facet
	-- `CFrame =` crosses the language boundary once per facet, and this scene
	-- is here to measure the *mirror* path rather than the property bindings -
	-- see `scene::BulkMoveTo`, which carries what the boundary costs.
	workspace:BulkMoveTo(bodies, placements)
end)

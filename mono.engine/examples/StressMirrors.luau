--!strict
-- A mirror ball: an ico-sphere whose every facet is a flat mirror.
--
-- **`ROADMAP.md` v0.17 asks for an ico-sphere mirror ball with subdivisions and
-- a radius, to stress the mirror path.** `MirrorCorridor.luau` and
-- `MirrorDepth.luau` are the two correctness scenes - one asks how deep the
-- chain resolves, the other asks whether visibility follows it - and both are
-- built out of two or three panes because that is what those questions need.
-- This asks the third question, which is what happens when a scene wants far
-- more mirrors than the engine has.
--
-- ## The answer, up front, because it is the finding rather than a caveat
--
-- **`scene::MAX_SURFACES` is sixteen.** A surface slot owns a ping-ponged
-- texture pair, `AimSurfaceCameras` hands them out in walk order, and a camera
-- that does not get one is aimed and renders nothing - its pane falls through to
-- the plain lit slab `opaque.frag` draws outside the projected rectangle. So a
-- ball of eighty facets does not give eighty reflections however it is authored;
-- sixteen of them reflect and the rest are grey.
--
-- This scene is therefore built the way the engine can actually draw it: a full
-- ico-sphere of facets, with `MIRRORS` of them - sixteen by default, spread
-- evenly over the ball rather than clustered - carrying a `SurfaceCamera`. The
-- rest are the same tiles without one, which is what a disco ball's unlit facets
-- look like anyway.
--
-- **Raising `MIRRORS` past sixteen is the stress**, and it is a deliberate knob
-- rather than an accident: the extra cameras are gathered, placed and reflected
-- every frame and reach no screen at all. That is the cost of a ceiling nobody
-- can see, and this is the scene to re-run if `MAX_SURFACES` is ever raised.
--
-- ## Why the depth is stated
--
-- `workspace.SurfaceBounces = 1`, and without it this scene is a hang rather
-- than a slow frame. Left alone the renderer measures the depth it reached and
-- goes one level deeper next frame while any pane at the bottom still had
-- somewhere to descend into - and on a ball *every* pane can see most of the
-- others, so it says yes at every level and climbs to `MAX_SURFACE_DEPTH`. The
-- cost of a level multiplies rather than adds: `Renderer.cpp` states it as
-- `panes x (panes - 1) ^ (levels - 1)`, which at sixteen panes is 16 passes at
-- one level, 240 at two and **3,600** at three.
--
-- One level is a mirror ball that reflects the room. Two is a mirror ball that
-- reflects the room and the other facets, at fifteen times the cost. Both are
-- worth looking at and only one of them is a default.
--
-- ## Run it
--
--     client --script StressMirrors.luau
--     client --script StressMirrors.luau --frames 120 --capture out.bmp
--     client --script StressMirrors.luau --surface-bounces 2
--
-- In the studio it is `World -> New Scene from Example...`. The ball turns, so
-- two captures of one build are not identical - `--frames N` fixes the frame and
-- makes them so.
--
-- ## The two knobs the roadmap names
--
-- `SUBDIVISIONS` and `RADIUS`.
--
-- Subdivisions are what an ico-sphere is parameterised by: 0 is the bare
-- icosahedron at 20 facets, 1 is 80, 2 is 320, 3 is 1,280. Every level
-- quadruples the facet count *and* the core mesh's triangles, so it is the knob
-- that stresses the `EditableMesh` upload as well as the mirrors.
--
-- The radius scales the whole ball, and the facets with it - a tile is sized
-- from its own edge rather than from a constant, so a ball of any size has
-- facets that meet without overlapping.

local SUBDIVISIONS = 1
local RADIUS = 12

-- How many facets get a camera. Sixteen is `scene::MAX_SURFACES` and is the
-- most that can reflect; anything above it is the stress described in the
-- header, and anything below it is a cheaper scene.
local MIRRORS = 16

-- The texture each mirror renders into. Square, because a facet is as tall as
-- it is wide, and 512 rather than 1024 because sixteen ping-ponged pairs at
-- 1024 is 64 MB of target for one ball.
local SURFACE_SIZE = Vector3.new(512, 512, 0)

-- How high the ball hangs, measured to its centre, and how fast it turns.
local HEIGHT = RADIUS + 10
local SPIN = 0.35

local RunService = game:GetService("RunService")

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

-- --- the core mesh -----------------------------------------------------------
--
-- **A dark ball behind the facets, and it is not decoration.** The tiles are
-- sized to sit inside their triangles with a gap, so a bare shell of them is
-- see-through: the far side's tile backs show through the near side's gaps and
-- the thing reads as a cloud rather than as a ball. A solid core is what makes
-- a reflection legible, because a reflection needs something to be *on*.
--
-- It is also the second half of the subdivision knob. At three levels this is a
-- 1,280 triangle mesh built vertex by vertex in Luau and re-uploaded by
-- `client::EditableMeshUploader`, which is a real exercise of the path
-- `EditableMesh.luau` demonstrates at five vertices.

local mesh = Instance.new("EditableMesh")

-- Slightly inside the facets, so the core cannot poke through a tile and
-- z-fight with it. Four per cent of the radius is comfortably more than a
-- tile's own half-thickness at every size this scene is usable at.
local CORE_SCALE = 0.96

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
-- One tile per triangle, lying on the sphere at that triangle's middle and
-- facing straight out of it.
--
-- **`Front` is -Z and a pane reflects along its own -Z**, which is what
-- `CFrame.lookAt(at, at + outward)` produces: the look direction is the frame's
-- -Z, so aiming it away from the centre points the reflecting face outward.
-- Getting this backwards gives a ball whose facets reflect its own inside,
-- which looks exactly like a ball whose facets reflect nothing.

type Tile = { Body: Part, Local: CFrame }

local tiles: { Tile } = {}

-- Which facets get a camera, spread rather than taken from the front of the
-- list. Subdivision emits a base triangle's four children together, so the
-- first sixteen faces of an eighty-face ball are four adjacent base triangles -
-- every mirror on one cap and the other five sixths grey.
local stride = math.max(math.floor(#faces / MIRRORS), 1)
local mirrored = 0

for index, face in faces do
	local a = vertices[face[1]]
	local b = vertices[face[2]]
	local c = vertices[face[3]]

	local centre = (a + b + c) * (1 / 3)
	local outward = centre.Unit

	-- Sized from this facet's own edge, so `RADIUS` and `SUBDIVISIONS` both
	-- scale the tiles without a second constant that has to be kept in step.
	-- 0.62 of the edge is a square that fits inside an equilateral triangle
	-- with a visible gap at every corner, which is what makes the ball read as
	-- tiles rather than as a faceted sphere.
	local edge = (b - a).Magnitude
	local width = edge * 0.62
	local thickness = math.max(edge * 0.06, 0.1)

	local at = outward * RADIUS
	local frame = CFrame.lookAt(at, at + outward)

	local tile = Instance.new("Part")
	tile.Name = `Facet_{index}`
	tile.Anchored = true
	tile.Size = Vector3.new(width, width, thickness)
	tile.CFrame = CFrame.new(Vector3.new(0, HEIGHT, 0)) * frame
	tile.Color = Color3.new(0.88, 0.90, 0.94)
	tile.Parent = workspace

	if (index - 1) % stride == 0 and mirrored < MIRRORS then
		local eye = Instance.new("SurfaceCamera")
		eye.Name = `FacetCamera_{index}`
		eye.SurfaceSize = SURFACE_SIZE
		eye.Face = Enum.NormalId.Front
		eye.Parent = tile
		mirrored += 1
	end

	table.insert(tiles, { Body = tile, Local = frame })
end

-- --- something to reflect ----------------------------------------------------
--
-- **A room with structure in it, and it is load-bearing.** A ball hanging over
-- an empty plane reflects an empty plane, so a mirror that works and one that
-- draws a flat tint are the same picture. Coloured pillars in a ring mean each
-- facet catches a recognisable slice of the room, and a facet showing the wrong
-- slice is visible by eye.

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(160, 1, 160)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
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

print(string.format(
	"stressmirrors: %d facets at subdivision %d, %d of them mirrors",
	#faces,
	SUBDIVISIONS,
	mirrored
))

-- --- the camera --------------------------------------------------------------
--
-- Outside the pillar ring and level with the ball's middle, so the facets in
-- the frame are the ones facing the viewer rather than the ones facing the
-- floor. A camera looking up from underneath sees the half of the ball whose
-- mirrors are all reflecting the ground.

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.FieldOfView = 60
camera.FarPlaneZ = 600
camera.CFrame = CFrame.lookAt(Vector3.new(0, HEIGHT + 4, 46), Vector3.new(0, HEIGHT, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

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

	local ball = CFrame.new(Vector3.new(0, HEIGHT, 0)) * CFrame.Angles(0, elapsed * SPIN, elapsed * SPIN * 0.4)

	core.CFrame = ball

	for _, tile in tiles do
		tile.Body.CFrame = ball * tile.Local
	end
end)

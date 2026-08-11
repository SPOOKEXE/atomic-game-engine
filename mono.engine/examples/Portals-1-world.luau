--!strict
-- Holes in walls that lead somewhere the wall does not.
--
-- `Mirrors-1-world.luau` is this scene's sibling and the two are worth reading
-- together, because **a portal here is a mirror with one property set**. Both
-- parent a camera to a face; both are placed by `scene::AimSurfaceCameras`, drawn
-- by the surface pass and projected onto the pane by `opaque.frag`. The only
-- difference is where the camera stands: a mirror reflects the eye through its
-- own plane, and a portal maps it through `destination · source⁻¹`.
--
-- That one matrix is the whole feature, and `docs/NON-EUCLIDEAN.md` is the
-- investigation that established it. There is no portal renderer, no second
-- pipeline and no exotic geometry — which is the point, and the reason the
-- investigation recommended building it this way rather than as a subsystem.
--
-- ## Why this looks impossible
--
-- **Nothing constrains the two panes to describe one space.** A destination
-- turned, moved or placed anywhere at all is still a legal pair, so the space
-- behind a hole need not be the space behind the wall. One pair below is an
-- ordinary adjacency and two of them contradict each other; none of the three
-- costs anything the others do not.
--
-- The rooms are placed genuinely far apart in world coordinates and the portals
-- do the lying. That is the cheap version and it is enough for corridors that
-- loop, rooms bigger on the inside, and every hallway trick the demo this was
-- filed against is famous for. Two regions literally interpenetrating would need
-- a physics filter this engine does not have — `SurfaceCamera.TagFilter` already
-- covers the rendering half if it ever grows one.
--
-- ## What this cannot do yet, said plainly
--
-- **You can look through these and not walk through them.** Traversal means
-- detecting the eye crossing the plane inside the rectangle and moving the body
-- with it, and there is no body: the character controller is `ROADMAP.md` v0.15.
-- `world::Postbox::Teleport` is a different thing — it moves a player between
-- *worlds*, not between two places in one.
--
-- **And a portal's image is one frame old**, because the surface pass draws with
-- last frame's textures — which is what breaks the cycle between a surface and
-- the scene it shows. On a mirror that is invisible. On a hole somebody walks
-- through it would be a seam on the frame they cross, and removing it means
-- rendering the portal chain inside the frame, deepest first. Both are recorded
-- in `D00112` as what remains.

local RunService = game:GetService("RunService")

-- Each room is built off one number so its walls cannot drift out of agreement
-- with the floor they stand on.
local ROOM = 40
local ROOM_HALF = ROOM / 2

local WALL_HEIGHT = 14
local WALL_THICKNESS = 0.5
local WALL_OFFSET = ROOM_HALF + WALL_THICKNESS / 2

-- **Far enough apart that no frustum reaches from one to another.** The whole
-- illusion is that the rooms are adjacent; placing them adjacent would make it a
-- window rather than a lie, and the first thing a viewer would notice is the far
-- room's lighting arriving early at the near room's edges.
local SEPARATION = 300

-- Wide rather than square, because a portal pane is twice as wide as it is tall
-- and a square texture spends half of itself on nothing. Since v0.8 each surface
-- index owns its own pair, so all of these declaring one size does not make them
-- share one — six holes is six targets, against the sixteen `MAX_SURFACES`
-- allows a world. Smaller than a mirror's 2048x512 for that reason: the scene
-- has six of these where `Mirrors-1-world.luau` has four, and the total is still
-- less than that scene spends.
local SURFACE_SIZE = Vector3.new(1024, 512)

type Room = {
	Name: string,
	Origin: Vector3,
	Floor: Color3,
	Accent: Color3,
}

-- **Three rooms, because two would not show the trick.** A pair of portals
-- between two rooms is a corridor; a third room reached from the same room by a
-- different hole is what makes "where does this go" a question the geometry
-- cannot answer.
local ROOMS: { Room } = {
	{
		Name = "Hall",
		Origin = Vector3.new(0, 0, 0),
		Floor = Color3.fromRGB(120, 120, 128),
		Accent = Color3.fromRGB(220, 90, 90),
	},
	{
		Name = "Garden",
		Origin = Vector3.new(SEPARATION, 0, 0),
		Floor = Color3.fromRGB(90, 130, 95),
		Accent = Color3.fromRGB(240, 220, 120),
	},
	{
		Name = "Vault",
		Origin = Vector3.new(0, 0, SEPARATION),
		Floor = Color3.fromRGB(80, 90, 130),
		Accent = Color3.fromRGB(120, 220, 240),
	},
}

local function build(room: Room): { [string]: Part }
	local floor = Instance.new("Part")
	floor.Name = room.Name .. "Floor"
	floor.Anchored = true
	floor.Size = Vector3.new(ROOM, 0.5, ROOM)
	floor.Position = room.Origin + Vector3.new(0, -0.25, 0)
	floor.Color = room.Floor
	floor.Parent = workspace

	-- A ceiling, so the far room reads as enclosed rather than as an island
	-- floating in the clear colour. Through a hole that difference is most of
	-- what makes it a place.
	local ceiling = Instance.new("Part")
	ceiling.Name = room.Name .. "Ceiling"
	ceiling.Anchored = true
	ceiling.Size = Vector3.new(ROOM, 0.5, ROOM)
	ceiling.Position = room.Origin + Vector3.new(0, WALL_HEIGHT, 0)
	ceiling.Color = room.Floor
	ceiling.Parent = workspace

	-- Something with a shape in the middle of each room, so a viewer can tell
	-- one room from another through a hole rather than by its floor colour.
	local marker = Instance.new("Part")
	marker.Name = room.Name .. "Marker"
	marker.Anchored = true
	marker.Size = Vector3.new(2.5, 7, 2.5)
	marker.Position = room.Origin + Vector3.new(0, 3.5, 0)
	marker.Color = room.Accent
	marker.Parent = workspace

	-- **Axis-aligned, so there is no rotation anywhere in this file.** `Face` is
	-- resolved in the part's local space and then rotated into the world, so an
	-- unrotated wall with the right face already points where it should: `Back`
	-- is +Z, `Front` is -Z, `Right` is +X, `Left` is -X.
	local walls: { [string]: Part } = {}

	local sides = {
		{ Key = "North", Offset = Vector3.new(0, 0, -WALL_OFFSET), Size = Vector3.new(ROOM, WALL_HEIGHT, WALL_THICKNESS) },
		{ Key = "South", Offset = Vector3.new(0, 0, WALL_OFFSET), Size = Vector3.new(ROOM, WALL_HEIGHT, WALL_THICKNESS) },
		{ Key = "East", Offset = Vector3.new(WALL_OFFSET, 0, 0), Size = Vector3.new(WALL_THICKNESS, WALL_HEIGHT, ROOM) },
		{ Key = "West", Offset = Vector3.new(-WALL_OFFSET, 0, 0), Size = Vector3.new(WALL_THICKNESS, WALL_HEIGHT, ROOM) },
	}

	for _, side in sides do
		local wall = Instance.new("Part")
		wall.Name = room.Name .. side.Key
		wall.Anchored = true
		wall.Size = side.Size
		wall.Position = room.Origin + side.Offset + Vector3.new(0, WALL_HEIGHT / 2, 0)
		wall.Color = Color3.new(1, 1, 1)

		-- Not a caster: a wall on every side would rake its own floor with
		-- shadow and bury everything this scene exists to show.
		wall.CastShadow = false
		wall.Parent = workspace

		walls[side.Key] = wall
	end

	return walls
end

local hall = build(ROOMS[1])
local garden = build(ROOMS[2])
local vault = build(ROOMS[3])

-- **A pair is two portals and not one**, because a hole you can only see through
-- from one side is a television. Each is authored independently and each names
-- the other, which is also what makes the pairing survive one of them being
-- deleted: the survivor falls back to being a mirror rather than to a blank.
--
-- **`Enum.NormalId` in the annotation, which is the spelling everywhere else in
-- this file.** An enum is `Enum.<set>.<member>` as a value, so it is
-- `Enum.<set>` as a type; `mono.tools/scriptcheck` registers the `Enum` prefix
-- and `just typecheck` resolves it. `docs/DEFERRED.md` D00031 is the one place
-- that does not — luau-lsp's standard platform reads the declaration file, which
-- can only declare the flat `Enum_NormalId`, so an editor squiggles a line the
-- build accepts.
local function link(pane: Part, face: Enum.NormalId, destination: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = SURFACE_SIZE
	portal.Face = face

	-- The whole feature, in one assignment.
	portal.Destination = destination

	-- Solid: what a hole shows is not "some of the wall behind it". A portal
	-- with an image transparency is a window with glass in, which is a different
	-- thing and also expressible.
	portal.ImageTransparency = 0
	portal.Parent = pane
end

-- **`Face` is resolved on the destination too, and that is the one authoring
-- rule a portal has.** `scene::AimSurfaceCameras` builds the far frame from
-- `NormalOf(Face)` applied to the *destination's* transform — not from any idea
-- of front the destination carries itself — and what the hole shows is whatever
-- that face points at.
--
-- So the destination must be a wall whose matching face points **into** the far
-- room. Aim one at a wall whose matching face points outward and nothing
-- complains: the pane renders, the camera is placed, the frustum fits, and the
-- picture is the empty space behind the far wall. That is the shape of mistake
-- this file exists to make impossible to repeat — it was made once here, and it
-- passes every check except looking at it.
--
-- Every room here is axis-aligned and unrotated, so "the matching face, pointing
-- inward" is just the wall on the same side of the far room: `Back` is the north
-- wall in both, `Left` is the east wall in both. Rotating a destination is the
-- other way to say it, and is what an arbitrary pair of panes would need.

-- **An ordinary corridor first**, so that the impossible one below is legible as
-- a departure rather than as the only thing on offer. The two north walls face
-- each other through the hole: walk north out of the hall and you are in the
-- garden, walk back and you are where you started, and a single garden placed
-- north of the hall would explain everything you saw.
link(hall.North, Enum.NormalId.Back, garden.North, "ToGarden")
link(garden.North, Enum.NormalId.Back, hall.North, "ToHall")

-- **And now two holes that cannot both be true.** The hall's east wall opens
-- onto the vault and so does its west wall — the same room, through opposite
-- sides of the one you are standing in. Either hole alone is an ordinary
-- adjacency; the pair puts the vault east and west of the hall at once, which no
-- arrangement of two rooms in one space allows.
--
-- It is also a corridor with no end. Go east through the hall's east wall and
-- you arrive in the vault; cross it and go through its west wall and you are
-- back in the hall, entering from the west, having never turned round. Two rooms
-- walked forever.
--
-- Nothing in the engine objects, and nothing had to be added for it. That is the
-- claim `docs/NON-EUCLIDEAN.md` was filed to test, and it is the reason these
-- two pairs are in the scene rather than one.
link(hall.East, Enum.NormalId.Left, vault.East, "ToVaultEast")
link(vault.East, Enum.NormalId.Left, hall.East, "ToHallEast")

link(hall.West, Enum.NormalId.Right, vault.West, "ToVaultWest")
link(vault.West, Enum.NormalId.Right, hall.West, "ToHallWest")

-- Deterministic placement, for `Rings.luau`'s reason: a shared generator makes
-- the scene depend on the order things happened to be created in.
local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, salt * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

local function range(index: number, salt: number, low: number, high: number): number
	return low + hashed(index, salt) * (high - low)
end

type Drifter = { Part: Part, Base: Vector3, Phase: number, Rate: number }
local drifters: { Drifter } = {}

-- Motion in every room, because a still portal reads as a painted mural. It is
-- the parallax of something moving on the far side that says the hole is a view
-- rather than a texture.
for index = 0, 17 do
	local room = ROOMS[(index % 3) + 1]

	local part = Instance.new("Part")
	part.Name = "Drifter"
	part.Anchored = true

	local edge = range(index, 31, 0.9, 2.2)
	part.Size = Vector3.new(edge, edge, edge)
	part.Color = room.Accent

	local base = room.Origin
		+ Vector3.new(range(index, 3, -12, 12), range(index, 5, 1.5, 9), range(index, 7, -12, 12))

	part.Position = base
	part.Parent = workspace

	table.insert(drifters, {
		Part = part,
		Base = base,
		Phase = range(index, 11, 0, 6.283185),
		Rate = range(index, 13, 0.4, 1.2),
	})
end

-- Where the scene is watched from. **A position only** — every portal is placed
-- from wherever this camera is, every frame, so moving it moves all six images
-- with it rather than leaving them correct for one spot.
--
-- In the hall, looking north down the ordinary corridor, with the two holes that
-- contradict each other to left and right: the first thing anybody should see is
-- a hole that behaves, and the second is that the room through one side wall is
-- the room through the other.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.new(Vector3.new(0, 6, 12))
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, drifter in drifters do
		drifter.Phase += drifter.Rate * deltaTime
		drifter.Part.Position = drifter.Base + Vector3.new(0, math.sin(drifter.Phase) * 1.6, 0)
		drifter.Part.CFrame *= CFrame.Angles(0, drifter.Rate * deltaTime, 0)
	end
end)

print(
	`portals: 3 rooms {SEPARATION} apart, 6 portals — one corridor that closes and two that cannot both be true`
)

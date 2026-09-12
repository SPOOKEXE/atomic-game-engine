--!strict
-- A square building with four quarters and three rooms in it.
--
-- **The shape is the whole demonstration, so it is worth stating before any
-- code.** From outside this is an ordinary block: eighty metres square, one
-- door in the west wall, four quarters' worth of building. Inside there are
-- three rooms - hall, library, garden - laid out clockwise around the middle,
-- and the fourth quarter is walled off with no way in.
--
-- The way round is a loop. Out of the hall through its east doorway into the
-- library, out of the library through its south doorway into the garden, then
-- **straight through the garden's west wall and back into the hall**, which is
-- where the fourth quarter should have been. Three rooms, three right turns,
-- and you are where you started facing the way you set off.
--
-- A square needs four. That missing quarter-turn is the point: the middle of
-- this building is a cone point with ninety degrees of angle missing, and
-- walking a lap around it turns you a quarter more than you turned yourself.
-- Nothing in the engine knows that. It is two `Portal` instances.
--
-- ## What a portal is here
--
-- `Mirrors-1-world.luau` is this scene's sibling and the two are worth reading
-- together, because **a portal is a mirror with one property set**. Both parent
-- a camera to a face; both are placed by `scene::AimSurfaceCameras`, drawn by
-- the surface pass and projected onto the pane by `opaque.frag`. The only
-- difference is where the camera stands: a mirror reflects the eye through its
-- own plane, and a portal maps it through `destination · half-turn · source⁻¹`.
--
-- That one matrix is the whole feature, and `NON-EUCLIDEAN.md` is the
-- investigation that established it. There is no portal renderer, no second
-- pipeline and no exotic geometry.
--
-- ## Why the pair below is a corner and not a corridor
--
-- The two panes are the two inner walls that bound the empty quarter: the
-- hall's south wall, which runs east-west, and the garden's west wall, which
-- runs north-south. They are **perpendicular to each other**, so the map
-- between them carries a quarter turn - walk west out of the garden and you
-- come out walking north into the hall, having turned nothing yourself.
--
-- That is where the deficit lives. An earlier version of this scene put three
-- rooms three hundred metres apart and paired parallel walls, which is an
-- impossible *adjacency* - two rooms through opposite walls of a third. This is
-- an impossible *angle*, it fits in one building, and you can walk it.
--
-- **The gluing is coherent**, which is the one thing that had to be got right
-- by hand: the end of each pane at the middle of the building maps to the other
-- pane's end at the middle. Get that backwards and the corner is a twist rather
-- than a cone - every check still passes and the room arrives mirrored.
--
-- ## The rule that catches the mistake this file used to make
--
-- **`Face` is resolved on the destination as well.** `scene::AimSurfaceCameras`
-- builds the far frame from `NormalOf(Face)` applied to the *destination's*
-- transform, so the destination must be a part whose matching face points into
-- the far room. Aim one at a wall whose matching face points outward and
-- nothing complains: the pane renders, the camera is placed, the frustum fits,
-- and the picture is the empty space behind the far wall.
--
-- Both portals here use `Right`, and the hall's south wall is therefore the one
-- part in this file with a rotation on it - turned a quarter so its own +X
-- points north, into the hall. The rotation is not decoration: it *is* the
-- corner, written where the engine reads it.
--
-- ## What you can and cannot do
--
-- **You can walk through these.** `scene::CrossPortals` moves a body through
-- the same matrix the camera goes through and `scene::OpenPortals` takes the
-- pane's collider out of the solver's way, so a character walks the loop -
-- press Play in the studio, or host this file with
-- `scripts/demos/run-local-server.sh`. The `SpawnLocation` below is inside the
-- door for exactly that.
--
-- The camera and a demo character at the bottom of this file both walk the lap:
-- in through the door, round the three rooms, through the hole, round again,
-- forever. Neither route performs its own teleport, so the traversal system is
-- what closes the lap.
--
-- **A portal's image is one frame old**, because the surface pass draws with
-- last frame's textures - which is what breaks the cycle between a surface and
-- the scene it shows. On a mirror that is invisible. Crossing one, it is a seam
-- on the frame you cross, and removing it means rendering the portal chain
-- inside the frame, deepest first. That is what remains of `D00112`.
--
-- ## The look
--
-- Checkers, out of parts, because this engine's textures come off a CDN and a
-- demo that needs `--cdn` to be legible is a demo that usually is not run. A
-- flat colour is also the worst possible surface to judge a portal by: no
-- pattern means no parallax, and no parallax means a hole and a painted mural
-- look the same. Eight hundred five-metre tiles is a rounding error against
-- `SkyGrid.luau` and it gives every surface something to slide against.

local RunService = game:GetService("RunService")
local Players = game:GetService("Players")

-- ## One grid, and everything measured off it
--
-- Every distance the layout is measured in is a multiple of `TILE`, so no wall
-- ends mid-tile and no doorway is a tile and a half wide. A scene laid out this
-- way cannot drift out of agreement with itself the way one with independent
-- constants can. The two thicknesses below are the exception and say why.
local TILE = 5
local ROOM = TILE * 8
local HALF = ROOM / 2

-- The building is two rooms across, so its outer wall is `ROOM` from the
-- middle and each room's centre is `HALF`.
local OUTER = ROOM

local WALL_HEIGHT = TILE * 3
local WALL_THICKNESS = 2

-- **A hole is thinner than a wall, and this is not a detail.** A pane's
-- rectangle is its *face*, and `scene::CrossPortals` fires when a body's step
-- changes side of that plane - so a two-metre pane has two metres of slab
-- between its faces that a body can stand in, with the near half of itself in
-- one room and the far half in another. That is what "standing in the middle of
-- the mirror" is, and no amount of arithmetic fixes it: there is simply room in
-- there.
--
-- A quarter of a metre is thinner than one tick of walking, so the state exists
-- and cannot be occupied. It also lines the two floors up at the seam - with a
-- thick pane the imaged floor starts a metre late, which is a step the eye
-- catches even when nothing else is wrong.
local PANE_THICKNESS = 0.25

-- Two tiles wide and two high, which leaves a lintel row above every opening.
local DOOR_HALF = TILE
local DOOR_HEIGHT = TILE * 2

local GROUND = 240
local GROUND_TILE = 20

-- Wide rather than square, because a pane is forty by fifteen and a square
-- texture would spend half of itself on nothing - the ratio is the pane's, so
-- a texel is square on the wall.
--
-- **Large, because there are two of them.** A hole is not a mirror glimpsed
-- across a room: you walk up to this one until it fills the screen, and at that
-- range the far room is magnified from whatever this says. Two targets against
-- the sixteen `MAX_SURFACES` allows is what pays for it - the six the old scene
-- had could not have afforded a quarter of this each.
local SURFACE_SIZE = Vector3.new(2048, 768)

type Checker = { A: Color3, B: Color3 }

local GRASS: Checker = { A = Color3.fromRGB(104, 176, 72), B = Color3.fromRGB(142, 204, 96) }
-- **Near white, because a wall in this engine is as bright as its albedo lets
-- it be.** The renderer's ambient term is a constant 0.26 and the sun is a
-- fixed direction, so an interior face turned away from it is lit by ambient
-- alone - a mid-grey wall comes out at a quarter of mid-grey, which is black
-- with a hint. The checker is what keeps it readable at that brightness.
local STONE: Checker = { A = Color3.fromRGB(242, 242, 246), B = Color3.fromRGB(178, 178, 186) }
local ROOF: Checker = { A = Color3.fromRGB(26, 26, 26), B = Color3.fromRGB(52, 52, 52) }

-- **A floor colour per room, and that is how you tell where you are.** Through
-- a hole the floor arrives before anything else does, so three rooms that
-- differ only in furniture are three rooms nobody can tell apart at a glance.
local BLUE: Checker = { A = Color3.fromRGB(0, 132, 214), B = Color3.fromRGB(122, 200, 240) }
local RED: Checker = { A = Color3.fromRGB(196, 20, 30), B = Color3.fromRGB(140, 14, 22) }
local LIME: Checker = { A = Color3.fromRGB(150, 220, 40), B = Color3.fromRGB(96, 184, 32) }

-- **White, and it is not a colour choice.** `opaque.frag` multiplies the
-- projected image by the pane's own colour - "tinted rather than replaced, so a
-- coloured mirror is possible and a white one is unchanged" - so a pane painted
-- wall-grey shows the far room at half brightness. It reads as the far room
-- being dimly lit, which is a scene bug that looks like a lighting bug.
local PANE = Color3.new(1, 1, 1)
local PAD = Color3.fromRGB(38, 114, 216)

type Room = {
	Name: string,
	Centre: Vector3,
	Floor: Checker,
	Accent: Color3,
}

-- Clockwise from the north-west, which is the order the walk visits them in.
-- The fourth quarter - south-west, behind the two panes - is not in this list
-- because it is not a room: it has outer walls so the building is a square from
-- outside, and no floor, no ceiling and no door, because nothing can ever be in
-- it to see them.
local HALL: Room = {
	Name = "Hall",
	Centre = Vector3.new(-HALF, 0, -HALF),
	Floor = BLUE,
	Accent = Color3.fromRGB(90, 190, 255),
}
-- **The library is the one through the doorway and the garden is the one
-- through the hole**, and their colours are that way round on purpose: the
-- ordinary adjacency is the quiet green one and the impossible one is the red
-- that catches the eye from the middle of the hall.
local LIBRARY: Room = {
	Name = "Library",
	Centre = Vector3.new(HALF, 0, -HALF),
	Floor = LIME,
	Accent = Color3.fromRGB(180, 255, 90),
}
local GARDEN: Room = {
	Name = "Garden",
	Centre = Vector3.new(HALF, 0, HALF),
	Floor = RED,
	Accent = Color3.fromRGB(255, 110, 90),
}

local ROOMS: { Room } = { HALL, LIBRARY, GARDEN }

-- **Anchored and shadowless, all of it.** Anchored because scenery the solver
-- may move is scenery that ends up somewhere else after a character has leaned
-- on it, and an anchored part carries no rigid body at all. Shadowless because
-- a room with four walls and a lid would otherwise rake its own floor with
-- shadow and bury everything this scene exists to show.
local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.CastShadow = false
	part.Size = size
	part.Position = at
	part.Color = colour
	part.Parent = workspace
	return part
end

local function shadeOf(shade: Checker, index: number): Color3
	return if index % 2 == 0 then shade.A else shade.B
end

-- A square of tiles lying flat, centred on `at`, used for every floor, every
-- ceiling and the grass. `keepOut` drops the tiles whose centre is inside a
-- centred square of that half-width, which is how the lawn makes room for the
-- building rather than z-fighting its floors.
local function tileFloor(
	name: string,
	at: Vector3,
	extent: number,
	tile: number,
	thickness: number,
	shade: Checker,
	keepOut: number?
)
	local steps = math.floor(extent / tile)

	for row = 0, steps - 1 do
		for column = 0, steps - 1 do
			local offset = Vector3.new(
				(column + 0.5) * tile - extent / 2,
				0,
				(row + 0.5) * tile - extent / 2
			)

			if keepOut ~= nil and math.abs(offset.X) < keepOut and math.abs(offset.Z) < keepOut then
				continue
			end

			block(name, Vector3.new(tile, thickness, tile), at + offset, shadeOf(shade, row + column))
		end
	end
end

-- One room-length of wall, tiled, standing on the floor at `at` and running
-- `ROOM` metres along `axis`. `door` cuts the two-by-two opening out of the
-- middle - every doorway in this scene is in the middle of its wall, because
-- the walk goes room centre to room centre and a door anywhere else would put a
-- kink in it.
local function tileWall(name: string, at: Vector3, axis: Vector3, tile: Vector3, shade: Checker, door: boolean)
	local columns = math.floor(ROOM / TILE)
	local rows = math.floor(WALL_HEIGHT / TILE)

	for column = 0, columns - 1 do
		local along = (column + 0.5) * TILE - ROOM / 2

		for row = 0, rows - 1 do
			local height = (row + 0.5) * TILE

			if door and math.abs(along) < DOOR_HALF and height < DOOR_HEIGHT then
				continue
			end

			block(
				name,
				tile,
				at + axis * along + Vector3.new(0, height, 0),
				shadeOf(shade, column + row)
			)
		end
	end
end

local ALONG_X = Vector3.new(1, 0, 0)
local ALONG_Z = Vector3.new(0, 0, 1)

local WALL_X = Vector3.new(TILE, TILE, WALL_THICKNESS)
local WALL_Z = Vector3.new(WALL_THICKNESS, TILE, TILE)

-- --- the ground, and the three rooms that have one ------------------------

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

tileFloor("Ground", Vector3.new(0, -0.5 + GROUND_LIFT, 0), GROUND, GROUND_TILE, 1, GRASS, OUTER)

for _, room in ROOMS do
	tileFloor(room.Name .. "Floor", room.Centre + Vector3.new(0, -0.5 + GROUND_LIFT, 0), ROOM, TILE, 1, room.Floor)
	tileFloor(
		room.Name .. "Ceiling",
		room.Centre + Vector3.new(0, WALL_HEIGHT + 0.5, 0),
		ROOM,
		TILE,
		1,
		ROOF
	)

	-- **A lamp per room, and it is a pool of light rather than room lighting.**
	-- A world's `Lighting` service round-trips and nothing reads it -
	-- `scene::LightingServiceComponent` says so in as many words - so the only
	-- light a scene can actually add is a `PointLight`, and `opaque.frag`'s
	-- falloff is a windowed inverse square. Over a forty-metre room that cannot
	-- be flat: whatever brightness lights the far corner blows out the ceiling
	-- it hangs from. So this is authored as what it can be - a fitting with
	-- light under it, brightest at the middle of the room the walk turns in.
	--
	-- **A hundred and twenty is not a typo.** `Brightness = 1` against
	-- `1 / (1 + d²)` is invisible at any distance a room has; the number that
	-- reads as "a lamp" at ten metres is two orders up, and `Range` is only the
	-- cutoff the window closes at.
	local fitting = block(
		room.Name .. "Lamp",
		Vector3.new(TILE / 2, 0.5, TILE / 2),
		room.Centre + Vector3.new(0, WALL_HEIGHT - 1, 0),
		Color3.fromRGB(255, 250, 235)
	)
	fitting.CanCollide = false

	local bulb = Instance.new("PointLight")
	bulb.Name = "Bulb"
	bulb.Color = Color3.fromRGB(255, 248, 232)
	bulb.Brightness = 120
	bulb.Range = 60
	bulb.Parent = fitting
end

-- --- the outside, which is a plain square with one door in it -------------

tileWall("HallNorth", Vector3.new(-HALF, 0, -OUTER), ALONG_X, WALL_X, STONE, false)
tileWall("LibraryNorth", Vector3.new(HALF, 0, -OUTER), ALONG_X, WALL_X, STONE, false)
tileWall("LibraryEast", Vector3.new(OUTER, 0, -HALF), ALONG_Z, WALL_Z, STONE, false)
tileWall("GardenEast", Vector3.new(OUTER, 0, HALF), ALONG_Z, WALL_Z, STONE, false)
tileWall("GardenSouth", Vector3.new(HALF, 0, OUTER), ALONG_X, WALL_X, STONE, false)

-- The two that face the quarter nobody can reach. They are here so the
-- building is a square from the outside; there is nothing behind them.
tileWall("QuarterSouth", Vector3.new(-HALF, 0, OUTER), ALONG_X, WALL_X, STONE, false)
tileWall("QuarterWest", Vector3.new(-OUTER, 0, HALF), ALONG_Z, WALL_Z, STONE, false)

-- **The break, and it is the only way in.** In the middle of the hall's west
-- wall, on the line the walk runs down.
tileWall("HallWest", Vector3.new(-OUTER, 0, -HALF), ALONG_Z, WALL_Z, STONE, true)

-- --- the cross in the middle: two doorways and two holes ------------------
--
-- Four arms, one per quarter-boundary. The two that separate rooms are
-- ordinary walls with ordinary doors; the two that bound the empty quarter are
-- the pair, and they are what the fourth room would have been reached through.

tileWall("HallEast", Vector3.new(0, 0, -HALF), ALONG_Z, WALL_Z, STONE, true)
tileWall("LibrarySouth", Vector3.new(HALF, 0, 0), ALONG_X, WALL_X, STONE, true)

-- **One part each, not a course of tiles.** A pane is the rectangle the surface
-- camera is fitted to and the rectangle `CrossPortals` tests against, so a wall
-- built out of sixteen blocks would be sixteen holes in a row - sixteen
-- surfaces against a budget of sixteen, and fifteen seams down the middle of
-- the picture.
--
-- **Anchored and not collidable, which is a pair and not two settings.**
-- `scene::OpenPortals` turns a pane's collider into a trigger for exactly this
-- reason, but it is installed with the *character* systems: a host that never
-- registers them - the studio's edit view, a scene loaded by a tool, anything
-- looking at the world rather than simulating a body in it - leaves the pane
-- solid, and a solid pane is a wall a walker is parked on the face of.
-- Authoring it here is the same fact stated where it cannot be missed, and
-- anchored keeps it a wall rather than something the solver drops through the
-- floor once it stops colliding.
local function pane(name: string, at: Vector3): Part
	local part = block(name, Vector3.new(PANE_THICKNESS, WALL_HEIGHT, ROOM), at, PANE)
	part.CanCollide = false
	return part
end

local gardenWest = pane("GardenWest", Vector3.new(0, WALL_HEIGHT / 2, HALF))

-- **The one rotation in this file, and it is the corner itself.** Unrotated,
-- this slab's `Right` face would point east along the wall's own length, which
-- names no room at all. Turned a quarter, its +X points north into the hall -
-- and because `Face` is resolved on the destination too, that is what makes the
-- garden's west wall show the hall rather than the space behind it.
local hallSouth = pane("HallSouth", Vector3.new(-HALF, WALL_HEIGHT / 2, 0))
hallSouth.CFrame = CFrame.new(hallSouth.Position) * CFrame.Angles(0, math.pi / 2, 0)

-- **A pair is two portals and not one**, because a hole you can only see
-- through from one side is a television. Each is authored independently and
-- each names the other, which is also what makes the pairing survive one of
-- them being deleted: the survivor falls back to being a mirror rather than to
-- a blank pane.
--
-- **`Enum.NormalId` in the annotation, which is the spelling everywhere else in
-- this file.** An enum is `Enum.<set>.<member>` as a value, so it is
-- `Enum.<set>` as a type; `mono.tools/scriptcheck` registers the `Enum` prefix
-- and `just typecheck` resolves it. The editor resolves it too -
-- `docs/retired/DEFERRED.md` D00031 - so a line the build accepts is a line
-- luau-lsp accepts.
local function link(pane: Part, face: Enum.NormalId, destination: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = SURFACE_SIZE
	portal.Face = face

	-- The whole feature, in one assignment.
	portal.Destination = destination

	-- Solid: what a hole shows is not "some of the wall behind it". A portal
	-- with an image transparency is a window with glass in, which is a
	-- different thing and also expressible.
	portal.ImageTransparency = 0
	portal.Parent = pane
end

-- `Right` on both, because both panes have their own +X pointing into the room
-- they are seen from - the garden's west wall by being unrotated on the west
-- side of the garden, the hall's south wall by the quarter turn above.
link(gardenWest, Enum.NormalId.Right, hallSouth, "ToHall")
link(hallSouth, Enum.NormalId.Right, gardenWest, "ToGarden")

-- Where a character starts, just inside the door. The name is the contract:
-- `scene::FindSpawn` looks for exactly this under `Workspace` and a world
-- without one spawns everybody at the origin - which here is inside the wall
-- between two rooms.
block("SpawnLocation", Vector3.new(TILE * 2, 1, TILE * 2), Vector3.new(-ROOM + TILE * 2, 0.5, -HALF), PAD)

-- --- something moving in every room ---------------------------------------
--
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
--
-- **Kept out of the middle of each room**, because the middle is where the walk
-- turns and a drifter parked there is a cube the camera flies through.
for index = 0, 17 do
	local room = ROOMS[(index % 3) + 1]

	local part = Instance.new("Part")
	part.Name = "Drifter"
	part.Anchored = true

	local edge = range(index, 31, 1.2, 2.6)
	part.Size = Vector3.new(edge, edge, edge)
	part.Color = room.Accent

	local base = room.Centre
		+ Vector3.new(
			range(index, 3, 8, 16) * (if index % 2 == 0 then 1 else -1),
			range(index, 7, 3, 10),
			range(index, 5, 8, 16) * (if index % 4 < 2 then 1 else -1)
		)

	part.Position = base
	part.Parent = workspace

	table.insert(drifters, {
		Part = part,
		Base = base,
		Phase = range(index, 11, 0, 6.283185),
		Rate = range(index, 13, 0.4, 1.2),
	})
end

-- --- the lap, crossed by the engine ---------------------------------------
--
-- **A position only** - every portal is placed from wherever this camera is,
-- every frame, so moving it moves both images with it rather than leaving them
-- correct for one spot.
--
-- The legs below are the walk: in through the door, east through the hall,
-- south through the library, west across the garden into the pane, and out of
-- the hall's south wall heading north. The last two are one straight line to
-- whoever is walking it and a quarter turn in world coordinates, which is the
-- whole scene in two table entries.
--
-- The camera and the character below both walk continuously across the garden
-- pane. `scene::CrossPortals` supplies the quarter-turn and the destination;
-- no hand-written jump can hide a traversal regression.
local EAST = Vector3.new(1, 0, 0)
local SOUTH = Vector3.new(0, 0, 1)
local WEST = Vector3.new(-1, 0, 0)
local NORTH = Vector3.new(0, 0, -1)

local EYE = 6
local PACE = 9

type Route = { Stage: number }

local function heading(route: Route, at: Vector3): Vector3
	if route.Stage == 1 and at.X >= -HALF then
		route.Stage = 2
	elseif route.Stage == 2 and at.X >= HALF then
		route.Stage = 3
	elseif route.Stage == 3 and at.Z >= HALF then
		route.Stage = 4
	elseif route.Stage == 4 and at.X < -HALF / 2 then
		-- Only the portal can move the westbound traveller from the garden's
		-- non-negative X coordinates to the hall at X = -HALF.
		route.Stage = 5
	elseif route.Stage == 5 and at.Z <= -HALF then
		route.Stage = 2
	end

	if route.Stage == 1 or route.Stage == 2 then
		return EAST
	elseif route.Stage == 3 then
		return SOUTH
	elseif route.Stage == 4 then
		return WEST
	else
		return NORTH
	end
end

local view = Instance.new("Camera")
view.Name = "Viewer"
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view

view.CFrame = CFrame.lookAt(
	Vector3.new(-OUTER - 30, EYE, -HALF),
	Vector3.new(-OUTER - 29, EYE, -HALF)
)
local cameraRoute: Route = { Stage = 1 }

local walker = Instance.new("Player")
walker.Name = "PortalWalker"
walker.Parent = Players

local character = walker:LoadCharacter()
local root: BasePart? = nil
local humanoid: Humanoid? = nil
if character ~= nil then
	root = character:FindFirstChild("HumanoidRootPart") :: BasePart?
	humanoid = character:FindFirstChild("Humanoid") :: Humanoid?
	if root ~= nil then
		root.CFrame = CFrame.lookAt(
			Vector3.new(-OUTER - 22, 3, -HALF),
			Vector3.new(-OUTER, 3, -HALF)
		)
	end
	if humanoid ~= nil then
		humanoid.WalkSpeed = PACE
	end
end
local characterRoute: Route = { Stage = 1 }

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, drifter in drifters do
		drifter.Phase += drifter.Rate * deltaTime
		drifter.Part.Position = drifter.Base + Vector3.new(0, math.sin(drifter.Phase) * 1.6, 0)
		drifter.Part.CFrame *= CFrame.Angles(0, drifter.Rate * deltaTime, 0)
	end

	local cameraHeading = heading(cameraRoute, view.CFrame.Position)
	local eye = view.CFrame.Position + cameraHeading * PACE * deltaTime
	view.CFrame = CFrame.lookAt(eye, eye + cameraHeading)

	if root ~= nil and humanoid ~= nil then
		humanoid.MoveDirection = heading(characterRoute, root.Position)
	end
end)

print(
	`portals: one building, four quarters, three rooms - a lap of three right angles that closes`
)

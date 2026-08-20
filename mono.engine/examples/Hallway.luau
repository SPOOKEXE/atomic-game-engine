--!strict
-- Two corridors that are each other's insides.
--
-- **CodeParade's first level, which is the smallest scene that proves a portal
-- is a hole and not a picture.** Two straight corridors stand side by side on an
-- empty plain. One is thirty-two studs long and one is four. Each has a mouth at
-- either end, and the four mouths are paired across: the long corridor's north
-- mouth leads out of the short one's north mouth, and both south mouths likewise.
--
-- So there are two walks, and they are the same two sentences everybody uses to
-- describe non-Euclidean space:
--
--   * **Small outside, long inside.** Walk into the short corridor - a box you
--     can see both ends of at once - and come out thirty-two studs later at the
--     far end of the long one.
--   * **Long outside, small inside.** Walk into the long corridor, which you can
--     see stretching away, and come out of its far end after four studs.
--
-- Nothing here is scripted, animated or faked. It is four `Portal` instances and
-- some walls. `NON-EUCLIDEAN.md` is the investigation, the comparison against
-- the demo this is a port of, and the design of what a hole still owes.
--
-- ## The one authoring rule, and it is the only thing to get right
--
-- A pane's `Face` names its front, and the map through a hole carries **the
-- front hemisphere of one pane to the front hemisphere of the far one** - a
-- traveller who walks in from the side a pane faces arrives on the side the
-- destination faces. So the rule reads:
--
--   > point each pane's face at the space a traveller comes from, and point the
--   > destination's face at the space they should arrive in.
--
-- That is why the four mouths below are not all turned the same way. The long
-- corridor's mouths face **out**, because that is where you walk in from; the
-- short corridor's mouths face **in**, because inside it is where you arrive.
-- Turn one of them round and nothing complains - the pane renders, the camera is
-- placed, the frustum fits - and the hole shows the empty plain behind the far
-- mouth instead of the corridor. `PortalProbe.luau` records the same trap.
--
-- **`Face` is resolved on the destination too**, from the source portal's own
-- face id applied to the destination part's rotation. Every portal here uses
-- `Front`, so each mouth's rotation is the whole of its aim.

local WORLD = workspace

-- --- the shape ------------------------------------------------------------

local WIDTH = 6 -- the corridor's inside, across
local HEIGHT = 8 -- and up
local WALL = 1 -- how thick the shell is

-- **Eight times, which is enough that the eye cannot mistake one for the
-- other.** The illusion is a comparison, so the two lengths have to be
-- obviously different from outside.
local LONG = 32
local SHORT = 4

local LONG_X = -20
local SHORT_X = 20

local PANE_THICKNESS = 0.25

local SURFACE_SIZE = Vector3.new(1024, 1024)

local DARK = Color3.new(0.05, 0.05, 0.06)
local SHELL = Color3.new(0.16, 0.17, 0.2)
local FLOOR = Color3.new(0.28, 0.29, 0.33)
local PANE = Color3.new(0.5, 0.52, 0.58)
local LONG_MARK = Color3.new(1.0, 0.42, 0.12)
local SHORT_MARK = Color3.new(0.15, 0.75, 1.0)

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = CFrame.new(at)
	part.Color = colour
	part.Anchored = true
	part.Parent = WORLD
	return part
end

-- **Two lifts rather than one, because this scene stacks floors.** A world may
-- already hold a baseplate whose top face is exactly y = 0, so the ground below
-- clears that by a hundredth of a stud - and each corridor floor is laid inside
-- the ground's own footprint, so it has to clear the ground by the same again or
-- the two of them are the coplanar pair rather than the ground and the
-- baseplate. Coplanar surfaces do not flicker in this engine: the depth tie
-- resolves the same way every frame and one of them simply wins every pixel,
-- which is why this reads as the wrong floor rather than as a fight.
-- `scripts/demos/zfight-scan.sh` holds the control for it.
local GROUND_LIFT = 0.01

-- **How high a corridor's own floor sits above the plain outside it, and the
-- reason the whole shell moves rather than only the floor.** The panes fill the
-- cross-section exactly - that is what the shell build is for - so a floor
-- lifted on its own opens a crack along the bottom of every pane, which is the
-- lip this scene is arranged not to have.
local INSIDE = GROUND_LIFT * 2

-- The plain everything stands on. **Black**, so the only bright things in the
-- picture are the ones placed to be recognised.
block("Ground", Vector3.new(200, 1, 200), Vector3.new(0, -0.5 + GROUND_LIFT, 0), DARK)

-- A corridor: floor, ceiling and two side walls, open at both ends along Z.
--
-- **Built as a shell rather than as a box with the ends removed**, because the
-- mouths are where the panes go and a mouth with a lip on it is a mouth whose
-- pane does not fill it.
local function corridor(name: string, x: number, half: number)
	local span = half * 2

	block(name .. "Floor", Vector3.new(WIDTH + WALL * 2, WALL, span), Vector3.new(x, -WALL / 2 + INSIDE, 0), FLOOR)
	block(
		name .. "Ceiling",
		Vector3.new(WIDTH + WALL * 2, WALL, span),
		Vector3.new(x, HEIGHT + WALL / 2 + INSIDE, 0),
		SHELL
	)
	block(
		name .. "WestWall",
		Vector3.new(WALL, HEIGHT, span),
		Vector3.new(x - WIDTH / 2 - WALL / 2, HEIGHT / 2 + INSIDE, 0),
		SHELL
	)
	block(
		name .. "EastWall",
		Vector3.new(WALL, HEIGHT, span),
		Vector3.new(x + WIDTH / 2 + WALL / 2, HEIGHT / 2 + INSIDE, 0),
		SHELL
	)
end

corridor("Long", LONG_X, LONG / 2)
corridor("Short", SHORT_X, SHORT / 2)

-- --- the mouths -----------------------------------------------------------

-- A pane filling one end of a corridor.
--
-- **`CanCollide = false` authored here rather than left to `OpenPortals`.** A
-- pane is an ordinary part and an ordinary part is a wall, so anything that
-- loads this scene without running the portal systems - the studio's edit view,
-- a tool reading the world - would otherwise park a walker on the glass.
--
-- `turned` is the whole aim: `Front` is local -Z, so an unrotated mouth faces
-- -Z and a mouth turned half a lap faces +Z.
local function mouth(name: string, x: number, z: number, turned: boolean): Part
	local part = block(
		name,
		Vector3.new(WIDTH, HEIGHT, PANE_THICKNESS),
		Vector3.new(x, HEIGHT / 2 + INSIDE, z),
		PANE
	)
	part.CanCollide = false
	if turned then
		part.CFrame = CFrame.new(part.Position) * CFrame.Angles(0, math.pi, 0)
	end
	return part
end

-- **The long corridor's mouths face outwards**, at the plain a traveller walks
-- in from.
local longNorth = mouth("LongNorth", LONG_X, LONG / 2, true)
local longSouth = mouth("LongSouth", LONG_X, -LONG / 2, false)

-- **And the short corridor's face inwards**, at the space a traveller arrives
-- in. This is the asymmetry the header describes and the only subtle line in the
-- file.
local shortNorth = mouth("ShortNorth", SHORT_X, SHORT / 2, false)
local shortSouth = mouth("ShortSouth", SHORT_X, -SHORT / 2, true)

-- **A pair is two portals and not one**, because a hole you can only see through
-- from one side is a television. Each names the other, so deleting one leaves
-- the survivor a mirror rather than a blank pane.
local function link(pane: Part, destination: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = SURFACE_SIZE
	portal.Face = Enum.NormalId.Front
	portal.Destination = destination

	-- Solid: what a hole shows is not "some of the wall behind it".
	portal.ImageTransparency = 0
	portal.Parent = pane
end

link(longNorth, shortNorth, "LongNorthToShort")
link(shortNorth, longNorth, "ShortNorthToLong")
link(longSouth, shortSouth, "LongSouthToShort")
link(shortSouth, longSouth, "ShortSouthToLong")

-- --- something to recognise -----------------------------------------------
--
-- **One colour per corridor, repeated down its length.** Which corridor you are
-- standing in is the only fact either walk turns on, and counting stripes as
-- they go past is how you feel the length rather than infer it.
for index = 0, 7 do
	local z = -LONG / 2 + 2 + index * ((LONG - 4) / 7)
	block("LongStripe" .. index, Vector3.new(WIDTH, 0.3, 0.6), Vector3.new(LONG_X, 0.2, z), LONG_MARK)
end

for index = 0, 1 do
	local z = -SHORT / 2 + 1 + index * (SHORT - 2)
	block("ShortStripe" .. index, Vector3.new(WIDTH, 0.3, 0.6), Vector3.new(SHORT_X, 0.2, z), SHORT_MARK)
end

-- Posts outside each mouth, so the two buildings can be told apart from the
-- plain before you walk into either.
block("LongPost", Vector3.new(1, 6, 1), Vector3.new(LONG_X - 6, 3, LONG / 2 + 4), LONG_MARK)
block("ShortPost", Vector3.new(1, 6, 1), Vector3.new(SHORT_X - 6, 3, SHORT / 2 + 4), SHORT_MARK)

-- --- light ----------------------------------------------------------------
--
-- **A lamp is the only light a scene can add.** A world's `Lighting` service
-- round-trips and nothing reads it - `scene::LightingServiceComponent` says so
-- - so every lit thing here hangs under a `PointLight`, and `opaque.frag`'s
-- falloff is a windowed inverse square. `Brightness = 1` against `1 / (1 + d²)`
-- is invisible at any distance a corridor has; two orders up is what reads as a
-- lamp across a room.
local function lamp(name: string, at: Vector3, range: number)
	local fitting = block(name, Vector3.new(1, 0.4, 1), at, Color3.new(1, 0.98, 0.92))
	fitting.CanCollide = false

	local bulb = Instance.new("PointLight")
	bulb.Name = "Bulb"
	bulb.Color = Color3.fromRGB(255, 248, 232)
	bulb.Brightness = 160
	bulb.Range = range
	bulb.Parent = fitting
end

-- **Evenly down the long corridor and once in the short one**, so neither walk
-- ends in a dark stretch - a corridor you cannot see the end of is a corridor
-- whose length you cannot judge, which is the whole demonstration.
for index = 0, 3 do
	local z = -LONG / 2 + 4 + index * ((LONG - 8) / 3)
	lamp("LongLamp" .. index, Vector3.new(LONG_X, HEIGHT - 0.5, z), 40)
end

lamp("ShortLamp", Vector3.new(SHORT_X, HEIGHT - 0.5, 0), 40)

-- And two on the plain, so the outsides of the two buildings can be compared
-- before either is walked into.
lamp("PlainLampNorth", Vector3.new(0, 14, 20), 90)
lamp("PlainLampSouth", Vector3.new(0, 14, -20), 90)

-- --- where you start ------------------------------------------------------
--
-- Between the two, facing them both. The name is the contract: `scene::FindSpawn`
-- looks for exactly this under `Workspace`, and a world without one spawns
-- everybody at the origin - which here is inside neither corridor and on the
-- wrong side of both.
block("SpawnLocation", Vector3.new(8, 1, 8), Vector3.new(0, 0.5, 26), FLOOR)

-- --- a still, for when nobody is walking ----------------------------------
--
-- **Off by default, because this scene is meant to be walked.** A
-- `workspace.CurrentCamera` overrides the character's, so setting one
-- unconditionally would make the demo a video. `VIEW` below names a
-- viewpoint for a headless capture - which is how a change to the portal pass
-- gets looked at without a display.
local viewpoint: CFrame? = nil

-- **A line this file rewrites rather than a global.** `_G` is readonly in the
-- script sandbox, so nothing outside a scene can set one - the capture scripts
-- copy the file and substitute the line below, which is the only mechanism that
-- actually works from a shell.
local VIEW = "short"
local WHICH = VIEW

if WHICH == "long" then
	-- Outside the long corridor's north mouth, looking down it. What the hole
	-- shows is the short corridor's inside, so the far wall is four studs away
	-- through a doorway at the end of a thirty-two stud building.
	viewpoint = CFrame.new(Vector3.new(LONG_X, 4, LONG / 2 + 12))
elseif WHICH == "short" then
	-- Outside the short corridor's north mouth. The hole shows the long
	-- corridor's inside, stretching away out of a four-stud box.
	viewpoint = CFrame.new(Vector3.new(SHORT_X, 4, SHORT / 2 + 12))
elseif WHICH == "plain" then
	-- Both buildings at once, which is the comparison the walks turn on.
	viewpoint = CFrame.new(Vector3.new(0, 12, 60)) * CFrame.Angles(-0.15, 0, 0)
end

if viewpoint ~= nil then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = viewpoint
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end

print("hallway: long corridor at x=-20 is " .. LONG .. " studs, short at x=20 is " .. SHORT)

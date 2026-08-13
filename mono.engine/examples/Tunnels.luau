--!strict
-- Two tunnels whose insides are not as long as their outsides say.
--
-- **The same illusion as `Hallway.luau`, folded into single buildings.** The
-- hallway is two corridors that are each other's insides: you walk into one and
-- come out of the other. A *tunnel* is the stricter case — you walk in one end
-- of a building and out of the *same* building's far end — so the length has to
-- be added or taken away somewhere in the middle of the walk rather than at the
-- mouths. That is the shape of a doorway in a wall, which is what a portal is
-- usually drawn as, and it is the one shape the hallway does not cover.
--
-- So there are two tunnels on the plain, and two walks:
--
--   * **Long outside, short inside.** The west tunnel is thirty-two studs of
--     building. Crossing it takes four. Look in the north mouth and the daylight
--     at the other end is a few steps away, out of a building you can pace the
--     length of.
--   * **Short outside, long inside.** The east tunnel is a four-stud box you can
--     see both ends of at once. Crossing it takes twenty-eight, down a corridor
--     that will not fit in it.
--
-- ## The trick is that there is only one corridor's worth of space
--
-- **The west tunnel's middle is the east tunnel's inside.** Nobody ever walks
-- the west tunnel's middle — its own pair of panes skips over it — so those
-- twenty-six studs are furnished, lit, and standing empty. The east tunnel
-- splices them in. One building lends the space it does not use to the one that
-- cannot hold it, and no hidden third corridor is buried under the map.
--
-- Which means a walker in the small box is standing in the belly of the big one,
-- and a walker crossing the big one steps straight over their head without
-- either seeing the other. The stripes give it away: the east tunnel's inside is
-- painted in the east tunnel's colour, and it is a room inside the west
-- building.
--
-- ## Six panes in three pairs
--
-- Along Z, at the west tunnel's `x`:
--
--     ‑16    ‑14  ‑13                    +13  +14    +16
--      |      |    |                      |    |      |
--      mouth  skip splice     middle   splice  skip   mouth
--
-- The skip pair carries the west walk over the middle. The two splice panes are
-- the middle's own ends, and they lead to the east box's two panes, one stud
-- inside each of its mouths. The one-stud gap between a skip pane and a splice
-- pane is dead space nobody occupies; it is there so the two panes are not
-- coplanar, and so each hides the other's back.
--
-- ## The one authoring rule
--
-- `Hallway.luau` states it and this file obeys it without restating the
-- argument:
--
--   > point each pane's face at the space a traveller comes from, and point the
--   > destination's face at the space they should arrive in.
--
-- Every pair below is one pane facing +Z and its partner facing -Z, which is the
-- arrangement that carries a heading through unchanged. `Face` is `Front` on all
-- six, so a pane's rotation is the whole of its aim.

local WORLD = workspace
local RunService = game:GetService("RunService")

-- --- the shape ------------------------------------------------------------

local WIDTH = 6 -- a tunnel's inside, across
local HEIGHT = 8 -- and up
local WALL = 1 -- how thick the shell is

-- **Eight times, which is enough that the eye cannot mistake one for the
-- other.** Both illusions are comparisons against what the outside promised, so
-- the two buildings have to be obviously different lengths from the plain.
local LONG = 32
local SHORT = 4

-- Where the west tunnel's skip pair stands, either side of the middle. The
-- walked part of that tunnel is therefore `LONG / 2 - SKIP` studs at each end.
local SKIP = 14

-- And where the middle's own ends are — the two panes the east tunnel arrives
-- through. One stud inside the skip pair, so neither pane's back is ever the
-- nearest thing to an eye.
local SPLICE = 13

-- The east tunnel's pair, one stud inside each of its mouths.
local ENTRY = 1

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

-- The plain everything stands on. **Black**, so the only bright things in the
-- picture are the ones placed to be recognised.
block("Ground", Vector3.new(200, 1, 200), Vector3.new(0, -0.5, 0), DARK)

-- A tunnel: floor, ceiling and two side walls, open at both ends along Z.
--
-- **Built as a shell rather than as a box with the ends removed**, for
-- `Hallway.luau`'s reason: the mouths are where a pane could go and a mouth with
-- a lip on it is a mouth whose pane does not fill it. Here the mouths are left
-- open — both tunnels are walked *into*, not through a hole in a wall — but the
-- shell is still the right build, because the panes inside have to fill the
-- cross-section exactly and a lipped section would leave a crack round each.
local function tunnel(name: string, x: number, half: number)
	local span = half * 2

	block(name .. "Floor", Vector3.new(WIDTH + WALL * 2, WALL, span), Vector3.new(x, -WALL / 2, 0), FLOOR)
	block(
		name .. "Ceiling",
		Vector3.new(WIDTH + WALL * 2, WALL, span),
		Vector3.new(x, HEIGHT + WALL / 2, 0),
		SHELL
	)
	block(
		name .. "WestWall",
		Vector3.new(WALL, HEIGHT, span),
		Vector3.new(x - WIDTH / 2 - WALL / 2, HEIGHT / 2, 0),
		SHELL
	)
	block(
		name .. "EastWall",
		Vector3.new(WALL, HEIGHT, span),
		Vector3.new(x + WIDTH / 2 + WALL / 2, HEIGHT / 2, 0),
		SHELL
	)
end

tunnel("Long", LONG_X, LONG / 2)
tunnel("Short", SHORT_X, SHORT / 2)

-- --- the panes ------------------------------------------------------------

-- A pane filling a tunnel's cross-section at some point along it.
--
-- **`CanCollide = false` authored here rather than left to `OpenPortals`.** A
-- pane is an ordinary part and an ordinary part is a wall, so anything that
-- loads this scene without running the portal systems — the studio's edit view,
-- a tool reading the world — would otherwise park a walker on the glass, and
-- here that means bricking both tunnels rather than leaving a doorway shut.
--
-- `turned` is the whole aim: `Front` is local -Z, so an unrotated pane faces -Z
-- and a pane turned half a lap faces +Z.
local function pane(name: string, x: number, z: number, turned: boolean): Part
	local part = block(name, Vector3.new(WIDTH, HEIGHT, PANE_THICKNESS), Vector3.new(x, HEIGHT / 2, z), PANE)
	part.CanCollide = false
	if turned then
		part.CFrame = CFrame.new(part.Position) * CFrame.Angles(0, math.pi, 0)
	end
	return part
end

-- **The skip pair, which is the whole of "long outside, short inside".** The
-- north one faces the north stub, because that is where a southbound traveller
-- comes from; the south one faces the south stub, because that is where they
-- should arrive. Twenty-eight studs of building pass underneath the step.
local skipNorth = pane("LongSkipNorth", LONG_X, SKIP, true)
local skipSouth = pane("LongSkipSouth", LONG_X, -SKIP, false)

-- **The middle's own ends**, facing inwards at the middle, because the middle is
-- where a traveller out of the east tunnel arrives.
local spliceNorth = pane("MiddleNorth", LONG_X, SPLICE, false)
local spliceSouth = pane("MiddleSouth", LONG_X, -SPLICE, true)

-- **And the east tunnel's pair**, facing outwards at its two mouths, because
-- that is where its travellers come from. Between them is one stud of box and
-- twenty-six studs of somewhere else.
local entryNorth = pane("ShortNorth", SHORT_X, ENTRY, true)
local entrySouth = pane("ShortSouth", SHORT_X, -ENTRY, false)

-- **A pair is two portals and not one**, because a hole you can only see through
-- from one side is a television. Each names the other, so deleting one leaves
-- the survivor a mirror rather than a blank pane.
local function link(surface: Part, destination: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = SURFACE_SIZE
	portal.Face = Enum.NormalId.Front
	portal.Destination = destination

	-- Solid: what a hole shows is not "some of the wall behind it".
	portal.ImageTransparency = 0
	portal.Parent = surface
end

link(skipNorth, skipSouth, "SkipSouthward")
link(skipSouth, skipNorth, "SkipNorthward")

link(entryNorth, spliceNorth, "ShortNorthToMiddle")
link(spliceNorth, entryNorth, "MiddleToShortNorth")

link(entrySouth, spliceSouth, "ShortSouthToMiddle")
link(spliceSouth, entrySouth, "MiddleToShortSouth")

-- --- something to recognise -----------------------------------------------
--
-- **One colour per *space*, not per building.** Which tunnel a stripe belongs to
-- is the only fact either walk turns on, and the middle belongs to the east
-- tunnel however much it looks like it is inside the west one. Painting it in
-- the east tunnel's blue is the demonstration rather than a decoration: a walker
-- who steps into a four-stud box and finds twenty-six studs of blue floor has
-- found where the space came from.
local function stripe(name: string, x: number, z: number, colour: Color3)
	block(name, Vector3.new(WIDTH, 0.3, 0.6), Vector3.new(x, 0.2, z), colour)
end

-- The west tunnel's two stubs, which are all of it anybody walks.
stripe("LongNorthStripe", LONG_X, SKIP + 1, LONG_MARK)
stripe("LongSouthStripe", LONG_X, -SKIP - 1, LONG_MARK)

-- Its middle, in the east tunnel's colour, because that is whose inside it is.
for index = 0, 6 do
	local z = -SPLICE + 2 + index * ((SPLICE - 2) * 2 / 6)
	stripe("MiddleStripe" .. index, LONG_X, z, SHORT_MARK)
end

-- And the east box's own stud at either end.
stripe("ShortNorthStripe", SHORT_X, ENTRY + 0.5, SHORT_MARK)
stripe("ShortSouthStripe", SHORT_X, -ENTRY - 0.5, SHORT_MARK)

-- Posts outside each tunnel, so the two buildings can be told apart from the
-- plain before you walk into either.
block("LongPost", Vector3.new(1, 6, 1), Vector3.new(LONG_X - 6, 3, LONG / 2 + 4), LONG_MARK)
block("ShortPost", Vector3.new(1, 6, 1), Vector3.new(SHORT_X - 6, 3, SHORT / 2 + 4), SHORT_MARK)

-- --- light ----------------------------------------------------------------
--
-- **A lamp is the only light a scene can add.** A world's `Lighting` service
-- round-trips and nothing reads it — `scene::LightingServiceComponent` says so —
-- so every lit thing here hangs under a `PointLight`, and `opaque.frag`'s
-- falloff is a windowed inverse square. `Brightness = 1` against `1 / (1 + d²)`
-- is invisible at any distance a tunnel has; two orders up is what reads as a
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

-- One over each of the west tunnel's stubs, and three down the middle. A
-- corridor you cannot see the end of is a corridor whose length you cannot
-- judge, which is the whole demonstration.
lamp("LongNorthLamp", Vector3.new(LONG_X, HEIGHT - 0.5, SKIP + 1), 40)
lamp("LongSouthLamp", Vector3.new(LONG_X, HEIGHT - 0.5, -SKIP - 1), 40)

for index = 0, 2 do
	local z = -SPLICE + 3 + index * ((SPLICE - 3) * 2 / 2)
	lamp("MiddleLamp" .. index, Vector3.new(LONG_X, HEIGHT - 0.5, z), 44)
end

lamp("ShortLamp", Vector3.new(SHORT_X, HEIGHT - 0.5, 0), 40)

-- And two on the plain, so the outsides of the two buildings can be compared
-- before either is walked into.
lamp("PlainLampNorth", Vector3.new(0, 14, 22), 90)
lamp("PlainLampSouth", Vector3.new(0, 14, -22), 90)

-- --- the drifting blocks --------------------------------------------------
--
-- **A block that shuttles each tunnel, so the illusion is timed rather than
-- guessed at.** Standing still and looking through a hole tells you what the
-- hole shows; watching something cross tells you how long the crossing *took*,
-- and that is the fact both tunnels lie about. The west block enters a
-- thirty-two stud building and leaves the far end almost at once; the east block
-- vanishes into a four-stud box and is gone for the better part of five seconds.
--
-- **Script-driven rather than thrown, and the reason is a gap rather than a
-- preference.** `CrossPortals` carries "anything with a velocity, not only a
-- character" through a hole, so the engine would do this itself given a moving
-- body — but a script cannot author one. `Motion` is engine-declared and
-- therefore has no script-readable schema (`SetComponent` refuses it by design),
-- and `BasePart` exposes no velocity property, so the only body a scene can make
-- is an unanchored one, which falls. These blocks are anchored, `CanCollide`
-- false, and walk a route this file spells out.
--
-- What the route spells out is exactly what a seam would have done: every pair
-- here is one pane facing +Z and its partner facing -Z with no rotation between
-- them, so each hop is a pure translation and a leg boundary sits on the pane
-- plane. The cost of doing it by hand is the one frame of pop as the block
-- changes ends — a real crosser is drawn straddling the seam and this is not.

type Leg = {
	From: Vector3,
	To: Vector3,
	Direction: Vector3,
	Length: number,
}

local function leg(from: Vector3, to: Vector3): Leg
	local span = to - from
	return { From = from, To = to, Direction = span.Unit, Length = span.Magnitude }
end

local function routeLength(route: { Leg }): number
	local total = 0
	for index = 1, #route do
		total += route[index].Length
	end
	return total
end

-- Where a route is `distance` studs along it, the legs being consumed in order.
local function placeAlong(route: { Leg }, distance: number): Vector3
	for index = 1, #route do
		local part = route[index]
		if distance <= part.Length then
			return part.From + part.Direction * distance
		end
		distance -= part.Length
	end
	return route[#route].To
end

local DRIFT_SPEED = 6 -- studs a second, about a walk
local DRIFT_Y = HEIGHT / 2 -- eye height, so it passes in front of a watcher
local DRIFT_SIZE = Vector3.new(1.6, 1.6, 1.6)

-- **Ping-pong rather than a loop.** A block that restarted at the north end
-- would only ever show the southbound crossing, and half the point of a pair of
-- portals is that the walk works both ways.
local function drifter(name: string, colour: Color3, route: { Leg })
	local body = block(name, DRIFT_SIZE, route[1].From, colour)
	body.CanCollide = false

	local total = routeLength(route)
	local travelled = 0
	local heading = 1

	RunService.Heartbeat:Connect(function(deltaTime: number)
		travelled += DRIFT_SPEED * deltaTime * heading

		if travelled >= total then
			travelled = total
			heading = -1
		elseif travelled <= 0 then
			travelled = 0
			heading = 1
		end

		body.CFrame = CFrame.new(placeAlong(route, travelled))
	end)
end

-- The west walk: six studs of plain, two studs of tunnel, the skip, two more
-- studs of tunnel and out. Twenty studs of travel, four of them inside a
-- building thirty-two long.
drifter("LongDrifter", LONG_MARK, {
	leg(Vector3.new(LONG_X, DRIFT_Y, LONG / 2 + 6), Vector3.new(LONG_X, DRIFT_Y, SKIP)),
	leg(Vector3.new(LONG_X, DRIFT_Y, -SKIP), Vector3.new(LONG_X, DRIFT_Y, -(LONG / 2 + 6))),
})

-- The east walk: six studs of plain, one stud of box, twenty-six studs of the
-- *other* building's middle, one stud of box and out. Forty studs of travel,
-- twenty-eight of them inside a building four long.
drifter("ShortDrifter", SHORT_MARK, {
	leg(Vector3.new(SHORT_X, DRIFT_Y, SHORT / 2 + 6), Vector3.new(SHORT_X, DRIFT_Y, ENTRY)),
	leg(Vector3.new(LONG_X, DRIFT_Y, SPLICE), Vector3.new(LONG_X, DRIFT_Y, -SPLICE)),
	leg(Vector3.new(SHORT_X, DRIFT_Y, -ENTRY), Vector3.new(SHORT_X, DRIFT_Y, -(SHORT / 2 + 6))),
})

-- --- where you start ------------------------------------------------------
--
-- North of both, facing them. The name is the contract: `scene::FindSpawn` looks
-- for exactly this under `Workspace`, and a world without one spawns everybody
-- at the origin — which here is between the two tunnels and level with neither
-- mouth.
block("SpawnLocation", Vector3.new(8, 1, 8), Vector3.new(0, 0.5, 30), FLOOR)

-- --- a still, for when nobody is walking ----------------------------------
--
-- **Off by default, because this scene is meant to be walked.** A
-- `workspace.CurrentCamera` overrides the character's, so setting one
-- unconditionally would make the demo a video. `VIEW` below names a
-- viewpoint for a headless capture — which is how a change to the portal pass
-- gets looked at without a display.
-- **Unset means no camera at all**, which is the difference between this and
-- `Hallway.luau`: that file names a default still because it is mostly read as a
-- capture, and this one is a studio world somebody presses Play in. A
-- `CurrentCamera` left standing there would override the character's.
local viewpoint: CFrame? = nil

-- **A line this file rewrites rather than a global.** `_G` is readonly in the
-- script sandbox, so nothing outside a scene can set one — the capture scripts
-- copy the file and substitute the line below, which is the only mechanism that
-- actually works from a shell.
local VIEW = ""
local WHICH = if VIEW == "" then nil else VIEW

if WHICH == "long" then
	-- Outside the west tunnel's north mouth, looking down it. The far end is
	-- four studs away through a building thirty-two studs long.
	viewpoint = CFrame.new(Vector3.new(LONG_X, 4, LONG / 2 + 12))
elseif WHICH == "short" then
	-- Outside the east tunnel's north mouth. Twenty-six studs of corridor
	-- stretch away out of a four-stud box.
	viewpoint = CFrame.new(Vector3.new(SHORT_X, 4, SHORT / 2 + 12))
elseif WHICH == "plain" then
	-- Both buildings at once, which is the comparison the walks turn on.
	viewpoint = CFrame.new(Vector3.new(0, 12, 62)) * CFrame.Angles(-0.14, 0, 0)
end

if viewpoint ~= nil then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = viewpoint
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end

print(
	"tunnels: west is " .. LONG .. " studs of building and " .. (LONG - SKIP * 2) .. " of walk; "
		.. "east is " .. SHORT .. " studs of building and " .. (SPLICE * 2 + (ENTRY * 2)) .. " of walk"
)

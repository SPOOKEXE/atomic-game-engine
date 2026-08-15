--!strict
-- Four worlds, four view producers, one composited frame.
--
-- `Mirrors-1-world.luau` proves the *rendering* path inside one world - shadows,
-- surface cameras, panes that show them. This proves the **view** path between
-- worlds: four independent simulations, each publishing to its own
-- `world::ViewChannel`, drawn side by side by one compositor.
--
--     just run --script assets/examples/Mirrors-4-worlds.luau --worlds 4
--
-- ## One file, four worlds, and how it knows which it is
--
-- `--worlds N` creates N worlds and runs the *same* script in every one of
-- them. Each gets its own store, its own clock, its own runtime and its own view
-- channel; nothing is shared, which is what makes them separable at all.
--
-- So the script has to ask. `game.JobId` is the world's name - `client.world`,
-- `client.world.1`, and so on - and the trailing number is this scene's index.
-- Roblox's `JobId` identifies the server instance a script is standing on, and
-- a world here *is* that instance, so the mapping is the one `game` and
-- `workspace` already use rather than a new idea.
--
-- **Without it every world would build the same scene**, and that is precisely
-- the bug this demo is shaped to catch: four identical rooms in a row look
-- correct whatever order the compositor put them in.
--
-- ## Why four and not two
--
-- Two of anything is the count at which a placement bug still looks like
-- correct behaviour. Two rooms side by side are symmetric: swap them, mirror the
-- spacing, get the sign of the offset wrong, and the frame is still two rooms
-- side by side. Four with **distinct, ordered identities** cannot do that - the
-- palette below runs warm to cool left to right, so a wrong order reads
-- immediately as a wrong order rather than as a scene.
--
-- Each world also gets a different caster count, which is the second signal: a
-- compositor that drew one world's view twice would show two rooms with the same
-- population, and counting cubes is something a person can do from a screenshot.
--
-- ## What is *not* being proved here
--
-- **The worlds are parallel, not separate processes.** `--worlds N` runs them in
-- one process under `world::Universe`, dispatched through `parallel::Jobs`; the
-- multi-process arrangement is `mono.unified_server_client`'s and the
-- supervisor's. What this file proves is the part both share: a world publishes
-- a view and knows nothing about who composites it.
--
-- The rooms are deliberately small. `Mirrors-1-world.luau` is the scene to read
-- for what a mirror *is* - this one is four of a simpler thing, because what is
-- under test is the arrangement rather than the room.

local RunService = game:GetService("RunService")

-- --- which world this is ------------------------------------------------------

-- The trailing number of the world's name, or zero when it has none.
--
-- **Zero for a name with no suffix**, because the first world is called
-- `client.world` rather than `client.world.0` - see `Client::BuildDemoWorlds`,
-- which keeps the original name so nothing that referred to it has to learn a
-- new one. A script that assumed a suffix would fail on exactly one of the four.
local function worldIndex(): number
	local name = game.JobId
	local trailing = string.match(name, "%.(%d+)$")
	return trailing ~= nil and tonumber(trailing) or 0
end

local INDEX = worldIndex()

-- --- what makes each world tell itself apart ----------------------------------

-- **Warm to cool, left to right.** The compositor places world 0 leftmost, so
-- reading the frame left to right should read this table top to bottom. Any
-- other order on screen is a placement bug, and that is the whole reason the
-- four differ at all.
local PALETTES = {
	{ Name = "Ember", Wall = Color3.fromRGB(196, 84, 72), Casters = 6 },
	{ Name = "Amber", Wall = Color3.fromRGB(214, 158, 74), Casters = 9 },
	{ Name = "Moss", Wall = Color3.fromRGB(126, 178, 96), Casters = 12 },
	{ Name = "Slate", Wall = Color3.fromRGB(96, 152, 196), Casters = 15 },
}

-- Wraps rather than clamps, so `--worlds 6` still builds six distinguishable
-- rooms instead of three that look alike. Luau tables are one-based and the
-- index is zero-based, which is the one conversion in this file.
local PALETTE = PALETTES[(INDEX % #PALETTES) + 1]

-- --- the room -----------------------------------------------------------------

local PLATE = 40
local PLATE_HALF = PLATE / 2

local WALL_HEIGHT = 14
local WALL_THICKNESS = 0.4

-- Outside the plate rather than on its edge, so the floor runs to the glass
-- with no seam of background between them. `Mirrors-1-world.luau` gives the
-- arithmetic at length.
local WALL_OFFSET = PLATE_HALF + WALL_THICKNESS / 2

local CASTER_SPREAD = 9

-- Where the viewer stands in *this* world's coordinates. Every world uses the
-- same eye, and the compositor is what moves the views apart - which is the
-- property under test: a world does not know where its image will be drawn.
local EYE = Vector3.new(0, 6, 16)
local EYE_FOV = 60

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(PLATE, 0.5, PLATE)
floor.Position = Vector3.new(0, -0.25, 0)

-- A fraction of the wall colour, so the floor carries this world's identity too
-- without competing with the mirror for it.
floor.Color = Color3.new(
	0.15 + PALETTE.Wall.R * 0.15,
	0.15 + PALETTE.Wall.G * 0.15,
	0.15 + PALETTE.Wall.B * 0.15
)
floor.Parent = workspace

-- **One mirrored wall rather than four.** Four walls each reflecting the other
-- three is `Mirrors-1-world.luau`'s subject and costs four surface targets per
-- world - sixteen across this scene, which is a texture budget rather than a
-- demonstration. One north wall shows that a surface camera works inside a
-- world the compositor is also reading, which is the thing four worlds put in
-- doubt.
local pane = Instance.new("Part")
pane.Name = "Mirror"
pane.Anchored = true
pane.Position = Vector3.new(0, WALL_HEIGHT / 2, -WALL_OFFSET)
pane.Size = Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS)

-- The tint multiplies whatever the reflection carries, so this is what makes
-- one world's mirror legibly not another's.
pane.Color = PALETTE.Wall

-- Not a caster: a wall this size would rake the floor with its own shadow and
-- bury the cube shadows the scene exists to show.
pane.CastShadow = false
pane.Parent = workspace

local reflection = Instance.new("SurfaceCamera")
reflection.Name = "Reflection"

-- Half the single-world scene's resolution per axis, because there are four of
-- these on screen at once and each is a quarter of the width it would have had
-- alone. Sized to the pane's shape rather than square, for the reason
-- `Mirrors-1-world.luau` states.
reflection.SurfaceSize = Vector3.new(1024, 256)

-- `Back` is +Z on an unrotated part, which is the face pointing at the origin
-- for a wall on -Z. Nothing in this file rotates anything.
reflection.Face = Enum.NormalId.Back
reflection.ImageTransparency = 0
reflection.Parent = pane

-- --- the things it reflects ---------------------------------------------------

-- Deterministic placement, for the reason `Rings.luau` gives: a shared
-- generator makes the scene depend on the order things were created in, and two
-- runs of one recording must put the same cube in the same place.
--
-- **Salted with the world index**, so the four rooms are not the same room in
-- four colours. A viewer comparing two views should be able to see they are
-- different simulations rather than one image drawn twice.
local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, (salt + INDEX * 7919) * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

local function range(index: number, salt: number, low: number, high: number): number
	return low + hashed(index, salt) * (high - low)
end

type Caster = { Part: Part, Base: Vector3, Phase: number, Rate: number }
local casters: { Caster } = {}

for index = 0, PALETTE.Casters - 1 do
	local part = Instance.new("Part")
	part.Name = "Caster"
	part.Anchored = true

	local edge = range(index, 31, 0.8, 1.8)
	part.Size = Vector3.new(edge, edge, edge)

	-- Pulled toward this world's palette rather than fully random, so a cube
	-- belongs to a room visibly. Fully random colours in four rooms read as one
	-- population scattered across four boxes.
	part.Color = Color3.new(
		PALETTE.Wall.R * 0.5 + range(index, 19, 0.0, 0.5),
		PALETTE.Wall.G * 0.5 + range(index, 23, 0.0, 0.5),
		PALETTE.Wall.B * 0.5 + range(index, 29, 0.0, 0.5)
	)

	local base = Vector3.new(
		range(index, 3, -CASTER_SPREAD, CASTER_SPREAD),
		range(index, 5, 1.5, 6),
		range(index, 7, -CASTER_SPREAD, CASTER_SPREAD)
	)
	part.Position = base
	part.Parent = workspace

	table.insert(casters, {
		Part = part,
		Base = base,
		Phase = range(index, 11, 0, 6.283185),
		Rate = range(index, 13, 0.4, 1.3),
	})
end

-- --- the eye ------------------------------------------------------------------

-- The camera the scene is *watched* through, which is what the view channel
-- publishes from. Not a surface one: a world has several cameras and
-- `workspace.CurrentCamera` names the live one.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.new(EYE)
view.FieldOfView = EYE_FOV
view.Parent = workspace
workspace.CurrentCamera = view

-- --- what moves ---------------------------------------------------------------

-- **Each world runs at its own rate.** The bob is scaled by the world index, so
-- the four views are visibly out of step with each other - which is what proves
-- they are four simulations rather than one image drawn four times. Identical
-- motion in four boxes is the failure mode a still screenshot cannot rule out.
local RATE = 0.7 + INDEX * 0.25

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, caster in casters do
		caster.Phase += caster.Rate * RATE * deltaTime
		caster.Part.Position = caster.Base + Vector3.new(0, math.sin(caster.Phase) * 1.2, 0)
		caster.Part.CFrame *= CFrame.Angles(0, caster.Rate * RATE * deltaTime, 0)
	end
end)

print(`mirrors[{INDEX}] '{PALETTE.Name}': {#casters} casters, 1 mirrored wall, world '{game.JobId}'`)

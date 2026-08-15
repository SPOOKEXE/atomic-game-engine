--!strict
-- Ten thousand loose blocks stacked into a cube, dropped into a pit.
--
-- **The physics stress scene.** `Slide.luau` proves the four steps work; this
-- one asks what they cost when there are enough of them to matter. A slide
-- carries a few dozen bodies and is a correctness demo. This carries ten
-- thousand and is a budget: `physics.narrowphase` and `physics.solve` are the
-- two spans that dominate its frame, and it is the scene to open the frame
-- graph on before changing either.
--
-- ## Why a cube that collapses rather than a cube that stands
--
-- The blocks start on a lattice with a gap between them, so on the first tick
-- nothing touches anything - the broad phase reports almost no pairs and the
-- solver has no rows. Then they fall, meet each other and the floor, and settle
-- into a pile. One run therefore walks the whole cost curve: an empty narrow
-- phase, a burst as the lattice closes up, and a steady state where a deep
-- static pile is being held apart every tick.
--
-- That last state is the one worth measuring. A pile at rest is the worst case
-- for the solver - every block is in contact with several neighbours, none of
-- the contacts go away, and the impulses have to be found again every tick
-- until sleeping takes them. It is also the case a game actually ships.
--
-- ## The pit is what makes it dense
--
-- Dropped onto an open floor the blocks spread into a layer two or three deep
-- and the contact count falls off with them. Walls keep the footprint at the
-- cube's own width, so the pile lands about seven blocks deep and stays there.
-- Contact count is what this scene exists to produce, and depth is what
-- produces it.
--
-- ## What it is not
--
-- It is not a rendering test. Ten thousand cubes is a lot of draw calls and the
-- renderer will show up in the frame graph beside physics, but `SkyGrid.luau`
-- is the scene built to ask that question properly - this one puts its geometry
-- where the collisions are, not where the overdraw is.

-- **The count, and the lattice that holds it.** Twenty-two cubed is 10648, so
-- the walk below stops at ten thousand and leaves the top layer partly filled.
-- A partial layer is honest: rounding the count up to a perfect cube would make
-- the number in the comment disagree with the number in the scene.
local COUNT = 10000
local EDGE = 22

-- One stud per block, one and a half between centres. The gap is what keeps the
-- first tick free of contacts: a lattice built at exactly the block size starts
-- every neighbour touching, and ten thousand simultaneous contacts resolved on
-- frame one is a scene that explodes rather than one that falls.
local BLOCK = 1.0
local SPACING = 1.5

local SPAN = (EDGE - 1) * SPACING

-- How high the cube starts above the floor. Enough that it is clear of the
-- walls and visibly falls, short enough that it does not spend ten seconds in
-- free flight before the interesting part begins.
local DROP_HEIGHT = 6

-- The pit, sized to the cube's own footprint plus a stud of clearance on each
-- side. Any wider and the pile spreads and thins.
local PIT = SPAN + BLOCK * 2
local WALL_HEIGHT = 40
local WALL_THICKNESS = 2

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(PIT, 2, PIT)
floor.Position = Vector3.new(0, -1, 0)
floor.Color = Color3.fromRGB(118, 120, 126)
floor.Parent = workspace

-- Four walls, placed outside the footprint rather than on it, so the pit's
-- inside measures `PIT` across and a block resting against a wall is resting
-- where the arithmetic above says it is.
for index, side in { Vector3.new(1, 0, 0), Vector3.new(-1, 0, 0), Vector3.new(0, 0, 1), Vector3.new(0, 0, -1) } do
	local wall = Instance.new("Part")
	wall.Name = `Wall_{index}`
	wall.Anchored = true
	wall.Size = Vector3.new(
		side.X ~= 0 and WALL_THICKNESS or PIT + WALL_THICKNESS * 2,
		WALL_HEIGHT,
		side.Z ~= 0 and WALL_THICKNESS or PIT + WALL_THICKNESS * 2
	)
	wall.Position = side * ((PIT + WALL_THICKNESS) / 2) + Vector3.new(0, WALL_HEIGHT / 2, 0)
	wall.Color = Color3.fromRGB(96, 98, 104)
	wall.Parent = workspace
end

-- **A fixed palette walked by index rather than a colour per block.** The rule
-- `SkyGrid.luau` states and for the same reason: a random scene is a different
-- scene every run, and a frame time compared against a different scene is not a
-- comparison. Six shades is enough that neighbouring blocks differ, which is
-- what makes the pile's structure readable while it settles.
local PALETTE = {
	Color3.fromRGB(196, 84, 72),
	Color3.fromRGB(214, 158, 74),
	Color3.fromRGB(122, 172, 96),
	Color3.fromRGB(86, 146, 196),
	Color3.fromRGB(146, 108, 186),
	Color3.fromRGB(206, 206, 198),
}

-- The lattice corner, so the cube is centred over the pit horizontally and its
-- underside sits `DROP_HEIGHT` above the floor.
local ORIGIN = Vector3.new(-SPAN / 2, DROP_HEIGHT + BLOCK / 2, -SPAN / 2)

for index = 0, COUNT - 1 do
	-- Index unpacked into lattice coordinates, Y slowest, so the walk fills one
	-- layer before starting the next and the partial layer is the top one.
	local x = index % EDGE
	local z = math.floor(index / EDGE) % EDGE
	local y = math.floor(index / (EDGE * EDGE))

	local part = Instance.new("Part")
	part.Name = `Block_{index + 1}`

	-- **Unanchored, which is the whole point.** An anchored part carries no
	-- rigid body, so a cube of anchored blocks integrates nothing and solves
	-- nothing - it would be `SkyGrid.luau` with more of them.
	part.Anchored = false
	part.Size = Vector3.new(BLOCK, BLOCK, BLOCK)
	part.Position = ORIGIN + Vector3.new(x * SPACING, y * SPACING, z * SPACING)
	part.Color = PALETTE[(index % #PALETTE) + 1]
	part.Parent = workspace
end

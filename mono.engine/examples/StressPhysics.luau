--!strict
-- A hundred thousand loose blocks in a tray that never stops tilting.
--
-- **`ROADMAP.md` v0.17 asks for a hundred thousand parts *moving around*, and
-- the two words after the number are what make this a different scene from
-- `Cube.luau`.** That one drops ten thousand blocks into a tight pit and lets
-- them settle, because a deep pile at rest is the solver's worst case and the
-- one a game actually ships. It is a scene that ends: after a few seconds
-- nothing is moving and the frame time stops changing.
--
-- Ten times as many, kept in motion for as long as the window is open, is a
-- different measurement. Every body has a non-zero velocity every tick, so
-- nothing ever sleeps, the dynamic index is rebuilt from scratch every tick,
-- and the contact set is churning rather than steady. That is the case that
-- says what the broad phase costs when it can assume nothing from last tick.
--
-- ## How it keeps moving, and why it is five property writes
--
-- There is no per-block work here at all. The floor and its four walls are one
-- rigid tray, and the tick rewrites the tray's placement so it rocks a few
-- degrees about both horizontal axes. Gravity does the rest: the low corner
-- moves, so downhill moves, so a hundred thousand blocks slide towards it and
-- then slide back.
--
-- A script that walked the blocks itself would be measuring Luau. A script that
-- gave each one a shove would be measuring the property bindings. Five writes a
-- tick measures the thing the scene is named for and nothing else.
--
-- **The rock ramps in over the first eight seconds**, because the tray is
-- level while the lattice is still falling. Rocking during the fall throws the
-- blocks at a wall in one direction, which is a spectacular first ten seconds
-- and a scene whose steady state is a wedge against one side.
--
-- ## It needs a host that installs the physics pipeline
--
-- **`client --script` does not, and that is worth knowing before running it
-- rather than afterwards.** A client registers the *character* systems only -
-- `physics::RegisterCharacterSystems` - so a scripted client world integrates
-- nothing and solves nothing, and this scene opens as a hundred thousand blocks
-- hanging motionless in the air. That is not a bug in the scene.
--
-- The two hosts that do install it are the studio and the server:
--
--     just studio        # World -> New Scene from Example..., then Play
--     just host --game .cache/build/release/assets/examples/StressPhysics.luau \
--         --listen 9099 --physics-tick-rate 20
--
-- **`--physics-tick-rate` is the knob this scene exists to be pointed at.** A
-- world may solve slower than it ticks since v0.17, so the six steps run once
-- every few ticks while replication, scripts and rendering keep the tick rate. A
-- hundred thousand bodies is exactly the size of world that decision was added
-- for. In the studio it is the world's `PhysicsTickRate`; on a server it is the
-- flag above.
--
-- ## What it measured
--
-- `release` preset, 24-thread machine, headless server at 30 Hz, 600 ticks:
--
-- | Physics rate | Mean tick | p50 | p95 | Wall time |
-- |---|---|---|---|---|
-- | the tick rate (30) | 361 ms | 395 ms | 439 ms | 217 s |
-- | **10** | **125 ms** | **22 ms** | 432 ms | **80 s** |
--
-- **Read the p50 and the p95 together rather than the mean.** At a tenth of the
-- tick rate two ticks in three do no physics at all and cost 22 milliseconds;
-- the third does the whole of it and costs what it always did. The rate buys
-- total throughput, not a smoother frame - a world that wants both needs fewer
-- bodies, and lowering `COUNT` is how.
--
-- **A hundred thousand contacting bodies is past interactive on this machine
-- either way**, and saying so is the honest version of a stress scene: 361
-- milliseconds a tick is not a frame rate, it is a measurement. `Cube.luau` at
-- ten thousand is the one that runs at a playable speed.
--
-- ## The knobs
--
-- `COUNT`, and the lattice that holds it. 160 x 160 x 4 is 102,400, so the walk
-- stops at a hundred thousand and leaves the top layer partly filled - a
-- partial layer is honest, and rounding the count up to fill it would make the
-- number in this comment disagree with the number in the scene.
--
-- 10,000 is `Cube.luau`'s figure and is where a laptop should start. Lower
-- `EDGE` with it rather than only `COUNT`, or the tray stays 240 metres across
-- and the blocks spread into a layer one deep.

local COUNT = 100000
local EDGE = 160
local LAYERS = 4

-- One stud per block, one and a half between centres. The gap is what keeps the
-- first tick free of contacts: a lattice built at exactly the block size starts
-- every neighbour touching, and a hundred thousand simultaneous contacts
-- resolved on tick one is a scene that explodes rather than one that falls.
local BLOCK = 1.0
local SPACING = 1.5

local SPAN = (EDGE - 1) * SPACING

-- The tray's inside, which is the lattice footprint plus a stud of clearance on
-- each side. Wider and the pile thins to one block deep and the contact count
-- goes with it; narrower and the lattice starts inside a wall.
local TRAY = SPAN + BLOCK * 2
local FLOOR_THICKNESS = 2
local WALL_HEIGHT = 30
local WALL_THICKNESS = 2

-- How high the lattice starts above the floor. Enough to be visibly a fall,
-- short enough that it is not ten seconds of free flight before anything
-- interesting happens.
local DROP_HEIGHT = 8

-- **How far the tray leans, in radians, and how fast.** Six degrees over 240
-- metres lifts a corner by twelve, which is a slope a stack of blocks slides
-- down rather than merely leans on. Faster than about half a radian a second
-- and the tray moves further in a tick than a block can follow, which the
-- solver answers by shoving everything through everything.
local TILT = 0.105
local RATE = 0.35

-- How long the tray stays level while the lattice falls. See the header.
local SETTLE = 8

local RunService = game:GetService("RunService")

-- --- the tray ----------------------------------------------------------------
--
-- Five anchored parts and their placements *in the tray's own frame*, kept so
-- the tick can re-apply the lean to each without re-deriving where the piece
-- goes. An anchored part carries no rigid body, so these five are the whole of
-- the static index and rebuilding it every tick costs almost nothing - which is
-- the reason a moving floor is affordable at all.

type Piece = { Body: Part, Local: CFrame }

local pieces: { Piece } = {}

local function piece(name: string, size: Vector3, at: Vector3, colour: Color3)
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = size
	part.CFrame = CFrame.new(at)
	part.Color = colour
	part.Parent = workspace

	table.insert(pieces, { Body = part, Local = CFrame.new(at) })
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

piece(
	"Floor",
	Vector3.new(TRAY, FLOOR_THICKNESS, TRAY),
	Vector3.new(0, -FLOOR_THICKNESS / 2 + GROUND_LIFT, 0),
	Color3.fromRGB(118, 120, 126)
)

-- Four walls, placed outside the footprint rather than on it, so the tray's
-- inside measures `TRAY` across and a block resting against a wall is resting
-- where the arithmetic above says it is.
for index, side in { Vector3.new(1, 0, 0), Vector3.new(-1, 0, 0), Vector3.new(0, 0, 1), Vector3.new(0, 0, -1) } do
	piece(
		`Wall_{index}`,
		Vector3.new(
			side.X ~= 0 and WALL_THICKNESS or TRAY + WALL_THICKNESS * 2,
			WALL_HEIGHT,
			side.Z ~= 0 and WALL_THICKNESS or TRAY + WALL_THICKNESS * 2
		),
		side * ((TRAY + WALL_THICKNESS) / 2) + Vector3.new(0, WALL_HEIGHT / 2, 0),
		Color3.fromRGB(96, 98, 104)
	)
end

-- --- the blocks --------------------------------------------------------------
--
-- **A fixed palette walked by index rather than a colour per block.** A random
-- scene is a different scene every run, and a frame time compared against a
-- different scene is not a comparison. Six shades is enough that neighbours
-- differ, which is what makes the sliding readable: a uniform field of blocks
-- looks static even while all of it is moving.

local PALETTE = {
	Color3.fromRGB(196, 84, 72),
	Color3.fromRGB(214, 158, 74),
	Color3.fromRGB(122, 172, 96),
	Color3.fromRGB(86, 146, 196),
	Color3.fromRGB(146, 108, 186),
	Color3.fromRGB(206, 206, 198),
}

local ORIGIN = Vector3.new(-SPAN / 2, DROP_HEIGHT + BLOCK / 2, -SPAN / 2)

-- Y slowest, so the walk fills one layer before starting the next and the
-- partial layer is the top one.
local total = math.min(COUNT, EDGE * EDGE * LAYERS)

for index = 0, total - 1 do
	local x = index % EDGE
	local z = math.floor(index / EDGE) % EDGE
	local y = math.floor(index / (EDGE * EDGE))

	local part = Instance.new("Part")
	part.Name = `Block_{index + 1}`

	-- **Unanchored, which is the whole point.** An anchored part carries no
	-- rigid body, so a lattice of anchored blocks integrates nothing and solves
	-- nothing - it would be `SkyGrid.luau` with a hundred thousand of them.
	part.Anchored = false
	part.Size = Vector3.new(BLOCK, BLOCK, BLOCK)
	part.Position = ORIGIN + Vector3.new(x * SPACING, y * SPACING, z * SPACING)
	part.Color = PALETTE[(index % #PALETTE) + 1]
	part.Parent = workspace
end

print(string.format("stressphysics: %d blocks in a %.0f metre tray", total, TRAY))

-- --- the camera --------------------------------------------------------------
--
-- Outside the tray and above the rim, looking down the diagonal. Inside would
-- put the near wall across half the frame; straight down would flatten the
-- sliding into a texture that shimmers.

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.FieldOfView = 70
camera.FarPlaneZ = 2000
camera.CFrame = CFrame.lookAt(
	Vector3.new(TRAY * 0.75, TRAY * 0.45, TRAY * 0.75),
	Vector3.new(0, 0, 0)
)
camera.Parent = workspace
workspace.CurrentCamera = camera

-- --- the lean ----------------------------------------------------------------
--
-- The whole of the motion, and it is one rotation applied to five placements.
-- Two axes at different multiples of one rate, so the low corner walks round
-- the tray rather than rocking along one line - which is what keeps the pile
-- from finding a corner and staying in it.

local elapsed = 0

RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta

	-- Level while the lattice falls, then eased in. `math.min` rather than a
	-- branch, so there is no frame on which the tray jumps from one to the
	-- other.
	local ramp = math.min(elapsed / SETTLE, 1)
	local lean = TILT * ramp

	local tray = CFrame.Angles(math.sin(elapsed * RATE) * lean, 0, math.cos(elapsed * RATE * 0.7) * lean)

	for _, item in pieces do
		item.Body.CFrame = tray * item.Local
	end
end)

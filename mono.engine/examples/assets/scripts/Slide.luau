--!strict
-- A giant slide, and the first thing this engine ever simulated.
--
-- **The physics module was complete and connected to nothing.** `DEFERRED.md`
-- D00039: integrate, broad phase, narrow phase and solver had suites and
-- benchmarks and no consumer - `RegisterPhysicsSystems` was called from
-- `physics/tests/` and nowhere else. Every world this engine shipped was
-- anchored throughout, which is why nobody noticed: an anchored part carries no
-- rigid body at all, so a scene of anchored geometry integrates nothing and
-- solves nothing.
--
-- This world is the one that does. Blocks spawn at the top, slide down a curved
-- ramp, launch off the lip and are cleaned up ten seconds later.
--
-- ## Why a slide rather than a stack
--
-- A stack tests the solver and nothing else. A slide asks for all four steps at
-- once and each one fails visibly:
--
--   * **integrate** - a block that does not accelerate down the curve is not
--     being stepped at all;
--   * **narrow phase** - a contact normal that is subtly wrong shows here before
--     anywhere else, as a block that sinks into the ramp or skates across it;
--   * **broad phase** - blocks bunch up on the curve and spread out after the
--     launch, so the grid is asked for a burst and then for almost nothing;
--   * **solver** - blocks arriving behind one another have to queue rather than
--     tunnel through each other.
--
-- And the failure is legible without instrumentation. A slide that works looks
-- like a slide; one that does not looks like nothing else.
--
-- ## The curve is segments, not a curve
--
-- There is no mesh collider and no heightfield - `physics::ShapeKind` is boxes
-- and spheres - so the ramp is a run of overlapping boxes each rotated to the
-- local tangent. Overlapping rather than abutting, because a seam between two
-- boxes is a lip a block catches on, and a block that stops halfway down reads
-- as the solver being wrong when it is the geometry.
--
-- ## Why the blocks are destroyed
--
-- A spawner with no reaper is a world that grows without bound: at one block a
-- second this scene would hold six hundred after ten minutes, and the broad
-- phase would be measuring a demo's memory leak rather than a slide. Ten seconds
-- is comfortably longer than the run takes, so nothing vanishes on screen.

local RunService = game:GetService("RunService")

-- Where the slide starts and how far it falls. The curve is a quarter turn from
-- vertical to horizontal, which is what makes the launch flat rather than upward.
local TOP = Vector3.new(-26, 30, 0)
local RADIUS = 26

-- How many boxes the ramp is made of. Enough that the facets are shorter than a
-- block is wide - below that a block bridges two segments and rocks between
-- them, which looks like jitter and is geometry.
local SEGMENTS = 24

local RAMP_WIDTH = 10
local RAMP_THICKNESS = 2

-- How long a block lives after it spawns, and how often one arrives.
local BLOCK_LIFETIME = 10
local SPAWN_EVERY = 0.45

-- The floor, so a launched block lands rather than falling for ever.
-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local function SetFriction(part: Part, friction: number)
	part.CustomPhysicalProperties = true
	part.Friction = friction
end

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(140, 2, 60)
floor.Position = Vector3.new(28, -1 + GROUND_LIFT, 0)
floor.Color = Color3.fromRGB(118, 120, 126)
floor.Parent = workspace

-- **The curve, as a run of overlapping boxes.** The angle sweeps a quarter turn,
-- so the ramp leaves the top vertical and arrives at the bottom horizontal - the
-- shape in the sketch this was built from.
-- Parented straight to the workspace, like every other example here: a `Folder`
-- would only be tidier in the explorer and this scene names its parts clearly
-- enough to find them.
for index = 0, SEGMENTS - 1 do
	-- **A quarter circle centred level with the top and a radius to its right.**
	-- Sweeping from half a turn to three quarters puts the start at the top-left
	-- and the end directly below the centre, which is the shape in the sketch:
	-- near-vertical where a block arrives and flat where it leaves.
	--
	-- Halfway along the segment rather than at its start, so each box is centred
	-- on the piece of curve it stands in for.
	local t = (index + 0.5) / SEGMENTS
	local angle = math.pi + t * (math.pi / 2)

	local centre = Vector3.new(TOP.X + RADIUS, TOP.Y, 0)
	local position = centre + Vector3.new(math.cos(angle) * RADIUS, math.sin(angle) * RADIUS, 0)

	-- The tangent, which is what the box has to lie along. Differentiating the
	-- circle gives `(-sin, cos)`; its inclination is the rotation about Z, and
	-- it comes out vertical at the top and flat at the bottom without either
	-- being special-cased.
	local slope = math.atan2(math.cos(angle), -math.sin(angle))

	-- **Long enough to overlap its neighbour**, which is what removes the seams.
	-- One segment spans `RADIUS * (pi/2) / SEGMENTS` of arc; a third again
	-- covers the gap the rotation opens on the outside of the curve, and a seam
	-- is a lip a block catches on - which reads as the solver being wrong when
	-- it is the geometry.
	local span = (RADIUS * (math.pi / 2) / SEGMENTS) * 1.35

	local part = Instance.new("Part")
	part.Name = `Segment_{index + 1}`
	part.Anchored = true
	part.Size = Vector3.new(span, RAMP_THICKNESS, RAMP_WIDTH)
	part.CFrame = CFrame.new(position) * CFrame.Angles(0, 0, slope)

	-- A gradient down the slide, so which way it runs is readable in a still.
	local shade = 0.35 + t * 0.45
	part.Color = Color3.new(shade, shade * 0.35, shade * 0.32)
	SetFriction(part, 0.05)
	part.Parent = workspace
end

-- Low walls along both edges, so a block that arrives crooked stays on the ramp
-- instead of falling off the side and making the scene look broken.
for _, side in { -1, 1 } do
	for index = 0, SEGMENTS - 1 do
		local t = (index + 0.5) / SEGMENTS
		local angle = math.pi + t * (math.pi / 2)
		local slope = math.atan2(math.cos(angle), -math.sin(angle))

		local centre = Vector3.new(TOP.X + RADIUS, TOP.Y, 0)
		local along = centre + Vector3.new(math.cos(angle) * RADIUS, math.sin(angle) * RADIUS, 0)
		local span = (RADIUS * (math.pi / 2) / SEGMENTS) * 1.35

		local rail = Instance.new("Part")
		rail.Name = `Rail_{side < 0 and "L" or "R"}_{index + 1}`
		rail.Anchored = true
		rail.Size = Vector3.new(span, 2, 1)
		rail.CFrame = CFrame.new(along + Vector3.new(0, 1.4, side * (RAMP_WIDTH / 2)))
			* CFrame.Angles(0, 0, slope)
		rail.Color = Color3.fromRGB(150, 60, 55)
		SetFriction(rail, 0.05)
		rail.Parent = workspace
	end
end

-- Where a block is dropped in: just above the top of the ramp, so it starts by
-- falling a little and then meets the curve.
local SPAWN_AT = Vector3.new(TOP.X + 2.5, TOP.Y + 2, 0)

-- Deterministic jitter, so two runs of this world are the same world - the rule
-- `Rings.luau` states and the reason nothing here calls `math.random`.
local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, salt * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

type Rider = { Part: Part, Age: number }

-- Where the ramp ends, so "it left the slide" is a comparison rather than a
-- guess. The arc finishes a radius to the right of where it began.
local LAUNCH_X = TOP.X + RADIUS - 1

local riders: { Rider } = {}
local launched = false
local spawned = 0
local since = 0

local function spawnBlock()
	spawned += 1

	local part = Instance.new("Part")
	part.Name = `Block_{spawned}`

	-- **Unanchored, and that is the whole point of this world.** Setting this
	-- false is what gives a part a rigid body; leaving it true is what every
	-- other example here does, and is why none of them simulate.
	part.Anchored = false

	local edge = 1.4 + hashed(spawned, 31) * 1.0
	part.Size = Vector3.new(edge, edge, edge)

	-- Nudged across the ramp's width so blocks do not stack into one column and
	-- turn a slide into a tower.
	part.Position = SPAWN_AT + Vector3.new(0, 0, (hashed(spawned, 7) - 0.5) * (RAMP_WIDTH - 3))

	part.Color = Color3.fromRGB(
		140 + hashed(spawned, 19) * 60,
		90 + hashed(spawned, 23) * 45,
		55 + hashed(spawned, 29) * 35
	)
	SetFriction(part, 0.2)
	part.Parent = workspace

	table.insert(riders, { Part = part, Age = 0 })
end

RunService.Heartbeat:Connect(function(deltaTime: number)
	since += deltaTime
	if since >= SPAWN_EVERY then
		since -= SPAWN_EVERY
		spawnBlock()
	end

	-- **Said once, because "does the slide work" is otherwise a judgement about
	-- pixels.** A block that has left the ramp has been integrated, has been in
	-- contact with the curve, and has been solved against whatever was ahead of
	-- it - so one line naming where it got to is the whole simulation reporting
	-- itself. Once rather than per block: this is a demo, not a telemetry feed.
	if not launched then
		for _, rider in riders do
			if rider.Part.Position.X > LAUNCH_X and rider.Part.Position.Y < TOP.Y - RADIUS then
				launched = true
				print(
					`slide: a block launched - x {string.format("%.1f", rider.Part.Position.X)}, `
						.. `y {string.format("%.1f", rider.Part.Position.Y)}, `
						.. `after {string.format("%.1f", rider.Age)}s`
				)
				break
			end
		end
	end

	-- **Walked backwards, because this removes as it goes.** Removing from the
	-- front of a list being iterated forwards skips the element that slides into
	-- the gap, which here would be a block that never ages and never dies.
	for index = #riders, 1, -1 do
		local rider = riders[index]
		rider.Age += deltaTime

		if rider.Age >= BLOCK_LIFETIME then
			rider.Part:Destroy()
			table.remove(riders, index)
		end
	end
end)

print(`slide: {SEGMENTS} segments, a block every {SPAWN_EVERY}s living {BLOCK_LIFETIME}s - all unanchored`)

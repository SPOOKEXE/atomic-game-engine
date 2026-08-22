--!strict
-- Every authored emitter property, and then a hundred thousand of them.
--
-- **`ROADMAP.md` v0.17 asks for a full particle demo, and "full" is the word
-- that makes this a different scene from `Particles.luau`.** That one is a
-- *count*: it answers v0.10's hundred-thousand-emitter target and the flipbook
-- path, with a grid of identical emitters so that what the frame time measures
-- is the number of them and nothing about what any one is doing. An untouched
-- property can stop working without anything noticing, so this scene covers the
-- full declared surface including the resident force controls added at v0.20.
--
-- This is the other question. Every property `ParticleEmitter` declares gets a
-- labelled bay it is the only variable in, so a reader who knows what the
-- property means can see whether it does it - and then the same baseplate
-- carries the spam, so the feature matrix and the load are one scene rather
-- than two states nobody runs together.
--
-- ## Run it
--
--     client --script StressParticles.luau
--     client --script StressParticles.luau --stats        # F3 for the counters
--     client --script StressParticles.luau --frames 240 --uncapped
--
-- In the studio it is `World -> New Scene from Example...`, and it is a scene to
-- open with the frame graph rather than only with the frame time.
--
-- **`dev` is `-O0` and no frame-rate claim from it means anything.** The same
-- rule `Particles.luau` states: build `release` before quoting a number.
--
-- ## Read `render.particles`, not the frame time
--
-- The counter that says what is wrong is the live particle count. A scene that
-- is slow with the count where this file says it should be is a rendering
-- problem; a scene that is slow with the count somewhere else is an emitter
-- whose rate ran away, and the frame time cannot tell those apart.
--
-- `ParticleStatistics::EmittersRefused` is the other one, and it is the only
-- thing that distinguishes a starved scene from a fast one - an emitter that
-- never got a block from the pool is silent rather than wrong.
--
-- ## The knobs, and why the shape changed
--
-- **A thousand parts carrying five emitters each, not a hundred thousand parts
-- carrying one.** The old field was 320 by 320 invisible one-metre hosts with a
-- single emitter apiece, which measured one thing well - the per-emitter cost of
-- a refresh pass walking a hundred thousand rows - and measured it in a shape no
-- game has. A game has scenery, and a piece of scenery that emits usually emits
-- several things at once: a torch is flame and smoke and embers, a thruster is
-- exhaust and heat-haze and sparks. Five on one part is that, and it is also the
-- case the old field could not reach at all: every emitter under one `Transform`
-- and one `Bounds`, which is a different walk and a different cache pattern from
-- one emitter per host.
--
-- **The particle count is deliberately the same as before**, so the two shapes
-- are comparable rather than merely different. `HOSTS` of 32 is 1,024 parts;
-- five emitters each is 5,120; a rate of 20 over a five-second life is a hundred
-- alive apiece, and 5,120 times a hundred is 512,000 - the same load the 320
-- grid carried, against the same default pool of 524,288. What moved is where
-- the cost sits: twenty times fewer emitters to refresh, the same number of
-- particles to step and draw. A frame that got faster is a refresh-bound frame
-- and a frame that did not is a fill-bound one, and that is a question the old
-- scene could not ask.
--
-- Raise the pool before raising either knob, or the extra emitters are refused
-- and the count stops climbing while the cost of authoring them does not.
--
-- 16 is 256 parts and is where a machine without a discrete GPU should start.
--
-- ## What it measures on this machine
--
-- `release`, uncapped, 4,000 frames - long enough for a five-second life to
-- fill the pool, which 400 frames is not: **249.6 fps average with 1,039,274
-- triangles in the busiest frame**, which is 519,637 particles, and 60 Hz ticks
-- with none dropped. A short run reports a number several times higher and it
-- means nothing; check the triangle count reached about a million before
-- quoting anything.
--
-- `dev` runs the same scene at 4 fps, which is `-O0` and not a finding.

local HOSTS = 32

-- **Five, and each one points a different way.** Five emitters at one place
-- with the same settings is one emitter with five times the rate, which is not
-- what this is for: they differ by `EmissionDirection`, so a host sprays up,
-- down and along both horizontal axes and a reader can see all five. It is also
-- the only place in this repository where that property is exercised at scale.
local EMITTERS_PER_HOST = 5

-- Six metres between hosts, up from two. The old field was invisible
-- one-metre hosts packed tight because only the emitter count mattered; these
-- are parts somebody can see, spraying five ways, and at two metres the plumes
-- of neighbouring hosts are one fog.
local SPACING = 6

-- Where the feature bays sit relative to the spam, which is deliberately in
-- front of it rather than inside it. A bay surrounded by a hundred thousand
-- other emitters is a bay nobody can read.
local BAY_ROW_Z = -50
local BAY_SPACING = 14

local RunService = game:GetService("RunService")

-- --- the baseplate -----------------------------------------------------------

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local plate = Instance.new("Part")
plate.Name = "Baseplate"
plate.Anchored = true
plate.Size = Vector3.new(HOSTS * SPACING + 80, 1, HOSTS * SPACING + 80)
plate.CFrame = CFrame.new(Vector3.new(0, -0.5 + GROUND_LIFT, 0))
plate.Color = Color3.new(0.10, 0.11, 0.14)
plate.Parent = workspace

-- --- the feature bays --------------------------------------------------------
--
-- One pedestal each, walked left to right, so a bay's position in the row is
-- its position in this file and neither has to be counted.

local bayIndex = 0

local function bay(name: string, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = Vector3.new(2, 1, 2)
	part.CFrame = CFrame.new(Vector3.new(bayIndex * BAY_SPACING - 70, 0.5, BAY_ROW_Z))
	part.Color = colour
	part.Parent = workspace
	bayIndex += 1
	return part
end

-- The emitter every bay below is a departure from. Kept as a function rather
-- than copied eleven times, because a bay that differs from its neighbours in
-- two properties demonstrates neither of them.
local function baseline(host: Instance, name: string): ParticleEmitter
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = name
	emitter.Rate = 40
	emitter.Lifetime = NumberRange.new(2, 2)
	emitter.Speed = NumberRange.new(4, 4)
	emitter.SpreadAngle = Vector2.new(10, 10)
	emitter.Size = NumberSequence.new(0.6)
	emitter.Transparency = NumberSequence.new(0, 1)
	emitter.Color = ColorSequence.new(Color3.new(0.6, 0.8, 1))
	emitter.Parent = host
	return emitter
end

-- **`Brightness`, which is not `Color`.** The colour sequence is what the
-- particle is; brightness is how hard it is driven, and it is packed into the
-- instance's colour past 1.0 rather than clamped there - so this bay is the one
-- that says whether the HDR path survives all the way to the framebuffer. At
-- `Brightness = 1` it would be indistinguishable from the baseline, which is
-- exactly why it is 6.
do
	local emitter = baseline(bay("Brightness", Color3.new(0.6, 0.5, 0.2)), "Overdriven")
	emitter.Brightness = 6
	emitter.Color = ColorSequence.new(Color3.new(1, 0.75, 0.35))
end

-- **`LightEmission` and `LightInfluence` are two ends of one question**, and a
-- bay each would put them a pedestal apart with nothing to compare against. So
-- both are here: one emitter self-lit and unaffected by the scene's light, one
-- taking the scene's light and adding none of its own.
do
	local host = bay("Lighting", Color3.new(0.2, 0.5, 0.6))

	local glowing = baseline(host, "SelfLit")
	glowing.LightEmission = 1
	glowing.LightInfluence = 0
	glowing.Color = ColorSequence.new(Color3.new(1, 0.4, 0.2))

	-- Offset along the pedestal so the two plumes stand beside each other
	-- rather than through each other.
	local shaded = Instance.new("Attachment")
	shaded.CFrame = CFrame.new(Vector3.new(1.4, 1, 0))
	shaded.Parent = host

	local lit = baseline(shaded, "SceneLit")
	lit.LightEmission = 0
	lit.LightInfluence = 1
	lit.Color = ColorSequence.new(Color3.new(1, 0.4, 0.2))
end

-- **`EmissionDirection`, which is `Top` unless somebody says otherwise.** A
-- sideways plume is the one departure here that is unmistakable from any angle:
-- get the normal table wrong and this bay sprays into its own pedestal.
do
	local emitter = baseline(bay("EmissionDirection", Color3.new(0.5, 0.3, 0.6)), "Sideways")
	emitter.EmissionDirection = Enum.NormalId.Right
	emitter.Speed = NumberRange.new(8, 8)
	emitter.Acceleration = Vector3.new(0, -4, 0)
end

-- **`ShapePartial` keeps a share of the shape's own axis**, so a sphere emitting
-- outward becomes a hemisphere. Paired with `Sphere` and `Surface`, because on
-- the default point shape it has no axis to keep and the property would look
-- like it does nothing.
do
	local emitter = baseline(bay("ShapePartial", Color3.new(0.3, 0.6, 0.35)), "Hemisphere")
	emitter.Shape = Enum.ParticleEmitterShape.Sphere
	emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Surface
	emitter.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
	emitter.ShapePartial = 0.5
	emitter.Speed = NumberRange.new(5, 5)
end

-- **`InAndOut`, the third shape direction and the one no other scene uses.**
-- `Particles.luau` covers `Outward` and `Inward`; a cylinder emitting both ways
-- is what is left, and it is the case where getting the sign test backwards
-- gives a plume that looks correct from one side.
do
	local emitter = baseline(bay("ShapeInOut", Color3.new(0.4, 0.4, 0.5)), "BothWays")
	emitter.Shape = Enum.ParticleEmitterShape.Cylinder
	emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Volume
	emitter.ShapeInOut = Enum.ParticleEmitterShapeInOut.InAndOut
	emitter.Speed = NumberRange.new(3, 6)
	emitter.Additive = true
end

-- **`TimeScale` is the emitter's own clock and not the world's.** Half speed
-- here, so this bay emits at half the rate of its neighbours from the same
-- authored `Rate` - which is the property's whole point: a slow-motion effect
-- should not need every number in it rewritten.
do
	local emitter = baseline(bay("TimeScale", Color3.new(0.55, 0.25, 0.25)), "HalfSpeed")
	emitter.TimeScale = 0.5
end

-- **`ZOffset` moves the quad along the view axis without moving the particle.**
-- A large positive offset pulls the card towards the eye, so this plume sorts in
-- front of its own pedestal instead of being cut by it. Nothing about where the
-- particle *is* changes, which is the distinction that makes it a rendering
-- property rather than a simulation one.
do
	local emitter = baseline(bay("ZOffset", Color3.new(0.25, 0.45, 0.3)), "PulledForward")
	emitter.ZOffset = 3
	emitter.Size = NumberSequence.new(1.2)
end

-- **`Orientation`, all four of them, because the default is the only one any
-- other scene ever sets.** Four emitters on one pedestal at four heights: a
-- reader looking along the row sees the same particle held four ways, which is
-- the only arrangement in which "velocity parallel" and "velocity
-- perpendicular" are distinguishable at all.
do
	local host = bay("Orientation", Color3.new(0.45, 0.45, 0.2))

	local kinds = {
		Enum.ParticleOrientation.FacingCamera,
		Enum.ParticleOrientation.FacingCameraWorldUp,
		Enum.ParticleOrientation.VelocityParallel,
		Enum.ParticleOrientation.VelocityPerpendicular,
	}

	for index, kind in kinds do
		local point = Instance.new("Attachment")
		point.CFrame = CFrame.new(Vector3.new(0, index * 2, 0))
		point.Parent = host

		local emitter = baseline(point, `Orientation{index}`)
		emitter.Orientation = kind

		-- Squashed, so the quad is a rectangle rather than a square. A square
		-- looks identical under all four of these and the bay would demonstrate
		-- nothing.
		emitter.Squash = NumberSequence.new(1.6)
		emitter.Speed = NumberRange.new(2, 2)
		emitter.Rate = 12
	end
end

-- **`Enabled`, toggled from the tick.** The property is a boolean nobody would
-- think to check, and the failure it hides is the expensive one: an emitter
-- that keeps emitting while disabled is a scene whose cost does not fall when a
-- game turns its effects off. Pulsed rather than set once, so the bay shows the
-- turn-off *and* the turn-on - a block released and reacquired.
local pulsed = baseline(bay("Enabled", Color3.new(0.2, 0.3, 0.55)), "Pulsed")
pulsed.Rate = 120
pulsed.Color = ColorSequence.new(Color3.new(0.5, 1, 0.7))

-- **`LockedToPart` and `VelocityInheritance`, on the one thing here that
-- moves.** Both are properties of a *moving* emitter and both are invisible on
-- a still one, so the bay is a swinging arm with three emitters on it:
--
--   * locked - every particle rides the arm, so the plume is rigid;
--   * free with no inheritance - particles are left behind in world space,
--     which is the default and the thing the other two are read against;
--   * free with full inheritance - particles are born with the arm's own
--     velocity, so they are flung along its travel.
--
-- Get inheritance backwards and the third plume trails the wrong way, which is
-- unreadable in a still frame and obvious the moment the arm swings.
local swinger = bay("Motion", Color3.new(0.5, 0.2, 0.4))
do
	local locked = Instance.new("Attachment")
	locked.CFrame = CFrame.new(Vector3.new(0, 3, 0))
	locked.Parent = swinger

	local rigid = baseline(locked, "Locked")
	rigid.LockedToPart = true
	rigid.Color = ColorSequence.new(Color3.new(1, 0.9, 0.4))

	local free = Instance.new("Attachment")
	free.CFrame = CFrame.new(Vector3.new(0, 5, 0))
	free.Parent = swinger

	local trailing = baseline(free, "Free")
	trailing.VelocityInheritance = 0
	trailing.Color = ColorSequence.new(Color3.new(0.4, 0.9, 1))

	local carried = Instance.new("Attachment")
	carried.CFrame = CFrame.new(Vector3.new(0, 7, 0))
	carried.Parent = swinger

	local flung = baseline(carried, "Inherited")
	flung.VelocityInheritance = 1
	flung.Color = ColorSequence.new(Color3.new(1, 0.4, 0.7))
end

-- Shared force modules stay on the emitter's resident parameter row. Three
-- emitters begin with no speed, so their shape is made by exactly one force:
-- away from the centre, around local up, or through scrolling noise.
do
	local host = bay("Forces", Color3.new(0.25, 0.45, 0.55))
	local colours = {
		Color3.new(1, 0.35, 0.2),
		Color3.new(0.3, 1, 0.5),
		Color3.new(0.35, 0.55, 1),
	}

	for index = 1, 3 do
		local point = Instance.new("Attachment")
		point.CFrame = CFrame.new(Vector3.new((index - 2) * 2.5, 3, 0))
		point.Parent = host

		local emitter = baseline(point, `Force{index}`)
		emitter.Shape = Enum.ParticleEmitterShape.Sphere
		emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Surface
		emitter.Speed = NumberRange.new(0, 0)
		emitter.Lifetime = NumberRange.new(3, 3)
		emitter.Color = ColorSequence.new(colours[index])
		if index == 1 then
			emitter.RadialAcceleration = 8
		elseif index == 2 then
			emitter.TangentialAcceleration = 10
		else
			emitter.NoiseStrength = 12
			emitter.NoiseFrequency = 0.8
			emitter.NoiseScrollSpeed = 2
		end
	end
end

-- A deliberately excessive acceleration held to one metre per second. The
-- short plume is the visible assertion that MaxSpeed is applied after forces.
do
	local emitter = baseline(bay("MaxSpeed", Color3.new(0.55, 0.3, 0.2)), "Limited")
	emitter.Acceleration = Vector3.new(0, 100, 0)
	emitter.Speed = NumberRange.new(0, 0)
	emitter.MaxSpeed = 1
	emitter.Lifetime = NumberRange.new(4, 4)
	emitter.Color = ColorSequence.new(Color3.new(1, 0.75, 0.2))
end

-- Disabled continuous emission with explicit bursts. MaxParticles makes the
-- ring exactly twenty-four rows, and Clear removes it between pulses.
local burst = baseline(bay("EmitClear", Color3.new(0.5, 0.2, 0.5)), "Burst")
burst.Enabled = false
burst.Rate = 0
burst.MaxParticles = 24
burst.Lifetime = NumberRange.new(4, 4)
burst.Speed = NumberRange.new(5, 9)
burst.Shape = Enum.ParticleEmitterShape.Sphere
burst.ShapeStyle = Enum.ParticleEmitterShapeStyle.Surface
burst.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
burst.Additive = true
burst.Color = ColorSequence.new(Color3.new(1, 0.25, 0.8))

-- No time rate at all. This emitter can only leave a trail when its parent
-- moves, which makes RateOverDistance distinguishable from ordinary emission.
local distanceHost = bay("RateOverDistance", Color3.new(0.2, 0.5, 0.35))
local distanceOrigin = distanceHost.Position
local distance = baseline(distanceHost, "DistanceTrail")
distance.Rate = 0
distance.RateOverDistance = 12
distance.MaxParticles = 240
distance.Lifetime = NumberRange.new(3, 3)
distance.Speed = NumberRange.new(0, 0)
distance.Size = NumberSequence.new(0.35, 0.05)
distance.Color = ColorSequence.new(Color3.new(0.25, 1, 0.65))

-- --- the spam ----------------------------------------------------------------
--
-- **`HOSTS x HOSTS` visible parts on the baseplate, five emitters in each.**
-- `Particles.luau` makes every emitter in its grid identical on purpose, so
-- that the count is the only variable. This one varies two things and nothing
-- else.
--
--   * **`EmissionDirection` per emitter**, which is what makes five on one part
--     five plumes rather than one five times as dense - and is the only place
--     the property is under load rather than in a labelled bay of its own.
--   * **`Additive` alternating**, because the draw loop groups batches by
--     exactly that and then by texture. A field of one kind is one run and a
--     field of two is two, so a scene that only ever spammed one kind would
--     never notice a grouping rule that stopped grouping.
--
-- Everything else about them is identical, for the reason the other scene
-- gives: a random scene is a different scene every run.
--
-- **The hosts are visible now, and that is affordable where it was not.** The
-- old field left them `Visible = false` because a hundred thousand submitted
-- instances cost more than the particles did - measured at 4 FPS when they were
-- authored `Transparency = 1`, which is *submitted and blended* rather than
-- skipped. A thousand opaque cubes is a rounding error against half a million
-- particles, and a stress scene where the emitters hang in empty air is one
-- nobody can tell is working.

local DIRECTIONS = {
	Enum.NormalId.Top,
	Enum.NormalId.Front,
	Enum.NormalId.Back,
	Enum.NormalId.Right,
	Enum.NormalId.Left,
}

local half = HOSTS * SPACING * 0.5
local hosts = 0
local made = 0

for row = 0, HOSTS - 1 do
	for column = 0, HOSTS - 1 do
		local host = Instance.new("Part")
		host.Name = "Emitters"
		host.Anchored = true
		host.Size = Vector3.new(2, 2, 2)
		host.Color = Color3.new(0.18, 0.20, 0.26)
		host.CFrame = CFrame.new(Vector3.new(column * SPACING - half, 1, row * SPACING - half))
		host.Parent = workspace
		hosts += 1

		for index = 1, EMITTERS_PER_HOST do
			local emitter = Instance.new("ParticleEmitter")

			-- A hundred alive apiece: twenty a second over a five-second life.
			-- See the knobs at the top for why that number and not another.
			emitter.Rate = 20
			emitter.Lifetime = NumberRange.new(5, 5)
			emitter.Speed = NumberRange.new(2, 4)
			emitter.SpreadAngle = Vector2.new(16, 16)
			emitter.Acceleration = Vector3.new(0, -1, 0)
			emitter.Size = NumberSequence.new(0.35, 0.05)

			-- **The one thing that differs within a host.** Roblox's
			-- `EmissionDirection` is the face a particle leaves by, so five
			-- faces is five plumes off one two-metre cube.
			emitter.EmissionDirection = DIRECTIONS[index]

			-- Alternated across the *emitter*, not the host, so a single part
			-- carries both blend modes and the grouping has to sort within a
			-- host as well as across the field.
			emitter.Additive = (index % 2) == 0
			emitter.Color = ColorSequence.new({
				ColorSequenceKeypoint.new(0, Color3.new(0.4, 0.7, 1)),
				ColorSequenceKeypoint.new(1, Color3.new(0.1, 0.2, 0.5)),
			})
			emitter.Parent = host
			made += 1
		end
	end
end

print(
	string.format(
		"stressparticles: %d bays, %d host(s) x %d emitter(s) = %d, ~%d particles",
		bayIndex,
		hosts,
		EMITTERS_PER_HOST,
		made,
		made * 100
	)
)

-- --- the camera --------------------------------------------------------------

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.FieldOfView = 70
camera.FarPlaneZ = 4000

-- **Low and looking along the bays into the field.** Overdraw is what a
-- particle system is limited by, so a camera looking down at the whole plane
-- would measure the upload and the simulation while measuring almost no fill.
camera.CFrame = CFrame.lookAt(Vector3.new(0, 9, -20), Vector3.new(0, 4, BAY_ROW_Z))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- --- the one moving thing, and the one blinking thing ------------------------
--
-- Both bays above need the tick, and neither is a system: this is what a person
-- writing a game writes, which is the half of the scripting surface these
-- scenes exist to keep honest.

local elapsed = 0
local burstPhase = -1

RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta

	-- The arm the three motion emitters ride. Swept rather than spun, so it
	-- reverses - inheritance that is correct in one direction and wrong in the
	-- other shows up at the turn.
	swinger.CFrame = CFrame.new(Vector3.new(swinger.Position.X, 0.5, BAY_ROW_Z))
		* CFrame.Angles(0, math.sin(elapsed * 1.6) * 1.2, 0)

	-- A second on, a second off. Slow enough to watch the pool release the
	-- block and slow enough to see it come back.
	pulsed.Enabled = (math.floor(elapsed) % 2) == 0

	-- The green host sweeps six metres while its zero-rate emitter leaves a
	-- distance-sampled trail behind it.
	distanceHost.CFrame = CFrame.new(Vector3.new(distanceOrigin.X, 0.5, BAY_ROW_Z))
		* CFrame.new(Vector3.new(math.sin(elapsed * 1.5) * 3, 0, 0))

	-- One burst, one clear, then repeat. Calls happen only on phase changes so a
	-- slow frame cannot queue the same transition twice.
	local phase = math.floor(elapsed) % 4
	if phase ~= burstPhase then
		if phase == 0 then
			burst:Emit(24)
		elseif phase == 2 then
			burst:Clear()
		end
		burstPhase = phase
	end
end)

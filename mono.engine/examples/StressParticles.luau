--!strict
-- Every authored emitter property, and then a hundred thousand of them.
--
-- **`ROADMAP.md` v0.17 asks for a full particle demo, and "full" is the word
-- that makes this a different scene from `Particles.luau`.** That one is a
-- *count*: it answers v0.10's hundred-thousand-emitter target and the flipbook
-- path, with a grid of identical emitters so that what the frame time measures
-- is the number of them and nothing about what any one is doing. It leaves
-- eleven authored properties untouched, and an untouched property is one that
-- can stop working without anything noticing.
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
-- ## The knob
--
-- `GRID`. 320 is 102,400 emitters, and it is not an arbitrary number: at five
-- particles each that is 512,000 alive against a default pool of 524,288, so
-- 320 is the largest square this scene can spam without starving itself. Raise
-- the pool before raising this, or the extra emitters are refused and the count
-- stops climbing while the cost of authoring them does not.
--
-- 100 is 10,000 and is where a machine without a discrete GPU should start.

local GRID = 320

-- Two metres between hosts. `Particles.luau` carries the argument: at four the
-- field is sub-pixel from any camera that can see the whole of it, which is a
-- stress scene that costs everything and shows nothing.
local SPACING = 2

-- Where the feature bays sit relative to the spam, which is deliberately in
-- front of it rather than inside it. A bay surrounded by a hundred thousand
-- other emitters is a bay nobody can read.
local BAY_ROW_Z = -50
local BAY_SPACING = 14

local RunService = game:GetService("RunService")

-- --- the baseplate -----------------------------------------------------------

local plate = Instance.new("Part")
plate.Name = "Baseplate"
plate.Anchored = true
plate.Size = Vector3.new(GRID * SPACING + 80, 1, GRID * SPACING + 80)
plate.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
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

-- --- the spam ----------------------------------------------------------------
--
-- **`GRID x GRID` hosts on the baseplate, and the field is not quite uniform.**
-- `Particles.luau` makes every emitter in its grid identical on purpose, so that
-- the count is the only variable. This one alternates `Additive`, because the
-- draw loop groups batches by exactly that and then by texture - so a field of
-- one kind is one run and a field of two kinds is two, and a scene that only
-- ever spammed one kind would never notice a grouping rule that stopped
-- grouping.
--
-- Everything else about them is identical, for the reason the other scene
-- gives: a random scene is a different scene every run.

local half = GRID * SPACING * 0.5
local made = 0

for row = 0, GRID - 1 do
	for column = 0, GRID - 1 do
		local host = Instance.new("Part")
		host.Anchored = true
		host.Size = Vector3.one

		-- **`Visible = false` and not `Transparency = 1`.** They look the same
		-- and they are different questions: a fully transparent part is still
		-- *submitted*, and being blended it lands in the pass sorted
		-- back-to-front from the eye every frame. A hundred thousand of those is
		-- a hundred thousand instances sorted and drawn to produce nothing -
		-- measured at 4 FPS on the scene this rule was found on. `Visible =
		-- false` submits nothing at all.
		host.Visible = false
		host.CFrame = CFrame.new(Vector3.new(column * SPACING - half, 1, row * SPACING - half))
		host.Parent = workspace

		local emitter = Instance.new("ParticleEmitter")

		-- Five alive apiece: five a second over a one-second life.
		emitter.Rate = 5
		emitter.Lifetime = NumberRange.new(1, 1)
		emitter.Speed = NumberRange.new(1, 2)
		emitter.SpreadAngle = Vector2.new(20, 20)
		emitter.Acceleration = Vector3.new(0, -1, 0)
		emitter.Size = NumberSequence.new(0.35, 0.05)
		emitter.Additive = (column % 2) == 0
		emitter.Color = ColorSequence.new({
			ColorSequenceKeypoint.new(0, Color3.new(0.4, 0.7, 1)),
			ColorSequenceKeypoint.new(1, Color3.new(0.1, 0.2, 0.5)),
		})
		emitter.Parent = host
		made += 1
	end
end

print(string.format("stressparticles: %d bays, %d grid emitters, ~%d particles", bayIndex, made, made * 5))

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
end)

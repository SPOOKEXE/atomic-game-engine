--!strict
-- The particle stress scene, and the property surface that goes with it.
--
-- **`ROADMAP.md` v0.10 asks for 100,000 emitters at five particles each**, and
-- this is the scene that puts that number on screen rather than in a benchmark.
-- `engine.effects.bench.particles` measures the simulation half headless - 389
-- microseconds a frame for the pair of passes; this is the other half, which is
-- what the upload and the draw cost when somebody is looking at it.
--
-- ## What it measured
--
-- `release` preset, 24-thread machine, Vulkan, 1280x720, `--frames 60 --uncapped`:
--
-- | Emitters | Particles | FPS (avg) | Draw calls |
-- |---|---|---|---|
-- | 6,400 | ~32,000 | 285 | 4 |
-- | **102,400** | **~500,000** | **170** | **5** |
--
-- The 102,400 row read 287 FPS before the default pool was raised - and that
-- figure was a *starved* scene rather than a fast one. At the old 250,000-slot
-- default only about 41,000 of the grid's emitters ever got a block, so the
-- number being reported was the cost of drawing 40 per cent of the scene. 170 is
-- the whole of it. `ParticleStatistics::EmittersRefused` is what tells the two
-- apart, and nothing else does - a starved emitter is silent.
--
-- **The roadmap's number holds.** Five draw calls at a hundred thousand emitters
-- is what the state grouping in `Renderer::Impl::PrepareParticles` buys: the
-- first version issued one per emitter, and the same scene at 1,600 emitters cost
-- 1,608 draw calls.
--
-- **Two things were found by running this rather than by reading it**, and both
-- are recorded where they were fixed:
--
-- - The grid's host parts were authored `Transparency = 1`, which *submits* them
--   into the sorted blended pass. A hundred thousand invisible cubes, sorted
--   back-to-front every frame: 1,228,920 triangles and 4 FPS. `Visible = false`
--   submits nothing - see the comment on the grid loop below.
-- - The particle upload memcpy'd into the transfer buffer and never issued the
--   copy to the vertex buffer. It drew *something*, because the buffer held the
--   previous frame, so only a capture found it.
--
-- **`dev` is `-O0` and this is not a scene to judge there.** The same scene runs
-- at 4 FPS unoptimised, which is the engine's stated default - `AGENTS.md` says a
-- profile should measure what the engine does rather than what the optimiser
-- rescued, and the corollary is that a frame-rate claim has to name its preset.
--
-- ## Run it
--
--     client --script Particles.luau
--     client --script Particles.luau --stats     # F3 for the counters
--
-- **Read `render.particles` on the F3 panel, not the frame time alone.** A scene
-- that is slow and looks fine is almost always an emitter whose rate ran away,
-- and the particle count is the number that says so - the frame time only says
-- that something is wrong.
--
-- ## What the scene is
--
-- Three things, in increasing order of how much they are meant to hurt:
--
-- 1. **A demonstration row** - one emitter per authored feature, spaced out and
--    large enough to look at. This is the half that is a *test* rather than a
--    stress: if a flipbook is playing backwards or a squash is inverted, it is
--    visible here and invisible in the grid.
-- 2. **The grid** - `GRID x GRID` emitters at a low rate, which is the roadmap's
--    hundred thousand. Deliberately boring: every one of them is the same, so
--    what is being measured is the count and nothing else.
-- 3. **A beam and a trail**, because they share `Attachment` with the emitters
--    and a scene that exercised only particles would leave the other half of the
--    module unlooked-at.
--
-- ## The knob
--
-- `GRID` is the only number worth changing. 320 is 102,400 emitters, which is
-- the roadmap's target; 100 is 10,000 and is what a machine without a discrete
-- GPU should start from.

local GRID = 320

-- **Two metres and not four.** At four the grid is 1,280 metres across and its
-- particles are sub-pixel from any camera that can see the whole of it - a
-- stress scene that costs everything and shows nothing. Two puts the far edge at
-- 640 m, which still overflows the frustum and gives the near half enough
-- overdraw to be the thing being measured.
local SPACING = 2
local POOL_RESERVE = GRID * GRID * 6

-- --- the floor ---------------------------------------------------------------

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local plate = Instance.new("Part")
plate.Name = "Baseplate"
plate.Anchored = true
plate.Size = Vector3.new(GRID * SPACING + 40, 1, GRID * SPACING + 40)
plate.CFrame = CFrame.new(Vector3.new(0, -0.5 + GROUND_LIFT, 0))
plate.Color = Color3.new(0.10, 0.11, 0.14)
plate.Parent = workspace

-- --- the demonstration row ---------------------------------------------------
--
-- One emitter per feature, on its own pedestal, far enough apart to tell apart.
-- Every one of these is an assertion made visually: a reader who knows what the
-- property does can see whether it does it.

local function pedestal(x: number, z: number, colour: Color3): Part
	local part = Instance.new("Part")
	part.Anchored = true
	part.Size = Vector3.new(2, 1, 2)
	part.CFrame = CFrame.new(Vector3.new(x, 0.5, z))
	part.Color = colour
	part.Parent = workspace
	return part
end

-- A plume: small at the bottom, wide at the top, fading as it rises. The
-- ordinary case, and the one every other row is a departure from.
do
	local host = pedestal(-30, -40, Color3.new(0.6, 0.2, 0.2))
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "Plume"
	emitter.Rate = 40
	emitter.Lifetime = NumberRange.new(2, 3)
	emitter.Speed = NumberRange.new(3, 5)
	emitter.SpreadAngle = Vector2.new(12, 12)
	emitter.Acceleration = Vector3.new(0, 2, 0)
	emitter.Size = NumberSequence.new({
		NumberSequenceKeypoint.new(0, 0.3),
		NumberSequenceKeypoint.new(1, 2.5),
	})
	emitter.Transparency = NumberSequence.new({
		NumberSequenceKeypoint.new(0, 0.2),
		NumberSequenceKeypoint.new(0.6, 0.4),
		NumberSequenceKeypoint.new(1, 1),
	})
	emitter.Color = ColorSequence.new({
		ColorSequenceKeypoint.new(0, Color3.new(1.0, 0.7, 0.2)),
		ColorSequenceKeypoint.new(1, Color3.new(0.2, 0.2, 0.25)),
	})
	emitter.Parent = host
end

-- An explosion shell: a sphere emitting from its surface, outward, additive.
-- **Additive is the visible difference** - these overlap and add up rather than
-- averaging, which is what makes a spark look hot.
do
	local host = pedestal(-10, -40, Color3.new(0.2, 0.2, 0.6))
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "Shell"
	emitter.Rate = 80
	emitter.Lifetime = NumberRange.new(1, 1.6)
	emitter.Speed = NumberRange.new(6, 9)
	emitter.Shape = Enum.ParticleEmitterShape.Sphere
	emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Surface
	emitter.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
	emitter.Additive = true
	emitter.Drag = 1.5
	emitter.Size = NumberSequence.new(0.6, 0.05)
	emitter.Color = ColorSequence.new({
		ColorSequenceKeypoint.new(0, Color3.new(1, 1, 0.8)),
		ColorSequenceKeypoint.new(0.4, Color3.new(1, 0.5, 0.1)),
		ColorSequenceKeypoint.new(1, Color3.new(0.4, 0.05, 0.0)),
	})
	emitter.Parent = host
end

-- Spinning, squashed confetti falling under gravity. `RotSpeed` and `Squash`
-- are the two properties nothing else in this scene exercises.
do
	local host = pedestal(10, -40, Color3.new(0.2, 0.6, 0.3))
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "Confetti"
	emitter.Rate = 60
	emitter.Lifetime = NumberRange.new(3, 4)
	emitter.Speed = NumberRange.new(4, 7)
	emitter.SpreadAngle = Vector2.new(50, 50)
	emitter.Acceleration = Vector3.new(0, -6, 0)
	emitter.Rotation = NumberRange.new(0, 360)
	emitter.RotSpeed = NumberRange.new(-240, 240)
	emitter.Size = NumberSequence.new(0.4)

	-- Positive squash stretches the quad's height and shrinks its width, so a
	-- flake is a rectangle rather than a square. Ramped, so it flattens as it
	-- falls.
	emitter.Squash = NumberSequence.new(0, 1.5)
	emitter.Color = ColorSequence.new({
		ColorSequenceKeypoint.new(0, Color3.new(1, 0.3, 0.5)),
		ColorSequenceKeypoint.new(0.5, Color3.new(0.3, 0.9, 1)),
		ColorSequenceKeypoint.new(1, Color3.new(1, 0.9, 0.2)),
	})
	emitter.Parent = host
end

-- A ring in the ground plane, emitting inward. `Disc` plus `Inward` is the
-- combination that makes a vortex, and it is the one where getting the sign
-- backwards is invisible in the code and obvious on screen.
do
	local host = pedestal(30, -40, Color3.new(0.5, 0.3, 0.6))
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "Vortex"
	emitter.Rate = 100
	emitter.Lifetime = NumberRange.new(1.5, 1.5)
	emitter.Speed = NumberRange.new(3, 3)
	emitter.Shape = Enum.ParticleEmitterShape.Disc
	emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Surface
	emitter.ShapeInOut = Enum.ParticleEmitterShapeInOut.Inward
	emitter.Acceleration = Vector3.new(0, 4, 0)
	emitter.Additive = true
	emitter.Size = NumberSequence.new(0.5, 0.05)
	emitter.Color = ColorSequence.new(Color3.new(0.4, 0.8, 1))
	emitter.Parent = host
end

-- **The fox, dancing.** A flipbook from a real GIF, on an emitter parented to an
-- `Attachment` rather than to the part.
--
-- Two features in one row on purpose. The flipbook is what v0.10 adds to the
-- texture path, and the attachment is what beams and trails share with emitters -
-- so a scene where the attachment resolve is broken shows it here, as an effect
-- emitting from the world origin instead of from its pedestal.
--
-- ## Where the texture comes from
--
--     assetc --input ART --output content        # fox_dance.gif -> fox_dance.atex
--     cdn --publish content --store store --signing-key HEX
--     client --cdn dir:store --publisher-key PUB --script Particles.luau
--
-- `scripts/demos/bake-fox-flipbook.sh` is those three lines with the paths
-- filled in, and it exists because this bay drew an untextured square on every
-- machine that had the GIF. `contentimport` - which is how the local store gets
-- filled - renames every file it takes to its own content hash, so the GIF was
-- in the store as `67e91cae....atex` and no name in any scene could reach it.
-- `assetc` is the half of the pipeline that keeps the tree, so the script stages
-- the GIFs under the names the scenes use, bakes, and publishes a store of its
-- own beside the library rather than over it.
--
-- `bake` decodes the GIF into **one flipbook sheet** - 48 frames of a 420x420
-- animation laid out as an 8x8 grid - so by the time it reaches here it is an
-- ordinary texture and nothing downstream knows it animates.
--
-- ## Why the numbers are these numbers
--
-- **The frame count is not written here at all, and that is a fix.** It used to
-- say 24; the GIF has 48. Half the dance never played, and nothing looked broken
-- - a flipbook that stops early is just a shorter animation, which is the whole
-- reason the number should never have been copied. The bake puts it on the
-- texture, the client records it into the world, and an emitter that leaves
-- `FlipbookFrames` at zero adopts it.
--
-- It still matters that 48 is not 64: the grid is the next square power of two
-- that fits, so sixteen of the cells are empty and playing them would spend a
-- quarter of every particle's life on blanks.
--
-- **`Lifetime = 5` with `OneShot`.** A one-shot flipbook is stretched over the
-- particle's whole life, so five seconds of life is five seconds of dance at
-- about 9.6 frames a second - the whole loop, once, per particle. The GIF's own
-- rate is 24fps and is deliberately not used here: `OneShot` paces itself off
-- the lifetime, which is what makes "it lasts five seconds and plays the whole
-- thing" true regardless of how many frames the source has.
--
-- **`Rate = 0.2`, which is one particle every five seconds.** Exactly one fox on
-- screen at a time: a new one is born as the last expires, so the dance appears
-- continuous rather than a crowd of overlapping foxes at different points in the
-- loop.
do
	local host = pedestal(50, -40, Color3.new(0.6, 0.5, 0.2))

	local point = Instance.new("Attachment")
	point.Name = "Stage"
	point.CFrame = CFrame.new(Vector3.new(0, 4, 0))
	point.Parent = host

	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "DancingFox"

	-- The manifest name, extension included, exactly as the publisher wrote it.
	-- There is no table anywhere mapping one to the other - the manifest's name
	-- *is* the texture's name.
	emitter.Texture = "effects/fox_dance.atex"

	emitter.FlipbookLayout = Enum.ParticleFlipbookLayout.Grid8x8
	emitter.FlipbookMode = Enum.ParticleFlipbookMode.OneShot

	-- **The frame count is not stated here, and that is the fix.** It used to say
	-- 24 and the GIF has 48, so half the dance never played and nothing looked
	-- broken - a flipbook that stops early is just a shorter animation. The
	-- number is in the baked texture, the client records it into the world, and
	-- an emitter that leaves `FlipbookFrames` at zero adopts it. Writing it down
	-- here would be a second copy of a fact, which is the thing that was wrong.

	emitter.Rate = 0.2
	emitter.Lifetime = NumberRange.new(5, 5)

	-- **Still, so the animation is the only thing moving.** A dancing fox drifting
	-- across the scene would make it impossible to tell whether the flipbook is
	-- playing or the particle is.
	emitter.Speed = NumberRange.new(0, 0)
	emitter.SpreadAngle = Vector2.new(0, 0)
	emitter.Acceleration = Vector3.zero
	emitter.Rotation = NumberRange.new(0, 0)
	emitter.RotSpeed = NumberRange.new(0, 0)

	-- **`FacingCamera`, which is what the roadmap's orientation default already
	-- is** - set here anyway, because a demo that relies on a default is one that
	-- silently stops demonstrating when the default changes.
	emitter.Orientation = Enum.ParticleOrientation.FacingCamera

	-- Eight metres, so it reads from the camera's distance.
	emitter.Size = NumberSequence.new(8)

	-- **Opaque throughout.** Every other emitter here fades, and this one must not:
	-- a fading fox would make a flipbook frame that is genuinely blank
	-- indistinguishable from one that has faded out.
	emitter.Transparency = NumberSequence.new(0)
	emitter.Color = ColorSequence.new(Color3.new(1, 1, 1))

	emitter.Parent = point
end

-- --- a beam and a trail ------------------------------------------------------
--
-- Both hang off `Attachment`s, which is the whole reason that class exists
-- before anything that uses it.

do
	local left = pedestal(-50, -20, Color3.new(0.3, 0.3, 0.35))
	local right = pedestal(-20, -20, Color3.new(0.3, 0.3, 0.35))

	local from = Instance.new("Attachment")
	from.CFrame = CFrame.new(Vector3.new(0, 1, 0))
	from.Parent = left

	local to = Instance.new("Attachment")
	to.CFrame = CFrame.new(Vector3.new(0, 1, 0))
	to.Parent = right

	local beam = Instance.new("Beam")
	beam.Attachment0 = from
	beam.Attachment1 = to
	beam.Width0 = 1.2
	beam.Width1 = 0.2

	-- The curve pushes the control points along each attachment's own axis, so a
	-- beam sags or arcs without anybody authoring a spline.
	beam.CurveSize0 = 6
	beam.CurveSize1 = 6
	beam.Additive = true
	beam.Color = ColorSequence.new({
		ColorSequenceKeypoint.new(0, Color3.new(0.2, 0.8, 1)),
		ColorSequenceKeypoint.new(0.5, Color3.new(1, 1, 1)),
		ColorSequenceKeypoint.new(1, Color3.new(0.8, 0.2, 1)),
	})
	beam.Parent = left
end

-- The trail needs something that moves, so it goes on a part the tick spins.
local swinger: Part = pedestal(20, -20, Color3.new(0.5, 0.2, 0.2))
do
	local top = Instance.new("Attachment")
	top.CFrame = CFrame.new(Vector3.new(0, 2.5, 0))
	top.Parent = swinger

	local bottom = Instance.new("Attachment")
	bottom.CFrame = CFrame.new(Vector3.new(0, 0.5, 0))
	bottom.Parent = swinger

	local trail = Instance.new("Trail")
	trail.Attachment0 = top
	trail.Attachment1 = bottom
	trail.Lifetime = 0.8
	trail.Additive = true
	trail.Color = ColorSequence.new(Color3.new(1, 0.6, 0.1))
	trail.Transparency = NumberSequence.new(0, 1)
	trail.Parent = swinger
end

-- --- the grid ----------------------------------------------------------------
--
-- **The roadmap's number, and it is deliberately dull.** Every emitter here is
-- identical, so what the frame time measures is the count and nothing about what
-- any one of them is doing. The five-particle floor comes out of the rate and the
-- lifetime: five a second over one second is five alive.
--
-- The parts are one metre and unanchored-free - anchored, so the physics step
-- does not become what is being measured.

local half = GRID * SPACING * 0.5
local made = 0

for row = 0, GRID - 1 do
	for column = 0, GRID - 1 do
		local host = Instance.new("Part")
		host.Anchored = true
		host.Size = Vector3.one

		-- **`Visible = false` and not `Transparency = 1`, and at this count the
		-- difference is the whole scene.** They look identical and they are three
		-- different questions - `scene/Components.hpp` says so under
		-- `Visual::CastShadow`. A part at full transparency is *submitted*, and
		-- because it is blended it lands in the pass that is sorted back-to-front
		-- from the eye every frame. A hundred thousand of those is a hundred
		-- thousand instances sorted and drawn to produce nothing.
		--
		-- Measured: authored with `Transparency = 1` this scene ran at **4 FPS**
		-- with 1,228,920 triangles a frame - twelve per host cube, none of them
		-- visible. `Visible = false` submits nothing at all.
		host.Visible = false
		host.CFrame = CFrame.new(Vector3.new(
			column * SPACING - half,
			1,
			row * SPACING - half
		))
		host.Parent = workspace

		local emitter = Instance.new("ParticleEmitter")
		emitter.Rate = 5
		emitter.Lifetime = NumberRange.new(1, 1)
		emitter.Speed = NumberRange.new(1, 2)
		emitter.SpreadAngle = Vector2.new(20, 20)
		emitter.Acceleration = Vector3.new(0, -1, 0)
		emitter.Size = NumberSequence.new(0.35, 0.05)
		emitter.Color = ColorSequence.new({
			ColorSequenceKeypoint.new(0, Color3.new(0.4, 0.7, 1)),
			ColorSequenceKeypoint.new(1, Color3.new(0.1, 0.2, 0.5)),
		})
		emitter.Parent = host
		made += 1
	end
end

print(string.format("particles: %d emitters, pool reserve %d", made, POOL_RESERVE))

-- --- the camera and the one moving thing -------------------------------------

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.FieldOfView = 70
camera.FarPlaneZ = 4000
-- Low and close, so the near grid fills the frame. **Overdraw is what a
-- particle system is actually limited by**, and a camera looking down at the
-- whole plane measures upload and simulation while measuring almost no fill.
camera.CFrame = CFrame.lookAt(Vector3.new(0, 6, 40), Vector3.new(0, 3, -30))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The swinger, so the trail has something to trace. Everything else is static
-- on purpose: a scene where the parts move too would be measuring the physics
-- step as well.
local elapsed = 0
game:GetService("RunService").Heartbeat:Connect(function(delta: number)
	elapsed += delta
	swinger.CFrame = CFrame.new(Vector3.new(20, 1.5, -20))
		* CFrame.Angles(0, elapsed * 2.5, math.sin(elapsed * 3) * 0.8)
end)

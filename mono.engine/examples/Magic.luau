--!strict
-- Spells fired at generated terrain, which they dig holes in.
--
-- The whole stack, in this engine, from data to a part on screen:
--
--     Presets -> Compiler.compileGraph -> SpellSolver.castTrusted
--       -> SpellSolver.advance      (fixed-step, plain-data events)
--       -> MagicRuntime.build       (compiled presentation -> a Part)
--       -> TerrainCore.Edits        (impact -> a crater)
--       -> TerrainRuntime.applyEdit (crater -> chunks queued)
--       -> TerrainRuntime.pump      (one chunk re-meshed a frame)
--
-- `MagicCore` and `TerrainCore` are byte-identical to the Rojo project they came
-- from; `MagicRuntime` and `TerrainRuntime` are this engine's, because that is
-- the only layer that names an instance. All four are children of this script -
-- see the mount below.
--
-- ## What terrain changes about the magic
--
-- Two things, and only two:
--
--   * **Impact is a height query.** With a flat range an impact is a plane
--     crossing at y = 0. Here it is `HeightField:heightAt(x, z)`, so a bolt
--     fired over a valley flies further than one fired at a hillside and a
--     lobbed shell lands on the slope it was aimed at. Everything downstream --
--     `MeteorCluster` splitting, `ChainLightning` hopping, `Splinterstorm`
--     breaking -- follows from the impact arriving at the right moment.
--   * **The impact carves.** `Edits:crater` appends to the edit list; the height
--     field reads that list rather than a cached copy, so a chunk that was not
--     loaded picks the crater up whenever it is next built.
--
-- Nothing in `MagicCore` knows terrain exists and nothing in `TerrainCore` knows
-- about spells. The coupling is this file.
--
-- ## What draws it
--
-- Every authored emitter, the muzzle beam, the ribbon behind each projectile and
-- the blast light, all out of `mono.engine/effects`. This scene drew none of them
-- until the classes existed: `MagicRuntime` was written against an engine that
-- had only `Part`, said so at the top of itself, and went on saying it after
-- `effects` landed - so the spells solved correctly, cratered the terrain, and
-- looked like coloured cubes. The count printed at startup is what is drawn.
--
-- The timeline cues are wired here rather than in `MagicRuntime`, because they
-- arrive as solver events and only this file reads those. `Timeline.EmitParticles`
-- is what grows `AtomicNova`'s mushroom out of one part, and a scene that drops
-- it is a scene where five presets detonate into nothing.

local RunService = game:GetService("RunService")

-- **The modules hang off this script, which is Rojo's arrangement and not the
-- engine's.** They used to be mirrored into `ReplicatedStorage` from a single
-- staged `assets/lib` tree, which made them a property of the *program*:
-- `MagicCore` appeared under every world of every game, including a brand-new
-- empty one, and this file asserted that somebody else had already put them
-- there. Now they are staged under `assets/examples/Magic` and mounted as
-- children of the `Script` that requires them, so a world that never opens this
-- scene has no trace of any of it. Nothing inside the modules changed: every
-- `require(script.Parent.X)` in them is relative and resolves the same either
-- way.
local Modules = script :: any

local Compiler = require(Modules.MagicCore.Compiler)
local Presets = require(Modules.MagicCore.Presets)
local SpellSolver = require(Modules.MagicCore.SpellSolver)

local Terrain = require(Modules.TerrainCore)
local TerrainRuntime = require(Modules.TerrainRuntime)
local MagicRuntime = require(Modules.MagicRuntime)

local SEED = 4242

-- How wide a terrain chunk is, in studs. A multiple of `HeightField.BaseLattice`,
-- which `Chunker.build` asserts.
--
-- **Sixteen rather than thirty-two, and the reason is what a rebuild looks
-- like.** A chunk is re-meshed whole: every part in it takes its new size and
-- position on one frame, so the bigger the chunk the bigger the patch of ground
-- that visibly jumps when a crater lands anywhere in it. At thirty-two studs a
-- single hop's crater restated a quarter of the arena in one frame.
--
-- It costs parts. A greedy merge cannot run a box across a chunk border, so
-- halving the chunk adds seams: 5344 parts at load against 4892, about nine per
-- cent, for a rebuild unit a quarter the size. It also makes each rebuild about
-- four times cheaper, which is what lets `REBUILDS_PER_FRAME` be four.
local CHUNK = 16

-- A fixed arena rather than a streamed world: the point is the craters, and a
-- camera that has to fly to find the terrain gets in the way of seeing them.
local ARENA_CHUNKS = 8

local CAST_PERIOD = 6
local LANE_STAGGER = 0.9
local FIRST_CAST_DELAY = 1.5

-- How far above the ground a projectile must climb before a crossing counts.
-- Burst children are born at the impact point, which is on the ground.
local ARMING_HEIGHT = 2

-- The lanes, and how big a hole each leaves.
--
-- **Crater size is authored here rather than derived from damage.** Damage is a
-- number about actors and a crater is a number about geometry; tying them
-- together means retuning the arena whenever somebody balances a spell, and it
-- makes a healing spell dig a hole.
local LANES = {
	{ preset = "ChainLightning", radius = 5, depth = 2 },
	{ preset = "GlacialLance", radius = 8, depth = 3 },
	{ preset = "Splinterstorm", radius = 11, depth = 5 },
	{ preset = "MeteorCluster", radius = 14, depth = 7 },
	{ preset = "AtomicNova", radius = 26, depth = 14 },
}

-- Children crater far smaller than their parent, so submunitions do not erase
-- the warhead's own hole.
local CHILD_SCALE = 0.35

-- How far above its muzzle a projectile may climb before the demo stops drawing
-- it.
--
-- **A guard over authored behaviour, not a fix to it.** `BurstDistribution.Arc`
-- is a hollow cone around the parent's heading with the azimuth drawn over the
-- whole circle, so a chain hop turns by the authored angle in *any* direction -
-- including straight up. `ChainArcLink` is `Motion.Linear` with no gravity, so a
-- hop aimed at the sky flies in a straight line until its lifetime runs out, and
-- `maxBurstDepth` is 3, so it hops again from up there. In the game these came
-- from that is fine: a hop is aimed at another target and there is always one.
-- This arena has a single enemy and a lot of sky.
--
-- Nothing is told to stop: the solver goes on tracking a strayed hop, and its
-- own bursts are caught by this same test on the frame they appear. What changes
-- is that a lance does not sail out of the world in front of the camera.
--
-- Sixty studs, against a measured worst legitimate climb of 31 for
-- `MeteorCluster` and 18 for `AtomicNova`. High enough that no authored arc
-- reaches it and low enough to catch a stray before it is a dot.
local STRAY_CEILING = 60

local edits = Terrain.Edits.new()
local field = Terrain.HeightField.new({ seed = SEED }, edits)

local terrainRoot = Instance.new("Folder")
terrainRoot.Name = "Terrain"
terrainRoot.Parent = workspace

local builder = TerrainRuntime.new(field, CHUNK, terrainRoot)

-- The top surface of a column. A block at integer height `h` occupies
-- [h, h + 1), so its surface is one above its index.
-- Where the ground is, including whatever has been blown out of it.
--
-- **Asked of the builder's field rather than the bare one, and that is not a
-- tidying.** `TerrainRuntime` wraps the height field in one that remembers the
-- noise - which cannot change - and the dug depth, which it folds forward on
-- each crater. The bare field recomputes about sixty octaves of noise per call
-- and walks every crater filed near the column, so this got slower with every
-- shot fired.
--
-- It is called per projectile per frame, twice: once to arm and once to test
-- for impact. At two hundred craters with five lanes in the air that was enough
-- to trip the Luau step budget inside `Noise.luau`, which reads as the scene
-- freezing rather than as a sampler being asked too much.
local terrain = builder.field
local function groundAt(x: number, z: number): number
	return terrain:solidTopAt(x, z, terrain:heightAt(x, z)) + 1
end

local function plain(value: Vector3)
	return { x = value.X, y = value.Y, z = value.Z }
end

local function world(value: any, fallback: Vector3): Vector3
	if type(value) ~= "table" then
		return fallback
	end
	return Vector3.new(value.x, value.y, value.z)
end

-- Somewhere dry with room to shoot across. Sampling the height field is cheap
-- and meshing is not, so searching for a site costs far less than building
-- chunks until one looks right.
local originX = 0
local originZ = 0
do
	local best = -math.huge
	for index = 1, 500 do
		local angle = index * 2.39996
		local distance = index * 7
		local x = math.floor(math.cos(angle) * distance)
		local z = math.floor(math.sin(angle) * distance)
		local height = field:heightAt(x, z)
		local score = -math.abs(height - (field.settings.seaLevel + 14))
		if height > field.settings.seaLevel + 8 and score > best then
			best = score
			originX = x
			originZ = z
		end
	end
end

local centreChunkX = originX // CHUNK
local centreChunkZ = originZ // CHUNK
builder:fill(centreChunkX, centreChunkZ, ARENA_CHUNKS)

type Visual = {
	entityId: number,
	handle: any,
	isRoot: boolean,
	armed: boolean,
	impacted: boolean,
	last: Vector3,
}

-- A projectile whose body is finished and whose tails are not.
--
-- **Destroying a part takes its emitters with it**, and an emitter's particles
-- die with the emitter - so a spell destroyed at the instant it detonates loses
-- the burst it fired detonating. See `MagicRuntime.longestTail`.
type Retiring = {
	handle: any,
	remaining: number,
}

type Lane = {
	name: string,
	index: number,
	radius: number,
	depth: number,
	graph: any,
	solver: any,
	container: Instance,
	-- What a projectile's muzzle beam tethers back to. One per lane and not one
	-- per shot: the far end is the caster, and the caster does not move.
	muzzleAnchor: Attachment,
	muzzle: Vector3,
	direction: Vector3,
	simulationTime: number,
	nextCastAt: number,
	castCount: number,
	recipeByEntity: { [number]: any },
	visuals: { [number]: Visual },
	retiring: { Retiring },
}

local lanes: { Lane } = {}
local craters = 0
local drawnEmitters = 0

local function actor(id: string, position: Vector3, faction: string, tags: { string })
	return {
		id = id,
		factionId = faction,
		position = plain(position),
		health = 100000,
		maxHealth = 100000,
		tags = tags,
		velocity = { x = 0, y = 0, z = 0 },
		statuses = {},
	}
end

for index, entry in LANES do
	local documents = Presets.getGraph(entry.preset)
	local graph, diagnostics = Compiler.compileGraph(documents.root, documents.recipes)

	if graph == nil then
		warn(`magic: lane {entry.preset} skipped - {diagnostics[1].message} at {diagnostics[1].path}`)
		continue
	end

	drawnEmitters += MagicRuntime.emitterCount(graph)

	-- Lanes stand on a circle and fire inwards, so every shot crosses different
	-- ground and the craters do not all land in one line.
	local angle = (index - 1) * (math.pi * 2 / #LANES)
	local standoff = ARENA_CHUNKS * CHUNK * 0.6
	local padX = originX + math.cos(angle) * standoff
	local padZ = originZ + math.sin(angle) * standoff

	local muzzle = Vector3.new(padX, groundAt(padX, padZ) + 7, padZ)
	local target = Vector3.new(originX, groundAt(originX, originZ) + 6, originZ)

	local container = Instance.new("Folder")
	container.Name = entry.preset
	container.Parent = workspace

	local post = Instance.new("Part")
	post.Name = "Muzzle"
	post.Anchored = true
	post.Size = Vector3.new(1.6, 7, 1.6)
	post.Position = Vector3.new(padX, muzzle.Y - 3.5, padZ)
	post.Color = Color3.fromRGB(96, 104, 122)
	post.Parent = container

	-- At the top of the post, which is where a shot leaves it.
	local anchor = Instance.new("Attachment")
	anchor.CFrame = CFrame.new(Vector3.new(0, post.Size.Y * 0.5, 0))
	anchor.Parent = post

	local sourceId = entry.preset .. ".caster"
	local enemyId = entry.preset .. ".enemy"

	table.insert(lanes, {
		name = entry.preset,
		index = index,
		radius = entry.radius,
		depth = entry.depth,
		graph = graph,
		solver = SpellSolver.new({
			graph = graph,
			-- Matching the step to the tick keeps a fast bolt smooth; the default
			-- 1/30 moves a 220 studs/s projectile in visible jumps.
			projectile = { eventSolver = { stepSeconds = 1 / 60 } },
			actors = {
				actor(sourceId, muzzle, "blue", { "caster" }),
				actor(enemyId, target, "red", { "enemy" }),
			},
		}),
		container = container,
		muzzleAnchor = anchor,
		muzzle = muzzle,
		direction = (target - muzzle).Unit,
		simulationTime = 0,
		nextCastAt = FIRST_CAST_DELAY + (index - 1) * LANE_STAGGER,
		castCount = 0,
		recipeByEntity = {},
		visuals = {},
		retiring = {},
	})
end

-- **The one place magic reaches terrain.**
--
-- Appending to the edit list is what makes the crater exist; rebuilding is what
-- makes it visible. They are separate because the second is optional -- a chunk
-- nobody has loaded picks the crater up from the list whenever it is next built.
local function carve(x: number, z: number, radius: number, depth: number)
	craters += 1
	builder:applyEdit(edits:crater(x, z, radius, depth, craters))
end

local function registerChildren(lane: Lane, records: { any })
	for _, record in records do
		-- Bound to a typed local rather than tested inline: refining `any` with
		-- `~= nil` gives `~nil`, which is not a `number` and so cannot index a
		-- number-keyed table. The annotation is the assertion.
		local entityId: number? = record.entityId
		if record.kind == "ChildSpawned" and entityId ~= nil then
			lane.recipeByEntity[entityId] = lane.graph.recipes[record.recipeId]
		end
	end
end

-- Whether a projectile has left the arena upwards and is not coming back.
local function strayed(lane: Lane, position: Vector3): boolean
	return position.Y > lane.muzzle.Y + STRAY_CEILING
end

local function onSpawned(lane: Lane, entityId: number, position: Vector3)
	local compiled = lane.recipeByEntity[entityId]
	if compiled == nil then
		return
	end

	-- **Tested before anything is built**, so a hop that bursts from a strayed
	-- parent never gets a part at all rather than getting one for a frame.
	if strayed(lane, position) then
		lane.recipeByEntity[entityId] = nil
		return
	end

	local handle =
		MagicRuntime.build(compiled, lane.container, `{lane.name}_{entityId}`, lane.muzzleAnchor)
	MagicRuntime.setPosition(handle, position)

	lane.visuals[entityId] = {
		entityId = entityId,
		handle = handle,
		isRoot = compiled == lane.graph.root,
		armed = position.Y > groundAt(position.X, position.Z) + ARMING_HEIGHT,
		impacted = false,
		last = position,
	}
end

local function impact(lane: Lane, visual: Visual, position: Vector3)
	if visual.impacted or not visual.armed then
		return
	end

	local ground = groundAt(position.X, position.Z)
	if position.Y > ground then
		return
	end

	visual.impacted = true

	local scale = if visual.isRoot then 1 else CHILD_SCALE
	carve(position.X, position.Z, lane.radius * scale, lane.depth * scale)

	-- The arena runs unattended, so one refused trigger must not tear down the
	-- beat for every other lane.
	local ok, outcome = pcall(function()
		return lane.solver:triggerImpact({
			entityId = visual.entityId,
			scheduledTime = lane.simulationTime,
			position = plain(Vector3.new(position.X, ground, position.Z)),
			targetActorId = lane.name .. ".enemy",
			areaCandidates = { lane.name .. ".enemy" },
		})
	end)

	if ok then
		registerChildren(lane, outcome.records)
	end

	MagicRuntime.conceal(visual.handle)
end

-- Ends a projectile's body and lets what it put in the air finish.
--
-- The Roblox runtime does the same thing by keeping the part until its emitters
-- release. Here the wait is explicit because nothing reports a particle dying:
-- the pool is device-resident and the host only knows how long a particle *can*
-- live. See `MagicRuntime.longestTail`.
local function retire(lane: Lane, visual: Visual)
	MagicRuntime.conceal(visual.handle)
	table.insert(lane.retiring, {
		handle = visual.handle,
		remaining = MagicRuntime.longestTail(visual.handle),
	})
end

local function step(lane: Lane, delta: number)
	local result = lane.solver:advance(delta)
	lane.simulationTime = result.simulationTime

	registerChildren(lane, result.records)

	for _, event in result.projectileEvents do
		if event.entityId == nil then
			continue
		end
		if event.kind == "ProjectileSpawned" then
			onSpawned(lane, event.entityId, world(event.position, lane.muzzle))
		elseif event.kind == "ProjectileExpired" or event.kind == "ProjectileCancelled" then
			local visual = lane.visuals[event.entityId]
			if visual ~= nil then
				retire(lane, visual)
				lane.visuals[event.entityId] = nil
				lane.recipeByEntity[event.entityId] = nil
			end
		elseif event.kind == "TimelineCue" then
			-- **The five presets that detonate rather than fade are all here.**
			-- An emitter authored at rate zero says "a cue fires this", so a
			-- scene that ignores the cue draws an emitter that never emits.
			local visual = lane.visuals[event.entityId]
			local cue = event.timelineCue
			if visual ~= nil and cue ~= nil and cue.emitterId ~= nil then
				if cue.kind == "Timeline.EmitParticles" then
					MagicRuntime.emit(visual.handle, cue.emitterId, cue.count or 0)
				elseif cue.kind == "Timeline.SetEmitterEnabled" then
					MagicRuntime.setEmitterEnabled(visual.handle, cue.emitterId, cue.enabled == true)
				end
			end
		elseif event.kind == "EmitterDisabled" and event.emitterId ~= nil then
			-- The solver's own copy of the same decision, which arrives for a
			-- cancellation as well as for a cue. Applying both is idempotent.
			local visual = lane.visuals[event.entityId]
			if visual ~= nil then
				MagicRuntime.setEmitterEnabled(visual.handle, event.emitterId, false)
			end
		end
	end

	for entityId, visual in lane.visuals do
		local entity = lane.solver:getEntity(entityId)
		if entity == nil then
			continue
		end

		local position = world(entity.position, visual.last)

		if strayed(lane, position) then
			retire(lane, visual)
			lane.visuals[entityId] = nil
			lane.recipeByEntity[entityId] = nil
			continue
		end

		MagicRuntime.setPosition(visual.handle, position)
		MagicRuntime.advance(visual.handle, delta)
		if not visual.impacted then
			MagicRuntime.applyCurves(visual.handle, entity.age)
		end
		visual.last = position

		if position.Y > groundAt(position.X, position.Z) + ARMING_HEIGHT then
			visual.armed = true
		end
		if entity.logicalState == "Active" then
			impact(lane, visual, position)
		end
	end

	-- **Backwards, because this removes from the list it is walking.** A forward
	-- walk with `table.remove` skips the element after every removal, which here
	-- is a projectile that never gets destroyed.
	for index = #lane.retiring, 1, -1 do
		local held = lane.retiring[index]
		held.remaining -= delta
		if held.remaining <= 0 then
			MagicRuntime.destroy(held.handle)
			table.remove(lane.retiring, index)
		end
	end

	if lane.simulationTime >= lane.nextCastAt then
		lane.nextCastAt += CAST_PERIOD
		lane.castCount += 1

		local accepted = lane.solver:castTrusted({
			castId = `{lane.name}.cast.{lane.castCount}`,
			sourceActorId = lane.name .. ".caster",
			targetActorId = lane.name .. ".enemy",
			areaCandidates = { lane.name .. ".enemy" },
			origin = plain(lane.muzzle),
			direction = plain(lane.direction),
			-- Deterministic per lane and per shot, so a replay digs the same
			-- craters in the same order.
			rootSeed = lane.index * 100003 + lane.castCount,
			scheduledTime = lane.simulationTime,
		})
		lane.recipeByEntity[accepted.entityId] = lane.graph.root
	end
end

local eye = Instance.new("Camera")
eye.Name = "Eye"
eye.FieldOfView = 70
eye.FarPlaneZ = ARENA_CHUNKS * CHUNK * 6
eye.Parent = workspace
workspace.CurrentCamera = eye

local ORBIT_RADIUS = ARENA_CHUNKS * CHUNK * 1.05
local ORBIT_PERIOD = 90
local EYE_HEIGHT = 46

local function aim(seconds: number)
	local angle = seconds * (math.pi * 2 / ORBIT_PERIOD)
	local centre = Vector3.new(originX, groundAt(originX, originZ), originZ)
	eye.CFrame = CFrame.lookAt(
		centre + Vector3.new(math.cos(angle) * ORBIT_RADIUS, EYE_HEIGHT, math.sin(angle) * ORBIT_RADIUS),
		centre
	)
end

aim(0)

local stats = builder:stats()
print(
	`magic: {#lanes} lane(s) over a {ARENA_CHUNKS * CHUNK * 2}-stud arena at {originX}, {originZ}; `
		.. `{stats.chunks} chunks, {stats.parts} terrain parts`
)
print(`magic: {drawnEmitters} authored particle emitter(s), a muzzle beam, a trail and a light per shot`)

local reportedAt = 0

-- How many craters have to have landed before the cache is worth auditing.
--
-- Enough that craters have overlapped and that a chunk has been rebuilt for a
-- second crater while holding the first - which is the case a drop-and-recompute
-- cache would pass and a fold-forward one can fail.
local AUDIT_AFTER_CRATERS = 12
local audited = false

-- How many chunks may be re-meshed in one frame.
--
-- **Four, because a chunk is sixteen studs.** A rebuild re-meshes a chunk and
-- rewrites every part in it, and five lanes on a shared cadence land several
-- craters on one tick - which used to rebuild as many as nineteen chunks inside
-- a single heartbeat and cost a 117 ms frame. Rebuilding on a budget spreads
-- that over frames.
--
-- The number tracks `CHUNK`: a sixteen-stud chunk is a quarter the columns of a
-- thirty-two, so four of them cost about what one of the old ones did, and a
-- crater that reaches forty small chunks still finishes in ten frames rather
-- than forty. Raising it trades frame time for how quickly a crater appears.
--
-- The edit is recorded the moment it lands, so what this delays is the picture
-- and never the physics: a crater is already in the height field, already
-- deflecting the next bolt, before it is drawn.
local REBUILDS_PER_FRAME = 4

RunService.Heartbeat:Connect(function(delta: number)
	for _, lane in lanes do
		step(lane, delta)
	end

	-- After the lanes, so a crater carved this frame is queued before the pump
	-- looks - one frame earlier than doing it first, for free.
	builder:pump(REBUILDS_PER_FRAME)

	-- **Once, after enough craters to have exercised the cache, and never
	-- again.** `TerrainRuntime` folds each new crater into its cached column
	-- depths by hand rather than recomputing them, which is what keeps a rebuild
	-- cheap however long the arena has been shot at - and is the one thing here
	-- that can silently be wrong. The marker this leaves is what
	-- `examples/tests/Scene.cpp` asserts on, so the check runs in `just test`
	-- rather than only when somebody is watching.
	if not audited and craters >= AUDIT_AFTER_CRATERS then
		audited = true
		local wrong = builder:audit(4)
		local marker = Instance.new("Part")
		marker.Name = if wrong > 0 then "TerrainCacheStale" else "TerrainCacheFresh"
		marker.Anchored = true
		marker.CanCollide = false
		marker.Visible = false
		marker.Parent = terrainRoot
		if wrong > 0 then
			warn(`magic: {wrong} column(s) hold a stale dug depth`)
		end
	end

	local now = if lanes[1] ~= nil then lanes[1].simulationTime else 0
	aim(now)

	if now - reportedAt >= 15 then
		reportedAt = now
		local current = builder:stats()
		print(
			`magic: {craters} crater(s), {current.chunks} chunks, `
				.. `{current.parts} terrain parts, {current.idle} idle, `
				.. `{builder:queued()} chunk(s) waiting to re-mesh`
		)
	end
end)

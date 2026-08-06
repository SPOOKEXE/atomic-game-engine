--!strict
-- Spells fired at generated terrain, which they dig holes in.
--
-- The whole stack, in this engine, from data to a part on screen:
--
--     Presets -> Compiler.compileGraph -> SpellSolver.castTrusted
--       -> SpellSolver.advance      (fixed-step, plain-data events)
--       -> MagicRuntime.build       (compiled presentation -> a Part)
--       -> TerrainCore.Edits        (impact -> a crater)
--       -> TerrainRuntime.applyEdit (crater -> chunks rebuilt)
--
-- `MagicCore` and `TerrainCore` are byte-identical to the Rojo project they came
-- from; `MagicRuntime` and `TerrainRuntime` are this engine's, because that is
-- the only layer that names an instance.
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
-- ## What you will not see
--
-- Particles, beams, trails and lights. This engine has none of those classes, so
-- every authored emitter -- the tails, the smoke, `AtomicNova`'s nine-emitter
-- mushroom -- has nothing to draw it. The count is printed at startup so the
-- gap is in the log rather than a surprise. See `lib/MagicRuntime/init.luau`.

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local MagicCore = ReplicatedStorage:FindFirstChild("MagicCore")
assert(MagicCore ~= nil, "ReplicatedStorage.MagicCore was not mounted — is assets/lib staged?")

local Compiler = require((MagicCore :: any):FindFirstChild("Compiler") :: any)
local Presets = require((MagicCore :: any):FindFirstChild("Presets") :: any)
local SpellSolver = require((MagicCore :: any):FindFirstChild("SpellSolver") :: any)

local Terrain = require(ReplicatedStorage:FindFirstChild("TerrainCore") :: any)
local TerrainRuntime = require(ReplicatedStorage:FindFirstChild("TerrainRuntime") :: any)
local MagicRuntime = require(ReplicatedStorage:FindFirstChild("MagicRuntime") :: any)

local SEED = 4242
local CHUNK = 32

-- A fixed arena rather than a streamed world: the point is the craters, and a
-- camera that has to fly to find the terrain gets in the way of seeing them.
local ARENA_CHUNKS = 4

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

local edits = Terrain.Edits.new()
local field = Terrain.HeightField.new({ seed = SEED }, edits)

local terrainRoot = Instance.new("Instance")
terrainRoot.Name = "Terrain"
terrainRoot.Parent = workspace

local builder = TerrainRuntime.new(field, CHUNK, terrainRoot)

-- The top surface of a column. A block at integer height `h` occupies
-- [h, h + 1), so its surface is one above its index.
local function groundAt(x: number, z: number): number
	return field:solidTopAt(x, z, field:heightAt(x, z)) + 1
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

type Lane = {
	name: string,
	index: number,
	radius: number,
	depth: number,
	graph: any,
	solver: any,
	container: Instance,
	muzzle: Vector3,
	direction: Vector3,
	simulationTime: number,
	nextCastAt: number,
	castCount: number,
	recipeByEntity: { [number]: any },
	visuals: { [number]: Visual },
}

local lanes: { Lane } = {}
local craters = 0
local missingEmitters = 0

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
		warn(`magic: lane {entry.preset} skipped — {diagnostics[1].message} at {diagnostics[1].path}`)
		continue
	end

	missingEmitters += MagicRuntime.missingEmitters(graph)

	-- Lanes stand on a circle and fire inwards, so every shot crosses different
	-- ground and the craters do not all land in one line.
	local angle = (index - 1) * (math.pi * 2 / #LANES)
	local standoff = ARENA_CHUNKS * CHUNK * 0.6
	local padX = originX + math.cos(angle) * standoff
	local padZ = originZ + math.sin(angle) * standoff

	local muzzle = Vector3.new(padX, groundAt(padX, padZ) + 7, padZ)
	local target = Vector3.new(originX, groundAt(originX, originZ) + 6, originZ)

	local container = Instance.new("Instance")
	container.Name = entry.preset
	container.Parent = workspace

	local post = Instance.new("Part")
	post.Name = "Muzzle"
	post.Anchored = true
	post.Size = Vector3.new(1.6, 7, 1.6)
	post.Position = Vector3.new(padX, muzzle.Y - 3.5, padZ)
	post.Color = Color3.fromRGB(96, 104, 122)
	post.Parent = container

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
		muzzle = muzzle,
		direction = (target - muzzle).Unit,
		simulationTime = 0,
		nextCastAt = FIRST_CAST_DELAY + (index - 1) * LANE_STAGGER,
		castCount = 0,
		recipeByEntity = {},
		visuals = {},
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

local function onSpawned(lane: Lane, entityId: number, position: Vector3)
	local compiled = lane.recipeByEntity[entityId]
	if compiled == nil then
		return
	end

	local handle = MagicRuntime.build(compiled, lane.container, `{lane.name}_{entityId}`)
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
				MagicRuntime.destroy(visual.handle)
				lane.visuals[event.entityId] = nil
				lane.recipeByEntity[event.entityId] = nil
			end
		end
	end

	for entityId, visual in lane.visuals do
		local entity = lane.solver:getEntity(entityId)
		if entity == nil then
			continue
		end

		local position = world(entity.position, visual.last)
		MagicRuntime.setPosition(visual.handle, position)
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
print(`magic: {missingEmitters} authored particle emitter(s) have nothing to draw them in this engine`)

local reportedAt = 0

RunService.Heartbeat:Connect(function(delta: number)
	for _, lane in lanes do
		step(lane, delta)
	end

	local now = if lanes[1] ~= nil then lanes[1].simulationTime else 0
	aim(now)

	if now - reportedAt >= 15 then
		reportedAt = now
		local current = builder:stats()
		print(
			`magic: {craters} crater(s), {current.chunks} chunks, `
				.. `{current.parts} terrain parts, {current.idle} idle`
		)
	end
end)

--!strict

local Enums = require(script.Parent.Enums)
type EventSolverConfig = {
	stepSeconds: number?,
	fixedStep: number?,
	maxCatchUpSteps: number?,
	maxBacklogSeconds: number?,
	maxQueuedEvents: number?,
	maxBufferedEvents: number?,
	maxEventsPerStep: number?,
}

type SchedulerProcessedEvent = {
	sequence: number,
	scheduledTime: number,
	processedAt: number,
	kind: string,
	payload: unknown?,
}

type SchedulerScheduledEvent = {
	sequence: number,
	scheduledTime: number,
	kind: string,
	payload: unknown?,
}

type SchedulerScheduleInput = {
	scheduledTime: number,
	kind: string,
	payload: unknown?,
}

type SchedulerAdvanceResult = {
	simulationTime: number,
	steps: number,
	processedEvents: number,
	accumulator: number,
	droppedBacklogSeconds: number,
	totalDroppedBacklogSeconds: number,
	queuedEvents: number,
	bufferedEvents: number,
	eventBackpressure: boolean,
}

type EventScheduler = {
	Schedule: (
		self: EventScheduler,
		inputOrScheduledTime: { scheduledTime: number, kind: string, payload: unknown? } | number,
		kind: string?,
		payload: unknown?
	) -> (unknown?, string?),
	Advance: (self: EventScheduler, deltaSeconds: number) -> SchedulerAdvanceResult,
	Drain: (self: EventScheduler) -> { SchedulerProcessedEvent },
	CanSchedule: (self: EventScheduler, count: number?) -> boolean,
	CanReplaceScheduled: (
		self: EventScheduler,
		predicate: (event: SchedulerScheduledEvent) -> boolean,
		replacementCount: number
	) -> boolean,
	ReplaceScheduled: (
		self: EventScheduler,
		predicate: (event: SchedulerScheduledEvent) -> boolean,
		replacements: { SchedulerScheduleInput }
	) -> ({ SchedulerScheduledEvent }?, string?),
}

type EventSolverModule = {
	new: (config: EventSolverConfig?) -> EventScheduler,
}

type VisualEmitterState = {
	id: string,
	lifetimeMax: number,
	disableAt: number?,
	latestEmitAt: number?,
}

type VisualLifecycleState = {
	requestKind: ("Release" | "Cancel")?,
	requestAt: number?,
	releaseAt: number?,
	emitters: { VisualEmitterState },
}

type VisualTracker = {
	RegisterEmitter: (
		self: VisualTracker,
		registrationOrId: { id: string, lifetime: { min: number?, max: number }?, lifetimeMax: number? } | string,
		lifetimeMax: number?
	) -> (VisualEmitterState, boolean),
	RecordEmission: (self: VisualTracker, emitterId: string, emitAt: number) -> boolean,
	DisableEmitter: (self: VisualTracker, emitterId: string, disableAt: number) -> boolean,
	RequestRelease: (self: VisualTracker, requestAt: number) -> (number, { string }),
	RequestCancel: (self: VisualTracker, requestAt: number) -> (number, { string }),
	CanRelease: (self: VisualTracker, now: number) -> boolean,
	GetReleaseAt: (self: VisualTracker) -> number?,
	GetState: (self: VisualTracker) -> VisualLifecycleState,
}

type VisualLifecycleModule = {
	new: (initialEmitters: unknown?) -> VisualTracker,
}

local EventSolver = ((require(script.Parent.EventSolver) :: unknown) :: EventSolverModule)
local VisualLifecycle = ((require(script.Parent.VisualLifecycle) :: unknown) :: VisualLifecycleModule)

export type Vector = {
	x: number,
	y: number,
	z: number,
}

export type RuntimeInput = {
	origin: Vector,
	direction: Vector,
	rootSeed: number,
}

export type EmitterDefinition = {
	id: string,
	enabledSeconds: number,
	lifetime: {
		min: number?,
		max: number,
	},
}

export type TimelineRecord = {
	kind: string,
	id: string,
	time: number,
	emitterId: string?,
	count: number?,
	enabled: boolean?,
}

export type BurstChildGroup = {
	id: string,
	recipeId: string,
	count: number,
}

export type ExpandedBurstChild = {
	childIndex: number,
	groupId: string,
	recipeId: string,
}

export type BurstDefinition = {
	kind: string,
	trigger: "Impact" | "Expire" | "ImpactOrExpire",
	distribution: string,
	children: { BurstChildGroup },
	maxDepth: number,
	random: unknown?,
	time: number?,
	expandedChildren: { ExpandedBurstChild }?,
}

export type LinearMotion = {
	kind: "Motion.Linear",
	velocity: Vector?,
	speed: number?,
	space: "LaunchLocal" | "WorldSnapshot"?,
}

export type BallisticMotion = {
	kind: "Motion.Ballistic",
	velocity: Vector?,
	initialVelocity: Vector?,
	speed: number?,
	acceleration: Vector?,
	gravity: Vector?,
	gravityScale: number?,
	velocitySpace: "LaunchLocal" | "WorldSnapshot"?,
}

export type BezierMotion = {
	kind: "Motion.Bezier",
	duration: number,
	space: "LaunchLocal" | "WorldSnapshot",
	points: { Vector },
}

export type Motion = LinearMotion | BallisticMotion | BezierMotion

export type ProjectileBlueprint = {
	lifetime: {
		maxSeconds: number?,
		seconds: number?,
	},
	motion: Motion,
	presentation: {
		emitters: { EmitterDefinition }?,
		particles: { EmitterDefinition }?,
		timeline: { TimelineRecord }?,
	},
	timeline: { TimelineRecord }?,
	burst: BurstDefinition?,
	costs: unknown?,
}

export type CompiledSpell = ProjectileBlueprint | {
	projectileBlueprint: ProjectileBlueprint?,
	blueprint: ProjectileBlueprint?,
	projectile: ProjectileBlueprint?,
	timeline: { TimelineRecord }?,
	burst: BurstDefinition?,
	costs: unknown?,
	serverProgram: {
		burst: BurstDefinition?,
	}?,
}

export type EventKind =
	"ProjectileSpawned"
	| "TimelineCue"
	| "EmitterDisabled"
	| "ProjectileExpired"
	| "ProjectileCancelled"
	| "VisualReleased"
	| "BurstScheduled"

export type ProjectileEvent = {
	kind: EventKind,
	entityId: number,
	sequence: number,
	scheduledTime: number,
	processedAt: number,
	position: Vector,
	emitterId: string?,
	timelineIndex: number?,
	timelineCue: TimelineRecord?,
	burstChildIndex: number?,
	burstGroupId: string?,
	burstRecipeId: string?,
	rootSeed: number?,
	reason: string?,
}

export type LogicalState = "Active" | "Expired" | "Cancelled"
export type VisualState = "Active" | "Released"

export type Entity = {
	id: number,
	logicalState: LogicalState,
	visualState: VisualState,
	spawnTime: number,
	expiresAt: number,
	logicalEndTime: number?,
	releaseAt: number,
	age: number,
	origin: Vector,
	direction: Vector,
	position: Vector,
	rootSeed: number,
	cancelReason: string?,
	costs: unknown?,
	visualLifecycle: VisualLifecycleState,
}

export type Config = {
	eventSolver: EventSolverConfig?,
	maxEventHistory: number?,
	maxEntityTombstones: number?,
}

export type AdvanceResult = {
	simulationTime: number,
	steps: number,
	events: { ProjectileEvent },
	droppedBacklogSeconds: number,
	totalDroppedBacklogSeconds: number,
	queuedEvents: number,
	eventBackpressure: boolean,
	droppedEventHistory: number,
}

type RuntimeLinear = {
	kind: "Motion.Linear",
	velocity: Vector,
}

type RuntimeBallistic = {
	kind: "Motion.Ballistic",
	velocity: Vector,
	acceleration: Vector,
}

type RuntimeBezier = {
	kind: "Motion.Bezier",
	duration: number,
	p0: Vector,
	p1: Vector,
	p2: Vector,
	p3: Vector,
}

type RuntimeMotion = RuntimeLinear | RuntimeBallistic | RuntimeBezier

type InternalEntity = {
	id: number,
	logicalState: LogicalState,
	visualState: VisualState,
	spawnTime: number,
	expiresAt: number,
	logicalEndTime: number?,
	releaseAt: number,
	age: number,
	origin: Vector,
	direction: Vector,
	position: Vector,
	rootSeed: number,
	cancelReason: string?,
	costs: unknown?,
	motion: RuntimeMotion,
	visualLifecycle: VisualTracker,
	generation: number,
	spawnEventEmitted: boolean,
	visualReleaseScheduledAt: number,
}

type InternalPayload = {
	eventKind: EventKind,
	entityId: number,
	generation: number,
	emitterId: string?,
	timelineIndex: number?,
	timelineCue: TimelineRecord?,
	burstChildIndex: number?,
	burstGroupId: string?,
	burstRecipeId: string?,
	reason: string?,
	emitterStateRecorded: boolean?,
}

export type Solver = {
	_scheduler: EventScheduler,
	_entities: { [number]: InternalEntity },
	_entityOrder: { number },
	_nextEntityId: number,
	_simulationTime: number,
	_eventHistory: { ProjectileEvent },
	_maxEventHistory: number,
	_droppedEventHistory: number,
	_releasedEntities: { [number]: Entity },
	_releasedEntityOrder: { number },
	_maxEntityTombstones: number,

	Spawn: (self: Solver, compiledSpell: CompiledSpell, runtimeInput: RuntimeInput) -> number,
	CanCancelAt: (self: Solver, entityId: number, scheduledTime: number) -> boolean,
	CanCancel: (self: Solver, entityId: number) -> boolean,
	CancelAt: (self: Solver, entityId: number, scheduledTime: number, reason: string?) -> boolean,
	Cancel: (self: Solver, entityId: number, reason: string?) -> boolean,
	GetEntity: (self: Solver, entityId: number) -> Entity?,
	GetEvents: (self: Solver, clear: boolean?) -> { ProjectileEvent },
	Advance: (self: Solver, deltaSeconds: number) -> AdvanceResult,
}

local ProjectileSolver = {}
ProjectileSolver.__index = ProjectileSolver

local function isFinite(value: number): boolean
	return value == value and math.abs(value) ~= math.huge
end

local function timeReached(now: number, scheduledTime: number): boolean
	local tolerance = math.max(1, math.abs(now), math.abs(scheduledTime)) * 1e-9
	return scheduledTime <= now + tolerance
end

local function vector(x: number, y: number, z: number): Vector
	return { x = x, y = y, z = z }
end

local function clone(value: Vector): Vector
	return vector(value.x, value.y, value.z)
end

local function add(a: Vector, b: Vector): Vector
	return vector(a.x + b.x, a.y + b.y, a.z + b.z)
end

local function scale(value: Vector, scalar: number): Vector
	return vector(value.x * scalar, value.y * scalar, value.z * scalar)
end

local function dot(a: Vector, b: Vector): number
	return a.x * b.x + a.y * b.y + a.z * b.z
end

local function cross(a: Vector, b: Vector): Vector
	return vector(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	)
end

local function magnitude(value: Vector): number
	return math.sqrt(dot(value, value))
end

local function unit(value: Vector): Vector
	local length = magnitude(value)
	assert(length > 0 and isFinite(length), "direction must be finite and non-zero")
	return scale(value, 1 / length)
end

local function assertVector(value: Vector, name: string)
	assert(
		isFinite(value.x) and isFinite(value.y) and isFinite(value.z),
		name .. " must contain finite components"
	)
end

local function transformLaunchLocal(localValue: Vector, direction: Vector): Vector
	local worldUp = vector(0, 1, 0)
	if math.abs(dot(direction, worldUp)) > 0.999 then
		worldUp = vector(0, 0, 1)
	end
	local right = unit(cross(direction, worldUp))
	local up = unit(cross(right, direction))
	return add(add(scale(right, localValue.x), scale(up, localValue.y)), scale(direction, -localValue.z))
end

local function resolveBlueprint(compiledSpell: CompiledSpell): (ProjectileBlueprint, { TimelineRecord }?, BurstDefinition?, unknown?)
	local wrapper = (compiledSpell :: unknown) :: {
		projectileBlueprint: ProjectileBlueprint?,
		blueprint: ProjectileBlueprint?,
		projectile: ProjectileBlueprint?,
		timeline: { TimelineRecord }?,
		burst: BurstDefinition?,
		costs: unknown?,
		serverProgram: {
			burst: BurstDefinition?,
		}?,
	}
	local blueprint = wrapper.projectileBlueprint or wrapper.blueprint or wrapper.projectile
	if blueprint == nil then
		blueprint = (compiledSpell :: unknown) :: ProjectileBlueprint
	end

	local resolvedBlueprint = blueprint :: ProjectileBlueprint
	assert(
		resolvedBlueprint.lifetime ~= nil
			and resolvedBlueprint.motion ~= nil
			and resolvedBlueprint.presentation ~= nil,
		"compiled spell has no projectile blueprint"
	)
	local timeline = wrapper.timeline or resolvedBlueprint.timeline or resolvedBlueprint.presentation.timeline
	local burst = wrapper.burst
		or (if wrapper.serverProgram == nil then nil else wrapper.serverProgram.burst)
		or resolvedBlueprint.burst
	local costs = if wrapper.costs ~= nil then wrapper.costs else resolvedBlueprint.costs
	return resolvedBlueprint, timeline, burst, costs
end

local function resolveLifetime(blueprint: ProjectileBlueprint): number
	local lifetime = blueprint.lifetime.maxSeconds or blueprint.lifetime.seconds
	assert(lifetime ~= nil and isFinite(lifetime) and lifetime >= 0, "projectile lifetime must be finite and non-negative")
	return lifetime
end

local function resolveMotion(motion: Motion, origin: Vector, direction: Vector): RuntimeMotion
	if motion.kind == "Motion.Linear" then
		local velocity: Vector
		if motion.velocity ~= nil then
			assertVector(motion.velocity, "linear velocity")
			velocity = if motion.space == "WorldSnapshot"
				then clone(motion.velocity)
				else transformLaunchLocal(motion.velocity, direction)
		else
			local speed = motion.speed
			assert(speed ~= nil and isFinite(speed), "linear motion needs velocity or speed")
			velocity = scale(direction, speed)
		end
		return { kind = "Motion.Linear", velocity = velocity }
	elseif motion.kind == "Motion.Ballistic" then
		local sourceVelocity = motion.velocity or motion.initialVelocity
		local velocity: Vector
		if sourceVelocity ~= nil then
			assertVector(sourceVelocity, "ballistic velocity")
			velocity = if motion.velocitySpace == "WorldSnapshot"
				then clone(sourceVelocity)
				else transformLaunchLocal(sourceVelocity, direction)
		else
			local speed = motion.speed
			assert(speed ~= nil and isFinite(speed), "ballistic motion needs velocity or speed")
			velocity = scale(direction, speed)
		end

		local acceleration = motion.acceleration or motion.gravity or vector(0, 0, 0)
		assertVector(acceleration, "ballistic acceleration")
		acceleration = scale(acceleration, motion.gravityScale or 1)
		return {
			kind = "Motion.Ballistic",
			velocity = velocity,
			acceleration = acceleration,
		}
	else
		assert(isFinite(motion.duration) and motion.duration > 0, "Bezier duration must be finite and positive")
		assert(#motion.points == 3 or #motion.points == 4, "Bezier motion needs three or four points")

		local points = {}
		for _, point in ipairs(motion.points) do
			assertVector(point, "Bezier point")
			if motion.space == "LaunchLocal" then
				table.insert(points, add(origin, transformLaunchLocal(point, direction)))
			else
				table.insert(points, clone(point))
			end
		end

		local p0: Vector
		local p1: Vector
		local p2: Vector
		local p3: Vector
		if #points == 3 then
			p0 = clone(origin)
			p1 = points[1]
			p2 = points[2]
			p3 = points[3]
		else
			p0 = points[1]
			p1 = points[2]
			p2 = points[3]
			p3 = points[4]
		end

		return {
			kind = "Motion.Bezier",
			duration = motion.duration,
			p0 = p0,
			p1 = p1,
			p2 = p2,
			p3 = p3,
		}
	end
end

local function samplePosition(entity: InternalEntity, absoluteTime: number): Vector
	local sampleTime = absoluteTime
	if entity.logicalEndTime ~= nil then
		sampleTime = math.min(sampleTime, entity.logicalEndTime)
	end
	local age = math.max(0, sampleTime - entity.spawnTime)
	local motion = entity.motion
	if motion.kind == "Motion.Linear" then
		return add(entity.origin, scale(motion.velocity, age))
	elseif motion.kind == "Motion.Ballistic" then
		return add(
			add(entity.origin, scale(motion.velocity, age)),
			scale(motion.acceleration, 0.5 * age * age)
		)
	else
		local alpha = math.clamp(age / motion.duration, 0, 1)
		local inverse = 1 - alpha
		return add(
			add(scale(motion.p0, inverse * inverse * inverse), scale(motion.p1, 3 * inverse * inverse * alpha)),
			add(scale(motion.p2, 3 * inverse * alpha * alpha), scale(motion.p3, alpha * alpha * alpha))
		)
	end
end

local function copyEvent(event: ProjectileEvent): ProjectileEvent
	return {
		kind = event.kind,
		entityId = event.entityId,
		sequence = event.sequence,
		scheduledTime = event.scheduledTime,
		processedAt = event.processedAt,
		position = clone(event.position),
		emitterId = event.emitterId,
		timelineIndex = event.timelineIndex,
		timelineCue = event.timelineCue,
		burstChildIndex = event.burstChildIndex,
		burstGroupId = event.burstGroupId,
		burstRecipeId = event.burstRecipeId,
		rootSeed = event.rootSeed,
		reason = event.reason,
	}
end

local function copyVisualLifecycle(state: VisualLifecycleState): VisualLifecycleState
	local emitters = {}
	for _, emitter in ipairs(state.emitters) do
		table.insert(emitters, {
			id = emitter.id,
			lifetimeMax = emitter.lifetimeMax,
			disableAt = emitter.disableAt,
			latestEmitAt = emitter.latestEmitAt,
		})
	end
	return {
		requestKind = state.requestKind,
		requestAt = state.requestAt,
		releaseAt = state.releaseAt,
		emitters = emitters,
	}
end

local function publicEntity(entity: InternalEntity): Entity
	return {
		id = entity.id,
		logicalState = entity.logicalState,
		visualState = entity.visualState,
		spawnTime = entity.spawnTime,
		expiresAt = entity.expiresAt,
		logicalEndTime = entity.logicalEndTime,
		releaseAt = entity.releaseAt,
		age = entity.age,
		origin = clone(entity.origin),
		direction = clone(entity.direction),
		position = clone(entity.position),
		rootSeed = entity.rootSeed,
		cancelReason = entity.cancelReason,
		costs = entity.costs,
		visualLifecycle = copyVisualLifecycle(entity.visualLifecycle:GetState()),
	}
end

local function copyEntity(entity: Entity): Entity
	return {
		id = entity.id,
		logicalState = entity.logicalState,
		visualState = entity.visualState,
		spawnTime = entity.spawnTime,
		expiresAt = entity.expiresAt,
		logicalEndTime = entity.logicalEndTime,
		releaseAt = entity.releaseAt,
		age = entity.age,
		origin = clone(entity.origin),
		direction = clone(entity.direction),
		position = clone(entity.position),
		rootSeed = entity.rootSeed,
		cancelReason = entity.cancelReason,
		costs = entity.costs,
		visualLifecycle = copyVisualLifecycle(entity.visualLifecycle),
	}
end

function ProjectileSolver.new(config: Config?): Solver
	local supplied: Config = if config == nil then {} else config
	local maxEventHistory = supplied.maxEventHistory or 4096
	local maxEntityTombstones = if supplied.maxEntityTombstones == nil
		then maxEventHistory
		else supplied.maxEntityTombstones
	assert(maxEventHistory > 0 and maxEventHistory % 1 == 0, "maxEventHistory must be a positive integer")
	assert(
		maxEntityTombstones > 0 and maxEntityTombstones % 1 == 0,
		"maxEntityTombstones must be a positive integer"
	)

	local self = setmetatable({
		_scheduler = EventSolver.new(supplied.eventSolver),
		_entities = {},
		_entityOrder = {},
		_nextEntityId = 1,
		_simulationTime = 0,
		_eventHistory = {},
		_maxEventHistory = maxEventHistory,
		_droppedEventHistory = 0,
		_releasedEntities = {},
		_releasedEntityOrder = {},
		_maxEntityTombstones = maxEntityTombstones,
	}, ProjectileSolver)
	return (self :: unknown) :: Solver
end

local function schedule(self: Solver, scheduledTime: number, payload: InternalPayload)
	local _, reason = self._scheduler:Schedule({
		scheduledTime = scheduledTime,
		kind = payload.eventKind,
		payload = payload,
	})
	assert(reason == nil, "could not schedule projectile event: " .. (reason or "Unknown"))
end

local function scheduleVisualRelease(self: Solver, entity: InternalEntity, releaseAt: number)
	if releaseAt <= entity.visualReleaseScheduledAt then
		return
	end

	schedule(self, releaseAt, {
		eventKind = "VisualReleased",
		entityId = entity.id,
		generation = entity.generation,
	})
	entity.visualReleaseScheduledAt = releaseAt
end

local function extendVisualReleaseFromLifecycle(self: Solver, entity: InternalEntity)
	local lifecycleReleaseAt = entity.visualLifecycle:GetReleaseAt()
	if lifecycleReleaseAt == nil or lifecycleReleaseAt <= entity.releaseAt then
		return
	end

	entity.releaseAt = lifecycleReleaseAt
	scheduleVisualRelease(self, entity, lifecycleReleaseAt)
end

local function appendEvent(self: Solver, event: ProjectileEvent, emitted: { ProjectileEvent })
	if #self._eventHistory >= self._maxEventHistory then
		table.remove(self._eventHistory, 1)
		self._droppedEventHistory += 1
	end
	table.insert(self._eventHistory, event)
	table.insert(emitted, copyEvent(event))
end

local function rememberReleasedEntity(self: Solver, entity: InternalEntity)
	local sampleTime = entity.logicalEndTime or math.min(self._simulationTime, entity.expiresAt)
	entity.age = math.max(0, sampleTime - entity.spawnTime)
	entity.position = samplePosition(entity, sampleTime)
	self._releasedEntities[entity.id] = publicEntity(entity)
	table.insert(self._releasedEntityOrder, entity.id)
	if #self._releasedEntityOrder > self._maxEntityTombstones then
		local removedId = table.remove(self._releasedEntityOrder, 1)
		if removedId ~= nil then
			self._releasedEntities[removedId] = nil
		end
	end
end

local function expandBurstChildren(burst: BurstDefinition?): { ExpandedBurstChild }
	if burst == nil or burst.trigger == Enums.BurstTrigger.Impact then
		return {}
	end
	if burst.expandedChildren ~= nil then
		local expanded = {}
		for _, child in ipairs(burst.expandedChildren) do
			table.insert(expanded, child)
		end
		return expanded
	end

	local expanded = {}
	local childIndex = 0
	for _, group in ipairs(burst.children) do
		assert(group.count >= 0 and group.count % 1 == 0, "burst child count must be a non-negative integer")
		for _ = 1, group.count do
			childIndex += 1
			table.insert(expanded, {
				childIndex = childIndex,
				groupId = group.id,
				recipeId = group.recipeId,
			})
		end
	end
	return expanded
end

function ProjectileSolver.Spawn(self: Solver, compiledSpell: CompiledSpell, runtimeInput: RuntimeInput): number
	assertVector(runtimeInput.origin, "origin")
	assertVector(runtimeInput.direction, "direction")
	assert(isFinite(runtimeInput.rootSeed) and runtimeInput.rootSeed % 1 == 0, "rootSeed must be a finite integer")

	local blueprint, timeline, burst, costs = resolveBlueprint(compiledSpell)
	local lifetime = resolveLifetime(blueprint)
	local direction = unit(runtimeInput.direction)
	local origin = clone(runtimeInput.origin)
	local emitters = blueprint.presentation.emitters or blueprint.presentation.particles or {}
	local timelineRecords = timeline or {}
	local burstChildren = expandBurstChildren(burst)
	local scheduledCount = 3 + #emitters + #timelineRecords + #burstChildren
	-- Expiry, release, and per-emitter disable work can later be atomically
	-- replaced by cancellation work, so cancellation never needs extra queue room.
	assert(self._scheduler:CanSchedule(scheduledCount), "projectile event queue has no room for this spawn")

	local entityId = self._nextEntityId
	self._nextEntityId += 1
	local spawnTime = self._simulationTime
	local expiresAt = spawnTime + lifetime
	local visualLifecycle = VisualLifecycle.new()
	local releaseAt = expiresAt
	local emitterLifetimeById: { [string]: number } = {}
	local emitterDisableAtById: { [string]: number } = {}
	local latestEmitAtById: { [string]: number } = {}

	for _, emitter in ipairs(emitters) do
		assert(isFinite(emitter.enabledSeconds) and emitter.enabledSeconds >= 0, "emitter enabledSeconds must be finite and non-negative")
		visualLifecycle:RegisterEmitter({ id = emitter.id, lifetime = emitter.lifetime })
		emitterLifetimeById[emitter.id] = emitter.lifetime.max
		emitterDisableAtById[emitter.id] = spawnTime + math.min(emitter.enabledSeconds, lifetime)
	end
	for _, cue in ipairs(timelineRecords) do
		assert(isFinite(cue.time) and cue.time >= 0, "timeline cue time must be finite and non-negative")
		local emitterId = cue.emitterId
		if emitterId ~= nil and emitterLifetimeById[emitterId] ~= nil then
			local cueAt = spawnTime + cue.time
			if cue.kind == "Timeline.EmitParticles" then
				local latestEmitAt = latestEmitAtById[emitterId]
				if latestEmitAt == nil or cueAt > latestEmitAt then
					latestEmitAtById[emitterId] = cueAt
				end
			elseif cue.kind == "Timeline.SetEmitterEnabled" and cue.enabled == false then
				local disableAt = emitterDisableAtById[emitterId]
				if disableAt == nil or cueAt < disableAt then
					emitterDisableAtById[emitterId] = cueAt
				end
			end
		end
	end
	for emitterId, emitterLifetime in emitterLifetimeById do
		local disableAt = emitterDisableAtById[emitterId]
		if disableAt ~= nil then
			releaseAt = math.max(releaseAt, disableAt + emitterLifetime)
		end
		local latestEmitAt = latestEmitAtById[emitterId]
		if latestEmitAt ~= nil then
			releaseAt = math.max(releaseAt, latestEmitAt + emitterLifetime)
		end
	end

	local entity: InternalEntity = {
		id = entityId,
		logicalState = "Active",
		visualState = "Active",
		spawnTime = spawnTime,
		expiresAt = expiresAt,
		logicalEndTime = nil,
		releaseAt = releaseAt,
		age = 0,
		origin = origin,
		direction = direction,
		position = clone(origin),
		rootSeed = runtimeInput.rootSeed,
		cancelReason = nil,
		costs = costs,
		motion = resolveMotion(blueprint.motion, origin, direction),
		visualLifecycle = visualLifecycle,
		generation = 1,
		spawnEventEmitted = false,
		visualReleaseScheduledAt = -math.huge,
	}
	self._entities[entityId] = entity
	table.insert(self._entityOrder, entityId)

	schedule(self, spawnTime, {
		eventKind = "ProjectileSpawned",
		entityId = entityId,
		generation = entity.generation,
	})

	for index, cue in ipairs(timelineRecords) do
		assert(isFinite(cue.time) and cue.time >= 0, "timeline cue time must be finite and non-negative")
		schedule(self, spawnTime + cue.time, {
			eventKind = "TimelineCue",
			entityId = entityId,
			generation = entity.generation,
			timelineIndex = index,
			timelineCue = cue,
		})
	end

	if burst ~= nil and burst.trigger ~= Enums.BurstTrigger.Impact then
		local burstTime = if burst.time == nil then expiresAt else spawnTime + burst.time
		assert(isFinite(burstTime), "burst time must be finite")
		for _, child in ipairs(burstChildren) do
			schedule(self, burstTime, {
				eventKind = "BurstScheduled",
				entityId = entityId,
				generation = entity.generation,
				burstChildIndex = child.childIndex,
				burstGroupId = child.groupId,
				burstRecipeId = child.recipeId,
			})
		end
	end

	for _, emitter in ipairs(emitters) do
		schedule(self, spawnTime + math.min(emitter.enabledSeconds, lifetime), {
			eventKind = "EmitterDisabled",
			entityId = entityId,
			generation = entity.generation,
			emitterId = emitter.id,
		})
	end

	schedule(self, expiresAt, {
		eventKind = "ProjectileExpired",
		entityId = entityId,
		generation = entity.generation,
	})
	scheduleVisualRelease(self, entity, releaseAt)

	return entityId
end

type CancellationPlan = {
	generation: number,
	releaseAt: number,
	disabledEmitterIds: { string },
	replacements: { SchedulerScheduleInput },
	predicate: (event: SchedulerScheduledEvent) -> boolean,
}

local function cancellationPlan(entity: InternalEntity, scheduledTime: number, reason: string?): CancellationPlan
	local disabledEmitterIds = {}
	local releaseAt = scheduledTime
	for _, emitter in ipairs(entity.visualLifecycle:GetState().emitters) do
		local resolvedDisableAt = emitter.disableAt or scheduledTime
		if emitter.disableAt == nil then
			table.insert(disabledEmitterIds, emitter.id)
		end
		releaseAt = math.max(releaseAt, resolvedDisableAt + emitter.lifetimeMax)
		if emitter.latestEmitAt ~= nil then
			releaseAt = math.max(releaseAt, emitter.latestEmitAt + emitter.lifetimeMax)
		end
	end

	local generation = entity.generation + 1
	local cancelReason = reason or "Cancelled"
	local replacements: { SchedulerScheduleInput } = {
		{
			scheduledTime = scheduledTime,
			kind = "ProjectileCancelled",
			payload = {
				eventKind = "ProjectileCancelled",
				entityId = entity.id,
				generation = generation,
				reason = cancelReason,
			},
		},
	}
	for _, emitterId in ipairs(disabledEmitterIds) do
		table.insert(replacements, {
			scheduledTime = scheduledTime,
			kind = "EmitterDisabled",
			payload = {
				eventKind = "EmitterDisabled",
				entityId = entity.id,
				generation = generation,
				emitterId = emitterId,
				emitterStateRecorded = true,
			},
		})
	end
	table.insert(replacements, {
		scheduledTime = releaseAt,
		kind = "VisualReleased",
		payload = {
			eventKind = "VisualReleased",
			entityId = entity.id,
			generation = generation,
		},
	})

	local entityId = entity.id
	local priorGeneration = entity.generation
	local function predicate(event: SchedulerScheduledEvent): boolean
		local payload = (event.payload :: unknown) :: InternalPayload
		return payload ~= nil
			and payload.entityId == entityId
			and payload.generation == priorGeneration
			and payload.eventKind ~= "ProjectileSpawned"
	end

	return {
		generation = generation,
		releaseAt = releaseAt,
		disabledEmitterIds = disabledEmitterIds,
		replacements = replacements,
		predicate = predicate,
	}
end

local function validCancellationTime(self: Solver, entity: InternalEntity, scheduledTime: number): boolean
	return isFinite(scheduledTime)
		and timeReached(scheduledTime, entity.spawnTime)
		and timeReached(self._simulationTime, scheduledTime)
		and not timeReached(scheduledTime, entity.expiresAt)
end

-- Cancellation is valid only inside the projectile's elapsed logical lifetime.
-- Capacity is proven against an atomic replacement of this entity's future work.
function ProjectileSolver.CanCancelAt(self: Solver, entityId: number, scheduledTime: number): boolean
	local entity = self._entities[entityId]
	if entity == nil or entity.logicalState ~= "Active" or not validCancellationTime(self, entity, scheduledTime) then
		return false
	end
	local plan = cancellationPlan(entity, scheduledTime, nil)
	return self._scheduler:CanReplaceScheduled(plan.predicate, #plan.replacements)
end

function ProjectileSolver.CanCancel(self: Solver, entityId: number): boolean
	return self:CanCancelAt(entityId, self._simulationTime)
end

function ProjectileSolver.CancelAt(
	self: Solver,
	entityId: number,
	scheduledTime: number,
	reason: string?
): boolean
	local entity = self._entities[entityId]
	if entity == nil or entity.logicalState ~= "Active" or not validCancellationTime(self, entity, scheduledTime) then
		return false
	end

	local plan = cancellationPlan(entity, scheduledTime, reason)
	local _, replaceReason = self._scheduler:ReplaceScheduled(plan.predicate, plan.replacements)
	if replaceReason ~= nil then
		return false
	end

	entity.generation = plan.generation
	entity.logicalState = "Cancelled"
	entity.logicalEndTime = scheduledTime
	entity.cancelReason = reason or "Cancelled"
	entity.age = math.max(0, scheduledTime - entity.spawnTime)
	entity.position = samplePosition(entity, scheduledTime)

	local releaseAt, disabledIds = entity.visualLifecycle:RequestCancel(scheduledTime)
	assert(releaseAt == plan.releaseAt, "cancellation release plan drifted")
	assert(#disabledIds == #plan.disabledEmitterIds, "cancellation emitter plan drifted")
	entity.releaseAt = releaseAt
	entity.visualReleaseScheduledAt = releaseAt
	return true
end

function ProjectileSolver.Cancel(self: Solver, entityId: number, reason: string?): boolean
	return self:CancelAt(entityId, self._simulationTime, reason)
end

local function emitFromProcessed(
	self: Solver,
	processed: SchedulerProcessedEvent,
	emitted: { ProjectileEvent }
)
	local payload = (processed.payload :: unknown) :: InternalPayload
	local entity = self._entities[payload.entityId]
	if entity == nil then
		return
	end

	if payload.eventKind == "ProjectileSpawned" then
		if entity.spawnEventEmitted then
			return
		end
		entity.spawnEventEmitted = true
	elseif payload.generation ~= entity.generation then
		return
	elseif payload.eventKind == "TimelineCue" then
		if entity.visualState ~= "Active" or processed.scheduledTime > entity.releaseAt then
			return
		end
		local timelineCue = payload.timelineCue
		if timelineCue == nil then
			return
		end
		if timelineCue.kind == "Timeline.EmitParticles" then
			local emitterId = timelineCue.emitterId
			if emitterId ~= nil then
				entity.visualLifecycle:RecordEmission(emitterId, processed.scheduledTime)
				extendVisualReleaseFromLifecycle(self, entity)
			end
		elseif timelineCue.kind == "Timeline.SetEmitterEnabled" then
			if timelineCue.enabled ~= false then
				return
			end
			local emitterId = timelineCue.emitterId
			if emitterId ~= nil then
				entity.visualLifecycle:DisableEmitter(emitterId, processed.scheduledTime)
			end
		end
	elseif payload.eventKind == "BurstScheduled" then
		if entity.logicalState ~= "Active" then
			return
		end
	elseif payload.eventKind == "EmitterDisabled" then
		local emitterId = payload.emitterId
		if emitterId == nil then
			return
		end
		if payload.emitterStateRecorded ~= true
			and not entity.visualLifecycle:DisableEmitter(emitterId, processed.scheduledTime)
		then
			return
		end
	elseif payload.eventKind == "ProjectileExpired" then
		if entity.logicalState ~= "Active" then
			return
		end
		entity.logicalState = "Expired"
		entity.logicalEndTime = processed.scheduledTime
		entity.visualLifecycle:RequestRelease(processed.scheduledTime)
		extendVisualReleaseFromLifecycle(self, entity)
	elseif payload.eventKind == "ProjectileCancelled" then
		if entity.logicalState ~= "Cancelled" then
			return
		end
	elseif payload.eventKind == "VisualReleased" then
		if entity.visualState == "Released" then
			return
		end
		extendVisualReleaseFromLifecycle(self, entity)
		if processed.scheduledTime < entity.releaseAt then
			scheduleVisualRelease(self, entity, entity.releaseAt)
			return
		end
		if not entity.visualLifecycle:CanRelease(processed.scheduledTime) then
			local correctedReleaseAt = entity.visualLifecycle:GetReleaseAt()
			if correctedReleaseAt ~= nil and correctedReleaseAt > processed.scheduledTime then
				entity.releaseAt = math.max(entity.releaseAt, correctedReleaseAt)
				scheduleVisualRelease(self, entity, entity.releaseAt)
			end
			return
		end
		entity.visualState = "Released"
	end

	local event: ProjectileEvent = {
		kind = payload.eventKind,
		entityId = entity.id,
		sequence = processed.sequence,
		scheduledTime = processed.scheduledTime,
		processedAt = processed.processedAt,
		position = samplePosition(entity, processed.scheduledTime),
		emitterId = payload.emitterId,
		timelineIndex = payload.timelineIndex,
		timelineCue = payload.timelineCue,
		burstChildIndex = payload.burstChildIndex,
		burstGroupId = payload.burstGroupId,
		burstRecipeId = payload.burstRecipeId,
		rootSeed = if payload.eventKind == "BurstScheduled" then entity.rootSeed else nil,
		reason = payload.reason,
	}
	appendEvent(self, event, emitted)
	if payload.eventKind == "VisualReleased" then
		rememberReleasedEntity(self, entity)
		self._entities[entity.id] = nil
	end
end

function ProjectileSolver.Advance(self: Solver, deltaSeconds: number): AdvanceResult
	local schedulerResult = self._scheduler:Advance(deltaSeconds)
	self._simulationTime = schedulerResult.simulationTime
	local emitted = {}
	for _, processed in ipairs(self._scheduler:Drain()) do
		emitFromProcessed(self, processed, emitted)
	end

	local activeOrder = {}
	for _, entityId in ipairs(self._entityOrder) do
		local entity = self._entities[entityId]
		if entity ~= nil then
			local sampleTime = self._simulationTime
			if entity.logicalEndTime ~= nil then
				sampleTime = math.min(sampleTime, entity.logicalEndTime)
			else
				sampleTime = math.min(sampleTime, entity.expiresAt)
			end
			entity.age = math.max(0, sampleTime - entity.spawnTime)
			entity.position = samplePosition(entity, sampleTime)
			table.insert(activeOrder, entityId)
		end
	end
	self._entityOrder = activeOrder

	return {
		simulationTime = schedulerResult.simulationTime,
		steps = schedulerResult.steps,
		events = emitted,
		droppedBacklogSeconds = schedulerResult.droppedBacklogSeconds,
		totalDroppedBacklogSeconds = schedulerResult.totalDroppedBacklogSeconds,
		queuedEvents = schedulerResult.queuedEvents,
		eventBackpressure = schedulerResult.eventBackpressure,
		droppedEventHistory = self._droppedEventHistory,
	}
end

function ProjectileSolver.GetEntity(self: Solver, entityId: number): Entity?
	local entity = self._entities[entityId]
	if entity == nil then
		local released = self._releasedEntities[entityId]
		return if released == nil then nil else copyEntity(released)
	end

	return publicEntity(entity)
end

function ProjectileSolver.GetEvents(self: Solver, clear: boolean?): { ProjectileEvent }
	local events = {}
	for _, event in ipairs(self._eventHistory) do
		table.insert(events, copyEvent(event))
	end
	if clear then
		self._eventHistory = {}
	end
	return events
end

ProjectileSolver.New = ProjectileSolver.new

return table.freeze(ProjectileSolver)

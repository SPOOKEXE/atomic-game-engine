--!strict

local ActorState = require(script.Parent.ActorState)
local Enums = require(script.Parent.Enums)
local TableUtil = require(script.Parent.TableUtil)
local Types = require(script.Parent.Types)

type WorkKind = "Remove" | "Cleanse" | "Apply" | "Tick" | "Expire"

type Work = {
	kind: WorkKind,
	phase: number,
	scheduledTime: number,
	sequence: number,
	operationId: string,
	reservedLifecycleSlots: number,
	payload: any,
}

type InternalInstance = {
	instanceId: string,
	definitionId: string,
	definitionVersion: number,
	sourceCastId: string,
	sourceActorId: string,
	targetActorId: string,
	appliedAt: number,
	expiresAt: number,
	nextTickTime: number?,
	nextTickOrdinal: number,
	ticksEmitted: number,
	stacks: number,
	potency: number,
	sequence: number,
	generation: number,
}

export type Config = {
	maxStatusesPerActor: number?,
	maxStatusTicksPerAdvance: number?,
	maxEffectCommandsPerAdvance: number?,
	maxLedgerEntries: number?,
	maxQueuedOperations: number?,
}

export type AdvanceResult = {
	currentTime: number,
	events: { any },
	commands: { any },
	ticksProcessed: number,
	processedEffects: number,
	pendingOperations: number,
}

export type Solver = {
	_definitionsByKey: { [string]: any },
	_actorsById: { [string]: any },
	_instancesById: { [string]: InternalInstance },
	_instancesByActor: { [string]: { string } },
	_queue: { Work },
	_nextSequence: number,
	_currentTime: number,
	_acceptedOperations: { [string]: boolean },
	_acceptedOrder: { string },
	_processedOperations: { [string]: boolean },
	_processedOrder: { string },
	_maxStatusesPerActor: number,
	_maxStatusTicksPerAdvance: number,
	_maxEffectCommandsPerAdvance: number,
	_maxLedgerEntries: number,
	_maxQueuedOperations: number,
	_reservedLifecycleSlots: number,

	canQueueApply: (self: Solver, input: any) -> (boolean, string?),
	canQueueCleanse: (self: Solver, input: any) -> (boolean, string?),
	canQueueRemove: (self: Solver, input: any) -> (boolean, string?),
	preflightBatch: (self: Solver, requests: { any }) -> (boolean, string?),
	queueApply: (self: Solver, input: any) -> (boolean, string?),
	queueCleanse: (self: Solver, input: any) -> (boolean, string?),
	queueRemove: (self: Solver, input: any) -> (boolean, string?),
	advance: (self: Solver, toTime: number, processedAt: number?, maxEffectCommands: number?) -> AdvanceResult,
	getStatuses: (self: Solver, actorId: string) -> { any },
	getStatus: (self: Solver, instanceId: string) -> any?,
}

local StatusEffectSolver = {}
StatusEffectSolver.__index = StatusEffectSolver

local PHASE_REMOVE = 1
local PHASE_APPLY = 2
local PHASE_TICK = 3
local PHASE_EXPIRE = 4

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function timeReached(now: number, scheduledTime: number): boolean
	local tolerance = math.max(1, math.abs(now), math.abs(scheduledTime)) * 1e-9
	return scheduledTime <= now + tolerance
end

local function definitionKey(id: string, version: number): string
	return id .. "@" .. tostring(version)
end

local function lifecycleSlotCount(definition: any): number
	return 1 + (if definition.tickInterval ~= nil and #definition.periodicEffects > 0 then 1 else 0)
end

local function publicInstance(instance: InternalInstance): any
	return {
		instanceId = instance.instanceId,
		definitionId = instance.definitionId,
		definitionVersion = instance.definitionVersion,
		sourceCastId = instance.sourceCastId,
		sourceActorId = instance.sourceActorId,
		targetActorId = instance.targetActorId,
		appliedAt = instance.appliedAt,
		expiresAt = instance.expiresAt,
		nextTickTime = instance.nextTickTime,
		stacks = instance.stacks,
		potency = instance.potency,
		sequence = instance.sequence,
	}
end

local function comesBefore(a: Work, b: Work): boolean
	if a.scheduledTime ~= b.scheduledTime then
		return a.scheduledTime < b.scheduledTime
	end
	if a.phase ~= b.phase then
		return a.phase < b.phase
	end
	return a.sequence < b.sequence
end

local function insertWork(queue: { Work }, work: Work)
	local low = 1
	local high = #queue + 1
	while low < high do
		local middle = math.floor((low + high) / 2)
		local current = queue[middle]
		if current ~= nil and comesBefore(current, work) then
			low = middle + 1
		else
			high = middle
		end
	end
	table.insert(queue, low, work)
end

local function boundedRemember(set: { [string]: boolean }, order: { string }, id: string, maximum: number)
	if set[id] then
		return
	end
	set[id] = true
	table.insert(order, id)
	if #order > maximum then
		local removed = table.remove(order, 1)
		if removed ~= nil then
			set[removed] = nil
		end
	end
end

local function lifecycleWorkIsLive(self: Solver, work: Work): boolean
	if work.kind ~= "Tick" and work.kind ~= "Expire" then
		return true
	end
	local instance = self._instancesById[work.payload.instanceId]
	return instance ~= nil and instance.generation == work.payload.generation
end

local function liveQueueCount(self: Solver): number
	local count = 0
	for _, work in ipairs(self._queue) do
		if lifecycleWorkIsLive(self, work) then
			count += 1
		end
	end
	return count
end

local function pruneInvalidLifecycleWork(self: Solver)
	for index = #self._queue, 1, -1 do
		if not lifecycleWorkIsLive(self, self._queue[index]) then
			table.remove(self._queue, index)
		end
	end
end

local function addWork(
	self: Solver,
	kind: WorkKind,
	phase: number,
	scheduledTime: number,
	operationId: string,
	payload: any,
	reservedLifecycleSlots: number
): boolean
	if #self._queue >= self._maxQueuedOperations then
		return false
	end
	local work: Work = {
		kind = kind,
		phase = phase,
		scheduledTime = scheduledTime,
		sequence = self._nextSequence,
		operationId = operationId,
		reservedLifecycleSlots = reservedLifecycleSlots,
		payload = payload,
	}
	self._nextSequence += 1
	insertWork(self._queue, work)
	return true
end

local function acceptExternalOperation(self: Solver, operationId: any): (boolean, string?)
	if type(operationId) ~= "string" or operationId == "" then
		return false, "InvalidOperationId"
	end
	if self._acceptedOperations[operationId] or self._processedOperations[operationId] then
		return false, "DuplicateOperationId"
	end
	if #self._acceptedOrder >= self._maxLedgerEntries then
		return false, "LedgerFull"
	end
	self._acceptedOperations[operationId] = true
	table.insert(self._acceptedOrder, operationId)
	return true, nil
end

local function forgetAcceptedOperation(self: Solver, operationId: string)
	self._acceptedOperations[operationId] = nil
	for index, acceptedId in ipairs(self._acceptedOrder) do
		if acceptedId == operationId then
			table.remove(self._acceptedOrder, index)
			break
		end
	end
end

local function markExternalOperationProcessed(self: Solver, operationId: string)
	forgetAcceptedOperation(self, operationId)
	boundedRemember(self._processedOperations, self._processedOrder, operationId, self._maxLedgerEntries)
end

local function definitionFor(self: Solver, instance: InternalInstance): any
	return self._definitionsByKey[definitionKey(instance.definitionId, instance.definitionVersion)]
end

local function syncActorStatuses(self: Solver, actorId: string)
	local actor = self._actorsById[actorId]
	if actor == nil then
		return
	end
	local statuses = {}
	for _, instanceId in ipairs(self._instancesByActor[actorId] or {}) do
		local instance = self._instancesById[instanceId]
		if instance ~= nil then
			table.insert(statuses, publicInstance(instance))
		end
	end
	table.sort(statuses, function(a: any, b: any): boolean
		if a.sequence == b.sequence then
			return a.instanceId < b.instanceId
		end
		return a.sequence < b.sequence
	end)
	actor.statuses = statuses
end

local function actorStatusCount(self: Solver, actorId: string): number
	local count = 0
	for _, instanceId in ipairs(self._instancesByActor[actorId] or {}) do
		if self._instancesById[instanceId] ~= nil then
			count += 1
		end
	end
	return count
end

local function matchingInstances(self: Solver, actorId: string, definitionId: string, version: number): { InternalInstance }
	local result = {}
	for _, instanceId in ipairs(self._instancesByActor[actorId] or {}) do
		local instance = self._instancesById[instanceId]
		if instance ~= nil and instance.definitionId == definitionId and instance.definitionVersion == version then
			table.insert(result, instance)
		end
	end
	table.sort(result, function(a: InternalInstance, b: InternalInstance): boolean
		if a.sequence == b.sequence then
			return a.instanceId < b.instanceId
		end
		return a.sequence < b.sequence
	end)
	return result
end

local function removeInstance(self: Solver, instance: InternalInstance)
	self._instancesById[instance.instanceId] = nil
	local actorIds = self._instancesByActor[instance.targetActorId] or {}
	for index, instanceId in ipairs(actorIds) do
		if instanceId == instance.instanceId then
			table.remove(actorIds, index)
			break
		end
	end
	syncActorStatuses(self, instance.targetActorId)
end

local function modifierCommand(
	kind: "Add" | "Update" | "Remove",
	definition: any,
	instance: InternalInstance,
	scheduledTime: number,
	operationId: string
): any
	return {
		kind = "StatusModifiersChanged",
		action = kind,
		operationId = operationId,
		instanceId = instance.instanceId,
		definitionId = instance.definitionId,
		definitionVersion = instance.definitionVersion,
		castId = instance.sourceCastId,
		sourceActorId = instance.sourceActorId,
		targetActorId = instance.targetActorId,
		scheduledTime = scheduledTime,
		stacks = instance.stacks,
		potency = instance.potency,
		modifiers = TableUtil.deepCopy(definition.modifiers),
	}
end

local function isAt(a: number, b: number, interval: number): boolean
	return math.abs(a - b) <= interval * 1e-9
end

local function scheduleNextTick(self: Solver, instance: InternalInstance, definition: any)
	instance.nextTickTime = nil
	if definition.tickInterval == nil or #definition.periodicEffects == 0 then
		return
	end
	if instance.ticksEmitted >= Types.Limits.MAX_STATUS_TICKS_PER_INSTANCE then
		return
	end
	local tickAt = instance.appliedAt + instance.nextTickOrdinal * definition.tickInterval
	local atExpiry = isAt(tickAt, instance.expiresAt, definition.tickInterval)
	if tickAt > instance.expiresAt and not atExpiry then
		return
	end
	if atExpiry and not definition.tickAtExpiry then
		return
	end
	instance.nextTickTime = tickAt
	local queued = addWork(self, "Tick", PHASE_TICK, tickAt, instance.instanceId .. ":tick:" .. tostring(instance.nextTickOrdinal), {
		instanceId = instance.instanceId,
		generation = instance.generation,
		tickOrdinal = instance.nextTickOrdinal,
	}, 0)
	assert(queued, "reserved status lifecycle capacity was exhausted while scheduling a tick")
end

local function scheduleLifecycle(self: Solver, instance: InternalInstance, definition: any)
	scheduleNextTick(self, instance, definition)
	local queued = addWork(self, "Expire", PHASE_EXPIRE, instance.expiresAt, instance.instanceId .. ":expire", {
		instanceId = instance.instanceId,
		generation = instance.generation,
	}, 0)
	assert(queued, "reserved status lifecycle capacity was exhausted while scheduling expiry")
end

local function uniqueInstanceId(self: Solver, operationId: string): string
	local base = operationId .. ":instance"
	if self._instancesById[base] == nil then
		return base
	end
	return base .. ":" .. tostring(self._nextSequence)
end

local function createInstance(self: Solver, input: any, definition: any, expiresAt: number): InternalInstance
	local sequence = self._nextSequence
	local instance: InternalInstance = {
		instanceId = uniqueInstanceId(self, input.operationId),
		definitionId = input.definitionId,
		definitionVersion = input.definitionVersion,
		sourceCastId = input.castId,
		sourceActorId = input.sourceActorId,
		targetActorId = input.targetActorId,
		appliedAt = input.scheduledTime,
		expiresAt = expiresAt,
		nextTickTime = nil,
		nextTickOrdinal = if definition.tickOnApply then 0 else 1,
		ticksEmitted = 0,
		stacks = math.min(input.stacks, definition.maxStacks),
		potency = input.potency,
		sequence = sequence,
		generation = 1,
	}
	self._instancesById[instance.instanceId] = instance
	local actorInstances = self._instancesByActor[input.targetActorId]
	if actorInstances == nil then
		actorInstances = {}
		self._instancesByActor[input.targetActorId] = actorInstances
	end
	table.insert(actorInstances, instance.instanceId)
	scheduleLifecycle(self, instance, definition)
	syncActorStatuses(self, input.targetActorId)
	return instance
end

local function baseEvent(work: Work, input: any, processedAt: number): any
	return {
		operationId = work.operationId,
		castId = input.castId,
		sourceActorId = input.sourceActorId,
		targetActorId = input.targetActorId,
		scheduledTime = work.scheduledTime,
		processedAt = processedAt,
	}
end

local function emitRejected(work: Work, input: any, processedAt: number, reason: string, events: { any })
	local event = baseEvent(work, input, processedAt)
	event.kind = "StatusRejected"
	event.reason = reason
	event.definitionId = input.definitionId
	event.definitionVersion = input.definitionVersion
	table.insert(events, event)
end

local function processApply(self: Solver, work: Work, processedAt: number, events: { any }, commands: { any })
	local input = work.payload
	local definition = self._definitionsByKey[definitionKey(input.definitionId, input.definitionVersion)]
	if definition == nil then
		emitRejected(work, input, processedAt, "MissingDefinition", events)
		return
	end
	if self._actorsById[input.targetActorId] == nil then
		emitRejected(work, input, processedAt, "MissingTarget", events)
		return
	end
	local duration = input.durationOverride or definition.duration
	local expiresAt = work.scheduledTime + duration
	local matches = matchingInstances(self, input.targetActorId, input.definitionId, input.definitionVersion)

	if definition.stackPolicy == "Independent" then
		if #matches >= definition.maxIndependentInstances then
			emitRejected(work, input, processedAt, "IndependentCap", events)
			return
		end
		if actorStatusCount(self, input.targetActorId) >= self._maxStatusesPerActor then
			emitRejected(work, input, processedAt, "ActorStatusCap", events)
			return
		end
		local instance = createInstance(self, input, definition, expiresAt)
		local event = baseEvent(work, input, processedAt)
		event.kind = "StatusApplied"
		event.instance = publicInstance(instance)
		event.cueId = definition.cueId
		table.insert(events, event)
		table.insert(commands, modifierCommand("Add", definition, instance, work.scheduledTime, work.operationId))
		return
	end

	local existing = matches[1]
	if existing == nil then
		if actorStatusCount(self, input.targetActorId) >= self._maxStatusesPerActor then
			emitRejected(work, input, processedAt, "ActorStatusCap", events)
			return
		end
		local instance = createInstance(self, input, definition, expiresAt)
		local event = baseEvent(work, input, processedAt)
		event.kind = "StatusApplied"
		event.instance = publicInstance(instance)
		event.cueId = definition.cueId
		table.insert(events, event)
		table.insert(commands, modifierCommand("Add", definition, instance, work.scheduledTime, work.operationId))
		return
	end

	if definition.stackPolicy == "ReplaceIfStronger" then
		local stronger = input.potency > existing.potency
			or (input.potency == existing.potency and expiresAt > existing.expiresAt)
		if not stronger then
			local event = baseEvent(work, input, processedAt)
			event.kind = "StatusMergeNoOp"
			event.instanceId = existing.instanceId
			event.reason = "NotStronger"
			table.insert(events, event)
			return
		end
		local replacedId = existing.instanceId
		removeInstance(self, existing)
		pruneInvalidLifecycleWork(self)
		table.insert(commands, modifierCommand("Remove", definition, existing, work.scheduledTime, work.operationId .. ":replace-remove"))
		local replacement = createInstance(self, input, definition, expiresAt)
		local event = baseEvent(work, input, processedAt)
		event.kind = "StatusReplaced"
		event.replacedInstanceId = replacedId
		event.instance = publicInstance(replacement)
		event.cueId = definition.cueId
		table.insert(events, event)
		table.insert(commands, modifierCommand("Add", definition, replacement, work.scheduledTime, work.operationId))
		return
	end

	local priorPotency = existing.potency
	local priorStacks = existing.stacks
	local priorExpiry = existing.expiresAt
	if definition.stackPolicy == "AddStacks" then
		existing.stacks = math.min(definition.maxStacks, existing.stacks + input.stacks)
	end
	if input.potency > existing.potency then
		existing.potency = input.potency
		existing.sourceCastId = input.castId
		existing.sourceActorId = input.sourceActorId
	end
	existing.expiresAt = expiresAt
	existing.generation += 1
	pruneInvalidLifecycleWork(self)
	scheduleLifecycle(self, existing, definition)
	syncActorStatuses(self, existing.targetActorId)
	local event = baseEvent(work, input, processedAt)
	event.kind = "StatusMerged"
	event.instance = publicInstance(existing)
	event.stackPolicy = definition.stackPolicy
	event.priorStacks = priorStacks
	event.priorPotency = priorPotency
	event.priorExpiresAt = priorExpiry
	event.refreshedAtExpiry = work.scheduledTime == priorExpiry
	event.cueId = definition.cueId
	table.insert(events, event)
	table.insert(commands, modifierCommand("Update", definition, existing, work.scheduledTime, work.operationId))
end

local function tagsMatch(definition: any, filter: any): boolean
	local tags: { [string]: boolean } = {}
	for _, tag in ipairs(definition.tags) do
		tags[tag] = true
	end
	if #filter.includeAnyTags > 0 then
		local found = false
		for _, tag in ipairs(filter.includeAnyTags) do
			if tags[tag] then
				found = true
				break
			end
		end
		if not found then
			return false
		end
	end
	for _, tag in ipairs(filter.includeAllTags) do
		if not tags[tag] then
			return false
		end
	end
	for _, tag in ipairs(filter.excludeAnyTags) do
		if tags[tag] then
			return false
		end
	end
	return true
end

local function processRemove(self: Solver, work: Work, processedAt: number, events: { any }, commands: { any })
	local input = work.payload
	local instance = self._instancesById[input.instanceId]
	if instance == nil then
		local event = baseEvent(work, input, processedAt)
		event.kind = "StatusRemoveNoOp"
		event.instanceId = input.instanceId
		table.insert(events, event)
		return
	end
	local definition = definitionFor(self, instance)
	removeInstance(self, instance)
	pruneInvalidLifecycleWork(self)
	local event = baseEvent(work, input, processedAt)
	event.kind = "StatusRemoved"
	event.instanceId = instance.instanceId
	event.definitionId = instance.definitionId
	event.definitionVersion = instance.definitionVersion
	table.insert(events, event)
	table.insert(commands, modifierCommand("Remove", definition, instance, work.scheduledTime, work.operationId))
end

local function processCleanse(self: Solver, work: Work, processedAt: number, events: { any }, commands: { any })
	local input = work.payload
	local candidates: { InternalInstance } = {}
	for _, instanceId in ipairs(self._instancesByActor[input.targetActorId] or {}) do
		local instance = self._instancesById[instanceId]
		if instance ~= nil and tagsMatch(definitionFor(self, instance), input) then
			table.insert(candidates, instance)
		end
	end
	table.sort(candidates, function(a: InternalInstance, b: InternalInstance): boolean
		if a.sequence == b.sequence then
			return a.instanceId < b.instanceId
		end
		return a.sequence < b.sequence
	end)
	local removed = 0
	for _, instance in ipairs(candidates) do
		if removed >= input.maxRemovals then
			break
		end
		local definition = definitionFor(self, instance)
		removeInstance(self, instance)
		removed += 1
		local event = baseEvent(work, input, processedAt)
		event.kind = "StatusCleansed"
		event.operationId = work.operationId .. ":remove:" .. instance.instanceId
		event.parentOperationId = work.operationId
		event.instanceId = instance.instanceId
		event.definitionId = instance.definitionId
		event.definitionVersion = instance.definitionVersion
		table.insert(events, event)
		table.insert(
			commands,
			modifierCommand("Remove", definition, instance, work.scheduledTime, work.operationId .. ":remove:" .. instance.instanceId)
		)
	end
	local event = baseEvent(work, input, processedAt)
	event.kind = "CleanseCompleted"
	event.removedCount = removed
	table.insert(events, event)
	pruneInvalidLifecycleWork(self)
end

type TickPlan = {
	instance: InternalInstance,
	definition: any,
	atExpiry: boolean,
	effectCount: number,
}

local function planTick(self: Solver, work: Work): TickPlan?
	local payload = work.payload
	local instance = self._instancesById[payload.instanceId]
	if instance == nil or instance.generation ~= payload.generation or instance.nextTickOrdinal ~= payload.tickOrdinal then
		return nil
	end
	local definition = definitionFor(self, instance)
	local interval = definition.tickInterval
	if interval == nil then
		return nil
	end
	local expectedTime = instance.appliedAt + payload.tickOrdinal * interval
	if not isAt(expectedTime, work.scheduledTime, interval) then
		return nil
	end
	local atExpiry = isAt(work.scheduledTime, instance.expiresAt, interval)
	if work.scheduledTime > instance.expiresAt and not atExpiry then
		return nil
	end
	if atExpiry and not definition.tickAtExpiry then
		return nil
	end
	if self._processedOperations[work.operationId] then
		return nil
	end
	return {
		instance = instance,
		definition = definition,
		atExpiry = atExpiry,
		effectCount = #definition.periodicEffects,
	}
end

local function processTick(self: Solver, work: Work, processedAt: number, events: { any }, plan: TickPlan)
	local payload = work.payload
	local instance = plan.instance
	local definition = plan.definition
	boundedRemember(self._processedOperations, self._processedOrder, work.operationId, self._maxLedgerEntries)
	local tickEvent = {
		kind = "StatusTick",
		operationId = work.operationId,
		tickId = work.operationId,
		instanceId = instance.instanceId,
		definitionId = instance.definitionId,
		definitionVersion = instance.definitionVersion,
		castId = instance.sourceCastId,
		sourceActorId = instance.sourceActorId,
		targetActorId = instance.targetActorId,
		scheduledTime = work.scheduledTime,
		processedAt = processedAt,
		tickOrdinal = payload.tickOrdinal,
		atExpiry = plan.atExpiry,
	}
	table.insert(events, tickEvent)
	for effectIndex, effect in ipairs(definition.periodicEffects) do
		local actor = self._actorsById[instance.targetActorId]
		if actor ~= nil then
			local amount = effect.amount * instance.potency * instance.stacks
			local nextActor, delta
			if effect.kind == Enums.EffectKind.Damage then
				nextActor, delta = ActorState.damage(actor, amount)
			else
				nextActor, delta = ActorState.heal(actor, amount)
			end
			self._actorsById[instance.targetActorId] = nextActor
			syncActorStatuses(self, instance.targetActorId)
			table.insert(events, {
				kind = if effect.kind == Enums.EffectKind.Damage then "DamageApplied" else "HealApplied",
				effectKind = effect.kind,
				operationId = work.operationId .. ":effect:" .. tostring(effectIndex),
				tickId = work.operationId,
				instanceId = instance.instanceId,
				castId = instance.sourceCastId,
				sourceActorId = instance.sourceActorId,
				targetActorId = instance.targetActorId,
				scheduledTime = work.scheduledTime,
				processedAt = processedAt,
				damageType = effect.damageType,
				sourceTags = TableUtil.deepCopy(effect.sourceTags),
				requestedAmount = delta.requestedAmount,
				appliedAmount = delta.appliedAmount,
				beforeHealth = delta.beforeHealth,
				afterHealth = delta.afterHealth,
				maxHealth = delta.maxHealth,
			})
		end
	end
	instance.ticksEmitted += 1
	instance.nextTickOrdinal += 1
	scheduleNextTick(self, instance, definition)
	syncActorStatuses(self, instance.targetActorId)
end

local function processExpire(self: Solver, work: Work, processedAt: number, events: { any }, commands: { any })
	local payload = work.payload
	local instance = self._instancesById[payload.instanceId]
	if instance == nil or instance.generation ~= payload.generation or not isAt(instance.expiresAt, work.scheduledTime, 1) then
		return
	end
	if self._processedOperations[work.operationId] then
		return
	end
	boundedRemember(self._processedOperations, self._processedOrder, work.operationId, self._maxLedgerEntries)
	local definition = definitionFor(self, instance)
	removeInstance(self, instance)
	pruneInvalidLifecycleWork(self)
	table.insert(events, {
		kind = "StatusExpired",
		operationId = work.operationId,
		instanceId = instance.instanceId,
		definitionId = instance.definitionId,
		definitionVersion = instance.definitionVersion,
		castId = instance.sourceCastId,
		sourceActorId = instance.sourceActorId,
		targetActorId = instance.targetActorId,
		scheduledTime = work.scheduledTime,
		processedAt = processedAt,
	})
	table.insert(commands, modifierCommand("Remove", definition, instance, work.scheduledTime, work.operationId))
end

function StatusEffectSolver.new(definitions: { any }, actorsById: { [string]: any }, config: Config?): Solver
	local supplied: Config = if config == nil then {} else config
	local maxStatuses = supplied.maxStatusesPerActor or Types.DefaultBudgets.maxStatusesPerActor
	local maxTicks = supplied.maxStatusTicksPerAdvance or Types.DefaultBudgets.maxStatusTicksPerAdvance
	local maxEffects = supplied.maxEffectCommandsPerAdvance or Types.DefaultBudgets.maxEffectCommands
	local maxLedger = supplied.maxLedgerEntries or Types.Limits.MAX_STATUS_LEDGER_ENTRIES
	local maxQueue = supplied.maxQueuedOperations or Types.Limits.MAX_STATUS_LEDGER_ENTRIES
	assert(maxStatuses > 0 and maxStatuses % 1 == 0, "maxStatusesPerActor must be a positive integer")
	assert(maxTicks > 0 and maxTicks % 1 == 0, "maxStatusTicksPerAdvance must be a positive integer")
	assert(maxEffects > 0 and maxEffects % 1 == 0, "maxEffectCommandsPerAdvance must be a positive integer")
	assert(maxLedger > 0 and maxLedger % 1 == 0, "maxLedgerEntries must be a positive integer")
	assert(maxQueue > 0 and maxQueue % 1 == 0, "maxQueuedOperations must be a positive integer")
	local registry: { [string]: any } = {}
	for _, definition in ipairs(definitions) do
		local key = definitionKey(definition.id, definition.version)
		assert(registry[key] == nil, "duplicate status definition ID/version")
		registry[key] = TableUtil.deepFreeze(TableUtil.deepCopy(definition))
	end
	local self = setmetatable({
		_definitionsByKey = registry,
		_actorsById = actorsById,
		_instancesById = {},
		_instancesByActor = {},
		_queue = {},
		_nextSequence = 1,
		_currentTime = 0,
		_acceptedOperations = {},
		_acceptedOrder = {},
		_processedOperations = {},
		_processedOrder = {},
		_maxStatusesPerActor = maxStatuses,
		_maxStatusTicksPerAdvance = maxTicks,
		_maxEffectCommandsPerAdvance = maxEffects,
		_maxLedgerEntries = maxLedger,
		_maxQueuedOperations = maxQueue,
		_reservedLifecycleSlots = 0,
	}, StatusEffectSolver)
	local solver = (self :: unknown) :: Solver
	for actorId in pairs(actorsById) do
		solver._instancesByActor[actorId] = {}
		syncActorStatuses(solver, actorId)
	end
	return solver
end

local function validateCommonInput(self: Solver, input: any): (boolean, string?)
	if type(input) ~= "table" or getmetatable(input) ~= nil then
		return false, "InvalidInput"
	end
	if type(input.operationId) ~= "string" or input.operationId == "" then
		return false, "InvalidOperationId"
	end
	if type(input.castId) ~= "string"
		or input.castId == ""
		or type(input.sourceActorId) ~= "string"
		or input.sourceActorId == ""
		or type(input.targetActorId) ~= "string"
		or input.targetActorId == ""
	then
		return false, "InvalidAttribution"
	end
	if not isFinite(input.scheduledTime) or input.scheduledTime < 0 then
		return false, "InvalidScheduledTime"
	end
	if self._actorsById[input.targetActorId] == nil then
		return false, "MissingTarget"
	end
	return true, nil
end

local function validateStringArray(value: any): boolean
	if type(value) ~= "table" or getmetatable(value) ~= nil then
		return false
	end
	local count = 0
	local maximum = 0
	for key, child in next, value do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 or type(child) ~= "string" then
			return false
		end
		count += 1
		maximum = math.max(maximum, key)
	end
	return count == maximum
end

local function validateStatusDefinition(definition: any): boolean
	if type(definition) ~= "table"
		or getmetatable(definition) ~= nil
		or type(definition.id) ~= "string"
		or definition.id == ""
		or not isFinite(definition.version)
		or definition.version < 1
		or definition.version % 1 ~= 0
		or not validateStringArray(definition.tags)
		or not isFinite(definition.duration)
		or definition.duration <= 0
		or definition.duration > Types.Limits.MAX_STATUS_DURATION
		or type(definition.tickOnApply) ~= "boolean"
		or type(definition.tickAtExpiry) ~= "boolean"
		or (definition.stackPolicy ~= "RefreshDuration"
			and definition.stackPolicy ~= "AddStacks"
			and definition.stackPolicy ~= "ReplaceIfStronger"
			and definition.stackPolicy ~= "Independent")
		or not isFinite(definition.maxStacks)
		or definition.maxStacks < 1
		or definition.maxStacks > Types.Limits.MAX_STATUS_STACKS
		or definition.maxStacks % 1 ~= 0
		or not isFinite(definition.maxIndependentInstances)
		or definition.maxIndependentInstances < 1
		or definition.maxIndependentInstances > Types.Limits.MAX_INDEPENDENT_STATUS_INSTANCES
		or definition.maxIndependentInstances % 1 ~= 0
		or type(definition.periodicEffects) ~= "table"
		or getmetatable(definition.periodicEffects) ~= nil
		or type(definition.modifiers) ~= "table"
		or getmetatable(definition.modifiers) ~= nil
		or (definition.cueId ~= nil and type(definition.cueId) ~= "string")
	then
		return false
	end
	if definition.tickInterval ~= nil
		and (not isFinite(definition.tickInterval) or definition.tickInterval <= 0)
	then
		return false
	end
	local periodicCount = 0
	local periodicMaximum = 0
	for key, effect in next, definition.periodicEffects do
		local periodicEffect = effect :: any
		if type(key) ~= "number"
			or key < 1
			or key % 1 ~= 0
			or type(periodicEffect) ~= "table"
			or getmetatable(periodicEffect) ~= nil
			or (periodicEffect.kind ~= Enums.EffectKind.Damage and periodicEffect.kind ~= Enums.EffectKind.Heal)
			or not isFinite(periodicEffect.amount)
			or periodicEffect.amount < 0
			or periodicEffect.amount > Types.Limits.MAX_EFFECT_AMOUNT
			or (periodicEffect.kind == Enums.EffectKind.Damage
				and (type(periodicEffect.damageType) ~= "string" or not validateStringArray(periodicEffect.sourceTags)))
		then
			return false
		end
		periodicCount += 1
		periodicMaximum = math.max(periodicMaximum, key)
	end
	if periodicCount ~= periodicMaximum or (periodicCount > 0 and definition.tickInterval == nil) then
		return false
	end
	local modifierCount = 0
	local modifierMaximum = 0
	for key, modifier in next, definition.modifiers do
		local typedModifier = modifier :: any
		if type(key) ~= "number"
			or key < 1
			or key % 1 ~= 0
			or type(typedModifier) ~= "table"
			or getmetatable(typedModifier) ~= nil
			or type(typedModifier.id) ~= "string"
			or type(typedModifier.stat) ~= "string"
			or (typedModifier.operation ~= "Add" and typedModifier.operation ~= "Multiply")
			or not isFinite(typedModifier.value)
		then
			return false
		end
		modifierCount += 1
		modifierMaximum = math.max(modifierMaximum, key)
	end
	if modifierCount ~= modifierMaximum then
		return false
	end
	return true
end

local function validateApplyInput(self: Solver, input: any): (boolean, string?, number)
	local valid, reason = validateCommonInput(self, input)
	if not valid then
		return false, reason, 0
	end
	if type(input.definitionId) ~= "string"
		or input.definitionId == ""
		or not isFinite(input.definitionVersion)
		or input.definitionVersion < 1
		or input.definitionVersion % 1 ~= 0
	then
		return false, "InvalidDefinitionReference", 0
	end
	if not isFinite(input.stacks)
		or input.stacks < 1
		or input.stacks > Types.Limits.MAX_STATUS_STACKS
		or input.stacks % 1 ~= 0
	then
		return false, "InvalidStacks", 0
	end
	if not isFinite(input.potency) or input.potency < 0 or input.potency > Types.Limits.MAX_EFFECT_AMOUNT then
		return false, "InvalidPotency", 0
	end
	if input.durationOverride ~= nil
		and (not isFinite(input.durationOverride)
			or input.durationOverride <= 0
			or input.durationOverride > Types.Limits.MAX_STATUS_DURATION)
	then
		return false, "InvalidDurationOverride", 0
	end
	local definition = self._definitionsByKey[definitionKey(input.definitionId, input.definitionVersion)]
	if definition == nil then
		return false, "MissingDefinition", 0
	end
	if not validateStatusDefinition(definition) then
		return false, "InvalidDefinition", 0
	end
	if definition.tickInterval ~= nil and #definition.periodicEffects > 0 then
		local duration = input.durationOverride or definition.duration
		local fullOrdinals = math.floor(duration / definition.tickInterval)
		local exactExpiry = math.abs(fullOrdinals * definition.tickInterval - duration)
			<= definition.tickInterval * 1e-9
		local tickCount = fullOrdinals + (if definition.tickOnApply then 1 else 0)
		if exactExpiry and not definition.tickAtExpiry then
			tickCount = math.max(0, tickCount - 1)
		end
		if tickCount > Types.Limits.MAX_STATUS_TICKS_PER_INSTANCE then
			return false, "StatusTickCap", 0
		end
	end
	return true, nil, lifecycleSlotCount(definition)
end

local function validateCleanseInput(self: Solver, input: any): (boolean, string?, number)
	local valid, reason = validateCommonInput(self, input)
	if not valid then
		return false, reason, 0
	end
	if not validateStringArray(input.includeAnyTags)
		or not validateStringArray(input.includeAllTags)
		or not validateStringArray(input.excludeAnyTags)
	then
		return false, "InvalidTagFilter", 0
	end
	if not isFinite(input.maxRemovals)
		or input.maxRemovals < 1
		or input.maxRemovals > Types.Limits.MAX_STATUS_BUDGET
		or input.maxRemovals % 1 ~= 0
	then
		return false, "InvalidMaxRemovals", 0
	end
	return true, nil, 0
end

local function validateRemoveInput(self: Solver, input: any): (boolean, string?, number)
	local valid, reason = validateCommonInput(self, input)
	if not valid then
		return false, reason, 0
	end
	if type(input.instanceId) ~= "string" or input.instanceId == "" then
		return false, "InvalidInstanceId", 0
	end
	return true, nil, 0
end

local function validateQueuedRequest(self: Solver, request: any): (boolean, string?, number)
	if type(request) ~= "table" or getmetatable(request) ~= nil then
		return false, "InvalidQueueRequest", 0
	end
	if request.kind == "Apply" then
		return validateApplyInput(self, request.input)
	elseif request.kind == "Cleanse" then
		return validateCleanseInput(self, request.input)
	elseif request.kind == "Remove" then
		return validateRemoveInput(self, request.input)
	end
	return false, "InvalidQueueKind", 0
end

-- Side-effect-free proof used by effect batches before any direct gameplay mutation.
local function preflightQueueBatch(self: Solver, requests: { any }): (boolean, string?)
	if type(requests) ~= "table" or getmetatable(requests) ~= nil then
		return false, "InvalidQueueBatch"
	end
	local requestCount = 0
	local maximumIndex = 0
	local lifecycleSlots = 0
	local seenOperationIds: { [string]: boolean } = {}
	for key, request in next, requests do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return false, "InvalidQueueBatch"
		end
		requestCount += 1
		maximumIndex = math.max(maximumIndex, key)
		local valid, reason, reservation = validateQueuedRequest(self, request)
		if not valid then
			return false, reason
		end
		local operationId = request.input.operationId
		if seenOperationIds[operationId]
			or self._acceptedOperations[operationId]
			or self._processedOperations[operationId]
		then
			return false, "DuplicateOperationId"
		end
		for _, queuedWork in ipairs(self._queue) do
			if queuedWork.operationId == operationId then
				return false, "DuplicateOperationId"
			end
		end
		seenOperationIds[operationId] = true
		lifecycleSlots += reservation
	end
	if requestCount ~= maximumIndex then
		return false, "InvalidQueueBatch"
	end
	if #self._acceptedOrder + requestCount > self._maxLedgerEntries then
		return false, "LedgerFull"
	end
	if liveQueueCount(self) + self._reservedLifecycleSlots + requestCount + lifecycleSlots > self._maxQueuedOperations then
		return false, "QueueFull"
	end
	return true, nil
end

function StatusEffectSolver.canQueueApply(self: Solver, input: any): (boolean, string?)
	return preflightQueueBatch(self, { { kind = "Apply", input = input } })
end

function StatusEffectSolver.canQueueCleanse(self: Solver, input: any): (boolean, string?)
	return preflightQueueBatch(self, { { kind = "Cleanse", input = input } })
end

function StatusEffectSolver.canQueueRemove(self: Solver, input: any): (boolean, string?)
	return preflightQueueBatch(self, { { kind = "Remove", input = input } })
end

function StatusEffectSolver.preflightBatch(self: Solver, requests: { any }): (boolean, string?)
	return preflightQueueBatch(self, requests)
end

local function enqueueExternal(
	self: Solver,
	kind: WorkKind,
	phase: number,
	input: any,
	reservedLifecycleSlots: number
): (boolean, string?)
	pruneInvalidLifecycleWork(self)
	local accepted, reason = acceptExternalOperation(self, input.operationId)
	if not accepted then
		return false, reason
	end
	if not addWork(self, kind, phase, input.scheduledTime, input.operationId, TableUtil.deepCopy(input), reservedLifecycleSlots) then
		forgetAcceptedOperation(self, input.operationId)
		return false, "QueueFull"
	end
	self._reservedLifecycleSlots += reservedLifecycleSlots
	return true, nil
end

function StatusEffectSolver.queueApply(self: Solver, input: any): (boolean, string?)
	local valid, reason = self:canQueueApply(input)
	if not valid then
		return false, reason
	end
	local definition = self._definitionsByKey[definitionKey(input.definitionId, input.definitionVersion)]
	return enqueueExternal(self, "Apply", PHASE_APPLY, input, lifecycleSlotCount(definition))
end

function StatusEffectSolver.queueCleanse(self: Solver, input: any): (boolean, string?)
	local valid, reason = self:canQueueCleanse(input)
	if not valid then
		return false, reason
	end
	return enqueueExternal(self, "Cleanse", PHASE_REMOVE, input, 0)
end

function StatusEffectSolver.queueRemove(self: Solver, input: any): (boolean, string?)
	local valid, reason = self:canQueueRemove(input)
	if not valid then
		return false, reason
	end
	return enqueueExternal(self, "Remove", PHASE_REMOVE, input, 0)
end

function StatusEffectSolver.advance(
	self: Solver,
	toTime: number,
	processedAt: number?,
	maxEffectCommands: number?
): AdvanceResult
	assert(isFinite(toTime) and toTime >= 0, "toTime must be finite and non-negative")
	local effectCommandBudget = maxEffectCommands or self._maxEffectCommandsPerAdvance
	assert(
		isFinite(effectCommandBudget) and effectCommandBudget >= 0 and effectCommandBudget % 1 == 0,
		"maxEffectCommands must be a non-negative integer"
	)
	local resolvedTime = math.max(self._currentTime, toTime)
	local suppliedProcessedAt = processedAt or resolvedTime
	assert(isFinite(suppliedProcessedAt), "processedAt must be finite")
	local resolvedProcessedAt = math.max(resolvedTime, suppliedProcessedAt)
	self._currentTime = resolvedTime
	local events = {}
	local commands = {}
	local ticksProcessed = 0
	local processedEffects = 0
	while true do
		local work = self._queue[1]
		if work == nil or not timeReached(resolvedTime, work.scheduledTime) then
			break
		end
		local workProcessedAt = math.max(resolvedProcessedAt, work.scheduledTime)
		if work.kind == "Tick" then
			local tickPlan = planTick(self, work)
			if tickPlan == nil then
				-- Invalidated tick work is discarded without spending either cap.
				table.remove(self._queue, 1)
				continue
			end
			if ticksProcessed >= self._maxStatusTicksPerAdvance then
				break
			end
			if processedEffects + tickPlan.effectCount > effectCommandBudget then
				-- A periodic tick is atomic: leave all of it queued for a later advance.
				break
			end
			table.remove(self._queue, 1)
			processTick(self, work, workProcessedAt, events, tickPlan)
			ticksProcessed += 1
			processedEffects += tickPlan.effectCount
			continue
		end

		table.remove(self._queue, 1)
		if work.reservedLifecycleSlots > 0 then
			self._reservedLifecycleSlots -= work.reservedLifecycleSlots
			assert(self._reservedLifecycleSlots >= 0, "status lifecycle reservation underflow")
		end
		if work.kind == "Apply" then
			processApply(self, work, workProcessedAt, events, commands)
			markExternalOperationProcessed(self, work.operationId)
		elseif work.kind == "Cleanse" then
			processCleanse(self, work, workProcessedAt, events, commands)
			markExternalOperationProcessed(self, work.operationId)
		elseif work.kind == "Remove" then
			processRemove(self, work, workProcessedAt, events, commands)
			markExternalOperationProcessed(self, work.operationId)
		else
			processExpire(self, work, workProcessedAt, events, commands)
		end
	end
	return {
		currentTime = self._currentTime,
		events = events,
		commands = commands,
		ticksProcessed = ticksProcessed,
		processedEffects = processedEffects,
		pendingOperations = #self._queue,
	}
end

function StatusEffectSolver.getStatuses(self: Solver, actorId: string): { any }
	local result = {}
	for _, instanceId in ipairs(self._instancesByActor[actorId] or {}) do
		local instance = self._instancesById[instanceId]
		if instance ~= nil then
			table.insert(result, publicInstance(instance))
		end
	end
	table.sort(result, function(a: any, b: any): boolean
		if a.sequence == b.sequence then
			return a.instanceId < b.instanceId
		end
		return a.sequence < b.sequence
	end)
	return TableUtil.deepCopy(result)
end

function StatusEffectSolver.getStatus(self: Solver, instanceId: string): any?
	local instance = self._instancesById[instanceId]
	return if instance == nil then nil else TableUtil.deepCopy(publicInstance(instance))
end

StatusEffectSolver.New = StatusEffectSolver.new
StatusEffectSolver.CanQueueApply = StatusEffectSolver.canQueueApply
StatusEffectSolver.CanQueueCleanse = StatusEffectSolver.canQueueCleanse
StatusEffectSolver.CanQueueRemove = StatusEffectSolver.canQueueRemove
StatusEffectSolver.PreflightBatch = StatusEffectSolver.preflightBatch
StatusEffectSolver.QueueApply = StatusEffectSolver.queueApply
StatusEffectSolver.Apply = StatusEffectSolver.queueApply
StatusEffectSolver.QueueCleanse = StatusEffectSolver.queueCleanse
StatusEffectSolver.Cleanse = StatusEffectSolver.queueCleanse
StatusEffectSolver.QueueRemove = StatusEffectSolver.queueRemove
StatusEffectSolver.Remove = StatusEffectSolver.queueRemove
StatusEffectSolver.Advance = StatusEffectSolver.advance
StatusEffectSolver.GetStatuses = StatusEffectSolver.getStatuses
StatusEffectSolver.GetStatus = StatusEffectSolver.getStatus

StatusEffectSolver.PHASE_REMOVE = PHASE_REMOVE
StatusEffectSolver.PHASE_APPLY = PHASE_APPLY
StatusEffectSolver.PHASE_TICK = PHASE_TICK
StatusEffectSolver.PHASE_EXPIRE = PHASE_EXPIRE
StatusEffectSolver.TIMING_POLICY = table.freeze({
	tickOnApply = "When enabled, ordinal zero ticks at appliedAt after all same-time apply/merge work.",
	tickAtExpiry = "Only an ordinal tick exactly aligned with expiresAt may tick at expiry; no off-grid expiry tick is invented.",
	refreshAtExpiry = "Same-time apply/merge runs before tick and expiry, invalidates the old expiry, and keeps tick ordinals anchored to appliedAt.",
	lateAdvance = "Due work keeps its exact scheduledTime and reports the advance time separately as processedAt.",
	phaseOrder = "Remove/Cleanse, Apply/Merge, Tick, Expire.",
})

return table.freeze(StatusEffectSolver)

--!strict

local Enums = require(script.Parent.Enums)
local ActorState = require(script.Parent.ActorState)
local AreaSolver = require(script.Parent.AreaSolver)
local StatusEffectSolver = require(script.Parent.StatusEffectSolver)
local TableUtil = require(script.Parent.TableUtil)
local Types = require(script.Parent.Types)
local Vector3 = require(script.Parent.Vector3)

export type Context = {
	operationId: string,
	castId: string,
	sourceActorId: string,
	targetActorId: string?,
	scheduledTime: number,
	processedAt: number?,
	impactPosition: { x: number, y: number, z: number }?,
	projectilePosition: { x: number, y: number, z: number }?,
	sourcePosition: { x: number, y: number, z: number }?,
	candidates: { any }?,
	areaCandidates: { any }?,
	falloff: number?,
}

export type Result = {
	ok: boolean,
	failureReason: string?,
	events: { any },
	commands: { any },
	processedEffects: number,
}

export type PreflightResult = {
	ok: boolean,
	failureReason: string?,
}

export type Config = {
	actors: { any }?,
	statusDefinitions: { any }?,
	budgets: any?,
	maxLedgerEntries: number?,
	maxQueuedStatusOperations: number?,
}

export type Solver = {
	_actorsById: { [string]: any },
	_actorOrder: { string },
	_statusSolver: any,
	_maxEffectCommands: number,
	_maxAreaTargets: number,
	_operationLedger: { [string]: boolean },
	_operationOrder: { string },
	_maxLedgerEntries: number,

	preflightBatch: (self: Solver, requests: { any }) -> PreflightResult,
	execute: (self: Solver, effect: any, context: Context) -> Result,
	executeBatch: (self: Solver, requests: { any }) -> Result,
	advance: (self: Solver, toTime: number, processedAt: number?) -> Result,
	getActor: (self: Solver, actorId: string) -> any?,
	getActors: (self: Solver) -> { any },
	getStatuses: (self: Solver, actorId: string) -> { any },
}

local EffectSolver = {}
EffectSolver.__index = EffectSolver

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function rememberOperation(self: Solver, operationId: string): boolean
	if self._operationLedger[operationId] then
		return false
	end
	self._operationLedger[operationId] = true
	table.insert(self._operationOrder, operationId)
	if #self._operationOrder > self._maxLedgerEntries then
		local removed = table.remove(self._operationOrder, 1)
		if removed ~= nil then
			self._operationLedger[removed] = nil
		end
	end
	return true
end

local function baseEvent(effect: any, context: Context): any
	return {
		effectKind = effect.kind,
		operationId = context.operationId,
		castId = context.castId,
		sourceActorId = context.sourceActorId,
		targetActorId = context.targetActorId,
		scheduledTime = context.scheduledTime,
		processedAt = context.processedAt or context.scheduledTime,
	}
end

local function skipped(effect: any, context: Context, reason: string): any
	local event = baseEvent(effect, context)
	event.kind = "EffectSkipped"
	event.reason = reason
	return event
end

local function resolveCenter(self: Solver, centerName: string, context: Context): any?
	if centerName == Enums.AreaCenter.Impact then
		return context.impactPosition
	elseif centerName == Enums.AreaCenter.Projectile then
		return context.projectilePosition
	elseif centerName == "Source" then
		if context.sourcePosition ~= nil then
			return context.sourcePosition
		end
		local source = self._actorsById[context.sourceActorId]
		return if source == nil then nil else source.position
	end
	return nil
end

local function validateContext(context: Context)
	assert(type(context.operationId) == "string" and context.operationId ~= "", "operationId must be a non-empty string")
	assert(type(context.castId) == "string" and context.castId ~= "", "castId must be a non-empty string")
	assert(type(context.sourceActorId) == "string" and context.sourceActorId ~= "", "sourceActorId must be a non-empty string")
	assert(isFinite(context.scheduledTime) and context.scheduledTime >= 0, "scheduledTime must be finite and non-negative")
	if context.processedAt ~= nil then
		assert(isFinite(context.processedAt) and context.processedAt >= context.scheduledTime, "processedAt must not precede scheduledTime")
	end
end

local function actorCandidate(self: Solver, candidate: any): any?
	local actorId: string?
	if type(candidate) == "string" then
		actorId = candidate
	elseif type(candidate) == "table" and getmetatable(candidate) == nil then
		local candidateId = rawget(candidate, "id") or rawget(candidate, "actorId")
		if type(candidateId) == "string" then
			actorId = candidateId
		end
	end
	if type(actorId) ~= "string" then
		return nil
	end
	local actor = self._actorsById[actorId]
	if actor == nil then
		return nil
	end
	local result = {
		id = actor.id,
		position = TableUtil.deepCopy(actor.position),
		tags = TableUtil.deepCopy(actor.tags),
		factionId = actor.factionId,
	}
	return result
end

local function normalizeCandidates(self: Solver, candidates: { any }): ({ any }?, string?)
	if type(candidates) ~= "table" or getmetatable(candidates) ~= nil then
		return nil, "InvalidAreaCandidates"
	end
	local count = 0
	local maximum = 0
	for key in next, candidates do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return nil, "InvalidAreaCandidates"
		end
		count += 1
		maximum = math.max(maximum, key)
	end
	if count ~= maximum then
		return nil, "InvalidAreaCandidates"
	end
	local result = {}
	for _, candidate in ipairs(candidates) do
		local normalized = actorCandidate(self, candidate)
		if normalized == nil then
			return nil, "UnknownAreaCandidate"
		end
		table.insert(result, normalized)
	end
	return result, nil
end

local function isFiniteVector(value: any): boolean
	return type(value) == "table"
		and getmetatable(value) == nil
		and isFinite(rawget(value, "x"))
		and isFinite(rawget(value, "y"))
		and isFinite(rawget(value, "z"))
end

local function isStringArray(value: any): boolean
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

local function isDenseArray(value: any): boolean
	if type(value) ~= "table" or getmetatable(value) ~= nil then
		return false
	end
	local count = 0
	local maximum = 0
	for key in next, value do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return false
		end
		count += 1
		maximum = math.max(maximum, key)
	end
	return count == maximum
end

local function copyContext(context: Context): Context
	return {
		operationId = context.operationId,
		castId = context.castId,
		sourceActorId = context.sourceActorId,
		targetActorId = context.targetActorId,
		scheduledTime = context.scheduledTime,
		processedAt = context.processedAt,
		impactPosition = context.impactPosition,
		projectilePosition = context.projectilePosition,
		sourcePosition = context.sourcePosition,
		candidates = context.candidates,
		areaCandidates = context.areaCandidates,
		falloff = context.falloff,
	}
end

local function statusApplyInput(effect: any, context: Context): any
	return {
		operationId = context.operationId,
		castId = context.castId,
		sourceActorId = context.sourceActorId,
		targetActorId = context.targetActorId,
		scheduledTime = context.scheduledTime,
		definitionId = effect.definitionId,
		definitionVersion = effect.definitionVersion,
		stacks = effect.stacks,
		potency = effect.potency * (context.falloff or 1),
		durationOverride = effect.durationOverride,
	}
end

local function statusCleanseInput(effect: any, context: Context): any
	return {
		operationId = context.operationId,
		castId = context.castId,
		sourceActorId = context.sourceActorId,
		targetActorId = context.targetActorId,
		scheduledTime = context.scheduledTime,
		includeAnyTags = TableUtil.deepCopy(effect.includeAnyTags),
		includeAllTags = TableUtil.deepCopy(effect.includeAllTags),
		excludeAnyTags = TableUtil.deepCopy(effect.excludeAnyTags),
		maxRemovals = effect.maxRemovals,
	}
end

local function areaConfig(effect: any, maximumTargets: number): any
	return {
		radius = effect.radius,
		falloff = effect.falloff,
		relationToSource = effect.relationToSource,
		includeAnyTags = TableUtil.deepCopy(effect.includeAnyTags),
		includeAllTags = TableUtil.deepCopy(effect.includeAllTags),
		excludeAnyTags = TableUtil.deepCopy(effect.excludeAnyTags),
		maxTargets = math.min(effect.maxTargets, maximumTargets),
	}
end

type PreflightState = {
	processedEffects: number,
	operationCount: number,
	seenOperationIds: { [string]: boolean },
	statusRequests: { any },
}

local function contextFailure(self: Solver, context: any): string?
	if type(context) ~= "table" or getmetatable(context) ~= nil then
		return "InvalidContext"
	end
	if type(context.operationId) ~= "string" or context.operationId == "" then
		return "InvalidOperationId"
	end
	if type(context.castId) ~= "string" or context.castId == "" then
		return "InvalidCastId"
	end
	if type(context.sourceActorId) ~= "string" or context.sourceActorId == "" then
		return "InvalidSourceActorId"
	end
	if self._actorsById[context.sourceActorId] == nil then
		return "MissingSource"
	end
	if context.targetActorId ~= nil
		and (type(context.targetActorId) ~= "string" or context.targetActorId == "")
	then
		return "InvalidTargetActorId"
	end
	if not isFinite(context.scheduledTime) or context.scheduledTime < 0 then
		return "InvalidScheduledTime"
	end
	if context.processedAt ~= nil
		and (not isFinite(context.processedAt) or context.processedAt < context.scheduledTime)
	then
		return "InvalidProcessedAt"
	end
	for _, field in ipairs({ "impactPosition", "projectilePosition", "sourcePosition" }) do
		local position = context[field]
		if position ~= nil and not isFiniteVector(position) then
			return "InvalidPosition"
		end
	end
	if context.falloff ~= nil and (not isFinite(context.falloff) or context.falloff < 0 or context.falloff > 1) then
		return "InvalidFalloff"
	end
	return nil
end

local function targetFailure(self: Solver, context: Context): string?
	if context.targetActorId == nil then
		return "MissingTarget"
	end
	if self._actorsById[context.targetActorId] == nil then
		return "MissingTarget"
	end
	return nil
end

local function registerOperation(self: Solver, context: Context, state: PreflightState): string?
	if state.seenOperationIds[context.operationId] or self._operationLedger[context.operationId] then
		return "DuplicateOperationId"
	end
	state.seenOperationIds[context.operationId] = true
	state.operationCount += 1
	if state.operationCount > self._maxLedgerEntries then
		return "LedgerFull"
	end
	return nil
end

local preflightEffect: (Solver, any, Context, PreflightState, boolean) -> string?

preflightEffect = function(
	self: Solver,
	effect: any,
	context: Context,
	state: PreflightState,
	allowArea: boolean
): string?
	local invalidContext = contextFailure(self, context)
	if invalidContext ~= nil then
		return invalidContext
	end
	if type(effect) ~= "table" or getmetatable(effect) ~= nil or type(effect.kind) ~= "string" then
		return "InvalidEffect"
	end
	local duplicate = registerOperation(self, context, state)
	if duplicate ~= nil then
		return duplicate
	end

	if effect.kind == Enums.EffectKind.Area then
		if not allowArea then
			return "NestedArea"
		end
		if (effect.center ~= Enums.AreaCenter.Impact and effect.center ~= Enums.AreaCenter.Projectile and effect.center ~= Enums.AreaCenter.Source)
			or not isFinite(effect.radius)
			or effect.radius <= 0
			or effect.radius > Types.Limits.MAX_AREA_RADIUS
			or (effect.falloff ~= Enums.AreaFalloff.Constant and effect.falloff ~= Enums.AreaFalloff.Linear)
			or (effect.relationToSource ~= Enums.Relation.Any
				and effect.relationToSource ~= Enums.Relation.Self
				and effect.relationToSource ~= Enums.Relation.Ally
				and effect.relationToSource ~= Enums.Relation.Enemy)
			or not isStringArray(effect.includeAnyTags)
			or not isStringArray(effect.includeAllTags)
			or not isStringArray(effect.excludeAnyTags)
			or not isFinite(effect.maxTargets)
			or effect.maxTargets < 1
			or effect.maxTargets % 1 ~= 0
			or not isDenseArray(effect.effects)
			or #effect.effects == 0
		then
			return "InvalidAreaEffect"
		end
		local center = resolveCenter(self, effect.center, context)
		if center == nil or not isFiniteVector(center) then
			return "MissingCenter"
		end
		local suppliedCandidates = context.areaCandidates or context.candidates
		if suppliedCandidates == nil then
			return "MissingAreaCandidates"
		end
		local candidates, candidateError = normalizeCandidates(self, suppliedCandidates)
		if candidates == nil then
			return candidateError or "InvalidAreaCandidates"
		end
		local resolvedAreaConfig = areaConfig(effect, self._maxAreaTargets)
		local sourceActor = self._actorsById[context.sourceActorId]
		local areaResult = AreaSolver.solve(
			resolvedAreaConfig,
			center,
			candidates,
			context.sourceActorId,
			if sourceActor == nil then nil else sourceActor.factionId
		)
		if not areaResult.ok then
			return areaResult.error or "AreaRejected"
		end
		local selectedTargets: { any } = areaResult.targets
		for _, target in ipairs(selectedTargets) do
			for effectIndex, childEffect in ipairs(effect.effects) do
				local childContext = copyContext(context)
				childContext.operationId = context.operationId
					.. ":target:"
					.. target.actorId
					.. ":effect:"
					.. tostring(effectIndex)
				childContext.targetActorId = target.actorId
				childContext.falloff = (context.falloff or 1) * target.falloff
				local childFailure = preflightEffect(self, childEffect, childContext, state, false)
				if childFailure ~= nil then
					return childFailure
				end
			end
		end
		return nil
	end

	local missingTarget = targetFailure(self, context)
	if missingTarget ~= nil then
		return missingTarget
	end
	if effect.kind == Enums.EffectKind.Damage then
		if not isFinite(effect.amount)
			or effect.amount < 0
			or effect.amount > Types.Limits.MAX_EFFECT_AMOUNT
			or type(effect.damageType) ~= "string"
			or not isStringArray(effect.sourceTags)
		then
			return "InvalidDamageEffect"
		end
	elseif effect.kind == Enums.EffectKind.Heal then
		if not isFinite(effect.amount) or effect.amount < 0 or effect.amount > Types.Limits.MAX_EFFECT_AMOUNT then
			return "InvalidHealEffect"
		end
	elseif effect.kind == Enums.EffectKind.Pull then
		if (effect.center ~= Enums.AreaCenter.Impact and effect.center ~= Enums.AreaCenter.Projectile and effect.center ~= Enums.AreaCenter.Source)
			or not isFinite(effect.deltaSpeed)
			or effect.deltaSpeed < 0
			or effect.deltaSpeed > Types.Limits.MAX_PULL_SPEED
			or not isFinite(effect.maxDeltaSpeed)
			or effect.maxDeltaSpeed < 0
			or effect.maxDeltaSpeed > Types.Limits.MAX_PULL_SPEED
		then
			return "InvalidPullEffect"
		end
		local center = resolveCenter(self, effect.center, context)
		if center == nil or not isFiniteVector(center) then
			return "MissingCenter"
		end
	elseif effect.kind == Enums.EffectKind.ApplyStatus then
		if type(effect.definitionId) ~= "string"
			or effect.definitionId == ""
			or not isFinite(effect.definitionVersion)
			or effect.definitionVersion < 1
			or effect.definitionVersion % 1 ~= 0
			or not isFinite(effect.stacks)
			or effect.stacks < 1
			or effect.stacks % 1 ~= 0
			or not isFinite(effect.potency)
			or effect.potency < 0
			or effect.durationOverride ~= nil
				and (not isFinite(effect.durationOverride) or effect.durationOverride <= 0)
		then
			return "InvalidApplyStatusEffect"
		end
		table.insert(state.statusRequests, { kind = "Apply", input = statusApplyInput(effect, context) })
	elseif effect.kind == Enums.EffectKind.Cleanse then
		if not isStringArray(effect.includeAnyTags)
			or not isStringArray(effect.includeAllTags)
			or not isStringArray(effect.excludeAnyTags)
		then
			return "InvalidCleanseEffect"
		end
		table.insert(state.statusRequests, { kind = "Cleanse", input = statusCleanseInput(effect, context) })
	else
		return "UnsupportedEffectKind"
	end
	state.processedEffects += 1
	if state.processedEffects > self._maxEffectCommands then
		return "MaxEffectCommands"
	end
	return nil
end

type Execution = {
	events: { any },
	commands: { any },
	processedEffects: number,
}

local executeEffect: any

local function consumeCommand(self: Solver, effect: any, context: Context, execution: Execution): boolean
	if execution.processedEffects >= self._maxEffectCommands then
		local event = baseEvent(effect, context)
		event.kind = "EffectBudgetExceeded"
		event.reason = "MaxEffectCommands"
		table.insert(execution.events, event)
		return false
	end
	execution.processedEffects += 1
	return true
end

local function executeDamage(self: Solver, effect: any, context: Context, execution: Execution)
	if not consumeCommand(self, effect, context, execution) then
		return
	end
	local targetId = context.targetActorId
	local actor = if targetId == nil then nil else self._actorsById[targetId]
	if actor == nil then
		table.insert(execution.events, skipped(effect, context, "MissingTarget"))
		return
	end
	local amount = effect.amount * (context.falloff or 1)
	local nextActor, delta = ActorState.damage(actor, amount)
	self._actorsById[targetId :: string] = nextActor
	local event = baseEvent(effect, context)
	event.kind = "DamageApplied"
	event.damageType = effect.damageType
	event.sourceTags = TableUtil.deepCopy(effect.sourceTags)
	event.requestedAmount = delta.requestedAmount
	event.appliedAmount = delta.appliedAmount
	event.beforeHealth = delta.beforeHealth
	event.afterHealth = delta.afterHealth
	event.maxHealth = delta.maxHealth
	table.insert(execution.events, event)
end

local function executeHeal(self: Solver, effect: any, context: Context, execution: Execution)
	if not consumeCommand(self, effect, context, execution) then
		return
	end
	local targetId = context.targetActorId
	local actor = if targetId == nil then nil else self._actorsById[targetId]
	if actor == nil then
		table.insert(execution.events, skipped(effect, context, "MissingTarget"))
		return
	end
	local amount = effect.amount * (context.falloff or 1)
	local nextActor, delta = ActorState.heal(actor, amount)
	self._actorsById[targetId :: string] = nextActor
	local event = baseEvent(effect, context)
	event.kind = "HealApplied"
	event.requestedAmount = delta.requestedAmount
	event.appliedAmount = delta.appliedAmount
	event.beforeHealth = delta.beforeHealth
	event.afterHealth = delta.afterHealth
	event.maxHealth = delta.maxHealth
	table.insert(execution.events, event)
end

local function executePull(self: Solver, effect: any, context: Context, execution: Execution)
	if not consumeCommand(self, effect, context, execution) then
		return
	end
	local targetId = context.targetActorId
	local actor = if targetId == nil then nil else self._actorsById[targetId]
	local center = resolveCenter(self, effect.center, context)
	if actor == nil then
		table.insert(execution.events, skipped(effect, context, "MissingTarget"))
		return
	elseif center == nil or not Vector3.isFinite(center) then
		table.insert(execution.events, skipped(effect, context, "MissingCenter"))
		return
	end
	local inward = Vector3.sub(center, actor.position)
	local distance = Vector3.magnitude(inward)
	if distance == 0 then
		local event = baseEvent(effect, context)
		event.kind = "PullSkipped"
		event.reason = "ZeroDistance"
		event.center = TableUtil.deepCopy(center)
		table.insert(execution.events, event)
		return
	end
	local deltaSpeed = math.min(effect.deltaSpeed * (context.falloff or 1), effect.maxDeltaSpeed)
	local deltaVelocity = Vector3.scale(inward, deltaSpeed / distance)
	local command = baseEvent(effect, context)
	command.kind = "PullCommand"
	command.center = TableUtil.deepCopy(center)
	command.distance = distance
	command.deltaSpeed = deltaSpeed
	command.deltaVelocity = deltaVelocity
	table.insert(execution.commands, command)
	local event = TableUtil.deepCopy(command)
	event.kind = "PullCommanded"
	table.insert(execution.events, event)
end

local function executeApplyStatus(self: Solver, effect: any, context: Context, execution: Execution)
	if not consumeCommand(self, effect, context, execution) then
		return
	end
	if context.targetActorId == nil then
		table.insert(execution.events, skipped(effect, context, "MissingTarget"))
		return
	end
	local ok, reason = self._statusSolver:queueApply(statusApplyInput(effect, context))
	if not ok then
		local event = baseEvent(effect, context)
		event.kind = "StatusCommandRejected"
		event.reason = reason
		table.insert(execution.events, event)
	else
		local event = baseEvent(effect, context)
		event.kind = "StatusCommandQueued"
		table.insert(execution.events, event)
	end
end

local function executeCleanse(self: Solver, effect: any, context: Context, execution: Execution)
	if not consumeCommand(self, effect, context, execution) then
		return
	end
	if context.targetActorId == nil then
		table.insert(execution.events, skipped(effect, context, "MissingTarget"))
		return
	end
	local ok, reason = self._statusSolver:queueCleanse(statusCleanseInput(effect, context))
	if not ok then
		local event = baseEvent(effect, context)
		event.kind = "StatusCommandRejected"
		event.reason = reason
		table.insert(execution.events, event)
	else
		local event = baseEvent(effect, context)
		event.kind = "StatusCommandQueued"
		table.insert(execution.events, event)
	end
end

local function executeArea(self: Solver, effect: any, context: Context, execution: Execution)
	local center = resolveCenter(self, effect.center, context)
	if center == nil or not Vector3.isFinite(center) then
		table.insert(execution.events, skipped(effect, context, "MissingCenter"))
		return
	end
	local suppliedCandidates = context.areaCandidates or context.candidates
	if suppliedCandidates == nil then
		table.insert(execution.events, skipped(effect, context, "MissingAreaCandidates"))
		return
	end
	local candidates, candidateError = normalizeCandidates(self, suppliedCandidates)
	if candidates == nil then
		table.insert(execution.events, skipped(effect, context, candidateError or "InvalidAreaCandidates"))
		return
	end
	local resolvedAreaConfig = areaConfig(effect, self._maxAreaTargets)
	local sourceActor = self._actorsById[context.sourceActorId]
	local sourceFactionId = if sourceActor == nil then nil else sourceActor.factionId
	local areaResult = AreaSolver.solve(resolvedAreaConfig, center, candidates, context.sourceActorId, sourceFactionId)
	if not areaResult.ok then
		local event = skipped(effect, context, areaResult.error or "AreaRejected")
		event.actorId = areaResult.actorId
		table.insert(execution.events, event)
		return
	end
	local selectedEvent = baseEvent(effect, context)
	selectedEvent.kind = "AreaSelected"
	selectedEvent.center = TableUtil.deepCopy(center)
	selectedEvent.targets = TableUtil.deepCopy(areaResult.targets)
	table.insert(execution.events, selectedEvent)
	local selectedTargets: { any } = areaResult.targets
	for _, target in ipairs(selectedTargets) do
		for effectIndex, childEffect in ipairs(effect.effects) do
			if childEffect.kind == Enums.EffectKind.Area then
				local nestedContext = copyContext(context)
				nestedContext.operationId = context.operationId .. ":target:" .. target.actorId .. ":effect:" .. tostring(effectIndex)
				table.insert(execution.events, skipped(childEffect, nestedContext, "NestedArea"))
			else
				local childContext = copyContext(context)
				childContext.operationId = context.operationId .. ":target:" .. target.actorId .. ":effect:" .. tostring(effectIndex)
				childContext.targetActorId = target.actorId
				childContext.falloff = (context.falloff or 1) * target.falloff
				executeEffect(self, childEffect, childContext, execution)
			end
		end
	end
end

executeEffect = function(self: Solver, effect: any, context: Context, execution: Execution)
	validateContext(context)
	if not rememberOperation(self, context.operationId) then
		local event = baseEvent(effect, context)
		event.kind = "EffectDuplicateIgnored"
		table.insert(execution.events, event)
		return
	end
	if effect.kind == Enums.EffectKind.Damage then
		executeDamage(self, effect, context, execution)
	elseif effect.kind == Enums.EffectKind.Heal then
		executeHeal(self, effect, context, execution)
	elseif effect.kind == Enums.EffectKind.Area then
		executeArea(self, effect, context, execution)
	elseif effect.kind == Enums.EffectKind.Pull then
		executePull(self, effect, context, execution)
	elseif effect.kind == Enums.EffectKind.ApplyStatus then
		executeApplyStatus(self, effect, context, execution)
	elseif effect.kind == Enums.EffectKind.Cleanse then
		executeCleanse(self, effect, context, execution)
	else
		table.insert(execution.events, skipped(effect, context, "UnsupportedEffectKind"))
	end
end

function EffectSolver.new(configOrActors: any, statusDefinitions: { any }?, extraConfig: any?): Solver
	local config: any
	if type(configOrActors) == "table" and configOrActors.actors ~= nil then
		config = configOrActors
	else
		config = extraConfig or {}
		config.actors = configOrActors or {}
		config.statusDefinitions = statusDefinitions or {}
	end
	local actorsById: { [string]: any } = {}
	local actorOrder = {}
	local sourceActors = config.actors or {}
	if #sourceActors > 0 then
		for _, sourceActor in ipairs(sourceActors) do
			local actor = ActorState.new(sourceActor)
			assert(actorsById[actor.id] == nil, "duplicate actor id")
			actorsById[actor.id] = actor
			table.insert(actorOrder, actor.id)
		end
	else
		for actorId, sourceActor in pairs(sourceActors) do
			local actor = ActorState.new(sourceActor)
			assert(actor.id == actorId, "actor map key must match actor id")
			assert(actorsById[actor.id] == nil, "duplicate actor id")
			actorsById[actor.id] = actor
			table.insert(actorOrder, actor.id)
		end
		table.sort(actorOrder)
	end
	local budgets = config.budgets or {}
	local maxEffects = budgets.maxEffectCommands or Types.DefaultBudgets.maxEffectCommands
	local maxAreaTargets = budgets.maxAreaTargets or Types.DefaultBudgets.maxAreaTargets
	local maxLedger = config.maxLedgerEntries or Types.Limits.MAX_STATUS_LEDGER_ENTRIES
	assert(maxEffects > 0 and maxEffects % 1 == 0, "maxEffectCommands must be a positive integer")
	assert(maxAreaTargets > 0 and maxAreaTargets % 1 == 0, "maxAreaTargets must be a positive integer")
	local statusSolver = StatusEffectSolver.new(config.statusDefinitions or {}, actorsById, {
		maxStatusesPerActor = budgets.maxStatusesPerActor or Types.DefaultBudgets.maxStatusesPerActor,
		maxStatusTicksPerAdvance = budgets.maxStatusTicksPerAdvance or Types.DefaultBudgets.maxStatusTicksPerAdvance,
		maxEffectCommandsPerAdvance = maxEffects,
		maxLedgerEntries = maxLedger,
		maxQueuedOperations = config.maxQueuedStatusOperations,
	})
	local self = setmetatable({
		_actorsById = actorsById,
		_actorOrder = actorOrder,
		_statusSolver = statusSolver,
		_maxEffectCommands = maxEffects,
		_maxAreaTargets = maxAreaTargets,
		_operationLedger = {},
		_operationOrder = {},
		_maxLedgerEntries = maxLedger,
	}, EffectSolver)
	return (self :: unknown) :: Solver
end

type OrderedRequest = {
	index: number,
	effect: any,
	context: Context,
}

local function orderedRequests(self: Solver, requests: any): ({ OrderedRequest }?, string?)
	if type(requests) ~= "table" or getmetatable(requests) ~= nil then
		return nil, "InvalidRequestBatch"
	end
	local result: { OrderedRequest } = {}
	local count = 0
	local maximumIndex = 0
	for key, request in next, requests do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return nil, "InvalidRequestBatch"
		end
		count += 1
		maximumIndex = math.max(maximumIndex, key)
		if type(request) ~= "table" or getmetatable(request) ~= nil then
			return nil, "InvalidRequest"
		end
		local typedRequest = request :: any
		local failure = contextFailure(self, typedRequest.context)
		if failure ~= nil then
			return nil, failure
		end
		local index = key
		result[index] = {
			index = index,
			effect = typedRequest.effect,
			context = typedRequest.context,
		}
	end
	if count ~= maximumIndex then
		return nil, "InvalidRequestBatch"
	end
	table.sort(result, function(a: OrderedRequest, b: OrderedRequest): boolean
		if a.context.scheduledTime == b.context.scheduledTime then
			return a.index < b.index
		end
		return a.context.scheduledTime < b.context.scheduledTime
	end)
	return result, nil
end

-- Proves the entire expanded batch without touching health, queues, or ledgers.
function EffectSolver.preflightBatch(self: Solver, requests: { any }): PreflightResult
	local ordered, orderingFailure = orderedRequests(self, requests)
	if ordered == nil then
		return { ok = false, failureReason = orderingFailure }
	end
	local state: PreflightState = {
		processedEffects = 0,
		operationCount = 0,
		seenOperationIds = {},
		statusRequests = {},
	}
	for _, request in ipairs(ordered) do
		local failure = preflightEffect(self, request.effect, request.context, state, true)
		if failure ~= nil then
			return { ok = false, failureReason = failure }
		end
	end
	local statusesOk, statusFailure = self._statusSolver:preflightBatch(state.statusRequests)
	if not statusesOk then
		return { ok = false, failureReason = statusFailure }
	end
	return { ok = true }
end

function EffectSolver.executeBatch(self: Solver, requests: { any }): Result
	local preflight = self:preflightBatch(requests)
	if not preflight.ok then
		local failureEvents = {}
		if preflight.failureReason == "DuplicateOperationId" then
			local ordered = orderedRequests(self, requests)
			if ordered ~= nil and #ordered == 1 then
				local event = baseEvent(ordered[1].effect, ordered[1].context)
				event.kind = "EffectDuplicateIgnored"
				table.insert(failureEvents, event)
			end
		end
		return {
			ok = false,
			failureReason = preflight.failureReason,
			events = failureEvents,
			commands = {},
			processedEffects = 0,
		}
	end
	local execution: Execution = {
		events = {},
		commands = {},
		processedEffects = 0,
	}
	local ordered = orderedRequests(self, requests)
	assert(ordered ~= nil, "effect batch changed after successful preflight")
	for _, request in ipairs(ordered) do
		executeEffect(self, request.effect, request.context, execution)
	end
	return {
		ok = true,
		events = execution.events,
		commands = execution.commands,
		processedEffects = execution.processedEffects,
	}
end

function EffectSolver.execute(self: Solver, effect: any, context: Context): Result
	return self:executeBatch({ { effect = effect, context = context } })
end

function EffectSolver.advance(self: Solver, toTime: number, processedAt: number?): Result
	local result = self._statusSolver:advance(toTime, processedAt, self._maxEffectCommands)
	return {
		ok = true,
		events = result.events,
		commands = result.commands,
		processedEffects = result.processedEffects,
	}
end

function EffectSolver.getActor(self: Solver, actorId: string): any?
	local actor = self._actorsById[actorId]
	return if actor == nil then nil else TableUtil.deepCopy(actor)
end

function EffectSolver.getActors(self: Solver): { any }
	local result: { any } = {}
	for _, actorId in ipairs(self._actorOrder) do
		local actor = self._actorsById[actorId]
		if actor ~= nil then
			result[#result + 1] = TableUtil.deepCopy(actor)
		end
	end
	return result
end

function EffectSolver.getStatuses(self: Solver, actorId: string): { any }
	return self._statusSolver:getStatuses(actorId)
end

EffectSolver.New = EffectSolver.new
EffectSolver.PreflightBatch = EffectSolver.preflightBatch
EffectSolver.Execute = EffectSolver.execute
EffectSolver.Apply = EffectSolver.execute
EffectSolver.ExecuteBatch = EffectSolver.executeBatch
EffectSolver.Advance = EffectSolver.advance
EffectSolver.GetActor = EffectSolver.getActor
EffectSolver.GetActors = EffectSolver.getActors
EffectSolver.GetStatuses = EffectSolver.getStatuses

-- Public execute calls may apply direct health changes immediately, but status
-- mutations are globally phase-ordered only when advance is called explicitly.
EffectSolver.STATUS_TIMING_POLICY = table.freeze({
	execute = "Queues status operations without flushing them.",
	advance = "Flushes due status work by scheduled time and global status phase order.",
	processedEffects = "Counts direct effects for execute calls and periodic damage/heal commands for advance calls.",
})

return table.freeze(EffectSolver)

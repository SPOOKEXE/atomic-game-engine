--!strict

local EffectSolver = require(script.Parent.EffectSolver)
local ProjectileSolver = require(script.Parent.ProjectileSolver)
local RandomStreams = require(script.Parent.RandomStreams)
local TableUtil = require(script.Parent.TableUtil)
local Types = require(script.Parent.Types)

type StatusCueContext = {
	operationPrefix: string,
	castId: string,
	entityId: number,
	trigger: "Impact" | "Expire",
	scheduledTime: number,
	position: any,
	allowedCueIds: { [string]: boolean },
}

export type Solver = {
	_compiledSpell: any,
	_recipes: { [string]: any },
	_projectileSolver: any,
	_effectSolver: any,
	_castByEntityId: { [number]: any },
	_triggerLedger: { [string]: boolean },
	_triggerOrder: { string },
	_maxTriggerLedger: number,
	_statusCueContexts: { StatusCueContext },
	_statusCueLedger: { [string]: boolean },
	_statusCueOrder: { string },
	_castIds: { [string]: boolean },

	castTrusted: (self: Solver, input: any) -> any,
	triggerImpact: (self: Solver, input: any) -> any,
	triggerExpire: (self: Solver, input: any) -> any,
	advance: (self: Solver, deltaSeconds: number) -> any,
	getActor: (self: Solver, actorId: string) -> any?,
	getEntity: (self: Solver, entityId: number) -> any?,
}

local SpellSolver = {}
SpellSolver.__index = SpellSolver

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function isFiniteVector(value: any): boolean
	return type(value) == "table"
		and getmetatable(value) == nil
		and isFinite(rawget(value, "x"))
		and isFinite(rawget(value, "y"))
		and isFinite(rawget(value, "z"))
end

local function appendAll(target: { any }, source: { any })
	for _, value in ipairs(source) do
		table.insert(target, value)
	end
end

local function serverProgram(compiledSpell: any): any
	return compiledSpell.serverProgram or {}
end

local function clientProgram(compiledSpell: any): any
	return compiledSpell.clientProgram or {}
end

local function deepEqual(a: any, b: any): boolean
	if type(a) ~= type(b) then
		return false
	end
	if type(a) ~= "table" then
		return a == b
	end
	for key, value in pairs(a) do
		if not deepEqual(value, b[key]) then
			return false
		end
	end
	for key in pairs(b) do
		if a[key] == nil then
			return false
		end
	end
	return true
end

-- Build one status registry for every spell this solver may execute.
local function mergedStatusDefinitions(compiledSpell: any, recipes: { [string]: any }): { any }
	local definitions: { any } = {}
	local byKey: { [string]: any } = {}
	local programs: { any } = { serverProgram(compiledSpell) }
	local recipeIds = TableUtil.sortedStringKeys(recipes) :: { string }
	for _, recipeId in ipairs(recipeIds) do
		table.insert(programs, serverProgram(recipes[recipeId]))
	end
	for _, program: any in ipairs(programs) do
		for _, definition: any in ipairs(program.statusDefinitions or {}) do
			local key = definition.id .. "@" .. tostring(definition.version)
			local existing = byKey[key]
			assert(existing == nil or deepEqual(existing, definition), "conflicting status definition " .. key)
			if existing == nil then
				local copy = TableUtil.deepCopy(definition)
				byKey[key] = copy
				table.insert(definitions, copy)
			end
		end
	end
	return definitions
end

local function expandedChildren(burst: any?): { any }
	if burst == nil then
		return {}
	end
	if burst.expandedChildren ~= nil then
		return burst.expandedChildren
	end
	local result: { any } = {}
	local childIndex = 0
	local groups: { any } = burst.children or {}
	for _, group: any in ipairs(groups) do
		for _ = 1, group.count do
			childIndex += 1
			table.insert(result, {
				childIndex = childIndex,
				groupId = group.id,
				recipeId = group.recipeId,
			})
		end
	end
	return result
end

local function cross(a: any, b: any): any
	return {
		x = a.y * b.z - a.z * b.y,
		y = a.z * b.x - a.x * b.z,
		z = a.x * b.y - a.y * b.x,
	}
end

local function unit(value: any): any
	local length = math.sqrt(value.x * value.x + value.y * value.y + value.z * value.z)
	assert(length > 0, "direction must be non-zero")
	return { x = value.x / length, y = value.y / length, z = value.z / length }
end

-- Derive one stable seed and launch direction for this exact burst slot.
local function deriveChildLaunch(entity: any, burst: any, child: any, childDepth: number): (number, any)
	local purpose = burst.random.purpose
	local childSeed = RandomStreams.deriveSeed(entity.rootSeed, purpose, child.groupId, child.childIndex, childDepth)
	local random = RandomStreams.new(childSeed, "direction")
	local azimuth = random:NextNumber(0, math.pi * 2)

	-- A Ring scatters level around the burst point and consumes exactly one draw, so a
	-- child's seed stream stays comparable with the other distributions. Launching level
	-- is what makes the recipe's local +Y resolve to world up, which is how a ballistic
	-- child lands on a predictable radius instead of somewhere on a sphere.
	if burst.distribution == "Ring" then
		return childSeed, { x = math.cos(azimuth), y = 0, z = math.sin(azimuth) }
	end

	local z: number
	if burst.distribution == "Cone" then
		z = random:NextNumber(math.cos(math.rad(burst.coneAngleDegrees)), 1)
	elseif burst.distribution == "Arc" then
		-- A hollow cone: the polar angle is drawn from a band that excludes
		-- straight ahead, so every child *turns*. Drawing `z` uniformly between
		-- the two cosines rather than drawing the angle uniformly keeps the
		-- children spread evenly over the band's area instead of bunching at its
		-- inner edge, which is the same reason a Sphere draws `z` and not a
		-- latitude.
		z = random:NextNumber(math.cos(math.rad(burst.arcMaxDegrees)), math.cos(math.rad(burst.arcMinDegrees)))
	else
		z = random:NextNumber(-1, 1)
	end
	local radius = math.sqrt(math.max(0, 1 - z * z))
	local localX = radius * math.cos(azimuth)
	local localY = radius * math.sin(azimuth)

	-- Sphere is already world space; Cone and Arc are both relative to where the
	-- parent was going, so both need the parent's basis built below.
	if burst.distribution ~= "Cone" and burst.distribution ~= "Arc" then
		return childSeed, { x = localX, y = localY, z = z }
	end

	local forward = unit(entity.direction)
	local reference = if math.abs(forward.y) < 0.999 then { x = 0, y = 1, z = 0 } else { x = 1, y = 0, z = 0 }
	local right = unit(cross(reference, forward))
	local up = cross(forward, right)
	return childSeed, unit({
		x = right.x * localX + up.x * localY + forward.x * z,
		y = right.y * localX + up.y * localY + forward.y * z,
		z = right.z * localX + up.z * localY + forward.z * z,
	})
end

local function matchingEventRecords(compiledSpell: any, triggerName: "Impact" | "Expire"): { any }
	local expectedKind = if triggerName == "Impact" then "Event.Impact" else "Event.Expire"
	local result: { any } = {}
	local records: { any } = serverProgram(compiledSpell).events or {}
	for _, record: any in ipairs(records) do
		if record.kind == expectedKind then
			table.insert(result, record)
		end
	end
	table.sort(result, function(a: any, b: any): boolean
		if a.order == b.order then
			return a.id < b.id
		end
		return a.order < b.order
	end)
	return result
end

local function matchingEffectTriggers(compiledSpell: any, triggerName: "Impact" | "Expire"): { any }
	local result: { any } = {}
	local triggers: { any } = serverProgram(compiledSpell).effectTriggers or {}
	for _, trigger: any in ipairs(triggers) do
		if trigger.trigger == triggerName then
			table.insert(result, trigger)
		end
	end
	table.sort(result, function(a: any, b: any): boolean
		if a.order == b.order then
			return a.id < b.id
		end
		return a.order < b.order
	end)
	return result
end

local function matchingCueDescriptors(compiledSpell: any, triggerName: "Impact" | "Expire"): { any }
	local result: { any } = {}
	local descriptors: { any } = clientProgram(compiledSpell).effectCues or {}
	for _, descriptor: any in ipairs(descriptors) do
		if descriptor.trigger == triggerName then
			table.insert(result, descriptor)
		end
	end
	table.sort(result, function(a: any, b: any): boolean
		if a.order == b.order then
			return a.id < b.id
		end
		return a.order < b.order
	end)
	return result
end

local function emptyDispatch(scheduledTime: number): any
	return {
		scheduledTime = scheduledTime,
		events = {},
		commands = {},
		records = {},
		processedEffects = 0,
		duplicate = false,
	}
end

local function rememberTrigger(self: Solver, operationId: string): boolean
	if self._triggerLedger[operationId] then
		return false
	end
	self._triggerLedger[operationId] = true
	table.insert(self._triggerOrder, operationId)
	if #self._triggerOrder > self._maxTriggerLedger then
		local removed = table.remove(self._triggerOrder, 1)
		if removed ~= nil then
			self._triggerLedger[removed] = nil
		end
	end
	return true
end

local function isRememberedTrigger(self: Solver, operationId: string): boolean
	return self._triggerLedger[operationId] == true
end

local function operationMatchesPrefix(operationId: any, prefix: string): boolean
	return type(operationId) == "string"
		and (operationId == prefix or string.sub(operationId, 1, #prefix + 1) == prefix .. ":")
end

local function rememberStatusCueContext(self: Solver, context: StatusCueContext)
	table.insert(self._statusCueContexts, context)
	if #self._statusCueContexts > self._maxTriggerLedger then
		table.remove(self._statusCueContexts, 1)
	end
end

local function rememberStatusCue(self: Solver, cueOperationId: string): boolean
	if self._statusCueLedger[cueOperationId] then
		return false
	end
	self._statusCueLedger[cueOperationId] = true
	table.insert(self._statusCueOrder, cueOperationId)
	if #self._statusCueOrder > self._maxTriggerLedger then
		local removed = table.remove(self._statusCueOrder, 1)
		if removed ~= nil then
			self._statusCueLedger[removed] = nil
		end
	end
	return true
end

local function appendTriggerCues(
	records: { any },
	descriptors: { any },
	operationRoot: string,
	cast: any,
	input: any,
	triggerName: "Impact" | "Expire",
	position: any
)
	for _, descriptor in ipairs(descriptors) do
		for cueIndex, cueId in ipairs(descriptor.triggerCueIds or {}) do
			table.insert(records, {
				kind = "PresentationCue",
				id = operationRoot .. ":cue:" .. descriptor.id .. ":" .. tostring(cueIndex),
				trigger = triggerName,
				cueId = cueId,
				castId = cast.castId,
				targetActorId = input.targetActorId or cast.targetActorId,
				entityId = input.entityId,
				scheduledTime = input.scheduledTime,
				position = TableUtil.deepCopy(position),
			})
		end
	end
end

local function appendStatusCues(self: Solver, events: { any }, records: { any })
	for _, event in ipairs(events) do
		if type(event.cueId) == "string" then
			for contextIndex = #self._statusCueContexts, 1, -1 do
				local context = self._statusCueContexts[contextIndex]
				if event.castId == context.castId
					and operationMatchesPrefix(event.operationId, context.operationPrefix)
				then
					local cueOperationId = event.operationId .. ":status-cue:" .. event.cueId
					if context.allowedCueIds[event.cueId] and rememberStatusCue(self, cueOperationId) then
						table.insert(records, {
							kind = "PresentationCue",
							id = event.operationId .. ":status-cue",
							trigger = context.trigger,
							cueId = event.cueId,
							castId = context.castId,
							targetActorId = event.targetActorId,
							entityId = context.entityId,
							scheduledTime = event.scheduledTime,
							position = TableUtil.deepCopy(context.position),
						})
					end
					break
				end
			end
		end
	end
end

local function cueDescriptorFor(descriptors: { any }, triggerId: string): any?
	for _, descriptor in ipairs(descriptors) do
		if descriptor.id == triggerId then
			return descriptor
		end
	end
	return nil
end

local function effectQueuesStatusCue(effect: any): boolean
	if effect.kind == "Effect.ApplyStatus" then
		return true
	end
	if effect.kind == "Effect.Area" then
		for _, childEffect in ipairs(effect.effects) do
			if effectQueuesStatusCue(childEffect) then
				return true
			end
		end
	end
	return false
end

local function triggerNeedsTermination(eventRecords: { any }): boolean
	for _, record in ipairs(eventRecords) do
		if record.action == "Terminate" then
			return true
		end
	end
	return false
end

local function terminationReady(
	self: Solver,
	entity: any,
	triggerName: "Impact" | "Expire",
	scheduledTime: number
): boolean
	if entity.logicalState == "Active" then
		return self._projectileSolver:CanCancelAt(entity.id, scheduledTime)
	end
	return triggerName == "Expire"
		and entity.logicalState == "Expired"
		and entity.logicalEndTime == scheduledTime
end

local function rejectUnavailableTermination(result: any, operationRoot: string, cast: any, input: any, triggerName: string)
	table.insert(result.events, {
		kind = "SpellTriggerRejected",
		reason = "TerminationUnavailable",
		operationId = operationRoot,
		castId = cast.castId,
		targetActorId = input.targetActorId or cast.targetActorId,
		scheduledTime = input.scheduledTime,
		processedAt = input.processedAt or input.scheduledTime,
		trigger = triggerName,
	})
end

local function rejectEffectPreflight(
	result: any,
	operationRoot: string,
	cast: any,
	input: any,
	triggerName: string,
	failureReason: string?
)
	table.insert(result.events, {
		kind = "SpellTriggerRejected",
		reason = "EffectPreflightFailed",
		failureReason = failureReason or "UnknownEffectPreflightFailure",
		operationId = operationRoot,
		castId = cast.castId,
		sourceActorId = cast.sourceActorId,
		targetActorId = input.targetActorId or cast.targetActorId,
		scheduledTime = input.scheduledTime,
		processedAt = input.processedAt or input.scheduledTime,
		trigger = triggerName,
	})
end

local function dispatch(self: Solver, triggerName: "Impact" | "Expire", input: any): any
	assert(type(input) == "table", "trigger input must be a table")
	assert(getmetatable(input) == nil, "trigger input cannot have a metatable")
	assert(type(input.entityId) == "number", "entityId must be a number")
	assert(isFinite(input.scheduledTime) and input.scheduledTime >= 0, "scheduledTime must be finite and non-negative")
	local cast = self._castByEntityId[input.entityId]
	assert(cast ~= nil, "unknown projectile entity")
	local entity = self._projectileSolver:GetEntity(input.entityId) or input._projectileSnapshot
	assert(entity ~= nil, "projectile entity is unavailable")
	local operationRoot = input.operationId
		or (cast.castId .. ":entity:" .. tostring(input.entityId) .. ":" .. string.lower(triggerName))
	local result = emptyDispatch(input.scheduledTime)
	if type(operationRoot) ~= "string" or operationRoot == "" then
		rejectEffectPreflight(result, "<invalid-operation>", cast, input, triggerName, "InvalidOperationId")
		return result
	end
	if isRememberedTrigger(self, operationRoot) then
		result.duplicate = true
		table.insert(result.events, {
			kind = "SpellTriggerDuplicateIgnored",
			operationId = operationRoot,
			castId = cast.castId,
			sourceActorId = cast.sourceActorId,
			targetActorId = input.targetActorId or cast.targetActorId,
			scheduledTime = input.scheduledTime,
			processedAt = input.processedAt or input.scheduledTime,
			trigger = triggerName,
		})
		return result
	end

	local compiledSpell = cast.compiledSpell
	local eventRecords = matchingEventRecords(compiledSpell, triggerName)
	local needsTermination = triggerNeedsTermination(eventRecords)
	local position = input.position or entity.position
	if not isFiniteVector(position) then
		rejectEffectPreflight(result, operationRoot, cast, input, triggerName, "InvalidPosition")
		return result
	end
	local targetActorId = input.targetActorId or cast.targetActorId
	local areaCandidates = input.areaCandidates or input.candidates or cast.areaCandidates
	local cueDescriptors = matchingCueDescriptors(compiledSpell, triggerName)
	local requests = {}
	local pendingStatusCueContexts: { StatusCueContext } = {}
	for _, trigger in ipairs(matchingEffectTriggers(compiledSpell, triggerName)) do
		local operationPrefix = operationRoot .. ":effect:" .. trigger.id
		table.insert(requests, {
			effect = trigger.effect,
			context = {
				operationId = operationPrefix,
				castId = cast.castId,
				sourceActorId = cast.sourceActorId,
				targetActorId = targetActorId,
				scheduledTime = input.scheduledTime,
				processedAt = input.processedAt or input.scheduledTime,
				impactPosition = if triggerName == "Impact" then position else input.impactPosition,
				projectilePosition = position,
				sourcePosition = cast.sourcePosition,
				areaCandidates = areaCandidates,
			},
		})
		local descriptor = cueDescriptorFor(cueDescriptors, trigger.id)
		if descriptor ~= nil and effectQueuesStatusCue(trigger.effect) then
			local allowedCueIds: { [string]: boolean } = {}
			local descriptorCueIds = (descriptor.allowedStatusCueIds or {}) :: { string }
			for _, cueId in ipairs(descriptorCueIds) do
				allowedCueIds[cueId] = true
			end
			if next(allowedCueIds) ~= nil then
				table.insert(pendingStatusCueContexts, {
					operationPrefix = operationPrefix,
					castId = cast.castId,
					entityId = input.entityId,
					trigger = triggerName,
					scheduledTime = input.scheduledTime,
					position = position,
					allowedCueIds = allowedCueIds,
				})
			end
		end
	end

	if needsTermination then
		if not terminationReady(self, entity, triggerName, input.scheduledTime) then
			rejectUnavailableTermination(result, operationRoot, cast, input, triggerName)
			return result
		end
	end
	local effectPreflight = self._effectSolver:preflightBatch(requests)
	if not effectPreflight.ok then
		rejectEffectPreflight(result, operationRoot, cast, input, triggerName, effectPreflight.failureReason)
		return result
	end

	if needsTermination then
		if entity.logicalState == "Active" then
			if not self._projectileSolver:CancelAt(input.entityId, input.scheduledTime, triggerName) then
				rejectUnavailableTermination(result, operationRoot, cast, input, triggerName)
				return result
			end
			entity = self._projectileSolver:GetEntity(input.entityId)
			assert(entity ~= nil, "terminated projectile entity is unavailable")
		end
	end
	local effectResult = self._effectSolver:executeBatch(requests)
	if not effectResult.ok then
		rejectEffectPreflight(result, operationRoot, cast, input, triggerName, effectResult.failureReason)
		return result
	end
	appendAll(result.events, effectResult.events)
	appendAll(result.commands, effectResult.commands)
	result.processedEffects = effectResult.processedEffects
	assert(rememberTrigger(self, operationRoot), "trigger dedupe state changed during dispatch")
	appendTriggerCues(result.records, cueDescriptors, operationRoot, cast, input, triggerName, position)
	for _, statusCueContext in ipairs(pendingStatusCueContexts) do
		statusCueContext.position = TableUtil.deepCopy(statusCueContext.position)
		rememberStatusCueContext(self, statusCueContext)
	end

	for _, record in ipairs(eventRecords) do
		if record.action == "Terminate" then
			table.insert(result.records, {
				kind = "Termination",
				id = operationRoot .. ":event:" .. record.id,
				castId = cast.castId,
				sourceActorId = cast.sourceActorId,
				targetActorId = targetActorId,
				entityId = input.entityId,
				scheduledTime = input.scheduledTime,
				reason = triggerName,
			})
		elseif record.action == "Burst" then
			local burst = serverProgram(compiledSpell).burst
			local allowsTrigger = burst ~= nil
				and (burst.trigger == triggerName or burst.trigger == "ImpactOrExpire")
			if allowsTrigger then
				for _, child in ipairs(expandedChildren(burst)) do
					local childDepth = cast.rootDepth + 1
					local ownerDepth = cast.ownerDepth
					local childOwnerDepth = childDepth
					local localDepth = childDepth - ownerDepth
					local rejectionReason: string? = nil
					local childCompiled = self._recipes[child.recipeId]
					if childCompiled == nil then
						rejectionReason = "UnknownRecipe"
					elseif localDepth > burst.maxDepth or childDepth > cast.rootState.maxBurstDepth then
						rejectionReason = "MaxDepth"
					elseif cast.rootState.projectileCount >= cast.rootState.maxProjectiles then
						rejectionReason = "MaxProjectiles"
					end
					local childRecord = {
						kind = if rejectionReason == nil then "ChildSpawned" else "ChildSpawnRejected",
						id = operationRoot .. ":event:" .. record.id .. ":child:" .. tostring(child.childIndex),
						castId = cast.castId,
						sourceActorId = cast.sourceActorId,
						targetActorId = targetActorId,
						parentEntityId = input.entityId,
						depth = childDepth,
						childIndex = child.childIndex,
						groupId = child.groupId,
						recipeId = child.recipeId,
						scheduledTime = input.scheduledTime,
						position = TableUtil.deepCopy(position),
						reason = rejectionReason,
					}
					if rejectionReason == nil then
						local childSeed, childDirection = deriveChildLaunch(entity, burst, child, childDepth)
						local spawnOk, childEntityId = pcall(self._projectileSolver.Spawn, self._projectileSolver, childCompiled, {
							origin = position,
							direction = childDirection,
							rootSeed = childSeed,
						})
						if not spawnOk then
							childRecord.kind = "ChildSpawnRejected"
							childRecord.reason = if string.find(tostring(childEntityId), "queue has no room", 1, true)
								then "QueueFull"
								else "SpawnFailed"
						else
							cast.rootState.projectileCount += 1
							self._castByEntityId[childEntityId] = {
								castId = cast.castId,
								sourceActorId = cast.sourceActorId,
								sourcePosition = TableUtil.deepCopy(cast.sourcePosition),
								targetActorId = cast.targetActorId,
								areaCandidates = TableUtil.deepCopy(cast.areaCandidates),
								rootSeed = childSeed,
								compiledSpell = childCompiled,
								rootDepth = childDepth,
								ownerDepth = childOwnerDepth,
								rootState = cast.rootState,
							}
							childRecord.entityId = childEntityId
							childRecord.seed = childSeed
							childRecord.direction = TableUtil.deepCopy(childDirection)
						end
					end
					table.insert(result.records, childRecord)
				end
			end
		end
	end
	return result
end

function SpellSolver.new(compiledOrConfig: any, actors: { any }?, extraConfig: any?): Solver
	local config: any
	if type(compiledOrConfig) == "table" and (compiledOrConfig.graph ~= nil or compiledOrConfig.compiledSpell ~= nil) then
		config = compiledOrConfig
	else
		config = extraConfig or {}
		config.compiledSpell = compiledOrConfig
		config.actors = actors or {}
	end
	local graph = config.graph
	if graph ~= nil then
		assert(type(graph) == "table", "graph must be a CompiledSpellGraph")
		assert(graph.kind == "CompiledSpellGraph", "graph must be compiler-produced")
		assert(graph.schemaVersion == Types.SCHEMA_VERSION, "graph schemaVersion is unsupported")
		assert(graph.compilerVersion == Types.COMPILER_VERSION, "graph compilerVersion is unsupported")
		assert(table.isfrozen(graph), "graph must be frozen")
		assert(type(graph.root) == "table" and table.isfrozen(graph.root), "graph root must be frozen")
		assert(type(graph.recipes) == "table" and table.isfrozen(graph.recipes), "graph recipes must be frozen")
		assert(graph.rootId == graph.root.id, "graph rootId must match root.id")
	end
	local compiledSpell = if graph ~= nil then graph.root else config.compiledSpell
	assert(compiledSpell ~= nil, "SpellSolver requires one compiled spell at construction")
	local program = serverProgram(compiledSpell)
	local recipes = if graph ~= nil then graph.recipes else (config.recipes or {})
	local projectileSolver = ProjectileSolver.new(config.projectile)
	local effectSolver = EffectSolver.new({
		actors = config.actors or {},
		statusDefinitions = mergedStatusDefinitions(compiledSpell, recipes),
		budgets = program.budgets or {},
		maxLedgerEntries = config.maxLedgerEntries,
		maxQueuedStatusOperations = config.maxQueuedStatusOperations,
	})
	local self = setmetatable({
		_compiledSpell = compiledSpell,
		_recipes = recipes,
		_projectileSolver = projectileSolver,
		_effectSolver = effectSolver,
		_castByEntityId = {},
		_triggerLedger = {},
		_triggerOrder = {},
		_maxTriggerLedger = config.maxLedgerEntries or 8192,
		_statusCueContexts = {},
		_statusCueLedger = {},
		_statusCueOrder = {},
		_castIds = {},
	}, SpellSolver)
	return (self :: unknown) :: Solver
end

-- Trusted server-adapter context only. Do not pass a RemoteEvent payload here.
-- Origin, target snapshots, candidate IDs, and rootSeed must already be derived
-- or checked by authoritative server code. The construction-time compiled spell
-- is always used; a per-cast compiled spell is rejected at this trust boundary.
function SpellSolver.castTrusted(self: Solver, input: any): any
	assert(type(input) == "table", "trusted cast context must be a table")
	assert(getmetatable(input) == nil, "trusted cast context cannot have a metatable")
	assert(type(input.castId) == "string" and input.castId ~= "", "castId must be a non-empty string")
	assert(type(input.sourceActorId) == "string" and input.sourceActorId ~= "", "sourceActorId must be a non-empty string")
	assert(input.compiledSpell == nil, "compiledSpell is not accepted in cast input")
	assert(isFiniteVector(input.origin), "origin must be a finite vector")
	assert(isFiniteVector(input.direction), "direction must be a finite vector")
	local directionLengthSquared = input.direction.x ^ 2 + input.direction.y ^ 2 + input.direction.z ^ 2
	assert(directionLengthSquared > 0 and directionLengthSquared < math.huge, "direction must be non-zero")
	assert(isFinite(input.rootSeed) and input.rootSeed % 1 == 0 and math.abs(input.rootSeed) <= 9007199254740991, "rootSeed must be a finite safe integer")
	assert(self._effectSolver:getActor(input.sourceActorId) ~= nil, "sourceActorId must identify a known actor")
	assert(not self._castIds[input.castId], "castId has already been used")
	local entityId = self._projectileSolver:Spawn(self._compiledSpell, {
		origin = input.origin,
		direction = input.direction,
		rootSeed = input.rootSeed,
	})
	self._castIds[input.castId] = true
	local rootBudgets = serverProgram(self._compiledSpell).budgets or {}
	local rootState = {
		projectileCount = 1,
		maxProjectiles = rootBudgets.maxProjectiles or 1,
		maxBurstDepth = rootBudgets.maxBurstDepth or 0,
	}
	self._castByEntityId[entityId] = {
		castId = input.castId,
		sourceActorId = input.sourceActorId,
		sourcePosition = TableUtil.deepCopy(input.origin),
		targetActorId = input.targetActorId,
		areaCandidates = TableUtil.deepCopy(input.areaCandidates or input.candidates),
		rootSeed = input.rootSeed,
		compiledSpell = self._compiledSpell,
		rootDepth = 0,
		ownerDepth = 0,
		rootState = rootState,
	}
	return {
		kind = "CastAccepted",
		castId = input.castId,
		sourceActorId = input.sourceActorId,
		entityId = entityId,
		scheduledTime = input.scheduledTime or 0,
	}
end

function SpellSolver.triggerImpact(self: Solver, input: any): any
	return dispatch(self, "Impact", input)
end

function SpellSolver.triggerExpire(self: Solver, input: any): any
	return dispatch(self, "Expire", input)
end

function SpellSolver.advance(self: Solver, deltaSeconds: number): any
	local projectileResult = self._projectileSolver:Advance(deltaSeconds)
	local result = {
		simulationTime = projectileResult.simulationTime,
		projectileEvents = projectileResult.events,
		events = {},
		commands = {},
		records = {},
		droppedBacklogSeconds = projectileResult.droppedBacklogSeconds,
		totalDroppedBacklogSeconds = projectileResult.totalDroppedBacklogSeconds,
		eventBackpressure = projectileResult.eventBackpressure,
		processedEffects = 0,
	}
	for _, projectileEvent in ipairs(projectileResult.events) do
		if projectileEvent.kind ~= "BurstScheduled" then
			table.insert(result.records, {
				kind = "ProjectileRecord",
				event = TableUtil.deepCopy(projectileEvent),
			})
		end
		if projectileEvent.kind == "ProjectileExpired" and self._castByEntityId[projectileEvent.entityId] ~= nil then
			local cast = self._castByEntityId[projectileEvent.entityId]
			local expireResult = dispatch(self, "Expire", {
				entityId = projectileEvent.entityId,
				scheduledTime = projectileEvent.scheduledTime,
				processedAt = projectileEvent.processedAt,
				position = projectileEvent.position,
				targetActorId = cast.targetActorId,
				areaCandidates = cast.areaCandidates,
				operationId = cast.castId
					.. ":entity:"
					.. tostring(projectileEvent.entityId)
					.. ":expire",
				_projectileSnapshot = {
					id = projectileEvent.entityId,
					logicalState = "Expired",
					logicalEndTime = projectileEvent.scheduledTime,
					position = TableUtil.deepCopy(projectileEvent.position),
					rootSeed = cast.rootSeed,
				},
			})
			appendAll(result.events, expireResult.events)
			appendAll(result.commands, expireResult.commands)
			appendAll(result.records, expireResult.records)
			result.processedEffects += expireResult.processedEffects
		end
		if projectileEvent.kind == "VisualReleased" then
			self._castByEntityId[projectileEvent.entityId] = nil
		end
	end
	local statusResult = self._effectSolver:advance(projectileResult.simulationTime, projectileResult.simulationTime)
	appendAll(result.events, statusResult.events)
	appendAll(result.commands, statusResult.commands)
	appendStatusCues(self, statusResult.events, result.records)
	result.processedEffects += statusResult.processedEffects
	return result
end

function SpellSolver.getActor(self: Solver, actorId: string): any?
	return self._effectSolver:getActor(actorId)
end

function SpellSolver.getEntity(self: Solver, entityId: number): any?
	return self._projectileSolver:GetEntity(entityId)
end

SpellSolver.New = SpellSolver.new
SpellSolver.CastTrusted = SpellSolver.castTrusted
SpellSolver.TriggerImpact = SpellSolver.triggerImpact
SpellSolver.TriggerExpire = SpellSolver.triggerExpire
SpellSolver.Advance = SpellSolver.advance
SpellSolver.GetActor = SpellSolver.getActor
SpellSolver.GetEntity = SpellSolver.getEntity

return table.freeze(SpellSolver)

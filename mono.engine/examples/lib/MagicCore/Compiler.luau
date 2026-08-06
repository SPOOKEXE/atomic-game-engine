--!strict

local Enums = require(script.Parent.Enums)
local TableUtil = require(script.Parent.TableUtil)
local Types = require(script.Parent.Types)
local Validator = require(script.Parent.Validator)

local Compiler = {}

local function normalizeNumber(value: number): number
	if value == 0 then
		return 0
	end
	return value
end

local function normalizeValue(value: any): any
	if type(value) == "number" then
		return normalizeNumber(value)
	elseif type(value) ~= "table" then
		return value
	end

	local result = {}
	for key, child in pairs(value) do
		result[key] = normalizeValue(child)
	end
	return result
end

local function normalizeBudgets(value: any?): { [string]: number }
	local result: { [string]: number } = {
		maxProjectiles = Types.DefaultBudgets.maxProjectiles,
		maxEvents = Types.DefaultBudgets.maxEvents,
		maxParticles = Types.DefaultBudgets.maxParticles,
		maxBurstDepth = Types.DefaultBudgets.maxBurstDepth,
		maxEffectCommands = Types.DefaultBudgets.maxEffectCommands,
		maxAreaTargets = Types.DefaultBudgets.maxAreaTargets,
		maxStatusesPerActor = Types.DefaultBudgets.maxStatusesPerActor,
		maxStatusTicksPerAdvance = Types.DefaultBudgets.maxStatusTicksPerAdvance,
	}
	if value ~= nil then
		for key, fieldValue in pairs(normalizeValue(value)) do
			result[key] = fieldValue
		end
	end
	return result
end

local function normalizePresentation(value: any): any
	local presentation = normalizeValue(value)
	presentation.shape = presentation.shape or "Ball"
	presentation.size = presentation.size or { x = 1, y = 1, z = 1 }
	presentation.color = presentation.color or { r = 1, g = 1, b = 1 }
	presentation.transparency = presentation.transparency or 0
	presentation.material = presentation.material or "Neon"
	presentation.particles = presentation.particles or {}
	presentation.curves = presentation.curves or {}
	presentation.timeline = presentation.timeline or {}
	for _, emitter in ipairs(presentation.particles) do
		emitter.rate = emitter.rate or 0
	end
	return presentation
end

local function sortEvents(events: { any }): { any }
	local result = normalizeValue(events)
	table.sort(result, function(a: any, b: any): boolean
		if a.order == b.order then
			return a.id < b.id
		end
		return a.order < b.order
	end)
	return result
end

local function sortEffectTriggers(effectTriggers: { any }): { any }
	local result = normalizeValue(effectTriggers)
	table.sort(result, function(a: any, b: any): boolean
		if a.order == b.order then
			return a.id < b.id
		end
		return a.order < b.order
	end)
	return result
end

local function sortStatusDefinitions(statusDefinitions: { any }): { any }
	local result = normalizeValue(statusDefinitions)
	table.sort(result, function(a: any, b: any): boolean
		if a.id == b.id then
			return a.version < b.version
		end
		return a.id < b.id
	end)
	return result
end

local function sortTimeline(timeline: { any }): { any }
	local result = normalizeValue(timeline)
	table.sort(result, function(a: any, b: any): boolean
		if a.time == b.time then
			return a.id < b.id
		end
		return a.time < b.time
	end)
	return result
end

local function expandBurstChildren(burst: any?): { any }
	local expanded = {}
	if burst == nil then
		return expanded
	end
	local groups: { any } = burst.children
	local childIndex = 0
	for _, group: any in ipairs(groups) do
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

local function saturatingAdd(a: number, b: number): number
	if a >= Types.Limits.COST_SATURATION - b then
		return Types.Limits.COST_SATURATION
	end
	return a + b
end

local function saturatingMultiply(a: number, b: number): number
	if a == 0 or b == 0 then
		return 0
	end
	if a >= Types.Limits.COST_SATURATION / b then
		return Types.Limits.COST_SATURATION
	end
	return a * b
end

type EffectCosts = {
	effectCommands: number,
	areaTargets: number,
	statusInstances: number,
	statusTicks: number,
}

local function definitionKey(id: string, version: number): string
	return id .. "@" .. tostring(version)
end

local function statusTickCount(definition: any, durationOverride: number?): number
	if definition.tickInterval == nil or #definition.periodicEffects == 0 then
		return 0
	end
	local interval = definition.tickInterval
	local duration = durationOverride or definition.duration
	local fullOrdinals = math.floor(duration / interval)
	local count = fullOrdinals
	local exactExpiry = math.abs(fullOrdinals * interval - duration) <= interval * 1e-9
	if exactExpiry and not definition.tickAtExpiry then
		count = math.max(0, count - 1)
	end
	if definition.tickOnApply then
		count = saturatingAdd(count, 1)
	end
	return math.min(count, Types.Limits.COST_SATURATION)
end

local estimateEffect: (any, { [string]: any }) -> EffectCosts

estimateEffect = function(effect: any, definitionsByKey: { [string]: any }): EffectCosts
	if effect.kind == Enums.EffectKind.Area then
		local children: EffectCosts = {
			effectCommands = 0,
			areaTargets = 0,
			statusInstances = 0,
			statusTicks = 0,
		}
		for _, child in ipairs(effect.effects) do
			local cost = estimateEffect(child, definitionsByKey)
			children.effectCommands = saturatingAdd(children.effectCommands, cost.effectCommands)
			children.areaTargets = saturatingAdd(children.areaTargets, cost.areaTargets)
			children.statusInstances = saturatingAdd(children.statusInstances, cost.statusInstances)
			children.statusTicks = saturatingAdd(children.statusTicks, cost.statusTicks)
		end
		return {
			effectCommands = saturatingMultiply(effect.maxTargets, children.effectCommands),
			areaTargets = saturatingAdd(effect.maxTargets, saturatingMultiply(effect.maxTargets, children.areaTargets)),
			statusInstances = saturatingMultiply(effect.maxTargets, children.statusInstances),
			statusTicks = saturatingMultiply(effect.maxTargets, children.statusTicks),
		}
	elseif effect.kind == Enums.EffectKind.ApplyStatus then
		local definition = definitionsByKey[definitionKey(effect.definitionId, effect.definitionVersion)]
		local ticks = if definition == nil then 0 else statusTickCount(definition, effect.durationOverride)
		local periodicCount = if definition == nil then 0 else #definition.periodicEffects
		return {
			effectCommands = saturatingAdd(1, saturatingMultiply(ticks, periodicCount)),
			areaTargets = 0,
			statusInstances = 1,
			statusTicks = ticks,
		}
	end
	return { effectCommands = 1, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
end

local function calculateEffectCosts(effectTriggers: { any }, statusDefinitions: { any }): EffectCosts
	local definitionsByKey: { [string]: any } = {}
	for _, definition in ipairs(statusDefinitions) do
		definitionsByKey[definitionKey(definition.id, definition.version)] = definition
	end
	local total: EffectCosts = { effectCommands = 0, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	for _, trigger in ipairs(effectTriggers) do
		local cost = estimateEffect(trigger.effect, definitionsByKey)
		total.effectCommands = saturatingAdd(total.effectCommands, cost.effectCommands)
		total.areaTargets = saturatingAdd(total.areaTargets, cost.areaTargets)
		total.statusInstances = saturatingAdd(total.statusInstances, cost.statusInstances)
		total.statusTicks = saturatingAdd(total.statusTicks, cost.statusTicks)
	end
	return total
end

local function appendCueId(output: { string }, seen: { [string]: boolean }, cueId: string)
	if seen[cueId] then
		return
	end
	seen[cueId] = true
	table.insert(output, cueId)
end

local function collectStatusCueIds(
	effect: any,
	definitionsByKey: { [string]: any },
	output: { string },
	seen: { [string]: boolean }
)
	if effect.kind == Enums.EffectKind.Area then
		for _, child in ipairs(effect.effects) do
			collectStatusCueIds(child, definitionsByKey, output, seen)
		end
	elseif effect.kind == Enums.EffectKind.ApplyStatus then
		local definition = definitionsByKey[definitionKey(effect.definitionId, effect.definitionVersion)]
		if definition ~= nil and definition.cueId ~= nil then
			appendCueId(output, seen, definition.cueId)
		end
	end
end

local function compilePresentationCues(effectTriggers: { any }, statusDefinitions: { any }): { any }
	local definitionsByKey: { [string]: any } = {}
	for _, definition in ipairs(statusDefinitions) do
		definitionsByKey[definitionKey(definition.id, definition.version)] = definition
	end
	local effectCues = {}
	for _, trigger in ipairs(effectTriggers) do
		local triggerCueIds = {}
		local seenTriggerCueIds: { [string]: boolean } = {}
		local authoredCueIds = (trigger.cueIds or {}) :: { string }
		for _, cueId in ipairs(authoredCueIds) do
			appendCueId(triggerCueIds, seenTriggerCueIds, cueId)
		end
		local allowedStatusCueIds = {}
		local seenStatusCueIds: { [string]: boolean } = {}
		collectStatusCueIds(trigger.effect, definitionsByKey, allowedStatusCueIds, seenStatusCueIds)
		table.insert(effectCues, {
			id = trigger.id,
			trigger = trigger.trigger,
			order = trigger.order,
			triggerCueIds = triggerCueIds,
			allowedStatusCueIds = allowedStatusCueIds,
		})
	end
	return effectCues
end

local function compileServerEffectTriggers(effectTriggers: { any }): { any }
	local result = {}
	for _, trigger in ipairs(effectTriggers) do
		table.insert(result, {
			id = trigger.id,
			trigger = trigger.trigger,
			order = trigger.order,
			effect = TableUtil.deepCopy(trigger.effect),
		})
	end
	return result
end

local function calculateVisualTail(projectile: any, timeline: { any }): number
	local ownerLifetime = projectile.lifetime.seconds
	local latestVisualEnd = ownerLifetime
	local particleLifetimeById: { [string]: number } = {}

	for _, emitter in ipairs(projectile.presentation.particles) do
		particleLifetimeById[emitter.id] = emitter.lifetime.max
		latestVisualEnd = math.max(latestVisualEnd, emitter.enabledSeconds + emitter.lifetime.max)
	end
	for _, cue in ipairs(timeline) do
		local particleLifetime = particleLifetimeById[cue.emitterId] or 0
		latestVisualEnd = math.max(latestVisualEnd, cue.time + particleLifetime)
	end
	return math.max(0, latestVisualEnd - ownerLifetime)
end

local function calculateCosts(
	projectile: any,
	timeline: { any },
	expandedChildren: { any },
	effectTriggers: { any },
	statusDefinitions: { any }
): { [string]: number }
	local particleEmissions = 0
	for _, emitter in ipairs(projectile.presentation.particles) do
		particleEmissions = saturatingAdd(particleEmissions, math.ceil(emitter.rate * emitter.enabledSeconds))
	end
	for _, cue in ipairs(timeline) do
		if cue.kind == "Timeline.EmitParticles" then
			particleEmissions = saturatingAdd(particleEmissions, cue.count)
		end
	end

	local childCount = #expandedChildren
	local effectCosts = calculateEffectCosts(effectTriggers, statusDefinitions)
	local scheduledEvents = saturatingAdd(3, #projectile.presentation.particles)
	scheduledEvents = saturatingAdd(scheduledEvents, #timeline)
	scheduledEvents = saturatingAdd(scheduledEvents, childCount)
	return {
		projectiles = 1,
		events = scheduledEvents,
		particleEmissions = particleEmissions,
		particleEmitters = #projectile.presentation.particles,
		burstChildren = childCount,
		effectCommands = effectCosts.effectCommands,
		areaTargets = effectCosts.areaTargets,
		statusInstances = effectCosts.statusInstances,
		statusTicks = effectCosts.statusTicks,
	}
end

local COST_FIELDS = {
	"projectiles", "events", "particleEmissions", "particleEmitters", "burstChildren",
	"effectCommands", "areaTargets", "statusInstances", "statusTicks",
}

local function addGraphDiagnostic(diagnostics: { any }, code: string, path: string, message: string)
	table.insert(diagnostics, { code = code, path = path, message = message, severity = "Error" })
end

local function addCosts(target: any, source: any, count: number)
	for _, field in ipairs(COST_FIELDS) do
		target[field] = saturatingAdd(target[field] or 0, saturatingMultiply(source[field] or 0, count))
	end
end

local function sameValue(a: any, b: any): boolean
	if type(a) ~= type(b) then return false end
	if type(a) ~= "table" then return a == b end
	for key, value in pairs(a) do
		if not sameValue(value, b[key]) then return false end
	end
	for key in pairs(b) do
		if a[key] == nil then return false end
	end
	return true
end

local BUDGET_COSTS = {
	maxProjectiles = { field = "projectiles", code = "BURST_CAP_EXCEEDED" },
	maxEvents = { field = "events", code = "EVENT_CAP_EXCEEDED" },
	maxParticles = { field = "particleEmissions", code = "PARTICLE_CAP_EXCEEDED" },
	maxEffectCommands = { field = "effectCommands", code = "EFFECT_COMMAND_CAP_EXCEEDED" },
	maxAreaTargets = { field = "areaTargets", code = "AREA_TARGET_CAP_EXCEEDED" },
	maxStatusesPerActor = { field = "statusInstances", code = "STATUS_CAP_EXCEEDED" },
}

local compileDocument: (any, boolean) -> (any?, { any })

-- Compiles only documents reachable from root and returns one frozen runtime graph.
function Compiler.compileGraph(rootDocument: any, recipeDocuments: any): (any?, { any })
	local diagnostics: { any } = {}
	local compiledRoot, rootDiagnostics = compileDocument(rootDocument, true)
	for _, diagnostic in ipairs(rootDiagnostics) do
		table.insert(diagnostics, diagnostic)
	end
	if compiledRoot == nil then return nil, diagnostics end
	local sourceRecipes = if type(recipeDocuments) == "table" then recipeDocuments else {}
	local compiledRecipes: { [string]: any } = {}
	local visiting: { [string]: boolean } = {}
	local compiledAttempted: { [string]: boolean } = {}
	local statusByKey: { [string]: any } = {}
	local statusPathByKey: { [string]: string } = {}
	local maxDepth = compiledRoot.budgets.maxBurstDepth

	local function registerStatuses(compiled: any, path: string)
		for index, definition: any in ipairs(compiled.serverProgram.statusDefinitions or {}) do
			local key = definitionKey(definition.id, definition.version)
			if statusByKey[key] ~= nil and not sameValue(statusByKey[key], definition) then
				addGraphDiagnostic(diagnostics, "CONFLICTING_STATUS_DEFINITION", path .. ".statusDefinitions[" .. tostring(index) .. "]", "Status " .. key .. " conflicts with " .. statusPathByKey[key] .. ".")
			elseif statusByKey[key] == nil then
				statusByKey[key] = definition
				statusPathByKey[key] = path .. ".statusDefinitions[" .. tostring(index) .. "]"
			end
		end
	end

	registerStatuses(compiledRoot, "$.root")
	local function visit(compiled: any, path: string, depth: number)
		local burst = compiled.burst
		if burst == nil then return end
		for groupIndex, group: any in ipairs(burst.children) do
			local childPath = path .. ".burst.children[" .. tostring(groupIndex) .. "].recipeId"
			local childDepth = depth + 1
			if childDepth > maxDepth then
				addGraphDiagnostic(diagnostics, "RECIPE_DEPTH_EXCEEDED", childPath, "Recipe " .. group.recipeId .. " exceeds root maxBurstDepth " .. tostring(maxDepth) .. ".")
			end
			if visiting[group.recipeId] then
				addGraphDiagnostic(diagnostics, "RECIPE_CYCLE", childPath, "Recipe cycle reaches " .. group.recipeId .. ".")
			else
				local document = sourceRecipes[group.recipeId]
				if document == nil then
					addGraphDiagnostic(diagnostics, "MISSING_RECIPE", childPath, "Missing reachable recipe " .. group.recipeId .. ".")
				elseif document.id ~= group.recipeId then
					addGraphDiagnostic(diagnostics, "RECIPE_ID_MISMATCH", "$.recipes[\"" .. group.recipeId .. "\"].id", "Recipe key " .. group.recipeId .. " does not match document id " .. tostring(document.id) .. ".")
				else
					if not compiledAttempted[group.recipeId] then
						compiledAttempted[group.recipeId] = true
						local childCompiled, childDiagnostics = compileDocument(document, true)
						for _, diagnostic in ipairs(childDiagnostics) do
							local copy = TableUtil.deepCopy(diagnostic)
							copy.path = "$.recipes[\"" .. group.recipeId .. "\"]" .. string.sub(copy.path, 2)
							table.insert(diagnostics, copy)
						end
						if childCompiled ~= nil then
							compiledRecipes[group.recipeId] = childCompiled
							registerStatuses(childCompiled, "$.recipes[\"" .. group.recipeId .. "\"]")
						end
					end
					local childCompiled = compiledRecipes[group.recipeId]
					if childCompiled ~= nil then
						visiting[group.recipeId] = true
						visit(childCompiled, "$.recipes[\"" .. group.recipeId .. "\"]", childDepth)
						visiting[group.recipeId] = nil
					end
				end
			end
		end
	end
	visiting[compiledRoot.id] = true
	visit(compiledRoot, "$.root", 0)
	visiting[compiledRoot.id] = nil
	if #diagnostics > 0 then return nil, diagnostics end

	local memo: { [string]: any } = {}
	local function subtree(compiled: any): any
		if memo[compiled.id] ~= nil then return memo[compiled.id] end
		local total = TableUtil.deepCopy(compiled.costs)
		if compiled.burst ~= nil then
			for _, group: any in ipairs(compiled.burst.children) do
				addCosts(total, subtree(compiledRecipes[group.recipeId]), group.count)
			end
		end
		memo[compiled.id] = total
		return total
	end
	local costs = subtree(compiledRoot)
	for budgetName, contract: any in pairs(BUDGET_COSTS) do
		local budget = compiledRoot.budgets[budgetName]
		if costs[contract.field] > budget then
			addGraphDiagnostic(diagnostics, contract.code, "$.root.budgets." .. tostring(budgetName), "Aggregate graph " .. contract.field .. " cost " .. tostring(costs[contract.field]) .. " exceeds root budget " .. tostring(budget) .. ".")
		end
	end
	if #diagnostics > 0 then return nil, diagnostics end
	return TableUtil.deepFreeze({
		kind = "CompiledSpellGraph",
		schemaVersion = Types.SCHEMA_VERSION,
		compilerVersion = Types.COMPILER_VERSION,
		rootId = compiledRoot.id,
		root = compiledRoot,
		recipes = compiledRecipes,
		costs = costs,
	}), diagnostics
end

compileDocument = function(document: any, graphDocument: boolean): (any?, { any })
	local valid, diagnostics = Validator.validate(document, if graphDocument then { skipRecursiveProjectileBudget = true } else nil)
	if not valid then
		return nil, diagnostics
	end

	local source = normalizeValue(TableUtil.deepCopy(document))
	local projectile = {
		lifetime = source.projectile.lifetime,
		motion = source.projectile.motion,
		presentation = normalizePresentation(source.projectile.presentation),
	}
	local events = sortEvents(source.events or {})
	local effectTriggers = sortEffectTriggers(source.effectTriggers or {})
	local statusDefinitions = sortStatusDefinitions(source.statusDefinitions or {})
	local timeline = sortTimeline(projectile.presentation.timeline)
	projectile.presentation.timeline = timeline
	local budgets = normalizeBudgets(source.budgets)
	local burst = if source.burst == nil then nil else normalizeValue(source.burst)
	local expandedChildren = expandBurstChildren(burst)
	if burst ~= nil then
		burst.expandedChildren = expandedChildren
	end

	local costs = calculateCosts(projectile, timeline, expandedChildren, effectTriggers, statusDefinitions)
	local effectCues = compilePresentationCues(effectTriggers, statusDefinitions)
	local serverEffectTriggers = compileServerEffectTriggers(effectTriggers)
	local serverProjectile = {
		lifetime = TableUtil.deepCopy(projectile.lifetime),
		motion = TableUtil.deepCopy(projectile.motion),
	}
	local serverProgram = {
		kind = "ProjectileServerProgram",
		projectile = serverProjectile,
		events = TableUtil.deepCopy(events),
		burst = TableUtil.deepCopy(burst),
		effectTriggers = serverEffectTriggers,
		statusDefinitions = TableUtil.deepCopy(statusDefinitions),
		budgets = TableUtil.deepCopy(budgets),
	}
	local clientProgram = {
		kind = "ProjectileClientProgram",
		projectile = TableUtil.deepCopy(projectile),
		timeline = TableUtil.deepCopy(timeline),
		effectCues = effectCues,
	}
	local compiled = {
		schemaVersion = 1,
		compilerVersion = Types.COMPILER_VERSION,
		id = source.id,
		revision = source.revision,
		projectileBlueprint = projectile,
		blueprint = projectile,
		serverProgram = serverProgram,
		clientProgram = clientProgram,
		burst = burst,
		events = events,
		timeline = timeline,
		visualTail = calculateVisualTail(projectile, timeline),
		costs = costs,
		costEstimate = costs,
		budgets = budgets,
	}

	return TableUtil.deepFreeze(compiled), diagnostics
end

function Compiler.compile(document: any): (any?, { any })
	return compileDocument(document, false)
end

function Compiler.compileResult(document: any): { [string]: any }
	local compiled, diagnostics = Compiler.compile(document)
	return {
		ok = compiled ~= nil,
		compiled = compiled,
		diagnostics = diagnostics,
	}
end

Compiler.Compile = Compiler.compile
Compiler.CompileResult = Compiler.compileResult
Compiler.CompileGraph = Compiler.compileGraph

return table.freeze(Compiler)

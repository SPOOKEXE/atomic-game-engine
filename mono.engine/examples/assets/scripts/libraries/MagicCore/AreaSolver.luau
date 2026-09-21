--!strict

local Enums = require(script.Parent.Enums)
local TableUtil = require(script.Parent.TableUtil)
local Vector3 = require(script.Parent.Vector3)

export type AreaConfig = {
	radius: number,
	falloff: "Constant" | "Linear",
	relationToSource: "Any" | "Self" | "Ally" | "Enemy",
	includeAnyTags: { string },
	includeAllTags: { string },
	excludeAnyTags: { string },
	maxTargets: number,
}

export type Candidate = {
	id: string,
	position: { x: number, y: number, z: number },
	tags: { string },
	factionId: string?,
	-- Compatibility-only caller hints are ignored; relation is derived below.
	relationToSource: ("Self" | "Ally" | "Enemy")?,
	[string]: any,
}

export type SelectedTarget = {
	actorId: string,
	distance: number,
	falloff: number,
	relationToSource: "Self" | "Ally" | "Enemy",
}

export type SolveResult = {
	ok: boolean,
	targets: { SelectedTarget },
	error: string?,
	actorId: string?,
}

local AreaSolver = {}

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function tagSet(tags: { string }): { [string]: boolean }
	local result: { [string]: boolean } = {}
	for _, tag in ipairs(tags) do
		result[tag] = true
	end
	return result
end

local function hasAny(tags: { [string]: boolean }, wanted: { string }): boolean
	for _, tag in ipairs(wanted) do
		if tags[tag] then
			return true
		end
	end
	return false
end

local function hasAll(tags: { [string]: boolean }, wanted: { string }): boolean
	for _, tag in ipairs(wanted) do
		if not tags[tag] then
			return false
		end
	end
	return true
end

local function resolveRelation(
	candidate: Candidate,
	sourceActorId: string,
	sourceFactionId: string?
): "Self" | "Ally" | "Enemy"
	if candidate.id == sourceActorId then
		return Enums.Relation.Self
	end
	if sourceFactionId ~= nil and sourceFactionId ~= "" and candidate.factionId == sourceFactionId then
		return Enums.Relation.Ally
	end
	return Enums.Relation.Enemy
end

local function passesFilters(
	config: AreaConfig,
	candidate: Candidate,
	sourceActorId: string,
	sourceFactionId: string?
): boolean
	local relation = resolveRelation(candidate, sourceActorId, sourceFactionId)
	if config.relationToSource ~= Enums.Relation.Any and config.relationToSource ~= relation then
		return false
	end
	local tags = tagSet(candidate.tags or {})
	if #config.includeAnyTags > 0 and not hasAny(tags, config.includeAnyTags) then
		return false
	end
	if not hasAll(tags, config.includeAllTags) then
		return false
	end
	if hasAny(tags, config.excludeAnyTags) then
		return false
	end
	return true
end

function AreaSolver.falloffAtDistance(falloff: "Constant" | "Linear", distance: number, radius: number): number
	assert(isFinite(distance) and distance >= 0, "distance must be finite and non-negative")
	assert(isFinite(radius) and radius > 0, "radius must be finite and positive")
	if falloff == Enums.AreaFalloff.Constant then
		return 1
	end
	assert(falloff == Enums.AreaFalloff.Linear, "unsupported area falloff")
	return math.clamp(1 - distance / radius, 0, 1)
end

function AreaSolver.solve(
	config: AreaConfig,
	center: { x: number, y: number, z: number },
	candidates: { Candidate },
	sourceActorId: string,
	sourceFactionId: string?
): SolveResult
	assert(Vector3.isFinite(center), "area center must be a finite plain vector")
	assert(isFinite(config.radius) and config.radius > 0, "area radius must be finite and positive")
	assert(config.maxTargets > 0 and config.maxTargets % 1 == 0, "maxTargets must be a positive integer")

	local filtered: { Candidate } = {}
	for _, candidate in ipairs(candidates) do
		if type(candidate) ~= "table" or type(candidate.id) ~= "string" or candidate.id == "" then
			return { ok = false, targets = {}, error = "InvalidCandidate" }
		end
		if passesFilters(config, candidate, sourceActorId, sourceFactionId) then
			table.insert(filtered, candidate)
		end
	end

	local seenIds: { [string]: boolean } = {}
	for _, candidate in ipairs(filtered) do
		if seenIds[candidate.id] then
			return { ok = false, targets = {}, error = "DuplicateActorId", actorId = candidate.id }
		end
		seenIds[candidate.id] = true
	end

	local selected: { SelectedTarget } = {}
	for _, candidate in ipairs(filtered) do
		if not Vector3.isFinite(candidate.position) then
			return { ok = false, targets = {}, error = "InvalidCandidatePosition", actorId = candidate.id }
		end
		local distance = Vector3.distance(center, candidate.position)
		if distance <= config.radius then
			local falloff = AreaSolver.falloffAtDistance(config.falloff, distance, config.radius)
			if falloff > 0 then
				table.insert(selected, {
					actorId = candidate.id,
					distance = distance,
					falloff = falloff,
					relationToSource = resolveRelation(candidate, sourceActorId, sourceFactionId),
				})
			end
		end
	end

	table.sort(selected, function(a: SelectedTarget, b: SelectedTarget): boolean
		if a.distance == b.distance then
			return a.actorId < b.actorId
		end
		return a.distance < b.distance
	end)
	while #selected > config.maxTargets do
		table.remove(selected)
	end

	return { ok = true, targets = TableUtil.deepCopy(selected) }
end

AreaSolver.Solve = AreaSolver.solve
AreaSolver.FalloffAtDistance = AreaSolver.falloffAtDistance

return table.freeze(AreaSolver)

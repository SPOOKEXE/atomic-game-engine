--!strict

local Fixtures = {}

function Fixtures.vector(x: number, y: number, z: number): { x: number, y: number, z: number }
	return { x = x, y = y, z = z }
end

function Fixtures.actor(id: string, x: number, health: number?, tags: { string }?, factionId: string?): any
	return {
		id = id,
		factionId = factionId,
		position = Fixtures.vector(x, 0, 0),
		health = health or 100,
		maxHealth = 100,
		tags = tags or {},
		velocity = Fixtures.vector(0, 0, 0),
		statuses = {},
	}
end

function Fixtures.context(operationId: string, targetActorId: string?, scheduledTime: number?): any
	local time = scheduledTime or 0
	return {
		operationId = operationId,
		castId = "cast.test",
		sourceActorId = "source",
		targetActorId = targetActorId,
		scheduledTime = time,
		processedAt = time,
		impactPosition = Fixtures.vector(0, 0, 0),
		projectilePosition = Fixtures.vector(0, 0, 0),
		sourcePosition = Fixtures.vector(0, 0, 0),
	}
end

function Fixtures.findEvent(events: { any }, kind: string): any?
	for _, event in ipairs(events) do
		if event.kind == kind then
			return event
		end
	end
	return nil
end

function Fixtures.eventsOfKind(events: { any }, kind: string): { any }
	local result = {}
	for _, event in ipairs(events) do
		if event.kind == kind then
			table.insert(result, event)
		end
	end
	return result
end

function Fixtures.statusDefinition(components: any, id: string, policy: string, config: any?): any
	local supplied = config or {}
	return components.Status.Definition(id, 1, {
		tags = supplied.tags or { id },
		duration = supplied.duration or 4,
		tickInterval = supplied.tickInterval,
		tickOnApply = supplied.tickOnApply or false,
		tickAtExpiry = supplied.tickAtExpiry or false,
		stackPolicy = policy,
		maxStacks = supplied.maxStacks or 5,
		maxIndependentInstances = supplied.maxIndependentInstances or (if policy == "Independent" then 2 else 1),
		periodicEffects = supplied.periodicEffects or {},
		modifiers = supplied.modifiers or {},
		cueId = supplied.cueId,
	})
end

return table.freeze(Fixtures)

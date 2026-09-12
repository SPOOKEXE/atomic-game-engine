--!strict

local TableUtil = require(script.Parent.TableUtil)
local Vector3 = require(script.Parent.Vector3)

export type Actor = {
	id: string,
	factionId: string?,
	position: { x: number, y: number, z: number },
	health: number,
	maxHealth: number,
	tags: { string },
	velocity: { x: number, y: number, z: number },
	statuses: { any },
	[string]: any,
}

export type HealthDelta = {
	kind: "Damage" | "Heal",
	requestedAmount: number,
	appliedAmount: number,
	beforeHealth: number,
	afterHealth: number,
	maxHealth: number,
}

local ActorState = {}

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function assertActor(actor: any)
	assert(type(actor) == "table", "actor must be a table")
	assert(type(actor.id) == "string" and actor.id ~= "", "actor id must be a non-empty string")
	assert(actor.factionId == nil or type(actor.factionId) == "string", "actor factionId must be a string when supplied")
	assert(Vector3.isFinite(actor.position), "actor position must be a finite plain vector")
	assert(Vector3.isFinite(actor.velocity), "actor velocity must be a finite plain vector")
	assert(isFinite(actor.maxHealth) and actor.maxHealth >= 0, "actor maxHealth must be finite and non-negative")
	assert(
		isFinite(actor.health) and actor.health >= 0 and actor.health <= actor.maxHealth,
		"actor health must be finite and between zero and maxHealth"
	)
	assert(type(actor.tags) == "table", "actor tags must be an array")
	assert(type(actor.statuses) == "table", "actor statuses must be an array")
end

function ActorState.new(config: any): Actor
	local actor = TableUtil.deepCopy(config)
	actor.tags = actor.tags or {}
	actor.velocity = actor.velocity or { x = 0, y = 0, z = 0 }
	actor.statuses = actor.statuses or {}
	assertActor(actor)
	return actor :: Actor
end

function ActorState.clone(actor: Actor): Actor
	assertActor(actor)
	return TableUtil.deepCopy(actor)
end

function ActorState.damage(actor: Actor, amount: number): (Actor, HealthDelta)
	assertActor(actor)
	assert(isFinite(amount) and amount >= 0, "damage amount must be finite and non-negative")
	local nextActor = TableUtil.deepCopy(actor)
	local beforeHealth = actor.health
	local appliedAmount = math.min(amount, beforeHealth)
	nextActor.health = beforeHealth - appliedAmount
	return nextActor, {
		kind = "Damage",
		requestedAmount = amount,
		appliedAmount = appliedAmount,
		beforeHealth = beforeHealth,
		afterHealth = nextActor.health,
		maxHealth = actor.maxHealth,
	}
end

function ActorState.heal(actor: Actor, amount: number): (Actor, HealthDelta)
	assertActor(actor)
	assert(isFinite(amount) and amount >= 0, "heal amount must be finite and non-negative")
	local nextActor = TableUtil.deepCopy(actor)
	local beforeHealth = actor.health
	local appliedAmount = math.min(amount, actor.maxHealth - beforeHealth)
	nextActor.health = beforeHealth + appliedAmount
	return nextActor, {
		kind = "Heal",
		requestedAmount = amount,
		appliedAmount = appliedAmount,
		beforeHealth = beforeHealth,
		afterHealth = nextActor.health,
		maxHealth = actor.maxHealth,
	}
end

function ActorState.isValid(actor: any): boolean
	local ok = pcall(assertActor, actor)
	return ok
end

ActorState.New = ActorState.new
ActorState.Clone = ActorState.clone
ActorState.Damage = ActorState.damage
ActorState.Heal = ActorState.heal
ActorState.IsValid = ActorState.isValid

return table.freeze(ActorState)

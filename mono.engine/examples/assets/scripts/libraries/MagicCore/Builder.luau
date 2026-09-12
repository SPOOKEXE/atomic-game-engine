--!strict

local Components = require(script.Parent.Components)
local TableUtil = require(script.Parent.TableUtil)

local Builder = {}

function Builder.projectile(lifetime: any, motion: any, presentation: any): { [string]: any }
	return {
		lifetime = TableUtil.deepCopy(lifetime),
		motion = TableUtil.deepCopy(motion),
		presentation = TableUtil.deepCopy(presentation),
	}
end

function Builder.new(id: string, revision: number?): { [string]: any }
	return {
		schemaVersion = 1,
		id = id,
		revision = revision or 1,
		effectTriggers = {},
		statusDefinitions = {},
		projectile = Builder.projectile(
			Components.Lifetime.Time(1),
			Components.Motion.Linear({ x = 0, y = 0, z = -100 }),
			Components.Presentation.Part()
		),
	}
end

-- Builds from a partial document while preserving unknown fields for validation.
function Builder.from(config: { [any]: any }): { [any]: any }
	local result = TableUtil.deepCopy(config)
	if result.schemaVersion == nil then
		result.schemaVersion = 1
	end
	if result.revision == nil then
		result.revision = 1
	end
	if result.projectile == nil then
		local fallback = Builder.new(result.id or "spell", result.revision)
		result.projectile = fallback.projectile
	end
	if result.effectTriggers == nil then
		result.effectTriggers = {}
	end
	if result.statusDefinitions == nil then
		result.statusDefinitions = {}
	end
	return result
end

function Builder.clone(document: any): any
	return TableUtil.deepCopy(document)
end

function Builder.withProjectile(document: any, projectile: any): any
	local result = TableUtil.deepCopy(document)
	result.projectile = TableUtil.deepCopy(projectile)
	return result
end

function Builder.withBurst(document: any, burst: any?): any
	local result = TableUtil.deepCopy(document)
	result.burst = TableUtil.deepCopy(burst)
	return result
end

function Builder.withEvents(document: any, events: { any }): any
	local result = TableUtil.deepCopy(document)
	result.events = TableUtil.deepCopy(events)
	return result
end

function Builder.withBudgets(document: any, budgets: any): any
	local result = TableUtil.deepCopy(document)
	result.budgets = TableUtil.deepCopy(budgets)
	return result
end

function Builder.withEffectTriggers(document: any, effectTriggers: { any }): any
	local result = TableUtil.deepCopy(document)
	result.effectTriggers = TableUtil.deepCopy(effectTriggers)
	return result
end

function Builder.withStatusDefinitions(document: any, statusDefinitions: { any }): any
	local result = TableUtil.deepCopy(document)
	result.statusDefinitions = TableUtil.deepCopy(statusDefinitions)
	return result
end

Builder.New = Builder.new
Builder.NewSpell = Builder.new
Builder.Spell = Builder.from
Builder.Projectile = Builder.projectile
Builder.Clone = Builder.clone
Builder.WithProjectile = Builder.withProjectile
Builder.WithBurst = Builder.withBurst
Builder.WithEvents = Builder.withEvents
Builder.WithBudgets = Builder.withBudgets
Builder.WithEffectTriggers = Builder.withEffectTriggers
Builder.WithStatusDefinitions = Builder.withStatusDefinitions

return table.freeze(Builder)

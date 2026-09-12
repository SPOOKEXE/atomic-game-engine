--!strict

local Components = require(script.Parent.Components)
local Enums = require(script.Parent.Enums)

local StatusDefinitions = {}

function StatusDefinitions.burn(): { [any]: any }
	return Components.Status.Definition(Enums.StatusId.Burn, 1, {
		tags = { Enums.EffectTag.Burn, Enums.EffectTag.DamageOverTime, Enums.EffectTag.Fire },
		duration = 6,
		tickInterval = 1,
		tickOnApply = false,
		tickAtExpiry = true,
		stackPolicy = "AddStacks",
		maxStacks = 5,
		maxIndependentInstances = 1,
		periodicEffects = {
			Components.Effect.Damage(4, Enums.DamageType.Fire, { Enums.EffectTag.Burn, Enums.EffectTag.Fire }),
		},
		modifiers = {},
		cueId = "status.burn",
	})
end

function StatusDefinitions.poison(): { [any]: any }
	return Components.Status.Definition(Enums.StatusId.Poison, 1, {
		tags = { Enums.EffectTag.DamageOverTime, Enums.EffectTag.Poison },
		duration = 8,
		tickInterval = 2,
		tickOnApply = true,
		tickAtExpiry = false,
		stackPolicy = "RefreshDuration",
		maxStacks = 1,
		maxIndependentInstances = 1,
		periodicEffects = {
			Components.Effect.Damage(3, Enums.DamageType.Poison, { Enums.EffectTag.Poison }),
		},
		modifiers = {},
		cueId = "status.poison",
	})
end

-- A slow that a stronger application overwrites.
--
-- **The first shipped definition with a non-empty `modifiers` list.** The solver
-- has always copied `definition.modifiers` into the `StatusModifiersChanged`
-- command it emits on apply, refresh and removal, and burn and poison both
-- declare `modifiers = {}` -- so the path existed, compiled, and had never
-- carried a value. A slow is the obvious thing it is for.
--
-- **And the first on `ReplaceIfStronger`.** Burn adds stacks and poison refreshes
-- its duration; neither exercises the compare. It is the right policy here for a
-- reason rather than for coverage: two chills should not stack into a total stop
-- and should not let a weak one refresh a strong one, so the stronger wins and
-- the weaker is discarded.
function StatusDefinitions.chill(): { [any]: any }
	return Components.Status.Definition(Enums.StatusId.Chill, 1, {
		tags = { Enums.EffectTag.Ice, Enums.EffectTag.Slow, Enums.EffectTag.DamageOverTime },
		duration = 6,
		tickInterval = 1.5,
		tickOnApply = false,
		tickAtExpiry = false,
		stackPolicy = "ReplaceIfStronger",
		maxStacks = 1,
		maxIndependentInstances = 1,
		periodicEffects = {
			Components.Effect.Damage(2, Enums.DamageType.Ice, { Enums.EffectTag.Ice }),
		},
		modifiers = {
			-- Multiply rather than Add, so the slow is a fraction of whatever the
			-- actor's speed happens to be. An additive slow that outruns a slow
			-- actor's base speed makes them walk backwards.
			Components.Status.Modifier("chill_slow", Enums.Stat.MoveSpeed, "Multiply", 0.55),
			Components.Status.Modifier("chill_brittle", Enums.Stat.DamageTaken, "Multiply", 1.15),
		},
		cueId = "status.chill",
	})
end

-- A bleed where every shard that lands keeps its own timer.
--
-- **The first shipped definition on `Independent`, and the only policy that is
-- right here.** A storm of shards is many separate wounds: `AddStacks` would
-- make ten shards one wound ten deep that all ends at once, and
-- `RefreshDuration` would make the tenth shard erase the first nine. Independent
-- instances expire when each was actually inflicted, which is also what makes
-- `IndependentCap` reachable from shipped content rather than only from a
-- fixture.
function StatusDefinitions.laceration(): { [any]: any }
	return Components.Status.Definition(Enums.StatusId.Laceration, 1, {
		tags = { Enums.EffectTag.Bleed, Enums.EffectTag.Physical, Enums.EffectTag.DamageOverTime },
		duration = 5,
		tickInterval = 1,
		tickOnApply = true,
		tickAtExpiry = false,
		stackPolicy = "Independent",
		maxStacks = 1,
		maxIndependentInstances = 6,
		periodicEffects = {
			Components.Effect.Damage(3, Enums.DamageType.Physical, { Enums.EffectTag.Bleed }),
		},
		modifiers = {
			Components.Status.Modifier("laceration_slow", Enums.Stat.MoveSpeed, "Multiply", 0.94),
		},
		cueId = "status.laceration",
	})
end

function StatusDefinitions.all(): { { [any]: any } }
	return {
		StatusDefinitions.burn(),
		StatusDefinitions.poison(),
		StatusDefinitions.chill(),
		StatusDefinitions.laceration(),
	}
end

StatusDefinitions.Burn = StatusDefinitions.burn
StatusDefinitions.Poison = StatusDefinitions.poison
StatusDefinitions.Chill = StatusDefinitions.chill
StatusDefinitions.Laceration = StatusDefinitions.laceration
StatusDefinitions.All = StatusDefinitions.all

return table.freeze(StatusDefinitions)

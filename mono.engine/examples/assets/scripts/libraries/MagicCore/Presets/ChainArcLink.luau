--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

-- One link of `ChainLightning`, parameterised.
--
-- **Not a preset.** `Presets.get` never returns this; the three link documents
-- do, and each of them is a file of its own because that is how every other
-- document in this folder is named and found. What they share is here because
-- the alternative is three sixty-line files differing in four numbers, and the
-- fourth time somebody edits two of the three and not the third is the bug.
--
-- `nextRecipeId` being nil is what ends the chain. The last link has no burst at
-- all rather than a burst the depth budget happens to refuse -- a refusal would
-- show up in the solver's records as a `ChildSpawnRejected` on every cast, which
-- is a diagnostic that means "you got the budget wrong" and would be lying.
return function(config: {
	id: string,
	nextRecipeId: string?,
	-- How far this link travels before it gives up and jumps again.
	lifetime: number,
	speed: number,
	damage: number,
	radius: number,
	thickness: number,
}): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(config.lifetime),
		Components.Motion.Linear({ x = 0, y = 0, z = -config.speed }),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = config.thickness, y = config.thickness, z = config.thickness * 8 },
			-- Cooler and dimmer down the chain, so the last hop reads as the
			-- weakest without anybody having to count them.
			color = { r = 0.55, g = 0.78, b = 1 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle(config.id .. "_tail", config.lifetime, 0.05, 0.16, {
					rate = 64,
				}),
				Components.Presentation.Particle(config.id .. "_spark", config.lifetime, 0.08, 0.28, {
					rate = 30,
				}),
				Components.Presentation.Particle(config.id .. "_impact_flash", 0, 0.04, 0.18, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve(config.id .. "_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0 },
					{ time = 0.6, value = 0.3 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {},
		})
	)

	local document = Builder.new(config.id, 1)
	document = Builder.withProjectile(document, projectile)

	-- **Built in ascending `order`, because the validator reads the array and
	-- not the numbers.** `INVALID_EVENT_ORDER` fires when the orders are not
	-- strictly increasing *in list position*, so appending the terminates after
	-- the hops -- which is how you would naturally write a conditional list --
	-- produces 1, 2, 4, 3, 5 and is refused. The hop has to sit next to its own
	-- terminate in both senses.
	local events = {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
	}

	local hops = config.nextRecipeId ~= nil

	if hops then
		document = Builder.withBurst(
			document,
			Components.Burst.Deterministic({
				Components.Burst.Child("chain_hop", config.nextRecipeId :: string, 1),
			}, {
				trigger = Enums.BurstTrigger.ImpactOrExpire,
				distribution = Enums.BurstDistribution.Arc,
				arcMinDegrees = 22,
				arcMaxDegrees = 68,
				maxDepth = 1,
				random = {
					kind = "Random.Derived",
					seedSource = "Server",
					purpose = "chain.hop.direction",
				},
			})
		)
		-- Burst before Terminate on each path, so the next link leaves from the
		-- strike point before this one is torn down.
		table.insert(events, Components.Event.Impact("impact_hop", 2, Enums.EventAction.Burst))
	end

	table.insert(events, Components.Event.Impact("impact_terminate", 3, Enums.EventAction.Terminate))

	if hops then
		table.insert(events, Components.Event.Expire("expire_hop", 4, Enums.EventAction.Burst))
	end

	table.insert(events, Components.Event.Expire("expire_terminate", 5, Enums.EventAction.Terminate))

	document = Builder.withEvents(document, events)
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"strike",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Area(Enums.AreaCenter.Impact, config.radius, {
				Components.Effect.Damage(config.damage, Enums.DamageType.Arcane, {
					Enums.EffectTag.Arcane,
					Enums.EffectTag.Spell,
				}),
			}, {
				falloff = Enums.AreaFalloff.Linear,
				relationToSource = Enums.Relation.Enemy,
				maxTargets = 3,
			}),
			{ "spell." .. config.id .. ".strike" }
		),
	})
	-- A recipe's own budgets are not what bounds the cast: `compileGraph` checks
	-- the aggregate against the *root's*, and the solver reads
	-- `rootState.maxBurstDepth` from the root as well. These are here so the
	-- document validates standalone, which is what the compiler tests do to it.
	document = Builder.withBudgets(document, {
		maxProjectiles = 4,
		maxEvents = 40,
		maxParticles = 256,
		maxBurstDepth = 3,
		maxEffectCommands = 64,
		maxAreaTargets = 12,
		maxStatusesPerActor = 4,
		maxStatusTicksPerAdvance = 16,
	})
	return document
end

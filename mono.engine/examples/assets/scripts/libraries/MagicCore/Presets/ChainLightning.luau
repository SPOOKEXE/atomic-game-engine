--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

-- A bolt that jumps three more times after it lands.
--
-- **This is the first preset built on recursive burst depth**, and that is why it
-- exists as well as what it looks like. Every other preset bursts once: the
-- meteor throws two children, the nova throws ten, and both stop there with
-- `maxDepth = 1`. The solver has always supported a child that bursts again --
-- `localDepth`, `rootState.maxBurstDepth` and `RECIPE_DEPTH_EXCEEDED` are all
-- there for it -- and nothing shipped had ever taken that path.
--
-- The chain is four documents rather than one that refers to itself, because
-- `compileGraph` refuses a cycle: `chain_lightning` bursts `chain_arc_two`,
-- which bursts `chain_arc_three`, which bursts `chain_arc_four`, which stops.
-- Distinct documents are also what lets each hop be weaker and shorter than the
-- one before, which is the behaviour anyone expects from a chain and would be
-- impossible to express with a single self-referencing recipe.
--
-- ## Why `Arc` rather than a narrow `Cone`
--
-- A cone is solid, so most children come out near its axis: a chain built on one
-- is a straight line with a wobble in it. `Arc` is a hollow cone -- children
-- leave between `arcMinDegrees` and `arcMaxDegrees` and never straight on -- so
-- every hop turns. That is the difference between a bolt that jags and a bolt
-- that stutters forwards.
return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(0.42),
		-- Fast and flat. A chain that arcs under gravity reads as a thrown
		-- object; lightning does not fall.
		Components.Motion.Linear({ x = 0, y = 0, z = -220 }),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = 0.35, y = 0.35, z = 3.4 },
			color = { r = 0.72, g = 0.88, b = 1 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("chain_tail", 0.42, 0.06, 0.2, {
					rate = 90,
				}),
				Components.Presentation.Particle("chain_spark", 0.42, 0.1, 0.35, {
					rate = 46,
				}),
				-- Rate zero and no timeline cue: the runtime finds these as
				-- `cueEmitterIds` and fires them wherever the bolt actually
				-- stopped, which for a chain is a different place every hop.
				Components.Presentation.Particle("chain_impact_flash", 0, 0.05, 0.22, {
					rate = 0,
				}),
				Components.Presentation.Particle("chain_impact_ring", 0, 0.18, 0.5, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("chain_flicker", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0 },
					{ time = 0.35, value = 0.35 },
					{ time = 0.7, value = 0 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {},
		})
	)

	-- One child, and the whole chain hangs off it. `ImpactOrExpire` so a bolt
	-- that hits nothing still jumps -- a chain that only continues on a hit
	-- stops dead over open ground, which looks like a bug.
	local hop = Components.Burst.Deterministic({
		Components.Burst.Child("chain_hop", "chain_arc_two", 1),
	}, {
		trigger = Enums.BurstTrigger.ImpactOrExpire,
		distribution = Enums.BurstDistribution.Arc,
		-- Never less than 22 degrees off the last heading, never more than 68.
		-- Below the floor the chain looks straight; above the ceiling it doubles
		-- back through where it came from.
		arcMinDegrees = 22,
		arcMaxDegrees = 68,
		maxDepth = 1,
		random = {
			kind = "Random.Derived",
			seedSource = "Server",
			purpose = "chain.hop.direction",
		},
	})

	local document = Builder.new("chain_lightning", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withBurst(document, hop)
	-- Burst before Terminate on both paths, so the next link leaves from the
	-- strike point before this one is torn down.
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact_hop", 2, Enums.EventAction.Burst),
		Components.Event.Impact("impact_terminate", 3, Enums.EventAction.Terminate),
		Components.Event.Expire("expire_hop", 4, Enums.EventAction.Burst),
		Components.Event.Expire("expire_terminate", 5, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"strike",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Area(Enums.AreaCenter.Impact, 12, {
				Components.Effect.Damage(48, Enums.DamageType.Arcane, {
					Enums.EffectTag.Arcane,
					Enums.EffectTag.Spell,
				}),
			}, {
				falloff = Enums.AreaFalloff.Linear,
				relationToSource = Enums.Relation.Enemy,
				maxTargets = 3,
			}),
			{ "spell.chain_lightning.strike" }
		),
	})
	-- **`maxBurstDepth` is 3 and that is the number that makes the chain a
	-- chain.** `compileGraph` walks the recipe graph and refuses anything deeper
	-- than this, and the solver checks the same budget again at spawn time --
	-- so lowering it to 1 does not produce a shorter chain, it produces a
	-- `RECIPE_DEPTH_EXCEEDED` at compile time. Four is the engine ceiling.
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

--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

-- A stone shell that bursts into eight splinters, each cutting its own wound.
--
-- **Chosen for the path it takes, not for the fantasy.** Reading the solvers
-- against the shipped presets leaves three things the engine supports and no
-- content had ever used: recursive burst depth, status modifiers, and the
-- `Independent` stack policy. `ChainLightning` takes the first and
-- `GlacialLance` the second. This takes the third, and it is also the only one
-- of the three whose gap is reachable in normal play rather than at a boundary.
--
-- ## Why `Independent` is the only correct policy here
--
-- Eight splinters that hit one target are eight separate wounds:
--
--   * `AddStacks` makes them one wound eight deep that all ends at the same
--     instant, so the bleeding stops in a step rather than trailing off.
--   * `RefreshDuration` makes the eighth splinter erase the first seven, so
--     hitting something with the whole storm is worth exactly as much as
--     clipping it with one.
--   * `Independent` gives each its own timer and its own expiry, which is what
--     "eight wounds" means, and makes `laceration`'s `maxIndependentInstances`
--     cap of 6 something a player can actually reach -- so `IndependentCap` is
--     now a rejection shipped content produces rather than one only a fixture
--     has ever seen.
--
-- ## The burst is a Cone, and that is load-bearing
--
-- Splinters spray along the shell's travel. `Sphere` would send half of them
-- backwards past the caster and `Ring` would lay them flat, and both look like
-- the shell exploded rather than broke apart.
return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(1.1),
		Components.Motion.Ballistic(
			{ x = 0, y = 16, z = -74 },
			{ x = 0, y = -34, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 1.6, y = 1.6, z = 1.6 },
			color = { r = 0.46, g = 0.4, b = 0.34 },
			material = "SmoothPlastic",
			particles = {
				Components.Presentation.Particle("splinter_shell_dust", 1.1, 0.3, 0.85, {
					rate = 22,
				}),
				Components.Presentation.Particle("splinter_shell_trail", 1.1, 0.15, 0.5, {
					rate = 30,
				}),
				Components.Presentation.Particle("splinter_impact_burst", 0, 0.25, 0.8, {
					rate = 0,
				}),
				Components.Presentation.Particle("splinter_impact_debris", 0, 0.5, 1.6, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("splinter_swell", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.8 },
					{ time = 0.8, value = 1 },
					{ time = 0.97, value = 1.25 },
					{ time = 1, value = 0.3 },
				}),
			},
			timeline = {
				Components.Timeline.EmitParticles("splinter_crack", 0.75, "splinter_shell_dust", 10),
			},
		})
	)

	local shatter = Components.Burst.Deterministic({
		Components.Burst.Child("splinters", "splinter_shard", 8),
	}, {
		trigger = Enums.BurstTrigger.ImpactOrExpire,
		distribution = Enums.BurstDistribution.Cone,
		coneAngleDegrees = 62,
		maxDepth = 1,
		random = {
			kind = "Random.Derived",
			seedSource = "Server",
			purpose = "splinterstorm.shard.direction",
		},
	})

	local document = Builder.new("splinterstorm", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withBurst(document, shatter)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact_shatter", 2, Enums.EventAction.Burst),
		Components.Event.Impact("impact_terminate", 3, Enums.EventAction.Terminate),
		Components.Event.Expire("expire_shatter", 4, Enums.EventAction.Burst),
		Components.Event.Expire("expire_terminate", 5, Enums.EventAction.Terminate),
	})
	-- Declared on the root as well as on the shard, because `compileGraph`
	-- gathers definitions across the whole graph and refuses two that share an
	-- id and version without being identical. Both come from the one factory, so
	-- they cannot drift into a conflict.
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.laceration(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"shell_break",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Area(Enums.AreaCenter.Impact, 20, {
				Components.Effect.Damage(58, Enums.DamageType.Physical, {
					Enums.EffectTag.Physical,
					Enums.EffectTag.Area,
					Enums.EffectTag.Spell,
				}),
			}, {
				falloff = Enums.AreaFalloff.Linear,
				relationToSource = Enums.Relation.Enemy,
				maxTargets = 8,
			}),
			{ "spell.splinterstorm.shell_break" }
		),
	})
	-- These cover the whole graph: `compileGraph` checks the aggregate of the
	-- shell plus eight shards against the root's budgets, so `maxStatusesPerActor`
	-- is what a single target can be carrying -- six independent lacerations plus
	-- headroom, matching the definition's own cap.
	document = Builder.withBudgets(document, {
		maxProjectiles = 12,
		maxEvents = 96,
		maxParticles = 512,
		maxBurstDepth = 1,
		maxEffectCommands = 256,
		maxAreaTargets = 64,
		maxStatusesPerActor = 8,
		maxStatusTicksPerAdvance = 128,
	})
	return document
end

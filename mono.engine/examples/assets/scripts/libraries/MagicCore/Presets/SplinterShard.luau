--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

-- One splinter thrown by `Splinterstorm`, and the thing that actually bleeds.
--
-- Each shard applies its own `laceration`, and `laceration` is `Independent` --
-- so eight shards on one target are eight instances with eight timers rather
-- than one instance eight stacks deep. The definition's cap of six is what
-- decides how many survive, which makes `IndependentCap` a rejection this
-- content produces in normal play.
return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(0.7),
		Components.Motion.Ballistic(
			{ x = 0, y = 4, z = -52 },
			{ x = 0, y = -40, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = 0.28, y = 0.28, z = 1.1 },
			color = { r = 0.52, g = 0.45, b = 0.38 },
			material = "SmoothPlastic",
			particles = {
				Components.Presentation.Particle("shard_tail", 0.7, 0.12, 0.42, {
					rate = 26,
				}),
				Components.Presentation.Particle("shard_impact_spark", 0, 0.1, 0.4, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("shard_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0 },
					{ time = 0.8, value = 0.2 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {},
		})
	)

	local document = Builder.new("splinter_shard", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.laceration(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"cut",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Damage(11, Enums.DamageType.Physical, {
				Enums.EffectTag.Physical,
				Enums.EffectTag.Projectile,
			}),
			{ "spell.splinter_shard.cut" }
		),
		Components.Effect.Trigger(
			"bleed",
			Enums.Trigger.Impact,
			2,
			Components.Effect.ApplyStatus(Enums.StatusId.Laceration, 1, 1, 1)
		),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 20,
		maxParticles = 96,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 1,
		maxStatusesPerActor = 8,
		maxStatusTicksPerAdvance = 32,
	})
	return document
end

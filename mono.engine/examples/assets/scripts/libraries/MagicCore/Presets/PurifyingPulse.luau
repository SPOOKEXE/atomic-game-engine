--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(0.65),
		Components.Motion.Ballistic(
			{ x = 0, y = 0, z = 0 },
			{ x = 0, y = 0, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 2.5, y = 2.5, z = 2.5 },
			color = { r = 0.72, g = 0.94, b = 1 },
			transparency = 0.4,
			material = "ForceField",
			particles = {
				Components.Presentation.Particle("purify_ring", 0, 0.25, 0.55, {
					rate = 0,
				}),
				Components.Presentation.Particle("purify_sparks", 0, 0.2, 0.45, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("purify_expand", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.2 },
					{ time = 0.65, value = 1.4 },
					{ time = 1, value = 1.8 },
				}),
				Components.Presentation.Curve("purify_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0.4 },
					{ time = 0.7, value = 0.7 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {
				Components.Timeline.EmitParticles("purify_ring_burst", 0.1, "purify_ring", 32),
				Components.Timeline.EmitParticles("purify_spark_burst", 0.1, "purify_sparks", 20),
			},
		})
	)

	local cleanseAndHeal = Components.Effect.Area(Enums.AreaCenter.Source, 18, {
		Components.Effect.Cleanse({
			includeAnyTags = { Enums.EffectTag.Burn, Enums.EffectTag.Poison },
			maxRemovals = 4,
		}),
		Components.Effect.Heal(12),
	}, {
		falloff = Enums.AreaFalloff.Constant,
		relationToSource = Enums.Relation.Ally,
		maxTargets = 12,
	})

	local document = Builder.new("purifying_pulse", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Expire("expire", 2, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("purify_allies", Enums.Trigger.Expire, 1, cleanseAndHeal, { "spell.purifying_pulse.expire" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 12,
		maxParticles = 128,
		maxBurstDepth = 1,
		maxEffectCommands = 32,
		maxAreaTargets = 12,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

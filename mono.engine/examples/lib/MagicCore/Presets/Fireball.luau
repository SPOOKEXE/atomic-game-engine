--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(2.4),
		Components.Motion.Ballistic(
			{ x = 0, y = 55, z = -82 },
			{ x = 0, y = -65, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 1.5, y = 1.5, z = 1.5 },
			color = { r = 1, g = 0.24, b = 0.04 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("fireball_flame", 2.25, 0.15, 0.45, {
					rate = 45,
				}),
				Components.Presentation.Particle("fireball_smoke", 2.35, 0.4, 1, {
					rate = 18,
				}),
				Components.Presentation.Particle("fireball_explosion", 0, 0.2, 0.6, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("fireball_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.45 },
					{ time = 0.18, value = 1 },
					{ time = 1, value = 0.8 },
				}),
			},
			timeline = {
				Components.Timeline.SetEmitterEnabled("fireball_flame_off", 2.25, "fireball_flame", false),
				Components.Timeline.SetEmitterEnabled("fireball_smoke_off", 2.35, "fireball_smoke", false),
			},
		})
	)

	local explosion = Components.Effect.Area(Enums.AreaCenter.Impact, 16, {
		Components.Effect.Damage(65, Enums.DamageType.Fire, { Enums.EffectTag.Area, Enums.EffectTag.Fire }),
		Components.Effect.ApplyStatus(Enums.StatusId.Burn, 1, 1, 1),
	}, {
		falloff = Enums.AreaFalloff.Linear,
		relationToSource = Enums.Relation.Enemy,
		maxTargets = 8,
	})

	local document = Builder.new("fireball", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.burn(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("impact_explosion", Enums.Trigger.Impact, 1, explosion, { "spell.fireball.impact" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 24,
		maxParticles = 512,
		maxBurstDepth = 1,
		maxEffectCommands = 96,
		maxAreaTargets = 8,
		maxStatusesPerActor = 8,
		maxStatusTicksPerAdvance = 64,
	})
	return document
end

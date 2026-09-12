--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(1.25),
		Components.Motion.Linear({ x = 0, y = 0, z = -135 }),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 0.65, y = 0.65, z = 1.35 },
			color = { r = 0.24, g = 0.62, b = 1 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("arcane_tail", 1.05, 0.12, 0.32, {
					rate = 32,
				}),
				Components.Presentation.Particle("arcane_impact_burst", 0, 0.12, 0.35, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("arcane_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.25 },
					{ time = 0.12, value = 1 },
					{ time = 1, value = 0.7 },
				}),
			},
			timeline = {},
		})
	)

	local document = Builder.new("arcane_bolt", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"impact_damage",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Damage(38, Enums.DamageType.Arcane, { Enums.EffectTag.Arcane, Enums.EffectTag.Projectile }),
			{ "spell.arcane_bolt.impact" }
		),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 16,
		maxParticles = 128,
		maxBurstDepth = 1,
		maxEffectCommands = 4,
		maxAreaTargets = 1,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

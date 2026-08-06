--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(1.9),
		Components.Motion.Bezier(1.9, {
			{ x = 0, y = 0, z = 0 },
			{ x = 7, y = 9, z = -18 },
			{ x = -6, y = 5, z = -42 },
			{ x = 0, y = 0, z = -68 },
		}, "LaunchLocal"),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = 0.3, y = 0.3, z = 1.15 },
			color = { r = 0.42, g = 0.95, b = 0.16 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("venom_tail", 1.7, 0.35, 0.9, {
					rate = 26,
				}),
				Components.Presentation.Particle("venom_spores", 0, 0.5, 1.25, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("venom_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0.05 },
					{ time = 0.75, value = 0.15 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {
				Components.Timeline.SetEmitterEnabled("venom_tail_off", 1.7, "venom_tail", false),
			},
		})
	)

	local document = Builder.new("venom_dart", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.poison(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"impact_damage",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Damage(32, Enums.DamageType.Poison, { Enums.EffectTag.Poison, Enums.EffectTag.Projectile }),
			{ "spell.venom_dart.impact" }
		),
		Components.Effect.Trigger(
			"impact_poison",
			Enums.Trigger.Impact,
			2,
			Components.Effect.ApplyStatus(Enums.StatusId.Poison, 1, 1, 1)
		),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 20,
		maxParticles = 192,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 1,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 8,
	})
	return document
end

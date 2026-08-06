--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(2),
		Components.Motion.Ballistic(
			{ x = 0, y = 48, z = -64 },
			{ x = 0, y = -52, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 1.1, y = 1.1, z = 1.1 },
			color = { r = 0.34, g = 1, b = 0.58 },
			transparency = 0.1,
			material = "ForceField",
			particles = {
				Components.Presentation.Particle("healing_petals", 1.85, 0.35, 0.8, {
					rate = 28,
				}),
				Components.Presentation.Particle("healing_bloom", 0, 0.3, 0.75, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("healing_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.35 },
					{ time = 0.2, value = 1 },
					{ time = 1, value = 0.65 },
				}),
			},
			timeline = {
				Components.Timeline.SetEmitterEnabled("healing_petals_off", 1.85, "healing_petals", false),
			},
		})
	)

	local areaHeal = Components.Effect.Area(Enums.AreaCenter.Impact, 14, {
		Components.Effect.Heal(42),
	}, {
		falloff = Enums.AreaFalloff.Linear,
		relationToSource = Enums.Relation.Ally,
		maxTargets = 10,
	})

	local document = Builder.new("healing_bloom", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("impact_heal", Enums.Trigger.Impact, 1, Components.Effect.Heal(18)),
		Components.Effect.Trigger("ally_bloom", Enums.Trigger.Impact, 2, areaHeal, { "spell.healing_bloom.impact" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 20,
		maxParticles = 192,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 10,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

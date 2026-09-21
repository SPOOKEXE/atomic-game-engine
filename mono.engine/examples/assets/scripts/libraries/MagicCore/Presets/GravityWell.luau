--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(2.5),
		Components.Motion.Ballistic(
			{ x = 0, y = 0, z = 0 },
			{ x = 0, y = 0, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 4, y = 4, z = 4 },
			color = { r = 0.36, g = 0.18, b = 0.78 },
			transparency = 0.35,
			material = "ForceField",
			particles = {
				Components.Presentation.Particle("gravity_motes", 2.2, 0.4, 1.1, {
					rate = 22,
				}),
				Components.Presentation.Particle("gravity_ring", 0, 0.4, 0.8, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("gravity_pulse", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.4 },
					{ time = 0.2, value = 1 },
					{ time = 0.75, value = 0.85 },
					{ time = 1, value = 0.25 },
				}),
			},
			timeline = {
				Components.Timeline.EmitParticles("gravity_ring_1", 0, "gravity_ring", 18),
				Components.Timeline.EmitParticles("gravity_ring_2", 0.6, "gravity_ring", 18),
				Components.Timeline.EmitParticles("gravity_ring_3", 1.2, "gravity_ring", 18),
				Components.Timeline.EmitParticles("gravity_ring_4", 1.8, "gravity_ring", 18),
				Components.Timeline.SetEmitterEnabled("gravity_motes_off", 2.2, "gravity_motes", false),
				Components.Timeline.EmitParticles("gravity_ring_5", 2.4, "gravity_ring", 18),
			},
		})
	)

	local pullPulse = Components.Effect.Area(Enums.AreaCenter.Projectile, 20, {
		Components.Effect.Pull(Enums.AreaCenter.Projectile, 42, 42),
	}, {
		falloff = Enums.AreaFalloff.Constant,
		relationToSource = Enums.Relation.Enemy,
		maxTargets = 12,
	})

	local document = Builder.new("gravity_well", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Expire("expire", 2, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("pull_pulse", Enums.Trigger.Expire, 1, pullPulse, { "spell.gravity_well.expire" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 24,
		maxParticles = 256,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 12,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

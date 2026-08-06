--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(3),
		Components.Motion.Linear({ x = 0, y = 0, z = -45 }),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = 0.45, y = 0.45, z = 1.8 },
			color = { r = 0.25, g = 1, b = 0.55 },
			transparency = 0.15,
			material = "Neon",
			particles = {
				Components.Presentation.Particle("core_tail", 2.7, 0.2, 0.5, {
					rate = 36,
				}),
				Components.Presentation.Particle("wide_sparks", 1.4, 0.5, 1.4, {
					rate = 14,
				}),
				Components.Presentation.Particle("slow_mist", 2.1, 0.8, 1.8, {
					rate = 8,
				}),
				Components.Presentation.Particle("pulse", 0, 0.25, 0.7, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("tail_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.5 },
					{ time = 0.2, value = 1.25 },
					{ time = 1, value = 0.2 },
				}),
				Components.Presentation.Curve("tail_fade", "Transparency", "AbsoluteSeconds", {
					{ time = 0, value = 0.15 },
					{ time = 2.5, value = 0.3 },
					{ time = 3, value = 1 },
				}),
			},
			timeline = {
				Components.Timeline.EmitParticles("pulse_start", 0, "pulse", 16),
				Components.Timeline.SetEmitterEnabled("sparks_off", 1.4, "wide_sparks", false),
				Components.Timeline.SetEmitterEnabled("mist_off", 2.1, "slow_mist", false),
				Components.Timeline.SetEmitterEnabled("core_off", 2.7, "core_tail", false),
				Components.Timeline.EmitParticles("pulse_finish", 2.9, "pulse", 24),
			},
		})
	)

	local document = Builder.new("particle_tail_showcase", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Expire("expire", 2, Enums.EventAction.Terminate),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 24,
		maxParticles = 320,
		maxBurstDepth = 1,
		maxEffectCommands = 1,
		maxAreaTargets = 1,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

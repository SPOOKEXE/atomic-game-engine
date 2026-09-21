--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(3),
		Components.Motion.Ballistic(
			{ x = 0, y = 72, z = -70 },
			{ x = 0, y = -70, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 2.2, y = 2.2, z = 2.2 },
			color = { r = 1, g = 0.34, b = 0.06 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("meteor_tail", 2.8, 0.3, 0.85, {
					rate = 34,
				}),
				Components.Presentation.Particle("meteor_parent_impact", 0, 0.2, 0.65, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("meteor_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.55 },
					{ time = 0.15, value = 1 },
					{ time = 1, value = 1.25 },
				}),
			},
			timeline = {
				Components.Timeline.SetEmitterEnabled("meteor_tail_off", 2.8, "meteor_tail", false),
			},
		})
	)

	local burst = Components.Burst.Deterministic({
		Components.Burst.Child("molten_children", "meteor_child_molten", 3),
		Components.Burst.Child("cinder_children", "meteor_child_cinder", 2),
	}, {
		trigger = Enums.BurstTrigger.Impact,
		distribution = Enums.BurstDistribution.Cone,
		coneAngleDegrees = 55,
		maxDepth = 1,
		random = {
			kind = "Random.Derived",
			seedSource = "Server",
			purpose = "meteor.child.direction",
		},
	})

	local parentImpact = Components.Effect.Area(Enums.AreaCenter.Impact, 12, {
		Components.Effect.Damage(85, Enums.DamageType.Fire, {
			Enums.EffectTag.Area,
			Enums.EffectTag.Fire,
			Enums.EffectTag.Meteor,
		}),
	}, {
		falloff = Enums.AreaFalloff.Constant,
		relationToSource = Enums.Relation.Enemy,
		maxTargets = 10,
	})

	local document = Builder.new("meteor_cluster", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withBurst(document, burst)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact_children", 2, Enums.EventAction.Burst),
		Components.Event.Impact("impact_terminate", 3, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 4, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("parent_impact", Enums.Trigger.Impact, 1, parentImpact, { "spell.meteor_cluster.impact" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 6,
		maxEvents = 32,
		maxParticles = 256,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 10,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

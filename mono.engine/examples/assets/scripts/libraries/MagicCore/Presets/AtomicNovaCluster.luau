--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

-- One of the ten submunitions AtomicNova throws out of its blast.
--
-- The parent burst uses a Ring distribution, so every child launches level. That is
-- what lets this recipe control where it lands: with a level launch direction the
-- authored local +Y resolves to world up, so 22 studs/s outward and 18 studs/s up under
-- 50 studs/s^2 of gravity returns to launch height after 2 * 18 / 50 = 0.72s, having
-- travelled 22 * 0.72 = about 15.8 studs. Change the gravity or either speed and the
-- ring radius moves with it.
--
-- The detonation emitters deliberately use the part-scaled roles rather than the
-- absolute-stud detonation roles AtomicNova uses. A submunition pop should be sized to
-- the submunition, not to a warhead.
return function(): { [string]: any }
	local document = Builder.new("atomic_nova_cluster", 1)
	document = Builder.withProjectile(document, Builder.projectile(
		Components.Lifetime.Time(0.9),
		Components.Motion.Ballistic(
			{ x = 0, y = 18, z = -22 },
			{ x = 0, y = -50, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 1.2, y = 1.2, z = 1.8 },
			color = { r = 1, g = 0.5, b = 0.15 },
			material = "Neon",
			particles = {
				Components.Presentation.Particle("cluster_trail", 0.9, 0.1, 0.3, {
					rate = 24,
				}),
				Components.Presentation.Particle("cluster_burst", 0, 0.2, 0.7, {
					rate = 0,
				}),
				Components.Presentation.Particle("cluster_sparks", 0, 0.25, 0.9, {
					rate = 0,
				}),
				Components.Presentation.Particle("cluster_smoke", 0, 0.6, 1.8, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("cluster_arm", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.5 },
					{ time = 0.25, value = 1 },
					{ time = 1, value = 1.2 },
				}),
			},
			timeline = {},
		})
	))
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger(
			"cluster_impact",
			Enums.Trigger.Impact,
			1,
			Components.Effect.Area(Enums.AreaCenter.Impact, 14, {
				Components.Effect.Damage(70, Enums.DamageType.Fire, {
					Enums.EffectTag.Area,
					Enums.EffectTag.Fire,
				}),
			}, {
				falloff = Enums.AreaFalloff.Linear,
				relationToSource = Enums.Relation.Enemy,
				maxTargets = 4,
			}),
			{ "spell.atomic_nova_cluster.impact" }
		),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 16,
		maxParticles = 64,
		maxBurstDepth = 1,
		maxEffectCommands = 16,
		maxAreaTargets = 8,
		maxStatusesPerActor = 1,
		maxStatusTicksPerAdvance = 1,
	})
	return document
end

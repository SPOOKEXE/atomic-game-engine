--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

-- A lobbed warhead that detonates into a mushroom cloud.
--
-- Only the two warhead emitters run on a rate. Every detonation emitter is authored
-- with rate zero and no Timeline.EmitParticles cue, which is this schema's way of
-- saying "an effect cue fires this". The runtime finds them as `cueEmitterIds` and
-- emits them at Impact or Expire, so the detonation lands wherever the warhead
-- actually stops rather than at a time guessed while authoring.
--
-- The emitter ids carry their own render roles: nova_flash, nova_core, nova_shock,
-- nova_spall, nova_debris, nova_dust, nova_stem, nova_cap, and nova_ash each resolve to
-- a distinct look, including how high above the blast they sit. That is what builds a
-- mushroom out of one part with no per-emitter placement in the schema.
return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(2.6),
		Components.Motion.Ballistic(
			{ x = 0, y = 42, z = -58 },
			{ x = 0, y = -46, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Ball",
			size = { x = 1.8, y = 1.8, z = 3.2 },
			color = { r = 1, g = 0.86, b = 0.55 },
			material = "Neon",
			particles = {
				-- Falling warhead.
				Components.Presentation.Particle("nova_warhead_trail", 2.6, 0.12, 0.42, {
					rate = 34,
				}),
				Components.Presentation.Particle("nova_warhead_smoke", 2.6, 0.35, 1.1, {
					rate = 14,
				}),

				-- Detonation, in the order it reads on screen.
				Components.Presentation.Particle("nova_flash", 0, 0.06, 0.3, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_core", 0, 0.35, 1.5, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_shock", 0, 0.3, 1.1, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_debris", 0, 0.7, 2.4, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_spall", 0, 0.5, 1.8, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_dust", 0, 0.9, 2.8, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_stem", 0, 1.2, 3.6, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_cap", 0, 1.6, 4.2, {
					rate = 0,
				}),
				Components.Presentation.Particle("nova_ash", 0, 2.2, 6, {
					rate = 0,
				}),
			},
			curves = {
				-- The warhead swells as it arms, then collapses into the flash.
				Components.Presentation.Curve("nova_arm_scale", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.6 },
					{ time = 0.5, value = 1 },
					{ time = 0.94, value = 1.3 },
					{ time = 1, value = 0.2 },
				}),
				Components.Presentation.Curve("nova_arm_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0.15 },
					{ time = 0.9, value = 0 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {
				Components.Timeline.EmitParticles("nova_arm_pulse_one", 0.6, "nova_warhead_smoke", 12),
				Components.Timeline.EmitParticles("nova_arm_pulse_two", 1.3, "nova_warhead_smoke", 16),
			},
		})
	)

	-- Ten submunitions thrown level out of the blast. Ring keeps every child horizontal
	-- so the cluster recipe's own ballistic arc decides the radius; see AtomicNovaCluster.
	local cluster = Components.Burst.Deterministic({
		Components.Burst.Child("nova_cluster", "atomic_nova_cluster", 10),
	}, {
		trigger = Enums.BurstTrigger.ImpactOrExpire,
		distribution = Enums.BurstDistribution.Ring,
		maxDepth = 1,
		random = {
			kind = "Random.Derived",
			seedSource = "Server",
			purpose = "nova.cluster.direction",
		},
	})

	local detonation = Components.Effect.Area(Enums.AreaCenter.Impact, 90, {
		Components.Effect.Damage(520, Enums.DamageType.Fire, {
			Enums.EffectTag.Area,
			Enums.EffectTag.Fire,
			Enums.EffectTag.Spell,
		}),
		Components.Effect.ApplyStatus(Enums.StatusId.Burn, 1, 5, 2),
	}, {
		falloff = Enums.AreaFalloff.Linear,
		relationToSource = Enums.Relation.Enemy,
		maxTargets = 24,
	})

	local document = Builder.new("atomic_nova", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withBurst(document, cluster)
	-- Burst before Terminate on both paths, so the submunitions are thrown from the
	-- blast point before the warhead entity is torn down.
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact_cluster", 2, Enums.EventAction.Burst),
		Components.Event.Impact("impact_terminate", 3, Enums.EventAction.Terminate),
		Components.Event.Expire("expire_cluster", 4, Enums.EventAction.Burst),
		Components.Event.Expire("expire_terminate", 5, Enums.EventAction.Terminate),
	})
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.burn(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("detonation", Enums.Trigger.Impact, 1, detonation, { "spell.atomic_nova.detonation" }),
		Components.Effect.Trigger("airburst", Enums.Trigger.Expire, 2, detonation, { "spell.atomic_nova.airburst" }),
	})
	-- These cover the whole graph, not just this document: compileGraph checks the
	-- aggregate of the warhead plus ten submunitions against the root's budgets.
	document = Builder.withBudgets(document, {
		maxProjectiles = 16,
		maxEvents = 160,
		maxParticles = 1024,
		maxBurstDepth = 1,
		maxEffectCommands = 768,
		maxAreaTargets = 96,
		maxStatusesPerActor = 64,
		maxStatusTicksPerAdvance = 512,
	})
	return document
end
